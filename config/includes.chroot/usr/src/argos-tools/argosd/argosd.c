/*
 * argosd - ArgOS hardening daemon.
 *
 * Runs as a systemd service. On startup it applies a small set of kernel
 * hardening sysctls, then loops forever watching for the two specific
 * risks ArgOS's own defaults create: the published default login
 * (argos/argos) left unchanged, and sshd listening while that default is
 * still in place. Either alone is logged; both together is a loud,
 * broadcast warning, since that combination means anyone on the network
 * can log in.
 *
 * `argosd --status` (no root required) prints the last status this loop
 * wrote out, for a quick check without reading syslog.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <syslog.h>
#include <time.h>
#include <errno.h>
#include <sys/wait.h>

#define STATUS_PATH "/run/argosd.status"
#define STATUS_TMP_PATH "/run/.argosd.status.tmp"
#define SHADOW_PATH "/etc/shadow"
#define CHECK_INTERVAL_SECONDS 60

/*
 * The exact /etc/shadow password hash config/hooks/normal/0100-create-argos-user.hook.chroot
 * bakes in for the "argos" user (SHA-512 crypt, fixed salt "argosdefaultsalt",
 * password "argos"). Every freshly-built ArgOS image has this exact string
 * in /etc/shadow; the moment someone runs `passwd argos`, a new hash with a
 * fresh random salt replaces it and this comparison stops matching.
 */
#define ARGOS_DEFAULT_HASH \
    "$6$argosdefaultsalt$Fg6xszR//6fzLe8WrCYqucX3ZQp8sBV/74BakbC8shDLIun5GahhppNbFshQXn/JoJmCfdEYgtNqY1NnFqI2v."

static volatile sig_atomic_t g_shutdown = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_shutdown = 1;
}

/* Hardening sysctls applied once at startup. Failures (module not loaded,
 * ipv6 disabled, running unprivileged in a test environment, ...) are
 * logged at LOG_WARNING and otherwise ignored -- none of these are fatal
 * to the daemon's job. */
struct sysctl_setting {
    const char *path;
    const char *value;
    const char *description;
};

static const struct sysctl_setting HARDENING_SETTINGS[] = {
    {"/proc/sys/net/ipv4/conf/all/rp_filter", "1", "reverse path filtering"},
    {"/proc/sys/net/ipv4/conf/default/rp_filter", "1", "reverse path filtering (default)"},
    {"/proc/sys/net/ipv4/conf/all/accept_source_route", "0", "reject source-routed packets"},
    {"/proc/sys/net/ipv4/conf/all/send_redirects", "0", "disable ICMP redirect sending"},
    {"/proc/sys/net/ipv4/icmp_echo_ignore_broadcasts", "1", "ignore broadcast ICMP (smurf mitigation)"},
    {"/proc/sys/net/ipv4/icmp_ignore_bogus_error_responses", "1", "ignore bogus ICMP error responses"},
    {"/proc/sys/net/ipv4/tcp_syncookies", "1", "SYN flood protection"},
    {"/proc/sys/kernel/kptr_restrict", "2", "hide kernel pointers from unprivileged reads"},
    {"/proc/sys/kernel/dmesg_restrict", "1", "restrict dmesg to privileged users"},
    {"/proc/sys/kernel/yama/ptrace_scope", "1", "restrict ptrace to child processes"},
    {NULL, NULL, NULL},
};

static void apply_hardening_settings(void) {
    for (int i = 0; HARDENING_SETTINGS[i].path != NULL; i++) {
        const struct sysctl_setting *s = &HARDENING_SETTINGS[i];
        int fd = open(s->path, O_WRONLY);
        if (fd < 0) {
            syslog(LOG_WARNING, "argosd: cannot open %s (%s): %s",
                   s->path, s->description, strerror(errno));
            continue;
        }
        ssize_t len = (ssize_t)strlen(s->value);
        if (write(fd, s->value, (size_t)len) != len) {
            syslog(LOG_WARNING, "argosd: failed to set %s=%s (%s)",
                   s->path, s->value, s->description);
        } else {
            syslog(LOG_INFO, "argosd: applied %s=%s (%s)",
                   s->path, s->value, s->description);
        }
        close(fd);
    }
}

/* Returns 1 if the argos user's password still matches the known
 * ArgOS-default hash, 0 if it differs (changed) or the user/line can't be
 * found, -1 on error reading the file. */
static int default_password_still_set(void) {
    FILE *f = fopen(SHADOW_PATH, "r");
    if (!f) {
        syslog(LOG_WARNING, "argosd: cannot open %s: %s", SHADOW_PATH, strerror(errno));
        return -1;
    }

    char line[1024];
    int result = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "argos:", 6) != 0) {
            continue;
        }
        char *hash_start = line + 6;
        char *hash_end = strchr(hash_start, ':');
        if (!hash_end) {
            break;
        }
        size_t hash_len = (size_t)(hash_end - hash_start);
        if (hash_len == strlen(ARGOS_DEFAULT_HASH) &&
            strncmp(hash_start, ARGOS_DEFAULT_HASH, hash_len) == 0) {
            result = 1;
        }
        break;
    }
    fclose(f);
    return result;
}

