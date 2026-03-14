# texty

A terminal-based IDE written in C, built from scratch.

## Features

- Open and edit files from the command line
- Insert, delete, and newline editing
- Undo / redo (Ctrl+Z / Ctrl+Y) with per-buffer history
- Visual selection with Shift+Arrow keys
- Copy, cut, and paste (Ctrl+C / Ctrl+X / Ctrl+V)
- Multiple open buffers with a tab bar (Ctrl+N / Ctrl+O / Ctrl+W)
- Arrow keys, Home/End, Page Up/Down, Ctrl+Home/End
- Save with Ctrl+S, quit with Ctrl+Q
- Line number gutter
- Status bar with filename, cursor position, and modified indicator
- Horizontal and vertical scrolling
- Terminal resize support

## Requirements

- macOS or Linux
- ncurses (`libncurses-dev` on Debian/Ubuntu, `ncurses-devel` on Fedora)

## Build

```sh
make
```

Produces the `./texty` binary.

## Usage

```sh
./texty [filename]
```

## Key bindings

### Navigation

| Key              | Action                      |
|------------------|-----------------------------|
| Arrow keys       | Move cursor                 |
| Home / End       | Start / end of line         |
| Page Up / Down   | Scroll one screen           |
| Ctrl+Home        | Jump to top of file         |
| Ctrl+End         | Jump to bottom of file      |

### Editing

| Key              | Action                               |
|------------------|--------------------------------------|
| Backspace        | Delete character before cursor       |
| Delete           | Delete character at cursor           |
| Enter            | Insert newline                       |
| Ctrl+Z           | Undo                                 |
| Ctrl+Y           | Redo                                 |

### Selection & clipboard

| Key              | Action                               |
|------------------|--------------------------------------|
| Shift+Arrow      | Extend selection                     |
| Ctrl+A           | Select all                           |
| Ctrl+C           | Copy selection                       |
| Ctrl+X           | Cut selection                        |
| Ctrl+V           | Paste                                |

### Buffers

| Key              | Action                               |
|------------------|--------------------------------------|
| Ctrl+N           | New empty buffer                     |
| Ctrl+O           | Open file (prompts for path)         |
| Ctrl+W           | Close current buffer (confirm if unsaved) |
| Ctrl+Right       | Next buffer                          |
| Ctrl+Left        | Previous buffer                      |

### File

| Key              | Action                               |
|------------------|--------------------------------------|
| Ctrl+S           | Save                                 |
| Ctrl+Q           | Quit (confirm if unsaved)            |

> **Note:** Ctrl+Right / Ctrl+Left for buffer switching use key codes for
> xterm-256color terminals (macOS Terminal, iTerm2). If they don't respond,
> run `cat -v` and press the key to find your terminal's code.

## Project structure

```
src/
  main.c      — Entry point and event loop
  buffer.h/c  — Text buffer (array of lines)
  editor.h/c  — Editor state, cursor movement, text operations
  display.h/c — ncurses terminal rendering
  input.h/c   — Keyboard input dispatch
  undo.h/c    — Undo / redo stack
Makefile
TODO.md       — Phased development roadmap
```
