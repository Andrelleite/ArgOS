/*
 * argos-triage - ArgOS recon/triage utility.
 *
 * Reads nmap greppable output (-oG, or the equivalent piped via
 * `nmap -oG -`), matches each open port's service/version against an
 * editable ruleset (/etc/argos/triage-rules.csv), and prints a
 * severity-ranked, colorized report. With --ai and ARGOS_AI_API_KEY set,
 * it also sends the report to an LLM API (default: Anthropic's Messages
 * API, configurable via /etc/argos/ai.conf) for a prose summary -- via a
 * `curl` subprocess, not a linked HTTP library, so this tool only needs a
 * C compiler and curl to build and run.
 *
 * Usage:
 *   nmap -oG - -p- target | argos-triage
 *   argos-triage -i scan.gnmap
 *   argos-triage -i scan.gnmap --ai
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdarg.h>

#define RULES_PATH_DEFAULT "/etc/argos/triage-rules.csv"
#define AI_CONF_PATH "/etc/argos/ai.conf"
#define MAX_RULES 512
#define MAX_FINDINGS 4096
#define REPORT_BUF_CAP (1u << 20) /* 1 MiB */

typedef struct {
    char service[64];
    char version[64];
    char severity[16];
    char note[256];
} rule_t;

typedef struct {
    char ip[64];
    char port[16];
    char proto[16];
    char service[64];
    char version[128];
    const rule_t *matched; /* NULL if no rule matched */
} finding_t;

static rule_t g_rules[MAX_RULES];
static int g_rule_count = 0;

static finding_t g_findings[MAX_FINDINGS];
static int g_finding_count = 0;

static int g_use_color = 1;

/* ---------- small string helpers ---------- */

static void safe_copy(char *dst, size_t dst_cap, const char *src) {
    snprintf(dst, dst_cap, "%s", src);
}

static void trim_newline(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

static int icontains(const char *haystack, const char *needle) {
    if (needle[0] == '\0') {
        return 1;
    }
    size_t hlen = strlen(haystack), nlen = strlen(needle);
    if (nlen > hlen) {
        return 0;
    }
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (strncasecmp(haystack + i, needle, nlen) == 0) {
            return 1;
        }
    }
    return 0;
}

static int severity_rank(const char *sev) {
    if (strcasecmp(sev, "CRITICAL") == 0) return 0;
    if (strcasecmp(sev, "HIGH") == 0) return 1;
    if (strcasecmp(sev, "MEDIUM") == 0) return 2;
    if (strcasecmp(sev, "LOW") == 0) return 3;
    if (strcasecmp(sev, "INFO") == 0) return 4;
    return 5;
}

static const char *severity_color(const char *sev) {
    if (!g_use_color) return "";
    switch (severity_rank(sev)) {
        case 0: return "\033[1;31m"; /* bright red */
        case 1: return "\033[31m";   /* red */
        case 2: return "\033[33m";   /* yellow */
        case 3: return "\033[36m";   /* cyan */
        default: return "\033[90m";  /* grey */
    }
}

static const char *color_reset(void) {
    return g_use_color ? "\033[0m" : "";
}

/* ---------- ruleset loading ---------- */

static void load_rules(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "argos-triage: warning: cannot open ruleset %s (%s) -- "
                        "continuing with zero rules\n", path, strerror(errno));
        return;
    }

    char line[1024];
    while (fgets(line, sizeof(line), f) && g_rule_count < MAX_RULES) {
        trim_newline(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        char *p1 = strchr(line, ',');
        if (!p1) continue;
        char *p2 = strchr(p1 + 1, ',');
        if (!p2) continue;
        char *p3 = strchr(p2 + 1, ',');
        if (!p3) continue;

        rule_t *r = &g_rules[g_rule_count];
        size_t svc_len = (size_t)(p1 - line);
        size_t ver_len = (size_t)(p2 - (p1 + 1));
        size_t sev_len = (size_t)(p3 - (p2 + 1));

        if (svc_len >= sizeof(r->service)) svc_len = sizeof(r->service) - 1;
        if (ver_len >= sizeof(r->version)) ver_len = sizeof(r->version) - 1;
        if (sev_len >= sizeof(r->severity)) sev_len = sizeof(r->severity) - 1;

        memcpy(r->service, line, svc_len);
        r->service[svc_len] = '\0';
        memcpy(r->version, p1 + 1, ver_len);
        r->version[ver_len] = '\0';
        memcpy(r->severity, p2 + 1, sev_len);
        r->severity[sev_len] = '\0';
        strncpy(r->note, p3 + 1, sizeof(r->note) - 1);
        r->note[sizeof(r->note) - 1] = '\0';

        g_rule_count++;
    }
    fclose(f);
}

