#include "model3d.h"

#include "image_cache.h" // ImageCache::decode: JPEG/PNG bytes -> RGBA, shrunk
#include "vendor/cJSON.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace ui {
namespace {

// ---- little-endian reads: the Wii U's CPU is big-endian, glTF is always little-endian ----
uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
float rdF32(const uint8_t* p) {
    const uint32_t u = rd32(p);
    float f;
    std::memcpy(&f, &u, sizeof f);
    return f;
}

// ---- 4x4 matrices, column-major like glTF ----
using Mat = std::array<float, 16>;

Mat identity() {
    Mat m{};
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    return m;
}

Mat mul(const Mat& a, const Mat& b) { // a * b
    Mat r{};
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[(size_t)(k * 4 + row)] * b[(size_t)(c * 4 + k)];
            r[(size_t)(c * 4 + row)] = s;
        }
    return r;
}

Mat fromTRS(const float t[3], const float q[4], const float s[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    Mat m = identity();
    m[0] = (1 - 2 * (yy + zz)) * s[0]; m[1] = 2 * (xy + wz) * s[0];       m[2] = 2 * (xz - wy) * s[0];
    m[4] = 2 * (xy - wz) * s[1];       m[5] = (1 - 2 * (xx + zz)) * s[1]; m[6] = 2 * (yz + wx) * s[1];
    m[8] = 2 * (xz + wy) * s[2];       m[9] = 2 * (yz - wx) * s[2];       m[10] = (1 - 2 * (xx + yy)) * s[2];
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2];
    return m;
}

