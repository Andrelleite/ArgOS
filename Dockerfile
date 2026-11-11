# Build environment for the ArgOS ISO.
#
# This does NOT build the ISO at image-build time -- it just gives you a
# Kali-rolling userspace with live-build and the Kali keyring installed, so
# `lb build` runs the same way it would on a real Kali/Debian host. Build the
# actual ISO by running this container with --privileged (live-build needs
# to mount/chroot/loopback-mount during the build) and a bind mount of this
# repo, via build.sh.

FROM kalilinux/kali-rolling

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        live-build \
        kali-archive-keyring \
        debootstrap \
        squashfs-tools \
        xorriso \
        isolinux \
        syslinux-common \
        dosfstools \
        mtools \
        ca-certificates \
        git \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

ENTRYPOINT ["/bin/bash"]
