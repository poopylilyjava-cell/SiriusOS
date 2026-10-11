// dear imgui: Renderer Backend for a plain 32-bit framebuffer, in software.
// See imgui_impl_softrast.h.
//
// How the time is spent decides the structure. Dear ImGui's output is almost
// entirely axis-aligned quads: every glyph, every frame and background, most
// borders. Those never go near the triangle rasterizer:
//
//   - a quad of one colour over the atlas' white pixel is a rectangle fill,
//     done a row at a time with SSE2 -- this is the fill rate of the desktop;
//   - a quad with a texture at one texel per pixel is a glyph, blitted;
//   - anything else (rounded corners, anti-aliased edges, circles, gradients,
//     scaled images) is a real triangle, and takes the general path.
//
// The general path uses integer edge functions with the top-left rule, so
// triangles that share an edge cover each pixel exactly once: with alpha
// blending, a pixel drawn twice along a seam is visible.

#include "imgui.h"
#ifndef IMGUI_DISABLE
#include "imgui_impl_softrast.h"

#include <math.h>
#include <string.h>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace {

struct ImGui_ImplSoftRast_Data
{
    ImGui_ImplSoftRast_Stats Stats;
};

ImGui_ImplSoftRast_Data* GetBackendData()
{
    return ImGui::GetCurrentContext() ? (ImGui_ImplSoftRast_Data*)ImGui::GetIO().BackendRendererUserData : nullptr;
}

void DrawCallback_ResetRenderState(const ImDrawList*, const ImDrawCmd*) {} // There is no state to reset.

// A colour with its channels apart, each 0..255.
struct Color { uint32_t r, g, b, a; };

inline Color Unpack(ImU32 col)
{
    return { (col >> IM_COL32_R_SHIFT) & 0xff, (col >> IM_COL32_G_SHIFT) & 0xff,
             (col >> IM_COL32_B_SHIFT) & 0xff, (col >> IM_COL32_A_SHIFT) & 0xff };
}

inline uint32_t Div255(uint32_t x) { x += 128; return (x + (x >> 8)) >> 8; }

inline uint32_t PackXrgb(const Color& c) { return (c.r << 16) | (c.g << 8) | c.b; }

// dst = src * a + dst * (1 - a), red and blue together in one multiply.
inline uint32_t Blend(uint32_t dst, uint32_t src_xrgb, uint32_t a)
{
    const uint32_t inv = 255 - a;
    uint32_t rb = (src_xrgb & 0xff00ff) * a + (dst & 0xff00ff) * inv + 0x800080;
    uint32_t g  = (src_xrgb & 0x00ff00) * a + (dst & 0x00ff00) * inv + 0x008000;
    rb = ((rb + ((rb >> 8) & 0xff00ff)) >> 8) & 0xff00ff;
    g  = ((g  + ((g  >> 8) & 0x00ff00)) >> 8) & 0x00ff00;
    return rb | g;
}

inline void PutPixel(uint32_t* dst, const Color& c)
{
    if (c.a == 255)    *dst = PackXrgb(c);
    else if (c.a != 0) *dst = Blend(*dst, PackXrgb(c), c.a);
}