// ---- cJSON helpers ----
const cJSON* member(const cJSON* o, const char* k) { return o ? cJSON_GetObjectItemCaseSensitive(o, k) : nullptr; }
const cJSON* arrayOf(const cJSON* o, const char* k) {
    const cJSON* a = member(o, k);
    return cJSON_IsArray(a) ? a : nullptr;
}
int sizeOf(const cJSON* a) { return a ? cJSON_GetArraySize(a) : 0; }
const cJSON* item(const cJSON* a, int i) { return a ? cJSON_GetArrayItem(a, i) : nullptr; }
double numOr(const cJSON* o, const char* k, double def) {
    const cJSON* v = member(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}
int intOr(const cJSON* o, const char* k, int def) {
    const cJSON* v = member(o, k);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : def;
}
bool numsOf(const cJSON* a, float* out, int n) {
    if (sizeOf(a) != n) return false;
    for (int i = 0; i < n; i++) {
        const cJSON* v = item(a, i);
        if (!cJSON_IsNumber(v)) return false;
        out[i] = (float)v->valuedouble;
    }
    return true;
}

struct Ctx {
    const cJSON* root = nullptr;
    const uint8_t* bin = nullptr;
    size_t binSize = 0;
    int texSide = 1024;
    std::vector<int> matMap;       // glTF material -> Model3D material (-1 = not made yet)
    std::map<int, int> texMap;     // glTF texture -> Model3D texture (-1 = could not be read)
    int defaultMat = -1;
    std::vector<bool> texNeedsAlpha; // per Model3D texture: some blended material uses it
    Model3D* out = nullptr;
};

// ---- accessors ----
struct Acc {
    const uint8_t* base = nullptr;
    size_t stride = 0, count = 0, csize = 0;
    int comps = 0, ctype = 0;
    bool norm = false;
};

size_t componentSize(int ctype) {
    switch (ctype) {
        case 5120: case 5121: return 1;
        case 5122: case 5123: return 2;
        case 5125: case 5126: return 4;
        default: return 0;
    }
}

bool accessor(const Ctx& c, int index, int wantComps, Acc& a, std::string& err) {
    const cJSON* acc = item(arrayOf(c.root, "accessors"), index);
    if (!acc) { err = "bad accessor index"; return false; }
    if (member(acc, "sparse")) { err = "sparse accessors are not supported"; return false; }
    const cJSON* ty = member(acc, "type");
    int comps = 0;
    if (cJSON_IsString(ty)) {
        if (!std::strcmp(ty->valuestring, "SCALAR")) comps = 1;
        else if (!std::strcmp(ty->valuestring, "VEC2")) comps = 2;
        else if (!std::strcmp(ty->valuestring, "VEC3")) comps = 3;
        else if (!std::strcmp(ty->valuestring, "VEC4")) comps = 4;
    }
    if (comps == 0 || comps != wantComps) { err = "unexpected accessor type"; return false; }
    const int ctype = intOr(acc, "componentType", 0);
    const size_t csize = componentSize(ctype);
    if (!csize) { err = "unsupported component type"; return false; }
    const double count = numOr(acc, "count", -1);
    if (count < 0 || count > 50e6) { err = "bad accessor count"; return false; }
    const cJSON* bv = item(arrayOf(c.root, "bufferViews"), intOr(acc, "bufferView", -1));
    if (!bv) { err = "accessor without bufferView"; return false; }
    if (intOr(bv, "buffer", 0) != 0) { err = "only the .glb's own binary chunk is supported"; return false; }
    const double bvOff = numOr(bv, "byteOffset", 0), accOff = numOr(acc, "byteOffset", 0), bvLen = numOr(bv, "byteLength", 0);
    const double strideJ = numOr(bv, "byteStride", 0);
    const size_t elem = csize * (size_t)comps;
    const size_t stride = strideJ > 0 ? (size_t)strideJ : elem;
    if (bvOff < 0 || accOff < 0 || bvLen < 0 || stride < elem) { err = "bad bufferView"; return false; }
    const double start = bvOff + accOff;
    const double need = count > 0 ? (double)stride * (count - 1) + (double)elem : 0.0;
    if (start + need > (double)c.binSize || accOff + need > bvLen + 0.5) { err = "accessor runs past the end of the data"; return false; }
    a.base = c.bin + (size_t)start;
    a.stride = stride;
    a.count = (size_t)count;
    a.csize = csize;
    a.comps = comps;
    a.ctype = ctype;
    const cJSON* nrm = member(acc, "normalized");
    a.norm = cJSON_IsTrue(nrm);
    return true;
}

float readFloat(const Acc& a, size_t i, int comp) {
    const uint8_t* p = a.base + i * a.stride + (size_t)comp * a.csize;
    switch (a.ctype) {
        case 5126: return rdF32(p);
        case 5120: { const int v = (int8_t)p[0]; return a.norm ? std::max(-1.0f, (float)v / 127.0f) : (float)v; }
        case 5121: return a.norm ? (float)p[0] / 255.0f : (float)p[0];
        case 5122: { const int v = (int16_t)rd16(p); return a.norm ? std::max(-1.0f, (float)v / 32767.0f) : (float)v; }
        case 5123: return a.norm ? (float)rd16(p) / 65535.0f : (float)rd16(p);
        case 5125: return (float)rd32(p);
        default: return 0;
    }
}

uint32_t readIndex(const Acc& a, size_t i) {
    const uint8_t* p = a.base + i * a.stride;
    switch (a.ctype) {
        case 5121: return p[0];
        case 5123: return rd16(p);
        default: return rd32(p);
    }
}

// ---- materials and textures ----
// Model3D texture index for a glTF texture (decoded once), -1 if it cannot be read.
int textureFor(Ctx& c, int gltfTex) {
    auto it = c.texMap.find(gltfTex);
    if (it != c.texMap.end()) return it->second;
    int result = -1;
    const cJSON* t = item(arrayOf(c.root, "textures"), gltfTex);
    const cJSON* img = item(arrayOf(c.root, "images"), intOr(t, "source", -1));
    const cJSON* bv = item(arrayOf(c.root, "bufferViews"), intOr(img, "bufferView", -1));
    if (bv && intOr(bv, "buffer", 0) == 0) {
        const double off = numOr(bv, "byteOffset", 0), len = numOr(bv, "byteLength", 0);
        if (off >= 0 && len > 0 && off + len <= (double)c.binSize) {
            const std::string bytes((const char*)c.bin + (size_t)off, (size_t)len);
            ModelTexture tx;
            if (ImageCache::decode(bytes, c.texSide, c.texSide, tx.rgba, tx.w, tx.h, c.texSide) && tx.w > 0 && tx.h > 0) {
                c.out->textures.push_back(std::move(tx));
                c.texNeedsAlpha.push_back(false);
                result = (int)c.out->textures.size() - 1;
            }
        }
    }
    c.texMap[gltfTex] = result;
    return result;
}

int materialFor(Ctx& c, int gltfMat) {
    int* slot = nullptr;
    if (gltfMat >= 0 && (size_t)gltfMat < c.matMap.size()) slot = &c.matMap[(size_t)gltfMat];
    else slot = &c.defaultMat;
    if (*slot >= 0) return *slot;
    ModelMaterial m;
    const cJSON* mat = item(arrayOf(c.root, "materials"), gltfMat);
    if (mat) {
        const cJSON* pbr = member(mat, "pbrMetallicRoughness");
        numsOf(arrayOf(pbr, "baseColorFactor"), m.color, 4);
        const cJSON* bt = member(pbr, "baseColorTexture");
        if (bt && intOr(bt, "index", -1) >= 0) m.tex = textureFor(c, intOr(bt, "index", -1));
        const cJSON* mode = member(mat, "alphaMode");
        m.blend = cJSON_IsString(mode) && std::strcmp(mode->valuestring, "OPAQUE") != 0;
        m.doubleSided = cJSON_IsTrue(member(mat, "doubleSided"));
    }
    if (m.tex >= 0 && m.blend) c.texNeedsAlpha[(size_t)m.tex] = true;
    c.out->materials.push_back(m);
    *slot = (int)c.out->materials.size() - 1;
    return *slot;
}

// ---- meshes ----
bool addPrimitive(Ctx& c, const cJSON* prim, const Mat& w, std::string& err) {
    if (intOr(prim, "mode", 4) != 4) return true; // lines, points, strips: skipped
    Model3D& o = *c.out;
    const cJSON* attrs = member(prim, "attributes");
    Acc ap;
    if (!accessor(c, intOr(attrs, "POSITION", -1), 3, ap, err)) return false;
    const size_t n = ap.count, base = o.vertexCount();
    if (o.vertexCount() + n > 400000) { err = "too many vertices"; return false; }
    Acc an, au;
    const bool hasN = member(attrs, "NORMAL") && accessor(c, intOr(attrs, "NORMAL", -1), 3, an, err) && an.count >= n;
    const bool hasUv = member(attrs, "TEXCOORD_0") && accessor(c, intOr(attrs, "TEXCOORD_0", -1), 2, au, err) && au.count >= n;
    err.clear();
    for (size_t i = 0; i < n; i++) {
        const float x = readFloat(ap, i, 0), y = readFloat(ap, i, 1), z = readFloat(ap, i, 2);
        o.pos.push_back(w[0] * x + w[4] * y + w[8] * z + w[12]);
        o.pos.push_back(w[1] * x + w[5] * y + w[9] * z + w[13]);
        o.pos.push_back(w[2] * x + w[6] * y + w[10] * z + w[14]);
        float nx = 0, ny = 0, nz = 1;
        if (hasN) {
            const float a = readFloat(an, i, 0), b = readFloat(an, i, 1), d = readFloat(an, i, 2);
            nx = w[0] * a + w[4] * b + w[8] * d;
            ny = w[1] * a + w[5] * b + w[9] * d;
            nz = w[2] * a + w[6] * b + w[10] * d;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-8f) { nx /= len; ny /= len; nz /= len; } else { nx = 0; ny = 0; nz = 1; }
        }
        o.nrm.push_back(nx); o.nrm.push_back(ny); o.nrm.push_back(nz);
        o.uv.push_back(hasUv ? readFloat(au, i, 0) : 0.0f);
        o.uv.push_back(hasUv ? readFloat(au, i, 1) : 0.0f);
    }
    std::vector<uint32_t> ind;
    if (member(prim, "indices")) {
        Acc ai;
        if (!accessor(c, intOr(prim, "indices", -1), 1, ai, err)) return false;
        if (ai.ctype != 5121 && ai.ctype != 5123 && ai.ctype != 5125) { err = "bad index type"; return false; }
        for (size_t i = 0; i < ai.count; i++) ind.push_back(readIndex(ai, i));
    } else {
        for (size_t i = 0; i < n; i++) ind.push_back((uint32_t)i);
    }
    const size_t tris = ind.size() / 3;
    if (o.triangleCount() + tris > kModelMaxTriangles * 20) { err = "far too many triangles"; return false; }
    for (size_t i = 0; i < tris * 3; i++)
        if (ind[i] >= n) { err = "index out of range"; return false; }
    const int mat = materialFor(c, intOr(prim, "material", -1));
    for (size_t t = 0; t < tris; t++) {
        for (int k = 0; k < 3; k++) o.idx.push_back((uint32_t)(base + ind[t * 3 + (size_t)k]));
        o.triMat.push_back((uint16_t)mat);
    }
    if (!hasN) { // smooth normals from the faces
        std::vector<float> acc(n * 3, 0.0f);
        for (size_t t = 0; t < tris; t++) {
            const size_t i0 = ind[t * 3], i1 = ind[t * 3 + 1], i2 = ind[t * 3 + 2];
            const float* p0 = &o.pos[(base + i0) * 3];
            const float* p1 = &o.pos[(base + i1) * 3];
            const float* p2 = &o.pos[(base + i2) * 3];
            const float ux = p1[0] - p0[0], uy = p1[1] - p0[1], uz = p1[2] - p0[2];
            const float vx = p2[0] - p0[0], vy = p2[1] - p0[1], vz = p2[2] - p0[2];
            const float fx = uy * vz - uz * vy, fy = uz * vx - ux * vz, fz = ux * vy - uy * vx;
            for (size_t idx : {i0, i1, i2}) { acc[idx * 3] += fx; acc[idx * 3 + 1] += fy; acc[idx * 3 + 2] += fz; }
        }
        for (size_t i = 0; i < n; i++) {
            float x = acc[i * 3], y = acc[i * 3 + 1], z = acc[i * 3 + 2];
            const float len = std::sqrt(x * x + y * y + z * z);
            if (len > 1e-12f) { x /= len; y /= len; z /= len; } else { x = 0; y = 0; z = 1; }
            o.nrm[(base + i) * 3] = x; o.nrm[(base + i) * 3 + 1] = y; o.nrm[(base + i) * 3 + 2] = z;
        }
    }
    return true;
}

