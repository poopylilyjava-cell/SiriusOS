# shellcheck shell=bash
# Locate the OVMF UEFI firmware. Sourced by run-qemu.sh and screenshot.sh.
#
# Distributions install it in different places, and CODE and VARS must come
# from the same build (they differ in size between the 2 MiB and 4 MiB
# variants), so candidates are tried as pairs. OVMF_CODE / OVMF_VARS in the
# environment override the search.

find_ovmf() {
    if [ -n "${OVMF_CODE:-}" ] && [ -n "${OVMF_VARS:-}" ]; then
        [ -f "$OVMF_CODE" ] && [ -f "$OVMF_VARS" ] && return 0
        echo "error: OVMF_CODE/OVMF_VARS set but not found: $OVMF_CODE $OVMF_VARS" >&2
        return 1
    fi

    local pair code vars
    for pair in \
        "/usr/share/edk2/x64/OVMF_CODE.4m.fd:/usr/share/edk2/x64/OVMF_VARS.4m.fd" \
        "/usr/share/OVMF/OVMF_CODE_4M.fd:/usr/share/OVMF/OVMF_VARS_4M.fd" \
        "/usr/share/edk2/ovmf/OVMF_CODE.fd:/usr/share/edk2/ovmf/OVMF_VARS.fd" \
        "/usr/share/OVMF/OVMF_CODE.fd:/usr/share/OVMF/OVMF_VARS.fd"; do
        code="${pair%%:*}"; vars="${pair#*:}"
        if [ -f "$code" ] && [ -f "$vars" ]; then
            OVMF_CODE="$code"; OVMF_VARS="$vars"
            return 0
        fi
    done

    echo "error: OVMF not found (Arch: pacman -S edk2-ovmf, Debian/Ubuntu: apt install ovmf)," >&2
    echo "       or set OVMF_CODE and OVMF_VARS" >&2
    return 1
}
