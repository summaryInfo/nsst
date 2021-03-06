Roadmap
=======

## Future versions

_NOTE: These plans can change any time_

### Until 2.4

* Hyperlinks autodetection

  * `--match-uri=<X>` where `<X>` is one of `off`,`manual`,`auto`

* Underlines

  * `CSI 58:2:... m` -- direct underline color
  * `CSI 58:5:... m` -- 256-color underline
  * `CSI 59 m` -- reset underline color
  * `CSI 21 m` -- double underline
  * `CSI 4:2 m` -- double underline
  * `CSI 4:3 m` -- curly underline
  * `CSI 4:4 m` -- dotted underline
  * `CSI 4:5 m` -- dashed underline
  * `--uri-underline-color` color

### Until 2.5 (or later)

* Keep non-rectangular selections on resize

* `CSI ? 35 l` / `CSI ? 35 h` -- font shifting functions

* OSC 50 ; Pt ST -- Set font

* Allow geometry to be specified in characters

* Pointer (for X11)

  * Shape
  * `CSI > Ps p` -- `XTSMPOINTER`
  * `OSC 13 Pt ST` -- Set mouse background color
  * `OSC 14 Pt ST` -- Set mouse foreground color
  * `OSC 113 Pt ST` -- Reset color mouse foreground color
  * `OSC 114 Pt ST` -- Reset color mouse background color

## Planned/possible features

### General

* We need better storage for lines contents that just malloc, to be able to scroll faster.

  Some kind of pool allocator, since we can only allocate scrollback from start and free from the back.
  Simplifies implementation of persistent scrollback feature (if it will be implemented at all).

* Add `always` value to some options to prevent overriding

  User might want to keep some settings in the certain way, not allowing an application
  to override it.

* Do something with multi-line shell prompt eating history lines

  This can possibly be done using shell integration.

* Make selection operate on physical lines?

  This allows to get rid of `term->view` numeric value and its recalculation on resize (which is linear).
  Allows to keep non-rectangular selections after resizing (_alternatively: recalculate their positions on resize too_).

* Some alternative to `XParseColor()`

  This is really hard since `XParseColor()` has some weird built-in colorspace conversions
  and implementing this in the way matching *XTerm* behaviour means implementing whole
  colorspace and colors handling from *xlib*.

### Rendering

* Font rendering

  * Colored glyphs

      This will allow rendering emoji
      In XRender this will require to use PutImage on every emoji (the most stright-forward solution)

  * Add options to select width of ambigues characters

      This requires custom `wcwidth()`

  * Combining characters support

      Basically, we need to create a global hashtable of privite runes
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
* Proper vt level restriction
* Is xterm private mode 1044 semantics correct/sould it be implemented at all?

* VT220

  * `DCS Pf ; Pc ; Pe ; Pw ; Ps ; Pt; Ph ; Px f Pt ST` -- **DECDLD**

* SIXEL

  * `CSI ? 80 l` / `CSI ? 80 h` -- **DECSDM**
  * `CSI ? 1070 l` / `CSI ? 1070 h` -- Use private resgisters
  * `CSI ? 8452 l` / `CSI ? 8452 h` -- Sixel scrolling leaves cursor to the right of graphics
  * `CSI ? Pi ; Pa; Pv S` -- **XTSMGRAPHICS** (`Pi` = 1, 2; `Pa` = 1, 2, 3, 4)
  * `DSC Pa ; Pb ; Ph q Ps..Ps ST` -- Send sixel image

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

  * `CSI ? 1039 l` / `CSI ? 1039 h` -- Alt sends escape (I don't want to differentiate between `Alt` and `Meta`)
  * `CSI ? 14 l` / `CSI ? 14 h` -- Cursor blinking XORing
  * `CSI Pm # p, CSI Pm # {` -- **XTPUSHSGR**
  * `CSI # q, CSI # }` -- **XTPOPSGR**
  * `CSI Pm # Q` -- **XTPOPCOLORS**
  * `CSI Pm # P` -- **XTPUSHCOLORS**
  * `CSI # R` -- **XTREPORTCOLORS**
  * `OSC I Pt ST` -- Set icon XPM file (Would not work in Wayland)

* Flawed and bad sequences (won't implemnet)

  * `CSI ? 38 l` / `CSI ? 38 h` -- **DECTEK** (this is complex, obscure and unused)
  * `OSC 3 ; Pt ST` -- Set X property (insecure)
  * `DCS + Q Pt ST` -> `DCS Ps + R Pt ST` -- **XTGETXRES** (too X11/xterm specific)
  * `CSI ? 1001 l` / `CSI ? 1001 h` -- Hightlight mouse tracking (can hang the terminal)
  * `CSI Ps ; Ps ; Ps ; Ps ; Ps T` -- **XTHIMOUSE** (can hang the terminal)
  * `ESC # 3` / `ESC # 4` -- **DECDHL** (purely interacts with mouse)
  * `ESC # 5` -- **DECSWL** (purely interacts with mouse)
  * `ESC # 6` -- **DECDWL** (purely interacts with mouse)
