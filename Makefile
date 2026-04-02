# ── Budget Tracker Makefile ──────────────────────────────────────────────
#
# Targets:
#   make          — build the 'budget' executable (default)
#   make debug    — build with AddressSanitizer and debug symbols
#   make clean    — remove compiled objects and the binary
#   make valgrind — run a quick memory-leak check via valgrind
#
# Requires: gcc, (optional) valgrind

CC      = gcc
TARGET  = budget
SRCS    = main.c budget.c fileio.c
OBJS    = $(SRCS:.c=.o)

# Production flags — strict warnings, C11, optimise
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic \
          -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
          -O2

# Link math library (used for fabs / math.h includes)
LDFLAGS = -lm

# ── Default target ───────────────────────────────────────────────────────
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build successful → ./$(TARGET)"

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ── Debug build (AddressSanitizer + full debug symbols) ──────────────────
debug: CFLAGS += -g3 -fsanitize=address,undefined -fno-omit-frame-pointer -O0
debug: LDFLAGS += -fsanitize=address,undefined
debug: $(TARGET)

# ── Memory-leak test with valgrind ───────────────────────────────────────
valgrind: $(TARGET)
	valgrind --leak-check=full --error-exitcode=1 \
	         ./$(TARGET) list

# ── Clean ────────────────────────────────────────────────────────────────
clean:
	rm -f $(OBJS) $(TARGET)
	@echo "Cleaned."

# Prevent make from confusing targets with file names
.PHONY: all debug valgrind clean
