# Download API

A descriptor's `downloads.mode` chooses how a package is fetched.

`direct` fetches the catalogue's `file` reference, resolved against the host
that served the catalogue and against the registry's mirrors, and checks the
bytes against the catalogue's SHA-256. It sends no install ID and calls no
API. `downloads.api` is refused on a direct descriptor.

`tokens` asks the download API for a key, then fetches the `http` URL the
key names. `<api>` below is the descriptor's `downloads.api`, with no
trailing slash added by the registry: the paths are `<api>/downloads` and so
on. When the API cannot be reached, OA fetches the catalogue's file
addresses instead. Neither a 4xx nor a 428 falls through to a mirror. A 5xx,
a 201 or 428 that does not read, and a failure to reach the host are retried,
and then a mirror may be tried.

Times are UTC, as [catalogue.md](catalogue.md) reads them. OA reads an API
body of at most 64 KiB. Unknown members on a request are ignored. OA ignores
a response member it does not read.

The examples are [examples/download-request.json](examples/download-request.json),
[examples/download-response.json](examples/download-response.json),
[examples/challenge.json](examples/challenge.json),
[examples/challenge-state.json](examples/challenge-state.json),
[examples/result.json](examples/result.json),
[examples/error.json](examples/error.json) and
[examples/counts.json](examples/counts.json). Each has a schema beside it
under [schemas/](schemas).

## Install ID

An install ID is 32 lower-case hex digits in groups of 8, 4, 4, 4 and 12,
as in `7f3a90d2-5b1c-4e8a-9d2f-0a6b3c4dc91e`. OA makes one from 16 bytes of
the system's generator, and from nowhere else, when a download needs one or
the player asks. There is one ID per registry. It is sent only on the key
request, and only when that registry's `install-id` is `required`. Direct
registries never send one.

The player can reset the ID, which replaces it, or turn it off. While it is
off, OA does not download from a registry that requires one, and it does not
send a request.

A registry whose `install-id` is `required` MUST refuse a key request that
leaves `install` out. Core Prime requires an install ID, and its server
refuses that request. A registry whose `install-id` is `none` MUST accept a
key request that leaves `install` out, and OA leaves it out. There is no
conflict: a server that always requires `install` is serving a `required`
registry.

The ID is not a player's name. A registry MUST NOT record the network
address a request came from, and MUST NOT keep a list of those addresses.

## Asking for a key

`POST <api>/downloads` with `Content-Type: application/json`. A charset
parameter is accepted. Any other type is `415 unsupported_media_type`. The
body is at most 8 KiB; a larger body is `413 too_large`. A body that is not
a JSON object is `400 bad_request`.

OA writes the members in this order, and leaves `install` out when the
registry does not use one. `language` is always sent, and may be an empty
string. A server MUST treat a missing `language` as empty.

| Member | Rule |
|---|---|
| `install` | Present only when `install-id` is `required`. Lower-case hex, `8-4-4-4-12`. |
| `package` | 1 to 64 characters of `[A-Za-z0-9][A-Za-z0-9._-]*`. This is wider than a catalogue key. |
| `kind` | `oamod`, `oamap`, `oalang`, `expansion`, `oamut`, `oabrain` or `oakeys`. |
| `release` | A whole number from 1 to 2147483647. |
| `sha256` | 64 hex digits. A server accepts either case and compares them in lower case. The catalogue itself requires lower case. |
| `engine` | 1 to 32 characters of `[0-9A-Za-z.+-]`. |
| `platform` | 1 to 16 characters of `[a-z0-9-]`. |
| `arch` | 1 to 16 characters of `[a-z0-9_]`. |
| `language` | Empty, or a tag of 2 or 3 letters, then up to three hyphen-joined parts of 1 to 8 letters or digits. |
| `reason` | `install`, `update`, `repair`, `offered-in-lobby` or `mirror`. |

A server that checks the fields checks them in that order and returns the
first failure as `400 invalid_field` with `field` set to the member's name.
`reason` `mirror` is a registry copying another registry's packages.

`201 Created` returns:

| Member | Rule |
|---|---|
| `download` | 1 to 128 characters of `A-Z`, `a-z`, `0-9`, `_` and `-`. This is the id used in the result path. |
| `expires` | UTC time. The URL is valid until this moment. |
| `url` | An `http` or `https` address. A query is allowed. |

Core Prime's ids are `d_` plus 22 characters from that alphabet, made from
16 random bytes. That shape is one registry's habit. OA accepts any id of
the wider form.

## Asking again

Before a result is stored, Core Prime answers another request for the same
`install`, `kind`, `package`, `release` and `sha256` with the same
`download`, a new URL and a new `expires`. That is one download record. The
record's key count increases by one, and a rate-limit window counts each
key. `GET <api>/counts` counts the record once, however many keys it took.
Another install, or another release, is a new record. After a result is
stored, the next request is a new record.

A registry whose `install-id` is `none` cannot match an earlier request by
install ID, and MAY answer with a new `download`.

