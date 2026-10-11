// Dear ImGui compile-time configuration for Sirius, in place of editing
// third_party/imgui/imconfig.h. Every build of Dear ImGui in this repository
// (siriusd, the host harness, the unit tests) uses it, via IMGUI_USER_CONFIG.
#pragma once

// New code only: the pre-1.92 names are not needed and should not creep in.
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS

// "Open in shell" would fork and exec a browser. There is none.
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
