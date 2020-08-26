Not So Simple Terminal
======================
This is an implementation of VT220-like X11 terminal emulator.

## Features
* Quite fast rendering
    * Almost same latency as `XTerm`, which is a lot faster than other modern terminals
    * Scrolling performance is on par with fastest terminals my system (`alacritty` and `urxvt`)
* Small size and almost no dependencies
* Uses xcb as X11 interface library
    * A litle bit faster because of its asyncrony
    * `size` including all loaded shared libs is only 80% of `st` on my system
* Most escape sequences from XTerm are implemented
* Full keyboard mode from XTerm
* Syncronous updates DCS
* `OSC 13001 ; Po ST` "Set background opacity to `Po`"
* Multiple terminal windows
    * This would be extended to full daemon mode later
    * `Shift-Ctrl-N` is default keybinding
* Configuration with Xrmdb and command line arguments
* MIT-SHM and XRender backends (compile time option)
* Compiles with `-flto` by default
* No warnings with `-Wall -Wextra` (except `-Wimplicit-fallthrough`)
* Rewraps text on resize (can be disabled by setting `--rewrap`/`rewrap` to false)

See TODO file for things that are to be implemented.

## Source structure

* `boxdraw.c` -- Boxdrawing characters rendering
* `config.c` -- Configuration handling and storage
* `font.c` -- Font loading and glyph caching
* `image.c` -- Image manipulation utilities
* `input.c` -- Input handling code (more or less compatible with Xterm)
* `nrcs.c` -- National replacement character sets logic
* `nsst.c` -- `main` function and arguments parsing
* `render-x11shm.c` -- X11 MIT-SHM backend
* `render-x11xrender.c` -- X11 XRender backend
* `term.c` -- Terminal logic
* `util.c` -- General utilities (encoding/decoding and logging)
* `window-x11.c` -- X11 specific window code
* `line.c` -- Line manipulation functions
* `tty.c` -- low level TTY/PTY code
* `mouse.c` -- Mouse events and selection handling

## Notes

Use `TERM=xterm` for now (via `-D`/`termName` option). Almost every escape sequence from [ctlseqs.ms](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html) is implemented.
See Emulation section of TODO for not yet implemented escape sequences.

Works well with [Iosevka](https://github.com/be5invis/Iosevka) font. (Set font spacing to -1 it it feels to wide.)
Multiple fonts could be loaded by enumerating them in parameter like:

    Nsst.font: Iosevka-13,MaterialDesignIcons-13

Set `--override-boxdrawing`/`overrideBoxdrawing` to `true` if box drawing characters of font you use does not align.

If font looks to blurry try setting `--font-gamma`/`fontGamma` to value greater than `10000`.

Set pixelMode to your monitor's pixel alignment to enable subpixel rendering.

All options are now available through Xrmdb and command line arguments.
No documentation yet for Xrmdb names, see `optmap[]` function in `config.c`.

For command line arguments see `nsst --help`.
For boolean options `--no-X`, `--without-X`, `--disable-X` are interpreted as `--X=0` and
`--X`, `--with-X`, `--enable-X` are interpreted as `--X=1`.

By default only DEC Special Graphics charset is allowed with UTF-8 mode enabled.
Spec is even stricter, disallowing any charset translations in UTF-8 mode, but DEC Special Graphics is used by applications frequently so it is allowed anyways.
To force full NRCS translation in UTF-8 mode set `--force-nrsc`/`forceNrcs`.

Sequential graphical characters are decoded all at once, so can be printed faster.

Debugging output can be enabled with `--trace-*`/`trace*` options.

For now nsst supports combining characters only via precomposition, but almost everything is ready to implement proper rendering of combining character (and variant glyphs support).
The only tricky part is to extract positioning tables and implemnt basic text shaping. It would be implemented using glyphs with codes `0x110000` - `0x1FFFFF`,
giving sufficient number of possible custom glyphs. DECDLD is also easy to implement this way.

Hotkeys are now configurable. With syntax `[<Mods>-]<Name>`, where `<Mods>` is XKB key name and mods is a set of one or more of folowing:

* `S` -- Shift
* `C` -- Control
* `L` -- Lock
* `T` -- Shift+Control (configurable with `--term-mod`/`termMod`)
* `1`/`A`/`M` -- Mod1/Alt/Meta
* `2` -- Mod2/Numlock
* `3` -- Mod3
* `4` -- Mod4/Super
* `5` -- Mod5

Default keybindings:

    Nsst.key.break: Break
    Nsst.key.numlock: T-Num_Lock
    Nsst.key.scrollUp: T-Up
    Nsst.key.scrollDown: T-Down
    Nsst.key.incFontSize: T-Page_Up
    Nsst.key.decFontSize: T-Page_Down
    Nsst.key.resetFontSize: T-Home
    Nsst.key.newWindow: T-N
    Nsst.key.reset: T-R
    Nsst.key.reloadConfig: T-X


## Dependencies
### Build

* pkg-config
* GNU make or BSD make
* C11 compatible compiler

### Runtime
* `libxcb`
* `xcb-util-xrm`
* `fontconfig`
* `freetype2`
* `xkbcommon`
* `xkbcommon-x11`

#### Void Linux

    xbps-install libxcb-devel libxkbcommon-devel xcb-util-xrm-devel \
        fontconfig-devel freetype-devel

#### Arch Linux and derivatives

    pacman -S libxcb xcb-util-xrm libxkbcommon libxkbcommon-x11 \
        fontconfig freetype2

#### Debian and derivatives

    apt update
    apt install libx11-xcb-dev libxcb-shm0-dev libxcb-render0-dev \
        libxcb-xrm-dev libxkbcommon-dev xkbcommon-x11-dev \
        libfontconfig1-dev libfreetype-dev

## How to get it

You need all dependencies installed before getting started.

First clone this repo and cd into its folder like this:

    git clone https://github.com/nsst
    cd nsst

Then configure and build:

    ./configure
    make -j

Default config is generally sane.
Alternatively do

    ./configure CFLAGS='-flto -O3 -march=native'
    make -j

if you want to use it the machine you compile it on.
These more aggressive optimization options works fine.
It's more relevant for MIT-SHM backend (default one).

XRender backend is slightly faster in general,
but a lot slower in some corner cases (e.g. small font and a lot of true color cells).
Use `--backend=x11xrender` configure option to enable it.

See `./configure --help` for more.

Finally install:

    make install

Default binary location is `/usr/local/bin/$name`
Sadly there is no man page yet.
