#include "keyboard.h"
#include "../input.h"
#include "../text_utf.h"
#include "../ufin_log.h"

#include <coreinit/debug.h>
#include <coreinit/filesystem.h>
#include <coreinit/memdefaultheap.h>
#include <nn/swkbd.h>
#include <padscore/kpad.h>
#include <vpad/input.h>
#include <whb/gfx.h>
#include <whb/proc.h>

namespace ui {

bool promptKeyboard(const char16_t* hint, std::string& out, std::string& error) {
    return promptKeyboard(hint, "", false, false, out, error);
}

bool promptKeyboard(const char16_t* hint, const std::string& initial, bool password, bool allowEmpty,
                    std::string& out, std::string& error) {
    const std::u16string initial16 = utf8ToUtf16(initial);
    out.clear();
    error.clear();

    // GX2 is already running (ui::Gfx owns it); swkbd draws into its
    // TV and GamePad targets below.
    FSInit(); // safe to call again; swkbd loads its data through an FS client

    FSClient* fsClient = (FSClient*)MEMAllocFromDefaultHeap(sizeof(FSClient));
    void* workMemory = MEMAllocFromDefaultHeap(nn::swkbd::GetWorkMemorySize(0));
    if (!fsClient || !workMemory) {
        if (fsClient) MEMFreeToDefaultHeap(fsClient);
        if (workMemory) MEMFreeToDefaultHeap(workMemory);
        error = "not enough memory for the keyboard";
        return false;
    }
    FSAddClient(fsClient, FS_ERROR_FLAG_NONE);

    nn::swkbd::CreateArg createArg;
    createArg.regionType = nn::swkbd::RegionType::Europe;
    createArg.workMemory = workMemory;
    createArg.fsClient = fsClient;
    bool created = nn::swkbd::Create(createArg);

    bool confirmed = false;
    if (!created) {
        error = "nn::swkbd::Create failed";
    } else {
        // Ufin doesn't initialise AX outside playback; keep swkbd quiet.
        nn::swkbd::MuteAllSound(true);

        nn::swkbd::AppearArg appearArg;
        appearArg.keyboardArg.configArg.languageType = nn::swkbd::LanguageType::English;
        appearArg.inputFormArg.hintText = hint;
        if (!initial16.empty()) appearArg.inputFormArg.initialText = initial16.c_str();
        if (password) appearArg.inputFormArg.passwordMode = nn::swkbd::PasswordMode::Fade;
        // Open where the user is: the GamePad, or the TV for a Wii Remote
        // (pointed at the screen) / Pro Controller.
        int last = input::lastController();
        appearArg.keyboardArg.configArg.controllerType =
            last >= 0 && last < 4 ? (nn::swkbd::ControllerType)last : nn::swkbd::ControllerType::DrcGamepad;
        if (!nn::swkbd::AppearInputForm(appearArg)) {
            error = "nn::swkbd::AppearInputForm failed";
        } else {
            bool done = false;
            while (!done && WHBProcIsRunning()) {
                VPADStatus vpad = {};
                VPADReadError vpadError;
                VPADRead(VPAD_CHAN_0, &vpad, 1, &vpadError);
                VPADGetTPCalibratedPoint(VPAD_CHAN_0, &vpad.tpNormal, &vpad.tpNormal);

                KPADStatus kpads[4];
                nn::swkbd::ControllerInfo controllerInfo;
                controllerInfo.vpad = &vpad;
                for (int c = 0; c < 4; c++) {
                    KPADError kerr = KPAD_ERROR_OK;
                    bool got = KPADReadEx((KPADChan)c, &kpads[c], 1, &kerr) > 0 && kerr == KPAD_ERROR_OK;
                    controllerInfo.kpad[c] = got ? &kpads[c] : nullptr;
                }
                nn::swkbd::Calc(controllerInfo);

                if (nn::swkbd::IsNeedCalcSubThreadFont()) nn::swkbd::CalcSubThreadFont();
                if (nn::swkbd::IsNeedCalcSubThreadPredict()) nn::swkbd::CalcSubThreadPredict();

                if (nn::swkbd::IsDecideOkButton(nullptr)) {
                    out = utf16ToUtf8(nn::swkbd::GetInputFormString());
                    if (!password) out = trimSpaces(out); // spaces can be part of a password
                    confirmed = allowEmpty || !out.empty();
                    nn::swkbd::DisappearInputForm();
                    done = true;
                } else if (nn::swkbd::IsDecideCancelButton(nullptr)) {
                    nn::swkbd::DisappearInputForm();
                    done = true;
                }

                WHBGfxBeginRender();
                WHBGfxBeginRenderTV();
                WHBGfxClearColor(0.06f, 0.07f, 0.09f, 1.0f);
                nn::swkbd::DrawTV();
                WHBGfxFinishRenderTV();
                WHBGfxBeginRenderDRC();
                WHBGfxClearColor(0.06f, 0.07f, 0.09f, 1.0f);
                nn::swkbd::DrawDRC();
                WHBGfxFinishRenderDRC();
                WHBGfxFinishRender();
            }
        }
        nn::swkbd::Destroy();
    }

    FSDelClient(fsClient, FS_ERROR_FLAG_NONE);
    MEMFreeToDefaultHeap(fsClient);
    MEMFreeToDefaultHeap(workMemory);

    OSReport("Ufin: keyboard %s (%u bytes)\n", confirmed ? "confirmed" : "cancelled", (unsigned)out.size());
    return confirmed;
}

} // namespace ui
