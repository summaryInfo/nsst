Not So Simple Terminal
======================
This is an implementation of VT220-like X11 terminal emulator.

## Features
* Quite fast rendering
    * Almost same latency as `XTerm`, which is a lot faster than other modern terminals
    * Scrolling performance is higher than most other terminals measured on my system
* Small size and almost no dependencies
* Uses xcb as X11 interface library
    * So it is faster and more lightweight
    * `size` including all loaded shared libs is only 80% of `st` on my system
* Most escape sequences are already implemented
* Full keyboard mode from XTerm
* OSC 13001 "Set background opacity"
* Multiple terminal windows
    * This would be extended to full daemon mode
    * `Shift-Ctrl-N` is default keybinding
* Configuration with Xrmdb and command line arguments
* MIT-SHM and XRender backends (compile time option)
* Compiles with `-flto` by default
* No warnings with `-Wall -Wextra` (except `-Wimplicit-fallthrough`)

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

## Notes

Use `TERM=xterm` for now (via `-D`/`termName` option). Almost every escape sequence from `ctlseqs.ms` is implemented.
See Emulation section of TODO for not yet implemented escape sequences.

Works well with [Iosevka](https://github.com/be5invis/Iosevka) font. (Set font spacing to -1 it it feels to wide.)
Multiple fonts could be loaded by enumerating them in parameter:

    Nsst.font: Iosevka-13:style=Thin,MaterialDesignIcons-13

Set `--override-boxdrawing`/`overrideBoxdrawing` to `true` if box drawing characters of font you use does not align.

If font looks to blurry try setting `--font-gamma`/`fontGamma` to value greater than `10000`.

Set pixelMode to your monitor's pixel alignment to enable subpixel rendering.

All options are now available through Xrmdb and command line arguments.
No documentation yet for Xrmdb names, see `optmap[]` function in `config.c`.

For command line arguments see `nsst --help`.
For boolean options `--no-X`, `--without-X`, `--disable-X` are interpreted as `--X=0` and
`--X`, `--with-X`, `--enable-X` are interpreted as `--X=1`

By default only DEC Special Graphics charset is allowed with UTF-8 mode enabled.
Spec is even stricter, disallowing any charset translations in UTF-8 mode, but DEC Special Graphics is used by applications frequently so it is allowed anyways.
To force full NRCS translation in UTF-8 mode set `--force-nrsc`/`forceNrcs`

Now nsst supports combining characters only via precomposition, but almost everything is ready to implement proper rendering of combining character (and variant glyphs support).
The only tricky part is to extract positioning tables and implemnt basic text shaping. It would be implemented using glyphs with codes `0x200000` - `0xFFFFFF`,
giving sufficient number of possible custom glyphs. DECDLD is also easy to implement this way.

Hotkeys are now configurable. With syntax `[<Mods>-]<Name>`, where `<Mods>` is XKB key name and mods is a set of one or more of folowing:

* `S` -- Shift
* `C` -- Control
* `L` -- Lock
* `T` -- Shift+Control
* `1`/`A`/`M` -- Mod1/Alt/Meta
* `2` -- Mod2/Numlock
* `3` -- Mod3
* `4` -- Mod4/Super
* `5` -- Mod5

Default keybindings:

    Nsst.break: Break
    Nsst.numlock: T-Num_Lock
    Nsst.scrollUp: T-Up
    Nsst.scrollDown: T-Down
    Nsst.incFontSize: T-Page_Up
    Nsst.decFontSize: T-Page_Down
    Nsst.resetFontSize: T-Home
    Nsst.newWindow: T-N
    Nsst.reset: T-R
    Nsst.reloadConfig: T-X

Example:

    # Default, CS-N will be the same
    Nsst.key.newWindow: T-N

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