// `count` pixels of one colour: the inner loop of every rectangle fill.
void FillSpan(uint32_t* dst, int count, uint32_t xrgb, uint32_t a)
{
    if (count <= 0 || a == 0)
        return;
    int i = 0;
    if (a == 255)
    {
#if defined(__SSE2__)
        const __m128i v = _mm_set1_epi32((int)xrgb);
        for (; i + 4 <= count; i += 4)
            _mm_storeu_si128((__m128i*)(dst + i), v);
#endif
        for (; i < count; i++)
            dst[i] = xrgb;
        return;
    }
#if defined(__SSE2__)
    // Per 16-bit channel: (src * a + 128) + dst * (255 - a), then the same
    // division by 255 as Div255(). The largest value, 255 * 255 + 128 + 254,
    // still fits in 16 bits.
    const __m128i zero = _mm_setzero_si128();
    const __m128i inv = _mm_set1_epi16((short)(255 - a));
    const __m128i src = _mm_add_epi16(
        _mm_mullo_epi16(_mm_unpacklo_epi8(_mm_set1_epi32((int)xrgb), zero), _mm_set1_epi16((short)a)),
        _mm_set1_epi16(128));
    for (; i + 4 <= count; i += 4)
    {
        const __m128i d = _mm_loadu_si128((const __m128i*)(dst + i));
        __m128i lo = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(d, zero), inv), src);
        __m128i hi = _mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(d, zero), inv), src);
        lo = _mm_srli_epi16(_mm_add_epi16(lo, _mm_srli_epi16(lo, 8)), 8);
        hi = _mm_srli_epi16(_mm_add_epi16(hi, _mm_srli_epi16(hi, 8)), 8);
        _mm_storeu_si128((__m128i*)(dst + i), _mm_packus_epi16(lo, hi));
    }
#endif
    for (; i < count; i++)
        dst[i] = Blend(dst[i], xrgb, a);
}

// --- textures -----------------------------------------------------------------

inline Color Texel(const ImGui_ImplSoftRast_Texture& tex, int x, int y)
{
    x = x < 0 ? 0 : (x >= tex.Width ? tex.Width - 1 : x);
    y = y < 0 ? 0 : (y >= tex.Height ? tex.Height - 1 : y);
    if (tex.Format == ImTextureFormat_Alpha8)
        return { 255, 255, 255, tex.Pixels[y * tex.Width + x] };
    const unsigned char* p = tex.Pixels + ((size_t)y * tex.Width + x) * 4;
    switch (tex.Layout)
    {
    case ImGui_ImplSoftRast_Layout_BGRX:
        return { p[2], p[1], p[0], 255 };
    case ImGui_ImplSoftRast_Layout_BGRA_Premultiplied:
    {
        // Everything else here is straight alpha. A colour channel larger
        // than its alpha is not a premultiplied colour; clamp it.
        const uint32_t a = p[3];
        if (a == 0) return { 0, 0, 0, 0 };
        if (a == 255) return { p[2], p[1], p[0], 255 };
        auto unmultiply = [a](uint32_t c) { const uint32_t v = (c * 255 + a / 2) / a; return v > 255 ? 255u : v; };
        return { unmultiply(p[2]), unmultiply(p[1]), unmultiply(p[0]), a };
    }
    default:
        return { p[0], p[1], p[2], p[3] };
    }
}

// One row of a BGRA texture at one texel per pixel and no tint: what a
// Wayland client's window is, every frame. The target's own format is BGRX,
// so an opaque row is a copy; a premultiplied one is dst = src + dst * (1 - a).
void BlitBgraRow(uint32_t* dst, const uint32_t* src, int count, bool premultiplied)
{
    int i = 0;
    if (!premultiplied)
    {
#if defined(__SSE2__)
        const __m128i rgb = _mm_set1_epi32(0x00ffffff);
        for (; i + 4 <= count; i += 4)
            _mm_storeu_si128((__m128i*)(dst + i), _mm_and_si128(_mm_loadu_si128((const __m128i*)(src + i)), rgb));
#endif
        for (; i < count; i++)
            dst[i] = src[i] & 0x00ffffff;
        return;
    }
    for (; i < count; i++)
    {
        const uint32_t s = src[i], a = s >> 24;
        if (a == 255) { dst[i] = s & 0x00ffffff; continue; }
        if (s == 0) continue;
        const uint32_t d = dst[i], inv = 255 - a;
        uint32_t rb = (d & 0xff00ff) * inv + 0x800080;
        uint32_t g  = (d & 0x00ff00) * inv + 0x008000;
        rb = ((rb + ((rb >> 8) & 0xff00ff)) >> 8) & 0xff00ff;
        g  = ((g  + ((g  >> 8) & 0x00ff00)) >> 8) & 0x00ff00;
        // Saturating add: a client may send a channel brighter than its alpha.
        uint32_t r = ((s >> 16) & 0xff) + (rb >> 16), gg = ((s >> 8) & 0xff) + (g >> 8), b = (s & 0xff) + (rb & 0xff);
        dst[i] = ((r > 255 ? 255 : r) << 16) | ((gg > 255 ? 255 : gg) << 8) | (b > 255 ? 255 : b);
    }
}

