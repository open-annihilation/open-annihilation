# platform/screen-reader

The screen-reader bridge. A player who uses a screen reader reaches it
through AccessKit, once a caller attaches the game's window. Nothing is
created when the game starts, and the game does not attach a window yet.

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
- `attach` takes the game's SDL window and creates the platform adapter:
  the window subclass on Windows, macOS and iOS, and the Unix adapter on
  Linux. `detach` drops it.
- `window_node`, `test_node` and `test_label` are the tree this module
  publishes until a caller replaces it: one window, and a label reading
  "Test" that the window owns.

`oa/platform/screen_reader/api.hpp` is the table of C functions `load`
binds. A check builds a tree update through that table and reads the test
node's role and label back before the update takes the node.

## State

Until `load` and `attach` are called, the module holds no library handle and
no adapter. The game calls neither. An action request is freed and not
answered here.

## Invariants

- Nothing is created when the game starts.
- Windows never imports `accesskit.dll` at load. The program loads the copy
  beside itself, and only on Windows 10 or later.
- A Windows 95 build, a build with the bridge switched off, and a platform
  with no library compile the bridge out.
- The action callback frees the request it is given.
- The test node is a label, and a child of the window node.

## Tests

`platform-screen-reader` builds a tree update through AccessKit's C API and
reads the test node's role and label. On Windows it loads the DLL beside
the test. It skips when that DLL is not there (the Visual Studio lane has
only the header). On a Windows older than Windows 10 it checks that
`accesskit.dll` is not loaded (`GetModuleHandleW` returns null). On a build
that leaves the bridge out it checks that the bridge is compiled out.

## Limitations

The preference and the tree the player hears are not wired yet. Windows 7,
Windows 8 and Windows XP do not load the DLL. Attaching on Windows returns
false, and does not call AccessKit, when the window is already showing:
AccessKit would abort. Attaching on macOS or iOS returns false when SDL has
no native window. SDL does not report the content view or the root view
controller, so those stay the caller's. The bridge does not speak: a machine
that cannot load it stays silent here.
