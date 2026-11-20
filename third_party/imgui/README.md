Dear ImGui 1.92.9 (https://github.com/ocornut/imgui, MIT license, see LICENSE.txt), unchanged.

Kestral builds it from these sources (instead of a package) so it can turn on Dear ImGui's item
hooks (IMGUI_ENABLE_TEST_ENGINE). Kestral uses them to know every button, menu, field and window by
name, which lets MCP apps click things by their label (see src/ui/UiItems.cpp).
