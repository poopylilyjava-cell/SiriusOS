# ADR 0009 — The desktop draws itself: a mapped framebuffer, evdev input, a software rasterizer

**Status:** accepted · 2026-10-10 · **milestone:** M5

## Context
M5 puts Dear ImGui on the screen: `siriusd` runs `ImGui::ShowDemoWindow()` on
the real framebuffer, with the mouse and the keyboard, at 1920×1080. ADR 0003
already fixed the shape of the port -- two backend files, portable to a Linux
host. This record is about what the kernel had to provide, and how the
rasterizer is built.

## Decisions

### The kernel exposes the display as memory, and nothing more
`/dev/fb0` answers one `ioctl` (the mode: size, pitch, channel layout) and can
be mapped with `mmap(MAP_SHARED)`. There is no drawing interface in the
kernel, no window system, no console on the framebuffer: whoever maps it owns
every pixel (architectural commitment 2 of the plan).

A device mapping is a new kind of page. It is marked with the last free
software bit of a page table entry; `fork` shares it instead of making it
copy-on-write, and unmapping or exiting forgets it instead of freeing it. A
device describes its memory one page at a time (`Vnode::physical_page`), so
`mmap` can refuse a bad range before it has mapped anything.

*Rejected:* `write()` to a framebuffer device. A system call per rectangle,
and a copy through the kernel, for the hottest path in the system.

### Input is one stream of evdev records
`/dev/input` delivers `struct sirius_input_event`: a timestamp and Linux's
`type`/`code`/`value`, with Linux's numbers for keys, buttons and axes.
Drivers post into one queue; the reader gets keys, mouse motion and buttons
interleaved in the order they happened.

Key codes name keys, not characters. The keyboard layout lives in user space
(for now a US table in the platform backend), where it can be changed without
a kernel. And Wayland's `wl_keyboard` and `wl_pointer` carry exactly these
codes, so M8 forwards them untranslated.

The device honours `O_NONBLOCK`; a blocking read sleeps on a wait queue. A
full queue drops its oldest events.

*Rejected:* a device per source (`/dev/kbd`, `/dev/mouse`). The desktop would
need `poll` to read two at once, and M6 has not built it yet.

### Device interrupts come through the I/O APIC, as ACPI describes it
The kernel reads the MADT -- nothing else of ACPI, and no AML -- for the I/O
APIC's address and the interrupt source overrides, and routes ISA IRQs through
it. Without a usable table it assumes the PC's defaults. The legacy PICs stay
masked. Drivers register a handler per vector; the handler runs with
interrupts off and must not block.

PS/2 is the first interrupt-driven driver (AHCI still polls). The keyboard and
mouse share the controller's one output buffer, so one handler drains it for
both. The byte-stream decoders are pure state machines, tested on the host
against real and corrupted sequences, as is the MADT parser.

### Time comes from the TSC
`clock_get` now reads the time stamp counter, calibrated against the PIT in
the same 50 ms that calibrate the APIC timer: nanoseconds instead of 10 ms
ticks. Frame times and input timestamps need it, and it exposed that
`sleep` could return a tick early; it now always sleeps at least as long as
asked.

### The rasterizer is built around what Dear ImGui actually draws
Dear ImGui's output is triangles, but nearly all of it is axis-aligned quads.
`imgui_impl_softrast` recognises them in the index stream and never sends
them to the triangle rasterizer:

- one colour over the atlas' white pixel: a rectangle fill, a row at a time
  with SSE2 -- window backgrounds, frames, the fill rate of the desktop;
- a texture at one texel per pixel: a glyph, copied with its coverage;
- everything else -- rounded corners, anti-aliased edges, gradients, scaled
  images -- is a real triangle: integer edge functions, the top-left fill
  rule (shared edges are drawn exactly once, which alpha blending makes
  visible if wrong), bilinear sampling.

The font atlas is requested as one byte of coverage per texel, and never
copied: the backend supports Dear ImGui 1.92's dynamic textures by pointing at
its pixel buffer. Anti-aliased lines are drawn as geometry, not from the baked
line texture, which only looks right under a GPU's filter.

Each frame is drawn into a back buffer in ordinary memory and copied to the
framebuffer with non-temporal stores. Blending reads what is underneath, and
the framebuffer is write-combining memory: reading it is slower than drawing
the frame.

The whole screen is redrawn every frame. At 1920×1080 a frame of the demo
costs about 0.5 ms on the development machine under KVM (≈2000 fps) and 10 ms
under QEMU's instruction emulation (≈100 fps), so damage tracking -- the first
mitigation listed in the plan's risk table -- has not been needed.

### Developed on the host, as planned
`tests/host_harness` runs the same two backends and the same desktop code in
an SDL window, or headless into a `.ppm`; SDL events are translated into the
kernel's event records, so the platform backend sees on Linux exactly what it
sees on Sirius. The unit tests check the rasterizer pixel by pixel. The first
boot on the OS drew the same image as the host; what needed debugging on the
OS was the PS/2 controller.

## Consequences
- `siriusd` redraws continuously and takes a whole CPU. There is no vertical
  sync to wait for and, without `poll`, no way to sleep until input arrives.
  M6's `poll` and an idle check fix that; M7's shell should draw on demand.
- Text is typed with a US layout, whatever is printed on the keys.
- The pointer is drawn by Dear ImGui into the frame: it moves at the frame
  rate, and a stalled desktop means a stalled pointer.
- Only PS/2 input exists. QEMU and older machines have it; the reference
  laptop needs USB HID (M9), which will post into the same queue.
- A process that maps the framebuffer keeps it: there is no handover, no
  virtual terminals, and a second mapper simply draws over the first.
- All device interrupts go to the CPU that initialised the driver.
- The `clock_get` time base assumes the TSC runs at one rate on all CPUs.
