# Builds and installs ArgOS's own original tooling. Run as root inside the
# chroot by config/hooks/normal/0150-build-argos-tools.hook.chroot during
# the ISO build; also just works as a normal Makefile if you want to build
# these on any Debian/Kali box directly (needs gcc, make, libc6-dev, curl).

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2
PREFIX  ?= /usr/local

.PHONY: all install clean

all: argosd/argosd argos-triage/argos-triage argos-toolfinder/argos-toolfinder

argosd/argosd: argosd/argosd.c
	$(CC) $(CFLAGS) -o $@ $<

argos-triage/argos-triage: argos-triage/argos-triage.c
	$(CC) $(CFLAGS) -o $@ $<

argos-toolfinder/argos-toolfinder: argos-toolfinder/argos-toolfinder.c
	$(CC) $(CFLAGS) -o $@ $<

install: all
	install -D -m 0755 argosd/argosd $(DESTDIR)$(PREFIX)/bin/argosd
	install -D -m 0755 argos-triage/argos-triage $(DESTDIR)$(PREFIX)/bin/argos-triage
	install -D -m 0755 argos-toolfinder/argos-toolfinder $(DESTDIR)$(PREFIX)/bin/argos-toolfinder
	install -D -m 0644 argosd/argosd.service $(DESTDIR)/etc/systemd/system/argosd.service

clean:
	rm -f argosd/argosd argos-triage/argos-triage argos-toolfinder/argos-toolfinder
