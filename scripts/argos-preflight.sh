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
    echo "argos-preflight: all packages resolve against ${SUITE}."
else
    echo "argos-preflight: ${MISSING_COUNT} package(s) do not currently resolve (see FAIL lines above)."
    echo "argos-preflight: comment them out (with a note) or find the renamed/replacement"
    echo "argos-preflight: package before running a full build -- see README's package-list"
    echo "argos-preflight: section for how nikto/wpscan were handled as a precedent."
fi

# ArgOS's own application menu (config/includes.chroot/etc/xdg/menus/
# applications-merged/argos-applications.menu) replaces kali-menu's
# with a flat, 8-category structure -- but it re-buckets tools by the
# exact kali-* Category tags each tool's own .desktop file already
# carries, hardcoded into a static file at the time it was written. If
# kali-menu ever adds a new Category tag upstream, any tool using only
# that new tag would have no top-level menu entry at all once
# kali-applications.menu is removed (0180-remove-upstream-branding-
# leftovers.hook.chroot) -- not a crash, just a tool quietly missing
# from navigation, exactly the kind of thing this project doesn't want
# left to chance. So: fetch the real, current kali-menu package and
# diff its actual Category tags against ours.
ARGOS_MENU="${REPO_ROOT}/config/includes.chroot/etc/xdg/menus/applications-merged/argos-applications.menu"
MENU_MISSING_COUNT=0

if [ -f "${ARGOS_MENU}" ]; then
    echo
    echo "argos-preflight: checking ArgOS's menu categories cover every real kali-menu Category tag"
    # main.gz was already fetched by the package-availability loop above
    # (component="main"); reused here rather than fetched again.
    KALI_MENU_PATH=$(gunzip -c "${WORKDIR}/main.gz" | awk '/^Package: kali-menu$/,/^$/' | grep '^Filename:' | awk '{print $2}')
    if [ -n "${KALI_MENU_PATH}" ] && curl -fsSL -m 60 "${MIRROR}/${KALI_MENU_PATH}" -o "${WORKDIR}/kali-menu.deb" 2>/dev/null; then
        mkdir -p "${WORKDIR}/kali-menu-x"
        if dpkg-deb -x "${WORKDIR}/kali-menu.deb" "${WORKDIR}/kali-menu-x" 2>/dev/null; then
            KALI_MENU_FILE="${WORKDIR}/kali-menu-x/etc/xdg/menus/applications-merged/kali-applications.menu"
            if [ -f "${KALI_MENU_FILE}" ]; then
                grep -oE '<Category>[a-z0-9-]+</Category>' "${KALI_MENU_FILE}" | sed 's/<\/*Category>//g' | sort -u > "${WORKDIR}/kali-categories.txt"
                grep -oE '<Category>[a-z0-9-]+</Category>' "${ARGOS_MENU}" | sed 's/<\/*Category>//g' | sort -u > "${WORKDIR}/argos-categories.txt"
                UNMAPPED=$(comm -23 "${WORKDIR}/kali-categories.txt" "${WORKDIR}/argos-categories.txt")
                if [ -n "${UNMAPPED}" ]; then
                    echo "FAIL  kali-menu now has categories ArgOS's menu doesn't map:"
                    echo "${UNMAPPED}" | sed 's/^/    /'
                    MENU_MISSING_COUNT=$(printf '%s\n' "${UNMAPPED}" | grep -c .)
                else
                    echo "  ok: every real kali-menu category is covered by argos-applications.menu"
                fi
            else
                echo "  (kali-menu package no longer ships kali-applications.menu at the expected path -- skipping this check)"
            fi
        fi
    else
        echo "  (could not fetch kali-menu .deb -- skipping this check, not treated as a failure)"
    fi
fi

if [ "${MISSING_COUNT}" -eq 0 ] && [ "${MENU_MISSING_COUNT}" -eq 0 ]; then
    echo
    echo "argos-preflight: safe to build."
    exit 0
else
    echo
    echo "argos-preflight: issues found above -- fix before running a full build."
    exit 1
fi
