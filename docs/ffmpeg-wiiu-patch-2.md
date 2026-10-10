# Building and patching FFmpeg-wiiu for Ufin

Ufin does not decode video itself. It links the static FFmpeg libraries built from
[FFmpeg-wiiu](https://github.com/GaryOderNichts/FFmpeg-wiiu), whose `h264_wiiu` decoder drives
the Wii U's hardware H.264 decoder. So **any change to `h264_wiiu.c` only takes effect after you
rebuild and install FFmpeg and then re-link Ufin.** Editing the FFmpeg source alone changes nothing.

## What the patches are for

| Problem | Symptom | Fixed by |
|---|---|---|
| Framebuffer sized from the raw (possibly cropped) height, not the hardware's 256-pixel pitch / 16-row-aligned height | Heap overflow / crash on non-16:9 video (e.g. 1280x536) | the alignment fix in `h264_wiiu.c` |
| `libavformat/os_support.c` defines its own `inet_aton`, which clashes with wut's | Build error "static declaration of 'inet_aton' follows non-static declaration" with current GCC | `#if !HAVE_INET_ATON && !defined(__WIIU__)` |
| `h264_wiiu_decode_frame()` honoured `H264DECCheckSkipableFrame()` on every packet | **Main/High play at half frame rate**: the hardware reports non-reference pictures (the non-reference B-frames) as skippable, so they were silently dropped (24 fps film came out of the decoder at 12 fps). Baseline has no such pictures, so it was never affected | the check now only runs when `skip_frame >= AVDISCARD_NONREF` (Ufin never sets it), so every picture is decoded and returned |

Ufin's player separately puts B-frame pictures back into display order
(`src/media/frame_reorder.h`), so it needs *both* halves: the patched decoder returning every
picture, and the player re-ordering them.

Without the skip fix, Settings > H.264 profile > Main/High still *works* but plays at half rate.
Baseline is unaffected either way.

## Which patch file to use

All in `patches/`:

- `ffmpeg-wiiu-all-fixes.patch` -- **start here for a fresh clone.** Alignment fix + `inet_aton`
  fix + skip fix in one `git apply`. Checked to apply cleanly to the upstream commit the fork was
  based on; not tested against newer upstream commits.
- `ffmpeg-wiiu-no-nonref-skip.patch` -- only the skip fix, for a checkout that already has the
  alignment and `inet_aton` fixes.
- `ffmpeg-wiiu-fixes.patch` -- the older patch against an earlier upstream. It also covers a
  `DCInvalidateRange` cache fix, reading from `avpkt->data`, allocation checks and
  `--disable-network`. It does **not** apply cleanly to a checkout that already has the alignment
  fix (some hunks are rejected).

## Where things live

| What | Where |
|---|---|
| FFmpeg source checkout | wherever you cloned `FFmpeg-wiiu` (the folder with `configure-wiiu`) |
| Installed FFmpeg libs (what Ufin links) | `$DEVKITPRO/portlibs/ppc/lib/` (usually `/opt/devkitpro/portlibs/ppc/lib/`) |
| Installed FFmpeg headers | `$DEVKITPRO/portlibs/ppc/include/` |
| Ufin build output | `<ufin repo>/build/ufin.wuhb` (and `ufin.rpx`) |
| App on the SD card | `sd:/wiiu/apps/` |
| Config / log on the SD card | `sd:/wiiu/apps/ufin/config.json`, `sd:/wiiu/apps/ufin/ufin_log.txt` |

Can't remember where the checkout is?

```sh
find ~ /mnt -maxdepth 6 -type d -name FFmpeg-wiiu 2>/dev/null
```

## 1. Environment

In a Linux / WSL terminal:

```sh
echo $DEVKITPRO $DEVKITPPC $WUT_ROOT
```

Expected: `/opt/devkitpro /opt/devkitpro/devkitPPC /opt/devkitpro/wut`. If `WUT_ROOT` is empty:
`export WUT_ROOT=$DEVKITPRO/wut`. If `DEVKITPRO` is empty, open a new terminal, or install
devkitPro with `wiiu-dev` (see the README).

## 2. Get the fix into the FFmpeg source

**Fresh clone:**

```sh
git clone https://github.com/GaryOderNichts/FFmpeg-wiiu.git
cd FFmpeg-wiiu
git apply /path/to/ufin/patches/ffmpeg-wiiu-all-fixes.patch
```

**Existing checkout that already has the alignment + `inet_aton` fixes:**

```sh
cd /path/to/FFmpeg-wiiu
patch -p1 < /path/to/ufin/patches/ffmpeg-wiiu-no-nonref-skip.patch
```

**Patch refuses to apply?** Edit `libavcodec/h264_wiiu.c` by hand: in `h264_wiiu_decode_frame()`,
wrap the `H264DECCheckSkipableFrame(...)` call and the `if (skippable) return 0;` that follows
in `if (avctx->skip_frame >= AVDISCARD_NONREF) { ... }`.

**Check it is in before building** (must print one line):

```sh
grep -n AVDISCARD_NONREF libavcodec/h264_wiiu.c
```

## 3. Build and install FFmpeg

Already built here before (a `config.h` exists in the top folder)? No need to reconfigure:

```sh
make -j$(nproc)
make install
```

First build, or a fresh clone:

```sh
chmod +x configure-wiiu
./configure-wiiu
make -j$(nproc)
make install
```

If anything behaves oddly, `make clean` and repeat the configure/make/install steps.

Notes:

- The compiler warnings about mixed declarations and `av_image_copy` in `h264_wiiu.c` are
  pre-existing and harmless. The long `INSTALL doc/examples/...` list is normal.
- `make install` says **permission denied**: an earlier `sudo` install left root-owned files.
  Fix once with `sudo chown -R $(whoami) $DEVKITPRO/portlibs/ppc` and rerun.
- `make` says **"Nothing to be done"**: your edit did not land. Re-run the `grep` above.
- Confirm the library was replaced (the time should be "now"):

```sh
ls -l --time-style=full-iso $DEVKITPRO/portlibs/ppc/lib/libavcodec.a
```

## 4. Rebuild Ufin so it re-links against the new libraries

CMake does not track `libavcodec.a` as a dependency, so a plain `make` can reuse the old binary.
Delete the outputs first:

```sh
cd /path/to/ufin
mkdir -p build && cd build
cmake ..
rm -f ufin.rpx ufin.elf ufin.wuhb
make
cd .. && tools/check_wiiu_build.sh      # expect: OK: no thread-local storage in the build
```

Build needs `tools/glslcompiler.elf` and `icon.png` in the repo (see the README); copy them from
an older checkout if the build complains they are missing.

Copy `build/ufin.wuhb` to the SD card (`sd:/wiiu/apps/`), replacing the old one. From WSL you can
drop it in Windows first, e.g. `cp build/ufin.wuhb /mnt/c/Users/<you>/Downloads/`.

## 5. Check it worked

Delete `sd:/wiiu/apps/ufin/ufin_log.txt` (so you cannot read an old one), set
Settings > H.264 profile > Main and Video quality > 720p, play ~20 seconds, quit, and open the log.

| Log line | Good | Broken (unpatched decoder) |
|---|---|---|
| `Ufin: stream h264 profile=` | `main ... display-order reorder ON` | -- |
| `Ufin: decoder output pts order:` | dense, e.g. `0.041 0.124 0.083 0.208 0.166 ...` (out of order, but no gaps) | every other frame missing, e.g. `0.041 0.208 0.124 0.375 0.291 ...` |
| `vdec N calls` (second line of each stats pair) | about the source frame rate (24/s for a 24 fps film) | about half of it |
| `reorder_late=` | `0` | -- |
| Papaya HUD fps | about the source frame rate | about half of it |

Quick log check from a shell:

```sh
grep -a "stream h264 profile\|pts order\|vdec" ufin_log.txt | head
```

Cemu is not a valid test for this fix: it already played Main fine because it does not drop those
pictures.

## What has and has not been verified

- Verified on a real Wii U (user report, screenshot of the Papaya HUD): Main profile, 720p,
  ~4 Mbps, 23.8 fps for over two minutes, visibly better quality than Baseline.
- Verified at build time only: the patches apply cleanly to the upstream commit the fork was
  based on, and the host tests (`tests/host/run_tests.sh`) pass.
- Not verified: 1080p Main/High, High profile, long playback with seeking, Ethernet vs Wi-Fi
  limits, and other titles/encoders. Baseline is the safe fallback if anything misbehaves.
- The `DCInvalidateRange` cache fix from the older `ffmpeg-wiiu-fixes.patch` is **not** part of
  `ffmpeg-wiiu-all-fixes.patch`. If your build relies on it, check it separately.
