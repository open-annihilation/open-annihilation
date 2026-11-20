# PNG pictures

Reads and writes PNG files. The game reads PNG for the pictures on its
markup pages. Tools write PNG for image output, including a stored form
that is quick to write. A picture can carry text: a caption, a time, the
players in the game.

Target `oa-formats-png`, header `oa/formats/png.hpp`, namespace
`oa::formats::png`.

## Entry points

- `read_info` reads the header and the palette up to the first image-data
  chunk. `read_image` decodes the rows and then the chunks after the image.
- `header_of` and `row_bytes` size a decoded row. `channel_count` is the
  number of samples in one pixel. `has_signature` recognises a PNG file.
- `write` encodes a picture, choosing a row filter and compressing.
  `write_stored` leaves the rows unfiltered and stores them without
  compression, so a frame is written in about one pass over the pixels.
- `TextChunk` is one `tEXt` chunk (Latin-1) or one uncompressed `iTXt`
  chunk (UTF-8). `Image::texts` lists them, last, so a picture built as
  header, palette and rows stays what it was. `read_texts` reads the
  chunks back.

## State

None. Each call reads its arguments and returns a result. The keyword and
text from `read_texts` point into the file the caller passed, and remain
valid while that file does.

## Invariants

- A picture written with an empty text list is the signature, the header,
  the palette when the picture uses one, the image data and the end
  chunk. Those bytes are the bytes the writer produces for a picture that
  carries no text.
- Text chunks are written after the header and the palette, and before
  the first image-data chunk, in the order given.
- A keyword is 1 to 79 bytes of printable Latin-1 (codes 32 to 126 and
  161 to 255), with no leading space, no trailing space and no two spaces
  in a row. A `tEXt` text holds no NUL. An `iTXt` text is UTF-8 and holds
  no NUL. One chunk that breaks a rule makes `write` and `write_stored`
  leave the output untouched and report that the picture was not written.
- An `iTXt` chunk is stored with compression flag 0, compression method 0,
  an empty language tag and an empty translated keyword.
- `read_texts` returns every `tEXt` chunk and every uncompressed `iTXt`
  chunk, in file order. It skips a `zTXt` chunk, and an `iTXt` chunk whose
  compression flag is not 0.
- `read_info` and `read_image` keep the checks and the messages the markup
  pages rely on. Text is read through `read_texts`.

## Tests

`png-format` round-trips the colour types and bit depths, and checks the
markup reader's errors and warnings. `png-unfilter-row` checks each row
filter. `png-text-chunks` round-trips both text kinds, including a UTF-8
caption, refuses a keyword or a text that breaks the rules, checks that
the text sits between the header and the image data, and pins the bytes
of one small file with two text chunks and of the same picture with none.
It also keeps an uncompressed `iTXt` chunk whose method byte is not 0,
skips an unknown ancillary chunk and a `tIME` chunk, returns a text chunk
that follows the image data, and rejects a declared length past the end
of the file and every cut-short prefix of that small file.

## Limitations

The writer does not write `zTXt`, and it does not compress `iTXt`.
`read_texts` does not inflate either of those. The writer does not store
gamma, chromaticity, time or the other ancillary chunks. A text view from
`read_texts` lasts only as long as the file bytes do.
