/*
 * fileio.c — Persistence layer
 *
 * Data format (budget.csv):
 *   # lines starting with '#' are ignored (header / comments)
 *   id|date|amount|category|description|type
 *   1|2024-01-15|1500.00|Salary|Monthly salary|income
 *
 * Using '|' as the field separator avoids the need to quote commas that
 * may appear in description text.
 *
 * Config format (budget.cfg):
 *   next_id=5
 *   budget_limit=2000.00
 */

#define _POSIX_C_SOURCE 200809L   /* for rename(), fileno() on POSIX    */

#include "fileio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ── Internal helpers ────────────────────────────────────────────────── */

/*
 * next_field — Copy one pipe-delimited field from src into dst.
 *
 * Returns a pointer to the character after the '|' separator, or NULL
 * if this was the last field on the line.  Trailing CR/LF are stripped
 * from the final field.
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
        /* Last field: strip trailing whitespace / newline */
        len = strlen(src);
        while (len > 0 &&
               (src[len - 1] == '\n' || src[len - 1] == '\r' ||
                src[len - 1] == ' '))
        {
            len--;
        }
    }

    if (len >= dsz) len = dsz - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';

    return sep ? sep + 1 : NULL;
}

/*
 * grow_if_needed — Double the transactions array when full.
 *
 * Returns 0 on success, -1 if realloc failed.
 */
static int grow_if_needed(Budget *b)
{
    if (b->count < b->capacity) return 0;

    int new_cap = b->capacity * 2;
    Transaction *tmp = realloc(b->transactions,
                               (size_t)new_cap * sizeof(Transaction));
    if (!tmp) {
        fprintf(stderr, "Error: out of memory while growing transaction "
                        "array\n");
        return -1;
    }
    b->transactions = tmp;
    b->capacity     = new_cap;
    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────── */

int load_transactions(Budget *b, const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f) {
        /* Missing file is not an error — just an empty budget */
        if (errno != ENOENT) {
            fprintf(stderr, "Warning: cannot open '%s': %s\n",
                    filename, strerror(errno));
        }
        return 0;
    }

    char line[512];
    int  loaded = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Skip blank lines and comment/header lines */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        if (grow_if_needed(b) < 0) break;

        Transaction t;
        memset(&t, 0, sizeof(t));

        char id_str[16]     = {0};
        char amount_str[32] = {0};
        char type_str[16]   = {0};

        const char *p = line;

        /* Parse each field in order: id|date|amount|category|description|type */
        if (!(p = next_field(p, id_str,       sizeof(id_str))))       continue;
        if (!(p = next_field(p, t.date,       MAX_DATE_LEN)))         continue;
        if (!(p = next_field(p, amount_str,   sizeof(amount_str))))   continue;
        if (!(p = next_field(p, t.category,   MAX_CATEGORY_LEN)))     continue;
        if (!(p = next_field(p, t.description,MAX_DESC_LEN)))         continue;
        next_field(p, type_str, sizeof(type_str)); /* last field       */

        t.id     = atoi(id_str);
        t.amount = atof(amount_str);
        t.type   = (strcmp(type_str, "income") == 0) ? INCOME : EXPENSE;

        /* Basic sanity check */
        if (t.id <= 0 || t.amount < 0.0) continue;

        b->transactions[b->count++] = t;
        loaded++;
    }

    fclose(f);
    return loaded;
}

int save_transactions(const Budget *b, const char *filename)
{
    /*
     * Write to a temp file first, then atomically rename.
     * This prevents data corruption if the program is interrupted
     * mid-write.
     */
    char tmp_path[512];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", filename)
            >= (int)sizeof(tmp_path)) {
        fprintf(stderr, "Error: filename too long\n");
        return -1;
    }

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s' for writing: %s\n",
                tmp_path, strerror(errno));
        return -1;
    }

    fprintf(f, "# Budget Tracker Data — do not edit manually\n");
    fprintf(f, "# id|date|amount|category|description|type\n");

    for (int i = 0; i < b->count; i++) {
        const Transaction *t = &b->transactions[i];
        fprintf(f, "%d|%s|%.2f|%s|%s|%s\n",
                t->id,
                t->date,
                t->amount,
                t->category,
                t->description,
                t->type == INCOME ? "income" : "expense");
    }

    if (fflush(f) != 0 || ferror(f)) {
        fprintf(stderr, "Error: write failed on '%s': %s\n",
                tmp_path, strerror(errno));
        fclose(f);
        remove(tmp_path);
        return -1;
    }
    fclose(f);

    if (rename(tmp_path, filename) != 0) {
        fprintf(stderr, "Error: cannot rename '%s' → '%s': %s\n",
                tmp_path, filename, strerror(errno));
        remove(tmp_path);
        return -1;
    }

    return 0;
}

int load_config(Budget *b, const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f) return 0; /* not an error */

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        char key[32] = {0};
        char val[64] = {0};

        /* Format: key=value */
        if (sscanf(line, "%31[^=]=%63s", key, val) != 2) continue;

        if (strcmp(key, "next_id") == 0) {
            b->next_id = atoi(val);
        } else if (strcmp(key, "budget_limit") == 0) {
            b->budget_limit = atof(val);
        }
    }

    fclose(f);
    return 1;
}

int save_config(const Budget *b, const char *filename)
{
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "Error: cannot write config '%s': %s\n",
                filename, strerror(errno));
        return -1;
    }

    fprintf(f, "# Budget Tracker Config — do not edit manually\n");
    fprintf(f, "next_id=%d\n",       b->next_id);
    fprintf(f, "budget_limit=%.2f\n", b->budget_limit);

    fclose(f);
    return 0;
}
