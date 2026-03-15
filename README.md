# texty

A terminal-based IDE written in C, built from scratch.

## Features

- Open and edit files from the command line
- Insert, delete, and newline editing
- Undo / redo (Ctrl+Z / Ctrl+Y) with per-buffer history
- Visual selection with Shift+Arrow keys
- Copy, cut, and paste (Ctrl+C / Ctrl+X / Ctrl+V)
- Multiple open buffers with a tab bar (Ctrl+N / Ctrl+O / Ctrl+W)
- Search and replace (Ctrl+F / F3 / Shift+F3 / Ctrl+R)
- Auto-indent on newline (matches current line's indentation)
- Tab key inserts 4 spaces (single undo step)
- Auto-close brackets and quotes — `(`, `[`, `{`, `"`, `'` insert the matching closing character
- Jump to line (Ctrl+G)
- Bracket match highlight — matching bracket shown in magenta
- Show/hide whitespace characters (F2)
- Word wrap toggle (F4)
- Arrow keys, Home/End, Page Up/Down, Ctrl+Home/End
- Save with Ctrl+S, quit with Ctrl+Q
- Line number gutter
- Status bar with filename, cursor position, and modified indicator
- Horizontal and vertical scrolling
- Terminal resize support
- Syntax highlighting for C, C++, Python, JavaScript, TypeScript, Rust, Go, JSON, Markdown, Shell, and Makefile
- Region highlight (Ctrl+U) — mark lines with a visible box border
- Split panes (F6 horizontal, F7 vertical) — view multiple files side by side
- File explorer panel (Ctrl+B) — browse, open, create, rename, and delete files

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
| Enter            | Insert newline (auto-indents)        |
| Tab              | Insert 4 spaces                      |
| `(` `[` `{`      | Auto-insert closing bracket          |
| `"` `'`          | Auto-insert closing quote            |
| Ctrl+G           | Jump to line number (prompts)        |
| Ctrl+Z           | Undo                                 |
| Ctrl+Y           | Redo                                 |

### View

| Key              | Action                               |
|------------------|--------------------------------------|
| F2               | Toggle whitespace characters         |
| F4               | Toggle word wrap                     |
| Ctrl+U           | Mark/clear region highlight          |
| F6               | Split pane horizontally (top/bottom) |
| F7               | Split pane vertically (left/right)   |

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
| Ctrl+]           | Next buffer                          |
| Ctrl+\           | Previous buffer                      |

### Search

| Key              | Action                                       |
|------------------|----------------------------------------------|
| Ctrl+F           | Find (prompts for search string)             |
| F3               | Find next match                              |
| Shift+F3         | Find previous match                          |
| Ctrl+R           | Replace all occurrences (prompts for both)   |
| Escape           | Clear search highlights                      |

> **Note:** Search is case-sensitive. Ctrl+H cannot be used for Replace because
> it maps to the same byte as Backspace in most terminals.

### File explorer

| Key              | Action                                          |
|------------------|-------------------------------------------------|
| Ctrl+B           | Open explorer and focus it                      |
| Ctrl+B           | Return focus to editor (panel stays open)       |
| Ctrl+B           | Focus explorer again (highlights current file)  |
| Escape           | Return focus to editor (panel stays open)       |
| Ctrl+W           | Close the explorer panel                        |
| Up / Down        | Move cursor                                     |
| Right            | Expand directory                                |
| Left             | Collapse directory                              |
| Enter            | Open file / toggle directory                    |
| `n`              | New file (prompts for name)                     |
| `N`              | New directory (prompts for name)                |
| `r`              | Rename entry (prompts for new name)             |
| `d`              | Delete entry (confirms before deleting)         |

> **Note:** Ctrl+B cycles through three states: explorer hidden → explorer
> focused → editor focused (panel visible). Opening a file that is already in
> an open buffer switches to that buffer instead of creating a duplicate.

### File

| Key              | Action                               |
|------------------|--------------------------------------|
| Ctrl+S           | Save                                 |
| Ctrl+Q           | Quit (confirm if unsaved)            |

> **Note:** Ctrl+] / Ctrl+\ for buffer switching use ASCII control characters
> (29 and 28) which are reliable across all terminal types.

## Project structure

```
src/
  main.c        — Entry point and event loop
  buffer.h/c    — Text buffer (array of lines)
  editor.h/c    — Editor state, cursor movement, text operations
  display.h/c   — ncurses terminal rendering
  input.h/c     — Keyboard input dispatch
  undo.h/c      — Undo / redo stack
  syntax.h/c    — Syntax highlighting (C, Python, JS, Rust, Go, and more)
  filetree.h/c  — File explorer tree logic
Makefile
TODO.md         — Phased development roadmap
```
