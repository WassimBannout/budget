/*
 * main.c — Entry point and command dispatcher
 *
 * Parses argv, loads data, dispatches to the appropriate budget
 * function, saves data, then frees memory cleanly.
 *
 * Usage:
 *   ./budget add
 *   ./budget list [--sort-date|--sort-date-desc|--sort-amount|--sort-amount-desc]
 *   ./budget delete <id>
 *   ./budget edit <id>
 *   ./budget filter <category>
 *   ./budget summary
 *   ./budget summary --month YYYY-MM
 *   ./budget setlimit <amount>
 *   ./budget help
 */

#include "budget.h"
#include "fileio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Usage / help ────────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    printf("\nUsage: %s <command> [options]\n\n", prog);
    printf("Commands:\n");
    printf("  add                        Interactively add a transaction\n");
    printf("  list                       List all transactions\n");
    printf("  list --sort-date           Sort by date (oldest first)\n");
    printf("  list --sort-date-desc      Sort by date (newest first)\n");
    printf("  list --sort-amount         Sort by amount (lowest first)\n");
    printf("  list --sort-amount-desc    Sort by amount (highest first)\n");
    printf("  delete <id>                Delete a transaction by ID\n");
    printf("  edit <id>                  Edit a transaction by ID\n");
    printf("  filter <category>          Filter transactions by category\n");
    printf("  summary                    Show overall income/expense summary\n");
    printf("  summary --month YYYY-MM    Show summary for a specific month\n");
    printf("  setlimit <amount>          Set monthly expense limit (0 to disable)\n");
    printf("  help                       Show this help message\n");
    printf("\nData files are stored in the current directory:\n");
    printf("  %-20s Transaction records\n", DATA_FILE);
    printf("  %-20s Configuration (next ID, budget limit)\n\n", CONFIG_FILE);
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

/*
 * parse_id — Convert a command-line argument to a positive integer ID.
 * Prints an error and returns -1 if the argument is missing or invalid.
 */
static int parse_id(int argc, char *argv[], int arg_index)
{
    if (arg_index >= argc) {
        fprintf(stderr, "Error: missing transaction ID.\n");
        return -1;
    }
    int id = atoi(argv[arg_index]);
    if (id <= 0) {
        fprintf(stderr, "Error: ID must be a positive integer (got \"%s\").\n",
                argv[arg_index]);
        return -1;
    }
    return id;
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    /* ── Initialise ──────────────────────────────────────────────────── */
    Budget b;
    budget_init(&b);

    /* Load persisted config first so next_id / budget_limit are correct */
    load_config(&b, CONFIG_FILE);

    /* Load existing transactions (missing file → empty set, not an error) */
    load_transactions(&b, DATA_FILE);

    /* ── Dispatch ────────────────────────────────────────────────────── */
    const char *cmd = argv[1];
    int ret = EXIT_SUCCESS;

    if (strcmp(cmd, "add") == 0) {

        if (budget_add(&b) == 0) {
            if (save_transactions(&b, DATA_FILE) < 0 ||
                save_config(&b, CONFIG_FILE)     < 0)
            {
                ret = EXIT_FAILURE;
            } else {
                budget_check_limit(&b);
            }
        }

    } else if (strcmp(cmd, "delete") == 0) {

        int id = parse_id(argc, argv, 2);
        if (id < 0) {
            ret = EXIT_FAILURE;
        } else if (budget_delete(&b, id) == 0) {
            if (save_transactions(&b, DATA_FILE) < 0 ||
                save_config(&b, CONFIG_FILE)     < 0)
                ret = EXIT_FAILURE;
        } else {
            ret = EXIT_FAILURE;
        }

    } else if (strcmp(cmd, "edit") == 0) {

        int id = parse_id(argc, argv, 2);
        if (id < 0) {
            ret = EXIT_FAILURE;
        } else if (budget_edit(&b, id) == 0) {
            if (save_transactions(&b, DATA_FILE) < 0 ||
                save_config(&b, CONFIG_FILE)     < 0)
                ret = EXIT_FAILURE;
        } else {
            ret = EXIT_FAILURE;
        }

    } else if (strcmp(cmd, "list") == 0) {

        SortOrder sort = SORT_NONE;
        if (argc >= 3) {
            if      (strcmp(argv[2], "--sort-date")         == 0) sort = SORT_DATE_ASC;
            else if (strcmp(argv[2], "--sort-date-desc")    == 0) sort = SORT_DATE_DESC;
            else if (strcmp(argv[2], "--sort-amount")       == 0) sort = SORT_AMOUNT_ASC;
            else if (strcmp(argv[2], "--sort-amount-desc")  == 0) sort = SORT_AMOUNT_DESC;
            else {
                fprintf(stderr,
                        "Warning: unknown sort option \"%s\" — ignored.\n",
                        argv[2]);
            }
        }
        budget_list(&b, sort);

    } else if (strcmp(cmd, "filter") == 0) {

        if (argc < 3) {
            fprintf(stderr, "Usage: %s filter <category>\n", argv[0]);
            ret = EXIT_FAILURE;
        } else {
            budget_filter(&b, argv[2]);
        }

    } else if (strcmp(cmd, "summary") == 0) {

        if (argc >= 4 && strcmp(argv[2], "--month") == 0) {
            budget_monthly_summary(&b, argv[3]);
        } else if (argc >= 3 && argv[2][0] != '-') {
            /* Allow: ./budget summary 2024-01 (no --month flag) */
            budget_monthly_summary(&b, argv[2]);
        } else {
            budget_summary(&b);
        }

    } else if (strcmp(cmd, "setlimit") == 0) {

        if (argc < 3) {
            fprintf(stderr, "Usage: %s setlimit <amount>\n", argv[0]);
            ret = EXIT_FAILURE;
        } else {
            char *end;
            double limit = strtod(argv[2], &end);
            if (*end != '\0' || limit < 0.0) {
                fprintf(stderr,
                        "Error: limit must be a non-negative number "
                        "(use 0 to disable).\n");
                ret = EXIT_FAILURE;
            } else {
                budget_setlimit(&b, limit);
                if (save_config(&b, CONFIG_FILE) < 0)
                    ret = EXIT_FAILURE;
            }
        }

    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
               strcmp(cmd, "-h") == 0) {

        print_usage(argv[0]);

    } else {

        fprintf(stderr, "Error: unknown command \"%s\".\n", cmd);
        print_usage(argv[0]);
        ret = EXIT_FAILURE;

    }

    /* ── Clean up ────────────────────────────────────────────────────── */
    budget_free(&b);
    return ret;
}
