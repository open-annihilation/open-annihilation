# Relay tunnel

The client side of the overlay relay. One TCP connection carries datagrams and
streams of bytes between overlay addresses. The game calls it once a frame
while a battle is carried through the relay. Target `oa-boneyards-tunnel`, headers
`oa/boneyards/tunnel.hpp` and `oa/boneyards/tunnel/`, namespace
`oa::boneyards::tunnel`.

`Tunnel` is the session. `connect` names the relay, this client's overlay
address and the key. `send_datagram` and `send_stream` queue one payload for a
destination overlay address; a datagram is UDP and a stream of bytes is TCP, and
either port equal to 47624 sets a classification bit the route ignores.
`pump` does not wait. `take` returns what arrived for this client's overlay
address. `state` and `fault` say how the connection is doing, and `close` stops
the retries.

## Entry points

`connect`, `send_datagram`, `send_stream`, `pump`, `take`, `state`, `fault`,
`fault_text`, `close`, `set_clock`.

`set_clock` replaces the steady clock `pump` reads, so a caller can move time
instead of waiting out a retry.

The frames, the HMAC and the sequence state are also public
(`oa/boneyards/tunnel/wire.hpp`, `hmac.hpp`, `stream.hpp`) so a test can drive
them without a socket.

## State

`connect` remembers the host, the port, the overlay address and the key. `pump`
reads and writes the socket, the read buffer, the write buffer, the resend ring
and the receive queue. `take` removes one packet from that queue. `close` drops
the socket and leaves packets already queued.

A retry keeps the sequence numbers. `connect` starts them over; the relay keeps
a session for a key identifier, so a new session uses a new identifier.

## Invariants

- Every multi-byte integer is big-endian. The version byte is 1.
- A data frame is 17 bytes plus its payload. An empty payload is a whole frame.
- A challenge is 18 bytes: the version, kind 1, and a 16-byte nonce. A response
  is 42 bytes: the version, kind 2, the overlay address, the key identifier and
  a 32-byte HMAC. The HMAC is over the 20 bytes of `coreprime-overlay-v1` and
  then the nonce, keyed by the overlay key.
- A drain notice is 3 bytes, kind 3. It is not delivered as a packet. The client
  reconnects at once and does not count that as another failed attempt.
- A stream record is 18 bytes plus its inner frame: an 8-byte sequence, an
  8-byte cumulative acknowledgement and a 2-byte length. Sequences start at 1.
  Sequence 0 carries only an acknowledgement, or the challenge, the response or
  a drain notice, and is not kept for resend.
- The client delivers the next sequence only. A duplicate is ignored. A record
  that arrives ahead of the next one is kept, up to 64 records and no further
  ahead than that, and delivered when the missing records arrive. The hold
  survives a reconnect. A gap still open after half a second drops the
  connection; the next handshake is a resume and the replay fills the gap.
  Each record is delivered once, in order.
- A record for this client is acknowledged only once it is stored for `take`.
  `take` is the only way out of that queue, so a record is never acknowledged
  and then discarded. A full queue is not read further.
- The receive queue holds 512 packets. A full queue is not read further, and a
  record that does not fit is not acknowledged, so the sender keeps it and
  retries. The queue never discards a packet it has acknowledged.
- The resend ring holds 256 records. A full ring drops the oldest. One inner
  record is at most 2048 bytes, so a payload is at most 2031 bytes.
- `pump` never blocks. The socket is non-blocking. There is one connection and
  no second thread.
- Tests listen on 127.0.0.1 only.

## Tests

`boneyards-tunnel-wire` checks the frame bytes, including the Go `overlaywire`
cases and the Rust `overlay_wire` and `overlay_encap` cases.
`boneyards-tunnel-hmac` checks HMAC-SHA256, including the RFC 4231 cases in the
Rust `crypto` tests. `boneyards-tunnel-stream` checks the Rust `overlay_stream`
cases. `boneyards-tunnel-session` runs two clients through an in-process relay
on 127.0.0.1: datagrams and streams of bytes cross, a cut connection delivers
each record once and in order, a challenge that is not a challenge is refused,
and a drain notice reconnects in time for a later datagram. A burst of 200
records queued between takes is delivered in order. Records past the receive
queue stay unacknowledged until `take` makes room. A record one ahead of a gap
is delivered when the missing record arrives, without connecting again. A gap
that does not fill connects again on its own, and the record it was holding is
delivered when the missing one then arrives. Silence for the stall interval,
while a record is still unacknowledged, connects again and that record arrives
once. A peer that accepts nothing more is reported as a lost connection after
the resume window, and is not dialled again.

## Limitations

IPv4 only. The relay is the only path: there is no direct path and no ICE.
This module does not read launch fields. A battle installs it to carry the match.

One connection, retried with a delay that starts at a quarter of a second,
doubles and stops at four seconds, plus a fraction of a second of jitter. The
retries stop six minutes after the connection was lost, which is as long as
the relay keeps the session. The resume uses the same key identifier. It sends
again from the last acknowledged sequence, and the acknowledgement it presents
asks the relay to replay from the first missing one. While a record is still
unacknowledged, five seconds with nothing arriving connects again in the same
way. A record larger than 2048 bytes is refused. The ring drops an
unacknowledged record once 256 newer ones are waiting. The receive queue holds
512 packets and then stops reading. A gap that stays open for half a second
connects again instead of waiting for the caller. The record it was holding is
kept.

An idle connection stays up until the relay closes it or `close` is called.
`close` sends no frame: the protocol has no end or goodbye. Kind 3 is the
relay's drain notice, which tells this client to connect again, and is not a
goodbye the client may send. The relay therefore keeps a closed connection
for its resume window. A follow-up on the relay would add an end frame.
The key does not change until the next `connect`, and there is no key rotation.
A second `connect` with the same key identifier starts this client's sequences
over, while the relay keeps that identifier's session and treats the connection
as a resume. A new match uses a new key identifier.
