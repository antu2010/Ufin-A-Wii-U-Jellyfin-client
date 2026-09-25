# Fixes and additions on top of "Add video playback and frontend UI"

Found while testing the PR in Cemu 2.6 (Linux) against Jellyfin 12.

## Build
- **CMakeLists.txt:** `src/config_loader.cpp` was missing from `add_executable`, so the
  Wii U build failed to link (`undefined reference to loadConfigFromSD`). The host
  tests compiled it, so they didn't catch this.
- **FFmpeg-wiiu:** new `patches/ffmpeg-wiiu-fixes.patch`, see "Playback" and the README.
- Removed a stray `FETCH_HEAD` that was committed by accident.

## Jellyfin 12 compatibility
- Login now sends `Authorization: MediaBrowser ...` instead of `X-Emby-Authorization`.
  Jellyfin 12 disables legacy authorization by default, ignores the old header, and
  rejects the login with `400 Error processing request`.
- Stream URLs use `ApiKey=` instead of `api_key=`, for the same reason.
- Both forms have been accepted since Jellyfin 10.8, so older servers keep working.

## Playback
- **Crash on starting a movie:** `Decoder::open` called `avformat_find_stream_info()`.
  For H.264 it opens its own throwaway decoder (`h264_wiiu`, the only one in the build)
  and crashed inside `h264_wiiu_decode_frame` (seen via `try_decode_frame` in the Cemu
  crash dump). The mov demuxer already provides everything the decoders need, so the
  call is gone; the stream parameters are logged instead. The frame rate is unknown
  without it, and Player's existing 1/30 s fallback matches the rate requested from
  Jellyfin.
- **Crash while decoding** (pointer overwritten with `0x80808080`, i.e. grey NV12 pixels):
  `h264_wiiu` allocated `width*height*1.5` bytes, but the hardware writes a 256-pixel
  pitch and 16-row aligned height. Fixed in the FFmpeg-wiiu patch, which also reads the
  packet from `avpkt->data/size` and checks its allocations.
- **Picture and sound taking turns (every ~5 s):** a live fragmented MP4 read front to
  back delivers each fragment as all its video packets, then all its audio packets.
  Decoding in that order with a 6-frame video queue meant audio ran dry while video
  waited for the clock, and the other way round. The decoder now demuxes into
  per-stream queues of *compressed* packets (capped at 24 MB), and the decode thread
  decodes whichever stream needs a frame, reading from the network only when that
  stream has nothing buffered. `test_playback_schedule` reproduces this with 5 s
  fragments: the old order let audio run dry on 342 of 354 ticks, the new one on 0.
- Playback progress reports are sent from a background thread every 10 s. They were
  blocking HTTP calls on the render loop, once a second.
- **Real-hardware cache handling:** Cemu doesn't emulate CPU/GPU caches, so two
  gaps never showed there. The FFmpeg patch now calls `DCInvalidateRange` on the
  decoder's output buffer before copying it (the hardware decoder writes it
  directly to memory). `VideoOutput` now calls `GX2Invalidate(CPU_TEXTURE)` on
  each uploaded plane.
- Non-200 stream responses now log (and show) the start of the server's error body.
- `MediaSourceId` is passed for movies, so multi-version items pick a source.
- The non-streaming HTTP client has a 20 s timeout: an unreachable-but-accepting
  server used to hang the "Connecting..." screen forever.

## Live TV (new)
- Opening the Live TV view lists channels (`/LiveTv/Channels`) with channel numbers and
  what's on now.
- Playing a channel opens the live stream via `PlaybackInfo` with `AutoOpenLiveStream`,
  streams it through the same 720p H.264 transcode as movies (with `MediaSourceId`,
  `LiveStreamId` and `PlaySessionId`), reports playback with those ids, and closes the
  live stream afterwards so the tuner is released.

## Pause, skip and search (new)
- **Pause:** A pauses and resumes playback. The audio device is paused and the clock
  freezes, so the picture holds. The decoder fills its buffers and then stops reading,
  and Jellyfin sees `IsPaused` in the progress reports.
- **Skip:** Left goes back 10 s and Right forward 30 s. Presses within 0.6 s add up,
  so Right three times is one +90 s restart. The progressive transcode can't be
  seeked, so Jellyfin restarts it at `StartTimeTicks`. Positions stay item-relative
  whether the new stream's timestamps restart at 0 or keep the original ones.
  Reports now say `CanSeek` (false for Live TV, which has only B).
  Each stream start (including every skip) uses a new `PlaySessionId`. Jellyfin names
  a transcode's output after media + device + session only, not the start time, and
  serves an existing file from its beginning (`FileStreamResponseHelpers.GetTranscodedFile`).
  So without a fresh id, a skip replayed the old transcode from 0:00. The stop report
  names the session that was actually playing, so Jellyfin ends that transcode.
