# HTTP client

A blocking HTTP/1.1 client for a worker thread. It fetches catalogues and
packages. `https` uses the system's TLS where this system has it, and is
read as `http` where it does not. A certificate the system rejects fails
the fetch and is not tried again as `http`. OA ships no TLS library and no
certificate list. `fetch` runs on the thread that calls it and is never
called on the game's main thread. One client serves one thread. Two
clients on two threads share nothing.

Library `oa-netgame-http`, header `include/oa/netgame/http/client.hpp`,
namespace `oa::netgame::http`.

## Entry points

- `Client` fetches one address at a time. `fetch` takes a `BodySink` or a
  byte vector. `close_idle` drops every pooled connection. The destructor
  does the same.
- `Request` names the method (GET, HEAD or POST), the address, the caller's
  headers, a POST body, a byte range, gzip, the redirect allow-list, the
  time and size limits, a cancel flag and a progress callback.
- `encode_form` (`form.hpp`) builds a `multipart/form-data` body and its
  content type from named text and file parts. The caller posts that body.
  A boundary that already occurs in a part is replaced.
- `Response` is the outcome: a failure, the status, the headers, the address
  after redirects, the content length as sent, a 206's content range, and
  how many decoded bytes the sink was given. `failure_text` is a short
  phrase for a log line.
- `BodySink` receives the final response: `begin` once, before any body
  byte, then `write` for each piece in order. `MemorySink` keeps that body
  in memory, up to the limit it was given.
- `ClientOptions` sets the User-Agent, how many idle connections one host
  and port may hold, how long an idle connection may wait, and the resolver.
  An empty User-Agent sends `OpenAnnihilation/` and the engine version.

## State

The client keeps a pool of idle plain-HTTP connections, keyed by host and
port. A TLS connection is never pooled. A
response that can carry another request is put back, up to
`idle_connections_per_server` (two unless the options say otherwise). A
connection that has waited longer than `keep_alive_ms` (30 seconds unless
the options say otherwise) is closed instead of reused. The pool is also
closed after `Connection: close`, after an HTTP/1.0 answer without
keep-alive, after a body read to the close, and after any failure.
`close_idle` and the destructor close whatever is still waiting.

## Invariants

- An `http` or `https` address is fetched. `https` uses the system's TLS
  where this system has it. Where it does not, the scheme becomes `http`
  before any connection, and a port the text named is kept. A rejected
  certificate fails the fetch and is not fetched again as `http`.
- Only IPv4. A name is resolved to one IPv4 address. A numeric address is
  read on the calling thread; any other name runs on a detached helper so
  the caller can walk away.
- A fetch never blocks past its connect, idle or total limit, and it notices
  a cancel within 50 milliseconds, including while a name is looked up.
  On Windows the system stack is held to those same limits: the wait ends
  when a limit runs out even if the stack has not reported an error, which
  is what happens when a refused address is treated as open and then left
  in a receive.
- Nothing is read without a bound: the status and headers stop at
  `max_header_bytes`, and the body stops at `max_body_bytes` after decoding.
- A GET or HEAD whose reused connection fails before the first response byte
  is sent once more on a new connection. A POST is never sent twice.
- A redirect is followed to the address's own host when the port is the
  same or is the port the target scheme uses by default, or to a host the
  allow-list names. An absolute `https` location is read as `http` when TLS
  is off. Only the final answer reaches the sink.
- A 200 answer to a range request is the whole resource from byte 0. A 206
  names the range it carried. A 416 is status 416 with no failure.

## Tests

`net-http-response` (`tests/response_reader_test.cpp`) feeds the response
reader whole messages and one byte at a time: length, chunked and read to
the close, an interim 100, HTTP/1.0, chunk extensions and trailers, and the
malformed forms and size limits.

`net-http` (`tests/client_test.cpp`) drives the client against the HTTP
fixture on 127.0.0.1. Names go through a test resolver. No test reaches the
network or the system's resolver.

## Limitations

- No proxies, cookies, authentication or IPv6. TLS is the system's, loaded
  at run time. A missing library is "no TLS", and the game still starts.
- One range, from a byte to the end. Several ranges are not asked for.
- A lookup that runs out of time leaves its helper thread running until the
  resolver answers. The caller has already returned.
