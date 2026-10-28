# Typing text

The game takes text in any script, Chinese among them, in these fields:

- the chat line in a game, and the battle room's chat line;
- a saved game's name, while the game's text is UTF-8: in a language drawn
  in the modern fonts, such as Simplified Chinese, or with Enable Unicode
  Multiplayer Chat on. Otherwise it takes the letters, digits and signs of
  an English keyboard;
- a marker's text on the whiteboard;
- the name typed after `.record`, which names a network game's recording.

Each field takes whole characters: a character that would not fit is left
out whole, never cut in two, and Backspace takes away the last whole
character. The player's name, the address of a game to join and a
password take the letters, digits and signs of an English keyboard, as
before.

The game's own fonts hold Western letters only. Characters of other
scripts show once **Use modern fonts for game text** is on in the
[Language settings](settings.md#language). What the other players see of
a chat line in another script is described in
[languages.md](languages.md).

## Input methods

On Windows, macOS and Linux, an input method, such as Pinyin, types into
these fields. What you are composing shows at the end of the field until
you choose the characters, which then go in. In a chat line, in a match
or in the battle room, and in the save dialog's name, it is underlined,
and drawn in the modern fonts as the characters it composes are. While a
composition is open the keys are the input method's: Backspace, Enter,
the arrows and the game's own keys act on the composition and do nothing
in the field or the game. Escape still closes the field.

## Keys while a field is open

While a field is taking text, the text wins. A key is typed, or it
belongs to the input method, and it does not run the command it is bound
to. Escape still closes the field.

On the chat line that close is **Cancel chat** (`chat.cancel`). On Open
Annihilation's own screens it is **Cancel screen** (`ui.screen-cancel`).
Both are fixed on Escape: a layout may add keys to them, and it cannot
take Escape away ([Keys](keys.md#escapes-one-fixed-job)).

The other chat keys move in the editor. **Open chat** (`chat.open`) is
Enter. **Open allies chat** (`chat.open-allies`) has no default key.
**Send chat** (`chat.send`) is Enter and Ctrl+Enter. Send is not fixed.

## Commands

A line that starts with an ASCII `+` or `.` is a command, such as a cheat
code or the recorder's `.record`, read and sent exactly as before. The
codes and where each one works are in [cheats.md](cheats.md); the chat
line itself is in [console.md](console.md). The full-width `＋`, `．` and
`。` a Chinese input method types are ordinary text. To type a command,
switch the input method to English first; in most Pinyin input methods
Shift switches it.

## Saved games and recordings named in any script

A saved game's name is the name of its file, and may be in any script
while the game's text is UTF-8. It cannot hold `\ / : * ? " < > |`; the
full-width `：` and `？` are ordinary characters. The name after `.record`
names a recording's file the same way.

On Windows such a name is stored and listed whatever the system's language
for non-Unicode programs, on Windows XP too, and so is a Documents folder
under a user name in any script. The file itself is a 3.1c saved game
whatever its name, but 3.1c opens one named in Chinese only on a Windows
whose language for non-Unicode programs holds the name, such as Chinese
(Simplified).

## iPhone and iPad

The system keyboard sends finished text only. While you choose characters
in its candidate bar, the field shows nothing of them; they go in once
chosen. A hardware keyboard's input method works the same way.

## Steam Deck

Steam's on-screen keyboard sends finished text too: with its Chinese
layout the characters are chosen in the keyboard itself and go into the
field once chosen. In Desktop Mode, the desktop's own input method works
as on [Linux](#input-methods).
