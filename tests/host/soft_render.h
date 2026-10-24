// Tiny software rasterizer for ImGui draw data, so the host tests can
// render Ufin's screens to pixels (and to PNG previews) without a GPU.
// Handles ImGui 1.92's dynamic textures (RGBA32 and Alpha8), clip rects,
// per-vertex colour and alpha blending. Slow and simple -- fine for tests.
#pragma once
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

// A texture handed to ImGui as an ImTextureID by the app (artwork),
// registered here so the rasterizer can sample it.
struct SoftUserTexture {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

struct SoftRender {
    static std::map<ImTextureID, SoftUserTexture>& userTextures() {
        static std::map<ImTextureID, SoftUserTexture> m;
        return m;
    }

    int w, h;
    std::vector<uint32_t> px; // 0xAABBGGRR like ImGui's IM_COL32

    // The frame being rendered right now: draw-list callbacks (below) run in the
    // middle of render() and need to see its pixels.
    static SoftRender*& current() {
        static SoftRender* c = nullptr;
        return c;
    }
    static int& snapshotCalls() {
        static int n = 0;
        return n;
    }

    // Stands in for the GX2 colour-buffer copy: when the UI asks for a snapshot,
    // copy everything drawn so far into the user texture named by UserCallbackData
    // (an ImTextureID, as an integer). Pass this to ui::setGlassSnapshot().
    static void snapshotCallback(const ImDrawList*, const ImDrawCmd* cmd) {
        SoftRender* r = current();
        if (!r) return;
        snapshotCalls()++;
        SoftUserTexture t;
        t.w = r->w;
        t.h = r->h;
        t.rgba.resize((size_t)r->w * r->h * 4);
        for (size_t i = 0; i < r->px.size(); i++) {
            const uint32_t c = r->px[i];
            t.rgba[i * 4 + 0] = (uint8_t)(c & 0xFF);
            t.rgba[i * 4 + 1] = (uint8_t)((c >> 8) & 0xFF);
            t.rgba[i * 4 + 2] = (uint8_t)((c >> 16) & 0xFF);
            t.rgba[i * 4 + 3] = 255;
        }
        // The frost (see ui::setGlassBlurTexture): a blurred copy, under the next texture id, standing in for the GPU's
        // downsample chain (a box blur of radius 4, twice).
        SoftUserTexture b = t;
        for (int pass = 0; pass < 2; pass++) {
            std::vector<uint8_t> tmp = b.rgba;
            for (int y = 0; y < b.h; y++)
                for (int x = 0; x < b.w; x++)
                    for (int c = 0; c < 3; c++) {
                        int sum = 0, n = 0;
                        for (int k = -4; k <= 4; k++) { const int xx = std::min(b.w - 1, std::max(0, x + k)); sum += b.rgba[((size_t)y * b.w + xx) * 4 + c]; n++; }
                        tmp[((size_t)y * b.w + x) * 4 + c] = (uint8_t)(sum / n);
                    }
            std::vector<uint8_t> out = tmp;
            for (int y = 0; y < b.h; y++)
                for (int x = 0; x < b.w; x++)
                    for (int c = 0; c < 3; c++) {
                        int sum = 0, n = 0;
                        for (int k = -4; k <= 4; k++) { const int yy = std::min(b.h - 1, std::max(0, y + k)); sum += tmp[((size_t)yy * b.w + x) * 4 + c]; n++; }
                        out[((size_t)y * b.w + x) * 4 + c] = (uint8_t)(sum / n);
                    }
            b.rgba = out;
        }
        userTextures()[(ImTextureID)(intptr_t)cmd->UserCallbackData + 1] = std::move(b);
        userTextures()[(ImTextureID)(intptr_t)cmd->UserCallbackData] = std::move(t);
    }

    SoftRender(int width, int height) : w(width), h(height), px((size_t)width * height, 0xFF000000u) {}

    // Accept every texture request (the pixel data stays in ImTextureData).
    static void handleTextures() {
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
            if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
                tex->SetTexID((ImTextureID)(intptr_t)tex->UniqueID);
                tex->SetStatus(ImTextureStatus_OK);
            } else if (tex->Status == ImTextureStatus_WantDestroy) {
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }

    static ImTextureData* findTex(ImTextureID id) {
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
            if (tex->TexID == id) return tex;
        }
        return nullptr;
    }

    static void unpack(uint32_t c, float out[4]) {
        out[0] = (c & 0xFF) / 255.0f;
        out[1] = ((c >> 8) & 0xFF) / 255.0f;
        out[2] = ((c >> 16) & 0xFF) / 255.0f;
        out[3] = ((c >> 24) & 0xFF) / 255.0f;
    }

    // One texture to sample: ImGui's own (font atlas) or an app texture.
    struct TexView {
        ImTextureData* imgui = nullptr;
        const SoftUserTexture* user = nullptr;
    };

    static TexView resolve(ImTextureID id) {
        TexView v;
        v.imgui = findTex(id);
        if (!v.imgui) {
            auto it = userTextures().find(id);
            if (it != userTextures().end()) v.user = &it->second;
        }
        return v;
    }

