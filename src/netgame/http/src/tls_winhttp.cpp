// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// HTTPS on Windows 7 and later: WinHTTP, loaded with LoadLibrary so the game
// still starts where winhttp.dll is missing or too old to do TLS 1.2. No
// import library is linked. Certificates use the system's check. A failure
// of that check is reported and is never retried as HTTP.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "hop_body.hpp"
#include "tls.hpp"

#include "oa/formats/url.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace oa::netgame::http {
namespace {

constexpr DWORD kNoProxy = 1;
constexpr DWORD kFlagAsync = 0x10000000;
constexpr DWORD kFlagSecure = 0x00800000;
constexpr DWORD kOptionSecureProtocols = 84;
constexpr DWORD kTls12 = 0x00000800;
constexpr DWORD kTls13 = 0x00002000;
constexpr DWORD kOptionDisableFeature = 63;
constexpr DWORD kDisableRedirects = 0x00000002;
constexpr DWORD kQueryStatusCode = 19;
constexpr DWORD kQueryFlagNumber = 0x20000000;
constexpr DWORD kQueryRawHeaders = 21;
constexpr DWORD kStatusSecureFailure = 0x00010000;
constexpr DWORD kStatusHeaders = 0x00020000;
constexpr DWORD kStatusData = 0x00040000;
constexpr DWORD kStatusRead = 0x00080000;
constexpr DWORD kStatusError = 0x00200000;
constexpr DWORD kStatusSend = 0x00400000;
constexpr DWORD kCallbackFlags =
    kStatusSecureFailure | kStatusHeaders | kStatusData | kStatusRead | kStatusError | kStatusSend;
/// Every notification, used when the callback is removed. Zero leaves the
/// notifications the request already has, so a later one can arrive after
/// the handles are closed.
constexpr DWORD kAllNotifications = 0xFFFFFFFFu;
constexpr DWORD kInsufficient = 122;
constexpr DWORD kPending = 997;
constexpr DWORD kErrorTimeout = 12002;
constexpr DWORD kErrorName = 12007;
constexpr DWORD kErrorCancelled = 12017;
constexpr DWORD kErrorConnect = 12029;
constexpr DWORD kErrorReset = 12030;
constexpr DWORD kErrorSecure = 12175;
/// WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT. The four WinHttpSetTimeouts
/// values do not set this one, and a missing value waits with no limit.
constexpr DWORD kOptionReceiveResponseTimeout = 7;
constexpr DWORD kCertCn = 0x00000010;
constexpr DWORD kCertDate = 0x00000020;
constexpr DWORD kCertRevoked = 0x00000004;
constexpr DWORD kCertInvalid = 0x00000002;
constexpr DWORD kCertCa = 0x00000008;

using OpenFn = void*(WINAPI*)(const wchar_t*, DWORD, const wchar_t*, const wchar_t*, DWORD);
using CloseFn = BOOL(WINAPI*)(void*);
using SetOptionFn = BOOL(WINAPI*)(void*, DWORD, void*, DWORD);
using ConnectFn = void*(WINAPI*)(void*, const wchar_t*, WORD, DWORD);
using OpenRequestFn = void*(WINAPI*)(void*,
                                     const wchar_t*,
                                     const wchar_t*,
                                     const wchar_t*,
                                     const wchar_t*,
                                     const wchar_t**,
                                     DWORD);
using SendFn = BOOL(WINAPI*)(void*, const wchar_t*, DWORD, void*, DWORD, DWORD, uintptr_t);
using ReceiveFn = BOOL(WINAPI*)(void*, void*);
using QueryFn = BOOL(WINAPI*)(void*, DWORD, const wchar_t*, void*, DWORD*, DWORD*);
using DataFn = BOOL(WINAPI*)(void*, DWORD*);
using ReadFn = BOOL(WINAPI*)(void*, void*, DWORD, DWORD*);
using TimeoutsFn = BOOL(WINAPI*)(void*, int, int, int, int);
using CallbackFn = void(WINAPI*)(void*, uintptr_t, DWORD, void*, DWORD);
using SetCallbackFn = CallbackFn(WINAPI*)(void*, CallbackFn, DWORD, uintptr_t);

struct Api {
    HMODULE module = nullptr;
    OpenFn open = nullptr;
    CloseFn close = nullptr;
    SetOptionFn set_option = nullptr;
    ConnectFn connect = nullptr;
    OpenRequestFn open_request = nullptr;
    SendFn send = nullptr;
    ReceiveFn receive = nullptr;
    QueryFn query = nullptr;
    DataFn data = nullptr;
    ReadFn read = nullptr;
    TimeoutsFn timeouts = nullptr;
    SetCallbackFn set_callback = nullptr;
};

Api g_api;

template <typename Fn>
Fn load_fn(HMODULE module, const char* name) {
    FARPROC symbol = GetProcAddress(module, name);
    Fn function = nullptr;
    static_assert(sizeof(Fn) == sizeof(symbol));
    std::memcpy(&function, &symbol, sizeof function);
    return function;
}

struct AsyncNote {
    std::mutex mu;
    std::condition_variable cv;
    bool got = false;
    bool closing = false;
    DWORD status = 0;
    DWORD error = 0;
    DWORD secure = 0;
    DWORD available = 0;
    DWORD read_count = 0;
};

struct AsyncResult {
    uintptr_t result{};
    DWORD error{};
};

/// Notes a callback may still touch after the fetch has stopped waiting.
std::mutex g_notes_mu;
std::vector<std::shared_ptr<AsyncNote>> g_notes;

/// Keeps a note alive for the callback until drop_note.
///
/// @return the note
std::shared_ptr<AsyncNote> keep_note() {
    auto note = std::make_shared<AsyncNote>();
    std::lock_guard<std::mutex> lock(g_notes_mu);
    g_notes.push_back(note);
    return note;
}

/// Drops the registry's reference. A callback already holding the note may finish.
///
/// @param note the note; null is ignored
void drop_note(AsyncNote* note) {
    if (note == nullptr)
        return;
    std::lock_guard<std::mutex> lock(g_notes_mu);
    for (auto it = g_notes.begin(); it != g_notes.end(); ++it) {
        if (it->get() == note) {
            g_notes.erase(it);
            return;
        }
    }
}

/// Finds a note the callback may still use.
///
/// @param context the pointer passed to the send
/// @return the note, or null when the fetch has dropped it
std::shared_ptr<AsyncNote> find_note(uintptr_t context) {
    std::lock_guard<std::mutex> lock(g_notes_mu);
    for (const std::shared_ptr<AsyncNote>& note : g_notes) {
        if (reinterpret_cast<uintptr_t>(note.get()) == context)
            return note;
    }
    return {};
}

void WINAPI
status_callback(void* /*handle*/, uintptr_t context, DWORD status, void* info, DWORD length) {
    const std::shared_ptr<AsyncNote> note = find_note(context);
    if (!note)
        return;
    std::lock_guard<std::mutex> lock(note->mu);
    if (note->closing)
        return;
    if (status == kStatusSecureFailure) {
        if (info != nullptr && length >= sizeof(DWORD))
            note->secure = *static_cast<const DWORD*>(info);
        return;
    }
    note->status = status;
    if (status == kStatusError && info != nullptr && length >= sizeof(AsyncResult))
        note->error = static_cast<const AsyncResult*>(info)->error;
    else if (status == kStatusData && info != nullptr && length >= sizeof(DWORD))
        note->available = *static_cast<const DWORD*>(info);
    else if (status == kStatusRead)
        note->read_count = length;
    note->got = true;
    note->cv.notify_one();
}

/// Copies ASCII into a wide string. Registry addresses are printable ASCII.
///
/// @param text the text
/// @return the wide text
std::wstring widen(std::string_view text) {
    std::wstring out;
    out.resize(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
        out[i] = static_cast<wchar_t>(static_cast<unsigned char>(text[i]));
    return out;
}

/// Copies a wide header into ASCII, byte for byte up to 0x7F.
///
/// @param text the wide text
/// @return the text
std::string narrow(const wchar_t* text, std::size_t size) {
    std::string out;
    out.resize(size);
    for (std::size_t i = 0; i < size; ++i)
        out[i] = text[i] <= 0x7F ? static_cast<char>(text[i]) : '?';
    return out;
}

/// The sentence for a certificate WinHTTP rejected. Ignore flags are never set.
///
/// @param flags WINHTTP_CALLBACK_STATUS_SECURE_FAILURE flags
/// @return the sentence
const char* secure_detail(DWORD flags) {
    if ((flags & kCertCn) != 0)
        return "the server's certificate is not for this host";
    if ((flags & kCertDate) != 0)
        return "the server's certificate has expired or is not yet valid";
    if ((flags & kCertRevoked) != 0)
        return "the server's certificate was revoked";
    if ((flags & kCertInvalid) != 0 || (flags & kCertCa) != 0)
        return "the server's certificate is not trusted";
    return "the server's certificate was rejected";
}

/// Maps a WinHTTP error. 12175 is a certificate failure, never an HTTP retry.
///
/// @param error the WinHTTP error
/// @param secure flags captured from the secure-failure callback
/// @param[out] failure the failure
/// @param[out] detail the sentence
void map_error(DWORD error, DWORD secure, Failure& failure, std::string& detail) {
    if (error == kErrorSecure || secure != 0) {
        failure = Failure::certificate;
        detail = secure_detail(secure);
        return;
    }
    if (error == kErrorTimeout) {
        failure = Failure::timed_out;
        detail = "the time limit ran out";
        return;
    }
    if (error == kErrorName) {
        failure = Failure::cannot_resolve;
        detail = "the name could not be resolved";
        return;
    }
    if (error == kErrorCancelled) {
        failure = Failure::cancelled;
        detail = "the fetch was cancelled";
        return;
    }
    if (error == kErrorConnect) {
        failure = Failure::cannot_connect;
        detail = "the connection could not be opened";
        return;
    }
    failure = Failure::connection_lost;
    detail = "the secure connection failed";
    (void)kErrorReset;
}

/// Milliseconds from a start until now, never negative.
///
/// @param from the start
/// @param now the current time
/// @return the elapsed milliseconds
int64_t
elapsed_ms(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point now) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - from).count();
    return elapsed < 0 ? 0 : elapsed;
}

