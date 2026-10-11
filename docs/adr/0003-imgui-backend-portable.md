# ADR 0003 — Two hand-written ImGui backends, portable from day one

**Status:** accepted · 2026-09-16

## Context
Dear ImGui's shipped backends target GLFW/SDL/Win32 and OpenGL/Vulkan/DX. None
of those exist on Sirius, and none will for years.

## Decision
Write exactly two files — `imgui_impl_sirius.cpp` (platform: input, time,
clipboard, cursor) and `imgui_impl_softrast.cpp` (renderer: a clipped, textured
triangle rasterizer over a linear framebuffer) — and keep both compiling against
a Linux/SDL host harness in `tests/host_harness/` as well as against the kernel.

## Alternatives rejected
- **Write a GPU driver first** — ImGui's vertex volume is tiny; the cost is fill
  rate, which a tuned SSE2 rasterizer handles at 1080p. A GPU driver is an
  optimization, and a large one.
- **Develop the backends only on Sirius** — every rasterizer bug would then be
  debugged through a QEMU serial port instead of a host debugger.

## Consequences
- The rasterizer is unit-testable on the host with golden-image comparison.
- These two files are the *entire* ImGui port; nothing else in the tree may
  depend on an ImGui backend detail.
- Performance work (damage tracking, virtio-gpu blits, eventually a GPU driver)
  slots in behind the renderer backend interface without touching the shell.