    static void sample(const TexView& t, float u, float v, float out[4]) {
        if (t.user) {
            int x = std::min(t.user->w - 1, std::max(0, (int)(u * t.user->w)));
            int y = std::min(t.user->h - 1, std::max(0, (int)(v * t.user->h)));
            const uint8_t* p = &t.user->rgba[((size_t)y * t.user->w + x) * 4];
            for (int i = 0; i < 4; i++) out[i] = p[i] / 255.0f;
            return;
        }
        ImTextureData* tex = t.imgui;
        if (!tex || !tex->Pixels) { out[0] = out[1] = out[2] = out[3] = 1.0f; return; }
        int x = (int)(u * tex->Width), y = (int)(v * tex->Height);
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= tex->Width) x = tex->Width - 1;
        if (y >= tex->Height) y = tex->Height - 1;
        const unsigned char* p = (const unsigned char*)tex->GetPixelsAt(x, y);
        if (tex->Format == ImTextureFormat_Alpha8) {
            out[0] = out[1] = out[2] = 1.0f;
            out[3] = p[0] / 255.0f;
        } else {
            for (int i = 0; i < 4; i++) out[i] = p[i] / 255.0f;
        }
    }

    void render(ImDrawData* dd) {
        current() = this;
        for (int n = 0; n < dd->CmdListsCount; n++) {
            const ImDrawList* list = dd->CmdLists[n];
            for (const ImDrawCmd& cmd : list->CmdBuffer) {
                if (cmd.UserCallback) {
                    if (cmd.UserCallback != ImDrawCallback_ResetRenderState) cmd.UserCallback(list, &cmd);
                    continue;
                }
                TexView tex = resolve(cmd.GetTexID());
                int cx0 = std::max(0, (int)cmd.ClipRect.x), cy0 = std::max(0, (int)cmd.ClipRect.y);
                int cx1 = std::min(w, (int)std::ceil(cmd.ClipRect.z)), cy1 = std::min(h, (int)std::ceil(cmd.ClipRect.w));
                for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                    const ImDrawVert& a = list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + i]];
                    const ImDrawVert& b = list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + i + 1]];
                    const ImDrawVert& c = list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + i + 2]];
                    tri(a, b, c, tex, cx0, cy0, cx1, cy1);
                }
            }
        }
        current() = nullptr;
    }

    void tri(const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c, const TexView& tex,
             int cx0, int cy0, int cx1, int cy1) {
        float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
        if (std::fabs(area) < 1e-6f) return;
        int x0 = std::max(cx0, (int)std::floor(std::min({a.pos.x, b.pos.x, c.pos.x})));
        int x1 = std::min(cx1, (int)std::ceil(std::max({a.pos.x, b.pos.x, c.pos.x})));
        int y0 = std::max(cy0, (int)std::floor(std::min({a.pos.y, b.pos.y, c.pos.y})));
        int y1 = std::min(cy1, (int)std::ceil(std::max({a.pos.y, b.pos.y, c.pos.y})));
        float ca[4], cb[4], cc[4];
        unpack(a.col, ca); unpack(b.col, cb); unpack(c.col, cc);
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                float px_ = x + 0.5f, py_ = y + 0.5f;
                float w0 = ((b.pos.x - px_) * (c.pos.y - py_) - (b.pos.y - py_) * (c.pos.x - px_)) / area;
                float w1 = ((c.pos.x - px_) * (a.pos.y - py_) - (c.pos.y - py_) * (a.pos.x - px_)) / area;
                float w2 = 1.0f - w0 - w1;
                if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                float u = w0 * a.uv.x + w1 * b.uv.x + w2 * c.uv.x;
                float v = w0 * a.uv.y + w1 * b.uv.y + w2 * c.uv.y;
                float t[4];
                sample(tex, u, v, t);
                float src[4];
                for (int k = 0; k < 4; k++) src[k] = (w0 * ca[k] + w1 * cb[k] + w2 * cc[k]) * t[k];
                uint32_t& d = px[(size_t)y * w + x];
                float dst[4];
                unpack(d, dst);
                float al = src[3];
                uint32_t out = 0xFF000000u;
                for (int k = 0; k < 3; k++) {
                    float v2 = src[k] * al + dst[k] * (1.0f - al);
                    out |= (uint32_t)(std::min(1.0f, std::max(0.0f, v2)) * 255.0f + 0.5f) << (8 * k);
                }
                d = out;
            }
        }
    }

    uint32_t at(int x, int y) const { return px[(size_t)y * w + x] & 0x00FFFFFFu; }

    // Binary PPM; the test script converts previews to PNG.
    bool savePPM(const std::string& path) const {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return false;
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (uint32_t c : px) {
            unsigned char rgb[3] = {(unsigned char)(c & 0xFF), (unsigned char)((c >> 8) & 0xFF),
                                    (unsigned char)((c >> 16) & 0xFF)};
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
        return true;
    }
};
