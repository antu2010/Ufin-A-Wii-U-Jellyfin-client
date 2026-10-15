#!/usr/bin/env bash
# Builds and runs Ufin's host-side tests with the system g++ (no Wii U
# toolchain needed). Everything that doesn't touch wut/GX2 is covered:
# the ui/ screens, JellyfinClient + http_client, the config loader, and
# -- given a host build of FFmpeg -- FrameQueue, AudioOutput's clock, and
# the real Decoder/HttpStreamIO/HttpStreamReader chain over HTTP.
#
# Usage:
#   tests/host/run_tests.sh                 # tests that need no FFmpeg
#   FFMPEG_HOST=/path/to/ffmpeg-prefix tests/host/run_tests.sh
#
# FFMPEG_HOST must point at an install prefix (include/ + lib/) of a
# static FFmpeg build. Any 4.x build works; to mirror the Wii U exactly,
# build GaryOderNichts/FFmpeg-wiiu with:
#   ./configure --prefix=$FFMPEG_HOST --disable-everything --disable-programs \
#     --disable-doc --disable-asm --disable-autodetect \
#     --enable-decoder=h264,aac --enable-demuxer=mov --enable-parser=h264,aac \
#     --enable-bsf=h264_mp4toannexb --enable-protocol=file --enable-static
# The decoder test also needs an `ffmpeg` binary with libx264 on PATH to
# generate its sample clip.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/../../src" && pwd)"
OUT="${OUT:-$HERE/build}"
mkdir -p "$OUT"

CXX="${CXX:-g++}"
CXXFLAGS="-std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -pthread -I$HERE -I$HERE/stubs -I$SRC -I$SRC/vendor"

status=0
run() { # name, then sources...
    local name="$1"; shift
    echo "== $name"
    if ! $CXX $CXXFLAGS "$@" -o "$OUT/$name" $LIBS 2>"$OUT/$name.build.log"; then
        echo "  BUILD FAILED (see $OUT/$name.build.log)"; head -30 "$OUT/$name.build.log"; status=1; return
    fi
    if ! "$OUT/$name" $RUN_ARGS; then status=1; fi
}

LIBS=""
RUN_ARGS=""
# Dear ImGui is compiled once into a static library for the UI test.
IMGUI="$SRC/vendor/imgui"
if [ ! -f "$OUT/libimgui.a" ] || [ "$IMGUI/imgui.cpp" -nt "$OUT/libimgui.a" ]; then
    echo "== building imgui (once)"
    rm -f "$OUT"/imgui_*.o
    for f in imgui imgui_draw imgui_tables imgui_widgets; do
        $CXX -std=c++17 -O2 -I"$IMGUI" -c "$IMGUI/$f.cpp" -o "$OUT/imgui_$f.o" || status=1
    done
    ar rcs "$OUT/libimgui.a" "$OUT"/imgui_*.o
fi
UI_FONT="${UI_FONT:-/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf}"
[ -f "$UI_FONT" ] && RUN_ARGS="$UI_FONT"
CXXFLAGS="$CXXFLAGS -I$IMGUI" LIBS="$OUT/libimgui.a" \
    run test_ui "$HERE/test_ui.cpp" "$SRC/ui/app_ui.cpp" "$SRC/ui/format.cpp" "$SRC/ui/glass_core.cpp" \
    "$SRC/ui/model3d.cpp" "$SRC/image_cache.cpp" "$SRC/vendor/cJSON.c"
