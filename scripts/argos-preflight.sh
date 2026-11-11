#!/bin/sh
set -eu
#
# argos-preflight.sh -- verify every package in config/package-lists/
# actually resolves against the live kali-rolling archive, before
# spending 20-60 minutes on a build that fails partway through.
#
# This exists because we hit exactly this problem building ArgOS
# itself: nikto and wpscan were verified present when the package lists
# were written, then were simply gone from the archive by the time of
# an actual build attempt (a rolling release has no staging/QA layer,
# so this kind of transient breakage is routine, not rare). Kali gives
# you no way to know this before `apt-get` fails on you mid-build; this
# script is that check, run standalone against the real mirror.
#
# Usage:
#   ./scripts/argos-preflight.sh                  # checks all lists
#   ./scripts/argos-preflight.sh --rules PATH      # one list.chroot file
#   MIRROR=https://http.kali.org/kali ./scripts/argos-preflight.sh
#
# Exit status: 0 if every package resolves, 1 if anything is missing
# (so this can gate CI or a pre-build check, not just inform a human).

MIRROR="${MIRROR:-https://kali.download/kali}"
SUITE="${SUITE:-kali-rolling}"
COMPONENTS="main contrib non-free non-free-firmware"
ARCH="${ARCH:-amd64}"

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"
LIST_DIR="${REPO_ROOT}/config/package-lists"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "${WORKDIR}"' EXIT

echo "argos-preflight: checking package availability against ${MIRROR} (${SUITE}/${ARCH})"
echo

KNOWN_PACKAGES="${WORKDIR}/known-packages.txt"
: > "${KNOWN_PACKAGES}"

for component in ${COMPONENTS}; do
    url="${MIRROR}/dists/${SUITE}/${component}/binary-${ARCH}/Packages.gz"
    dest="${WORKDIR}/${component}.gz"
    printf '  fetching %s ... ' "${component}"
    if ! curl -fsSL -m 60 "${url}" -o "${dest}" 2>"${WORKDIR}/curl.err"; then
        echo "FAILED"
        echo "    could not fetch ${url}:"
        sed 's/^/    /' "${WORKDIR}/curl.err"
        echo
        echo "argos-preflight: cannot verify anything without reaching the mirror -- aborting."
        exit 1
    fi
    echo "ok"
    gunzip -c "${dest}" | grep -oE '^Package: [^ ]+' | sed 's/^Package: //' >> "${KNOWN_PACKAGES}"
done
sort -u "${KNOWN_PACKAGES}" -o "${KNOWN_PACKAGES}"
echo
echo "  $(wc -l < "${KNOWN_PACKAGES}") package names known to ${SUITE}"
echo

if [ "${1:-}" = "--rules" ] && [ -n "${2:-}" ]; then
    LIST_FILES="$2"
else
    LIST_FILES=$(find "${LIST_DIR}" -name '*.list.chroot' | sort)
fi

MISSING_COUNT=0
CHECKED_COUNT=0

for list_file in ${LIST_FILES}; do
    rel_path="${list_file#"${REPO_ROOT}"/}"
    file_missing=""
    while IFS= read -r line; do
        case "${line}" in
            ''|'#'*) continue ;;
        esac
        pkg="${line%%[[:space:]]*}"
        CHECKED_COUNT=$((CHECKED_COUNT + 1))
        if ! grep -qx "${pkg}" "${KNOWN_PACKAGES}"; then
            file_missing="${file_missing}    ${pkg}\n"
            MISSING_COUNT=$((MISSING_COUNT + 1))
        fi
    done < "${list_file}"

    if [ -n "${file_missing}" ]; then
        echo "FAIL  ${rel_path}"
        printf "%b" "${file_missing}"
    fi
done

echo
echo "argos-preflight: checked ${CHECKED_COUNT} package entries."
if [ "${MISSING_COUNT}" -eq 0 ]; then
    echo "argos-preflight: all packages resolve against ${SUITE}. Safe to build."
    exit 0
else
    echo "argos-preflight: ${MISSING_COUNT} package(s) do not currently resolve (see FAIL lines above)."
    echo "argos-preflight: comment them out (with a note) or find the renamed/replacement"
    echo "argos-preflight: package before running a full build -- see README's package-list"
    echo "argos-preflight: section for how nikto/wpscan were handled as a precedent."
    exit 1
fi
