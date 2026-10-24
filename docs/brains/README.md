# Brains

A brain is a package that plays a computer player. It is a Lua 5.4 script
in the engine's sandboxed script host, called on the simulation's ticks and
unit events, giving orders through the same paths a player's orders take.
Brains are opt-in. The engine's own computer player stays the default and
stays in the engine. A brain plays only on a seat a player set to it.
Classic is an example, written in Lua, that plays as that computer player
does. It is not a replacement.

A brain gives only the orders a player could give. It plays only its own
seat. It runs only on the machine that seats it, and it has no file,
network or clock of its own. Other machines, and a recording played back,
mirror the seat's units. They do not run the script.

The format is [the OABrain standard](oabrain-standard.md). Writing one is
[Writing a brain](authoring.md). Playing with one is
[Playing with brains](../playing-with-brains.md). Where a package
installs, and how a registry serves it, are
[Packages](../registries/packages.md). The catalogue entry is
[the catalogue](../registries/catalogue.md). Choosing a brain on the
command line is [the command line](../command-line.md).
