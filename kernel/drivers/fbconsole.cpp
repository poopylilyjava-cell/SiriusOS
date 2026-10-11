#include <drivers/fbconsole.hpp>
#include <drivers/framebuffer.hpp>
#include <klib/spinlock.hpp>
#include <klib/string.hpp>

namespace fbconsole {
namespace {

#include "console_font.inc"

constexpr u32 kCellWidth = 8, kCellHeight = 16;
constexpr u32 kMaxColumns = 320, kMaxRows = 120;

enum class Mode : u8 { Off, Log, Panic };

Spinlock g_lock;
Mode g_mode = Mode::Off;
u32 g_scale = 1;                // pixels per font pixel: 2 on large displays
u32 g_columns = 0, g_rows = 0;
u32 g_column = 0, g_row = 0;
char g_text[kMaxRows][kMaxColumns];
u32 g_foreground = 0, g_background = 0;

void draw_cell(u32 column, u32 row) {
    const u8 c = static_cast<u8>(g_text[row][column]);
    const u8* glyph = kConsoleFont[(c >= 32 && c < 128) ? c - 32 : 0];
    const u64 x0 = static_cast<u64>(column) * kCellWidth * g_scale, y0 = static_cast<u64>(row) * kCellHeight * g_scale;
    for (u32 y = 0; y < kCellHeight * g_scale; y++) {
        const u8 bits = glyph[y / g_scale];
        for (u32 x = 0; x < kCellWidth * g_scale; x++)
            g_framebuffer.put_pixel(x0 + x, y0 + y, (bits & (0x80 >> (x / g_scale))) ? g_foreground : g_background);
    }
}

void redraw() {
    g_framebuffer.clear(g_background);
    for (u32 row = 0; row < g_rows; row++)
        for (u32 column = 0; column < g_columns; column++)
            if (g_text[row][column] != ' ') draw_cell(column, row);
}

// Out of rows: keep the newer half, and carry on below it. Redrawing the
// screen once per half screen of text, not once per line.
void scroll() {
    const u32 keep = g_rows / 2;
    for (u32 row = 0; row < keep; row++) memcpy(g_text[row], g_text[g_rows - keep + row], kMaxColumns);
    for (u32 row = keep; row < g_rows; row++) memset(g_text[row], ' ', kMaxColumns);
    g_row = keep;
    redraw();
}

void newline() {
    g_column = 0;
    if (++g_row >= g_rows) scroll();
}

void put(char c) {
    if (c == '\n') { newline(); return; }
    if (c == '\r') { g_column = 0; return; }
    if (c == '\t') { do put(' '); while (g_column % 8); return; }
    if (g_column >= g_columns) newline();
    g_text[g_row][g_column] = c;
    draw_cell(g_column, g_row);
    g_column++;
}

void reset(u32 foreground, u32 background) {
    g_foreground = foreground;
    g_background = background;
    for (auto& row : g_text) memset(row, ' ', kMaxColumns);
    g_column = g_row = 0;
    g_framebuffer.clear(g_background);
}

} // namespace

void init(bool mirror) {
    if (!g_framebuffer.valid()) return;
    g_scale = g_framebuffer.width() >= 2560 ? 2 : 1;
    g_columns = static_cast<u32>(g_framebuffer.width() / (kCellWidth * g_scale));
    g_rows = static_cast<u32>(g_framebuffer.height() / (kCellHeight * g_scale));
    if (g_columns > kMaxColumns) g_columns = kMaxColumns;
    if (g_rows > kMaxRows) g_rows = kMaxRows;
    if (mirror) {
        ScopedLock guard(g_lock);
        reset(g_framebuffer.pack(0xb0, 0xb8, 0xc8), g_framebuffer.pack(0, 0, 0));
        g_mode = Mode::Log;
    }
}

void write(const char* text) {
    if (g_mode == Mode::Off || g_columns == 0) return;
    if (g_mode == Mode::Panic) {   // one CPU left, and it may hold the lock it panicked under
        for (; *text; text++) put(*text);
        return;
    }
    ScopedLock guard(g_lock);
    if (g_mode != Mode::Log) return;
    for (; *text; text++) put(*text);
}

void release() {
    ScopedLock guard(g_lock);
    if (g_mode == Mode::Log) g_mode = Mode::Off;
}

void take_over() {
    if (!g_framebuffer.valid() || g_columns == 0) return;
    g_mode = Mode::Panic;
    reset(g_framebuffer.pack(0xff, 0xff, 0xff), g_framebuffer.pack(0x5a, 0x10, 0x14));
    write("Sirius has stopped.\n\n");
}

} // namespace fbconsole
