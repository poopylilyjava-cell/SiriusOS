# Convenience wrapper. The real build is CMake+Ninja; this exists so the common
# loop is one word long.

SHELL     := /bin/bash
ROOT      := $(CURDIR)
BUILD     := $(ROOT)/build
KERNEL    := $(BUILD)/kernel/sirius.elf
IMAGE     := $(BUILD)/sirius.img
JOBS      ?= $(shell nproc)
CROSS     := $(ROOT)/toolchain/local/bin/x86_64-elf-gcc
SIRIUS_CC := $(ROOT)/toolchain/local/bin/x86_64-sirius-g++
USERLAND  := $(BUILD)/userland
DESKTOP_ENTRY := 9
WAYLAND_ENTRY := 10
LIVE_ENTRY := 11

.PHONY: all help deps submodules toolchain toolchain-sirius libc configure kernel \
        userland image usb run run-bios debug gdb test test-configure harness check-desktop check-compositor check-wayland \
        clean distclean

all: image

help:
	@echo "Sirius OS"
	@echo "  make deps        host packages needed (prints the pacman command)"
	@echo "  make submodules  fetch limine + imgui"
	@echo "  make toolchain   build the x86_64-elf cross compiler (slow, once)"
	@echo "  make toolchain-sirius  build x86_64-sirius gcc, mlibc and libstdc++ (slow, once)"
	@echo "  make libc        rebuild mlibc after changing its sysdeps or the syscall ABI"
	@echo "  make kernel      build the kernel"
	@echo "  make userland    build the mlibc programs (needs toolchain-sirius)"
	@echo "  make image       build the bootable disk image"
	@echo "  make usb         build build/sirius-usb.img, for ./tools/write-usb.sh /dev/sdX"
	@echo "  make run         boot the desktop in QEMU (UEFI); ENTRY=1 boots the tests instead"
	@echo "  make run-bios    boot it in QEMU (legacy BIOS)"
	@echo "  make debug       boot halted with a gdb stub; then 'make gdb'"
	@echo "  make test        build and run host-side unit tests"
	@echo "  make harness     run the desktop in an SDL window on this machine"
	@echo "  make check-desktop  boot the desktop and test it through QEMU's monitor"
	@echo "  make check-compositor  the same, with Wayland clients (after ./tools/ports.sh add ... weston-clients)"
	@echo "  make check-wayland  run libwayland's test suite on Sirius (after ./tools/ports.sh add libffi wayland)"
	@echo "  make clean       remove build output"
	@echo "  make distclean   also remove both toolchains, the sysroot and the ports cache"
	@echo "Third-party ports are built by ./tools/ports.sh (see ports/README.md)."

deps:
	@echo "sudo pacman -S --needed nasm cmake ninja xorriso meson mtools parted gdb edk2-ovmf rsync patch git wget zstd sdl2 imagemagick"

submodules:
	git submodule update --init --recursive

$(CROSS):
	./toolchain/build-cross.sh -j $(JOBS)

toolchain: $(CROSS)
	@echo "toolchain ready: $(CROSS)"

$(SIRIUS_CC):
	./toolchain/build-sirius.sh -j $(JOBS)

toolchain-sirius: $(SIRIUS_CC)
	@echo "toolchain ready: $(SIRIUS_CC)"

libc:
	./toolchain/build-sirius.sh -j $(JOBS) --libc

configure: $(CROSS)
	cmake -B $(BUILD) -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-elf.cmake
	@ln -sf $(BUILD)/compile_commands.json $(ROOT)/compile_commands.json

kernel: configure
	cmake --build $(BUILD) -j $(JOBS)

# The kernel does not need the system toolchain, and building that takes a
# while: without it the image is made from the kernel and the bring-up
# programs alone, and /bin/init skips what is missing.
userland:
	@if [ -x $(SIRIUS_CC) ]; then \
	    cmake -S user -B $(USERLAND) -G Ninja -DCMAKE_TOOLCHAIN_FILE=$(ROOT)/cmake/x86_64-sirius.cmake >/dev/null && \
	    cmake --build $(USERLAND) -j $(JOBS); \
	else \
	    echo "userland: skipped -- no x86_64-sirius toolchain (make toolchain-sirius)"; \
	fi

# ENTRY picks the boot menu entry (1-based, see boot/limine.conf); unset, the
# image boots the first one: /bin/init and the system's tests.
image: kernel userland
	DEFAULT_ENTRY=$(ENTRY) ./tools/mkimage.sh $(KERNEL) $(IMAGE)

# For a USB stick: the live entry (the root filesystem in memory), with three
# seconds to pick another. Written by ./tools/write-usb.sh.
usb: kernel userland
	DEFAULT_ENTRY=$(LIVE_ENTRY) MENU_TIMEOUT=3 ./tools/mkimage.sh $(KERNEL) $(BUILD)/sirius-usb.img

# `make run` is the desktop; `make run ENTRY=1` the tests.
run: ENTRY ?= $(DESKTOP_ENTRY)
run: image
	./tools/run-qemu.sh --kvm

run-bios: image
	./tools/run-qemu.sh --bios

debug: image
	./tools/run-qemu.sh --debug

gdb:
	./tools/gdb.sh $(KERNEL)

test-configure:
	cmake -S tests -B $(BUILD)/tests -G Ninja

test: test-configure
	cmake --build $(BUILD)/tests -j $(JOBS)
	cd $(BUILD)/tests && ctest --output-on-failure

# The desktop without the kernel: the same backends and drawing code, in a
# window. Where rendering and input bugs are meant to be found.
harness:
	cmake -S tests/host_harness -B $(BUILD)/host_harness -G Ninja >/dev/null
	cmake --build $(BUILD)/host_harness -j $(JOBS)
	$(BUILD)/host_harness/siriusd_host $(HARNESS_ARGS)

check-desktop: ENTRY := $(DESKTOP_ENTRY)
check-desktop: image
	./tools/check-desktop.sh

check-compositor: ENTRY := $(DESKTOP_ENTRY)
check-compositor: image
	./tools/check-compositor.sh

check-wayland: ENTRY := $(WAYLAND_ENTRY)
check-wayland: image
	./tools/check-wayland.sh

clean:
	rm -rf $(BUILD) $(ROOT)/compile_commands.json

distclean: clean
	rm -rf $(ROOT)/toolchain/local $(ROOT)/toolchain/build $(ROOT)/toolchain/src $(ROOT)/sysroot
	rm -rf $(ROOT)/ports/.staging $(ROOT)/ports/.jinx-cache $(ROOT)/ports/sources $(ROOT)/ports/host-sources