/// Which clock bounds this wait. Opening an address uses the connect limit.
/// Waiting for a byte uses the idle limit. The whole-fetch limit applies to both.
enum class WaitPhase : uint8_t { connect, idle };

/// True when cancel or a clock says this wait is over.
///
/// The system stack's own report is not required. A refused address can be
/// treated as connected and then left waiting in a receive, so this clock is
/// what ends the hop.
///
/// @param clock the clocks
/// @param from when this phase started
/// @param phase connect while the address is opening, idle after that
/// @param[out] failure the failure, when this returns true
/// @param[out] detail the sentence
/// @return true when the hop should stop
bool clocks_fired(
    const FetchClock& clock,
    std::chrono::steady_clock::time_point from,
    WaitPhase phase,
    Failure& failure,
    std::string& detail
) {
    if (clock.cancel != nullptr && clock.cancel->load()) {
        failure = Failure::cancelled;
        detail = "the fetch was cancelled";
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (clock.timeouts.total_ms > 0 &&
        elapsed_ms(clock.started, now) >= static_cast<int64_t>(clock.timeouts.total_ms)) {
        failure = Failure::timed_out;
        detail = "the time limit ran out";
        return true;
    }
    const uint32_t limit =
        phase == WaitPhase::connect ? clock.timeouts.connect_ms : clock.timeouts.idle_ms;
    const std::chrono::steady_clock::time_point origin =
        phase == WaitPhase::connect ? clock.hop : from;
    if (elapsed_ms(origin, now) >= static_cast<int64_t>(limit)) {
        if (phase == WaitPhase::connect) {
            failure = Failure::cannot_connect;
            detail = "the connection could not be opened";
        } else {
            failure = Failure::timed_out;
            detail = "the time limit ran out";
        }
        return true;
    }
    return false;
}

/// What one callback recorded, copied while the note's lock was held.
struct CallbackSnap {
    bool completed = false;
    DWORD status = 0;
    DWORD error = 0;
    DWORD secure = 0;
    DWORD count = 0;
};

/// Waits until the callback records a completion, or a clock fires.
///
/// The recorded fields are copied before the lock is released. A callback
/// that arrives after the wait has ended must not race those reads.
///
/// @param note the callback's note
/// @param clock the clocks
/// @param from when this phase started
/// @param phase connect while the address is opening, idle after that
/// @param[out] failure why the wait stopped
/// @param[out] detail the sentence
/// @return the copy; completed is false when a clock stopped the wait
CallbackSnap wait_callback(
    AsyncNote& note,
    const FetchClock& clock,
    std::chrono::steady_clock::time_point from,
    WaitPhase phase,
    Failure& failure,
    std::string& detail
) {
    CallbackSnap snap;
    std::unique_lock<std::mutex> lock(note.mu);
    while (!note.got) {
        if (clocks_fired(clock, from, phase, failure, detail))
            return snap;
        note.cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    snap.completed = true;
    snap.status = note.status;
    snap.error = note.error;
    snap.secure = note.secure;
    if (note.status == kStatusData)
        snap.count = note.available;
    else if (note.status == kStatusRead)
        snap.count = note.read_count;
    return snap;
}

/// Starts an async call and waits for its callback.
///
/// @param started what the WinHTTP call returned
/// @param note the note, cleared by the caller before the call
/// @param clock the clocks
/// @param from when this phase started
/// @param phase connect while the address is opening, idle after that
/// @param[out] exchange the hop, filled when this returns false
/// @param[out] count bytes the callback reported, when it reported a count
/// @return true when the callback reported the step finished
bool finish_async(
    BOOL started,
    AsyncNote& note,
    const FetchClock& clock,
    std::chrono::steady_clock::time_point from,
    WaitPhase phase,
    HttpsExchange& exchange,
    DWORD* count
) {
    if (started == FALSE) {
        const DWORD error = GetLastError();
        if (error != kPending) {
            DWORD secure = 0;
            {
                std::lock_guard<std::mutex> lock(note.mu);
                secure = note.secure;
            }
            map_error(error, secure, exchange.failure, exchange.detail);
            return false;
        }
    }
    const CallbackSnap snap =
        wait_callback(note, clock, from, phase, exchange.failure, exchange.detail);
    if (!snap.completed)
        return false;
    if (snap.status == kStatusError) {
        map_error(snap.error, snap.secure, exchange.failure, exchange.detail);
        return false;
    }
    if (count != nullptr)
        *count = snap.count;
    return true;
}

/// Clears the note before one async call.
///
/// @param note the note
void reset_note(AsyncNote& note) {
    std::lock_guard<std::mutex> lock(note.mu);
    note.got = false;
    note.status = 0;
    note.error = 0;
    note.available = 0;
    note.read_count = 0;
}

/// Reads the status and the headers. Synchronous once the headers have arrived.
///
/// @param request the request
/// @param[out] exchange the status and headers
/// @return false when the status could not be read
bool read_headers(void* request, HttpsExchange& exchange) {
    // No header index: an index picks the nth header of one name, and WinHTTP
    // finds no header at index 0xFFFFFFFF.
    DWORD status = 0;
    DWORD status_size = sizeof status;
    if (g_api.query(
            request, kQueryStatusCode | kQueryFlagNumber, nullptr, &status, &status_size, nullptr
        ) == FALSE)
        return false;
    exchange.status = static_cast<int>(status);
    DWORD bytes = 0;
    g_api.query(request, kQueryRawHeaders, nullptr, nullptr, &bytes, nullptr);
    if (GetLastError() != kInsufficient || bytes < sizeof(wchar_t))
        return true;
    std::vector<wchar_t> wide(bytes / sizeof(wchar_t) + 1, L'\0');
    if (g_api.query(request, kQueryRawHeaders, nullptr, wide.data(), &bytes, nullptr) == FALSE)
        return true;
    const std::size_t chars = bytes / sizeof(wchar_t);
    std::size_t at = 0;
    while (at < chars && wide[at] != L'\0') {
        std::size_t end = at;
        while (end < chars && wide[end] != L'\0')
            ++end;
        const std::string line = narrow(wide.data() + at, end - at);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos && colon > 0) {
            Header header;
            header.name = line.substr(0, colon);
            std::size_t value_at = colon + 1;
            if (value_at < line.size() && line[value_at] == ' ')
                ++value_at;
            header.value = line.substr(value_at);
            exchange.headers.push_back(std::move(header));
        }
        at = end + 1;
    }
    return true;
}

/// Closes the three handles. The callback is detached first.
///
/// @param note the note; further callbacks are ignored
/// @param request the request; may be null
/// @param connection the connection; may be null
/// @param session the session; may be null
void close_all(AsyncNote& note, void* request, void* connection, void* session) {
    {
        std::lock_guard<std::mutex> lock(note.mu);
        note.closing = true;
    }
    if (request != nullptr && g_api.set_callback != nullptr)
        g_api.set_callback(request, nullptr, kAllNotifications, 0);
    if (request != nullptr)
        g_api.close(request);
    if (connection != nullptr)
        g_api.close(connection);
    if (session != nullptr)
        g_api.close(session);
    drop_note(&note);
}

/// True when TLS 1.2 can be selected on a session.
///
/// @param session an open session
/// @return false when this WinHTTP is too old
bool allow_tls12(void* session) {
    DWORD protocols = kTls12 | kTls13;
    if (g_api.set_option(session, kOptionSecureProtocols, &protocols, sizeof protocols) != FALSE)
        return true;
    protocols = kTls12;
    return g_api.set_option(session, kOptionSecureProtocols, &protocols, sizeof protocols) != FALSE;
}

/// One millisecond when a limit is zero. The stack treats zero as no limit.
///
/// @param limit the limit, in milliseconds
/// @return the value passed to the stack
int stack_timeout(uint32_t limit) {
    if (limit == 0)
        return 1;
    if (limit > static_cast<uint32_t>(INT32_MAX))
        return INT32_MAX;
    return static_cast<int>(limit);
}

/// Gives one handle the fetch's clocks.
///
/// The receive-response limit is separate from the four timeout values.
/// While the address is still opening it is the connect limit, so a receive
/// that follows a refused connection the stack treated as open cannot outlast
/// that limit. After the request has been sent it is the idle limit.
///
/// @param handle a session or a request
/// @param clock the clocks
/// @param response_ms how long a receive of the response may wait
void apply_clocks(void* handle, const FetchClock& clock, uint32_t response_ms) {
    if (handle == nullptr)
        return;
    const int connect = stack_timeout(clock.timeouts.connect_ms);
    const int idle = stack_timeout(clock.timeouts.idle_ms);
    const int response = stack_timeout(response_ms);
    if (g_api.timeouts != nullptr)
        g_api.timeouts(handle, connect, connect, idle, idle);
    if (g_api.set_option != nullptr) {
        DWORD response_timeout = static_cast<DWORD>(response);
        g_api.set_option(
            handle, kOptionReceiveResponseTimeout, &response_timeout, sizeof response_timeout
        );
    }
}

} // namespace

