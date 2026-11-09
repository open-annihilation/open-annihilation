# platform/screen-reader

The screen-reader bridge. It maps the game's controls onto the tree a
screen reader walks, and turns what the reader asks into the keys and
presses the game already handles. It draws nothing. With no screen reader
asking, it builds no tree and sends nothing.

## Entry points

`oa/platform/screen_reader.hpp` (namespace `oa::platform::screen_reader`):

- `load` binds AccessKit's C functions. On macOS, iOS and Linux the static
  library is linked with the bridge, and a program that calls the bridge
  pulls that code in. On Windows it loads `accesskit.dll` from the program's
  own folder, only on Windows 10 or later, with the critical-error mode set
  around that load.
- `compiled_in` is false when the bridge is left out: a Windows 95 build, a
  build with the bridge switched off, or a platform with no library. That
  build ships no DLL.
- `preference_of` reads `open-annihilation.access.screen-reader`. Absent, or
  any text other than `on` and `off`, is `auto`. `set_preference` remembers
  it. `off` drops an adapter that was attached and lets a later change try
  again.
- `attach` takes the game's SDL window and creates the platform adapter:
  on Windows, AccessKit's subclassing adapter while the window is still
  hidden, and the plain adapter plus this module's own subclass once the
  window is showing; the window subclass on macOS and iOS; and the Unix
  adapter on Linux. Attaching again to the same window does nothing.
  `detach` drops it. The game calls both from its frame. On Windows, a
  window created hidden for a reader that is already running is attached
  when it is made, and shown at once.
- `attach_skipped` is true after an attach of that window has failed, until
  the preference changes. `note_attach_finished` records that, and whether
  speech may run in place of the bridge.
- `speech_where_bridge_cannot` is true when the preference is `on` and the
  adapter did not attach, including a build that leaves the bridge out. This
  module does not speak.
- `reader_running` is true while the system says a screen reader, or another
  assistive client, is running. The answer is cached for about half a
  second. Windows asks `SystemParametersInfo`, macOS and iOS ask whether
  VoiceOver is on, and Linux asks AT-SPI with Properties.Get of
  org.a11y.Status.IsEnabled on the shared session bus. The connection stays
  open. The call is flushed and its reply is dispatched without waiting, and
  the accessibility bus is not opened. ScreenReaderEnabled is the older
  property, used when IsEnabled does not answer. A build that leaves the
  bridge out returns false.
- `active` is true after a screen reader has asked for the tree. The game
  publishes a tree only while `reader_running` is also true.
- `refresh_window` tells the adapter the window's size and whether it holds
  the input focus, when either has changed. It does not build a tree.
- `window_node` is the root. `test_node` and `test_label` are the label a
  unit test builds by hand. The live tree does not publish that label.

`oa/platform/screen_reader/tree.hpp` is the tree and the events, and a
build that leaves the bridge out does not include it:

- A `Node` is one control: its role, label, value, number and range,
  toggled, selected, expanded, disabled, modal, place in a set, shortcut,
  live setting and bounds. Bounds are window points, the same space as
  `SDL_GetWindowSize`.
- `kept_ids` leaves out a hidden control and decoration (`none`).
- `make_node` and `build_update` build AccessKit nodes through the bound C
  functions. An unchanged tree is not sent: `submit_tree` asks the adapter
  only when a reader has asked and the fingerprint differs from the tree
  last sent.
- `event_for` turns an action into one event. Click is a press at the
  control's centre; a switch is pressed on the half it does not show.
  Focus does nothing. Increment of a button whose press steps its value
  one way is that press, and does not need the game's focus. Decrement is
  not that press, and a drop-down does not step. A key or
  typed text reaches the game's focused control, so those are made only
  for that control. Increment and expand of anything else are Right.
  Decrement and collapse are Left. Set value on a field replaces its text:
  one backspace for each character it holds, then the new text. Set value
  on a slider is as many Left or Right taps as the step needs, at most 64.
- `take_events` returns the events queued from the reader's actions, in the
  order they arrived. The game pushes them onto SDL's queue before it polls.

`oa/platform/screen_reader/api.hpp` is the table of C functions `load`
binds. A check builds a node through that table and reads it back.

## State

