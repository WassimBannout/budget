/*
 * budget.c — Core budget logic
 *
 * Implements all CRUD operations, display/formatting routines,
 * monthly summaries, sorting, and the monthly budget-limit warning.
 *
 * Terminal colors are enabled only when stdout is a TTY so that
 * piped output (e.g. ./budget list | less) stays plain text.
 */

#define _POSIX_C_SOURCE 200809L   /* for isatty() */

#include "budget.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>   /* isatty(), STDOUT_FILENO */

/* ── ANSI color codes ───────────────────────────────────────────────── */
#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_RED     "\033[31m"
#define C_GREEN   "\033[32m"
#define C_YELLOW  "\033[33m"
#define C_CYAN    "\033[36m"

/* Resolved once in budget_init(); all display code reads this. */
static int colors_on = 0;

#define COL(code)  (colors_on ? (code)  : "")
#define CRESET     (colors_on ? C_RESET : "")

/* ── Shared capacity management (also called from fileio.c) ─────────── */

int budget_ensure_capacity(Budget *b)
{
    if (b->count < b->capacity) return 0;

    int new_cap = b->capacity * 2;
    Transaction *tmp = realloc(b->transactions,
                               (size_t)new_cap * sizeof(Transaction));
    if (!tmp) {
        fprintf(stderr, "Fatal: out of memory\n");
        return -1;
    }
    b->transactions = tmp;
    b->capacity     = new_cap;
    return 0;
}

/* ── Internal helpers ────────────────────────────────────────────────── */

/*
 * find_by_id — Linear scan; returns pointer into b->transactions or NULL.
 */
static Transaction *find_by_id(Budget *b, int id)
{
    for (int i = 0; i < b->count; i++) {
        if (b->transactions[i].id == id)
            return &b->transactions[i];
    }
    return NULL;
}

/*
 * str_tolower_n — Copy at most (max-1) chars of src into dst, lowercased,
 * and null-terminate.  Safe when dst and src are the same buffer.
 */
static void str_tolower_n(char *dst, const char *src, size_t max)
{
    size_t i;
    for (i = 0; i < max - 1 && src[i] != '\0'; i++)
        dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = '\0';
}

/*
 * parse_type_str — Map a (already lowercased) string to a TransactionType.
 * Returns 1 on success, 0 if the string is not recognised.
 * Separating parsing from prompting lets budget_edit reuse this without
 * duplicating the loop logic.
 */
static int parse_type_str(const char *s, TransactionType *out)
{
    if (strcmp(s, "i") == 0 || strcmp(s, "income") == 0) {
        *out = INCOME;
        return 1;
    }
    if (strcmp(s, "e") == 0 || strcmp(s, "expense") == 0) {
        *out = EXPENSE;
        return 1;
    }
    return 0;
}

/* ── Input helpers ───────────────────────────────────────────────────── */

/*
 * read_line — Display prompt, read a line from stdin into buf.
 *
 * Strips the trailing newline and trims leading/trailing whitespace.
 * Returns 1 on success, 0 on EOF (Ctrl-D).
 */
static int read_line(const char *prompt, char *buf, size_t size)
{
    printf("%s", prompt);
    fflush(stdout);

    if (!fgets(buf, (int)size, stdin)) {
        printf("\n");
        return 0;
    }

    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';

    /* Trim leading whitespace */
    size_t start = 0;
    while (start < len && isspace((unsigned char)buf[start])) start++;
    if (start > 0) {
        memmove(buf, buf + start, len - start + 1);
        len -= start;
    }

    /* Trim trailing whitespace */
    while (len > 0 && isspace((unsigned char)buf[len - 1])) buf[--len] = '\0';

    return 1;
}

/*
 * read_double_positive — Prompt and parse a positive monetary amount.
 * Loops until valid or EOF.
 */
