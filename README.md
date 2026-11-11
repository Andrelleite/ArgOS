<p align="center">
  <img src="assets/plymouth-logo-1920x1080.png" alt="ArgOS logo" width="600">
</p>

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
config/hooks/normal/*.hook.chroot    default user, sshd-off, boot theming, argos-tools build, cleanup
config/includes.chroot/         files dropped verbatim into the chroot (Plymouth + GRUB themes,
                                 /etc/argos/ config, /usr/src/argos-tools/ original C tooling)
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

`auto/config` and `auto/clean` were verified by actually installing
Kali's real `live-build` package (`1:20250814+kali3`, pulled straight
from `kali.download`) and running `lb clean`/`lb config` against this
repo, not written from flag names recalled from memory or tested only
against Debian/Ubuntu's own packaging of live-build, which uses
different flag spellings for several of these (plural vs. singular
`--architecture`/`--binary-image`/`--bootloaders`, and
`--debootstrap-options` vs. `--bootstrap-keyring`) and lacks others
entirely. If `lb config` errors on a different live-build version, run
`lb config -u` to see what that version actually accepts.

## Performance choices

Two deliberate speed decisions, both config/hooks-level (no custom
packaging needed):

- **`--chroot-squashfs-compression-type zstd`** in `auto/config`. The
  live root filesystem is read through this compression on every file
  access while running from USB/live media, not just once at boot;
  `zstd` decompresses markedly faster than live-build's `xz` default for
  a modest size cost, which is almost always the right trade for a live
  OS (build-time compression ratio barely matters; runtime decompression
  speed does, constantly).
- **`config/hooks/normal/0250-disable-background-timers.hook.chroot`**
  masks `apt-daily.timer`/`apt-daily-upgrade.timer` (shipped by the `apt`
  package itself) so a live session never spends boot-time I/O on them,
  and — more importantly — never silently mutates packages mid-engagement
  via an unattended background upgrade.

## Package categories

Every list in `config/package-lists/` is CLI-only by design — no GUI
pentest tools. Where Kali's default toolset ships both a GUI and a
headless/CLI variant, we install the CLI one:

| Excluded (GUI)                        | Included instead                          |
|----------------------------------------|--------------------------------------------|
| burpsuite, zaproxy                     | sqlmap, nuclei, ffuf, gobuster, skipfish    |
| wireshark (GUI)                        | tshark, tcpdump                             |
| maltego, zenmap                        | nmap, masscan, amass, recon-ng              |
| armitage                                | metasploit-framework (msfconsole)           |
| ghidra, cutter                          | radare2, gdb, objdump, binutils             |
| johnny, hydra-gtk                       | john, hydra                                 |
| ettercap-graphical                      | ettercap-text-only                          |
| cherrytree                              | (plain text/markdown notes — no GUI notetaker shipped) |
| autopsy                                 | sleuthkit, testdisk, foremost, scalpel      |

Categories: `base`, `recon`, `web`, `exploit`, `password`, `wireless`,
`forensics`, `reverse-engineering`, `post-exploit` — **277 distinct
packages across 9 files**, not just a hand-picked starter set. The bulk of the
breadth comes directly from Kali's own `kali-linux-headless` metapackage
(the official metapackage Kali itself ships for exactly this use case:
every tool from `kali-linux-default`/`kali-linux-everything` *except* the
GUI ones) — ArgOS pulls its ~190 packages apart into the category files
above instead of depending on it as one opaque blob, so the lists stay
readable and independently auditable. `base.list.chroot` also pulls
`kali-linux-core` + `kali-system-cli` directly, so ArgOS's base system
tracks Kali's own definition of "core," not a guess at one.

**Every package name in every list was verified against the live
`kali-rolling` archive** (`https://kali.download`, main + contrib +
non-free + non-free-firmware components) **on 2026-11-11** — not just
recalled from memory. Two exceptions are called out explicitly in
`web.list.chroot`: `nikto` and `wpscan` are commented out because they
were confirmed *absent* from the amd64 archive on that date (likely a
transient rebuild/transition gap — `kali-linux-headless` itself still
hard-depends on both upstream, so Kali's own metapackage would currently
fail to install too). Re-check `apt-cache policy nikto wpscan` against a
current mirror and uncomment them once they're back.

Package sets drift on a rolling release — if you build significantly
later than the verification date above, re-run a similar check (or just
`apt-get update && apt-get install -s <package-list contents>` as a dry
run) before committing to a full `lb build`.

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

## ArgOS's own tooling

Unlike everything else in this image, these two are original — written for
ArgOS, not pulled from Kali. Source lives in the image itself at
`/usr/src/argos-tools/` (a normal FHS location for exactly this), and
`config/hooks/normal/0150-build-argos-tools.hook.chroot` compiles and
installs them at image-build time via `make install`. Both are plain C,
built with only `gcc`/`make`/libc (no linked HTTP/JSON library — see
below) and both were compiled and exercised locally before being added
here.

### `argosd` — hardening daemon

A systemd service (`argosd.service`, enabled by default) that:

- Applies a handful of kernel-hardening sysctls at startup (`rp_filter`,
  disabling source-routed packets/ICMP redirects, `kptr_restrict`,
  `dmesg_restrict`, `yama/ptrace_scope`, SYN cookies).
- Every 60 seconds, checks two things: whether the `argos` user's password
  still matches ArgOS's known default hash (see the comment in
  `config/hooks/normal/0100-create-argos-user.hook.chroot` — the hash is
  fixed/reproducible across builds specifically so this comparison works),
  and whether sshd is listening on port 22 (read directly from
  `/proc/net/tcp{,6}`, no shelling out).
- If **both** are true at once — sshd reachable *and* the default
  credentials still work — it logs a `LOG_CRIT` syslog entry and
  broadcasts a warning to every terminal via `wall`. Either alone just
  gets a quieter log line.
- `argosd --status` prints the last check's result (`/run/argosd.status`)
  without needing root.

### `argos-triage` — recon/triage utility

```
nmap -oG - -p- <target> | argos-triage
argos-triage -i scan.gnmap --ai
```

Parses nmap greppable (`-oG`) output, matches each open port's
service/version against `/etc/argos/triage-rules.csv` (service substring,
version substring, severity, note — edit this file to extend the
ruleset; it's reloaded on every run, no recompiling), and prints a
severity-ranked, colorized report (CRITICAL first). The shipped ruleset is
a small, accurate starter set (known historical backdoors/RCEs like
vsftpd 2.3.4 and the Samba `usermap_script` bug, cleartext-protocol
warnings, a few "worth a second look" service flags) — not a
vulnerability database.

**`--ai`**: this is where the AI feature lives. If `ARGOS_AI_API_KEY` is
set in the environment, the plain-text report (colors stripped,
regardless of terminal settings) is sent to an LLM for a prioritized,
prose summary. Endpoint/model are configurable in `/etc/argos/ai.conf`
(defaults to Anthropic's Messages API); the key itself is **never** read
from that file or passed on the command line — only the environment
variable — and never touches `argv`/`ps` output: the request is made by
shelling out to `curl` with the API key written to a 0600 temp file via
curl's `--config`/header mechanism, unlinked immediately after. With no
key set, `argos-triage` still works fully offline (rule-based triage
only) and just says how to enable the AI step.

### Extending either one

Both are small, single-file C programs (`argosd/argosd.c`,
`argos-triage/argos-triage.c`) with no build system beyond the one
`Makefile`. `cd /usr/src/argos-tools && make` rebuilds either; `argos-tools`
ships in the image as source specifically so this is possible on a live
system, not just at ISO-build time.

## License

MIT — see [LICENSE](LICENSE).
