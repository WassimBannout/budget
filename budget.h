/*
 * budget.h — Core data structures and function declarations
 *
 * Defines the Transaction and Budget types, enumerations for
 * transaction types and sort orders, and the full public API
 * for budget operations.
 */

#ifndef BUDGET_H
#define BUDGET_H

#include <stddef.h>

/* ── Field size limits ──────────────────────────────────────────────── */
#define MAX_DATE_LEN      11    /* "YYYY-MM-DD\0"                        */
#define MAX_CATEGORY_LEN  64
#define MAX_DESC_LEN      256

/* ── Default file paths (relative to cwd) ───────────────────────────── */
#define DATA_FILE         "budget.csv"
#define CONFIG_FILE       "budget.cfg"

/* ── Initial dynamic array capacity ────────────────────────────────── */
#define INITIAL_CAPACITY  16

/* ── Transaction type ───────────────────────────────────────────────── */
typedef enum {
    INCOME  = 0,
    EXPENSE = 1
} TransactionType;

/* ── Sort order for budget_list ─────────────────────────────────────── */
typedef enum {
    SORT_NONE         = 0,
    SORT_DATE_ASC     = 1,
    SORT_DATE_DESC    = 2,
    SORT_AMOUNT_ASC   = 3,
    SORT_AMOUNT_DESC  = 4
} SortOrder;

/* ── Single transaction record ──────────────────────────────────────── */
typedef struct {
    int             id;
    char            date[MAX_DATE_LEN];         /* "YYYY-MM-DD"          */
    double          amount;                     /* always positive       */
    char            category[MAX_CATEGORY_LEN];
    char            description[MAX_DESC_LEN];
    TransactionType type;
} Transaction;

/* ── Top-level budget container ─────────────────────────────────────── */
typedef struct {
    Transaction *transactions;  /* heap-allocated, grows as needed       */
    int          count;         /* number of active transactions         */
    int          capacity;      /* allocated slots                       */
    int          next_id;       /* auto-increment ID counter             */
    double       budget_limit;  /* monthly expense limit; 0 = unlimited  */
} Budget;

/* ── Lifecycle ───────────────────────────────────────────────────────── */
void budget_init(Budget *b);
void budget_free(Budget *b);

/* ── CRUD ────────────────────────────────────────────────────────────── */
int  budget_add(Budget *b);
int  budget_delete(Budget *b, int id);
int  budget_edit(Budget *b, int id);

/* ── Display ─────────────────────────────────────────────────────────── */
void budget_list(const Budget *b, SortOrder sort);
void budget_filter(const Budget *b, const char *category);
void budget_summary(const Budget *b);
void budget_monthly_summary(const Budget *b, const char *month); /* "YYYY-MM" */

/* ── Budget limit ────────────────────────────────────────────────────── */
void budget_setlimit(Budget *b, double limit);
void budget_check_limit(const Budget *b);

#endif /* BUDGET_H */
