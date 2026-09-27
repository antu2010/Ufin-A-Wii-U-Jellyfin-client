# Compiling FFmpeg-wiiu with the macroblock-alignment patch

This builds a patched `libavcodec` (and the rest of the static FFmpeg libs)
for the Wii U, containing the fix for the `h264_wiiu` decoder's
under-allocated framebuffer (see `h264_wiiu.c` — it now sizes and offsets
the decode buffer using a 16-row-aligned height instead of the raw,
possibly-cropped display height).

You are building a **static library**, not an app. Nothing runs on the Wii U
by itself after this — Ufin links against the result. You'll rebuild Ufin
as the last step.

## 1. Prerequisites

You need a working **devkitPro** environment with the Wii U toolchain and
`wut`, already set up for cross-compiling. If you've built Ufin before on
this machine, you already have this — skip to step 2.

If not, on WSL2 (Ubuntu) or native Linux:

```sh
# devkitPro's pacman wrapper
wget https://apt.devkitpro.org/install-devkitpro-pacman
chmod +x ./install-devkitpro-pacman
sudo ./install-devkitpro-pacman

# Wii U toolchain + wut
sudo dkp-pacman -S wiiu-dev
```

Then make sure these environment variables are set (usually done for you in
`/etc/profile.d/devkit-env.sh` — open a new shell, or `source` it, after
installing):

```sh
echo $DEVKITPRO   # e.g. /opt/devkitpro
echo $DEVKITPPC   # e.g. /opt/devkitpro/devkitPPC
```

`WUT_ROOT` should point at wut's install location — typically
`$DEVKITPRO/wut`. If `echo $WUT_ROOT` is empty, add to your shell profile:

```sh
export WUT_ROOT=$DEVKITPRO/wut
```

> **Known gotcha:** installing devkitPro's pacman package can occasionally
> hit a Cloudflare 403 from behind certain networks/VPNs — retry, or try a
> different network, if the install script fails to download packages.

## 2. Get the source and apply the patch

```sh
git clone https://github.com/GaryOderNichts/FFmpeg-wiiu.git
cd FFmpeg-wiiu
```

Replace two files with the patched versions attached alongside this doc:

- `libavcodec/h264_wiiu.c` — the actual crash fix: round `avctx->width` up
  to a multiple of 256 and `avctx->height` up to a multiple of 16 before
  using them for the framebuffer size and the chroma-plane offset, instead
  of using the raw (possibly non-macroblock-aligned) values directly.
- `libavformat/os_support.c` — a separate, pre-existing build bug against
  modern GCC (see the gotcha below); needed just to get the fork compiling
  at all right now, unrelated to the crash fix itself.

## 3. Configure

From the `FFmpeg-wiiu` source root:

```sh
chmod +x configure-wiiu
./configure-wiiu
```

This is the repo's own pre-set configure script — it wires up the
cross-compiler, wut's include/lib paths, and enables only the
decoders/demuxers/filters Ufin actually needs (`h264_wiiu`, `aac`, `ac3`,
`mp3`, the `mov`/`h264` demuxers, etc). You shouldn't need to pass any flags
yourself. If it fails, it's almost always one of the environment variables
in step 1 not being set in the shell you're running it from.

> **Known gotcha — `inet_aton` redeclaration error:** `wut`'s own
> `arpa/inet.h` already provides a real `inet_aton`, but FFmpeg's configure
> fails to detect that while cross-compiling, so `libavformat/os_support.c`
> also defines its own `static` fallback — which modern GCC treats as a
> hard error (`static declaration of 'inet_aton' follows non-static
> declaration`), not just a warning like older GCC did. The patched
> `os_support.c` attached alongside this doc skips that fallback
> specifically on the Wii U build (`#if !HAVE_INET_ATON && !defined(__WIIU__)`),
> since wut's version is already there. This is a real bug in the fork
> against current GCC, not a network/environment issue — no need to search
> further if you hit it, just use the patched file.

## 4. Build and install

```sh
make -j$(nproc)
make install
```

`make install` copies the static libs into
`$DEVKITPRO/portlibs/ppc/lib/` (per the `--prefix` baked into
`configure-wiiu`) — that's where Ufin's own build picks them up from.

> **Known gotcha — permission denied during `make install`:** if
> `$DEVKITPRO/portlibs/ppc/` was ever populated by a `sudo`-run install
> (yours or a package installed that way), a later non-`sudo` `make install`
> will fail trying to overwrite those root-owned files. Fix it once,
> permanently, rather than prefixing every future install with `sudo`:
> ```sh
> sudo chown -R $(whoami) /opt/devkitpro/portlibs/ppc
> ```
> (adjust the path if your `$DEVKITPRO` differs).

If you're re-building after having built FFmpeg-wiiu before on this
machine, do a clean first so the installed libs actually get replaced
rather than reused:

```sh
make clean
```
then repeat steps 3–4.

## 5. Rebuild Ufin

Nothing in Ufin's own source changes for this fix — it's entirely inside
the FFmpeg library it links against. Just rebuild Ufin the normal way:

```sh
cd /path/to/Ufin
make clean   # make sure it re-links against the freshly-installed libs
make
```

## 6. Test

Test in this order, same as any Ufin change:

1. **Inside Out** (or any known-good 16:9 title) — must still play exactly
   as before. This patch changes nothing for already-macroblock-aligned
   video.
2. **Big Hero 6** / **A Minecraft Movie** (or any non-16:9 title that
   previously hit the "unsafe geometry" error card) — should now decode and
   play correctly instead of being refused.
3. Real Wii U hardware, not just Cemu, before trusting this broadly — Cemu
   doesn't model the hardware decoder's memory layout precisely enough to
   have caught the original bug, so it's not guaranteed to catch a
   remaining problem either.
