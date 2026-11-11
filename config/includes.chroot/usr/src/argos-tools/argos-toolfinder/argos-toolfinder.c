/*
 * argos-toolfinder - "what tool do I use for X?"
 *
 * Type a word or phrase describing what you want to do; prints the
 * matching tool(s) ranked by how many of your words actually matched,
 * not just a single keyword lookup. Backed by /etc/argos/toolfinder.csv
 * (same editable-CSV-no-recompiling pattern as argos-triage's ruleset),
 * so adding a tool or a synonym is a one-line edit, not a code change.
 *
 * Usage:
 *   argos-toolfinder                    interactive: type queries, blank/quit to exit
 *   argos-toolfinder crack wifi password one-shot: search this phrase and exit
 *   argos-toolfinder --list             print every entry grouped by category
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>

#define MANIFEST_PATH_DEFAULT "/etc/argos/toolfinder.csv"
#define MAX_ENTRIES 1024
#define MAX_RESULTS 8

typedef struct {
    char keywords[384];
    char tool[64];
    char category[48];
    char description[256];
} entry_t;

static entry_t g_entries[MAX_ENTRIES];
static int g_entry_count = 0;
static int g_use_color = 1;

static void to_lower_inplace(char *s) {
    for (; *s; s++) {
        *s = (char)tolower((unsigned char)*s);
    }
}

static void safe_copy(char *dst, size_t cap, const char *src) {
    /* Not snprintf("%s", ...): with -O2 inlining, GCC's format-truncation
     * checker can't bound a dynamically-indexed global array access and
     * assumes the source could span the whole array rather than one
     * field -- memcpy/strlen sidesteps that warning class entirely. */
    size_t len = strlen(src);
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void trim_newline(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

static int load_manifest(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "argos-toolfinder: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    char line[1024];
    while (fgets(line, sizeof(line), f) && g_entry_count < MAX_ENTRIES) {
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

        entry_t *e = &g_entries[g_entry_count];
        size_t kw_len = (size_t)(p1 - line);
        size_t tool_len = (size_t)(p2 - (p1 + 1));
        size_t cat_len = (size_t)(p3 - (p2 + 1));

        if (kw_len >= sizeof(e->keywords)) kw_len = sizeof(e->keywords) - 1;
        if (tool_len >= sizeof(e->tool)) tool_len = sizeof(e->tool) - 1;
        if (cat_len >= sizeof(e->category)) cat_len = sizeof(e->category) - 1;

        memcpy(e->keywords, line, kw_len);
        e->keywords[kw_len] = '\0';
        memcpy(e->tool, p1 + 1, tool_len);
        e->tool[tool_len] = '\0';
        memcpy(e->category, p2 + 1, cat_len);
        e->category[cat_len] = '\0';
        safe_copy(e->description, sizeof(e->description), p3 + 1);

        g_entry_count++;
    }
    fclose(f);
    return 0;
}

static int icontains(const char *haystack, const char *needle) {
    if (needle[0] == '\0') return 0;
    size_t hlen = strlen(haystack), nlen = strlen(needle);
    if (nlen > hlen) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (strncasecmp(haystack + i, needle, nlen) == 0) {
            return 1;
        }
    }
    return 0;
}

typedef struct {
    int index;
    int score;
} match_t;

static int compare_matches(const void *a, const void *b) {
    const match_t *ma = (const match_t *)a;
    const match_t *mb = (const match_t *)b;
    if (mb->score != ma->score) return mb->score - ma->score;
    return ma->index - mb->index; /* stable-ish: earlier entries win ties */
}

static void print_entry(const entry_t *e) {
    const char *bold = g_use_color ? "\033[1m" : "";
    const char *dim = g_use_color ? "\033[90m" : "";
    const char *reset = g_use_color ? "\033[0m" : "";
    printf("  %s%-20s%s [%s] %s\n", bold, e->tool, reset, e->category, e->description);
    (void)dim;
}