/* Returns 1 if something is LISTENing on TCP port 22 (IPv4 or IPv6), 0 if
 * not, -1 on error. Reads /proc/net/tcp{,6} directly rather than shelling
 * out, since the format is stable kernel ABI. */
static int port_is_listening(const char *proc_path, unsigned short port) {
    FILE *f = fopen(proc_path, "r");
    if (!f) {
        return -1; /* e.g. /proc/net/tcp6 absent when IPv6 is disabled */
    }

    char line[512];
    int found = 0;
    /* Skip header line. */
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return 0;
    }
    /* Format (whitespace-separated): sl local_address rem_address st ...
     * local_address/rem_address look like "0100007F:0050" (hex addr:hex port). */
    while (fgets(line, sizeof(line), f)) {
        unsigned local_addr, local_port, rem_addr, rem_port, state;
        if (sscanf(line, "%*d: %x:%x %x:%x %x",
                   &local_addr, &local_port, &rem_addr, &rem_port, &state) != 5) {
            continue;
        }
        (void)local_addr;
        (void)rem_addr;
        (void)rem_port;
        if (local_port == port && state == 0x0A /* TCP_LISTEN */) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static int ssh_is_listening(void) {
    int v4 = port_is_listening("/proc/net/tcp", 22);
    int v6 = port_is_listening("/proc/net/tcp6", 22);
    if (v4 == 1 || v6 == 1) {
        return 1;
    }
    if (v4 == -1 && v6 == -1) {
        return -1;
    }
    return 0;
}

static void broadcast_warning(const char *message) {
    pid_t pid = fork();
    if (pid == 0) {
        execlp("wall", "wall", message, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }
}

static void write_status(int default_pw, int ssh_listening) {
    int fd = open(STATUS_TMP_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        syslog(LOG_WARNING, "argosd: cannot write %s: %s", STATUS_TMP_PATH, strerror(errno));
        return;
    }

    time_t now = time(NULL);
    char timebuf[64];
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S %Z", &tm_now);

    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
        "argosd status as of %s\n"
        "  default argos/argos password : %s\n"
        "  sshd listening on port 22    : %s\n"
        "  combined risk                : %s\n",
        timebuf,
        default_pw > 0 ? "STILL SET" : (default_pw == 0 ? "changed" : "unknown"),
        ssh_listening > 0 ? "yes" : (ssh_listening == 0 ? "no" : "unknown"),
        (default_pw > 0 && ssh_listening > 0) ? "CRITICAL - change the password or disable sshd" : "ok");
    if (n > 0) {
        ssize_t written = write(fd, buf, (size_t)n);
        if (written < 0) {
            syslog(LOG_WARNING, "argosd: write to %s failed: %s", STATUS_TMP_PATH, strerror(errno));
        }
    }
    close(fd);
    rename(STATUS_TMP_PATH, STATUS_PATH);
}

static void print_status_and_exit(void) {
    FILE *f = fopen(STATUS_PATH, "r");
    if (!f) {
        printf("no status recorded yet (is argosd running? try: systemctl status argosd)\n");
        exit(1);
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fwrite(buf, 1, n, stdout);
    }
    fclose(f);
    exit(0);
}

int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--status") == 0 || strcmp(argv[1], "-s") == 0)) {
        print_status_and_exit();
    }
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("usage: argosd [--status]\n"
               "  (no args)  run as the hardening daemon (intended to run under systemd)\n"
               "  --status   print the last recorded status and exit\n");
        return 0;
    }

    openlog("argosd", LOG_PID, LOG_DAEMON);
    syslog(LOG_INFO, "argosd starting");

    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    apply_hardening_settings();

    while (!g_shutdown) {
        int default_pw = default_password_still_set();
        int ssh_listening = ssh_is_listening();

        write_status(default_pw, ssh_listening);

        if (default_pw > 0 && ssh_listening > 0) {
            syslog(LOG_CRIT,
                   "argosd: CRITICAL - sshd is listening and the argos user still has the default "
                   "published password. Run 'passwd argos' now or 'systemctl disable --now ssh'.");
            broadcast_warning(
                "ArgOS SECURITY WARNING: sshd is listening and the default argos/argos password "
                "has not been changed. Run 'passwd argos' or disable sshd.");
        } else if (default_pw > 0) {
            syslog(LOG_WARNING, "argosd: the argos user still has the default published password");
        } else if (ssh_listening > 0) {
            syslog(LOG_INFO, "argosd: sshd is listening (default password has been changed)");
        }

        for (int waited = 0; waited < CHECK_INTERVAL_SECONDS && !g_shutdown; waited++) {
            sleep(1);
        }
    }

    syslog(LOG_INFO, "argosd shutting down");
    closelog();
    return 0;
}
