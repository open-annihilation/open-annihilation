# Windows 95 run-time functions

The functions Windows 95 lacks that the C++ run-time library and SDL call.
Target `oa-platform-win95-runtime`, namespace `oa::platform::win95_runtime`,
and no header: nothing outside this module calls what is here.

A Windows build made with `-DOA_WINDOWS_95=ON` (see
[OaWindows95.cmake](../../../cmake/OaWindows95.cmake)) compiles engine code
against the declarations of Windows 95 and links these objects, whole, into
every executable. MinGW's import libraries would make Windows refuse to start a
program that imports a function its system DLLs do not export, so the functions
are defined here under the names a program imports them by and the program
imports none of them.

## What Windows 95 does not have

It exports the wide half of the Win32 and C run-time surfaces — `CreateFileW`,
`FindFirstFileW`, `CreateWindowExW`, `_wopen` and the rest — but as **stubs**.
Every one of them fails with `ERROR_CALL_NOT_IMPLEMENTED` (120) while its
system-character-set twin works, and the size-suffixed C run-time variants
(`_wstat64`, `_findfirst32`) are not exported at all. `GetFileAttributesEx` is
absent in both forms, being an API of Windows 98 and Windows NT 4.

So each function here asks for what it needs in the form that works, and
narrows or widens around it: a wide call is answered by narrowing the path,
calling the system's own system-character-set function, and widening the answer
back. Where a function is simply absent, it is built from the calls the system
does have — `GetFileAttributesExA` from `FindFirstFileA`, `GetFullPathNameW`
from `GetFullPathNameA`.

## Where the system has the function after all

Every definition takes the system's own function where the running Windows
exports it and stands in only where it does not, so one binary serves Windows 95
and every Windows after it. Which is possible because a function Windows 95
lacks is still declared by the headers the build compiles against, and because
the import pointer is one a program may point elsewhere: the object's definition
of the import pointer replaces the one the import library would have made.

## The files

| File | What it covers |
|---|---|
| [`windows_95_kernel.cpp`](src/windows_95_kernel.cpp) | kernel32: file, directory, path, environment, console, memory-status, version and synchronisation calls, and `RaiseException` |
| [`windows_95_user.cpp`](src/windows_95_user.cpp) | user32: window classes, windows, messages, input, displays and monitors, window properties, the clipboard and raw input |
| [`windows_95_shell.cpp`](src/windows_95_shell.cpp) | setupapi, cfgmgr32, shell32, ole32, psapi and propidl: device enumeration, the shell's folder dialogue, the notification area, and process listing |

## Two things about the build

**They are objects of the program, not a library on the link line.** An import
library SDL names is linked before any library of ours, so a function SDL calls
has already been linked from it by the time ours arrives, and what the linker
sees is a second definition rather than a replacement it takes. An object of the
program is linked before all of them. The functions
[oa-platform-xp-runtime](../xp-runtime/README.md) stands in for are ones only
the C++ run-time library calls, which is linked last, so a library suits those —
which is why the two are separate modules rather than one.

**Each file declares `_WIN32_WINNT` for itself**, at the version whose
declarations it wants to see, so that none of the functions it defines is also
declared as the system's. The engine's own sources are compiled at Windows 95's
version; these are not, because they have to name functions that version's
headers do not declare.

## The machinery, and why it is not shared

Each file carries its own `SystemFunction` type and `DEFINE_SYSTEM` macro, and
[windows_functions.cpp](../xp-runtime/src/windows_functions.cpp) carries the
same pair. They are the same eighty lines four times over, deliberately: the two
modules are linked differently and built for different Windows versions, and a
header between them would be a seam neither module has any other use for.
Whether that is the right trade is worth revisiting if a fifth file ever needs
them.
