# Retained buffers and terminal mode

This fork starts at upstream commit
`254329d9f4f1df1158397080d403f3f112918aff`. Its editor changes were extracted
from the retained-buffer and terminal patches in JacksonDCollins/dotfiles.
They are ordinary source files here; development does not require regenerating
patches. The upstream license and history are retained.

The fork is public at https://github.com/JacksonDCollins/vis on branch
`retained-terminal`. `origin` points there; `upstream` remains
https://github.com/martanne/vis.git.

## Repository boundary

This repository owns the editor core, generic Lua APIs and matching Lua runtime.
Personal keymaps, plugins, nnn selection handling and installation/activation
remain in dotfiles. There is no nnn-specific protocol in the native code.

The dotfiles installer now ensures `vis-retained-terminal` is installed. If it
is missing, it uses a checkout beside dotfiles (`../vis`). If that path is absent,
it clones the latest `retained-terminal` branch of the public fork.
Existing paths/checkouts are never overwritten, reset or pulled automatically.
The package is built with makepkg as the user; sudo is only used by pacman.
Existing packages are not rebuilt automatically. To update a checkout, explicitly
review/fetch the desired changes on `retained-terminal` and run `makepkg -fsi`. Dotfiles no longer carries native source or
patch copies; this repository is their source of truth.

## Arch package

`PKGBUILD` builds `vis-retained-terminal` from this checkout, including tracked
modifications and non-ignored untracked files. It snapshots them into makepkg's
source directory without changing the checkout. It requires Git metadata: this
is a checkout-based development recipe, not a standalone AUR source recipe. Keep
custom `BUILDDIR` locations outside the checkout; the default `src/` and `pkg/`
locations and package archives are ignored. Bump `pkgrel` for package revisions.

On Arch, headers and libraries come together in the normal library packages;
separate `-dev` packages are not needed. Have the standard `base-devel` toolchain
installed, then run as your normal user from this repository:

```sh
makepkg -s       # install missing dependencies via pacman, then build
makepkg -si     # also offer to install the resulting package
```

Without `-s` or `-i`, `makepkg` only builds and fails if dependencies are missing.
Privileged dependency installation uses normal sudo/pacman authentication and
package verification; no signature or dependency checks are bypassed.

The package owns `/usr/bin/vis`, its helpers, `/usr/share/vis` (including lexers),
the `vis-direct` terminfo entry, man pages, license and documentation. It declares runtime library dependencies
and the ACL/ncurses ABI requirements advertised by Arch packages, and
provides/conflicts with both `vis` and `vis-lexers`. Lua, TRE and libvterm do not
currently advertise matching soname providers, so their package-name dependencies
do not automatically prevent ABI-breaking upgrades; rebuild when needed.
Review pacman's replacement/conflict prompts.

Normal PATH lookup runs the packaged `/usr/bin/vis`; dotfiles needs no launcher.
Dotfiles no longer builds a private bundle or lists this package's libraries as
standalone application dependencies. Old private bundles remain on disk for old
running sessions; do not delete them until those editors have exited. Packages
previously marked explicit stay explicit; installation does not automatically
reclassify them as dependencies.

A locally built package is not an automatic update source. Rebuild after source
or ABI changes; publish/sign versions through a package repository if automatic
updates are wanted later. Fresh dotfiles bootstrap clones follow the latest
`retained-terminal` branch; existing development checkouts intentionally build
their current files. For distributing binary packages,
build a reviewed clean revision and sign it under the target repository policy.
Never publish a package built from unintended untracked files.

## Build without installing

This variant currently requires Linux, a C compiler, make, pkg-config, Lua,
ncursesw (including `tic`), TRE and libvterm development files. The Lua runtime also uses LPeg
and lexers. No dependencies are downloaded by these commands.

```sh
./configure --enable-curses --enable-lua --enable-tre
make -j2 VERSION=v0.9-501-g254329d9-buffers-terminal-mode API=3234
```

This creates local executables; it does not install or activate them. Use the
matching `lua/` runtime, not a runtime from another vis build. Do not run
`make install` against system directories unless that is explicitly intended.
The explicit VERSION/API values above reproduce the extracted dotfiles build;
review them when changing the upstream base.

For sanitizer checks on the pinned upstream version, explicitly set
`TERMINFO=/usr/share/terminfo`. Its unchanged `external/unibilium.c` has an
out-of-bounds hexadecimal-directory fallback when an earlier lookup fails.
This workaround does not fix that upstream defect.

## True-color rendering

With extended-color ncurses, `COLORTERM=truecolor` or `COLORTERM=24bit` selects
24-bit rendering; an explicit `*-direct` TERM also requests it. The display uses
the installed `vis-direct` terminfo entry. Input decoding and the environment's
TERM retain their original values. This profile uses xterm-compatible control
sequences, suitable for Foot and tmux with RGB support enabled.

