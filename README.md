# texty

A terminal-based IDE written in C, built from scratch.

## Features (Phase 1)

- Open and edit files from the command line
- Insert, delete, and newline editing
- Arrow keys, Home/End, Page Up/Down, Ctrl+Home/End
- Save with Ctrl+S, quit with Ctrl+Q
- Line number gutter
- Status bar with filename, position, and modified indicator
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
./texty filename.txt
```

| Key         | Action                     |
|-------------|----------------------------|
| Ctrl+S      | Save                       |
| Ctrl+Q      | Quit (confirm if unsaved)  |
| Arrow keys  | Move cursor                |
| Home / End  | Start / end of line        |
| Page Up/Dn  | Scroll one screen          |
| Ctrl+Home   | Jump to top of file        |
| Ctrl+End    | Jump to bottom of file     |
| Backspace   | Delete character before cursor |
| Delete      | Delete character at cursor |

## Project structure

```
src/
  main.c      — Entry point and event loop
  buffer.h/c  — Text buffer (array of lines)
  editor.h/c  — Editor state, cursor movement, text operations
  display.h/c — ncurses terminal rendering
  input.h/c   — Keyboard input dispatch
Makefile
TODO.md       — Phased development roadmap
```
