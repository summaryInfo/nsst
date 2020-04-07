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

See TODO file for things that are to be implemented.

## Tips

Use TERM=xterm. The only unimplemented thing I encountered so far is unimplemented title stack.
There are other missing escape sequences, but they are not generally used (and still would be eventually implemented).

To change all settings edit file `config.c`

Works well with [Iosevka](https://github.com/be5invis/Iosevka) font.
To make other fonts look better try changing intercharecter distance.

Colors and background opacity are can be customized through xrdb

## Dependencies
### Build
	* pkg-config
	* GNU make
	* C99 compatible comiler

### Runtime
	* xcb-util
    * xcb-util-wm
    * xcb-util-renderutil
    * xcb-util-keysyms
    * xcb-util-xrm
	* fontconfig
	* freetype2
	* xkbcommon
	* xkbcommon-x11
