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
# --------------------------------------------------------------------------
CFLAGS  = -Wall -Wextra -std=c99 -g -Isrc

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
# Convenience targets
# --------------------------------------------------------------------------
.PHONY: run
run: all
	./$(TARGET)

.PHONY: clean
clean:
	rm -rf $(OBJDIR) $(TARGET)
	@echo "Cleaned."