struct TlsSession {};

HttpsTransport probe_https_transport() noexcept {
    HMODULE module = LoadLibraryA("winhttp.dll");
    if (module == nullptr)
        return HttpsTransport::none;
    Api api;
    api.module = module;
    api.open = load_fn<OpenFn>(module, "WinHttpOpen");
    api.close = load_fn<CloseFn>(module, "WinHttpCloseHandle");
    api.set_option = load_fn<SetOptionFn>(module, "WinHttpSetOption");
    api.connect = load_fn<ConnectFn>(module, "WinHttpConnect");
    api.open_request = load_fn<OpenRequestFn>(module, "WinHttpOpenRequest");
    api.send = load_fn<SendFn>(module, "WinHttpSendRequest");
    api.receive = load_fn<ReceiveFn>(module, "WinHttpReceiveResponse");
    api.query = load_fn<QueryFn>(module, "WinHttpQueryHeaders");
    api.data = load_fn<DataFn>(module, "WinHttpQueryDataAvailable");
    api.read = load_fn<ReadFn>(module, "WinHttpReadData");
    api.timeouts = load_fn<TimeoutsFn>(module, "WinHttpSetTimeouts");
    api.set_callback = load_fn<SetCallbackFn>(module, "WinHttpSetStatusCallback");
    if (api.open == nullptr || api.close == nullptr || api.set_option == nullptr ||
        api.connect == nullptr || api.open_request == nullptr || api.send == nullptr ||
        api.receive == nullptr || api.query == nullptr || api.data == nullptr ||
        api.read == nullptr || api.set_callback == nullptr) {
        FreeLibrary(module);
        return HttpsTransport::none;
    }
    void* session = api.open(L"OpenAnnihilation", kNoProxy, nullptr, nullptr, 0);
    if (session == nullptr) {
        FreeLibrary(module);
        return HttpsTransport::none;
    }
    const bool tls = api.set_option != nullptr && [&] {
        DWORD protocols = kTls12 | kTls13;
        if (api.set_option(session, kOptionSecureProtocols, &protocols, sizeof protocols) != FALSE)
            return true;
        protocols = kTls12;
        return api.set_option(session, kOptionSecureProtocols, &protocols, sizeof protocols) !=
               FALSE;
    }();
    api.close(session);
    if (!tls) {
        FreeLibrary(module);
        return HttpsTransport::none;
    }
    g_api = api;
    return HttpsTransport::platform;
}

