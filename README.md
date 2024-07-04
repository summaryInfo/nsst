Not So Simple Terminal
======================
This is an implementation of VT220-like X11/Wayland terminal emulator.

## Features
* Quite fast rendering
    * Almost same latency as `XTerm`, which is a lot lower than for other modern terminals
    * Scrolling performance is on par with fastest terminals on my system (`alacritty` and `urxvt`)
* Small size and almost no dependencies
* Uses xcb for X11 and libwayland for Wayland directly,
    * A little bit faster because of its asynchrony
    * `size` including all loaded shared libs is only 80% of `st` on my system for X11
* Most escape sequences from XTerm are implemented
* Full keyboard mode from XTerm
* Synchronous updates DCS
* `OSC 13001 ; Po ST` "Set background opacity to `Po` (`Po` is floating point)"
* Daemon mode (`urxvt`-like)
    * Multiple terminal windows
    * `Shift-Ctrl-N` is default keybinding to create new window in the same process
    * `nsstc` client that can create new terminal daemon
    * Daemon can be auto-launched by `nsstc` on demand (`nsstc -d ...`)
* Configuration with symmetrical config file options and command line arguments
* X11 MIT-SHM, X11 XRender and Wayland `wl_shm` backends
* Compiles with `-flto` by default
* No warnings (see list of all enabled warnings in Makefile)
* Re-wraps text on resize
* URL support (including autodetection)
* Command line integration (correct wrapping after command output without newline and jumping between commands)
* Can copy tab characters

See TODO file for things that are to be implemented.

## Source structure

* `boxdraw.c` -- Boxdrawing characters rendering
* `config.c` -- Configuration handling and storage
* `daemon.c` -- Daemon code
* `font.c` -- Font loading and glyph caching
* `image.c` -- Image manipulation utilities
* `input.c` -- Input handling code (more or less compatible with Xterm)
* `line.c` -- Terminal lines manipulation functions
* `mouse.c` -- Mouse events and selection handling
* `multipool.c` -- Allocator implementation optimized for lines allocation
* `nrcs.c` -- National replacement character sets logic
* `nsstc.c` -- Thin client that is able to connect to nsst daemon
* `nsst.c` -- `main` function and arguments parsing
* `poller.c` -- Event handling (`poll()` wrapper)
* `render-xrender-x11.c` -- X11 XRender backend
* `render-shm.c` -- common software rendening code
* `render-shm-x11.c` -- X11 MIT-SHM backend
* `render-shm-wayland.c` -- Wayland `wl_shm` backend
* `screen.c` -- Screen manipulation routines
* `term.c` -- Terminal logic
* `tty.c` -- Low level TTY/PTY code
* `uri.c` -- URL storage and validation
* `util.c` -- General utilities (encoding/decoding and logging)
* `window.c` -- Common window code
* `window-x11.c` -- X11 specific window code
* `window-wayland.c` -- Wayland specific window code

## Notes

Nsst uses `TERM=xterm` by default for higher compatibility. This is fine since almost every escape sequence from [ctlseqs.ms](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html) is supported.
See Emulation section of TODO for not yet implemented escape sequences. This repository contains native terminfo (`nsst.info`), which contains several extensions, like curly
underlines and synchronous updates. It is installed by default and can be selected if desired by setting `term-name=nsst` or `term-name=nsst-direct` in the config file. The latter uses 24-bit colors.

