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
    * Multiple terminal windows
        * This would be extended to full daemon mode
    * Configuration with Xrmdb and command line arguments
    * MIT-SHM and XRender backends (compile time option)

See TODO file for things that are to be implemented.

## Tips

Use TERM=xterm. The only unimplemented thing I encountered so far is unimplemented title stack.
There are other missing escape sequences, but they are not generally used (and still would be eventually implemented).

Works well with [Iosevka](https://github.com/be5invis/Iosevka) font. (Set font spacing to -1.)
Multiple fonts could be loaded by enumerating them in parameter:

    Nsst.font: Iosevka-13:style=Thin,MaterialDesignIcons-13

All options are now available though Xrmdb and command line arguments.
No documentation yet for Xrmdb names, see `optmap[]` function in `config.c`.
For command line arguments see `nsst --help`

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