TlsSession* tls_open(
    intptr_t /*fd*/,
    std::string_view /*host*/,
    bool /*ip_literal*/,
    Failure& failure,
    std::string& detail
) {
    failure = Failure::connection_lost;
    detail = "the secure connection failed";
    return nullptr;
}

void tls_close(TlsSession* session) noexcept {
    delete session;
}

TlsIoResult tls_handshake(TlsSession* /*session*/) {
    TlsIoResult result;
    result.kind = TlsIoResult::Kind::failed;
    result.failure = Failure::connection_lost;
    result.detail = "the secure connection failed";
    return result;
}

TlsIoResult tls_read(TlsSession* /*session*/, uint8_t* /*bytes*/, std::size_t /*size*/) {
    return tls_handshake(nullptr);
}

TlsIoResult tls_write(TlsSession* /*session*/, const uint8_t* /*bytes*/, std::size_t /*size*/) {
    return tls_handshake(nullptr);
}

HttpsExchange fetch_https_hop(
    const Request& request,
    const oa::formats::url::Url& url,
    Method method,
    const std::vector<uint8_t>& body,
    std::string_view user_agent,
    BodySink& sink,
    const FetchClock& clock
) {
    HttpsExchange exchange;
    if (g_api.open == nullptr) {
        exchange.failure = Failure::https_unsupported;
        exchange.detail = "This system cannot use HTTPS";
        return exchange;
    }
    const std::wstring agent = widen(user_agent);
    void* session = g_api.open(agent.c_str(), kNoProxy, nullptr, nullptr, kFlagAsync);
    if (session == nullptr) {
        exchange.failure = Failure::cannot_connect;
        exchange.detail = "the connection could not be opened";
        return exchange;
    }
    if (!allow_tls12(session)) {
        g_api.close(session);
        exchange.failure = Failure::https_unsupported;
        exchange.detail = "This system cannot use HTTPS";
        return exchange;
    }
    apply_clocks(session, clock, clock.timeouts.connect_ms);
    const std::shared_ptr<AsyncNote> note = keep_note();
    const std::wstring host = widen(url.host);
    void* connection = g_api.connect(session, host.c_str(), static_cast<WORD>(url.port), 0);
    if (connection == nullptr) {
        g_api.close(session);
        drop_note(note.get());
        exchange.failure = Failure::cannot_connect;
        exchange.detail = "the connection could not be opened";
        return exchange;
    }
    const wchar_t* verb = L"GET";
    if (method == Method::head)
        verb = L"HEAD";
    else if (method == Method::post)
        verb = L"POST";
    const std::wstring target = widen(url.target);
    const wchar_t* accepts[] = {L"*/*", nullptr};
    void* handle = g_api.open_request(
        connection, verb, target.c_str(), nullptr, nullptr, accepts, kFlagSecure
    );
    if (handle == nullptr) {
        close_all(*note, nullptr, connection, session);
        exchange.failure = Failure::cannot_connect;
        exchange.detail = "the connection could not be opened";
        return exchange;
    }
    apply_clocks(handle, clock, clock.timeouts.connect_ms);
    DWORD disable = kDisableRedirects;
    g_api.set_option(handle, kOptionDisableFeature, &disable, sizeof disable);
    // The last argument is reserved and must be null. The callback's context
    // is the pointer passed to WinHttpSendRequest. The invalid callback is
    // all-bits-one; copying the bits avoids a cast from an integer to a
    // function pointer.
    CallbackFn invalid_callback = nullptr;
    const auto invalid_bits = static_cast<uintptr_t>(-1);
    static_assert(sizeof invalid_callback == sizeof invalid_bits);
    std::memcpy(&invalid_callback, &invalid_bits, sizeof invalid_callback);
    const CallbackFn installed = g_api.set_callback(handle, status_callback, kCallbackFlags, 0);
    if (installed == invalid_callback) {
        close_all(*note, handle, connection, session);
        exchange.failure = Failure::connection_lost;
        exchange.detail = "the secure connection failed";
        return exchange;
    }

    std::wstring headers;
    if (request.range_from.has_value())
        headers += L"Range: bytes=" + widen(std::to_string(*request.range_from)) + L"-\r\n";
    if (request.accept_gzip)
        headers += L"Accept-Encoding: gzip\r\n";
    if (method == Method::post) {
        const std::string type = request.content_type.empty()
                                     ? std::string("application/octet-stream")
                                     : request.content_type;
        headers += L"Content-Type: " + widen(type) + L"\r\n";
    }
    for (const Header& header : request.headers)
        headers += widen(header.name) + L": " + widen(header.value) + L"\r\n";

    reset_note(*note);
    const DWORD body_size = static_cast<DWORD>(body.size());
    void* body_bytes = body.empty() ? nullptr : const_cast<uint8_t*>(body.data());
    const BOOL sent = g_api.send(
        handle,
        headers.empty() ? nullptr : headers.c_str(),
        static_cast<DWORD>(-1),
        method == Method::post ? body_bytes : nullptr,
        method == Method::post ? body_size : 0,
        method == Method::post ? body_size : 0,
        reinterpret_cast<uintptr_t>(note.get())
    );
    if (!finish_async(sent, *note, clock, clock.hop, WaitPhase::connect, exchange, nullptr)) {
        close_all(*note, handle, connection, session);
        return exchange;
    }
    auto idle_from = std::chrono::steady_clock::now();
    apply_clocks(handle, clock, clock.timeouts.idle_ms);
    reset_note(*note);
    const BOOL received = g_api.receive(handle, nullptr);
    if (!finish_async(received, *note, clock, idle_from, WaitPhase::idle, exchange, nullptr)) {
        close_all(*note, handle, connection, session);
        return exchange;
    }
    if (!read_headers(handle, exchange)) {
        exchange.failure = Failure::bad_response;
        exchange.detail = "the response could not be read";
        close_all(*note, handle, connection, session);
        return exchange;
    }
    HopBody hop(request, sink, exchange, oa::formats::url::url_text(url));
    if (!hop.take_head()) {
        close_all(*note, handle, connection, session);
        return exchange;
    }
    if (!hop.redirect() && method != Method::head) {
        while (exchange.failure == Failure::none) {
            idle_from = std::chrono::steady_clock::now();
            reset_note(*note);
            const BOOL asked = g_api.data(handle, nullptr);
            DWORD available = 0;
            if (!finish_async(
                    asked, *note, clock, idle_from, WaitPhase::idle, exchange, &available
                ))
                break;
            if (available == 0)
                break;
            DWORD want = available;
            if (want > 16384)
                want = 16384;
            std::vector<uint8_t> buffer(want);
            idle_from = std::chrono::steady_clock::now();
            reset_note(*note);
            const BOOL reading = g_api.read(handle, buffer.data(), want, nullptr);
            DWORD read_count = 0;
            if (!finish_async(
                    reading, *note, clock, idle_from, WaitPhase::idle, exchange, &read_count
                ))
                break;
            if (read_count == 0)
                break;
            buffer.resize(read_count);
            if (!hop.take_bytes(buffer))
                break;
        }
        if (exchange.failure == Failure::none && !hop.finish() &&
            exchange.failure == Failure::none) {
            exchange.failure = Failure::decode_failed;
            exchange.detail = "the body could not be decoded";
        }
    }
    close_all(*note, handle, connection, session);
    return exchange;
}

} // namespace oa::netgame::http
