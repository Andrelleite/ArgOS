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
#
# This script only dresses up its OWN section markers (colors, step
# counters, elapsed time, a final summary). It deliberately never
# filters or reformats live-build's own output -- that's real apt/dpkg
# progress, and the one place you'd actually spot a build-breaking
# error, so hiding or rewriting it would be the one way this script
# could actively work against you. The full raw log is also always
# saved to a timestamped file, color codes included, for scrolling back
# through afterward.

SKIP_PREFLIGHT=0
for arg in "$@"; do
    if [ "$arg" = "--skip-preflight" ]; then
        SKIP_PREFLIGHT=1
    fi
done

# Color only when writing to a real terminal that isn't opted out via
# NO_COLOR (https://no-color.org) -- stays plain text in logs/pipes.
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_BOLD=$'\033[1m'
    C_DIM=$'\033[2m'
    C_RED=$'\033[31m'
    C_GREEN=$'\033[32m'
    C_YELLOW=$'\033[33m'
    C_CYAN=$'\033[36m'
    C_RESET=$'\033[0m'
else
    C_BOLD=''; C_DIM=''; C_RED=''; C_GREEN=''; C_YELLOW=''; C_CYAN=''; C_RESET=''
fi

TOTAL_STEPS=4
STEP_NUM=0
BUILD_START=$(date +%s)
STAGE_START=$BUILD_START

human_duration() {
    local secs=$1
    if [ "$secs" -ge 60 ]; then
        printf '%dm %ds' $((secs / 60)) $((secs % 60))
    else
        printf '%ds' "$secs"
    fi
}

step() {
    STEP_NUM=$((STEP_NUM + 1))
    STAGE_START=$(date +%s)
    printf '\n%s┌─[%s%d/%d%s]%s %s%s%s\n' \
        "$C_CYAN" "$C_BOLD" "$STEP_NUM" "$TOTAL_STEPS" "$C_RESET$C_CYAN" "$C_RESET" \
        "$C_BOLD" "$1" "$C_RESET"
}

step_ok() {
    local elapsed=$(( $(date +%s) - STAGE_START ))
    printf '%s└─ ✓ %s %s(%s)%s\n' "$C_GREEN" "$1" "$C_DIM" "$(human_duration "$elapsed")" "$C_RESET"
}

step_skip() {
    printf '%s└─ – %s%s\n' "$C_YELLOW" "$1" "$C_RESET"
}

die() {
    printf '\n%s✗ %s%s\n' "$C_RED$C_BOLD" "$1" "$C_RESET" >&2
    exit 1
}

on_error() {
    local elapsed=$(( $(date +%s) - BUILD_START ))
    printf '\n%s┌─ ✗ build failed at step %d/%d, after %s%s\n' \
        "$C_RED$C_BOLD" "$STEP_NUM" "$TOTAL_STEPS" "$(human_duration "$elapsed")" "$C_RESET" >&2
    if [ -n "${LOG_FILE:-}" ]; then
        printf '%s└─ full log: %s%s\n' "$C_RED" "$LOG_FILE" "$C_RESET" >&2
    fi
}
trap on_error ERR

if [ "$(id -u)" -ne 0 ]; then
    die "live-build requires root (chroot/mount/loopback access). Re-run as: sudo ./build.sh"
fi

if ! command -v lb >/dev/null 2>&1; then
    die "live-build (the 'lb' command) is not installed. Install it with: apt-get install live-build kali-archive-keyring"
fi

cd "$(dirname "$0")"

LOG_FILE="build-$(date +%Y%m%d-%H%M%S).log"
: > "$LOG_FILE"

printf '%s%s  ArgOS ISO build%s %s(log: %s)%s\n' "$C_BOLD" "$C_CYAN" "$C_RESET" "$C_DIM" "$LOG_FILE" "$C_RESET"

if [ "$SKIP_PREFLIGHT" -eq 0 ]; then
    step "Preflight: verifying package lists against kali-rolling"
    if ! ./scripts/argos-preflight.sh 2>&1 | tee -a "$LOG_FILE"; then
        die "preflight found package(s) that don't currently resolve -- see above. Fix the package lists, or re-run with --skip-preflight to build anyway."
    fi
    step_ok "all packages resolve"
else
    step "Preflight"
    step_skip "skipped (--skip-preflight)"
fi

step "lb clean --purge"
lb clean --purge 2>&1 | tee -a "$LOG_FILE"
step_ok "cleaned"

step "lb config"
lb config 2>&1 | tee -a "$LOG_FILE"
step_ok "config generated"

step "lb build"
printf '%s   (this is the long one -- package downloads + squashfs compression, typically 20-60 min)%s\n' "$C_DIM" "$C_RESET"
lb build 2>&1 | tee -a "$LOG_FILE"
step_ok "build finished"

TOTAL_ELAPSED=$(( $(date +%s) - BUILD_START ))
ISO=$(find . -maxdepth 1 -name '*.iso' -print -quit)

printf '\n%s%s  done %s(%s)%s\n' "$C_BOLD" "$C_GREEN" "$C_DIM" "$(human_duration "$TOTAL_ELAPSED")" "$C_RESET"
if [ -n "$ISO" ]; then
    printf '  %s%s%s  %s\n' "$C_BOLD" "$ISO" "$C_RESET" "$(du -h "$ISO" | cut -f1)"
else
    printf '  %swarning: no .iso found in %s -- check %s for errors.%s\n' "$C_YELLOW" "$(pwd)" "$LOG_FILE" "$C_RESET"
fi