static const rule_t *match_rule(const char *service, const char *version) {
    char haystack[256];
    snprintf(haystack, sizeof(haystack), "%s %s", service, version);

    for (int i = 0; i < g_rule_count; i++) {
        const rule_t *r = &g_rules[i];
        if (!icontains(haystack, r->service)) {
            continue;
        }
        if (r->version[0] != '\0' && !icontains(haystack, r->version)) {
            continue;
        }
        return r;
    }
    return NULL;
}

/* ---------- nmap -oG parsing ---------- */

/* Copies the field up to (but not including) the next '/' in *cursor into
 * dst (bounded), and advances *cursor past the '/'. Returns 0 on success,
 * -1 if there was no '/' left to find. */
static int next_slash_field(char **cursor, char *dst, size_t dst_cap) {
    char *slash = strchr(*cursor, '/');
    if (!slash) {
        return -1;
    }
    size_t len = (size_t)(slash - *cursor);
    if (len >= dst_cap) len = dst_cap - 1;
    memcpy(dst, *cursor, len);
    dst[len] = '\0';
    *cursor = slash + 1;
    return 0;
}

static void parse_ports_blob(const char *ip, char *ports_blob) {
    /* Entries are separated by ", ". Walk them with strtok_r on a copy. */
    char *saveptr = NULL;
    char *entry = strtok_r(ports_blob, ",", &saveptr);
    while (entry) {
        while (*entry == ' ') entry++; /* skip leading space after the comma */

        char cursor_buf[256];
        strncpy(cursor_buf, entry, sizeof(cursor_buf) - 1);
        cursor_buf[sizeof(cursor_buf) - 1] = '\0';
        char *cursor = cursor_buf;

        char port[16], state[16], proto[16], owner[32], service[64], rpc[32];
        if (next_slash_field(&cursor, port, sizeof(port)) == 0 &&
            next_slash_field(&cursor, state, sizeof(state)) == 0 &&
            next_slash_field(&cursor, proto, sizeof(proto)) == 0 &&
            next_slash_field(&cursor, owner, sizeof(owner)) == 0 &&
            next_slash_field(&cursor, service, sizeof(service)) == 0 &&
            next_slash_field(&cursor, rpc, sizeof(rpc)) == 0) {
            (void)owner;
            (void)rpc;

            /* Whatever remains is the version, minus a trailing '/' if present. */
            char version[128];
            strncpy(version, cursor, sizeof(version) - 1);
            version[sizeof(version) - 1] = '\0';
            size_t vlen = strlen(version);
            if (vlen > 0 && version[vlen - 1] == '/') {
                version[vlen - 1] = '\0';
            }

            if (strcasecmp(state, "open") == 0 && g_finding_count < MAX_FINDINGS) {
                finding_t *f = &g_findings[g_finding_count++];
                safe_copy(f->ip, sizeof(f->ip), ip);
                safe_copy(f->port, sizeof(f->port), port);
                safe_copy(f->proto, sizeof(f->proto), proto);
                safe_copy(f->service, sizeof(f->service), service);
                safe_copy(f->version, sizeof(f->version), version);
                f->matched = match_rule(service, version);
            }
        }

        entry = strtok_r(NULL, ",", &saveptr);
    }
}

static void parse_greppable_stream(FILE *in) {
    char line[65536];
    while (fgets(line, sizeof(line), in)) {
        if (strncmp(line, "Host: ", 6) != 0) {
            continue;
        }
        char *ports_kw = strstr(line, "Ports:");
        if (!ports_kw) {
            continue; /* e.g. a "Status: Up" line with no open ports */
        }

        char ip[64];
        {
            const char *p = line + 6;
            size_t i = 0;
            while (p[i] && p[i] != ' ' && i < sizeof(ip) - 1) {
                ip[i] = p[i];
                i++;
            }
            ip[i] = '\0';
        }

        char *ports_value = ports_kw + 6; /* skip "Ports:" */
        while (*ports_value == ' ' || *ports_value == '\t') ports_value++;
        char *end = strchr(ports_value, '\t');
        if (end) *end = '\0';
        trim_newline(ports_value);

        parse_ports_blob(ip, ports_value);
    }
}

