# Ufin -- milestone 1: library browser (no video yet)

A from-scratch Wii U homebrew Jellyfin client. This first milestone connects
to your Jellyfin server, logs in, and lets you browse libraries/folders with
the GamePad. No video playback yet -- selecting a playable item just shows
"would play: X" as a placeholder for milestone 2.

**(Claude)Heads up before you start:** I wrote this against my knowledge of the
wut/WHB APIs, but I don't have a devkitPPC toolchain available to actually
compile-test it. Treat this as a strong first draft, not guaranteed-working
code. When you hit build errors, paste them back to me exactly and we'll
fix them together -- that's the normal workflow for this, not a sign
something went wrong on your end.

## 1. Install devkitPro (Windows)

1. Download the devkitPro installer for Windows: search "devkitPro
   Windows installer" or grab it from https://github.com/devkitPro/installer/releases
   (get the latest `devkitProUpdater-*.exe`).
2. Run it. When it asks which platforms to install, make sure **Wii U
   Development** is checked (this pulls in `devkitPPC` + `wut`).
3. Restart your terminal (or reboot) afterward so the `DEVKITPRO` and
   `DEVKITPPC` environment variables actually take effect.
4. Open **MSYS2** (the installer sets this up) and run:
   ```
   pacman -Syu
   pacman -S wiiu-dev
   ```
   `wiiu-dev` is a group package that pulls in `devkitPPC`, `wut`, and the
   related tools (including `wuhbtool`, which we need to package the app).

Verify it worked:
```
echo $DEVKITPRO
echo $DEVKITPPC
```
Both should print real paths, not be empty.

## 2. Build

From inside MSYS2, in this project folder:
```
mkdir build && cd build
cmake -G "Unix Makefiles" ..
make
```

If it complains about a missing toolchain file, double check step 1.4 --
`wiiu-dev` needs to actually be installed, not just devkitPPC on its own.

On success you should get `build/ufin.rpx` and `build/ufin.wuhb`.

## 3. Configure before deploying

Edit `src/config.h` first (example):
```cpp
#define UFIN_SERVER_HOST "192.168.1.50"  //
#define UFIN_SERVER_PORT 8096
#define UFIN_USERNAME "test"
#define UFIN_PASSWORD "changeme"
```
Rebuild (`make`) after editing.

## 4. Deploy to the Wii U (Aroma)

Copy `ufin.wuhb` to:
```
sd:/wiiu/apps/ufin/ufin.wuhb
```
Reinsert the SD card, boot into Aroma, and it should show up as "Ufin" on
the Wii U menu.

## Known limitations / things we'll hit next

- **No TLS.** The HTTP client is raw sockets, plain `http://` only. Fine
  for your local Jellyfin server, would need mbedtls-wup wired in for
  `https://`.
- **No chunked transfer-encoding support** in the HTTP client. Jellyfin's
  JSON endpoints normally send `Content-Length`, so this should be fine for
  browsing, but flag it if a response looks truncated.
- **Credentials are hardcoded in `config.h`.** Fine for a single-user
  homebrew app; if you ever want a login screen, that's a real feature to
  design later, not a quick add.
- **No video/audio playback yet** -- that's milestone 2, and per our
  earlier conversation, the real constraint there is that the Wii U has no
  public hardware video decoder for homebrew, so it'll mean either software
  H.264 decode at modest resolutions, or leaning on Jellyfin's server-side
  transcoding (which you already have NVENC for) to hand the Wii U a
  low-bitrate stream it can actually decode in software.
- **No poster art / graphics** -- this is a plain text menu over WHB's log
  console on purpose, to prove the networking + API layer works before
  adding SDL2 rendering complexity on top.