bool addNode(Ctx& c, int nodeIndex, const Mat& parent, int depth, int& visited, std::string& err) {
    if (depth > 64 || ++visited > 100000) { err = "node tree too deep"; return false; }
    const cJSON* node = item(arrayOf(c.root, "nodes"), nodeIndex);
    if (!node) { err = "bad node index"; return false; }
    Mat local = identity();
    float mm[16];
    if (numsOf(arrayOf(node, "matrix"), mm, 16)) {
        for (size_t i = 0; i < 16; i++) local[i] = mm[i];
    } else {
        float t[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
        numsOf(arrayOf(node, "translation"), t, 3);
        numsOf(arrayOf(node, "rotation"), q, 4);
        numsOf(arrayOf(node, "scale"), s, 3);
        local = fromTRS(t, q, s);
    }
    const Mat world = mul(parent, local);
    const cJSON* mesh = item(arrayOf(c.root, "meshes"), intOr(node, "mesh", -1));
    const cJSON* prims = arrayOf(mesh, "primitives");
    for (int i = 0; i < sizeOf(prims); i++)
        if (!addPrimitive(c, item(prims, i), world, err)) return false;
    const cJSON* kids = arrayOf(node, "children");
    for (int i = 0; i < sizeOf(kids); i++) {
        const cJSON* k = item(kids, i);
        if (!cJSON_IsNumber(k)) continue;
        if (!addNode(c, (int)k->valuedouble, world, depth + 1, visited, err)) return false;
    }
    return true;
}

bool extensionOk(const char* name) {
    return !std::strncmp(name, "KHR_materials_", 14) || !std::strcmp(name, "KHR_texture_transform") ||
           !std::strcmp(name, "KHR_mesh_quantization") || !std::strcmp(name, "KHR_lights_punctual");
}

} // namespace

