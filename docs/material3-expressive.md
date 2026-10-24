# Material 3 Expressive UI

Settings -> **UI style** cycles **Plain -> Liquid Glass -> Material 3 Expressive** (M3E). Exactly one style is
active; `config.json` stores it as `liquid_glass` / `m3_expressive` (never both -- if a hand-edited file sets
both, Liquid Glass wins). Default: Plain. The accent colour and the light/dark setting are shared by all three.

M3E is an appearance layer: **no layout, navigation, input or playback code changed** (`src/media/` is untouched).
Every screen keeps its positions; only colours, shapes, type weight, motion and surfaces differ.

## What makes it M3E (and not Liquid Glass)
| | Plain | Liquid Glass | Material 3 Expressive |
|---|---|---|---|
| Surfaces | flat fills | translucent, refractive, lit rims | **opaque tonal containers**, no rims or sheen |
| Focus | accent fill + outline | glow + drifting highlight | **primary-container pill that springs open**, plus a primary ring |
| Shapes | 12-18 px corners | 1.45x corners | 1.4x corners, **pills, cookies (scalloped), squiggle** |
| Colour | fixed neutrals + accent | fixed neutrals + accent | **whole palette derived from the accent** (tonal roles) |
| Blur | none | refraction snapshot + blur | **still soft frost on dialogs only**, no refraction |
| Motion | bounce on profiles | animated light field | **spring morphs**, dialog spring-in, wavy progress, loader |

## Design system (`src/ui/m3e_core.h`, `src/ui/m3e_draw.inc`)
* **Palette** (`m3e::derive`): from the accent's hue/saturation, tones chosen on a CIE L* scale (so tone 80 on
  tone 20 is equally readable for yellow and blue). Roles: primary / onPrimary, primaryContainer,
  secondaryContainer (desaturated), tertiary (+60 degrees), five surface steps, onSurface / variant / outline,
  errorContainer. The old `col::BG/SIDEBAR/PANEL/PANEL_HI/TEXT/MUTED/DIM/TRACK` are reassigned from it, so every
  existing screen is coordinated for free; `accentMain/Deep/Second` map to primary / primaryContainer / tertiary
  and `onMain()/onFocus()` give the matching text colour. Rainbow keeps working (palette refreshed per frame:
  ~12 bisection steps x ~20 tones, no allocation).
* **Shapes**: `cookiePoints` (n-lobed scalloped disc, one triangle fan), `squigglePoints` (travelling sine,
  grows in from flat), pills via `morphRadius`. Used for: the no-cover music tile and Up Next tile (cookie),
  the selected profile picture (springs from circle to 9-lobed cookie), the usage celebration (cookie instead of
  the glass bubble), the loading indicator (a turning, breathing cookie in the buffering overlay and on "busy"
  message cards), an alert cookie on error cards, two very faint cookies behind the menus (only when the animated
  background is Off).
* **Typography**: one font file on the console, so weight is faux: headlines (>= 30 px) and the focused label are
  drawn twice, ~0.03 em apart. Sizes are Ufin's existing scale.