RUN_ARGS=""
run test_config    "$HERE/test_config.cpp" "$SRC/config_loader.cpp" "$SRC/vendor/cJSON.c"
run test_jellyfin  "$HERE/test_jellyfin.cpp" "$SRC/jellyfin_client.cpp" "$SRC/http_client.cpp" "$SRC/vendor/cJSON.c"
run test_image_cache "$HERE/test_image_cache.cpp" "$SRC/image_cache.cpp"
run test_input_map "$HERE/test_input_map.cpp"
RUN_ARGS="$HERE/../../patches/ffmpeg-wiiu-fixes.patch" run test_h264_geometry "$HERE/test_h264_geometry.cpp"
run test_playback_logic "$HERE/test_playback_logic.cpp"
# HttpStreamReader: failure causes, longer / cancellable startup wait (no FFmpeg needed).
run test_http_stream_reader "$HERE/test_http_stream_reader.cpp" "$SRC/http_stream_reader.cpp"
run test_h264_diag "$HERE/test_h264_diag.cpp"
# Startup update check: the decisions (no network), the HTTPS client's core against scripted servers, and
# the glue to the console's system TLS (NSSL) against a fake NSSL (the real TLS cannot run on a PC).
run test_update_check "$HERE/test_update_check.cpp" "$SRC/update_check.cpp" "$SRC/vendor/cJSON.c"
run test_https_client "$HERE/test_https_client.cpp" "$SRC/https_client.cpp" "$SRC/http_client.cpp"
CXXFLAGS="$CXXFLAGS -DUFIN_TLS_NSSL" run test_https_nssl "$HERE/test_https_nssl.cpp" "$HERE/stubs/fake_nssl.cpp" "$SRC/https_client.cpp" "$SRC/http_client.cpp"
if [ -x "$OUT/test_https_nssl" ]; then "$OUT/test_https_nssl" noroots || status=1; "$OUT/test_https_nssl" emulator || status=1; fi
# The DNS client (packets + a fake UDP server), and our own TLS client (vendored Mbed TLS, compiled once into a
# static library here) against a real `openssl s_server` with a throw-away CA.
run test_dns_client "$HERE/test_dns_client.cpp" "$SRC/dns_client.cpp"
MBED="$SRC/vendor/mbedtls"
if [ ! -f "$OUT/libmbedufin.a" ] || [ "$MBED/include/mbedtls/config.h" -nt "$OUT/libmbedufin.a" ]; then
    echo "== building mbedtls (once)"
    rm -rf "$OUT/mbed"; mkdir -p "$OUT/mbed"
    for f in "$MBED"/library/*.c; do
        gcc -std=gnu99 -O1 -c -I"$MBED/include" -I"$MBED/library" \
            "$f" -o "$OUT/mbed/$(basename "$f" .c).o" || status=1
    done
    rm -f "$OUT/libmbedufin.a"; ar rcs "$OUT/libmbedufin.a" "$OUT"/mbed/*.o
fi
if command -v openssl >/dev/null 2>&1; then
    CXXFLAGS="$CXXFLAGS -DUFIN_TLS_MBEDTLS -I$MBED/include" LIBS="$OUT/libmbedufin.a" \
        run test_https_mbedtls "$HERE/test_https_mbedtls.cpp" "$SRC/https_client.cpp" "$SRC/http_client.cpp" "$SRC/dns_client.cpp" "$SRC/tls_roots.cpp"
else
    echo "== test_https_mbedtls skipped (no openssl binary)"
fi
run test_login_utils "$HERE/test_login_utils.cpp"
# Who's watching?: the rules, bounce and config round trip; then the screen itself (app_ui.cpp also links
# model3d.cpp when the Help window's SD-card models are in the tree).
run test_profiles "$HERE/test_profiles.cpp" "$SRC/config_loader.cpp" "$SRC/vendor/cJSON.c"
PUI_EXTRA=""
[ -f "$SRC/ui/model3d.cpp" ] && PUI_EXTRA="$SRC/ui/model3d.cpp $SRC/image_cache.cpp $SRC/vendor/cJSON.c"
CXXFLAGS="$CXXFLAGS -I$IMGUI" LIBS="$OUT/libimgui.a" \
    run test_profiles_ui "$HERE/test_profiles_ui.cpp" "$SRC/ui/app_ui.cpp" "$SRC/ui/format.cpp" "$SRC/ui/glass_core.cpp" $PUI_EXTRA
run test_play_queue "$HERE/test_play_queue.cpp"
run test_playback_controls "$HERE/test_playback_controls.cpp"
run test_item_labels "$HERE/test_item_labels.cpp" "$SRC/item_labels.cpp" "$SRC/ui/format.cpp"
run test_audio_visualizer "$HERE/test_audio_visualizer.cpp" "$SRC/media/audio_visualizer.cpp"
run test_playback_state "$HERE/test_playback_state.cpp"
run test_glass_core "$HERE/test_glass_core.cpp" "$SRC/ui/glass_core.cpp"
run test_quality_presets "$HERE/test_quality_presets.cpp"
run test_h264_profile "$HERE/test_h264_profile.cpp"
run test_frame_reorder "$HERE/test_frame_reorder.cpp"
run test_backdrop_tracker "$HERE/test_backdrop_tracker.cpp"
run test_usage_timer "$HERE/test_usage_timer.cpp"
run test_help_content "$HERE/test_help_content.cpp"
run test_model3d "$HERE/test_model3d.cpp" "$SRC/ui/model3d.cpp" "$SRC/image_cache.cpp" "$SRC/vendor/cJSON.c"
RUN_ARGS="on"; run test_ufin_log "$HERE/test_ufin_log.cpp"
RUN_ARGS="off"; "$OUT/test_ufin_log" off || status=1   # same binary: the pending state lives per process
RUN_ARGS=""

if [ -n "${FFMPEG_HOST:-}" ] && [ -d "$FFMPEG_HOST/include/libavcodec" ]; then
    CXXFLAGS="$CXXFLAGS -I$FFMPEG_HOST/include"
    LIBS="-L$FFMPEG_HOST/lib -lavformat -lavcodec -lswresample -lswscale -lavutil -lm -lpthread"

    run test_frame_queue "$HERE/test_frame_queue.cpp"
    run test_audio_clock "$HERE/test_audio_clock.cpp" "$SRC/media/audio_output.cpp" "$HERE/stubs/fake_sdl.cpp"

    CLIP="$OUT/sample_frag.mp4"
    if command -v ffmpeg >/dev/null 2>&1; then
        if ffmpeg -v error -y -f lavfi -i "testsrc2=size=320x240:rate=30" \
                  -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 3 \
                  -c:v libx264 -profile:v baseline -pix_fmt yuv420p -g 30 \
                  -c:a aac -b:a 128k -ac 2 \
                  -movflags frag_keyframe+empty_moov+delay_moov "$CLIP" 2>"$OUT/ffmpeg.log"; then
            RUN_ARGS="$CLIP"
            run test_decoder_stream "$HERE/test_decoder_stream.cpp" "$SRC/media/decoder.cpp" "$SRC/ufin_log.cpp" \
                "$SRC/media/http_stream_io.cpp" "$SRC/http_stream_reader.cpp"
            RUN_ARGS=""
            LONG="$OUT/sample_long_frag.mp4"
            # 5 s fragments, like Jellyfin's: video run, then audio run.
            if ffmpeg -v error -y -f lavfi -i "testsrc2=size=320x240:rate=30" \
                      -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 12 \
                      -c:v libx264 -profile:v baseline -pix_fmt yuv420p -g 150 \
                      -c:a aac -b:a 128k -ac 2 \
                      -movflags frag_keyframe+empty_moov+delay_moov "$LONG" 2>>"$OUT/ffmpeg.log"; then
                RUN_ARGS="$LONG"
                run test_playback_schedule "$HERE/test_playback_schedule.cpp" "$SRC/media/decoder.cpp" "$SRC/ufin_log.cpp" \
                    "$SRC/media/http_stream_io.cpp" "$SRC/http_stream_reader.cpp"
                RUN_ARGS=""
            fi
            # The real Player against a Jellyfin-like server that transcodes
            # with ffmpeg -ss for every StartTimeTicks (skipping end to end).
            SRC60="$OUT/sample_60s.mp4"
            if ffmpeg -v error -y -f lavfi -i "testsrc2=size=320x240:rate=30" \
                      -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 60 \
                      -c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac "$SRC60" 2>>"$OUT/ffmpeg.log"; then
                H="$OUT/harness"
                mkdir -p "$H"
                cp "$SRC/media/player.cpp" "$H/player.cpp"
                cp "$HERE/fake_media/video_output.h" "$H/video_output.h"
                RUN_ARGS="$SRC60"
                CXXFLAGS="$CXXFLAGS -I$H -I$SRC/media" \
                    run test_player_seek "$HERE/test_player_seek.cpp" "$H/player.cpp" "$SRC/media/decoder.cpp" "$SRC/ufin_log.cpp" \
                    "$SRC/media/audio_output.cpp" "$SRC/media/http_stream_io.cpp" "$SRC/http_stream_reader.cpp" \
                    "$HERE/stubs/fake_sdl.cpp"
                RUN_ARGS=""
                # Subtitles burned in at a start offset: the real URL builder and Player
                # against a fake Jellyfin that applies its burn-in timestamp shift unless
                # CopyTimestamps=true (reproduces the frozen-picture-with-audio report).
                RUN_ARGS="$SRC60"
                CXXFLAGS="$CXXFLAGS -I$H -I$SRC/media" \
                    run test_player_burnin "$HERE/test_player_burnin.cpp" "$H/player.cpp" "$SRC/media/decoder.cpp" "$SRC/ufin_log.cpp" \
                    "$SRC/media/audio_output.cpp" "$SRC/media/http_stream_io.cpp" "$SRC/http_stream_reader.cpp" \
                    "$SRC/jellyfin_client.cpp" "$SRC/http_client.cpp" "$SRC/vendor/cJSON.c" "$HERE/stubs/fake_sdl.cpp"
                RUN_ARGS=""
            fi
            # Graceful rebuffering: the real Player against a fake Jellyfin whose
            # bandwidth is throttled below the stream's bitrate (needs ffmpeg with
            # libx264; takes ~1-2 minutes because it plays in real time).
            H2="$OUT/harness_rebuffer"
            mkdir -p "$H2"
            cp "$SRC/media/player.cpp" "$H2/player.cpp"
            cp "$HERE/fake_media/video_output.h" "$H2/video_output.h"
            CXXFLAGS="$CXXFLAGS -I$H2 -I$SRC/media" \
                run test_player_rebuffer "$HERE/test_player_rebuffer.cpp" "$H2/player.cpp" "$SRC/media/decoder.cpp" "$SRC/ufin_log.cpp" \
                "$SRC/media/audio_output.cpp" "$SRC/media/http_stream_io.cpp" "$SRC/http_stream_reader.cpp" \
                "$HERE/stubs/fake_sdl.cpp"
            SONG="$OUT/sample_song.m4a"
            # A short, loud tone as fragmented MP4 audio, like Jellyfin's
            # /Audio/.../stream.mp4 transcode.
            if ffmpeg -v error -y -f lavfi -i "sine=frequency=330:sample_rate=48000" -af volume=6 -t 1.2 \
                      -c:a aac -b:a 128k -ac 2 -movflags frag_keyframe+empty_moov+delay_moov "$SONG" \
                      2>>"$OUT/ffmpeg.log"; then
                RUN_ARGS="$SONG"
                run test_menu_music "$HERE/test_menu_music.cpp" "$SRC/media/menu_music.cpp" "$SRC/media/file_stream_io.cpp" \
                    "$SRC/media/decoder.cpp" "$SRC/ufin_log.cpp" "$SRC/media/audio_output.cpp" "$SRC/media/http_stream_io.cpp" \
                    "$SRC/http_stream_reader.cpp" "$HERE/stubs/fake_sdl.cpp"
                RUN_ARGS=""
            fi
        else
            echo "== test_decoder_stream: SKIPPED (ffmpeg could not create the sample clip; see $OUT/ffmpeg.log)"
        fi
    else
        echo "== test_decoder_stream: SKIPPED (no ffmpeg binary on PATH)"
    fi
else
    echo "== FFmpeg-dependent tests SKIPPED (set FFMPEG_HOST to a static FFmpeg install prefix)"
fi

exit $status
