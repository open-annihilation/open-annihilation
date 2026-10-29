# Signatures

A signed registry publishes a detached signature beside the catalogue,
usually `catalogue.json.sig`. OA verifies it over the exact bytes of the
catalogue before those bytes are parsed. A change of one byte, including
whitespace, is a different catalogue and needs a new signature.

**The example key `rfc8032-test-1` must never sign a real catalogue.** It is
the public test vector in RFC 8032 section 7.1 TEST 1, published so the
example can be checked, and it is not a key a registry may use. Its public
text is `ed25519:11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=` and its
fingerprint is `21fe · 31df · a154 · a261`. No private key is part of this
specification. The example signature is
[examples/catalogue.json.sig](examples/catalogue.json.sig), over the exact
bytes of [examples/catalogue.json](examples/catalogue.json).

## The signature file

The file is ASCII, at most 256 bytes, and exactly one line of three fields
separated by single spaces:

```text
ed25519 <key-id> <base64 signature>
```

A line end, LF or CRLF, may follow, and nothing else may. `ed25519` is those
letters. The key id is the id from the descriptor or the catalogue. The
signature is 88 characters of strict base64, the Ed25519 signature of the
catalogue's exact bytes. Ed25519 is deterministic: the same key and the same
bytes always give the same signature.

OA trusts a signature whose key id is one of the keys it already trusts for
that registry, and whose signature verifies. A missing file, a file that is
not this line, an unknown key id, or a signature that does not verify, refuses
the catalogue. An unsigned registry (no keys in the descriptor) skips the
signature. Unsigned registries are only for Developer mode on `127.0.0.1`,
as [descriptor.md](descriptor.md) says.

## Rotation

To publish a new key, put it in `keys` of a catalogue that an old trusted
key still signs, and publish that signature. OA reports the new set for an
added registry when `keys` is present, not empty, and not the set the player
already trusts. The player then trusts the new set. A built-in registry does
not rotate this way: its pinned keys are the ones in the build. An empty
`keys` list is not a rotation, and it does not turn a signed registry into
an unsigned one.

Keep signing with a key the previous catalogue already trusted until players
have accepted the new set. A catalogue signed only by a key nobody trusts yet
is refused.

A built-in registry's keys change only in a release. A new key ships before
the old one retires. The engine feed follows the same keys.

## `oa-tool catalogue`

`oa-tool catalogue keygen --id ID --out FILE` makes a new Ed25519 key, seals
the seed with a passphrase and writes the key file. The file is created only
when that name is new. The passphrase is at least 12 bytes, read from the
terminal with echo off, twice, or from the first line of `--passphrase-file`.
The command prints the key id, the public key as `ed25519:` and its base64,
and the fingerprint.

`oa-tool catalogue public KEY_FILE` prints the key id, the public key and
the fingerprint. It does not ask for the passphrase and does not open the
seed.

`oa-tool catalogue sign --key KEY_FILE CATALOGUE` signs the exact bytes.
Without `--out`, the signature is the catalogue's name with `.sig` added.
A catalogue this build would refuse is refused before the key is opened. The
written pair is checked before the command reports success.

`oa-tool catalogue verify CATALOGUE` checks a catalogue against its
signature. Without `--sig`, the signature is the catalogue's name with
`.sig` added. Pass `--descriptor FILE` to use that registry's id and keys,
or `--registry ID` and at least one `--key ed25519:<base64>`. It prints the
signing key and the verdict, and exits 0 when the catalogue can be used.
