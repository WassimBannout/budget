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
#include <math.h>
#include <unistd.h>   /* isatty(), STDOUT_FILENO */

/* ── ANSI color codes ───────────────────────────────────────────────── */
#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_RED     "\033[31m"
#define C_GREEN   "\033[32m"
#define C_YELLOW  "\033[33m"
#define C_BLUE    "\033[34m"
#define C_MAGENTA "\033[35m"
#define C_CYAN    "\033[36m"
#define C_WHITE   "\033[37m"

/* Resolved at runtime; see budget_init() */
static int colors_on = 0;

#define COL(code)  (colors_on ? (code)  : "")
#define CRESET     (colors_on ? C_RESET : "")

/* ── Internal helpers ────────────────────────────────────────────────── */

/*
 * grow_budget — Double the capacity of b->transactions.
 * Returns 0 on success, -1 on allocation failure.
 */
static int grow_budget(Budget *b)
{
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

/*
 * find_by_id — Linear scan for a transaction with the given ID.
 * Returns a pointer into b->transactions, or NULL if not found.
 */
static Transaction *find_by_id(Budget *b, int id)
{
    for (int i = 0; i < b->count; i++) {
        if (b->transactions[i].id == id)
            return &b->transactions[i];
    }
    return NULL;
}

/* ── Input helpers ───────────────────────────────────────────────────── */

/*
 * read_line — Display prompt, read a line from stdin into buf.
 *
 * Strips the trailing newline and trims leading/trailing whitespace.
 * Returns 1 on success, 0 on EOF (Ctrl-D) — caller should treat 0 as
 * "operation cancelled".
 */
static int read_line(const char *prompt, char *buf, size_t size)
{
    printf("%s", prompt);
    fflush(stdout);

    if (!fgets(buf, (int)size, stdin)) {
        printf("\n");
        return 0; /* EOF / Ctrl-D */
    }

    /* Strip newline */
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
 * Loops until the user enters a valid positive number, or returns 0
 * on EOF.
 */
static int read_double_positive(const char *prompt, double *out)
{
    char buf[64];
    while (1) {
        if (!read_line(prompt, buf, sizeof(buf))) return 0;
        if (buf[0] == '\0') {
            printf("  Amount cannot be empty. Please try again.\n");
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
 * is_valid_date — Return 1 if s is a valid "YYYY-MM-DD" date string.
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

    /* Days-per-month check (simplified: ignores leap years for day 29-31) */
    static const int mdays[] = {0,31,29,31,30,31,30,31,31,30,31,30,31};
    if (day > mdays[month]) return 0;

    return 1;
}

/*
 * read_date — Prompt for a date, defaulting to today if the user
 * presses Enter.  Loops until a valid YYYY-MM-DD is entered.
 * Returns 1 on success, 0 on EOF.
 */
static int read_date(const char *prompt, char *out, size_t out_size)
{
    /* Build today's date for the default hint */
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    char today[MAX_DATE_LEN];
    strftime(today, sizeof(today), "%Y-%m-%d", tm_now);

    char full_prompt[128];
    snprintf(full_prompt, sizeof(full_prompt), "%s [%s]: ", prompt, today);

    char buf[32];
    while (1) {
        if (!read_line(full_prompt, buf, sizeof(buf))) return 0;

        /* Empty input → use today */
        if (buf[0] == '\0') {
            snprintf(out, out_size, "%s", today);
            return 1;
        }

        if (!is_valid_date(buf)) {
            printf("  Invalid date. Use YYYY-MM-DD format.\n");
            continue;
        }

        /* is_valid_date guarantees exactly 10 chars; copy directly */
        memcpy(out, buf, 10);
        out[10] = '\0';
        return 1;
    }
}

/*
 * read_type — Prompt for "income" or "expense".
 * Accepts i/I/income and e/E/expense.
 * Returns 1 on success, 0 on EOF.
 */
static int read_type(const char *prompt, TransactionType *out)
{
    char buf[32];
    while (1) {
        if (!read_line(prompt, buf, sizeof(buf))) return 0;

        /* Lower-case the input */
        for (size_t i = 0; buf[i]; i++)
            buf[i] = (char)tolower((unsigned char)buf[i]);

        if (strcmp(buf, "i") == 0 || strcmp(buf, "income") == 0) {
            *out = INCOME;
            return 1;
        }
        if (strcmp(buf, "e") == 0 || strcmp(buf, "expense") == 0) {
            *out = EXPENSE;
            return 1;
        }
        printf("  Enter 'income' (or 'i') / 'expense' (or 'e').\n");
    }
}

/* ── Display helpers ─────────────────────────────────────────────────── */

/* Print a horizontal rule spanning the table width */
static void print_rule(void)
{
    printf("%s", COL(C_CYAN));
    printf("----+------------+---------+------------+--------------------"
           "+----------------------------------\n");
    printf("%s", CRESET);
}

/* Print the column header row */
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

/* Print a single transaction row with appropriate color */
static void print_row(const Transaction *t)
{
    const char *type_col  = (t->type == INCOME) ? COL(C_GREEN) : COL(C_RED);
    const char *type_str  = (t->type == INCOME) ? "INCOME"     : "EXPENSE";

    printf("%s %3d%s | %s | %s%-7s%s | %s%10.2f%s | %-20.20s | %-32.32s\n",
           COL(C_BOLD), t->id, CRESET,
           t->date,
           type_col, type_str, CRESET,
           type_col, t->amount, CRESET,
           t->category,
           t->description);
}

/* Print summary totals at the bottom of a list */
static void print_totals(double income, double expense, int count)
{
    double net = income - expense;
    print_rule();
    printf("  %s%d transaction(s)%s  |  "
           "Income: %s%.2f%s  |  "
           "Expenses: %s%.2f%s  |  "
           "Net: %s%.2f%s\n",
           COL(C_BOLD), count, CRESET,
           COL(C_GREEN), income,  CRESET,
           COL(C_RED),   expense, CRESET,
           net >= 0 ? COL(C_GREEN) : COL(C_RED), net, CRESET);
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
        fprintf(stderr, "Fatal: cannot allocate initial transaction array\n");
        exit(EXIT_FAILURE);
    }
    b->count        = 0;
    b->capacity     = INITIAL_CAPACITY;
    b->next_id      = 1;
    b->budget_limit = 0.0;

    /* Enable colors only when writing to a real terminal */
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

/*
 * budget_add — Interactively prompt the user for all fields and append
 * a new transaction.  Returns 0 on success, -1 if the user cancelled
 * (EOF) or a memory error occurred.
 */
int budget_add(Budget *b)
{
    if (b->count >= b->capacity && grow_budget(b) < 0) return -1;

    Transaction t;
    memset(&t, 0, sizeof(t));

    printf("\n%s── Add Transaction ─────────────────────────────────────%s\n",
           COL(C_CYAN), CRESET);
    printf("  (Press Ctrl-D at any prompt to cancel.)\n\n");

    /* Type */
    if (!read_type("  Type [income/expense]: ", &t.type)) {
        printf("  Cancelled.\n\n");
        return -1;
    }

    /* Date */
    if (!read_date("  Date (YYYY-MM-DD)", t.date, MAX_DATE_LEN)) {
        printf("  Cancelled.\n\n");
        return -1;
    }

    /* Amount */
    if (!read_double_positive("  Amount: ", &t.amount)) {
        printf("  Cancelled.\n\n");
        return -1;
    }

    /* Category */
    while (1) {
        if (!read_line("  Category: ", t.category, MAX_CATEGORY_LEN)) {
            printf("  Cancelled.\n\n");
            return -1;
        }
        if (t.category[0] != '\0') break;
        printf("  Category cannot be empty.\n");
    }

    /* Description (optional) */
    if (!read_line("  Description (optional): ", t.description, MAX_DESC_LEN)) {
        printf("  Cancelled.\n\n");
        return -1;
    }
    if (t.description[0] == '\0')
        strncpy(t.description, "(no description)", MAX_DESC_LEN - 1);

    /* Check for '|' in fields — would corrupt the CSV */
    if (strchr(t.category, '|') || strchr(t.description, '|')) {
        fprintf(stderr,
                "Error: fields may not contain the '|' character.\n");
        return -1;
    }

    t.id = b->next_id++;
    b->transactions[b->count++] = t;

    printf("\n  %s✓ Transaction #%d added.%s\n\n",
           COL(C_GREEN), t.id, CRESET);
    return 0;
}

/*
 * budget_delete — Remove the transaction with the given ID.
 * Uses a swap-with-last strategy for O(1) removal.
 * Returns 0 on success, -1 if not found.
 */
int budget_delete(Budget *b, int id)
{
    for (int i = 0; i < b->count; i++) {
        if (b->transactions[i].id == id) {
            /* Print what we're deleting */
            printf("  Deleting: %s#%d%s — %s%.2f%s (%s) \"%s\"\n",
                   COL(C_BOLD), id, CRESET,
                   COL(C_YELLOW), b->transactions[i].amount, CRESET,
                   b->transactions[i].category,
                   b->transactions[i].description);

            /* Swap with last, shrink count */
            b->transactions[i] = b->transactions[b->count - 1];
            b->count--;

            printf("  %s✓ Deleted.%s\n\n", COL(C_GREEN), CRESET);
            return 0;
        }
    }

    fprintf(stderr, "  %sError: no transaction with ID %d.%s\n\n",
            COL(C_RED), id, CRESET);
    return -1;
}

/*
 * budget_edit — Interactively edit an existing transaction.
 * Press Enter at any prompt to keep the existing value.
 * Returns 0 on success, -1 if not found or cancelled.
 */
int budget_edit(Budget *b, int id)
{
    Transaction *t = find_by_id(b, id);
    if (!t) {
        fprintf(stderr, "  %sError: no transaction with ID %d.%s\n\n",
                COL(C_RED), id, CRESET);
        return -1;
    }

    printf("\n%s── Edit Transaction #%d ─────────────────────────────────%s\n",
           COL(C_CYAN), id, CRESET);
    printf("  (Press Enter to keep current value, Ctrl-D to cancel.)\n\n");

    /* Type */
    {
        const char *cur = (t->type == INCOME) ? "income" : "expense";
        char prompt[64];
        snprintf(prompt, sizeof(prompt), "  Type [income/expense] (%s): ", cur);
        char buf[32];
        if (!read_line(prompt, buf, sizeof(buf))) {
            printf("  Cancelled.\n\n");
            return -1;
        }
        if (buf[0] != '\0') {
            TransactionType new_type;
            /* Reuse read_type logic inline */
            for (size_t i = 0; buf[i]; i++)
                buf[i] = (char)tolower((unsigned char)buf[i]);
            if (strcmp(buf, "i") == 0 || strcmp(buf, "income") == 0)
                new_type = INCOME;
            else if (strcmp(buf, "e") == 0 || strcmp(buf, "expense") == 0)
                new_type = EXPENSE;
            else {
                printf("  Invalid type — keeping '%s'.\n", cur);
                new_type = t->type;
            }
            t->type = new_type;
        }
    }

    /* Date */
    {
        char prompt[64];
        snprintf(prompt, sizeof(prompt),
                 "  Date (YYYY-MM-DD) (%s): ", t->date);
        char buf[32];
        if (!read_line(prompt, buf, sizeof(buf))) {
            printf("  Cancelled.\n\n");
            return -1;
        }
        if (buf[0] != '\0') {
            if (!is_valid_date(buf)) {
                printf("  Invalid date — keeping '%s'.\n", t->date);
            } else {
                strncpy(t->date, buf, MAX_DATE_LEN - 1);
                t->date[MAX_DATE_LEN - 1] = '\0';
            }
        }
    }

    /* Amount */
    {
        char prompt[64];
        snprintf(prompt, sizeof(prompt), "  Amount (%.2f): ", t->amount);
        char buf[64];
        if (!read_line(prompt, buf, sizeof(buf))) {
            printf("  Cancelled.\n\n");
            return -1;
        }
        if (buf[0] != '\0') {
            char *end;
            double v = strtod(buf, &end);
            if (*end != '\0' || v <= 0.0)
                printf("  Invalid amount — keeping %.2f.\n", t->amount);
            else
                t->amount = v;
        }
    }

    /* Category */
    {
        char prompt[128];
        snprintf(prompt, sizeof(prompt),
                 "  Category (%s): ", t->category);
        char buf[MAX_CATEGORY_LEN];
        if (!read_line(prompt, buf, sizeof(buf))) {
            printf("  Cancelled.\n\n");
            return -1;
        }
        if (buf[0] != '\0') {
            if (strchr(buf, '|')) {
                printf("  Category may not contain '|' — keeping '%s'.\n",
                       t->category);
            } else {
                strncpy(t->category, buf, MAX_CATEGORY_LEN - 1);
                t->category[MAX_CATEGORY_LEN - 1] = '\0';
            }
        }
    }

    /* Description */
    {
        char prompt[320];
        snprintf(prompt, sizeof(prompt),
                 "  Description (%s): ", t->description);
        char buf[MAX_DESC_LEN];
        if (!read_line(prompt, buf, sizeof(buf))) {
            printf("  Cancelled.\n\n");
            return -1;
        }
        if (buf[0] != '\0') {
            if (strchr(buf, '|')) {
                printf("  Description may not contain '|' — keeping original.\n");
            } else {
                strncpy(t->description, buf, MAX_DESC_LEN - 1);
                t->description[MAX_DESC_LEN - 1] = '\0';
            }
        }
    }

    printf("\n  %s✓ Transaction #%d updated.%s\n\n",
           COL(C_GREEN), id, CRESET);
    return 0;
}

/* ── Display ─────────────────────────────────────────────────────────── */

/*
 * budget_list — Print all transactions, optionally sorted.
 * Sorting is done on a heap copy so the stored order is unchanged.
 */
void budget_list(const Budget *b, SortOrder sort)
{
    if (b->count == 0) {
        printf("\n  %sNo transactions found.%s\n\n", COL(C_YELLOW), CRESET);
        return;
    }

    /* Build a working copy for sorting */
    Transaction *copy = malloc((size_t)b->count * sizeof(Transaction));
    if (!copy) {
        fprintf(stderr, "Error: out of memory\n");
        return;
    }
    memcpy(copy, b->transactions, (size_t)b->count * sizeof(Transaction));

    switch (sort) {
        case SORT_DATE_ASC:    qsort(copy, b->count, sizeof(Transaction), cmp_date_asc);    break;
        case SORT_DATE_DESC:   qsort(copy, b->count, sizeof(Transaction), cmp_date_desc);   break;
        case SORT_AMOUNT_ASC:  qsort(copy, b->count, sizeof(Transaction), cmp_amount_asc);  break;
        case SORT_AMOUNT_DESC: qsort(copy, b->count, sizeof(Transaction), cmp_amount_desc); break;
        default: break;
    }

    printf("\n");
    print_header();

    double total_income  = 0.0;
    double total_expense = 0.0;

    for (int i = 0; i < b->count; i++) {
        print_row(&copy[i]);
        if (copy[i].type == INCOME)  total_income  += copy[i].amount;
        else                          total_expense += copy[i].amount;
    }

    print_totals(total_income, total_expense, b->count);
    printf("\n");

    free(copy);
}

/*
 * budget_filter — Show only transactions whose category matches
 * the given string (case-insensitive substring match).
 */
void budget_filter(const Budget *b, const char *category)
{
    /* Build a lower-cased copy of the search term */
    char search[MAX_CATEGORY_LEN];
    size_t slen = strlen(category);
    if (slen >= MAX_CATEGORY_LEN) slen = MAX_CATEGORY_LEN - 1;
    for (size_t i = 0; i < slen; i++)
        search[i] = (char)tolower((unsigned char)category[i]);
    search[slen] = '\0';

    int   found   = 0;
    double income  = 0.0;
    double expense = 0.0;

    printf("\n%s── Transactions in category: \"%s%s%s\" ─────────────────%s\n\n",
           COL(C_CYAN),
           COL(C_BOLD), category, COL(C_CYAN),
           CRESET);
    print_header();

    for (int i = 0; i < b->count; i++) {
        /* Lower-case the stored category for comparison */
        char cat[MAX_CATEGORY_LEN];
        size_t clen = strlen(b->transactions[i].category);
        if (clen >= MAX_CATEGORY_LEN) clen = MAX_CATEGORY_LEN - 1;
        for (size_t j = 0; j < clen; j++)
            cat[j] = (char)tolower((unsigned char)b->transactions[i].category[j]);
        cat[clen] = '\0';

        if (strstr(cat, search) != NULL) {
            print_row(&b->transactions[i]);
            if (b->transactions[i].type == INCOME) income  += b->transactions[i].amount;
            else                                    expense += b->transactions[i].amount;
            found++;
        }
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

/*
 * budget_summary — Overall income / expenses / net balance.
 */
void budget_summary(const Budget *b)
{
    double total_income  = 0.0;
    double total_expense = 0.0;

    for (int i = 0; i < b->count; i++) {
        if (b->transactions[i].type == INCOME)
            total_income  += b->transactions[i].amount;
        else
            total_expense += b->transactions[i].amount;
    }

    double net = total_income - total_expense;

    printf("\n%s╔══════════════════════════════════════╗%s\n", COL(C_CYAN), CRESET);
    printf("%s║       BUDGET SUMMARY                 ║%s\n", COL(C_CYAN), CRESET);
    printf("%s╚══════════════════════════════════════╝%s\n", COL(C_CYAN), CRESET);
    printf("  Total transactions : %s%d%s\n",    COL(C_BOLD), b->count, CRESET);
    printf("  Total income       : %s+%.2f%s\n", COL(C_GREEN), total_income,  CRESET);
    printf("  Total expenses     : %s-%.2f%s\n", COL(C_RED),   total_expense, CRESET);
    printf("  ──────────────────────────────────────\n");

    const char *net_col = (net >= 0) ? COL(C_GREEN) : COL(C_RED);
    const char *net_sign = (net >= 0) ? "+" : "";
    printf("  Net balance        : %s%s%.2f%s\n\n",
           net_col, net_sign, net, CRESET);

    if (b->budget_limit > 0.0) {
        printf("  Monthly limit      : %.2f\n", b->budget_limit);
    }
}

/*
 * budget_monthly_summary — Summarise a specific calendar month.
 * month must be "YYYY-MM".
 */
void budget_monthly_summary(const Budget *b, const char *month)
{
    if (!month || strlen(month) != 7 || month[4] != '-') {
        fprintf(stderr, "  Error: month must be in YYYY-MM format "
                        "(e.g. 2024-01).\n\n");
        return;
    }

    double income  = 0.0;
    double expense = 0.0;
    int    count   = 0;

    for (int i = 0; i < b->count; i++) {
        /* Compare YYYY-MM prefix of date */
        if (strncmp(b->transactions[i].date, month, 7) == 0) {
            if (b->transactions[i].type == INCOME)
                income  += b->transactions[i].amount;
            else
                expense += b->transactions[i].amount;
            count++;
        }
    }

    double net = income - expense;

    printf("\n%s── Monthly Summary: %s%s%s ──────────────────────────────%s\n",
           COL(C_CYAN), COL(C_BOLD), month, COL(C_CYAN), CRESET);

    if (count == 0) {
        printf("  %sNo transactions for %s.%s\n\n",
               COL(C_YELLOW), month, CRESET);
        return;
    }

    printf("  Transactions : %d\n", count);
    printf("  Income       : %s+%.2f%s\n", COL(C_GREEN), income,  CRESET);
    printf("  Expenses     : %s-%.2f%s\n", COL(C_RED),   expense, CRESET);
    printf("  ─────────────────────────────────────\n");

    const char *net_col  = (net >= 0) ? COL(C_GREEN) : COL(C_RED);
    const char *net_sign = (net >= 0) ? "+" : "";
    printf("  Net balance  : %s%s%.2f%s\n", net_col, net_sign, net, CRESET);

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
        printf("  %s✓ Monthly budget limit removed.%s\n\n",
               COL(C_GREEN), CRESET);
    else
        printf("  %s✓ Monthly budget limit set to %.2f%s\n\n",
               COL(C_GREEN), limit, CRESET);
}

/*
 * budget_check_limit — Compare current-month expenses to budget_limit.
 * Prints a warning to stderr if the limit is exceeded or nearly so.
 */
void budget_check_limit(const Budget *b)
{
    if (b->budget_limit <= 0.0) return;

    /* Get current month string "YYYY-MM" */
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
                expense, b->budget_limit, pct,
                expense - b->budget_limit);
    } else if (pct >= 80.0) {
        fprintf(stderr,
                "\n  %s⚠  Budget warning:%s Spent %.2f of %.2f "
                "(%.1f%% of monthly limit)\n\n",
                COL(C_YELLOW), CRESET,
                expense, b->budget_limit, pct);
    }
}
