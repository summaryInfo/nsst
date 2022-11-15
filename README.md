Not So Simple Terminal
======================
This is an implementation of VT220-like X11 terminal emulator.

## Features
* Quite fast rendering
    * Almost same latency as `XTerm`, which is a lot lower than for other modern terminals
    * Scrolling performance is on par with fastest terminals my system (`alacritty` and `urxvt`)
* Small size and almost no dependencies
* Uses xcb as X11 interface library
    * A little bit faster because of its asynchrony
    * `size` including all loaded shared libs is only 80% of `st` on my system
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
* MIT-SHM and XRender backends (compile time option)
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
* `render-x11xrender.c` -- X11 XRender backend
* `render-x12shm.c` -- X11 MIT-SHM backend
* `screen.c` -- Screen manipulation routines
* `term.c` -- Terminal logic
* `tty.c` -- Low level TTY/PTY code
* `uri.c` -- URL storage and validation
* `util.c` -- General utilities (encoding/decoding and logging)
* `window.c` -- Common window code
* `window-x11.c` -- X11 specific window code

## Notes

Use `TERM=xterm` for now (via `term-name` option). Almost every escape sequence from [ctlseqs.ms](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html) is implemented.
See Emulation section of TODO for not yet implemented escape sequences.

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
Default config file location is `$XDG_CONFIG_HOME/nsst.conf`.
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
* Selection with left click, word selection on double left click, line selection on triple left click.
* Rectangular selection is Alt+left click (double and triple clicks will snap to words/lines).

### Shell integration

The most basic way to enable it is to put `\033]133;A\a` at the beginning of your shell prompt and `\e]133;B\a` at the end to enable jumping between commands with `T-F`/`T-B`.
To make T-N open window in current directory make sure to wrap `cd` to output `\033]7;$PWD\a` after directory change to notify the terminal.

_FIXME: write terminal integration snippets for most shells._

## Dependencies
### Build

* pkg-config
* GNU make or BSD make
* C11 compatible compiler

### Runtime
* `libxcb`
* `fontconfig`
* `freetype2`
* `xkbcommon`
* `xkbcommon-x11`

#### Void Linux

    xbps-install libxcb-devel libxkbcommon-devel \
        fontconfig-devel freetype-devel

#### Arch Linux and derivatives

    pacman -S libxcb libxkbcommon libxkbcommon-x11 \
        fontconfig freetype2

#### Debian and derivatives

    apt update
    apt install libx11-xcb-dev libxcb-shm0-dev libxcb-render0-dev \
        libxkbcommon-dev xkbcommon-x11-dev libfontconfig1-dev libfreetype-dev

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

    ./configure CFLAGS='-flto -O2 -march=native'
    make -j

if you want to use it the machine you compile it on.
These more aggressive optimization options works fine.
It's more relevant for MIT-SHM backend.

XRender backend is slightly faster in general,
but a lot slower in some corner cases (e.g. small font and a lot of true color cells).
XRender backend is default.
In order to switch to MIT-SHM backend use `--backend=x11shm`.

See `./configure --help` for more.

Finally install:

    make install

Default binary location is `/usr/local/bin/$name`
Sadly there is no man page yet.
