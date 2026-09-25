cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED ENV{DEVKITPRO})
    message(FATAL_ERROR "DEVKITPRO env var not set -- did the devkitPro installer add it? Restart your terminal after installing.")
endif()

set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/wut/share/wut.toolchain.cmake")

project(ufin CXX C)

# FFmpeg (built from FFmpeg-wiiu, see configure-wiiu) installs here --
# confirmed by actually running the build, not a guess.
include_directories("$ENV{DEVKITPRO}/portlibs/ppc/include")
link_directories("$ENV{DEVKITPRO}/portlibs/ppc/lib")

# SDL2 (wiiu-sdl2, installed via dkp-pacman) -- devkitPro's convention for
# Wii U-specific portlibs is a separate "wiiu" folder, distinct from the
# generic "ppc" one FFmpeg's own configure script chose. UNVERIFIED: if
# the build fails to find SDL2 headers/libs, run
#   find /opt/devkitpro/portlibs -maxdepth 3 -iname '*SDL2*'
# and fix these two paths to match reality.
include_directories("$ENV{DEVKITPRO}/portlibs/wiiu/include")
link_directories("$ENV{DEVKITPRO}/portlibs/wiiu/lib")

add_executable(ufin
    src/main.cpp
    src/ufin_log.cpp
    src/config_loader.cpp
    src/http_client.cpp
    src/http_stream_reader.cpp
    src/jellyfin_client.cpp
    src/item_labels.cpp
    src/image_cache.cpp
    src/input.cpp
    src/vendor/cJSON.c
    src/media/http_stream_io.cpp
    src/media/decoder.cpp
    src/media/video_output.cpp
    src/media/audio_output.cpp
    src/media/player.cpp
    src/media/menu_music.cpp
    src/ui/app_ui.cpp
    src/ui/format.cpp
    src/ui/gfx.cpp
    src/ui/gpu_image.cpp
    src/ui/keyboard.cpp
    src/vendor/imgui/imgui.cpp
    src/vendor/imgui/imgui_draw.cpp
    src/vendor/imgui/imgui_tables.cpp
    src/vendor/imgui/imgui_widgets.cpp
    src/vendor/imgui/backends/imgui_impl_gx2.cpp
)

target_include_directories(ufin PRIVATE src src/vendor src/vendor/imgui)
# ImGui's default "open URL in shell" helper uses fork/execvp/waitpid,
# which the Wii U doesn't have (link error). Ufin never uses it.
target_compile_definitions(ufin PRIVATE IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)

# Compile our GLSL shaders (src/media/shaders/nv12_video.{vert,frag}) into
# a .gsh binary via glslcompiler.elf (from Exzap/CafeGLSL -- a Mesa-based
# GLSL-to-Latte/GX2 shader compiler, MIT-licensed for its own additions),
# then embed that binary into a C header via tools/bin2h.py so it ships
# inside the executable rather than needing to be loaded from the SD
# card at runtime.
#
# glslcompiler.elf itself is NOT part of this repo -- download it from
# https://github.com/Exzap/CafeGLSL/releases and place it at
# tools/glslcompiler.elf before building.
set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/media/shaders)
set(GLSLCOMPILER ${CMAKE_CURRENT_SOURCE_DIR}/tools/glslcompiler.elf)
set(BIN2H ${CMAKE_CURRENT_SOURCE_DIR}/tools/bin2h.py)

add_custom_command(
    OUTPUT ${SHADER_DIR}/nv12_video_shader.h
    COMMAND ${GLSLCOMPILER} -vs ${SHADER_DIR}/nv12_video.vert -ps ${SHADER_DIR}/nv12_video.frag
            -o ${SHADER_DIR}/nv12_video.gsh
    COMMAND python3 ${BIN2H} ${SHADER_DIR}/nv12_video.gsh ${SHADER_DIR}/nv12_video_shader.h nv12_video_shader
    DEPENDS ${SHADER_DIR}/nv12_video.vert ${SHADER_DIR}/nv12_video.frag ${GLSLCOMPILER} ${BIN2H}
    COMMENT "Compiling nv12_video shader (glslcompiler.elf + bin2h.py)"
)
add_custom_target(ufin_shaders DEPENDS ${SHADER_DIR}/nv12_video_shader.h)
add_dependencies(ufin ufin_shaders)

# whb = "Wii U Homebrew Bootstrap" helper lib bundled with wut, gives us
# WHBProcInit/OSScreen-adjacent helpers without hand-rolling the app
# lifecycle. In current wut releases it's merged into libwut.a rather
# than shipping as a separate libwhb.a.
#
# FFmpeg libs: order matters for static linking -- avformat depends on
# avcodec, both depend on avutil, swresample/swscale depend on avutil.
# Listing avutil last lets the linker resolve symbols pulled in by the
# others above it. swscale is still linked because the h264_wiiu decoder
# inside libavcodec references it, even though Ufin itself no longer
# converts pixels on the CPU (the NV12 shader does that on the GPU).
target_link_libraries(ufin
    wut
    SDL2
    avformat
    avcodec
    swresample
    swscale
    avutil
)

wut_create_rpx(ufin)

# Package into a .wuhb so it shows up directly on the Wii U menu under
# Aroma, instead of needing the Homebrew Launcher.
find_program(WUHBTOOL wuhbtool)
if(WUHBTOOL)
    add_custom_command(TARGET ufin POST_BUILD
        COMMAND ${WUHBTOOL} "$<TARGET_FILE_DIR:ufin>/ufin.rpx" "${CMAKE_BINARY_DIR}/ufin.wuhb"
            --name="Ufin"
            --short-name="Ufin"
            --author="antu2010"
            --icon="${CMAKE_CURRENT_SOURCE_DIR}/icon.png"
        COMMENT "Packing ufin.wuhb"
    )
else()
    message(WARNING "wuhbtool not found -- you'll get ufin.rpx but no .wuhb. Install the wiiu-dev group with pacman.")
endif()
