# =============================================================================
# Makefile — Texty IDE
# =============================================================================
#
# Usage:
#   make          — build the editor (outputs ./texty)
#   make clean    — remove build artifacts
#   make run      — build and run with no arguments
#   make debug    — build with extra debug symbols (AddressSanitizer)
#
# Dependencies:
#   - gcc or clang
#   - ncurses (usually pre-installed on macOS and Linux)
#     Linux: sudo apt install libncurses-dev   (Debian/Ubuntu)
#            sudo dnf install ncurses-devel     (Fedora)
#     macOS: ships with the OS — no install needed
# =============================================================================

CC      = gcc
TARGET  = texty
SRCDIR  = src
OBJDIR  = obj

# --------------------------------------------------------------------------
# Compiler flags
#   -Wall -Wextra   : enable most useful warnings
#   -std=c99        : use the C99 standard
#   -g              : include debug symbols (needed for gdb/lldb)
#   -Isrc           : look in src/ for #include "..." headers
#   -D_POSIX_C_SOURCE=200809L : expose POSIX.1-2008 functions (strdup, etc.)
#       strdup() is not in the C99 standard, so GCC on Linux hides it from
#       <string.h> in strict C99 mode.  Without this define, the compiler
#       assumes strdup returns int (32 bits), which truncates the 64-bit
#       pointer and silently corrupts the undo stack at runtime.  macOS
#       always exposes strdup regardless of the C standard flag, which is
#       why this bug only appeared on Linux.
# --------------------------------------------------------------------------
CFLAGS  = -Wall -Wextra -std=c99 -g -Isrc -D_POSIX_C_SOURCE=200809L

# --------------------------------------------------------------------------
# Platform detection — link the right ncurses library
# --------------------------------------------------------------------------
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    # macOS — ncurses ships with the OS
    LIBS = -lncurses
endif

ifeq ($(UNAME_S),Linux)
    # Linux — install libncurses-dev if not present
    LIBS = -lncurses
endif

# Windows (MinGW/MSYS2) would use PDCurses instead, but is not yet supported.

# --------------------------------------------------------------------------
# Source and object file lists
# $(wildcard ...) finds all .c files; $(patsubst ...) maps them to .o paths
# --------------------------------------------------------------------------
SRCS := $(wildcard $(SRCDIR)/*.c)
OBJS := $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(SRCS))

# --------------------------------------------------------------------------
# Default target — build the editor
# --------------------------------------------------------------------------
.PHONY: all
all: $(TARGET)

# Link all object files into the final executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "Build complete: ./$(TARGET)"

# Compile each .c file into a .o file inside obj/
# The "| $(OBJDIR)" part means: make sure obj/ exists before compiling
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# Create the obj/ directory if it doesn't exist
$(OBJDIR):
	mkdir -p $(OBJDIR)

# --------------------------------------------------------------------------
# Debug build — AddressSanitizer catches memory bugs at runtime
# --------------------------------------------------------------------------
.PHONY: debug
debug: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
debug: LIBS   += -fsanitize=address,undefined
debug: $(TARGET)

# --------------------------------------------------------------------------
# Tests
#
# Each test file in tests/ compiles into its own binary linked against the
# source modules it exercises.  display.o, input.o, and main.o are excluded
# because they depend on ncurses or are entry points.
#
# Usage:
#   make test        — build and run all tests
#   make test-debug  — same but with AddressSanitizer
# --------------------------------------------------------------------------

TESTDIR   = tests
TESTOBJDIR = $(OBJDIR)/tests

# Source modules needed by tests (everything except main, display, input).
# display_stub.c provides fake implementations of display functions that
# editor.c calls (e.g. display_prompt), so we can link without ncurses.
TEST_DEPS = $(OBJDIR)/buffer.o $(OBJDIR)/undo.o $(OBJDIR)/editor.o \
            $(OBJDIR)/syntax.o $(OBJDIR)/filetree.o \
            $(TESTDIR)/display_stub.c

# One binary per test source file
TEST_SRCS = $(wildcard $(TESTDIR)/test_*.c)
TEST_BINS = $(patsubst $(TESTDIR)/%.c, $(TESTOBJDIR)/%, $(TEST_SRCS))

# Build the test object directory
$(TESTOBJDIR):
	mkdir -p $(TESTOBJDIR)

# Rule: compile one test binary from its .c file + shared object files
$(TESTOBJDIR)/%: $(TESTDIR)/%.c $(TEST_DEPS) | $(TESTOBJDIR)
	$(CC) $(CFLAGS) -I$(TESTDIR) $^ -o $@ $(LIBS)

.PHONY: test
test: $(TEST_BINS)
	@echo ""
	@failed=0; \
	for t in $(TEST_BINS); do \
	    ./$$t || failed=1; \
	done; \
	if [ $$failed -ne 0 ]; then \
	    echo "Some tests FAILED."; exit 1; \
	else \
	    echo "All test suites passed."; \
	fi

.PHONY: test-debug
test-debug: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
test-debug: LIBS   += -fsanitize=address,undefined
test-debug: test

# --------------------------------------------------------------------------
# Convenience targets
# --------------------------------------------------------------------------
.PHONY: run
run: all
	./$(TARGET)

.PHONY: clean
clean:
	rm -rf $(OBJDIR) $(TARGET)
	@echo "Cleaned."