Works well with [Iosevka](https://github.com/be5invis/Iosevka) font. (Set font spacing to -1 it it feels to wide.)
Multiple fonts could be loaded by enumerating them in parameter like:

font=Iosevka-13,MaterialDesignIcons-13

Set `override-boxdrawing` to `true` if box drawing characters of font you use does not align.

If font looks to blurry try setting `font-gamma` to value greater than `1`.

Set pixelMode to your monitor's pixel alignment to enable sub-pixel rendering.

By default only DEC Special Graphics charset is allowed with UTF-8 mode enabled.
Spec is even stricter, disallowing any charset translations in UTF-8 mode, but DEC Special Graphics is used by applications frequently so it is allowed anyways.
To force full NRCS translation in UTF-8 mode set `force-nrsc`.

Sequential graphical characters are decoded all at once, so can be printed faster.

Debugging output can be enabled with `trace-*` options.

For now nsst supports combining characters only via precomposition, but almost everything is ready to implement proper rendering of combining character (and variant glyphs support).
The only tricky part is to extract positioning tables and implement basic text shaping. It would be implemented using glyphs with codes `0x110000` - `0x1FFFFF`,
giving sufficient number of possible custom glyphs. DECDLD is also easy to implement this way.

## Options

For command line arguments see `nsst --help`.
Config file uses same option names, just without leading `--`.
Default config file location is `$XDG_CONFIG_HOME/nsst.conf` or `$XDG_CONFIG_HOME/nsst/nsst.conf`.
Config file path can be set via `--config`/`-C` argument.
For boolean arguments `--no-X`, `--without-X`, `--disable-X` are interpreted as `--X=0` and
`--X`, `--with-X`, `--enable-X` are interpreted as `--X=1`.

Also take a look at the [manual page](docs/nsst.1) (or `man nsst` if you have installed the terminal.)

## Key bindings

Hotkeys are configurable. Key binds have syntax `[<Mods>-]<Name>`, where `<Mods>` is XKB key name and mods is a set of one or more of following:

* `S` -- Shift
* `C` -- Control
* `L` -- Lock
* `T` -- Shift+Control (configurable with `term-mod`)
* `1`/`A`/`M` -- Mod1/Alt/Meta
* `2` -- Mod2/Numlock
* `3` -- Mod3
* `4` -- Mod4/Super
* `5` -- Mod5

Default keybindings:

    key-break=Break
    key-numlock=T-Num_Lock
    key-scroll-up=T-Up
    key-scroll-down=T-Down
    key-inc-font=T-Page_Up
    key-dec-font=T-Page_Down
    key-reset-font=T-Home
    key-new-window=T-N
    key-reset=T-R
    key-reload-config=T-X
    key-reverse-video=T-I
    key-copy=T-C
    key-copy-uri=T-U
    key-paste=T-V
    key-jump-next-cmd=T-F
    key-jump-prev-cmd=T-B

Copy URI key copies highlighted URI address. Highlighted URI is underlined by default.
For `key-jump-next-cmd`/`key-jump-prev-cmd` see shell integration section.

### Mouse support

If you are not using an application that enables mouse reporting, you can use built-in mouse interactions.
If application enables mouse reporting, built-in interactions can be forces with mouse pressing Ctrl+Shift (Can be configured with `force-mouse-mod` option).
Built-in mouse interactions:

* Scroll with mouse wheel.
* Jump between commands with Alt+mouse wheel (if your shell supports it more on shell integration below).
* Select with left click, select snapping on words on double left click, select snapping on lines on triple left click.
* Select rectangular area with Alt+left click (double click will snap to words).
* Select whole command output with Alt+triple click (requires shell integration).

### Shell integration

The most basic way to enable it is to put `\033]133;A\a` at the beginning of your shell prompt and `\e]133;B\a` at the end to enable jumping between commands with `T-F`/`T-B`.
To make T-N open window in current directory make sure to wrap `cd` to output `\033]7;$PWD\a` after directory change to notify the terminal.
To enable selecting whole command output (with Alt+triple click) additionally put `\033]133;D\a` at the end of the right prompt or before the beginning of the command output.

This repository includes shell integration scripts for *yash*, *zsh* and *fish* in `integration` directory. Source them at the bottom of your `.yashrc`/`.zshrc` file to enable it.
Other shells (bash) will be added in the future. Contributions are welcome.

**NOTE**: Some zsh themes, like [powerlevel10k](https://github.com/romkatv/powerlevel10k) override hooks, which causes native shell integration scripts
to stop working. You need to fix that depending on the theme. E.g. for powerlevel10k you need to set `POWERLEVEL9K_TERM_SHELL_INTEGRATION=true` in your
`.p10k.zsh` file, instead of using provided integration script.

This repository also includes *zsh*/*yash*/*fish*/*bash* completion scripts in `completion` directory. They are installed by default into appropriate paths.

### Wayland CSD

This implementation does not have first class support of CSD decorations, trying to rely on SSD instead.
Despite that terminal provides some mouse controls when SSD are not available:

* Use **Right** mouse button on the border to resize
* Use **Left** mouse button on the border to move
* Use **Middle** mouse button on the **top** border to close the window
* Scroll **up** on the top border to set maximized or fullscreen (in order)
* Scroll **down** on the top border to set maximized, normal or minimized (in order)

## Dependencies
### Build

* pkg-config
* GNU make or BSD make
* C11 compatible compiler

### Runtime

* Common:
    * `fontconfig`
    * `freetype2`
    * `xkbcommon`
* X11:
    * `libxcb`
    * `xcb-util-cursor`
    * `xkbcommon-x11`
* Wayland:
    * `libwayland-client`
    * `libwayland-cursor`
    * `wayland-protocols`

#### Void Linux

    xbps-install libxkbcommon-devel fontconfig-devel freetype-devel
    # For X11
    xbps-install libxcb-devel xcb-util-cursor-devel
    # For Wayland
    xbps-install wayland-devel wayland-protocols

#### Arch Linux and derivatives

    pacman -S libxkbcommon fontconfig freetype2
    # For X11
    pacman -S libxcb libxkbcommon-x11 xcb-util-cursor
    # For Wayland
    pacman -S wayland wayland-protocols

#### Debian and derivatives

    apt update
    apt install libxkbcommon-dev libfontconfig1-dev libfreetype-dev
    # For X11
    apt install libx11-xcb-dev libxcb-shm0-dev libxcb-render0-dev \
        xkbcommon-x11-dev libxcb-cursor0-dev
    # For Wayland
    apt install libwayland-dev wayland-protocols

## How to get it

You need all dependencies installed before getting started.

First clone this repo and cd into its folder like this:

    git clone https://github.com/summaryInfo/nsst
    cd nsst

Then configure and build:

    ./configure
    make -j

Default config is generally sane.
Alternatively do

    ./configure CFLAGS='-flto=auto -O2 -march=native'
    make -j

if you want to use it the machine you compile it on.
These more aggressive optimization options works fine.
It's more relevant for MIT-SHM backend.

XRender backend is slightly faster in general,
but a lot slower in some corner cases (e.g. small font and a lot of true color cells).
XRender backend is default.
In order to force MIT-SHM backend use `--backend=x11shm`.
In order to switch to Wayland use `--backend=waylandshm`.
By default wayland is used if available.

See `./configure --help` for more.

Finally install:

    make install

Default binary location is `/usr/local/bin/$name`
Sadly there is no man page yet.
