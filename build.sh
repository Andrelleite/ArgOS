#!/bin/bash
set -euo pipefail

# Build the ArgOS ISO with live-build.
#
# Run this on a real Debian/Kali host as root, or inside the provided
# Docker image with --privileged (live-build needs loopback mounts, chroot,
# and bind mounts that an unprivileged container can't do).
#
# Requirements: root, network access to http.kali.org, and roughly 20-30GB
# of free disk space. A full build typically takes 20-60 minutes depending
# on mirror speed and host CPU.
#
#   On bare metal / a VM:
#     sudo ./build.sh
#
#   Via Docker:
#     docker build -t argos-os-builder .
#     docker run --rm --privileged -v "$PWD:/build" argos-os-builder -c "cd /build && ./build.sh"
#
# Runs scripts/argos-preflight.sh first (checks every package list
# against the live archive before committing 20-60 minutes to a build
# that might fail partway through on a renamed/missing package -- see
# that script's own comments for why this exists). Pass --skip-preflight
# to skip it (e.g. no network access to the mirror from this check, but
# you're confident the lists are current).

SKIP_PREFLIGHT=0
for arg in "$@"; do
    if [ "$arg" = "--skip-preflight" ]; then
        SKIP_PREFLIGHT=1
    fi
done

if [ "$(id -u)" -ne 0 ]; then
    echo "error: live-build requires root (chroot/mount/loopback access)." >&2
    echo "Re-run as: sudo ./build.sh" >&2
    exit 1
fi

if ! command -v lb >/dev/null 2>&1; then
    echo "error: live-build (the 'lb' command) is not installed." >&2
    echo "Install it with: apt-get install live-build kali-archive-keyring" >&2
    exit 1
fi

cd "$(dirname "$0")"

if [ "$SKIP_PREFLIGHT" -eq 0 ]; then
    echo "==> argos-preflight"
    if ! ./scripts/argos-preflight.sh; then
        echo "error: preflight found package(s) that don't currently resolve -- see above." >&2
        echo "Fix the package lists, or re-run with --skip-preflight to build anyway." >&2
        exit 1
    fi
else
    echo "==> argos-preflight (skipped)"
fi

echo "==> lb clean"
lb clean --purge

echo "==> lb config"
lb config

echo "==> lb build"
lb build

echo "==> done"
ls -lh *.iso 2>/dev/null || echo "warning: no .iso found in $(pwd) -- check the build log above for errors."
