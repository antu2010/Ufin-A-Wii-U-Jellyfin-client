Dear ImGui 1.92.9b with the Wii U GX2 renderer backend, taken unmodified from
https://github.com/dkosmari/imgui (commit c60079c8ec43c16a744acaf04569390c7538d1de).

- Dear ImGui: MIT, Copyright (c) 2014-2026 Omar Cornut (see LICENSE.txt)
- backends/imgui_impl_gx2.*: Copyright (C) 2023 GaryOderNichts, (C) 2026 Daniel K.O. (same license)

Only the core library and the GX2 renderer are used. Ufin feeds input and
display size to ImGui itself (src/ui/gfx.cpp), so imgui_impl_wiiu is not
included -- its built-in software keyboard would clash with Ufin's search
keyboard (src/ui/keyboard.cpp).