// u and v in texels, with texel centres at .5 as everywhere in graphics.
Color SampleBilinear(const ImGui_ImplSoftRast_Texture& tex, float u, float v)
{
    u -= 0.5f; v -= 0.5f;
    const float fu = floorf(u), fv = floorf(v);
    const int x = (int)fu, y = (int)fv;
    const uint32_t wx = (uint32_t)((u - fu) * 256.0f), wy = (uint32_t)((v - fv) * 256.0f);
    const Color c00 = Texel(tex, x, y), c10 = Texel(tex, x + 1, y);
    const Color c01 = Texel(tex, x, y + 1), c11 = Texel(tex, x + 1, y + 1);
    auto mix = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        const uint32_t top = a * (256 - wx) + b * wx, bottom = c * (256 - wx) + d * wx;
        return (top * (256 - wy) + bottom * wy + 32768) >> 16;
    };
    return { mix(c00.r, c10.r, c01.r, c11.r), mix(c00.g, c10.g, c01.g, c11.g),
             mix(c00.b, c10.b, c01.b, c11.b), mix(c00.a, c10.a, c01.a, c11.a) };
}

inline Color Modulate(const Color& a, const Color& b)
{
    return { Div255(a.r * b.r), Div255(a.g * b.g), Div255(a.b * b.b), Div255(a.a * b.a) };
}

// --- the fast paths: axis-aligned quads ---------------------------------------

struct Clip { int x0, y0, x1, y1; };   // half open, already inside the target

// The pixels whose centres lie in [a, b).
inline int FirstPixel(float a) { return (int)ceilf(a - 0.5f); }

void DrawSolidRect(const ImGui_ImplSoftRast_Target& target, const Clip& clip, const ImVec2& p0, const ImVec2& p1,
                   const Color& color, ImGui_ImplSoftRast_Stats& stats)
{
    int x0 = FirstPixel(p0.x), x1 = FirstPixel(p1.x), y0 = FirstPixel(p0.y), y1 = FirstPixel(p1.y);
    if (x0 < clip.x0) x0 = clip.x0;
    if (y0 < clip.y0) y0 = clip.y0;
    if (x1 > clip.x1) x1 = clip.x1;
    if (y1 > clip.y1) y1 = clip.y1;
    if (x0 >= x1 || y0 >= y1 || color.a == 0)
        return;
    const uint32_t xrgb = PackXrgb(color);
    for (int y = y0; y < y1; y++)
        FillSpan((uint32_t*)((char*)target.Pixels + (size_t)y * target.Pitch) + x0, x1 - x0, xrgb, color.a);
    stats.PixelsFilled += (int64_t)(x1 - x0) * (y1 - y0);
}

