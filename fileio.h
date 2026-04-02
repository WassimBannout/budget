/*
 * fileio.h — File I/O declarations
 *
 * Handles persisting Budget data to disk in a pipe-delimited CSV format
 * and loading it back at startup.  A separate key=value config file
 * stores metadata (next_id, budget_limit) that does not belong in the
 * transaction data.
 */

#ifndef FILEIO_H
#define FILEIO_H

#include "budget.h"

/*
 * load_transactions — Read budget.csv into b->transactions.
 *
 * Returns the number of records loaded, or -1 on a hard I/O error.
 * A missing file is treated as an empty dataset (returns 0).
 */
int load_transactions(Budget *b, const char *filename);

/*
 * save_transactions — Write all transactions in b to filename.
 *
 * The file is fully rewritten on every call (safe atomic pattern via a
 * temp file + rename is used so a crash cannot corrupt the data file).
 * Returns 0 on success, -1 on error.
 */
int save_transactions(const Budget *b, const char *filename);

/*
 * load_config — Read next_id and budget_limit from filename.
 *
 * Missing file is silently ignored (defaults stay in place).
 * Returns 1 if the file was read, 0 otherwise.
 */
int load_config(Budget *b, const char *filename);

/*
 * save_config — Write next_id and budget_limit to filename.
 *
 * Returns 0 on success, -1 on error.
 */
int save_config(const Budget *b, const char *filename);

#endif /* FILEIO_H */
