// Builds small .glb files for the model tests: a box (24 vertices, outward normals, counter-clockwise
// faces) with an optional base-colour factor and an embedded PNG texture.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "vendor/stb_image_write.h"

namespace glbtest {

inline void put32(std::string& s, uint32_t v) { for (int i = 0; i < 4; i++) s.push_back((char)((v >> (8 * i)) & 0xFF)); }
inline void put16(std::string& s, uint16_t v) { s.push_back((char)(v & 0xFF)); s.push_back((char)(v >> 8)); }
inline void putF(std::string& s, float f) { uint32_t u; std::memcpy(&u, &f, 4); put32(s, u); }
inline void pad4(std::string& s, char c) { while (s.size() % 4) s.push_back(c); }

inline std::string png(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) { px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = 255; }
    std::string out;
    stbi_write_png_to_func([](void* c, void* d, int n) { ((std::string*)c)->append((const char*)d, (size_t)n); }, &out, w, h, 4,
                           px.data(), w * 4);
    return out;
}

struct BoxOpts {
    float dx = 1, dy = 1, dz = 1;       // full sizes
    bool texture = false;                // embed a 64x64 PNG and use it as base colour texture
    uint8_t texRGB[3] = {255, 0, 0};
    float color[4] = {1, 1, 1, 1};
    bool noNormals = false;
    std::string extraRequired;           // an entry for extensionsRequired
    bool wrapInRotatedNode = false;      // node with a 90-degree rotation about X
};

inline std::string box(const BoxOpts& o) {
    std::string bin;
    const float hx = o.dx / 2, hy = o.dy / 2, hz = o.dz / 2;
    // face: normal, then 4 corners counter-clockwise seen from outside
    struct Face { float n[3]; float v[4][3]; };
    const Face faces[6] = {
        {{0, 0, 1}, {{-hx, -hy, hz}, {hx, -hy, hz}, {hx, hy, hz}, {-hx, hy, hz}}},
        {{0, 0, -1}, {{hx, -hy, -hz}, {-hx, -hy, -hz}, {-hx, hy, -hz}, {hx, hy, -hz}}},
        {{1, 0, 0}, {{hx, -hy, hz}, {hx, -hy, -hz}, {hx, hy, -hz}, {hx, hy, hz}}},
        {{-1, 0, 0}, {{-hx, -hy, -hz}, {-hx, -hy, hz}, {-hx, hy, hz}, {-hx, hy, -hz}}},
        {{0, 1, 0}, {{-hx, hy, hz}, {hx, hy, hz}, {hx, hy, -hz}, {-hx, hy, -hz}}},
        {{0, -1, 0}, {{-hx, -hy, -hz}, {hx, -hy, -hz}, {hx, -hy, hz}, {-hx, -hy, hz}}},
    };
    for (const Face& f : faces) for (int i = 0; i < 4; i++) for (int k = 0; k < 3; k++) putF(bin, f.v[i][k]);
    const size_t nrmOff = bin.size();
    for (const Face& f : faces) for (int i = 0; i < 4; i++) for (int k = 0; k < 3; k++) putF(bin, f.n[k]);
    const size_t uvOff = bin.size();
    const float quv[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    for (int f = 0; f < 6; f++) for (int i = 0; i < 4; i++) { putF(bin, quv[i][0]); putF(bin, quv[i][1]); }
    const size_t idxOff = bin.size();
    for (int f = 0; f < 6; f++) {
        const uint16_t b = (uint16_t)(f * 4);
        const uint16_t t[6] = {b, (uint16_t)(b + 1), (uint16_t)(b + 2), b, (uint16_t)(b + 2), (uint16_t)(b + 3)};
        for (uint16_t v : t) put16(bin, v);
    }
    pad4(bin, 0);
    const size_t imgOff = bin.size();
    std::string img;
    if (o.texture) { img = png(64, 64, o.texRGB[0], o.texRGB[1], o.texRGB[2]); bin += img; pad4(bin, 0); }

    char buf[4096];
    std::string views = "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" + std::to_string(nrmOff) + "},"
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(nrmOff) + ",\"byteLength\":" + std::to_string(uvOff - nrmOff) + "},"
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(uvOff) + ",\"byteLength\":" + std::to_string(idxOff - uvOff) + "},"
        "{\"buffer\":0,\"byteOffset\":" + std::to_string(idxOff) + ",\"byteLength\":72}";
    if (o.texture) views += ",{\"buffer\":0,\"byteOffset\":" + std::to_string(imgOff) + ",\"byteLength\":" + std::to_string(img.size()) + "}";
    std::string attrs = "\"POSITION\":0,\"TEXCOORD_0\":2";
    if (!o.noNormals) attrs += ",\"NORMAL\":1";
    std::string mat = "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[" + std::to_string(o.color[0]) + "," + std::to_string(o.color[1]) + "," +
        std::to_string(o.color[2]) + "," + std::to_string(o.color[3]) + "]" +
        (o.texture ? ",\"baseColorTexture\":{\"index\":0}" : "") + "}}";
    std::string node = o.wrapInRotatedNode ? "{\"mesh\":0,\"rotation\":[0.7071068,0,0,0.7071068]}" : "{\"mesh\":0}";
    std::snprintf(buf, sizeof buf,
        "{\"asset\":{\"version\":\"2.0\"},%s\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[%s],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{%s},\"indices\":3,\"material\":0}]}],"
        "\"materials\":[%s],%s"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":24,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5123,\"count\":36,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":[%s],\"buffers\":[{\"byteLength\":%zu}]}",
        o.extraRequired.empty() ? "" : ("\"extensionsRequired\":[\"" + o.extraRequired + "\"],").c_str(), node.c_str(), attrs.c_str(),
        mat.c_str(),
        o.texture ? "\"textures\":[{\"source\":0}],\"images\":[{\"bufferView\":4,\"mimeType\":\"image/png\"}]," : "",
        views.c_str(), bin.size());
    std::string json = buf;
    pad4(json, ' ');
    std::string out;
    put32(out, 0x46546C67u); put32(out, 2);
    put32(out, (uint32_t)(12 + 8 + json.size() + 8 + bin.size()));
    put32(out, (uint32_t)json.size()); put32(out, 0x4E4F534Au); out += json;
    put32(out, (uint32_t)bin.size()); put32(out, 0x004E4942u); out += bin;
    return out;
}

inline bool writeFile(const std::string& path, const std::string& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return true;
}

} // namespace glbtest