bool parseGlb(const uint8_t* data, size_t size, Model3D& out, std::string& err) {
    out = Model3D();
    if (!data || size < 20 || rd32(data) != 0x46546C67u) { err = "not a .glb file"; return false; }
    if (rd32(data + 4) != 2) { err = "only glTF 2.0 is supported"; return false; }
    const size_t total = rd32(data + 8);
    if (total > size) { err = "file is cut short"; return false; }
    const uint8_t* json = nullptr;
    size_t jsonLen = 0;
    const uint8_t* bin = nullptr;
    size_t binLen = 0;
    size_t off = 12;
    while (off + 8 <= total) {
        const size_t clen = rd32(data + off);
        const uint32_t ctype = rd32(data + off + 4);
        if (clen > total - off - 8) { err = "file is cut short"; return false; }
        if (ctype == 0x4E4F534Au && !json) { json = data + off + 8; jsonLen = clen; }
        else if (ctype == 0x004E4942u && !bin) { bin = data + off + 8; binLen = clen; }
        off += 8 + ((clen + 3) & ~(size_t)3);
    }
    if (!json) { err = "no JSON chunk"; return false; }
    cJSON* root = cJSON_ParseWithLength((const char*)json, jsonLen);
    if (!root) { err = "broken JSON"; return false; }
    struct Guard { cJSON* r; ~Guard() { cJSON_Delete(r); } } guard{root};

    const cJSON* req = arrayOf(root, "extensionsRequired");
    for (int i = 0; i < sizeOf(req); i++) {
        const cJSON* e = item(req, i);
        if (cJSON_IsString(e) && !extensionOk(e->valuestring)) {
            err = std::string("needs glTF extension ") + e->valuestring + " (export a plain .glb, no compression)";
            return false;
        }
    }
    if (!bin) { err = "no binary data in the .glb"; return false; }

    Ctx c;
    c.root = root;
    c.bin = bin;
    c.binSize = binLen;
    c.out = &out;
    c.texSide = sizeOf(arrayOf(root, "images")) > 3 ? 512 : 1024;
    c.matMap.assign((size_t)sizeOf(arrayOf(root, "materials")), -1);

    int visited = 0;
    const Mat id = identity();
    const cJSON* scenes = arrayOf(root, "scenes");
    const cJSON* scene = item(scenes, intOr(root, "scene", 0));
    if (scene && arrayOf(scene, "nodes")) {
        const cJSON* roots = arrayOf(scene, "nodes");
        for (int i = 0; i < sizeOf(roots); i++) {
            const cJSON* r = item(roots, i);
            if (cJSON_IsNumber(r) && !addNode(c, (int)r->valuedouble, id, 0, visited, err)) return false;
        }
    } else { // no scene: every node nobody is a child of
        const int nn = sizeOf(arrayOf(root, "nodes"));
        std::vector<bool> child((size_t)nn, false);
        for (int i = 0; i < nn; i++) {
            const cJSON* kids = arrayOf(item(arrayOf(root, "nodes"), i), "children");
            for (int k = 0; k < sizeOf(kids); k++) {
                const cJSON* v = item(kids, k);
                if (cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble < nn) child[(size_t)v->valuedouble] = true;
            }
        }
        for (int i = 0; i < nn; i++)
            if (!child[(size_t)i] && !addNode(c, i, id, 0, visited, err)) return false;
    }
    if (out.triangleCount() == 0) { err = "no triangles in the model"; return false; }
    if (out.triangleCount() > kModelMaxTriangles) {
        err = std::to_string(out.triangleCount()) + " triangles (limit " + std::to_string(kModelMaxTriangles) + "), simplify the model";
        return false;
    }
    // Textures only blended materials need keep their alpha; the rest are made fully opaque.
    for (size_t t = 0; t < out.textures.size(); t++) {
        if (c.texNeedsAlpha[t]) continue;
        std::vector<uint8_t>& px = out.textures[t].rgba;
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    }
    if (out.materials.empty()) out.materials.push_back(ModelMaterial());
    return true;
}