The profile keeps all RGB values (including black and near-black) separate from
ANSI palette indices. Themes and terminal buffers both preserve RGB colors;
default terminal colors and indexed colors remain supported. No palette
redefinition is needed in direct mode. Without the profile or a true-color request,
the existing indexed-color approximation/palette-redefinition path remains.

`make` builds the profile in `terminfo/`, and `make install` installs it under
`${SHAREPREFIX}/terminfo`. For a checkout build without installation, run with
`TERMINFO=/usr/share/terminfo TERMINFO_DIRS="$PWD/terminfo:/usr/share/terminfo"`
in addition to the matching Lua runtime. The standalone `vis-single` wrapper
requires an installed profile for direct color; its payload does not bundle it.

Color pairs use a bounded sparse cache rather than a table sized by the square of
the color count. Pair IDs are never recycled while curses cells reference them.
At most `min(COLOR_PAIRS, 32767) - 1` distinct foreground/background combinations
are allocated per editor process. If exhausted, new combinations use the default
pair instead of recoloring existing cells; restarting clears the cache.

## Retained buffers

- `:e filename`, Lua `win.file = filename`, and `:bnext` / `:bprev` switch files
  in the existing window. Its identity, layout, options and mappings survive.
- Hidden files retain text, undo/redo history and their last primary cursor.
  `:buffers` lists them; `:bdelete` removes the current buffer. `:bdelete!`
  discards changes or terminates a running terminal. Shared buffers must first
  be removed from their other splits before deletion.
- `:e` reloads the current file; `:e!` discards edits when reloading. Reloading
  a buffer displayed in multiple splits is refused.
- Closing a non-last split hides its buffer. Final unforced exit checks all
  buffers, including hidden ones, for unsaved changes and running terminals.
  Forced final exit discards remaining edits and stops managed terminal jobs.
- Switching files re-emits `WIN_OPEN` on the same window to refresh syntax and
  plugins, without a corresponding `WIN_CLOSE`. Handlers must tolerate repeat
  initialization. An inactive target window does not permanently steal focus.

This is memory-only retention, not crash recovery or cross-session persistence.
Multiple selections and viewport state are not saved as buffer history.

## Terminal buffers and Lua API

```lua
local file = vis:terminal('exec sh -i')
```

The call starts a terminal buffer in the focused split and returns immediately.
The command is trusted shell source executed through vis's configured shell;
quote interpolated data. Setup failure raises a Lua error. By default stdin,
stdout and stderr share a controlling PTY. The prior file remains retained.

`vis.modes.TERMINAL` is a distinct mode. Its mappings run before forwarding
unmapped keys to libvterm and the PTY. The built-in **Ctrl-backslash Ctrl-n**
mapping leaves terminal input without killing the process. Normal-mode motions
and yank operate on a read-only text snapshot; **i/a** resumes input. Visual or
multiple selections freeze that snapshot while the emulator continues updating.
The editor event loop services PTYs, captured output and existing subprocess
connections, without a separate blocking terminal loop.

Terminal processes belong to files, not windows or input modes. Closing a split
can hide a running terminal. Application exit leaves a read-only screen and exit
status; forced buffer deletion terminates the managed job.

Optional stdout capture and completion:

```lua
local file = vis:terminal(command, true, function(status, output, terminal)
    -- status: exit code, 128 + signal, or -1 on terminal/capture failure
    -- output: binary-safe captured stdout; empty if capture was false
    -- Return true to discard the completed buffer if already hidden.
end)
```

Capture requires an exit callback. Stdout uses an anonymous pipe; stdin and
stderr remain on the PTY. Capture is limited to 1 MiB; overflow stops the job
and reports -1. Descendant pipe writers cannot delay completion indefinitely.
Explicit buffer deletion and editor shutdown release the callback without
invoking it. No temporary files or named FIFOs implement capture.

Additional APIs:

- `file.terminal`: identifies a terminal buffer, including after process exit.
- `win.file = file`: restores a retained File object, including unnamed files.
- `vis:terminal_send('text<Enter>')`: sends vis key notation to the focused
  terminal, bypassing mappings. This is not a binary-write API.

The curses terminal view has colours; Normal/Visual snapshots are plain text.
There is no scrollback, mouse forwarding or bracketed-paste integration. Input
queues are capped at 64 KiB; screens at 1000 rows/columns and 262144 cells.
This is not full Neovim terminal parity.

## Validation and maintenance

Before publishing a revision, build the actual amalgamated `main.c`; standalone
checks of its included C fragments lack internal types and configuration.
Exercise mode transitions, mapped/unmapped keys, output while editing another
split, selection stability, timed undo grouping, resize, binary capture and its
limit, callback-driven buffer deletion, exit/forced cleanup and tty restoration.
Check the dotfiles nnn consumer and real LSP integration separately. No new
regression suite is introduced by this extraction.

Keep editor changes in focused commits. Merge or
rebase upstream deliberately and repeat validation; a fork does not remove
upstream compatibility work. Avoid mixing personal configuration into core
commits or changing package sources without verifying the replacement.
