# ADR 0004 — Implement the Wayland server protocol directly; no wlroots

**Status:** accepted · 2026-09-16 · **milestone:** M8

## Context
The desktop should host real Wayland clients. On Linux, a compositor is built on
wlroots.

## Decision
Do not use wlroots. Implement the server side of the Wayland protocol inside
`siriusd`, generated from the upstream protocol XML.

## Alternatives rejected
- **Port wlroots** — it hard-depends on DRM/KMS, libinput, udev, seatd and
  libseat: Linux kernel subsystems, not portable libraries. Porting it means
  reimplementing those subsystems first. This is not a scheduling problem, it is
  an impossibility on the current plan.
- **Invent a native Sirius display protocol** — simpler, and it is in fact what
  M7 does in-process. But it means no existing application ever runs.

## Consequences
- The kernel must provide unix domain sockets **with `SCM_RIGHTS` fd passing**,
  anonymous shared memory, and `poll`/`epoll`. M6 exists solely to deliver this,
  and is scheduled before any compositor work.
- We implement only the subset a compositor serves: `wl_compositor`, `wl_surface`,
  `wl_shm`, `wl_seat`, `wl_output`, `xdg_shell`. Roughly 4k lines.
- Client surfaces arrive as shm buffers and become ImGui textures drawn with
  `ImDrawList::AddImage()`; ImGui owns the decorations and input routing.
- M7 ships a complete desktop with none of this, so an M8 stall is not fatal.
