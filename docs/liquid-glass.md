# Liquid Glass UI

Settings -> **Liquid Glass UI** (default **off**; `"liquid_glass": true` in
config.json). "What if Nintendo had given the Wii U system UI a modern glass
redesign": rounder, translucent, softly lit panels over a soft blurred
backdrop. Purely appearance -- **no playback code is involved** (`src/media/`
is untouched by this feature).

With it off, every screen is drawn exactly as before (the drawing functions
fall straight through to the original calls; checked pixel-for-pixel against
the previous build on every test screen), no glass resources are allocated and
nothing extra runs.

## How it is built
* **One shared blurred backdrop** (`src/ui/glass_core.*`): a small texture
  (~1/8 of the display: 107x60 from the 854x480 GamePad, the smaller of TV and
  GamePad) shared by every panel on both displays. It lives in the UI's virtual
  1280x720 space, so a panel maps to it with the same UVs on the TV and the
  GamePad whatever the real framebuffer is (`uvForRect`, `displayScale`).
* **Dual-Kawase**: synthesize a soft light field (the theme background plus
  drifting accent-coloured glows and, faintly, the selected artwork's colour),
  2 down passes (5 taps), 2 up passes (8 taps), convert to RGBA8. It runs on the
  **CPU on that tiny buffer**, not as GPU render-to-texture passes -- see
  "Design choice" below.
* **Time-sliced**: 16 rows of work per frame, one regeneration every 0.5 s
  (about 13 short frames of work out of every 30), so no frame ever pays for the
  whole blur. Skipped entirely when nothing changed.
* **Two textures, ping-pong**: written through the same CPU-write path artwork
  already uses (`updateTexture` in `gpu_image.cpp` mirrors `uploadTexture`); the
  texture written is never the one last frame sampled. Allocated once when the
  setting is switched on, released when it is switched off. No per-frame
  allocation. Roughly 26 KB of GPU memory + ~250 KB of CPU buffers.
* **Playback wins**: while a video frame is being presented nothing is
  regenerated or written; the video HUD's panels never sample the backdrop (they
  are plain translucent glass over the picture). If the measured cost per frame
  exceeds 3 ms the blur drops to one pass, above 6 ms it is abandoned for "flat
  glass" (tint + gloss, no texture).
* **Glass surface** (`glassSurface` in `app_ui.cpp`): soft shadow, the backdrop
  at the panel's own position, a translucent tint, a diagonal sheen + top edge
  highlight, an edge. Focus adds an accent tint, glow halo, accent border and a
  slow drifting highlight. Pressed is darker with an inner top edge (touch on the
  Now Playing buttons); disabled is dimmed. Corner radii are 1.45x.
* Falls back cleanly: if the textures cannot be created the setting still works
  as flat glass, and Settings says so.

## Design choice: CPU blur, not GX2 render-to-texture
Ufin's renderer has no render-target code today (only CPU-written textures and
ImGui draws). A render-to-texture Dual-Kawase needs new GX2 API use
(color buffers, `GX2SetColorBuffer`, cache invalidation after GPU writes) that
could not be compiled or tested off-console, and a mistake there can hang the
GPU. The backdrop is a synthesized soft field, not a copy of the live screen,
so a tiny CPU blur gives the same frosted result for menus at trivial cost and
only uses calls proven on hardware. The `TextureSink` seam in `glass_core.h`
is where a GPU generator could be swapped in later. Consequence: panels frost
the *backdrop*, not other UI or the video underneath.

## Config
`liquid_glass` (bool, default false), `glass_debug` (bool; logs the blur's
per-frame cost every 5 s). Log lines: `Glass blur initialized: 107x60`,
`Glass blur passes: 2 down + 2 up`, `Glass mode: enabled`,
`Glass blur frame time: ...`, and a fallback message if anything fails.

## Tests
`tests/host/test_glass_core.cpp` (dimensions, UV mapping, display scaling, the
blur, scheduling, resources, fallback) and `testLiquidGlass` in `test_ui.cpp`
(the look, video never sampled, pressed state, artwork tint, clean off switch).
