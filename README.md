# Budget Tracker

A command-line personal finance tracker written in C.  
Track income and expenses, filter by category, view monthly summaries,  
and set a monthly spending limit — all from your terminal.

---

## Features

| Feature | Details |
|---|---|
| Add transactions | Interactive prompts with input validation |
| Delete / Edit | By transaction ID |
| List | All transactions in a formatted, color-coded table |
| Sort | By date or amount (ascending / descending) |
| Filter | Case-insensitive category search |
| Summary | Overall income, expenses, net balance |
| Monthly summary | Breakdown for any YYYY-MM month |
| Budget limit | Set a monthly expense cap — warns at 80 % and 100 % |
| Persistence | Data survives across runs via a pipe-delimited CSV |
| Atomic saves | Writes to a temp file then renames — no corrupt data on crash |
| Color output | ANSI colors when writing to a TTY; plain text when piped |

---

## File Structure

```
budget/
├── main.c       Entry point and command dispatcher
├── budget.h     Data structures and public API declarations
├── budget.c     Core budget logic (CRUD, display, sorting)
├── fileio.h     File I/O declarations
├── fileio.c     CSV and config file persistence
├── Makefile     Build system
└── README.md    This file
```

Data files created at runtime (in the current directory):

| File | Purpose |
|---|---|
| `budget.csv` | Transaction records |
| `budget.cfg` | Metadata: next ID, monthly budget limit |

---

## Compile

```bash
# Standard build
make

# Debug build with AddressSanitizer (catches memory errors)
make debug

# Check for memory leaks with valgrind
make valgrind

# Remove compiled files
make clean
```

Requires `gcc` on Linux.  No external libraries needed.

---

## Usage

```
./budget <command> [options]
```

### Commands

```bash
# Add a new transaction (interactive)
./budget add

# List all transactions
./budget list

# List sorted by date (oldest → newest)
./budget list --sort-date

# List sorted by date (newest → oldest)
./budget list --sort-date-desc

# List sorted by amount (lowest → highest)
./budget list --sort-amount

# List sorted by amount (highest → lowest)
./budget list --sort-amount-desc

# Delete transaction with ID 3
./budget delete 3

# Edit transaction with ID 5 (press Enter to keep any field)
./budget edit 5

# Filter by category (case-insensitive, partial match)
./budget filter Food
./budget filter salary

# Overall summary
./budget summary

# Summary for a specific month
./budget summary --month 2024-01
./budget summary 2024-03          # shorthand (no flag needed)

# Set a monthly expense limit of $2000
./budget setlimit 2000

# Remove the monthly limit
./budget setlimit 0

# Show help
./budget help
```

---

## Example Session

```
$ ./budget add

── Add Transaction ─────────────────────────────────────
  (Press Ctrl-D at any prompt to cancel.)

  Type [income/expense]: income
  Date (YYYY-MM-DD) [2024-03-15]: 2024-03-01
  Amount: 3500
  Category: Salary
  Description (optional): March paycheck

  ✓ Transaction #1 added.

$ ./budget add

  Type [income/expense]: expense
  Date (YYYY-MM-DD) [2024-03-15]:
  Amount: 85.40
  Category: Food
  Description (optional): Weekly groceries

  ✓ Transaction #2 added.

$ ./budget list

----+------------+---------+------------+--------------------+----------------------------------
 ID  | Date       | Type    |     Amount | Category           | Description
----+------------+---------+------------+--------------------+----------------------------------
   1 | 2024-03-01 | INCOME  |    3500.00 | Salary             | March paycheck
   2 | 2024-03-15 | EXPENSE |      85.40 | Food               | Weekly groceries
----+------------+---------+------------+--------------------+----------------------------------
  2 transaction(s)  |  Income: 3500.00  |  Expenses: 85.40  |  Net: +3414.60
----+------------+---------+------------+--------------------+----------------------------------

$ ./budget summary

╔══════════════════════════════════════╗
║       BUDGET SUMMARY                 ║
╚══════════════════════════════════════╝
  Total transactions : 2
  Total income       : +3500.00
  Total expenses     : -85.40
  ──────────────────────────────────────
  Net balance        : +3414.60

$ ./budget setlimit 1500
  ✓ Monthly budget limit set to 1500.00

$ ./budget summary --month 2024-03

── Monthly Summary: 2024-03 ──────────────────────────────
  Transactions : 2
  Income       : +3500.00
  Expenses     : -85.40
  ─────────────────────────────────────
  Net balance  : +3414.60
  Limit used   : 5.7% of 1500.00

$ ./budget delete 2
  Deleting: #2 — 85.40 (Food) "Weekly groceries"
  ✓ Deleted.
```

---

## Data Format

`budget.csv` (human-readable, pipe-delimited):

```
# Budget Tracker Data — do not edit manually
# id|date|amount|category|description|type
1|2024-03-01|3500.00|Salary|March paycheck|income
2|2024-03-15|85.40|Food|Weekly groceries|expense
```

`budget.cfg`:

```
# Budget Tracker Config — do not edit manually
next_id=3
budget_limit=1500.00
```

---

## Technical Highlights

- **Dynamic array** — transaction list grows automatically via `realloc`
- **Atomic file writes** — temp file + `rename()` prevents data corruption
- **Color detection** — uses `isatty()` so piped output is always clean
- **Input validation** — dates, amounts, and types are all checked
- **No memory leaks** — verified with Valgrind and AddressSanitizer
- **Modular design** — `fileio` and `budget` layers are fully separated

