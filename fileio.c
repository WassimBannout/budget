/*
 * fileio.c — Persistence layer
 *
 * Data format (budget.csv):
 *   # lines starting with '#' are ignored
 *   id|date|amount|category|description|type
 *
 * Using '|' as the field separator avoids quoting commas in descriptions.
 *
 * Config format (budget.cfg):
 *   next_id=5
 *   budget_limit=2000.00
 *
 * Both files are written atomically via a temp file + rename so a crash
 * mid-write cannot corrupt stored data.
 */

#define _POSIX_C_SOURCE 200809L   /* rename() */

#include "fileio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ── Internal helpers ────────────────────────────────────────────────── */

/*
 * next_field — Copy one pipe-delimited field from src into dst (max dsz-1
 * bytes) and null-terminate.  Returns a pointer past the '|' separator,
 * or NULL when this was the last field on the line.
 * Trailing CR/LF are stripped from the final field.
 */
static const char *next_field(const char *src, char *dst, size_t dsz)
{
    if (!src || *src == '\0') {
        if (dst && dsz > 0) dst[0] = '\0';
        return NULL;
    }

    const char *sep = strchr(src, '|');
    size_t len;

    if (sep) {
        len = (size_t)(sep - src);
    } else {
        len = strlen(src);
        while (len > 0 && (src[len-1] == '\n' || src[len-1] == '\r' ||
                           src[len-1] == ' '))
            len--;
    }

    if (len >= dsz) len = dsz - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';

    return sep ? sep + 1 : NULL;
}

/*
 * atomic_fopen — Open a ".tmp" path for writing.
 * Writes the path into tmp_path (caller supplies buffer of size tmp_size).
 * Returns the open FILE *, or NULL on error.
 */
static FILE *atomic_fopen(const char *dest, char *tmp_path, size_t tmp_size)
{
    if (snprintf(tmp_path, tmp_size, "%s.tmp", dest) >= (int)tmp_size) {
        fprintf(stderr, "Error: filename too long\n");
        return NULL;
    }
    FILE *f = fopen(tmp_path, "w");
    if (!f)
        fprintf(stderr, "Error: cannot open '%s' for writing: %s\n",
                tmp_path, strerror(errno));
    return f;
}

/*
 * atomic_commit — Flush, close, and rename tmp_path → dest.
 * Returns 0 on success, -1 on error (tmp file is removed on failure).
 */
static int atomic_commit(FILE *f, const char *tmp_path, const char *dest)
{
    if (fflush(f) != 0 || ferror(f)) {
        fprintf(stderr, "Error: write failed on '%s': %s\n",
                tmp_path, strerror(errno));
        fclose(f);
        remove(tmp_path);
        return -1;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "Error: fclose failed on '%s': %s\n",
                tmp_path, strerror(errno));
        remove(tmp_path);
        return -1;
    }
    if (rename(tmp_path, dest) != 0) {
        fprintf(stderr, "Error: cannot rename '%s' → '%s': %s\n",
                tmp_path, dest, strerror(errno));
        remove(tmp_path);
        return -1;
    }
    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────── */

int load_transactions(Budget *b, const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f) {
        if (errno != ENOENT)
            fprintf(stderr, "Warning: cannot open '%s': %s\n",
                    filename, strerror(errno));
        return 0;
    }

    char line[512];
    int  loaded  = 0;
    int  max_id  = 0;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        if (budget_ensure_capacity(b) < 0) break;

        Transaction t;
        memset(&t, 0, sizeof(t));

        char id_str[16]     = {0};
        char amount_str[32] = {0};
        char type_str[16]   = {0};

        const char *p = line;
        if (!(p = next_field(p, id_str,         sizeof(id_str))))      continue;
        if (!(p = next_field(p, t.date,         MAX_DATE_LEN)))        continue;
        if (!(p = next_field(p, amount_str,     sizeof(amount_str))))  continue;
        if (!(p = next_field(p, t.category,     MAX_CATEGORY_LEN)))    continue;
        if (!(p = next_field(p, t.description,  MAX_DESC_LEN)))        continue;
        next_field(p, type_str, sizeof(type_str));

        t.id     = atoi(id_str);
        t.amount = atof(amount_str);
        t.type   = (strcmp(type_str, "income") == 0) ? INCOME : EXPENSE;

        if (t.id <= 0 || t.amount < 0.0) continue;

        b->transactions[b->count++] = t;
        if (t.id > max_id) max_id = t.id;
        loaded++;
    }

    fclose(f);

    /*
     * Guard against next_id being out of sync with the data file
     * (e.g. if budget.cfg was deleted but budget.csv survived).
     * Ensure next_id is always strictly greater than every existing ID.
     */
    if (max_id >= b->next_id)
        b->next_id = max_id + 1;

    return loaded;
}

int save_transactions(const Budget *b, const char *filename)
{
    char tmp_path[512];
    FILE *f = atomic_fopen(filename, tmp_path, sizeof(tmp_path));
    if (!f) return -1;

    fprintf(f, "# Budget Tracker Data — do not edit manually\n");
    fprintf(f, "# id|date|amount|category|description|type\n");

    for (int i = 0; i < b->count; i++) {
        const Transaction *t = &b->transactions[i];
        fprintf(f, "%d|%s|%.2f|%s|%s|%s\n",
                t->id, t->date, t->amount, t->category, t->description,
                t->type == INCOME ? "income" : "expense");
    }

    return atomic_commit(f, tmp_path, filename);
}

int load_config(Budget *b, const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f) return 0;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        char key[32] = {0};
        char val[64] = {0};
        if (sscanf(line, "%31[^=]=%63s", key, val) != 2) continue;

        if      (strcmp(key, "next_id")      == 0) b->next_id      = atoi(val);
        else if (strcmp(key, "budget_limit") == 0) b->budget_limit = atof(val);
    }

    fclose(f);
    return 1;
}

int save_config(const Budget *b, const char *filename)
{
    char tmp_path[512];
    FILE *f = atomic_fopen(filename, tmp_path, sizeof(tmp_path));
    if (!f) return -1;

    fprintf(f, "# Budget Tracker Config — do not edit manually\n");
    fprintf(f, "next_id=%d\n",        b->next_id);
    fprintf(f, "budget_limit=%.2f\n", b->budget_limit);

    return atomic_commit(f, tmp_path, filename);
}