Until `load` and `attach` are called, the module holds no library handle and
no adapter. The game does both under `on`, or under `auto` while a reader is
running: on Windows when it makes the window, elsewhere on the first frame.
It drops the adapter before the window is destroyed. A failed attach
is not tried again until the preference changes.

The preference defaults to `auto`. `auto` creates the adapter only while a
reader is running. `on` creates it even when none is. `off` creates nothing.
The tree the reader hears is kept here, with the fingerprint of the tree
last sent and the actions waiting to be taken. None of that is built until
a reader is running and one has asked.

Windows and macOS have no notice that the reader has gone, so once it has
asked, `active` stays true until `detach`. The game stops publishing when
`reader_running` becomes false. Linux and iOS drop `active` when the reader
disconnects, and the next tree is sent again.

The first tree a reader is shown, in the moment it asks, is a window named
Open Annihilation. The game's controls follow on the game's thread.

## Invariants

- Nothing is created when the game starts with no reader under `auto`. The
  adapter is attached when the window is made on Windows, or on the first
  frame elsewhere, under `on`, or `auto` while a reader is running. `off`
  attaches nothing.
- With no screen reader running, no control tree is built and nothing is
  sent. An unchanged tree sends nothing.
- The bridge draws nothing and speaks nothing.
- Windows never imports `accesskit.dll` at load. The program loads the copy
  beside itself, and only on Windows 10 or later.
- A Windows 95 build, a build with the bridge switched off, and a platform
  with no library compile the bridge out.
- The action callback frees the request it is given. The game's thread turns
  the queued events into keys and presses.
- Hidden controls and decoration are left out. A disabled control stays, and
  takes no action.
- The node a reader is told holds the focus is the control the game marks
  focused, whether or not the ring is drawn. When nothing is focused, the
  window is. A tab list, a radio group or a list that holds the focus
  announces its chosen child.
- A password's value is a length, never the text it holds.
- Bounds are window points. A point in the tree is a point a pointer event
  already uses.

## Tests

`platform-screen-reader` reads the preference, builds a tree update through
AccessKit's C API and reads the test node's role and label. It also maps a
fixture that holds each role a reader is shown, reads every node's role,
label, value, bounds and actions back through that API, and turns each
action into the game's event. An unchanged tree is staged and then submitted
again while a reader is marked active, and the factory is not called. On Windows it loads the DLL beside the
test. It skips when that DLL is not there (the Visual Studio lane has only
the header). On a Windows older than Windows 10 it checks that `accesskit.dll`
is not loaded (`GetModuleHandleW` returns null). On a build that leaves the
bridge out it checks that the bridge is compiled out.

`screen-reader-tree` maps a fixture of controls through the game's tree
builder and reads the nodes back through the same C API: each role, a dialog
named from its title, a list's rows, a tab list that adopts the tabs listed
before it, a hidden control left out, and a masked field.

## Limitations

Windows 7, Windows 8 and Windows XP do not load the DLL. The XP build still
carries the bridge and the DLL; `load` refuses to load it. On Windows the
game creates the window hidden only under `on`, or `auto` while a reader is
running, attaches as soon as the window is made, and shows it at once, so the
movies and the loading show as they always have. A window that is already showing uses the plain adapter.
Attaching on macOS or iOS returns false when SDL has no native window. SDL
does not report the content view or the root view controller, so those stay
the caller's.

On Linux the adapter needs no window, so it also runs under the dummy video
driver. With no D-Bus session bus the game plays as it does today and logs
`screen reader: no session bus, so the bridge is not started` once. When the
bus is set and the adapter still does not start, it logs `screen reader: the
bridge did not start` once.

A click replaces the pointer buttons the automation endpoint holds for that
click, then lets them go. Focus does not lie about which control is
focused: the announced node is the game's focused control. A reader cannot
move the game's focus: the game has no way to focus a control without a
press, and a press changes a radio, a tab, a row or a slider. Increment
of a button whose press steps its value one way is that press. A drop-down
does not offer it. Expand,
collapse, a slider's keys and set value on a field reach only the control
that holds the game's focus. A shortcut is set on the node. Neither
`accesskit_windows` nor `accesskit_macos` reads `keyboard_shortcut`.

The bridge does not speak. Where it cannot load, `on` is the switch that
lets speech run instead. Settings does not offer a row for the preference
yet.