- **Search:** X opens the Wii U software keyboard (`nn::swkbd`, drawn with GX2, so
  OSScreen steps aside as it does for video). It searches movies, shows, episodes and
  music across all libraries. Episode results name their series.
- **Tests:** new `test_playback_controls`; `test_jellyfin` covers search,
  `StartTimeTicks`, `IsPaused` and `CanSeek`; `test_audio_clock` covers pause.

## Music queue and shuffle (new)
- **Queue:** picking a song plays it and the rest of the list after it. Albums and
  seasons are now listed in disc/track/episode order
  (`SortBy=ParentIndexNumber,IndexNumber,SortName`).
- **Shuffle:** + shuffles the selected album or playlist, or every song in the current
  list. The chosen first track plays first, then every other track once.
- **Track controls:** L/R (or ZL/ZR) go to the previous/next track. L restarts the
  song if it's more than 3 s in.
- **Now Playing** shows "Track n of m" (and "Shuffle") plus the next track's name.
- **Tests:** `PlayQueue` is pure logic, covered by `test_play_queue`; `test_ui` covers
  the new Now Playing lines.

## Controllers, Continue watching, tracks, autoplay, watched & favourites (new)
- **Wii Remote, Nunchuk, Classic Controller, Pro Controller** (`src/input.*`,
  `src/input_map.h`): every controller is read each frame and translated into
  GamePad button bits, so menus, playback and settings work with the GamePad put away.
  - The copied button values are checked against wut's with `static_assert`s.
  - The software keyboard opens for the controller used last (on the TV for a Wii
    Remote) and gets the Wii Remote readings for pointing.
- **Home rows:**
  - "Continue watching" (`/UserItems/Resume`), "Next up" (`/Shows/NextUp`) and
    "Favourites" appear first on Home when they have anything in them.
  - A partly watched video offers **Resume from mm:ss / Start from the beginning**.
- **Audio & subtitles:** Y during a video opens a panel. Changing the audio track or
  subtitles (burned in with `SubtitleMethod=Encode`) restarts the stream at the same
  point, with `AudioStreamIndex` / `SubtitleStreamIndex`.
- **Autoplay:** when an episode finishes, "Up next" shows the following one (from
  `/Shows/{id}/Episodes`) with a 10 s countdown. The audio/subtitle language carries
  over. It can be turned off in Settings.
- **Watched & favourites:** lists show a tick for watched items, a progress bar under
  partly watched ones, an unwatched count on shows and seasons, and a heart for
  favourites.
  - Y toggles favourite and ZL toggles watched (`/UserFavoriteItems`,
    `/UserPlayedItems`, with the pre-10.9 routes as a fallback). Refresh moved to ZR.
  - Lists refresh after playback.
- **GamePad screen off during video:** a setting, and - during a video toggles it
  (`VPADSetLcdMode`). It always turns back on after playback and on exit.
- **Other:**
  - The video test picture moved to Settings.
  - `http_delete` added.
  - `tools/check_wiiu_build.sh` scans the build for thread-local storage.
- **Tests:**
  - `test_input_map`: every controller.
  - `test_playback_logic`: resume offer, next episode, track stepping, language carry-over.
  - `test_jellyfin`: user data, home rows, the fallback routes, POST/DELETE flags,
    tracks, and track indices in URLs.
  - `test_item_labels`: badges and resume detail.
  - `test_ui`: badges, the resume dialog, "Up next" and the track panel.
  - Every Wii U source also compiles with a 32-bit big-endian PowerPC compiler
    against the unmodified wut headers, with no thread-local relocations.

## Sign-in screen, Quick Connect, Settings, menu music, CRT easter egg (new)
- **Sign-in screen:** fields for Server, Username and Password (typed with the Wii U
  keyboard; passwords are hidden), plus a Quick Connect button. The server address is
  forgiving: `192.168.1.100`, `192.168.1.100:8097` and `http://host:8096/web` all work,
  and `https` is rejected with a clear reason, since there's no TLS.
- **Quick Connect:** Jellyfin's `POST /QuickConnect/Initiate`, then polling
  `GET /QuickConnect/Connect` every 2 s, then `POST /Users/AuthenticateWithQuickConnect`.
  Ufin shows the code big; you approve it from any signed-in device. If Quick Connect
  is disabled, or the code expires, it says so.