static int read_double_positive(const char *prompt, double *out)
{
    char buf[64];
    while (1) {
        if (!read_line(prompt, buf, sizeof(buf))) return 0;
        if (buf[0] == '\0') {
            printf("  Amount cannot be empty.\n");
            continue;
        }
        char *end;
        double v = strtod(buf, &end);
        if (*end != '\0' || v <= 0.0) {
            printf("  Invalid amount. Enter a positive number (e.g. 42.50).\n");
            continue;
        }
        *out = v;
        return 1;
    }
}

/*
 * is_valid_date — Return 1 if s is a valid "YYYY-MM-DD" string.
 */
static int is_valid_date(const char *s)
{
    if (!s || strlen(s) != 10) return 0;
    if (s[4] != '-' || s[7] != '-') return 0;

    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (!isdigit((unsigned char)s[i])) return 0;
    }

    int year  = atoi(s);
    int month = atoi(s + 5);
    int day   = atoi(s + 8);

    if (year < 1900 || year > 2100) return 0;
    if (month < 1   || month > 12)  return 0;
    if (day < 1     || day > 31)    return 0;

    static const int mdays[] = {0,31,29,31,30,31,30,31,31,30,31,30,31};
    if (day > mdays[month]) return 0;

    return 1;
}

/*
 * read_date — Prompt for a date, defaulting to today on empty input.
 * Returns 1 on success, 0 on EOF.
 */
static int read_date(const char *prompt, char *out, size_t out_size)
{
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    char today[MAX_DATE_LEN];
    strftime(today, sizeof(today), "%Y-%m-%d", tm_now);

    char full_prompt[128];
    snprintf(full_prompt, sizeof(full_prompt), "%s [%s]: ", prompt, today);

    char buf[32];
    while (1) {
        if (!read_line(full_prompt, buf, sizeof(buf))) return 0;

        if (buf[0] == '\0') {
            snprintf(out, out_size, "%s", today);
            return 1;
        }
        if (!is_valid_date(buf)) {
            printf("  Invalid date. Use YYYY-MM-DD format.\n");
            continue;
        }
        /* is_valid_date guarantees exactly 10 chars */
        memcpy(out, buf, 10);
        out[10] = '\0';
        return 1;
    }
}

/*
 * read_type — Prompt until the user enters a valid type or EOF.
 * Returns 1 on success, 0 on EOF.
 */
static int read_type(const char *prompt, TransactionType *out)
{
    char buf[32];
    while (1) {
        if (!read_line(prompt, buf, sizeof(buf))) return 0;
        str_tolower_n(buf, buf, sizeof(buf));
        if (parse_type_str(buf, out)) return 1;
        printf("  Enter 'income' (or 'i') / 'expense' (or 'e').\n");
    }
}

/*
 * has_pipe — Return 1 if s contains the field separator character.
 * Fields with '|' would corrupt the CSV.
 */
static int has_pipe(const char *s)
{
    return strchr(s, '|') != NULL;
}

/* ── Display helpers ─────────────────────────────────────────────────── */

static void print_rule(void)
{
    printf("%s----+------------+---------+------------+--------------------"
           "+----------------------------------\n%s",
           COL(C_CYAN), CRESET);
}

static void print_header(void)
{
    print_rule();
    printf("%s %-3s%s | %s%-10s%s | %s%-7s%s | %s%10s%s | %s%-20s%s | %s%-32s%s\n",
           COL(C_BOLD), "ID",  CRESET,
           COL(C_BOLD), "Date", CRESET,
           COL(C_BOLD), "Type", CRESET,
           COL(C_BOLD), "Amount", CRESET,
           COL(C_BOLD), "Category", CRESET,
           COL(C_BOLD), "Description", CRESET);
    print_rule();
}

static void print_row(const Transaction *t)
{
    const char *col = (t->type == INCOME) ? COL(C_GREEN) : COL(C_RED);
    const char *str = (t->type == INCOME) ? "INCOME" : "EXPENSE";

    printf("%s %3d%s | %s | %s%-7s%s | %s%10.2f%s | %-20.20s | %-32.32s\n",
           COL(C_BOLD), t->id, CRESET,
           t->date,
           col, str, CRESET,
           col, t->amount, CRESET,
           t->category,
           t->description);
}

