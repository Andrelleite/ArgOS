# ArgOS

ArgOS is a CLI-only, Kali-rolling-based security/pentesting live distro. No
desktop environment, no display manager — it boots straight to a console
login. It's built with [live-build](https://wiki.debian.org/DebianLive) (the
same tool Kali itself is built with), bootstrapped directly from Kali's own
`kali-rolling` repository rather than reimplementing anything Kali already
maintains.

## What's here

```
auto/config                    live-build invocation (distribution, arch, bootloader, mirrors)
auto/clean                     wrapper around `lb clean`
config/package-lists/*.list.chroot   CLI-only tool sets, one file per category
config/preseed.cfg              debconf answers so the build never blocks on a prompt
config/hooks/normal/*.hook.chroot    default user, sshd-off, boot theming, cleanup
config/includes.chroot/         files dropped verbatim into the chroot (Plymouth + GRUB themes)
assets/                         logo source + the derived splash images
Dockerfile                      Kali-rolling + live-build build environment
build.sh                        `lb clean && lb config && lb build` wrapper
```

## Building the ISO

This is **not run automatically** — building takes real root, network
access, ~20-30GB of free disk, and 20-60 minutes.

On a real Debian/Kali host, as root:

```bash
sudo ./build.sh
```

Or via Docker (the container needs `--privileged` because live-build does
chroot/mount/loopback work that an unprivileged container can't):

```bash
docker build -t argos-os-builder .
docker run --rm --privileged -v "$PWD:/build" argos-os-builder -c "cd /build && ./build.sh"
```

The resulting `.iso` lands in the project root.

## Package categories

Every list in `config/package-lists/` is CLI-only by design — no GUI
pentest tools. Where Kali's default toolset ships both a GUI and a
headless/CLI variant, we install the CLI one:

| Excluded (GUI)                        | Included instead                          |
|----------------------------------------|--------------------------------------------|
| burpsuite, zaproxy                     | nikto, sqlmap, nuclei, ffuf, gobuster       |
| wireshark (GUI)                        | tshark, tcpdump                             |
| maltego, zenmap                        | nmap, masscan, amass, recon-ng              |
| armitage                                | metasploit-framework (msfconsole)           |
| ghidra, cutter                          | radare2, gdb, objdump, binutils             |
| johnny, hydra-gtk                       | john, hydra                                 |
| ettercap-graphical                      | ettercap-text-only                          |
| cherrytree                              | (plain text/markdown notes — no GUI notetaker shipped) |
| autopsy                                 | sleuthkit, testdisk, foremost, scalpel      |

Categories: `base`, `recon`, `web`, `exploit`, `password`, `wireless`,
`forensics`, `reverse-engineering`, `post-exploit`.

**Before a real build**, diff these lists against `apt-cache search` /
`apt list` on a current `kali-rolling` mirror. Kali's package set moves
fast — a couple of tool names here (`crackmapexec` in particular, which
upstream has been migrating to `netexec`) may need updating by the time
you build.

## Default login

The live image creates one user: **`argos` / `argos`**, with passwordless
sudo. This is a deliberate, published default to get a live/USB session
booted — it is **not safe** on anything beyond a throwaway session:

```bash
passwd argos
```

do this (and review `/etc/sudoers.d/argos-nopasswd`) before trusting the
machine with anything sensitive, especially before installing it to disk or
putting it on an untrusted network. A MOTD reminder is baked in as well.

## SSH

`openssh-server` is installed but **disabled and masked** by default — it
will not listen on boot. Host keys are deleted at build time and
regenerated fresh on first boot via a oneshot systemd unit, so every image
doesn't share the same key pair. Enable it explicitly if you want it:

```bash
sudo systemctl unmask ssh.service
sudo systemctl enable --now ssh.service
```

## Boot splash

Plymouth shows a static logo (no animation) centered on a flat near-black
background, via Plymouth's `image` module
(`config/includes.chroot/usr/share/plymouth/themes/argos/`). If your
build's Plymouth package doesn't ship the `image` splash module, swap
`ModuleName=image` for `ModuleName=two-step` in `argos.plymouth` and set
`Watermark=logo.png` under a `[two-step]` section instead — leaving out the
throbber/progress-bar frame images keeps it static either way.

The same logo is reused as a GRUB background/theme
(`config/includes.chroot/boot/grub/themes/argos/`) for the installed
system's boot menu, applied via `GRUB_THEME` in
`/etc/default/grub.d/argos-theme.cfg`.

Both images were generated from `assets/argos-logo-source.jpg` by cropping
to the head+wordmark, flood-fill alpha-masking out the near-black
background, and compositing onto flat `#0d0d11` canvases
(`assets/plymouth-logo-1920x1080.png`, `assets/grub-logo-1024x768.png`).

## License

MIT — see [LICENSE](LICENSE).
