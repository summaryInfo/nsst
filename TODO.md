Roadmap
=======

## Future versions

_NOTE: These plans can change any time_

### Until 2.4

* **Find out where empty tail for scrolled off lines is comming from, since erase should have trimmed them.**
  * Optimize `concat_line()` by doing that.

* Optimize `screen_scroll_fast()` -- need to change handles internal structure to make then easer relocatable.
 * Replace linked list with array?

* Hyperlinks auto-detection

  * `--match-uri=<X>` where `<X>` is one of `off`,`manual`,`auto`

### Until 2.5 (or later)

* Optimize screen_damage_uri to iterate over attributes first to find whether the line has attributes with source URI.

* Fix flaky tests
  * `DECSETTests.test_DECSET_Allow80To132` causes flakes for itself and `DECSETTests.test_DECSET_DECAWM`, `DECSETTests.test_DECSET_DECCOLM`.

* Underlines
  * `CSI 4:3 m` -- curly underline

* Allow selecting over the end of the short line

* Wayland

* IME support

* Allow geometry to be specified in characters

* OSC 50 ; Pt ST -- Set font

* `CSI ? 35 l` / `CSI ? 35 h` -- font shifting functions

* Pointer (for X11)

  * Shape
  * `CSI > Ps p` -- `XTSMPOINTER`
  * `OSC 13 Pt ST` -- Set mouse background color
  * `OSC 14 Pt ST` -- Set mouse foreground color
  * `OSC 113 Pt ST` -- Reset mouse foreground color
  * `OSC 114 Pt ST` -- Reset mouse background color

## Planned/possible features

### General

* Add `always` value to some options to prevent overriding

  User might want to keep some settings in the certain way, not allowing an application
  to override it.

* Do something with multi-line shell prompt eating history lines

  This can possibly be done using shell integration.

* Some alternative to `XParseColor()`

  This is really hard since `XParseColor()` has some weird built-in colorspace conversions
  and implementing this in the way matching *XTerm* behavior means implementing whole
  colorspace and colors handling from *xlib*.

### Rendering

* Font rendering

  * Colored glyphs

      This will allow rendering emoji
      In XRender this will require to use PutImage on every emoji (the most straight-forward solution)

  * Add options to select width of ambiguous characters

  * Combining characters support

      Basically, we need to create a global hash table of private runes
      mapping (with values higher than 2^21) to unicode code point sequences,
      parse fonts to determine relative glyphs positions and render glyphs
      one atop another to create new glyph and associate it with private rune in glyph cache.

* Use multiple separate buffers in XRender backend for background/lines/glyphs to sort less

* MIT-SHM (software)

  * Threaded rendering?
  * Better `image_copy()` (just like `image_draw_rect()`, `image_compose_glyph()`)

* Refactor window/renderer code to separate X-independent parts

* Wayland (**EVERYTHING**)

### Input

* Implement terminal-wg protocol proposed in [!5](https://gitlab.freedesktop.org/terminal-wg/specifications/-/merge_requests/5) (or not to?..)

* Add *mintty* extensions to `CSI-u` and `xterm` protocols

   * Option to encode escape as `\e[27u` or `\e[27;1u`
   * Option to encode `Alt` as modifier, not prefix
   * Option to report key releases as `\e[<key>;<mods>;1u`
   * Option to report modifiers presses as `\e[0;<mods>u` or `\e[0;<mods>;1u`
   * Report `Super` (`mod4`) and `Hyper` (`mod5`) modifiers (as 16/32)

### Shell integration

* Select whole output or executed command

* Mouse interaction with shell line editing (?)

* Jumping between shell prompts in scrollback

### VTxxx/XTerm compatibility and emulation

* Encode NRCSs for printer autoprint mode
* Proper VT level restriction
* Is xterm private mode 1044 semantics correct/should it be implemented at all?

* VT220

  * `DCS Pf ; Pc ; Pe ; Pw ; Ps ; Pt; Ph ; Px f Pt ST` -- **DECDLD**

* SIXEL

  * `CSI ? 80 l` / `CSI ? 80 h` -- **DECSDM**
  * `CSI ? 1070 l` / `CSI ? 1070 h` -- Use private registers
  * `CSI ? 8452 l` / `CSI ? 8452 h` -- SIXEL scrolling leaves cursor to the right of graphics
  * `CSI ? Pi ; Pa; Pv S` -- **XTSMGRAPHICS** (`Pi` = 1, 2; `Pa` = 1, 2, 3, 4)
  * `DSC Pa ; Pb ; Ph q Ps..Ps ST` -- Send SIXEL image

* Termcap

  * `CSI ? 1040 l` / `CSI ? 1040 h` -- terminfo/termcap function key mode
  * `DCS + p Pt ST` -- **XTSETTCAP**
  * `DCS + q Pt ST` -- **XTGETTCAP**

* Extended editing (interacts well with shell integration)

  * `CSI ? 2001 l` / `CSI ? 2001 h` -- `srm_BUTTON1_MOVE_POINT`
  * `CSI ? 2002 l` / `CSI ? 2002 h` -- `srm_BUTTON2_MOVE_POINT`
  * `CSI ? 2003 l` / `CSI ? 2003 h` -- `srm_DBUTTON3_DELETE`
  * Also, need to look into terminal-wg discussion about mouse shell interaction

* Misc

  * `CSI 4:4 m` -- dotted underline
  * `CSI 4:5 m` -- dashed underline
  * `CSI ? 1039 l` / `CSI ? 1039 h` -- Alt sends escape (There's no plant to differentiate `Alt` and `Meta`)
  * `CSI ? 14 l` / `CSI ? 14 h` -- Cursor blinking XORing
  * `CSI Pm # p, CSI Pm # {` -- **XTPUSHSGR**
  * `CSI # q, CSI # }` -- **XTPOPSGR**
  * `CSI Pm # Q` -- **XTPOPCOLORS**
  * `CSI Pm # P` -- **XTPUSHCOLORS**
  * `CSI # R` -- **XTREPORTCOLORS**
  * `OSC I Pt ST` -- Set icon XPM file (Would not work in Wayland)

* Flawed and bad sequences (won't implement)

  * `CSI ? 38 l` / `CSI ? 38 h` -- **DECTEK** (this is complex, obscure and unused)
  * `OSC 3 ; Pt ST` -- Set X property (insecure)
  * `DCS + Q Pt ST` -> `DCS Ps + R Pt ST` -- **XTGETXRES** (too X11/xterm specific)
  * `CSI ? 1001 l` / `CSI ? 1001 h` -- Hightlight mouse tracking (can hang the terminal)
  * `CSI Ps ; Ps ; Ps ; Ps ; Ps T` -- **XTHIMOUSE** (can hang the terminal)
  * `ESC # 3` / `ESC # 4` -- **DECDHL** (poorly interacts with mouse)
  * `ESC # 5` -- **DECSWL** (poorly interacts with mouse)
  * `ESC # 6` -- **DECDWL** (poorly interacts with mouse)