// A quad whose texture coordinates run along the axes: p0/uv0 is the top-left
// corner, p1/uv1 the bottom-right. `uv` are in texels.
void DrawTexturedRect(const ImGui_ImplSoftRast_Target& target, const Clip& clip, const ImVec2& p0, const ImVec2& p1,
                      const ImVec2& uv0, const ImVec2& uv1, const Color& color,
                      const ImGui_ImplSoftRast_Texture& tex, ImGui_ImplSoftRast_Stats& stats)
{
    int x0 = FirstPixel(p0.x), x1 = FirstPixel(p1.x), y0 = FirstPixel(p0.y), y1 = FirstPixel(p1.y);
    if (x0 < clip.x0) x0 = clip.x0;
    if (y0 < clip.y0) y0 = clip.y0;
    if (x1 > clip.x1) x1 = clip.x1;
    if (y1 > clip.y1) y1 = clip.y1;
    if (x0 >= x1 || y0 >= y1 || color.a == 0)
        return;

    const float du = (uv1.x - uv0.x) / (p1.x - p0.x), dv = (uv1.y - uv0.y) / (p1.y - p0.y);
    const float u_start = uv0.x + ((float)x0 + 0.5f - p0.x) * du;
    const float v_start = uv0.y + ((float)y0 + 0.5f - p0.y) * dv;
    stats.PixelsTextured += (int64_t)(x1 - x0) * (y1 - y0);

    // One texel per pixel: text at its native size. Nearest sampling is then
    // exact, and for the font atlas each pixel is a single byte of coverage.
    const bool one_to_one = fabsf(du - 1.0f) < 1e-4f && fabsf(dv - 1.0f) < 1e-4f;
    const int tx0 = (int)floorf(u_start), ty0 = (int)floorf(v_start);
    if (one_to_one && tx0 >= 0 && ty0 >= 0 && tx0 + (x1 - x0) <= tex.Width && ty0 + (y1 - y0) <= tex.Height)
    {
        const uint32_t xrgb = PackXrgb(color);
        for (int y = y0; y < y1; y++)
        {
            uint32_t* dst = (uint32_t*)((char*)target.Pixels + (size_t)y * target.Pitch) + x0;
            if (tex.Format == ImTextureFormat_Alpha8)
            {
                const unsigned char* src = tex.Pixels + (size_t)(ty0 + y - y0) * tex.Width + tx0;
                for (int i = 0; i < x1 - x0; i++)
                {
                    const uint32_t coverage = src[i];
                    if (coverage == 0)
                        continue;
                    const uint32_t a = color.a == 255 ? coverage : Div255(coverage * color.a);
                    dst[i] = a == 255 ? xrgb : Blend(dst[i], xrgb, a);
                }
            }
            else if (tex.Layout != ImGui_ImplSoftRast_Layout_RGBA && color.r == 255 && color.g == 255 && color.b == 255 && color.a == 255)
            {
                const auto* src = (const uint32_t*)tex.Pixels + (size_t)(ty0 + y - y0) * tex.Width + tx0;
                BlitBgraRow(dst, src, x1 - x0, tex.Layout == ImGui_ImplSoftRast_Layout_BGRA_Premultiplied);
            }
            else
            {
                for (int i = 0; i < x1 - x0; i++)
                {
                    const Color texel = Texel(tex, tx0 + i, ty0 + y - y0);
                    if (texel.a != 0)
                        PutPixel(dst + i, Modulate(texel, color));
                }
            }
        }
        return;
    }

    for (int y = y0; y < y1; y++)
    {
        uint32_t* dst = (uint32_t*)((char*)target.Pixels + (size_t)y * target.Pitch) + x0;
        const float v = v_start + (float)(y - y0) * dv;
        for (int i = 0; i < x1 - x0; i++)
            PutPixel(dst + i, Modulate(SampleBilinear(tex, u_start + (float)i * du, v), color));
    }
}

// --- the general path: one triangle -------------------------------------------

constexpr int kSubpixelBits = 4;
constexpr int kSubpixel = 1 << kSubpixelBits;

struct Point { int64_t x, y; };

