# GnuChanRunner

GnuChanRunner is an application launcher in the shape of rofi or dmenu: it
scans the freedesktop `.desktop` files, matches what you type against them, and
starts the one you pick. It is written in C against Xlib and Xft for the window
and the text. It is Debian-only, on purpose.

## Building

    git clone <repository-url> GnuChanRunner
    cd GnuChanRunner/src
    python3 makefile.py

The makefile installs the build dependencies with apt-get, compiles the sources,
and re-runs itself through sudo to write into `/usr/local/bin`. The binary goes
to `/usr/local/bin/GnuChanRunner`; the settings script goes under
`~/.config/GnuChanRunner/`. It is a separate program from the window manager and
installs on its own.

The makefile takes an action as its first argument: `install` (the default),
`build`, `run`, `list` and `uninstall`. `list` compiles and prints what the scan
finds without opening a window.

## Dependencies

The makefile probes for these headers and installs the matching package when one
is missing:

- `build-essential` (gcc)
- `pkg-config`
- `libx11-dev` — `X11/Xlib.h`
- `libxft-dev` — `X11/Xft/Xft.h`

Xft's own header includes freetype's, which comes with `libxft-dev`. The linking
is driven by pkg-config for the modules `x11` and `xft`.

## Starting it

The window manager starts the launcher from its own settings script, bound to
Alt+R:

    gcl_key.MultiKey(keys=["Mod1", "r"],
                     action=gcl_spawn.RunProgram(command="GnuChanRunner"))

If the window manager's settings script does not bind it, run `GnuChanRunner`
from a shell.

Programs whose desktop entries set `Terminal=true` run inside `GnuChanTerm`
when it is installed. Other terminal emulators are only used as fallbacks.
Add desktop-entry names or executable names to `TerminalPrograms` in the
GnuChanRunner settings to force any other program to start in GnuChanTerm.
For custom programs added with `GnuChanRunner.add_program`, set
`terminal=True` to get the same behavior:

    GnuChanRunner.add_program(name="System Monitor", command="GnuChanTop",
                              terminal=True)

## X11 only

This program is built directly against X11 (Xlib, Xft and the rest of the X
stack). It is designed for an X11 session and does not support Wayland: it was
never tested there, its window management assumes an X server, and no work has
been done to port it. On a Wayland session it may run through XWayland for basic
use, but that is not a supported configuration and behaviour there is undefined.
Run it on X11.

## License

GPL3.
