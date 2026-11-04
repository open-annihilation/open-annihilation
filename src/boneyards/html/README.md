# HTML

The markup pages and the font chain behind them.

## Entry points

`oa/boneyards/html.hpp` and `oa/boneyards/html_scripts.hpp`. The library is
`oa-boneyards-html`. The faces the pages name are `oa-boneyards-html-host-fonts`.

## The markup it reads

The Help, News and Ladders consoles and the profile reports show pages in
this dialect. A page is read up to its first NUL. Anything not listed here is
skipped: an unknown tag is dropped and its content is read as if the tag were
not there; an unknown attribute is ignored.

| Tag | Attributes | What it does |
|---|---|---|
| `HTML` | | a plain container |
| `BODY` | `LEFTMARGIN`, `TOPMARGIN` (pixels) | text outside `BODY` is not shown; `BGCOLOR` is read and ignored |
| `FONT` | `SIZE`, `FACE`, `COLOR` | `SIZE=n` sets a character height of 2n + 6 pixels, `+n` and `-n` change the current one by 2n (the default is 12 pixels); `COLOR` takes exactly six hex digits, with no `#` |
| `B`, `I`, `U` | | bold, italic, underlined |
| `H1`, `H2`, `H3` | | 5 pixels taller and bold, 3 taller and italic, 1 taller and bold, on a line of their own |
| `A` | `HREF` | underlined; a click opens the target: a page of the game's files (`Boneyards/Help/Help74.htm`), `#Remote#/path` on the console's content server, or an `http://` address |
| `BR` | | ends the line |
| `P` | | ends the line and adds 2 pixels; it is not a container, and `</P>` does nothing. A blank line needs a line with something on it, such as `<BR>&nbsp;<BR>` |
| `CENTER` | | centres its lines across the page |
| `IMG` | `SRC`, `WIDTH`, `HEIGHT` | a PNG of the game's files, at most 256 pixels a side; `\` or `/` separate folders; an `http:` source shows nothing |
| `TABLE` | `ROWSPACING`, `COLSPACING` (pixels, 1 by default) | `BORDER` is read and ignored; no borders are drawn |
| `TR` | `HEIGHT` | a row; without `HEIGHT` it is as tall as its tallest cell plus 4 |
| `TD` | `WIDTH`, `ALIGN` (`CENTER`, `RIGHT`), `BGCOLOR` | a cell; its text starts in the default font, so a `FONT` goes inside the cell |

Tables:

- the first row's `TD WIDTH`s set the column widths; columns without one
  share what is left of the page width equally. A later row's widths are
  ignored, and a column past the first row's is 0 pixels wide;
- a cell's text wraps inside its column; `VALIGN` is read and ignored;
- `TD BGCOLOR` reads the hex letters A to F as 0 to 5, as 3.1c does: use
  digits only (`303030`) for a predictable colour;
- `TH`, `CAPTION`, `COLSPAN`, `ROWSPAN`, `CELLPADDING` and `CELLSPACING` are
  not read. Text inside `TABLE` or `TR` but outside a `TD` is shown outside
  the table, and so is a `TH`'s;
- a table goes inside a `TD` only: one directly inside another `TABLE` or
  `TR` ends the outer table, and a `TR` after it stops the page.

Text:

- spaces, tabs, line feeds, form feeds and carriage returns separate words,
  and any run of them is one gap: the width of the console's space. A page
  saved with CR LF line ends reads as one saved with LF;
- `&amp;` `&lt;` `&gt;` `&quot;` `&nbsp;` `&copy;` `&reg;` (any case, the `;`
  optional) and `&#NNN;` `&#xHH;` are decoded. `&nbsp;` is a space that does
  not break the line or collapse. `&reg;` draws the trade mark sign, as in
  3.1c; `&#174;` draws the registered sign. An `&` that starts nothing known
  is shown as written;
- the text is Windows-1252, or UTF-8 when a word is valid UTF-8; a character
  no face holds draws nothing, and control characters draw nothing.

Lists (`UL`, `OL`, `LI`), `HR`, `PRE`, `TT`, `STRONG`, `EM`, `DIV` and
`SPAN` are not read: their text runs on as plain text, with no line break
or gap where the tag stood, so `one<LI>two` reads "onetwo".

Faces: `Arial` (the default) and any face not listed below are set in
Liberation Sans, `Times New Roman` and `Times` in Liberation Serif, and
`Haettenschweiler` in Saira ExtraCondensed ExtraBold. Without the bundled
faces the game's FNT fonts stand in.

## State

Reads the page source and the fonts in the game's fonts folder. Writes the
8-bit picture of the page. The chain is opened once from that folder.

## Invariants

A character no face holds draws nothing. Chinese, Japanese and Korean come
from the engine's text faces in that folder, not from a collection file.
DejaVu and Noto Emoji stay on this chain's own slots.

## Tests

`boneyards-html-*`, including `boneyards-html-host-fonts`.
`boneyards-console-help-pages` opens every Help page of the install in the
Help console and can write each as a picture.

## Limitations

The chain's own stack is not the stack the engine's screens add a language
pack to. A pack's faces are used when that stack holds them, which this
open does not add.