void orientModel(Model3D& m, const ModelOrient& o) {
    const size_t n = m.vertexCount();
    if (!n) return;
    auto bounds = [&](float mn[3], float mx[3]) {
        for (int k = 0; k < 3; k++) { mn[k] = 1e30f; mx[k] = -1e30f; }
        for (size_t i = 0; i < n; i++)
            for (int k = 0; k < 3; k++) {
                mn[k] = std::min(mn[k], m.pos[i * 3 + (size_t)k]);
                mx[k] = std::max(mx[k], m.pos[i * 3 + (size_t)k]);
            }
    };
    // 3x3 applied to positions and normals: out = R * in
    auto apply = [&](const float R[9]) {
        for (size_t i = 0; i < n; i++)
            for (std::vector<float>* v : {&m.pos, &m.nrm}) {
                float* p = &(*v)[i * 3];
                const float x = p[0], y = p[1], z = p[2];
                p[0] = R[0] * x + R[1] * y + R[2] * z;
                p[1] = R[3] * x + R[4] * y + R[5] * z;
                p[2] = R[6] * x + R[7] * y + R[8] * z;
            }
    };
    float mn[3], mx[3];
    bounds(mn, mx);
    if (o.autoOrient) {
        const float e[3] = {mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]};
        int zAxis = 0;
        for (int k = 1; k < 3; k++) if (e[k] < e[zAxis]) zAxis = k;
        const int a = (zAxis + 1) % 3, b = (zAxis + 2) % 3;
        const int big = e[b] > e[a] ? b : a, small = big == a ? b : a;
        const int xAxis = o.tall ? small : big, yAxis = o.tall ? big : small;
        const int src[3] = {xAxis, yAxis, zAxis};
        float R[9] = {0};
        for (int r = 0; r < 3; r++) R[r * 3 + src[r]] = 1.0f;
        const int parity = (src[0] > src[1]) + (src[0] > src[2]) + (src[1] > src[2]); // inversions
        if (parity & 1) for (int k = 0; k < 3; k++) R[2 * 3 + k] = -R[2 * 3 + k];      // keep it a rotation
        apply(R);
    }
    const float d2r = 0.01745329252f;
    if (o.rotateDeg[0] != 0 || o.rotateDeg[1] != 0 || o.rotateDeg[2] != 0) {
        const float ax = o.rotateDeg[0] * d2r, ay = o.rotateDeg[1] * d2r, az = o.rotateDeg[2] * d2r;
        const float Rx[9] = {1, 0, 0, 0, std::cos(ax), -std::sin(ax), 0, std::sin(ax), std::cos(ax)};
        const float Ry[9] = {std::cos(ay), 0, std::sin(ay), 0, 1, 0, -std::sin(ay), 0, std::cos(ay)};
        const float Rz[9] = {std::cos(az), -std::sin(az), 0, std::sin(az), std::cos(az), 0, 0, 0, 1};
        apply(Rx); apply(Ry); apply(Rz);
    }
    bounds(mn, mx);
    float half = 1e-6f;
    for (int k = 0; k < 3; k++) half = std::max(half, (mx[k] - mn[k]) * 0.5f);
    const float s = kModelSize / half;
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) m.pos[i * 3 + (size_t)k] = (m.pos[i * 3 + (size_t)k] - (mn[k] + mx[k]) * 0.5f) * s;
    for (int k = 0; k < 3; k++) m.ext[k] = std::max(0.05f, (mx[k] - mn[k]) * 0.5f * s);
}