/* ---------- report rendering ---------- */

typedef struct {
    char buf[REPORT_BUF_CAP];
    size_t used;
} report_buf_t;

static void rprintf(report_buf_t *rb, const char *fmt, ...) {
    if (rb->used >= sizeof(rb->buf) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(rb->buf + rb->used, sizeof(rb->buf) - rb->used, fmt, ap);
    va_end(ap);
    if (n > 0) {
        rb->used += (size_t)n;
        if (rb->used > sizeof(rb->buf) - 1) rb->used = sizeof(rb->buf) - 1;
    }
}

static int compare_findings(const void *a, const void *b) {
    const finding_t *fa = (const finding_t *)a;
    const finding_t *fb = (const finding_t *)b;
    const char *sa = fa->matched ? fa->matched->severity : "INFO";
    const char *sb = fb->matched ? fb->matched->severity : "INFO";
    int ra = severity_rank(sa), rb_ = severity_rank(sb);
    if (ra != rb_) return ra - rb_;
    return strcmp(fa->ip, fb->ip);
}

static void render_report(report_buf_t *rb) {
    qsort(g_findings, (size_t)g_finding_count, sizeof(finding_t), compare_findings);

    rprintf(rb, "ArgOS triage report -- %d open port(s) examined, %d rule(s) loaded\n\n",
            g_finding_count, g_rule_count);

    for (int i = 0; i < g_finding_count; i++) {
        const finding_t *f = &g_findings[i];
        const char *sev = f->matched ? f->matched->severity : "INFO";
        const char *note = f->matched ? f->matched->note : "no automatic rule match -- review manually";

        rprintf(rb, "%s[%-8s]%s %s:%s/%s %s",
                severity_color(sev), sev, color_reset(),
                f->ip, f->port, f->proto, f->service);
        if (f->version[0]) {
            rprintf(rb, " (%s)", f->version);
        }
        rprintf(rb, " - %s\n", note);
    }

    if (g_finding_count == 0) {
        rprintf(rb, "No open ports found in the input (expected nmap -oG / -oG - output).\n");
    }
}

/* ---------- JSON string escaping (for the AI request payload) ---------- */

static void json_escape_append(report_buf_t *rb, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '\\': rprintf(rb, "\\\\"); break;
            case '"':  rprintf(rb, "\\\""); break;
            case '\n': rprintf(rb, "\\n"); break;
            case '\r': rprintf(rb, "\\r"); break;
            case '\t': rprintf(rb, "\\t"); break;
            default:
                if (*p < 0x20) {
                    rprintf(rb, "\\u%04x", *p);
                } else {
                    rprintf(rb, "%c", *p);
                }
        }
    }
}

/* Minimal best-effort unescape of a JSON string value (handles \\, \", \n,
 * \r, \t; \u sequences are passed through as '?' rather than fully decoded
 * -- this is a triage helper, not a JSON library). */
static void json_unescape_into(const char *src, char *dst, size_t dst_cap) {
    size_t di = 0;
    for (const char *p = src; *p && di + 1 < dst_cap; p++) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case 'n': dst[di++] = '\n'; break;
                case 'r': dst[di++] = '\r'; break;
                case 't': dst[di++] = '\t'; break;
                case '"': dst[di++] = '"'; break;
                case '\\': dst[di++] = '\\'; break;
                case 'u': dst[di++] = '?'; p += 4; break;
                default: dst[di++] = *p; break;
            }
        } else {
            dst[di++] = *p;
        }
    }
    dst[di] = '\0';
}

/* ---------- AI config + call ---------- */

typedef struct {
    char endpoint[256];
    char model[128];
    char anthropic_version[32];
} ai_config_t;

