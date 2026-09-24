# Ufin - A Wii U Jellyfin client
WIP jellyfin client for the Nintendo Wii U

-------------------------------------------

> ⚠️ Music and video playback are confirmed working on real hardware. Video playback has been tested successfully on real hardware; however, my current test setup uses a heavily congested 2.4 GHz Wi-Fi connection, so stuttering and audio/video desynchronization can occur under poor network conditions. Ethernet or a clean and fast 2.4 GHz connection still needs to be tested. Quitting the app may cause the console to hang.

# Jellyfin Wii U

A native Jellyfin client for the Nintendo Wii U because why not XD.

**Very early development.** Expect bugs, crashes, and probably the occasional Wii U death beep.

## Current status

- ✅ Wii U ↔ Jellyfin connection
- ✅ Authentication
- ✅ Library browsing
- ✅ Playback report to Jellyfin
- ✅ Music playback
- 🧪 Video playback (720p30 H.264 baseline + AAC, transcoded by the server; Video decoding confirmed working on real hardware and Cemu)
- 🧪 Live TV (channel list with what's on now; tuned and transcoded by the server)
- ✅ Jellyfin 10.8 through 12.x (uses the modern `Authorization` header and `ApiKey` query parameter)
- ✅ Basic Player Controls (Play/Pause)
- ❌ Full player Features (shuffle, play queue and working seek are yet to be added)
- ❌ Wii U style UI

## Goals

- In app config
- login screen
- TV interface
- Wii U-style UI
- GamePad media controls
- 480p60 software playback
- 720p30 H.264/AAC software playback
- 1080p60 hardware playback if possible
- Eventually, direct play where possible

## Installation

1. Download the ZIP file from the "Releases" page.

2. Extract the contents of the ZIP to the root of your Wii U's SD card.

3. Open "config.json" and enter your Jellyfin server details and credentials.
   Optional keys: `video_bitrate` (bits/s, default 2500000) and `video_profile`
   (`baseline` is the default and the safe choice for the Wii U's hardware
   decoder; `main`/`high` are there for experiments).

4. Insert the SD card into your Wii U and launch Ufin through your preferred homebrew method.

## Controls

| Button | Action |
|---|---|
| D-pad / left stick up-down | Move (hold to scroll fast) |
| L / R, D-pad left-right | Page up / down |
| A | Open folder / play |
| B | Back; stops playback |
| X | Search (Wii U on-screen keyboard) |
| Y | Refresh the current list |
| ZR | GX2 test picture (diagnostic) |

During playback: **A** pause / resume, **Left** back 10 s, **Right** forward 30 s
(presses add up: Right three times = +90 s), **B** stop. Live TV only has B.
Skipping restarts the server's transcode at the new position, so it takes a
moment, like starting playback.

## SD Card Layout:

```text
SD:/
├── wiiu/
│   └── apps/
│       └── ufin/
│           ├── ufin.wuhb
│           └── config.json
```     

## How video works

The Wii U has no public software H.264 decoder fast enough for 720p, so Ufin
leans on two things:

- **Server side:** Jellyfin is asked to transcode to exactly 1280x720 H.264
  *baseline* + AAC in fragmented MP4. The exact size matters -- the
  `h264_wiiu` decoder in FFmpeg-wiiu sizes its framebuffer as
  width x height x 1.5 while the hardware writes with a 256-pixel pitch and
  16-row height alignment, and 1280x720 is the size where those agree.
  Non-16:9 content therefore arrives anamorphically squeezed and is
  un-squeezed at draw time using the aspect ratio from the item's metadata.
  Baseline profile means no B-frames, so the hardware decoder's
  one-frame-per-call output comes out in display order.
- **Console side:** frames are decoded by the Wii U's hardware decoder
  (`h264.rpl`, through `h264_wiiu`), uploaded as two GX2 textures (Y and
  interleaved UV) and converted to RGB by a small pixel shader
  (`src/media/shaders/nv12_video.frag`). Video is paced against the audio
  clock; late frames are dropped.

Debugging aids: all `OSReport` lines are prefixed `Ufin:` (visible over a
Cemu/serial log), and pressing **ZR** in the menu draws a magenta test picture
through the exact same GX2 path with no decoding involved.

## Development

Built with WUT/devkitPro and tested on real Wii U hardware.

### Building

1. Install devkitPro with `wiiu-dev` and `wiiu-sdl2`. On Debian/Ubuntu/Mint
   the installer must be fetched with `wget -U "dkp-apt" https://apt.devkitpro.org/install-devkitpro-pacman`
   (the site's firewall blocks plain wget).
2. Build [FFmpeg-wiiu](https://github.com/GaryOderNichts/FFmpeg-wiiu) **with
   `patches/ffmpeg-wiiu-fixes.patch` applied** (`git apply` in the FFmpeg-wiiu
   checkout). It fixes a heap overflow in `h264_wiiu` (framebuffer sized as
   width*height*1.5 although the hardware writes a 256-pixel pitch and a
   16-row aligned height), reads the packet from `avpkt->data` instead of
   `avpkt->buf`, checks its allocations, and adds `--disable-network`
   (current wut ships its own `inet_aton`, which clashes with FFmpeg's).
   Then, with `DEVKITPRO`, `DEVKITPPC` and `WUT_ROOT=$DEVKITPRO/wut` set:
   `./configure-wiiu && make -j$(nproc) && sudo -E make install`.
3. Put `glslcompiler.elf` from [CafeGLSL](https://github.com/Exzap/CafeGLSL/releases)
   in `tools/`, then `mkdir build && cd build && cmake .. && make`.

### Testing in Cemu

Cemu maps `sd:/` to its `sdcard` folder (`~/.local/share/Cemu/sdcard` for the
Linux AppImage), so put `config.json` in `sdcard/wiiu/apps/ufin/`. Set up an
emulated **Wii U GamePad** in Options -> Input settings. `127.0.0.1` works as
the host when Jellyfin runs on the same PC (on a real Wii U it must be the
PC's LAN address). The log with all `Ufin:` lines is Cemu's `log.txt`; on
start-up it reports the measured text grid of both screens.

Cemu draws OSScreen text on a different grid than the menus were first laid
out for (16x24 glyphs from the screen edge), which made long lines wrap back
over themselves and the selection band sit a row off. The UI now measures
the grid at start-up instead of assuming it, so both look right.

Everything that doesn't touch the Wii U hardware also has host-side tests
(plain `g++`, no devkitPro needed): the menu screens, the Jellyfin client and
HTTP layer, the config loader, the frame queue, the audio clock, and the real
decoder/HTTP-stream chain pulling a fragmented MP4 over HTTP.

```
tests/host/run_tests.sh                      # UI, Jellyfin client, config
FFMPEG_HOST=/path/to/ffmpeg tests/host/run_tests.sh   # + FFmpeg-based tests
```

See the header of `tests/host/run_tests.sh` for how to build the FFmpeg
prefix it wants.

This project is developed with really heavy AI assistance (Claude, Gemini and ChatGPT Free tiers). Architecture, design, testing and development decisions are human, tho the majority of the code is generated by AI, I am not a programmer yet, and this was my way to make something no one else has yet

Contributions and help are welcome, if any experienced developer wants to add something to this project because they find it interesting I'm open to it!

## Credits

Huge credit to https://github.com/GaryOderNichts/FFmpeg-wiiu for his work on FFmpeg for Wii U. Ufin uses this work for its Wii U FFmpeg backend.

Another huge credit to https://github.com/LandValueGen for writing working video playback and a better UI