* **Motion** (`m3e::Spring`, under-damped ~0.6, fixed 1/120 s steps so it is frame-rate independent): focus
  pill morph (`FocusMorph`, 8 slots, keyed by the surface's rectangle), play/pause and toggle buttons morph
  circle <-> rounded square (and square up while pressed), dialog card spring-in (scale 0.9 -> 1 + fade, done by
  rewriting the card's new vertices, only while the spring runs), squiggle amplitude eases to flat on pause.
  Focus *position* changes are immediate; only the shape eases.
* **Surfaces** (`m3eSurface`): Panel/Row = tonal container; RowFocus/ButtonHot = primary-container pill + ring;
  RowFocusDim = secondary-container pill; Button = surfaceHigh; Pressed = squarer + darker; Disabled = faded;
  Overlay (over video) = translucent tonal scrim, never samples the picture; Dialog (Choice, Help, overlay
  messages) = soft shadow + still frost of the shared blur at ~44 % + 86 % tonal tint.

## Cost / hardware notes
* GPU: nothing new. M3E reuses Liquid Glass's tiny (~1/4-res) shared blur textures, **without** the full-screen
  refraction snapshot: `Gfx::setGlass(true, ..., refraction=false)`. The blur is generated once (not animated) and
  again only when the accent or light/dark changes; with Rainbow the accent is quantised to 5 bits/channel so it
  does not regenerate every frame. If the textures cannot be created, dialogs fall back to the opaque tint.
* CPU: cookies are <= 96 triangles (2 per menu frame); the squiggle <= ~330 points; no per-frame allocation (stack
  arrays); video playback is never touched -- while video is presented the blur does nothing (existing rule) and the
  HUD surfaces are opaque-ish scrims.
* **Not verified on a console.** This was built and tested with the host harness (software rasteriser); no Wii U
  / Cemu run was possible. See the implementation report for the list of things to check on hardware.

## Tests
`tests/host/test_m3e_core.cpp` (palette contrast for 13 accents x light/dark, hue follows accent, cookie/squiggle
bounds, spring overshoot and frame-rate independence, focus table), `testExpressive` in `test_ui.cpp` (style
exclusivity, palette on screen, accent / light / Rainbow, focus morph over frames, dialog entrance, wavy vs flat
progress, every screen draws, Plain restored), and the M3E cases in `test_config.cpp`. With
`UFIN_UI_PREVIEW=<dir>` the `m3e_*` screens are written out for eyeballing.

## Motion and shape details (second pass)

- **Loading indicator** (`m3e::loaderPose/loaderPoints`): one shape turning while it morphs through Material's own sequence (SoftBurst, Cookie9Sided, Pentagon, Pill, Sunny, Cookie4Sided, Oval). 48 points, one triangle fan. Used by every M3E spinner (buffering, loading cards).
- **Cover art** is clipped to the cookie (`m3eCookieImage`: a textured fan, no extra pass) on Now Playing and Up Next.
- **Animated icons**: `drawIcon` plays a per-icon flourish (hop, wiggle, turn, heartbeat...) when the icon comes on (focused row, selected sidebar item, appearing tile) via `IconFxTable`; the effect rewrites the icon's few vertices after drawing. Play/pause is a true shape morph; shuffle wiggles and repeat spins when toggled.
- **Button group**: pressing a Now Playing button swells it and springs its neighbours aside (`neighbourPush`), like Material's connected button groups.
- **Tonal buttons** carry a visible accent tint (`secondaryContainer`, chroma = 0.55 x accent saturation).

### Third pass
- The wavy progress line eases onto the centre line at both ends (anchored), wavelength 50 px.
- Icon animations replay on every event (`tag` in `IconFxTable::get`): repeat turns a full lap on each press or mode change, shuffle flips over, the seek icons are Android-style circular arrows that turn one lap in the direction of a seek (detected from a jump in position or a touch press), the volume icon nudges per volume step. Spins accumulate on a monotone spring, so they never unwind.
- `ui::pulse()` (called by main on A / tap in the browser and settings) makes the rows above and below the activated one bounce away and replays the activated row's icon animation.
- Profile cookie: the photo's texture coordinates follow the scalloped outline, so the picture is no longer stretched.

### Popup backdrop (Android-shade style)
Behind a dialog the whole screen goes out of focus and takes an accent wash. `m3e::Haze` rasterises the background draw list onto a 64x36 grid on the CPU (a few thousand tiny triangles, glyph quads counted at ~26 % ink, pictures skipped), box-blurs it, and `m3eHazeDraw` paints it as a 34x20 vertex-coloured mesh. No snapshot, no texture, no extra GPU pass; rebuilt when the popup appears and every ~0.25 s. Over video the haze has no coverage, so the video shows through the wash (it is not blurred). The dialog card is a nearly opaque tonal sheet on top.

### Progress indicator (per the M3E spec, 1.5 px per dp)
Stroke 4 dp for played part and track, 4 dp gap, wavelength 40 dp, amplitude 3 dp (10 dp tall), round caps, a 4 dp stop dot near the track's end; only the first and last 6 px of the wave are locked to the centre line. A slim vertical handle sits in the gap (as in the player reference).