static void load_ai_config(ai_config_t *cfg) {
    strncpy(cfg->endpoint, "https://api.anthropic.com/v1/messages", sizeof(cfg->endpoint) - 1);
    cfg->endpoint[sizeof(cfg->endpoint) - 1] = '\0';
    strncpy(cfg->model, "claude-sonnet-5", sizeof(cfg->model) - 1);
    cfg->model[sizeof(cfg->model) - 1] = '\0';
    strncpy(cfg->anthropic_version, "2023-06-01", sizeof(cfg->anthropic_version) - 1);
    cfg->anthropic_version[sizeof(cfg->anthropic_version) - 1] = '\0';

    FILE *f = fopen(AI_CONF_PATH, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;
        if (strcmp(key, "endpoint") == 0) {
            strncpy(cfg->endpoint, val, sizeof(cfg->endpoint) - 1);
        } else if (strcmp(key, "model") == 0) {
            strncpy(cfg->model, val, sizeof(cfg->model) - 1);
        } else if (strcmp(key, "anthropic_version") == 0) {
            strncpy(cfg->anthropic_version, val, sizeof(cfg->anthropic_version) - 1);
        }
    }
    fclose(f);
}

static int write_whole_file(const char *path, const char *data, size_t len, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    close(fd);
    return 0;
}

static char *read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = '\0';
    fclose(f);
    if (out_len) *out_len = got;
    return buf;
}

static void run_ai_summary(const char *report_text) {
    const char *api_key = getenv("ARGOS_AI_API_KEY");
    if (!api_key || api_key[0] == '\0') {
        printf("\n[AI assessment unavailable] set ARGOS_AI_API_KEY to enable "
               "(endpoint/model are configured in %s)\n", AI_CONF_PATH);
        return;
    }

    ai_config_t cfg;
    load_ai_config(&cfg);

    /* Build the JSON request body. */
    report_buf_t payload;
    payload.used = 0;
    rprintf(&payload,
            "{\"model\":\"%s\",\"max_tokens\":1024,\"messages\":[{\"role\":\"user\",\"content\":\"",
            cfg.model);
    json_escape_append(&payload,
        "You are assisting a red team triage workflow. Given this ArgOS "
        "argos-triage report, give a short prioritized summary of what to "
        "attack first and why, in plain text, no markdown:\n\n");
    json_escape_append(&payload, report_text);
    rprintf(&payload, "\"}]}");

    char payload_path[] = "/tmp/argos-triage-payload-XXXXXX";
    char config_path[] = "/tmp/argos-triage-curlcfg-XXXXXX";
    char output_path[] = "/tmp/argos-triage-response-XXXXXX";
    int pfd = mkstemp(payload_path);
    int cfd = mkstemp(config_path);
    int ofd = mkstemp(output_path);
    if (pfd < 0 || cfd < 0 || ofd < 0) {
        fprintf(stderr, "argos-triage: could not create temp files for the AI request: %s\n",
                strerror(errno));
        if (pfd >= 0) { close(pfd); unlink(payload_path); }
        if (cfd >= 0) { close(cfd); unlink(config_path); }
        if (ofd >= 0) { close(ofd); unlink(output_path); }
        return;
    }
    close(pfd);
    close(cfd);
    close(ofd);

    write_whole_file(payload_path, payload.buf, payload.used, 0600);

    /* The curl config file carries the Authorization header so the API
     * key never appears in argv (and so never shows up in `ps`/
     * /proc/<pid>/cmdline for other local users to read). 0600 and
     * unlinked immediately after the request completes. */
    report_buf_t curlcfg;
    curlcfg.used = 0;
    rprintf(&curlcfg, "silent\nshow-error\nrequest = \"POST\"\nurl = \"%s\"\n", cfg.endpoint);
    rprintf(&curlcfg, "header = \"content-type: application/json\"\n");
    rprintf(&curlcfg, "header = \"anthropic-version: %s\"\n", cfg.anthropic_version);
    rprintf(&curlcfg, "header = \"x-api-key: %s\"\n", api_key);
    rprintf(&curlcfg, "data = \"@%s\"\n", payload_path);
    write_whole_file(config_path, curlcfg.buf, curlcfg.used, 0600);

    pid_t pid = fork();
    if (pid == 0) {
        int out = open(output_path, O_WRONLY | O_TRUNC);
        if (out >= 0) {
            dup2(out, STDOUT_FILENO);
            close(out);
        }
        execlp("curl", "curl", "--config", config_path, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    if (pid > 0) {
        waitpid(pid, &status, 0);
    }

    unlink(payload_path);
    unlink(config_path);

    if (pid < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "argos-triage: curl failed to reach the AI endpoint "
                        "(is there network access? is %s reachable?)\n", cfg.endpoint);
        unlink(output_path);
        return;
    }

    size_t resp_len = 0;
    char *resp = read_whole_file(output_path, &resp_len);
    unlink(output_path);
    if (!resp) {
        fprintf(stderr, "argos-triage: got no response body from the AI endpoint\n");
        return;
    }

    /* Find the "text" KEY specifically -- "\"text\"" also matches as a
     * VALUE elsewhere in the response (e.g. "type": "text"), so keep
     * searching until a match is immediately followed by (whitespace
     * then) a colon, which only a key has. Don't assume any particular
     * JSON formatting (compact or pretty-printed both appear in the
     * wild). */
    const char *key = "\"text\"";
    char *start = NULL;
    char *search_from = resp;
    while ((search_from = strstr(search_from, key)) != NULL) {
        char *after_key = search_from + strlen(key);
        char *colon = after_key;
        while (*colon == ' ' || *colon == '\t' || *colon == '\n' || *colon == '\r') {
            colon++;
        }
        if (*colon == ':') {
            char *value = colon + 1;
            while (*value == ' ' || *value == '\t' || *value == '\n' || *value == '\r') {
                value++;
            }
            if (*value == '"') {
                start = value + 1; /* skip the opening quote */
                break;
            }
        }
        search_from = after_key;
    }
    if (!start) {
        fprintf(stderr, "argos-triage: could not find a text response in the API reply:\n%s\n", resp);
        free(resp);
        return;
    }

    /* Find the end of the JSON string value: an unescaped '"'. */
    char *p = start;
    while (*p) {
        if (*p == '"' && *(p - 1) != '\\') {
            break;
        }
        p++;
    }
    size_t raw_len = (size_t)(p - start);
    char *raw = malloc(raw_len + 1);
    memcpy(raw, start, raw_len);
    raw[raw_len] = '\0';

    char decoded[8192];
    json_unescape_into(raw, decoded, sizeof(decoded));

    printf("\n--- AI assessment (%s) ---\n%s\n", cfg.model, decoded);

    free(raw);
    free(resp);
}

/* ---------- main ---------- */

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [-i input.gnmap] [--rules path] [--ai] [--no-color]\n"
        "  reads nmap greppable output (-oG) from a file or stdin, prints a\n"
        "  severity-ranked triage report, optionally followed by an AI summary.\n",
        prog);
}