- **Saved sign-in:** a token, user id and a per-console `device_id` go into
  config.json (`saveConfigToSD`, which creates the folder if needed and keeps keys
  Ufin doesn't know). The password is never written. At start-up the token is checked
  with `GET /Users/Me`; if it has expired, you're back on the sign-in screen with the
  server filled in. Old hand-written configs with a password still work.
- **Settings** (new sidebar section):
  - menu music on/off;
  - the signed-in account with Sign out (`POST /Sessions/Logout`);
  - About;
  - CRT mode, once it has been discovered.
- **Menu music:** press **-** on any song in your library to make it the menu music.
  `MenuMusic` streams it on a background thread through the normal
  decoder/AudioOutput path, at 35% volume (`AudioOutput::setVolume`), and loops it
  with a fresh play session each time. It stops for real playback and on the sign-in
  screen, and backs off if the song is gone. Nintendo's music is deliberately not
  bundled; bring your own.
  The audio device is opened and closed on the main thread; the worker only decodes
  and queues samples. SDL's Wii U driver moves device *opening* onto CPU core 1 (AX
  functions must run on one core), but `WIIUAUDIO_CloseDevice` calls `AXQuit()` on the
  caller's core. Closing from the worker thread could therefore leave AX broken for the
  next player. `test_menu_music` checks which thread opens and closes the device.
- **Look & feel settings,** saved in config.json:
  - **Accent colour:** Jellyfin blue, Purple, Green, Orange, Pink. It drives
    selections, buttons, progress bars and the logo.
  - **Animated background:** soft glows drifting behind the menus.
  - **CRT mode:** scanlines, a phosphor tint, a rolling band and rounded tube
    corners over everything, video included.
  - **Snow** over the menus.
  - **Clock** in the header.
- **Easter egg:** pressing A on *About* seven times, like a phone's build number,
  unlocks a hidden **Rainbow** accent that slowly cycles through the colours.
- **Tests:**
  - `test_login_utils`: address parsing.
  - `test_menu_music`: plays a real AAC stream, loops with a new session each time,
    plays at reduced volume, backs off on a missing song, and stops promptly.
  - `test_jellyfin`: token check, Quick Connect happy path and errors, sign-out, and
    the device id in headers.
  - `test_config`: tokens, saving, keeping unknown keys, never adding a password.
  - `test_audio_clock`: volume.
  - `test_ui`: login, Quick Connect, toast, CRT, Settings, accent colours
    (including Rainbow animating), clock, animated background and snow.
  - `test_player_seek`: the real `Player` against a Jellyfin-like server that
    transcodes with `ffmpeg -ss` for every `StartTimeTicks`. It plays, skips +30 s,
    checks playback continues from there, skips -10 s and checks again.

## Artwork: posters, album art, channel logos (new)
- **List rows** show each item's artwork in a fixed slot: posters, square covers and
  wide channel logos are fitted inside it. The item's icon stands in until the image
  loads.
- **Now Playing** shows the album art instead of the placeholder tile.
- **Which image:** the item's own Primary image, else its album's (songs), else its
  series' (episodes, seasons). The image tag is part of the cache key, so a changed
  image reloads. The server scales images (`maxWidth` / `maxHeight`) and sends JPEG.
- **`ImageCache`** (`src/image_cache.*`):
  - A background thread fetches and decodes with stb_image, newest request first, off
    the render loop.
  - The main thread uploads at most 2 textures per frame, so scrolling never stutters.
  - Requests for rows that scrolled away before loading are dropped.
  - Least-recently-used textures are evicted beyond 96, never one drawn this frame.
  - Failed images aren't retried in the same session.
- **Textures** (`src/ui/gpu_image.*`): GX2 textures laid out as `{GX2Texture, GX2Sampler}`,
  the GX2 ImGui renderer's documented form for app textures, with cache invalidation
  for real hardware.
- **Thread safety:** `gethostbyname` is now serialised, since requests run on several
  threads (UI, image loader, progress reports).
- **Tests:**
  - New `test_image_cache`, which passes under ThreadSanitizer and AddressSanitizer. It
    covers background load, one fetch per image, upload pacing, LRU eviction, failures,
    stale requests, cleanup, and real JPEG/PNG decoding and shrinking.
  - `test_ui` checks that artwork lands in the right slot with the right shape.
  - `test_jellyfin` covers image-tag fallbacks, the image path and binary fetch.