inline int64_t Orient(const Point& a, const Point& b, const Point& c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// With y pointing down and the vertices in the order that makes Orient()
// positive, a top edge runs left to right and a left edge runs upwards. A
// pixel centre exactly on one of those belongs to the triangle; on any other
// edge it belongs to the neighbour.
inline bool IsTopLeft(const Point& a, const Point& b)
{
    return (a.y == b.y && b.x > a.x) || b.y < a.y;
}

void DrawTriangle(const ImGui_ImplSoftRast_Target& target, const Clip& clip, const ImDrawVert* v0, const ImDrawVert* v1,
                  const ImDrawVert* v2, const ImVec2& offset, const ImGui_ImplSoftRast_Texture& tex,
                  ImGui_ImplSoftRast_Stats& stats)
{
    auto snap = [&](const ImDrawVert* v) {
        return Point{ (int64_t)lroundf((v->pos.x - offset.x) * kSubpixel), (int64_t)lroundf((v->pos.y - offset.y) * kSubpixel) };
    };
    Point a = snap(v0), b = snap(v1), c = snap(v2);
    int64_t area = Orient(a, b, c);
    if (area == 0)
        return;
    if (area < 0)   // Dear ImGui does not cull; give both windings the same orientation.
    {
        const ImDrawVert* tv = v1; v1 = v2; v2 = tv;
        const Point tp = b; b = c; c = tp;
        area = -area;
    }

    auto min3 = [](int64_t p, int64_t q, int64_t r) { return p < q ? (p < r ? p : r) : (q < r ? q : r); };
    auto max3 = [](int64_t p, int64_t q, int64_t r) { return p > q ? (p > r ? p : r) : (q > r ? q : r); };
    // Pixel (x, y) is sampled at its centre, x * 16 + 8 in subpixels.
    int x0 = (int)((min3(a.x, b.x, c.x) - kSubpixel / 2 + kSubpixel - 1) >> kSubpixelBits);
    int y0 = (int)((min3(a.y, b.y, c.y) - kSubpixel / 2 + kSubpixel - 1) >> kSubpixelBits);
    int x1 = (int)((max3(a.x, b.x, c.x) - kSubpixel / 2) >> kSubpixelBits) + 1;
    int y1 = (int)((max3(a.y, b.y, c.y) - kSubpixel / 2) >> kSubpixelBits) + 1;
    if (x0 < clip.x0) x0 = clip.x0;
    if (y0 < clip.y0) y0 = clip.y0;
    if (x1 > clip.x1) x1 = clip.x1;
    if (y1 > clip.y1) y1 = clip.y1;
    if (x0 >= x1 || y0 >= y1)
        return;

    // w0 belongs to the edge opposite v0, and so on: w_i / area is v_i's weight.
    const int64_t bias0 = IsTopLeft(b, c) ? 0 : -1, bias1 = IsTopLeft(c, a) ? 0 : -1, bias2 = IsTopLeft(a, b) ? 0 : -1;
    const Point start{ (int64_t)x0 * kSubpixel + kSubpixel / 2, (int64_t)y0 * kSubpixel + kSubpixel / 2 };
    int64_t row0 = Orient(b, c, start) + bias0, row1 = Orient(c, a, start) + bias1, row2 = Orient(a, b, start) + bias2;
    const int64_t dx0 = -(c.y - b.y) * kSubpixel, dx1 = -(a.y - c.y) * kSubpixel, dx2 = -(b.y - a.y) * kSubpixel;
    const int64_t dy0 = (c.x - b.x) * kSubpixel, dy1 = (a.x - c.x) * kSubpixel, dy2 = (b.x - a.x) * kSubpixel;

    const bool flat_color = v0->col == v1->col && v0->col == v2->col;
    const bool flat_uv = v0->uv.x == v1->uv.x && v0->uv.x == v2->uv.x && v0->uv.y == v1->uv.y && v0->uv.y == v2->uv.y;
    const Color c0 = Unpack(v0->col), c1 = Unpack(v1->col), c2 = Unpack(v2->col);
    const float tw = (float)tex.Width, th = (float)tex.Height;
    // With one texture coordinate for the whole triangle there is one texel:
    // in practice the atlas' white pixel, under a shape of plain colour.
    const Color texel = flat_uv ? Texel(tex, (int)(v0->uv.x * tw), (int)(v0->uv.y * th)) : Color{};
    const float inv_area = 1.0f / (float)area;

    // The pixels a row covers are one run, since a triangle is convex, and
    // where it starts and ends follows from the edge functions without
    // visiting the pixels outside it: w(k) = w(0) + k * dx >= 0 bounds k from
    // below for an edge rising to the right and from above for one falling.
    // Exactly the pixels a per-pixel test would accept, at a fraction of the
    // work for the large triangles a rounded window's background is made of.
    const int width = x1 - x0;
    auto span = [&](int64_t w0, int64_t w1, int64_t w2, int& first, int& last) {
        int64_t lo = 0, hi = width - 1;
        const int64_t w[3] = { w0, w1, w2 }, d[3] = { dx0, dx1, dx2 };
        for (int i = 0; i < 3; i++)
        {
            if (d[i] > 0) { if (w[i] < 0) { const int64_t k = (-w[i] + d[i] - 1) / d[i]; if (k > lo) lo = k; } }
            else if (d[i] < 0) { if (w[i] < 0) return false; const int64_t k = w[i] / -d[i]; if (k < hi) hi = k; }
            else if (w[i] < 0) return false;
        }
        first = (int)lo;
        last = (int)hi;
        return lo <= hi;
    };

    if (flat_color && flat_uv)
    {
        const Color color = Modulate(c0, texel);
        if (color.a == 0)
            return;
        const uint32_t xrgb = PackXrgb(color);
        for (int y = y0; y < y1; y++, row0 += dy0, row1 += dy1, row2 += dy2)
        {
            int first, last;
            if (!span(row0, row1, row2, first, last))
                continue;
            FillSpan((uint32_t*)((char*)target.Pixels + (size_t)y * target.Pitch) + x0 + first, last - first + 1, xrgb, color.a);
            stats.PixelsShaded += last - first + 1;
        }
        return;
    }

    for (int y = y0; y < y1; y++, row0 += dy0, row1 += dy1, row2 += dy2)
    {
        int first, last;
        if (!span(row0, row1, row2, first, last))
            continue;
        uint32_t* dst = (uint32_t*)((char*)target.Pixels + (size_t)y * target.Pitch);
        int64_t w0 = row0 + first * dx0, w1 = row1 + first * dx1;
        for (int x = x0 + first; x <= x0 + last; x++, w0 += dx0, w1 += dx1)
        {
            const float l0 = (float)(w0 - bias0) * inv_area, l1 = (float)(w1 - bias1) * inv_area, l2 = 1.0f - l0 - l1;
            Color color = c0;
            if (!flat_color)
                color = { (uint32_t)((float)c0.r * l0 + (float)c1.r * l1 + (float)c2.r * l2 + 0.5f),
                          (uint32_t)((float)c0.g * l0 + (float)c1.g * l1 + (float)c2.g * l2 + 0.5f),
                          (uint32_t)((float)c0.b * l0 + (float)c1.b * l1 + (float)c2.b * l2 + 0.5f),
                          (uint32_t)((float)c0.a * l0 + (float)c1.a * l1 + (float)c2.a * l2 + 0.5f) };
            const Color sample = flat_uv ? texel
                : SampleBilinear(tex, (v0->uv.x * l0 + v1->uv.x * l1 + v2->uv.x * l2) * tw,
                                      (v0->uv.y * l0 + v1->uv.y * l1 + v2->uv.y * l2) * th);
            PutPixel(dst + x, Modulate(color, sample));
        }
        stats.PixelsShaded += last - first + 1;
    }
}

// Is the quad (a, b, c, d) -- as ImDrawList::PrimRect and PrimRectUV emit it,
// clockwise from the top-left -- an axis-aligned rectangle of one colour,
// with texture coordinates that are either all one point or axis-aligned too?
inline bool IsAxisAlignedQuad(const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c, const ImDrawVert& d)
{
    return a.col == b.col && a.col == c.col && a.col == d.col &&
           a.pos.y == b.pos.y && b.pos.x == c.pos.x && c.pos.y == d.pos.y && d.pos.x == a.pos.x &&
           a.pos.x < b.pos.x && a.pos.y < d.pos.y &&
           a.uv.y == b.uv.y && b.uv.x == c.uv.x && c.uv.y == d.uv.y && d.uv.x == a.uv.x;
}

} // namespace