ModelLoad loadModelFile(const std::string& path, ModelOrient orient, Model3D& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "file not found"; return ModelLoad::Missing; }
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0 || (size_t)len > kModelMaxFileBytes) {
        std::fclose(f);
        err = len <= 0 ? "file is empty"
                       : "file is " + std::to_string(len / (1024 * 1024)) + " MB (limit " +
                             std::to_string(kModelMaxFileBytes / (1024 * 1024)) + " MB), use the 1k-texture .glb";
        return ModelLoad::Error;
    }
    std::vector<uint8_t> bytes((size_t)len);
    const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (got != bytes.size()) { err = "read error"; return ModelLoad::Error; }
    if (!parseGlb(bytes.data(), bytes.size(), out, err)) return ModelLoad::Error;

    // Optional sidecar: gamepad.glb -> gamepad.json
    std::string side = path;
    if (side.size() > 4 && side.compare(side.size() - 4, 4, ".glb") == 0) side.replace(side.size() - 4, 4, ".json");
    if (FILE* sf = std::fopen(side.c_str(), "rb")) {
        std::string txt;
        char buf[512];
        size_t r;
        while ((r = std::fread(buf, 1, sizeof buf, sf)) > 0 && txt.size() < 8192) txt.append(buf, r);
        std::fclose(sf);
        if (cJSON* j = cJSON_ParseWithLength(txt.data(), txt.size())) {
            if (cJSON_IsFalse(member(j, "auto_orient"))) orient.autoOrient = false;
            numsOf(arrayOf(j, "rotate"), orient.rotateDeg, 3);
            cJSON_Delete(j);
        }
    }
    orientModel(out, orient);
    return ModelLoad::Ok;
}

} // namespace ui
