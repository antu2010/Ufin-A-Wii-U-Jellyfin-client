// model3d: the .glb loader behind the Help window's controller models -- parsing, orientation,
// refusing what it cannot show, and files on disk (missing, sidecar).
#include "check.h"
#include "glb_builder.h"
#include "ui/model3d.h"

#include <cstdlib>
#include <sys/stat.h>

using namespace ui;

static bool parse(const std::string& glb, Model3D& m, std::string& err) {
    return parseGlb((const uint8_t*)glb.data(), glb.size(), m, err);
}

int main() {
    std::string err;
    {   // A plain box: 12 triangles, 24 vertices, 1 material, no texture.
        glbtest::BoxOpts o; o.dx = 4; o.dy = 2; o.dz = 1; o.color[0] = 0.5f;
        Model3D m;
        CHECK(parse(glbtest::box(o), m, err));
        CHECK_EQ(m.triangleCount(), (size_t)12);
        CHECK_EQ(m.vertexCount(), (size_t)24);
        CHECK_EQ(m.materials.size(), (size_t)1);
        CHECK(m.materials[0].tex == -1);
        CHECK_NEAR(m.materials[0].color[0], 0.5f, 1e-5);
        CHECK(m.textures.empty());
        for (size_t t = 0; t < m.triangleCount(); t++) CHECK(m.triMat[t] == 0);
        // Winding: every triangle's face normal points the same way as its vertex normals (counter-clockwise = front).
        int outward = 0;
        for (size_t t = 0; t < m.triangleCount(); t++) {
            const float* a = &m.pos[m.idx[t * 3] * 3]; const float* b = &m.pos[m.idx[t * 3 + 1] * 3]; const float* c = &m.pos[m.idx[t * 3 + 2] * 3];
            const float* n = &m.nrm[m.idx[t * 3] * 3];
            const float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
            if ((uy * vz - uz * vy) * n[0] + (uz * vx - ux * vz) * n[1] + (ux * vy - uy * vx) * n[2] > 0) outward++;
        }
        CHECK_EQ(outward, 12);
    }
    {   // Embedded PNG texture is decoded; an opaque material forces alpha to 255.
        glbtest::BoxOpts o; o.texture = true; o.texRGB[0] = 10; o.texRGB[1] = 200; o.texRGB[2] = 30;
        Model3D m;
        CHECK(parse(glbtest::box(o), m, err));
        CHECK_EQ(m.textures.size(), (size_t)1);
        CHECK(m.materials[0].tex == 0);
        CHECK_EQ(m.textures[0].w, 64);
        CHECK_EQ(m.textures[0].rgba[0], 10); CHECK_EQ(m.textures[0].rgba[1], 200); CHECK_EQ(m.textures[0].rgba[3], 255);
    }
    {   // Missing normals are made; a node rotation is applied to the vertices.
        glbtest::BoxOpts o; o.dx = 1; o.dy = 4; o.dz = 2; o.noNormals = true; o.wrapInRotatedNode = true;
        Model3D m;
        CHECK(parse(glbtest::box(o), m, err));
        float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
        for (size_t i = 0; i < m.vertexCount(); i++) for (int k = 0; k < 3; k++) { mn[k] = std::min(mn[k], m.pos[i * 3 + k]); mx[k] = std::max(mx[k], m.pos[i * 3 + k]); }
        CHECK_NEAR(mx[0] - mn[0], 1.0, 1e-4);   // x untouched
        CHECK_NEAR(mx[1] - mn[1], 2.0, 1e-4);   // y and z swapped by the 90 degree turn about x
        CHECK_NEAR(mx[2] - mn[2], 4.0, 1e-4);
        for (size_t i = 0; i < m.vertexCount(); i++) CHECK_NEAR(m.nrm[i * 3] * m.nrm[i * 3] + m.nrm[i * 3 + 1] * m.nrm[i * 3 + 1] + m.nrm[i * 3 + 2] * m.nrm[i * 3 + 2], 1.0, 1e-4);
    }
    {   // orientModel: the thinnest axis ends up on Z, the longest on X (or on Y for `tall`); size normalised.
        glbtest::BoxOpts o; o.dx = 1; o.dy = 6; o.dz = 3;      // long along Y, thin along X
        Model3D wide, tall;
        CHECK(parse(glbtest::box(o), wide, err)); CHECK(parse(glbtest::box(o), tall, err));
        ModelOrient w; ModelOrient t; t.tall = true;
        orientModel(wide, w); orientModel(tall, t);
        CHECK_NEAR(wide.ext[0], kModelSize, 1e-4); CHECK_NEAR(wide.ext[1], kModelSize / 2, 1e-4); CHECK_NEAR(wide.ext[2], kModelSize / 6, 1e-4);
        CHECK_NEAR(tall.ext[1], kModelSize, 1e-4); CHECK_NEAR(tall.ext[0], kModelSize / 2, 1e-4); CHECK_NEAR(tall.ext[2], kModelSize / 6, 1e-4);
        // Still a proper rotation (no mirroring): front faces keep counter-clockwise winding vs. their normals.
        for (Model3D* m : {&wide, &tall}) {
            int outward = 0;
            for (size_t i = 0; i < m->triangleCount(); i++) {
                const float* a = &m->pos[m->idx[i * 3] * 3]; const float* b = &m->pos[m->idx[i * 3 + 1] * 3]; const float* c = &m->pos[m->idx[i * 3 + 2] * 3];
                const float* n = &m->nrm[m->idx[i * 3] * 3];
                const float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
                if ((uy * vz - uz * vy) * n[0] + (uz * vx - ux * vz) * n[1] + (ux * vy - uy * vx) * n[2] > 0) outward++;
            }
            CHECK_EQ(outward, 12);
        }
        // Centred on the origin.
        float mn = 1e9f, mx = -1e9f;
        for (size_t i = 0; i < wide.vertexCount(); i++) { mn = std::min(mn, wide.pos[i * 3]); mx = std::max(mx, wide.pos[i * 3]); }
        CHECK_NEAR(mn + mx, 0.0, 1e-4);
        // autoOrient off keeps the file's axes; an extra 90 degree turn about Z swaps x and y extents.
        Model3D raw; CHECK(parse(glbtest::box(o), raw, err));
        ModelOrient keep; keep.autoOrient = false; orientModel(raw, keep);
        CHECK_NEAR(raw.ext[1], kModelSize, 1e-4);
        Model3D spun; CHECK(parse(glbtest::box(o), spun, err));
        ModelOrient spin; spin.autoOrient = false; spin.rotateDeg[2] = 90; orientModel(spun, spin);
        CHECK_NEAR(spun.ext[0], kModelSize, 1e-3);
    }
    {   // Things it must refuse, with a reason.
        Model3D m;
        CHECK(!parse("not a glb at all, just text........", m, err)); CHECK(!err.empty());
        const std::string good = glbtest::box(glbtest::BoxOpts());
        CHECK(!parse(good.substr(0, good.size() / 2), m, err));
        glbtest::BoxOpts o; o.extraRequired = "KHR_draco_mesh_compression";
        CHECK(!parse(glbtest::box(o), m, err));
        CHECK(err.find("KHR_draco_mesh_compression") != std::string::npos);
        o.extraRequired = "KHR_materials_unlit";          // harmless: allowed
        CHECK(parse(glbtest::box(o), m, err));
        std::string bad = good; bad[4] = 1;               // glTF version 1
        CHECK(!parse(bad, m, err));
    }
    {   // Files: missing, too big, loaded + sidecar override.
        const std::string dir = "/tmp/ufin_model3d_test/";
        mkdir(dir.c_str(), 0777);
        remove((dir + "a.json").c_str());
        Model3D m;
        CHECK(loadModelFile(dir + "nope.glb", ModelOrient(), m, err) == ModelLoad::Missing);
        glbtest::BoxOpts o; o.dx = 1; o.dy = 6; o.dz = 3;
        CHECK(glbtest::writeFile(dir + "a.glb", glbtest::box(o)));
        CHECK(loadModelFile(dir + "a.glb", ModelOrient(), m, err) == ModelLoad::Ok);
        CHECK_NEAR(m.ext[0], kModelSize, 1e-4);
        CHECK(glbtest::writeFile(dir + "a.json", "{\"auto_orient\": false}"));
        CHECK(loadModelFile(dir + "a.glb", ModelOrient(), m, err) == ModelLoad::Ok);
        CHECK_NEAR(m.ext[1], kModelSize, 1e-4);           // file axes kept: Y is the long one
        CHECK(glbtest::writeFile(dir + "junk.glb", "xxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
        CHECK(loadModelFile(dir + "junk.glb", ModelOrient(), m, err) == ModelLoad::Error);
        CHECK(glbtest::writeFile(dir + "big.glb", std::string(kModelMaxFileBytes + 1024, 'x')));
        CHECK(loadModelFile(dir + "big.glb", ModelOrient(), m, err) == ModelLoad::Error);
        CHECK(err.find("MB") != std::string::npos);
    }
    return check::finish("test_model3d");
}
