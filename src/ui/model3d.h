// model3d.h -- loads a binary glTF (.glb) into plain arrays the Help window can draw.
//
// No ImGui, no GX2: only std + cJSON + stb_image (through ImageCache::decode), so it runs in the
// host tests. What it understands: the default scene's node tree (TRS or matrix), triangle meshes
// with POSITION / NORMAL / TEXCOORD_0, and per material a base colour factor + base colour
// texture (JPEG/PNG embedded in the .glb). Everything else (skins, animation, PBR maps,
// Draco/meshopt/KTX2 compression, external files) is ignored or rejected with a message.
//
// The result is centred on the origin, scaled so its longest side is kModelSize, and rotated so
// that the thinnest axis points at the viewer (+Z) -- see orientModel().
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ui {

constexpr size_t kModelMaxFileBytes = 24u * 1024u * 1024u; // bigger files are refused (use the 1k-texture GLB)
constexpr size_t kModelMaxTriangles = 16000;               // drawn as ImGui triangles every frame
constexpr float kModelSize = 1.85f;                         // half of the longest side, in model units

struct ModelTexture {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

struct ModelMaterial {
    int tex = -1;                         // index into Model3D::textures, -1 = flat colour
    float color[4] = {1, 1, 1, 1};        // base colour factor
    bool doubleSided = false;
    bool blend = false;                   // alphaMode BLEND/MASK: the alpha channel is used
};

struct Model3D {
    std::vector<float> pos;               // x,y,z per vertex
    std::vector<float> nrm;               // x,y,z per vertex (unit length)
    std::vector<float> uv;                // u,v per vertex
    std::vector<uint32_t> idx;            // 3 per triangle, counter-clockwise = front
    std::vector<uint16_t> triMat;         // material index per triangle
    std::vector<ModelMaterial> materials;
    std::vector<ModelTexture> textures;
    float ext[3] = {1, 1, 1};             // half extents of the bounding box after orientModel()

    size_t vertexCount() const { return pos.size() / 3; }
    size_t triangleCount() const { return idx.size() / 3; }
};

// How a model is turned before it is shown. `tall` = the long side goes up (the Wii Remote),
// otherwise it goes left-right. `autoOrient` false keeps the file's own axes (glTF: +Y up, +Z front).
// rotateDeg = extra turn (x, y, z; degrees) applied last, e.g. {0, 180, 0} to show the back instead.
struct ModelOrient {
    bool tall = false;
    bool autoOrient = true;
    float rotateDeg[3] = {0, 0, 0};
};

// Parses .glb bytes. On failure returns false and sets `err` to a short human-readable reason.
bool parseGlb(const uint8_t* data, size_t size, Model3D& out, std::string& err);

// Centres, scales and turns a parsed model (also fills Model3D::ext).
void orientModel(Model3D& m, const ModelOrient& o);

enum class ModelLoad { Ok, Missing, Error };

// Reads `path` (a .glb), parses and orients it. A missing file is Missing, anything else wrong is
// Error with a reason. An optional sidecar next to it (gamepad.glb -> gamepad.json) may hold
// {"auto_orient": false, "rotate": [x, y, z]} and overrides `orient`.
ModelLoad loadModelFile(const std::string& path, ModelOrient orient, Model3D& out, std::string& err);

} // namespace ui