// --- public API ------------------------------------------------------------------

void ImGui_ImplSoftRast_Clear(const ImGui_ImplSoftRast_Target& target, ImU32 col)
{
    const uint32_t xrgb = PackXrgb(Unpack(col));
    for (int y = 0; y < target.Height; y++)
        FillSpan((uint32_t*)((char*)target.Pixels + (size_t)y * target.Pitch), target.Width, xrgb, 255);
}

void ImGui_ImplSoftRast_UpdateTexture(ImTextureData* tex)
{
    // Nothing is uploaded anywhere: the rasterizer samples Dear ImGui's own
    // copy of the pixels. All there is to keep is a description of it.
    if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates)
    {
        auto* texture = (ImGui_ImplSoftRast_Texture*)tex->BackendUserData;
        if (texture == nullptr)
        {
            texture = IM_NEW(ImGui_ImplSoftRast_Texture)();
            tex->BackendUserData = texture;
        }
        texture->Width = tex->Width;
        texture->Height = tex->Height;
        texture->Format = tex->Format;
        texture->Pixels = (const unsigned char*)tex->GetPixels();
        tex->SetTexID((ImTextureID)(intptr_t)texture);
        tex->SetStatus(ImTextureStatus_OK);
    }
    else if (tex->Status == ImTextureStatus_WantDestroy)
    {
        if (auto* texture = (ImGui_ImplSoftRast_Texture*)tex->BackendUserData)
            IM_DELETE(texture);
        tex->BackendUserData = nullptr;
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

void ImGui_ImplSoftRast_RenderDrawData(ImDrawData* draw_data, const ImGui_ImplSoftRast_Target& target)
{
    ImGui_ImplSoftRast_Data* bd = GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplSoftRast_Init()?");

    if (draw_data->Textures != nullptr)
        for (ImTextureData* tex : *draw_data->Textures)
            if (tex->Status != ImTextureStatus_OK)
                ImGui_ImplSoftRast_UpdateTexture(tex);

    ImGui_ImplSoftRast_Stats& stats = bd->Stats;
    stats = ImGui_ImplSoftRast_Stats();
    if (target.Pixels == nullptr || target.Width <= 0 || target.Height <= 0)
        return;

    // Draw coordinates are relative to DisplayPos, and one unit is one pixel:
    // this backend has no notion of a scaled framebuffer.
    const ImVec2 offset = draw_data->DisplayPos;

    for (const ImDrawList* draw_list : draw_data->CmdLists)
    {
        const ImDrawVert* vertices = draw_list->VtxBuffer.Data;
        const ImDrawIdx* indices = draw_list->IdxBuffer.Data;

        for (const ImDrawCmd& cmd : draw_list->CmdBuffer)
        {
            if (cmd.UserCallback != nullptr)
            {
                if (cmd.UserCallback != DrawCallback_ResetRenderState)
                    cmd.UserCallback(draw_list, &cmd);
                continue;
            }

            Clip clip{ (int)(cmd.ClipRect.x - offset.x), (int)(cmd.ClipRect.y - offset.y),
                       (int)(cmd.ClipRect.z - offset.x), (int)(cmd.ClipRect.w - offset.y) };
            if (clip.x0 < 0) clip.x0 = 0;
            if (clip.y0 < 0) clip.y0 = 0;
            if (clip.x1 > target.Width) clip.x1 = target.Width;
            if (clip.y1 > target.Height) clip.y1 = target.Height;
            if (clip.x0 >= clip.x1 || clip.y0 >= clip.y1)
                continue;

            const auto* tex = (const ImGui_ImplSoftRast_Texture*)(intptr_t)cmd.GetTexID();
            if (tex == nullptr || tex->Pixels == nullptr || tex->Width <= 0 || tex->Height <= 0)
                continue;
            const float tw = (float)tex->Width, th = (float)tex->Height;

            const ImDrawVert* vtx = vertices + cmd.VtxOffset;
            const ImDrawIdx* idx = indices + cmd.IdxOffset;
            for (unsigned int i = 0; i + 3 <= cmd.ElemCount; )
            {
                // Two triangles (a, b, c) and (a, c, d) over four vertices?
                if (i + 6 <= cmd.ElemCount && idx[i] == idx[i + 3] && idx[i + 2] == idx[i + 4])
                {
                    const ImDrawVert& a = vtx[idx[i]], &b = vtx[idx[i + 1]], &c = vtx[idx[i + 2]], &d = vtx[idx[i + 5]];
                    if (IsAxisAlignedQuad(a, b, c, d))
                    {
                        const ImVec2 p0(a.pos.x - offset.x, a.pos.y - offset.y), p1(c.pos.x - offset.x, c.pos.y - offset.y);
                        const Color color = Unpack(a.col);
                        if (a.uv.x == c.uv.x && a.uv.y == c.uv.y)
                            DrawSolidRect(target, clip, p0, p1, Modulate(color, Texel(*tex, (int)(a.uv.x * tw), (int)(a.uv.y * th))), stats);
                        else
                            DrawTexturedRect(target, clip, p0, p1, ImVec2(a.uv.x * tw, a.uv.y * th),
                                             ImVec2(c.uv.x * tw, c.uv.y * th), color, *tex, stats);
                        stats.Rects++;
                        i += 6;
                        continue;
                    }
                }
                DrawTriangle(target, clip, &vtx[idx[i]], &vtx[idx[i + 1]], &vtx[idx[i + 2]], offset, *tex, stats);
                stats.Triangles++;
                i += 3;
            }
        }
    }
}

bool ImGui_ImplSoftRast_Init()
{
    ImGuiIO& io = ImGui::GetIO();
    IMGUI_CHECKVERSION();
    IM_ASSERT(io.BackendRendererUserData == nullptr && "Already initialized a renderer backend!");

    ImGui_ImplSoftRast_Data* bd = IM_NEW(ImGui_ImplSoftRast_Data)();
    io.BackendRendererUserData = (void*)bd;
    io.BackendRendererName = "imgui_impl_softrast";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;  // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;   // We can honor ImGuiPlatformIO::Textures[] requests during render.

    ImGui::GetPlatformIO().DrawCallback_ResetRenderState = DrawCallback_ResetRenderState;
    return true;
}

void ImGui_ImplSoftRast_Shutdown()
{
    ImGui_ImplSoftRast_Data* bd = GetBackendData();
    IM_ASSERT(bd != nullptr && "No renderer backend to shutdown, or already shutdown?");
    ImGuiIO& io = ImGui::GetIO();
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();

    for (ImTextureData* tex : platform_io.Textures)
        if (tex->RefCount == 1)
        {
            tex->SetStatus(ImTextureStatus_WantDestroy);
            ImGui_ImplSoftRast_UpdateTexture(tex);
        }

    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    platform_io.ClearRendererHandlers();
    IM_DELETE(bd);
}

void ImGui_ImplSoftRast_NewFrame()
{
    ImGui_ImplSoftRast_Data* bd = GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplSoftRast_Init()?");
    IM_UNUSED(bd);
}

const ImGui_ImplSoftRast_Stats& ImGui_ImplSoftRast_GetStats()
{
    ImGui_ImplSoftRast_Data* bd = GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplSoftRast_Init()?");
    return bd->Stats;
}

#endif // #ifndef IMGUI_DISABLE
