cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED ENV{DEVKITPRO})
    message(FATAL_ERROR "DEVKITPRO env var not set -- did the devkitPro installer add it? Restart your terminal after installing.")
endif()

set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/wut/share/wut.toolchain.cmake")

project(ufin CXX C)

add_executable(ufin
    src/main.cpp
    src/http_client.cpp
    src/jellyfin_client.cpp
    src/vendor/cJSON.c
)

target_include_directories(ufin PRIVATE src src/vendor)

# whb = "Wii U Homebrew Bootstrap" helper lib bundled with wut, gives us
# WHBProcInit/WHBLogConsole/etc without hand-rolling the app lifecycle.
target_link_libraries(ufin wut)

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
        COMMENT "Packing ufin.wuhb"
    )
else()
    message(WARNING "wuhbtool not found -- you'll get ufin.rpx but no .wuhb. Install the wiiu-dev group with pacman.")
endif()
