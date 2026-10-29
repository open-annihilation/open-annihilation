# Sign-in panel

A panel inside the game's window that shows a web site's sign-in page and closes
itself once the page goes to an address that starts as the caller says,
keeping that address. GOG's sign-in ends on a page of GOG's own whose address
carries the sign-in's code; in this panel the game reads it there, and the
player copies nothing. Target `oa-platform-web-sign-in`, header
`oa/platform/web_sign_in.hpp`, namespace `oa::platform::web_sign_in`.

On Windows 10 and later the panel is a child window in the middle of the
game's window and holds the system's WebView2 browser. Its frame is drawn
as the game's dialogs are, in the colours the caller gives: a band with
the game's icon, the title and a close button, which closes the panel and
every page in it. It lives on the game's thread: SDL's event loop delivers
its messages and the browser's answers, and that thread is the COM
single-threaded apartment the browser needs. The navigation to the landing
address is cancelled before that page loads. A page that asks for a window
of its own, as the sign-in with Google does, takes the first page's place
in the frame; the first is hidden until the second closes, and the page in
it still talks to the second and may close it. Escape
closes the panel it is pressed in. The
browser keeps its files in `%LOCALAPPDATA%\Open Annihilation\Sign-in browser`
and forgets its cookies as the page loads and as it lands.

It is built with Visual Studio when the CMake cache variable
`OA_WEBVIEW2_DIR` names the unpacked
[Microsoft.Web.WebView2](https://www.nuget.org/packages/Microsoft.Web.WebView2)
package (a `.nupkg` is a zip file): its header and its static loader are
taken from `build/native`. The WebView2 runtime itself is part of Windows 11
and of current Windows 10. Without the package, on other toolchains, in a
Windows XP build and on other systems, `available` is false and nothing
opens; the game's GOG download then opens the page in the web browser and
reads the address the player copies.
