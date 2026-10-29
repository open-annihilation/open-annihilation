// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The built-in registries whose keys the engine pins. A row is the
// registry's id, the key's id and the key's ed25519 text. An id with no
// row is trusted as its file stands.
//
// Core Prime's catalogue keys. Either one signs a catalogue and an engine
// feed. The fingerprints are 7001 · af50 · b971 · 5bb4 (steve-2026) and
// 4713 · b398 · 692c · ef47 (steve-2026-secondary). The second is kept
// apart from the first so either can retire.

inline constexpr std::array<PinnedKey, 2> pinned_key_table{{
    {"coreprime", "steve-2026", "ed25519:dHXqUJM9LbifF+boPakeKsZoSD29qhTEAxkyRj+s5yU="},
    {"coreprime", "steve-2026-secondary", "ed25519:SjxpBROn5onP1ILIa4tQQ3aL5BnghJ7L60Y97nA6YJY="},
}};