static void search(const char *query) {
    char query_lc[512];
    safe_copy(query_lc, sizeof(query_lc), query);
    to_lower_inplace(query_lc);

    /* Split query into words. */
    char *words[32];
    int word_count = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(query_lc, " \t", &saveptr);
    while (tok && word_count < 32) {
        words[word_count++] = tok;
        tok = strtok_r(NULL, " \t", &saveptr);
    }

    if (word_count == 0) {
        return;
    }

    match_t matches[MAX_ENTRIES];
    int match_count = 0;

    for (int i = 0; i < g_entry_count; i++) {
        /* Copy into local fixed-size buffers first: GCC's format-
         * truncation checker can't bound a dynamically-indexed global
         * array access, so it otherwise assumes each %s could span the
         * entire g_entries array rather than just one entry's field. */
        char kw[sizeof(g_entries[0].keywords)];
        char tool[sizeof(g_entries[0].tool)];
        char desc[sizeof(g_entries[0].description)];
        safe_copy(kw, sizeof(kw), g_entries[i].keywords);
        safe_copy(tool, sizeof(tool), g_entries[i].tool);
        safe_copy(desc, sizeof(desc), g_entries[i].description);

        char haystack[sizeof(kw) + sizeof(tool) + sizeof(desc) + 4];
        snprintf(haystack, sizeof(haystack), "%s %s %s", kw, tool, desc);

        int score = 0;
        for (int w = 0; w < word_count; w++) {
            if (icontains(haystack, words[w])) {
                score++;
            }
        }
        if (score > 0) {
            matches[match_count].index = i;
            matches[match_count].score = score;
            match_count++;
        }
    }

    if (match_count == 0) {
        printf("  (no matches -- try fewer/different words, or `argos-toolfinder --list`)\n");
        return;
    }

    qsort(matches, (size_t)match_count, sizeof(match_t), compare_matches);

    int shown = match_count < MAX_RESULTS ? match_count : MAX_RESULTS;
    for (int i = 0; i < shown; i++) {
        print_entry(&g_entries[matches[i].index]);
    }
    if (match_count > shown) {
        printf("  ... and %d more match(es) (refine your search to narrow it down)\n",
               match_count - shown);
    }
}

static void list_by_category(void) {
    /* Simple O(n^2) grouping -- entry counts here are in the hundreds at
     * most, this runs once per invocation, not worth a hash table. */
    char seen_categories[64][48];
    int seen_count = 0;

    for (int i = 0; i < g_entry_count; i++) {
        int already = 0;
        for (int s = 0; s < seen_count; s++) {
            if (strcmp(seen_categories[s], g_entries[i].category) == 0) {
                already = 1;
                break;
            }
        }
        if (already || seen_count >= 64) continue;
        safe_copy(seen_categories[seen_count], sizeof(seen_categories[0]), g_entries[i].category);
        seen_count++;
    }

    for (int s = 0; s < seen_count; s++) {
        printf("%s%s%s\n", g_use_color ? "\033[1m" : "", seen_categories[s],
               g_use_color ? "\033[0m" : "");
        for (int i = 0; i < g_entry_count; i++) {
            if (strcmp(g_entries[i].category, seen_categories[s]) == 0) {
                print_entry(&g_entries[i]);
            }
        }
        printf("\n");
    }
}

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [word or phrase describing a task]\n"
        "       %s --list\n"
        "  no arguments: interactive mode (type queries, blank line or 'quit' to exit)\n",
        prog, prog);
}

int main(int argc, char **argv) {
    const char *manifest_path = MANIFEST_PATH_DEFAULT;

    if (!isatty(STDOUT_FILENO)) {
        g_use_color = 0;
    }

    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        return 0;
    }

    if (load_manifest(manifest_path) != 0) {
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "--list") == 0) {
        list_by_category();
        return 0;
    }

    if (argc > 1) {
        char query[512];
        query[0] = '\0';
        for (int i = 1; i < argc; i++) {
            if (i > 1) strncat(query, " ", sizeof(query) - strlen(query) - 1);
            strncat(query, argv[i], sizeof(query) - strlen(query) - 1);
        }
        search(query);
        return 0;
    }

    printf("ArgOS tool-finder -- type what you want to do (e.g. \"crack wifi password\"),\n");
    printf("blank line or 'quit' to exit. `argos-toolfinder --list` shows everything.\n\n");

    char line[512];
    for (;;) {
        printf("search> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }
        trim_newline(line);
        if (line[0] == '\0') {
            break;
        }
        if (strcasecmp(line, "quit") == 0 || strcasecmp(line, "exit") == 0) {
            break;
        }
        search(line);
        printf("\n");
    }

    return 0;
}
