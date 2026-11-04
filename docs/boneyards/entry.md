# Entering the Boneyards

The main menu always shows the Boneyards strip: the status line, Single,
Multi, Intro and Exit, the credits spot, the red ENTER button and the row
of activity lights. It looks the same whether or not the Boneyards
expansion is installed. While `by.ccx` is not mounted, the strip is drawn
from the graphic embedded in the game. Once that archive is mounted, the
strip is drawn from it, and the picture is the same.

ENTER, the B key and starting with `-y` do not open the Boneyards at once.

When a newer Open Annihilation is offered, the game asks "A newer Open
Annihilation is out. Update before entering the Boneyards?" **Update**
downloads that release, the way Settings › Updates does, and stays on the
main menu. **Not now** stays on the main menu and does not open the
Boneyards. A click while that question is up does nothing. Choosing either
answer, then clicking ENTER again, asks again.

When no newer game is offered, or no update feed can be reached, the game
goes on. A check that cannot start (the welcome is not finished, updates
are set to never, or an unattended run was not given a feed) goes on too.

When the Boneyards expansion is not already in the game folder, the game
downloads and installs it from the Library. The menu shows "Fetching the
Boneyards… 42%" while that runs. A click during the fetch does nothing.
When the download finishes, the expansion is turned on and its archive is
mounted without leaving the menu, and the Boneyards open. When the
expansion is already in the game folder, none of that happens: ENTER opens
the Boneyards.

A catalogue with no such package, a download that fails, or a check the
server asked for that the game cannot do, shows the Library's own message
and stays on the main menu. **OK** closes that message. Clicking ENTER
again starts over.

Switching away after LOGIN, or while a login started with `-y` is still
connecting, does not drop the connection. Once the session is up it stays
up while the window is away. A battle played alone still pauses while the
window is away.

`native-boneyards-login-inactive` replays a login while the window stays
inactive. ENTER BONEYARDS opens the Boneyards, the guest name is filled and
LOGIN is clicked, and the login finishes before the loop is allowed to wait.
Its lines are `the Boneyards opened while the window was inactive`, `LOGIN
acted on while the window was inactive` and `logged in while the window was
inactive`.