Either way, OA posts one result, to the `download` of the last key it got.
It asks again when a key expires within 30 seconds, and when the package URL
answers 403 or 404, up to three times, and it replaces the id it will post
to with the new key. The `seconds` of the result are counted from the first
key, not from the latest one. A clock behind that first key is sent as 0.

## Challenges

`428 Precondition Required` asks the player to confirm they are a person
before another key is issued:

| Member | Rule |
|---|---|
| `error` | `challenge_required`. |
| `message` | An English sentence. |
| `challenge.code` | The challenge's id. 1 to 128 characters of the download-id alphabet. |
| `challenge.verify` | An `http` or `https` page. A query is allowed. |
| `challenge.expires` | UTC time. |
| `challenge.poll` | Whole seconds. Absent means 3. OA keeps a value inside 2 to 30. |

OA polls `GET <api>/challenges/<code>` every `poll` seconds (every 3 seconds
when the registry does not say). `200` is:

| Member | Rule |
|---|---|
| `code` | The same id. |
| `state` | `pending`, `solved` or `expired`. |
| `expires` | UTC time. |

`404` with `unknown_challenge` means the code is not known. OA treats that
404 as expired and does not ask the player to tell the two apart. When the
state is `solved`, OA asks for a key again.

`POST <api>/challenges/<code>/solve` is one registry's way to pass a check.
Core Prime's verify page calls it, behind its own check. OA never calls it.
A registry does not have to offer it. When it does, `200` is
`{"code", "state": "solved"}`, including when the challenge was already
solved, `410` is `challenge_expired`, and `404` is `unknown_challenge`.

Core Prime's codes are eight characters in two groups of four, drawn from
`23456789ABCDEFGHJKMNPQRSTVWXYZ`, and its `poll` is 3. The example uses that
shape. OA accepts any code of the wider alphabet.

## The result

`POST <api>/downloads/<download>/result` with:

| Member | Rule |
|---|---|
| `result` | `installed`, `refused`, `failed` or `cancelled`. |
| `bytes` | A whole number from 0 to 2^40, the bytes fetched. |
| `seconds` | A whole number from 0 to 604800, from the first key. |

Success is `204` with an empty body. A second result is
`409 result_recorded` and the first result stands. An id the registry does
not know is `404 unknown_download`. Core Prime also answers 404 when the id
is not `d_` plus 22 characters of the download-id alphabet.

OA posts once, with a 10 second limit. It treats a 2xx or a 409 as accepted,
because a 409 means a result is already stored.

## Counts

Open Annihilation does not read Counts. The endpoint stays so a registry
owner can publish how many times each package was downloaded. The game
shows no count and keeps no copy of the reply. An old file under
`<data>/content/counts` is left where it is.

`GET <api>/counts` returns `200` with `Cache-Control: public, max-age=300`:

| Member | Rule |
|---|---|
| `counts` | The shape version, `1`. Not a total. |
| `generated` | UTC time. |
| `packages` | Objects of `kind`, `id` and `downloads`, sorted by kind, then id. |

`downloads` counts download records, however many keys each record took.

## Refusals

Every refusal is `{"error", "message"}` plus the extra member named here.
`message` is an English sentence.

| Status | `error` | When | Extra |
|---|---|---|---|
| 400 | `bad_request` | The body is not a JSON object. | |
| 400 | `invalid_field` | A field breaks its rule. | `field` |
| 400 | `no_viewer_address` | The server needs a viewer address and has none. Core Prime returns this. A registry that does not need one does not. | |
| 413 | `too_large` | The body is over 8 KiB. | |
| 415 | `unsupported_media_type` | `Content-Type` is not `application/json`. | |
| 404 | `unknown_package` | No entry with this id and kind. | |
| 409 | `release_mismatch` | The id is known, but the release and digest match no entry. | `current_release`, a whole number |
| 429 | `rate_limited` | The caller is past the registry's limit. | `retry_after`, and the header `Retry-After`, the same whole number of seconds |
| 500 | `internal` | The registry failed. | |
| 503 | `catalogue_unavailable` | The registry has no catalogue yet. | |
| 428 | `challenge_required` | A check is required. | `challenge`, above |
| 404 | `unknown_challenge` | The challenge code is not known. | |
| 410 | `challenge_expired` | The challenge has ended. | |
| 404 | `unknown_download` | The result id is not known. | |
| 409 | `result_recorded` | A result is already stored. | |

OA treats `404` and `409` on the key request alike, as no longer offered,
and refreshes its catalogue. It does not treat `unknown_package` and
`release_mismatch` differently. Any other 4xx is a refusal and does not try
a mirror. `429` waits. A 5xx, a 201 that does not read, and a 428 that does
not read are retried, and then a mirror may be tried.

The registry sends `retry_after` and the header `Retry-After` as the same
number of whole seconds. Core Prime's wait is at least 60 seconds and can
be about an hour. OA reads the header, not the JSON member, and keeps the
wait it honours to at most 300 seconds.

[examples/error.json](examples/error.json) is a `release_mismatch` with
`current_release`. OA reads `error` and `message` from it. It does not read
`current_release`; the member is for the caller, and the schema allows it
on that code.
