Not So Simple Terminal
======================
This is an implementation of VT220-compatible X11 terminal emulator.
Inspired by [Simple Terminal](https://st.suckless.org/)

## MIT-SHM

This branch is nsst variant that uses MIT-SHM X11 extension (or just `xcb_put_image`) and
memory buffer for rendering, unlike master branch which uses XRender.

This behaves a lot better with corner cases (a lot of true color cells) and huge screen
and doen't cause lag in X server.

Also, it would be easier to implement SIXEL, DECDLD and port to Wayland with software rendering.

**WARNING: I do push --force here to keep commits relevant to MIT-SHM on top**

## Features
    * Most escape sequences are already implemented
    * Ful keyboard mode from XTerm
    * OSC 13001 "Set background opacity"
    * Quiet fast rendering
    * Small size and almost no dependencies
    * Multiple terminal windows
        * This would be extended to full daemon mode
    * Configuration with Xrmdb and command line arguments

See TODO file for things that are to be implemented.

## Tips

Use TERM=xterm. The only unimplemented thing I encountered so far is unimplemented title stack.
There are other missing escape sequences, but they are not generally used (and still would be eventually implemented).

Works well with [Iosevka](https://github.com/be5invis/Iosevka) font. (Set font spacing to -1.)
Multiple fonts could be loaded by enumerating them in parameter:

    Nsst.font: Iosevka-13:style=Thin,MaterialDesignIcons-13

All options are now available though Xrmdb and command line arguments.

## Dependencies
### Build
    * pkg-config
    * GNU make or BSD make
    * C11 compatible comiler

### Runtime
    * xcb-util
    * xcb-util-wm
    * xcb-util-keysyms
    * xcb-util-xrm
    * fontconfig
    * freetype2
    * xkbcommon
    * xkbcommon-x11