static void print_totals(double income, double expense, int count)
{
    double net = income - expense;
    const char *net_col = (net >= 0) ? COL(C_GREEN) : COL(C_RED);

    print_rule();
    printf("  %s%d transaction(s)%s  |  "
           "Income: %s%.2f%s  |  "
           "Expenses: %s%.2f%s  |  "
           "Net: %s%+.2f%s\n",
           COL(C_BOLD), count, CRESET,
           COL(C_GREEN), income,  CRESET,
           COL(C_RED),   expense, CRESET,
           net_col, net, CRESET);
    print_rule();
}

/* ── qsort comparators ───────────────────────────────────────────────── */

static int cmp_date_asc(const void *a, const void *b)
{
    return strcmp(((const Transaction *)a)->date,
                  ((const Transaction *)b)->date);
}

static int cmp_date_desc(const void *a, const void *b)
{
    return strcmp(((const Transaction *)b)->date,
                  ((const Transaction *)a)->date);
}

static int cmp_amount_asc(const void *a, const void *b)
{
    double da = ((const Transaction *)a)->amount;
    double db = ((const Transaction *)b)->amount;
    return (da > db) - (da < db);
}

static int cmp_amount_desc(const void *a, const void *b)
{
    double da = ((const Transaction *)a)->amount;
    double db = ((const Transaction *)b)->amount;
    return (db > da) - (db < da);
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

void budget_init(Budget *b)
{
    b->transactions = malloc(INITIAL_CAPACITY * sizeof(Transaction));
    if (!b->transactions) {
        fprintf(stderr, "Fatal: cannot allocate transaction array\n");
        exit(EXIT_FAILURE);
    }
    b->count        = 0;
    b->capacity     = INITIAL_CAPACITY;
    b->next_id      = 1;
    b->budget_limit = 0.0;

    colors_on = isatty(STDOUT_FILENO);
}

void budget_free(Budget *b)
{
    free(b->transactions);
    b->transactions = NULL;
    b->count        = 0;
    b->capacity     = 0;
}

/* ── CRUD ────────────────────────────────────────────────────────────── */

int budget_add(Budget *b)
{
    Transaction t;
    memset(&t, 0, sizeof(t));

    printf("\n%s── Add Transaction ─────────────────────────────────────%s\n",
           COL(C_CYAN), CRESET);
    printf("  (Press Ctrl-D at any prompt to cancel.)\n\n");

    if (!read_type("  Type [income/expense]: ", &t.type))
        goto cancelled;

    if (!read_date("  Date (YYYY-MM-DD)", t.date, MAX_DATE_LEN))
        goto cancelled;

    if (!read_double_positive("  Amount: ", &t.amount))
        goto cancelled;

    /* Category — required, validated for '|' immediately */
    while (1) {
        if (!read_line("  Category: ", t.category, MAX_CATEGORY_LEN))
            goto cancelled;
        if (t.category[0] == '\0') {
            printf("  Category cannot be empty.\n");
            continue;
        }
        if (has_pipe(t.category)) {
            printf("  Category may not contain '|'.\n");
            continue;
        }
        break;
    }

    /* Description — optional, validated for '|' immediately */
    while (1) {
        if (!read_line("  Description (optional): ", t.description, MAX_DESC_LEN))
            goto cancelled;
        if (has_pipe(t.description)) {
            printf("  Description may not contain '|'.\n");
            continue;
        }
        break;
    }
    if (t.description[0] == '\0')
        memcpy(t.description, "(no description)", 17);

    /* All input collected — grow array only if needed, then append */
    if (budget_ensure_capacity(b) < 0) return -1;

    t.id = b->next_id++;
    b->transactions[b->count++] = t;

    printf("\n  %s✓ Transaction #%d added.%s\n\n", COL(C_GREEN), t.id, CRESET);
    return 0;

cancelled:
    printf("  Cancelled.\n\n");
    return -1;
}

int budget_delete(Budget *b, int id)
{
    for (int i = 0; i < b->count; i++) {
        if (b->transactions[i].id != id) continue;

        printf("  Deleting: %s#%d%s — %s%.2f%s (%s) \"%s\"\n",
               COL(C_BOLD), id, CRESET,
               COL(C_YELLOW), b->transactions[i].amount, CRESET,
               b->transactions[i].category,
               b->transactions[i].description);

        /* Swap with last element for O(1) removal */
        b->transactions[i] = b->transactions[--b->count];

        printf("  %s✓ Deleted.%s\n\n", COL(C_GREEN), CRESET);
        return 0;
    }

    fprintf(stderr, "  %sError: no transaction with ID %d.%s\n\n",
            COL(C_RED), id, CRESET);
    return -1;
}

/*
 * budget_edit — Edit an existing transaction.
 *
 * Works on a *copy* of the stored transaction.  The original is only
 * replaced after all fields are successfully collected, so a mid-edit
 * cancellation leaves the stored data unchanged.
 */
int budget_edit(Budget *b, int id)
{
    Transaction *orig = find_by_id(b, id);
    if (!orig) {
        fprintf(stderr, "  %sError: no transaction with ID %d.%s\n\n",
                COL(C_RED), id, CRESET);
        return -1;
    }

    /* Work on a stack copy — only commit it back on full success */
    Transaction t = *orig;

    printf("\n%s── Edit Transaction #%d ─────────────────────────────────%s\n",
           COL(C_CYAN), id, CRESET);
    printf("  (Press Enter to keep current value, Ctrl-D to cancel.)\n\n");

    /* Type */
    {
        const char *cur = (t.type == INCOME) ? "income" : "expense";
        char prompt[64];
        snprintf(prompt, sizeof(prompt), "  Type [income/expense] (%s): ", cur);
        char buf[32];
        if (!read_line(prompt, buf, sizeof(buf))) goto cancelled;
        if (buf[0] != '\0') {
            str_tolower_n(buf, buf, sizeof(buf));
            TransactionType new_type;
            if (parse_type_str(buf, &new_type))
                t.type = new_type;
            else
                printf("  Invalid type — keeping '%s'.\n", cur);
        }
    }

    /* Date */
    {
        char prompt[64];
        snprintf(prompt, sizeof(prompt), "  Date (YYYY-MM-DD) (%s): ", t.date);
        char buf[32];
        if (!read_line(prompt, buf, sizeof(buf))) goto cancelled;
        if (buf[0] != '\0') {
            if (!is_valid_date(buf))
                printf("  Invalid date — keeping '%s'.\n", t.date);
            else {
                memcpy(t.date, buf, 10);
                t.date[10] = '\0';
            }
        }
    }

    /* Amount */
    {
        char prompt[64];
        snprintf(prompt, sizeof(prompt), "  Amount (%.2f): ", t.amount);
        char buf[64];
        if (!read_line(prompt, buf, sizeof(buf))) goto cancelled;
        if (buf[0] != '\0') {
            char *end;
            double v = strtod(buf, &end);
            if (*end != '\0' || v <= 0.0)
                printf("  Invalid amount — keeping %.2f.\n", t.amount);
            else
                t.amount = v;
        }
    }

    /* Category */
    {
        char prompt[128];
        snprintf(prompt, sizeof(prompt), "  Category (%s): ", t.category);
        char buf[MAX_CATEGORY_LEN];
        if (!read_line(prompt, buf, sizeof(buf))) goto cancelled;
        if (buf[0] != '\0') {
            if (has_pipe(buf))
                printf("  Category may not contain '|' — keeping '%s'.\n",
                       t.category);
            else
                snprintf(t.category, MAX_CATEGORY_LEN, "%s", buf);
        }
    }

    /* Description */
    {
        char prompt[MAX_DESC_LEN + 32];
        snprintf(prompt, sizeof(prompt), "  Description (%s): ", t.description);
        char buf[MAX_DESC_LEN];
        if (!read_line(prompt, buf, sizeof(buf))) goto cancelled;
        if (buf[0] != '\0') {
            if (has_pipe(buf))
                printf("  Description may not contain '|' — keeping original.\n");
            else
                snprintf(t.description, MAX_DESC_LEN, "%s", buf);
        }
    }

    /* All fields accepted — commit the copy back to the stored record */
    *orig = t;

    printf("\n  %s✓ Transaction #%d updated.%s\n\n", COL(C_GREEN), id, CRESET);
    return 0;

cancelled:
    printf("  Cancelled.\n\n");
    return -1;
}

/* ── Display ─────────────────────────────────────────────────────────── */

void budget_list(const Budget *b, SortOrder sort)
{
    if (b->count == 0) {
        printf("\n  %sNo transactions found.%s\n\n", COL(C_YELLOW), CRESET);
        return;
    }

    Transaction *copy = malloc((size_t)b->count * sizeof(Transaction));
    if (!copy) {
        fprintf(stderr, "Error: out of memory\n");
        return;
    }
    memcpy(copy, b->transactions, (size_t)b->count * sizeof(Transaction));

    typedef int (*cmp_fn)(const void *, const void *);
    static const cmp_fn cmps[] = {
        [SORT_DATE_ASC]    = cmp_date_asc,
        [SORT_DATE_DESC]   = cmp_date_desc,
        [SORT_AMOUNT_ASC]  = cmp_amount_asc,
        [SORT_AMOUNT_DESC] = cmp_amount_desc,
    };
    if (sort != SORT_NONE)
        qsort(copy, (size_t)b->count, sizeof(Transaction), cmps[sort]);

    printf("\n");
    print_header();

    double income = 0.0, expense = 0.0;
    for (int i = 0; i < b->count; i++) {
        print_row(&copy[i]);
        if (copy[i].type == INCOME) income  += copy[i].amount;
        else                        expense += copy[i].amount;
    }

    print_totals(income, expense, b->count);
    printf("\n");
    free(copy);
}

void budget_filter(const Budget *b, const char *category)
{
    char search[MAX_CATEGORY_LEN];
    str_tolower_n(search, category, sizeof(search));

    printf("\n%s── Transactions in category: \"%s%s%s\" ─────────────────%s\n\n",
           COL(C_CYAN), COL(C_BOLD), category, COL(C_CYAN), CRESET);
    print_header();

    int    found   = 0;
    double income  = 0.0, expense = 0.0;

    char cat[MAX_CATEGORY_LEN];
    for (int i = 0; i < b->count; i++) {
        str_tolower_n(cat, b->transactions[i].category, sizeof(cat));
        if (!strstr(cat, search)) continue;

        print_row(&b->transactions[i]);
        if (b->transactions[i].type == INCOME) income  += b->transactions[i].amount;
        else                                   expense += b->transactions[i].amount;
        found++;
    }

    if (found == 0) {
        printf("  %sNo transactions found for category \"%s\".%s\n",
               COL(C_YELLOW), category, CRESET);
        print_rule();
    } else {
        print_totals(income, expense, found);
    }
    printf("\n");
}

void budget_summary(const Budget *b)
{
    double income = 0.0, expense = 0.0;
    for (int i = 0; i < b->count; i++) {
        if (b->transactions[i].type == INCOME) income  += b->transactions[i].amount;
        else                                   expense += b->transactions[i].amount;
    }
    double net = income - expense;

    printf("\n%s╔══════════════════════════════════════╗%s\n", COL(C_CYAN), CRESET);
    printf("%s║         BUDGET SUMMARY               ║%s\n", COL(C_CYAN), CRESET);
    printf("%s╚══════════════════════════════════════╝%s\n", COL(C_CYAN), CRESET);
    printf("  Total transactions : %s%d%s\n",    COL(C_BOLD),  b->count, CRESET);
    printf("  Total income       : %s+%.2f%s\n", COL(C_GREEN), income,   CRESET);
    printf("  Total expenses     : %s-%.2f%s\n", COL(C_RED),   expense,  CRESET);
    printf("  ──────────────────────────────────────\n");
    printf("  Net balance        : %s%+.2f%s\n\n",
           net >= 0 ? COL(C_GREEN) : COL(C_RED), net, CRESET);

    if (b->budget_limit > 0.0)
        printf("  Monthly limit      : %.2f\n\n", b->budget_limit);
}

void budget_monthly_summary(const Budget *b, const char *month)
{
    if (!month || strlen(month) != 7 || month[4] != '-') {
        fprintf(stderr, "  Error: month must be YYYY-MM (e.g. 2024-01).\n\n");
        return;
    }

    double income = 0.0, expense = 0.0;
    int    count  = 0;

    for (int i = 0; i < b->count; i++) {
        if (strncmp(b->transactions[i].date, month, 7) != 0) continue;
        if (b->transactions[i].type == INCOME) income  += b->transactions[i].amount;
        else                                   expense += b->transactions[i].amount;
        count++;
    }

    printf("\n%s── Monthly Summary: %s%s%s ──────────────────────────────%s\n",
           COL(C_CYAN), COL(C_BOLD), month, COL(C_CYAN), CRESET);

    if (count == 0) {
        printf("  %sNo transactions for %s.%s\n\n", COL(C_YELLOW), month, CRESET);
        return;
    }

    double net = income - expense;
    printf("  Transactions : %d\n", count);
    printf("  Income       : %s+%.2f%s\n", COL(C_GREEN), income,  CRESET);
    printf("  Expenses     : %s-%.2f%s\n", COL(C_RED),   expense, CRESET);
    printf("  ─────────────────────────────────────\n");
    printf("  Net balance  : %s%+.2f%s\n",
           net >= 0 ? COL(C_GREEN) : COL(C_RED), net, CRESET);

    if (b->budget_limit > 0.0) {
        double pct = (expense / b->budget_limit) * 100.0;
        printf("  Limit used   : %s%.1f%%%s of %.2f\n",
               pct >= 100.0 ? COL(C_RED) : COL(C_YELLOW),
               pct, CRESET, b->budget_limit);
    }
    printf("\n");
}

/* ── Budget limit ────────────────────────────────────────────────────── */

void budget_setlimit(Budget *b, double limit)
{
    if (limit < 0.0) {
        fprintf(stderr, "  Error: limit must be >= 0 (use 0 to disable).\n");
        return;
    }
    b->budget_limit = limit;
    if (limit == 0.0)
        printf("  %s✓ Monthly budget limit removed.%s\n\n",  COL(C_GREEN), CRESET);
    else
        printf("  %s✓ Monthly budget limit set to %.2f%s\n\n", COL(C_GREEN), limit, CRESET);
}

void budget_check_limit(const Budget *b)
{
    if (b->budget_limit <= 0.0) return;

    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    char cur_month[8];
    strftime(cur_month, sizeof(cur_month), "%Y-%m", tm_now);

    double expense = 0.0;
    for (int i = 0; i < b->count; i++) {
        if (b->transactions[i].type == EXPENSE &&
            strncmp(b->transactions[i].date, cur_month, 7) == 0)
        {
            expense += b->transactions[i].amount;
        }
    }

    double pct = (expense / b->budget_limit) * 100.0;

    if (expense > b->budget_limit) {
        fprintf(stderr,
                "\n  %s⚠  BUDGET LIMIT EXCEEDED!%s\n"
                "  Spent %.2f of %.2f limit (%.1f%% — over by %.2f)\n\n",
                COL(C_RED), CRESET,
                expense, b->budget_limit, pct, expense - b->budget_limit);
    } else if (pct >= 80.0) {
        fprintf(stderr,
                "\n  %s⚠  Budget warning:%s Spent %.2f of %.2f "
                "(%.1f%% of monthly limit)\n\n",
                COL(C_YELLOW), CRESET, expense, b->budget_limit, pct);
    }
}
