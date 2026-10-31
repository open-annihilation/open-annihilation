# 3DO model runtime

This component holds the per-unit 3DO instance of Total Annihilation 3.1c: one
piece state per model object, copied runtime-coordinate vertices, hierarchy
links, flags, and an owner association. The links are piece indexes and the
owner association is an opaque token, not pointers.

`count_linked_objects` sizes that allocation: the model object plus its
`first_child` and `next_sibling` chains, with a missing link contributing
nothing.

Binding a COB case-insensitively moves matching 3DO pieces into the COB
piece-name prefix, then rebuilds hierarchy links after that reorder.
`make_instance` performs both operations, ignoring COB names beyond the model
piece count as 3.1c does. A model whose hierarchy `model_hierarchy_error`
refuses gives an instance with no pieces. `ModelHost` implements the model portion of the
script VM Host; gameplay callbacks remain abstract for match-runtime.

`attachment_position` accumulates piece offsets through the three pair
rotations. `rebuild_transforms` supplies the reset/transform behavior, and
the renderer keeps the transforms it rebuilds for itself. `piece_box` gives
the box around one piece's vertices as that rebuild would place them,
without changing the instance: the simulation places what it reads of a
piece so, never from a draw's transforms. Angle words are 16-bit turns
(`65536` per revolution), coordinates remain signed 16.16 values, and
conversion uses the X/Z-negated model-runtime convention of the 3DO loader. The radians multiplier is the game's stored double rather
than a recomputed approximation.

`rebuild_transforms` leaves every piece the transforms of a bounded reset and
full root-subtree rebuild. It works a piece out again only when those could
differ from what the last rebuild left: the root rotation, or the translation
or rotation of the piece or of an ancestor, differs from what that rebuild
worked from (the instance keeps those, and a copy of the instance keeps them
with its transforms). Any other piece keeps its transforms, which the same
integer arithmetic would give again, so the result is the full rebuild's.
Whether 3.1c ever keeps a previous transform where this rebuilds it is not
established, nor what two further words of the 3DO instance hold: this
component keeps neither, and the match keeps one with the unit's slot, clears
it at spawn and never reads it. Root-level siblings are reset but are not
included in the root transform traversal, as in 3.1c.

The piece callbacks read flag bits and normalize setter inputs through their
low bit. Position and angle changes clear the piece transform marker and
dirty the instance; visibility clears the marker under the full-input
comparison. Cache/shade changes preserve it.

The 16-bit transform marker, explosion effects, and final screen projection
remain unresolved here. They are preserved or left at explicit component
boundaries rather than named from conjecture. The owner association is an
opaque token that the match runtime's spawn bridge sets; VM lifetime and
scheduling belong to script/match runtime.
