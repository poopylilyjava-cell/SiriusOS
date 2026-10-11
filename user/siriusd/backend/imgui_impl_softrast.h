// dear imgui: Renderer Backend for a plain 32-bit framebuffer, in software.
//
// Rasterizes ImDrawData -- clipped, textured, vertex-coloured triangles --
// into memory. No GPU, no OS: this file and imgui_impl_sirius are the whole
// Dear ImGui port (ADR 0003), and both build unchanged on Linux, where
// tests/host_harness and tests/unit run them.
//
// Implemented features:
//  [X] Renderer: Texture updates support for dynamic font atlas (ImGuiBackendFlags_RendererHasTextures).
//  [X] Renderer: Large meshes support (64k+ vertices) even with 16-bit indices (ImGuiBackendFlags_RendererHasVtxOffset).
//  [X] Renderer: User textures (ImGui_ImplSoftRast_Texture), RGBA32 or Alpha8; RGBA32 also as
//                premultiplied or opaque BGRA, which is what Wayland clients draw.
//
// The target is XRGB8888: one uint32_t per pixel, 0x00RRGGBB. The top byte is
// written as zero and never read.
#pragma once
#include "imgui.h"
#ifndef IMGUI_DISABLE

#include <stdint.h>

// Where to draw. Pitch is in bytes and may exceed Width * 4.
struct ImGui_ImplSoftRast_Target
{
    uint32_t*   Pixels;
    int         Width;
    int         Height;
    int         Pitch;
};

// The order and meaning of the four bytes of an RGBA32 texel.
enum ImGui_ImplSoftRast_Layout
{
    ImGui_ImplSoftRast_Layout_RGBA,             // R, G, B, A; straight alpha: Dear ImGui's own
    ImGui_ImplSoftRast_Layout_BGRA_Premultiplied, // B, G, R, A; colour already multiplied by alpha: wl_shm's ARGB8888
    ImGui_ImplSoftRast_Layout_BGRX,             // B, G, R, ignored: opaque, wl_shm's XRGB8888 (and the target's own)
};

// A texture the application owns. Pass its address as the ImTextureID:
//   ImGui::Image((ImTextureID)(intptr_t)&my_texture, size);
// Pixels are read at draw time, so they may change from frame to frame.
// RGBA32 rows are Width * 4 bytes, laid out as `Layout` says.
struct ImGui_ImplSoftRast_Texture
{
    int                     Width;
    int                     Height;
    ImTextureFormat         Format;
    const unsigned char*    Pixels;
    ImGui_ImplSoftRast_Layout Layout = ImGui_ImplSoftRast_Layout_RGBA;
};

// What the last RenderDrawData did, for profiling: where the pixels went.
struct ImGui_ImplSoftRast_Stats
{
    int     Rects;              // axis-aligned quads taken by the fast paths
    int     Triangles;          // everything else
    int64_t PixelsFilled;       // by rects of one solid colour
    int64_t PixelsTextured;     // by textured rects: text
    int64_t PixelsShaded;       // inside general triangles
};

IMGUI_IMPL_API bool     ImGui_ImplSoftRast_Init();
IMGUI_IMPL_API void     ImGui_ImplSoftRast_Shutdown();
IMGUI_IMPL_API void     ImGui_ImplSoftRast_NewFrame();
IMGUI_IMPL_API void     ImGui_ImplSoftRast_RenderDrawData(ImDrawData* draw_data, const ImGui_ImplSoftRast_Target& target);

// Fill the whole target with one opaque colour (an IM_COL32 value).
IMGUI_IMPL_API void     ImGui_ImplSoftRast_Clear(const ImGui_ImplSoftRast_Target& target, ImU32 col);

// Called by RenderDrawData for textures that need it; exposed for symmetry with other backends.
IMGUI_IMPL_API void     ImGui_ImplSoftRast_UpdateTexture(ImTextureData* tex);

IMGUI_IMPL_API const ImGui_ImplSoftRast_Stats& ImGui_ImplSoftRast_GetStats();

#endif // #ifndef IMGUI_DISABLE
