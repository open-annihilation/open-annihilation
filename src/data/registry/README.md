# data/registry

Registry descriptors, the player's registry list and the rules for adding a
registry. A descriptor says where a catalogue lives, which keys sign it and
how its packages are downloaded. The same text is an `.oareg` file. The
player's list is `Registries.yaml` in the player's Open Annihilation folder.

Nothing here fetches a descriptor or a catalogue. [descriptor.hpp](include/oa/data/registry/descriptor.hpp),
[player_registries.hpp](include/oa/data/registry/player_registries.hpp) and
[trust.hpp](include/oa/data/registry/trust.hpp) are the entry points
(`oa::data::registry`).

## Entry points

- `read_descriptor` and `descriptor_text` read and write one descriptor.
  `registry` is `1`. `valid_registry_id` and `valid_key_id` are the id
  rules. `key_fingerprints` is what a player compares when adding a
  registry. `address_text` is the catalogue's host, and the port when it
  is not 80.
- `load_player_registries` and `save_player_registries` read and write
  `Registries.yaml`. A missing file reads as empty. `read_player_registries`
  and `player_registries_text` are the same rules for bytes a caller
  already holds.
- `check_new_registry` and `check_new_mirror` say whether a player may add
  a registry or a mirror. `add_registry`, `remove_registry`, `set_enabled`,
  `add_player_mirror` and `replace_trusted_keys` apply those rules to the
  player's list and change nothing when they refuse. `refusal_text` is the
  sentence for a refusal.
- `read_builtin_registries` reads the `.yaml` descriptors in a folder.
  `pinned_keys` is Core Prime's two catalogue keys.
  `registries_in_effect` lists built-in registries first, then the player's.

## Registries.yaml

Both top-level keys are optional. An absent one reads as empty. Nothing
else is allowed at the top. Plain, single-quoted and double-quoted scalars
read alike. The writer double-quotes every string. A key this build does
not know is refused.

`registries` is the registries this player added. Each entry keeps a copy
of the descriptor, so one added from a file and every one while offline
still work. Required: `id`, `name`, `catalogue`, `trusted-keys`,
`downloads`, `added`, `enabled`. Optional, and empty when absent: `url`,
`homepage`, `mirrors`, `player-mirrors`, `title`, `description`.

| Key | Required | Rule |
|---|---|---|
| `id` | yes | kebab-case, 1 to 32 bytes: words of a-z and 0-9 joined by single hyphens |
| `name` | yes | 1 to 64 bytes of UTF-8, no control character. The short name |
| `title` | no | the card's heading, 1 to 96 characters (Unicode scalar values). Absent: the card heads with `name` |
| `description` | no | a paragraph on the card, 1 to 400 characters, counted the same way |
| `url` | no | where the descriptor was read; http or https; absent when it came from a file |
| `homepage` | no | http or https, opened only in a browser |
| `catalogue` | yes | http, no query |
| `mirrors` | no | at most 8 other copies of the catalogue, http, no query |
| `player-mirrors` | no | mirrors this player added, http, no query |
| `trusted-keys` | yes | at most 8 keys; empty only for an unsigned registry. Replaces the descriptor's `keys` |
| `downloads` | yes | `mode` `direct` or `tokens`; see below |
| `added` | yes | the day it was added, `YYYY-MM-DD` |
| `enabled` | yes | `true` or `false` |

`downloads.mode` is `direct` or `tokens`. `tokens` requires `api` (http, no
query) and defaults `install-id` to `required`. `direct` refuses `api`,
refuses `install-id: required`, and defaults `install-id` to `none`.
`install-id` is `required` or `none`. A key's `id` is 1 to 64 bytes: the
first is a-z or 0-9, and each one after is a-z, 0-9, a dot or a hyphen.
Key ids and public keys are each unique. `public` is the key's `ed25519:`
text, 52 characters.

`disabled` is the built-in registry ids this player turned off. Each id is
kebab-case, as `id` is.

```yaml
# Open Annihilation's registries: the ones this player added, and the built-in ones turned off.
registries:
  - id: "example-maps"
    name: "Example Maps"
    title: "Example Maps - Sample packages"
    description: "A sample registry of maps, including the made-up set Ridge."
    url: "http://maps.example.org/oa/registry.yaml"
    homepage: "http://maps.example.org/"
    catalogue: "http://maps.example.org/oa/v1/catalogue.json"
    mirrors: []
    player-mirrors: []
    trusted-keys:
      - {id: "maps-2026", public: "ed25519:public-key-text"}
    downloads: {mode: "direct", install-id: "none"}
    added: "2026-11-03"
    enabled: true
disabled:
  - "example"
```

`public-key-text` stands in for the 52-character key text. A file is
refused when that text is not a public key.

A descriptor uses the same rules for `registry`, `id`, `name`, `homepage`,
`catalogue`, `mirrors`, `keys` and `downloads`. `registry: 1` is required.
`keys` absent or empty is an unsigned registry. `trusted-keys` in the
player's file is that list once the registry has been added.

## State

Reads and writes the `Registries.yaml` path it is given, and reads the
built-in folder it is given. The file's name inside the player's folder is
`Registries.yaml`. Nothing else is read or written.

## Invariants

- An unreadable `Registries.yaml` is never written over. The write goes
  through the platform's file replace, so a failure leaves the previous
  file in place.
- Nothing a descriptor holds can name a built-in registry's id, name, keys
  or hosts. A mirror cannot be added to a built-in registry. An added
  descriptor cannot name the logo the game ships (`icon: oa`).
- An unsigned registry is only for Developer mode, and only when every
  catalogue, mirror and API host is exactly `127.0.0.1`. A homepage is not
  part of that check.
- A built-in registry whose id has pinned keys is used only when its file
  names those keys. An id with no pinned keys is trusted as its file stands.
  Core Prime's pinned table holds two keys. Either one signs a catalogue.
  The second is kept apart from the first so either can retire.
- An added registry whose id a built-in registry has taken is listed as
  conflicting and is never used. The built-in registry wins.
- At most 64 registries are added.

## Tests

`data-registry` reads and writes descriptors and `Registries.yaml`, including
each refused field. `data-registry-trust` decides which registries and
mirrors may be added, reads a built-in folder and lists the registries in
effect. `platform-replace-file` covers the file replace the registry file
uses.

## Limitations

No network: fetching a descriptor is separate work (F23). The descriptor
format is version 1 only. Signing a catalogue, its sequence and its expiry
are separate, as is opening an `.oareg` file from the system.