int main(int argc, char **argv) {
    const char *input_path = NULL;
    const char *rules_path = RULES_PATH_DEFAULT;
    int want_ai = 0;

    if (!isatty(STDOUT_FILENO)) {
        g_use_color = 0;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            input_path = argv[++i];
        } else if (strcmp(argv[i], "--rules") == 0 && i + 1 < argc) {
            rules_path = argv[++i];
        } else if (strcmp(argv[i], "--ai") == 0) {
            want_ai = 1;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            g_use_color = 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    load_rules(rules_path);

    FILE *in = stdin;
    if (input_path) {
        in = fopen(input_path, "r");
        if (!in) {
            fprintf(stderr, "argos-triage: cannot open %s: %s\n", input_path, strerror(errno));
            return 1;
        }
    }
    parse_greppable_stream(in);
    if (input_path) fclose(in);

    report_buf_t report;
    report.used = 0;
    render_report(&report);

    fputs(report.buf, stdout);

    if (want_ai) {
        /* Always send a plain-text (no ANSI color codes) version to the
         * AI, regardless of whether this terminal has color on -- the
         * model has no use for escape sequences, and stripping them here
         * keeps the request payload clean. g_findings is already sorted
         * by render_report(), so re-rendering is cheap and safe. */
        int saved_color = g_use_color;
        g_use_color = 0;
        report_buf_t plain_report;
        plain_report.used = 0;
        render_report(&plain_report);
        g_use_color = saved_color;

        run_ai_summary(plain_report.buf);
    }

    return 0;
}
