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

echo "==> lb clean"
lb clean --purge

echo "==> lb config"
lb config

echo "==> lb build"
lb build

echo "==> done"
ls -lh *.iso 2>/dev/null || echo "warning: no .iso found in $(pwd) -- check the build log above for errors."