## New interface: GX2 + Dear ImGui (replaces OSScreen)
- **One display system for everything.** `ui::Gfx` sets up GX2 (WHBGfx) and Dear ImGui
  once and keeps them for the whole run. Menus, Now Playing, the keyboard and video all
  draw through it. The old switch from OSScreen to GX2 and back around every video is
  gone; it was the main suspect for "the picture only appears when stopping" on real
  hardware.
- **Screens** (`ui/app_ui.cpp`):
  - a dark theme with a left sidebar (Home, Live TV, Search);
  - a two-line list with icons, tags, a scrollbar and an `n / total` counter;
  - message cards (errors, loading);
  - Now Playing with a progress bar, queue and next track;
  - a HUD over video: title, time bar, a pause symbol, "+90 s -> 6:42" while collecting
    skips, and a LIVE badge.
- **Drawing:** everything is drawn with ImGui draw lists in a virtual 1280x720 space and
  scaled to the TV mode (720p/1080p). The GamePad shows a scaled copy.
- **Look:** the layout is inspired by CaféMP. No CaféMP code or artwork is used (its
  repository has no license); the icons are drawn with vector shapes in code.
- **Font:** text uses the console's own system font (`OSGetSharedData`), so accented
  letters show properly.
- **Touch:** tap a sidebar entry or a list row. Hit-testing uses the same layout
  functions as drawing (`ui/layout.h`).
- **VideoOutput** no longer initialises GX2. Each video frame is drawn as the underlay of
  an app frame, so the HUD can sit on top, and the picture keeps redrawing while paused.
  The TV picture is copied to the GamePad instead of being drawn twice.
- **Vendored libraries:** Dear ImGui 1.92.9b with the GX2 renderer from dkosmari/imgui
  (MIT), in `src/vendor/imgui/`. Its Wii U platform layer is not used: Ufin reads the
  GamePad and touch itself, and keeps its own swkbd search.
- **Tests:** `test_ui` renders every screen through ImGui and a small software
  rasterizer (`tests/host/soft_render.h`). It checks pixels (selection band, sidebar
  focus, scrollbar, HUD states) and the layout/hit-test maths. With
  `UFIN_UI_PREVIEW=<dir>` it writes each screen as an image (see `docs/screenshots/`).
  It also passes under AddressSanitizer/UBSan.
- **Removed:** the OSScreen UI: `os_screen_display`, `grid_probe`, `surface.h`,
  `screens.cpp` and their tests.

## UI overhaul (earlier, OSScreen -- superseded by the section above)
- **Measured text grid:** OSScreen text sits on different grids on hardware and in Cemu.
  With the old hardcoded 12x24 grid at (50,32), Cemu (16x24 from the edge) wrapped long
  lines back over the start of the row and drew the selection band a row below the
  cursor. `OSScreenDisplay` now measures the grid at start-up by drawing probes into its
  own framebuffer (`ui/grid_probe.h`, host-tested against fake screens) and logs the
  result. If measurement fails it falls back to the old values.
- **Header:** breadcrumb (keeps the deepest level when long), `n / total` counter, and an
  accent rule.
- **Detail line:** shows information on the selection (e.g. "Now: News - A: watch",
  "Season 2, Episode 5 - 42:00").
- **Friendly tags** (`src/item_labels.cpp`): year and duration for movies, `S2E5` for
  episodes, library kind for views, `LIVE` for channels.
- **Errors** are word-wrapped instead of cut off.
- **Accented Latin letters** show as their plain letter ("Citta") instead of "?".
- **Navigation:** fixed going back from two levels deep, which reloaded the folder you
  were in instead of its parent. Every level now keeps its list and selection, so B is
  instant and returns you to where you were.
- **Input:** hold up/down to scroll, L/R (or left/right) to page, Y to refresh, and the
  left stick works. On a login error, B retries.

## Tests
- `test_ui` runs every screen at both sizes with both grids, and checks that no text
  can exceed the grid.
- New `test_grid_probe` and `test_item_labels`.
- `test_jellyfin` covers the new auth header and `ApiKey`, Live TV, media sources and
  reporting ids.
- `test_decoder_stream` accepts an unknown frame rate.
- All pass: `tests/host/run_tests.sh`, and with `FFMPEG_HOST` set.

## Not verified yet
The Wii U build is syntax-checked against the wut headers and the host tests pass, but
none of this has run on a real console. The measured grid values printed at start-up
(`Ufin: TV text grid measured: ...`) are worth including in a hardware test report.
