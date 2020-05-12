Not So Simple Terminal
======================
This is an implementation of VT220-compatible X11 terminal emulator.
Inspired by [Simple Terminal](https://st.suckless.org/)

## Features
* Most escape sequences are already implemented
* Ful keyboard mode from XTerm
* OSC 13001 "Set background opacity"
* Quiet fast rendering
* Small size and almost no dependencies
* Uses xcb as X11 library
	* So it is faster and more lightweight
	* `size` including all loaded shared libs is only 75% of `st` on my system 
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

## Tips

Use TERM=xterm. The only unimplemented thing I encountered so far is unimplemented title stack.
There are other missing escape sequences, but they are not generally used (and still would be eventually implemented).

Works well with [Iosevka](https://github.com/be5invis/Iosevka) font. (Set font spacing to -1 it it feels to wide.)
Multiple fonts could be loaded by enumerating them in parameter:

    Nsst.font: Iosevka-13:style=Thin,MaterialDesignIcons-13
    
Wide glyphs are now just clipped (but `wide` cell property is respecteed), later I will add option to avoid that.

All options are now available though Xrmdb and command line arguments.
No documentation yet for Xrmdb names, see `optmap[]` function in `config.c`.

For command line arguments see `nsst --help`.
For boolean options `--no-X`, `--without-X`, `--disable-X` are interpreted as `--X=0` and
`--X`, `--with-X`, `--enable-X` are interpreted as `--X=1`

By default only DEC Special Graphics charset is allowed with UTF-8 mode enabled.
Spec is even stricter, disallowing any charset translations in UTF-8 mode, but DEC Special Graphics is used by applications frequently so it is allowed anyways.
To force full NRCS translation in UTF-8 mode set `--force-nrsc`/`forceNrcs`

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

## Building

### To build:

	./configure
	make

Default config is generally sane.
Use `./configure CFLAGS='-flto -O3 -march=native'` to make it faster (but also bigger).
It's more relevant for MIT-SHM backend.
See `./configure --help` for more.

### To install:

	make install

Default location is `/usr/local/bin/$name`
