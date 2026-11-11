// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// HTTPS on Linux and the Steam Deck: the system's libssl 3 or 1.1, and the
// matching libcrypto, loaded with dlopen. No OpenSSL header is included, so
// the build does not need the development package. The game starts when the
// libraries are missing; a fetch then reads https as http. Certificates are
// checked with the system's store. A failed check is a failure, not HTTP.

#include "tls.hpp"

#include <climits>
#include <cstdint>
#include <cstring>
#include <string>

#include <dlfcn.h>

namespace oa::netgame::http {
namespace {

struct SslCtx;
struct Ssl;
struct VerifyParam;

using MethodFn = const void* (*)();
using CtxNewFn = SslCtx* (*)(const void*);
using CtxFreeFn = void (*)(SslCtx*);
using CtxVerifyFn = void (*)(SslCtx*, int, int (*)(int, void*));
using CtxPathsFn = int (*)(SslCtx*);
using CtxCtrlFn = long (*)(SslCtx*, int, long, void*);
using SslNewFn = Ssl* (*)(SslCtx*);
using SslFreeFn = void (*)(Ssl*);
using SetFdFn = int (*)(Ssl*, int);
using SetHostFn = int (*)(Ssl*, const char*);
using CtrlFn = long (*)(Ssl*, int, long, void*);
using ConnectStateFn = void (*)(Ssl*);
using HandshakeFn = int (*)(Ssl*);
using GetErrorFn = int (*)(const Ssl*, int);
using VerifyResultFn = long (*)(const Ssl*);
using ReadFn = int (*)(Ssl*, void*, int);
using WriteFn = int (*)(Ssl*, const void*, int);
using GetParamFn = VerifyParam* (*)(Ssl*);
using SetIpFn = int (*)(VerifyParam*, const char*);
using InitFn = int (*)(uint64_t, const void*);

/// The symbols one pair of libraries provided. Null until the probe succeeds.
struct Openssl {
    void* crypto = nullptr;
    void* ssl = nullptr;
    MethodFn method = nullptr;
    CtxNewFn ctx_new = nullptr;
    CtxFreeFn ctx_free = nullptr;
    CtxVerifyFn ctx_verify = nullptr;
    CtxPathsFn ctx_paths = nullptr;
    CtxCtrlFn ctx_ctrl = nullptr;
    SslNewFn ssl_new = nullptr;
    SslFreeFn ssl_free = nullptr;
    SetFdFn set_fd = nullptr;
    SetHostFn set_host = nullptr;
    CtrlFn ctrl = nullptr;
    ConnectStateFn connect_state = nullptr;
    HandshakeFn handshake = nullptr;
    GetErrorFn get_error = nullptr;
    VerifyResultFn verify_result = nullptr;
    ReadFn read = nullptr;
    WriteFn write = nullptr;
    GetParamFn get_param = nullptr;
    SetIpFn set_ip = nullptr;
    SslCtx* ctx = nullptr;
};

Openssl g_ssl;

/// Copies a dlsym result into a function pointer without a cast warning.
///
/// @param handle the library
/// @param name the symbol
/// @return the function, or null
template <typename Fn>
Fn load_symbol(void* handle, const char* name) {
    void* symbol = dlsym(handle, name);
    Fn function = nullptr;
    static_assert(sizeof(Fn) == sizeof(symbol));
    std::memcpy(&function, &symbol, sizeof function);
    return function;
}

/// Drops a pair that cannot be used.
void drop_libraries() {
    if (g_ssl.ctx != nullptr && g_ssl.ctx_free != nullptr)
        g_ssl.ctx_free(g_ssl.ctx);
    g_ssl.ctx = nullptr;
    if (g_ssl.ssl != nullptr)
        dlclose(g_ssl.ssl);
    if (g_ssl.crypto != nullptr)
        dlclose(g_ssl.crypto);
    g_ssl.ssl = nullptr;
    g_ssl.crypto = nullptr;
}

/// Loads one version's libraries and the symbols a handshake needs.
///
/// @param ssl_name libssl's file name
/// @param crypto_name the matching libcrypto
/// @return true when the minimum TLS version can be set
bool bind_pair(const char* ssl_name, const char* crypto_name) {
    g_ssl = {};
    g_ssl.crypto = dlopen(crypto_name, RTLD_NOW | RTLD_LOCAL);
    g_ssl.ssl = dlopen(ssl_name, RTLD_NOW | RTLD_LOCAL);
    if (g_ssl.crypto == nullptr || g_ssl.ssl == nullptr) {
        drop_libraries();
        return false;
    }
    g_ssl.method = load_symbol<MethodFn>(g_ssl.ssl, "TLS_client_method");
    g_ssl.ctx_new = load_symbol<CtxNewFn>(g_ssl.ssl, "SSL_CTX_new");
    g_ssl.ctx_free = load_symbol<CtxFreeFn>(g_ssl.ssl, "SSL_CTX_free");
    g_ssl.ctx_verify = load_symbol<CtxVerifyFn>(g_ssl.ssl, "SSL_CTX_set_verify");
    g_ssl.ctx_paths = load_symbol<CtxPathsFn>(g_ssl.ssl, "SSL_CTX_set_default_verify_paths");
    // SSL_CTX_set_min_proto_version is a macro over SSL_CTX_ctrl in OpenSSL
    // 1.1 and 3, not a symbol either library exports.
    g_ssl.ctx_ctrl = load_symbol<CtxCtrlFn>(g_ssl.ssl, "SSL_CTX_ctrl");
    g_ssl.ssl_new = load_symbol<SslNewFn>(g_ssl.ssl, "SSL_new");
    g_ssl.ssl_free = load_symbol<SslFreeFn>(g_ssl.ssl, "SSL_free");
    g_ssl.set_fd = load_symbol<SetFdFn>(g_ssl.ssl, "SSL_set_fd");
    g_ssl.set_host = load_symbol<SetHostFn>(g_ssl.ssl, "SSL_set1_host");
    g_ssl.ctrl = load_symbol<CtrlFn>(g_ssl.ssl, "SSL_ctrl");
    g_ssl.connect_state = load_symbol<ConnectStateFn>(g_ssl.ssl, "SSL_set_connect_state");
    g_ssl.handshake = load_symbol<HandshakeFn>(g_ssl.ssl, "SSL_do_handshake");
    g_ssl.get_error = load_symbol<GetErrorFn>(g_ssl.ssl, "SSL_get_error");
    g_ssl.verify_result = load_symbol<VerifyResultFn>(g_ssl.ssl, "SSL_get_verify_result");
    g_ssl.read = load_symbol<ReadFn>(g_ssl.ssl, "SSL_read");
    g_ssl.write = load_symbol<WriteFn>(g_ssl.ssl, "SSL_write");
    g_ssl.get_param = load_symbol<GetParamFn>(g_ssl.ssl, "SSL_get0_param");
    g_ssl.set_ip = load_symbol<SetIpFn>(g_ssl.crypto, "X509_VERIFY_PARAM_set1_ip_asc");
    if (g_ssl.method == nullptr || g_ssl.ctx_new == nullptr || g_ssl.ctx_free == nullptr ||
        g_ssl.ctx_verify == nullptr || g_ssl.ctx_paths == nullptr || g_ssl.ctx_ctrl == nullptr ||
        g_ssl.ssl_new == nullptr || g_ssl.ssl_free == nullptr || g_ssl.set_fd == nullptr ||
        g_ssl.set_host == nullptr || g_ssl.ctrl == nullptr || g_ssl.connect_state == nullptr ||
        g_ssl.handshake == nullptr || g_ssl.get_error == nullptr ||
        g_ssl.verify_result == nullptr || g_ssl.read == nullptr || g_ssl.write == nullptr) {
        drop_libraries();
        return false;
    }
    // An old library that cannot refuse TLS older than 1.2 is not used.
    const InitFn init = load_symbol<InitFn>(g_ssl.ssl, "OPENSSL_init_ssl");
    if (init != nullptr)
        init(0, nullptr);
    SslCtx* ctx = g_ssl.ctx_new(g_ssl.method());
    if (ctx == nullptr) {
        drop_libraries();
        return false;
    }
    constexpr int verify_peer = 1;
    constexpr int tls_1_2 = 0x0303;
    // SSL_CTX_set_min_proto_version: SSL_CTRL_SET_MIN_PROTO_VERSION.
    constexpr int set_min_proto_version = 123;
    g_ssl.ctx_verify(ctx, verify_peer, nullptr);
    // The system's store. A failure here still leaves verification on, so a
    // certificate with no trusted root is rejected.
    (void)g_ssl.ctx_paths(ctx);
    if (g_ssl.ctx_ctrl(ctx, set_min_proto_version, tls_1_2, nullptr) != 1) {
        g_ssl.ctx_free(ctx);
        drop_libraries();
        return false;
    }
    g_ssl.ctx = ctx;
    return true;
}

/// The sentence a player reads for a certificate the system rejected.
///
/// @param code SSL_get_verify_result
/// @return the sentence
const char* verify_reason(long code) {
    switch (code) {
    case 10:
        return "the server's certificate has expired";
    case 9:
        return "the server's certificate is not yet valid";
    case 18:
        return "the server's certificate is self-signed and not trusted";
    case 19:
        return "the server's certificate chain is self-signed and not trusted";
    case 20:
        return "the server's certificate is not trusted";
    case 62:
        return "the server's certificate is not for this host";
    default:
        return "the server's certificate was rejected";
    }
}

/// A failed step, from the verify result when there is one.
///
/// @param ssl the session
/// @param rc what the call returned
/// @return the step
TlsIoResult failed_step(Ssl* ssl, int rc) {
    TlsIoResult result;
    if (ssl == nullptr || g_ssl.get_error == nullptr) {
        result.kind = TlsIoResult::Kind::failed;
        result.failure = Failure::connection_lost;
        result.detail = "the secure connection failed";
        return result;
    }
    const int error = g_ssl.get_error(ssl, rc);
    constexpr int want_read = 2;
    constexpr int want_write = 3;
    constexpr int zero_return = 6;
    if (error == want_read) {
        result.kind = TlsIoResult::Kind::want_read;
        return result;
    }
    if (error == want_write) {
        result.kind = TlsIoResult::Kind::want_write;
        return result;
    }
    if (error == zero_return) {
        result.kind = TlsIoResult::Kind::closed;
        return result;
    }
    const long verify = g_ssl.verify_result(ssl);
    result.kind = TlsIoResult::Kind::failed;
    if (verify != 0) {
        result.failure = Failure::certificate;
        result.detail = verify_reason(verify);
        return result;
    }
    result.failure = Failure::connection_lost;
    result.detail = "the secure connection failed";
    return result;
}

} // namespace

struct TlsSession {
    Ssl* ssl = nullptr;
};

HttpsTransport probe_https_transport() noexcept {
    if (bind_pair("libssl.so.3", "libcrypto.so.3") ||
        bind_pair("libssl.so.1.1", "libcrypto.so.1.1"))
        return HttpsTransport::socket;
    return HttpsTransport::none;
}

TlsSession* tls_open(
    intptr_t fd, std::string_view host, bool ip_literal, Failure& failure, std::string& detail
) {
    if (g_ssl.ctx == nullptr || fd < 0 || static_cast<unsigned long long>(fd) > INT_MAX) {
        failure = Failure::connection_lost;
        detail = "the secure connection failed";
        return nullptr;
    }
    Ssl* ssl = g_ssl.ssl_new(g_ssl.ctx);
    if (ssl == nullptr) {
        failure = Failure::connection_lost;
        detail = "the secure connection failed";
        return nullptr;
    }
    if (g_ssl.set_fd(ssl, static_cast<int>(fd)) != 1) {
        g_ssl.ssl_free(ssl);
        failure = Failure::connection_lost;
        detail = "the secure connection failed";
        return nullptr;
    }
    const std::string host_text(host);
    if (!ip_literal) {
        if (g_ssl.set_host(ssl, host_text.c_str()) != 1) {
            g_ssl.ssl_free(ssl);
            failure = Failure::connection_lost;
            detail = "the secure connection failed";
            return nullptr;
        }
        // SSL_set_tlsext_host_name: the server name, skipped for an address.
        constexpr int set_server_name = 55;
        g_ssl.ctrl(ssl, set_server_name, 0, const_cast<char*>(host_text.c_str()));
    } else {
        // An address is checked as an address. Without that check the
        // certificate's name would not be compared with what was dialled.
        VerifyParam* param =
            g_ssl.get_param != nullptr && g_ssl.set_ip != nullptr ? g_ssl.get_param(ssl) : nullptr;
        if (param == nullptr || g_ssl.set_ip(param, host_text.c_str()) != 1) {
            g_ssl.ssl_free(ssl);
            failure = Failure::connection_lost;
            detail = "the secure connection failed";
            return nullptr;
        }
    }
    g_ssl.connect_state(ssl);
    TlsSession* session = new TlsSession;
    session->ssl = ssl;
    return session;
}

void tls_close(TlsSession* session) noexcept {
    if (session == nullptr)
        return;
    if (session->ssl != nullptr && g_ssl.ssl_free != nullptr)
        g_ssl.ssl_free(session->ssl);
    delete session;
}

TlsIoResult tls_handshake(TlsSession* session) {
    if (session == nullptr || session->ssl == nullptr)
        return failed_step(nullptr, 0);
    const int rc = g_ssl.handshake(session->ssl);
    if (rc == 1) {
        const long verify = g_ssl.verify_result(session->ssl);
        TlsIoResult result;
        if (verify != 0) {
            result.kind = TlsIoResult::Kind::failed;
            result.failure = Failure::certificate;
            result.detail = verify_reason(verify);
            return result;
        }
        result.kind = TlsIoResult::Kind::ready;
        return result;
    }
    return failed_step(session->ssl, rc);
}

TlsIoResult tls_read(TlsSession* session, uint8_t* bytes, std::size_t size) {
    if (session == nullptr || session->ssl == nullptr || bytes == nullptr)
        return failed_step(nullptr, 0);
    const int cap = size > static_cast<std::size_t>(INT_MAX) ? INT_MAX : static_cast<int>(size);
    const int rc = g_ssl.read(session->ssl, bytes, cap);
    if (rc > 0) {
        TlsIoResult result;
        result.kind = TlsIoResult::Kind::ready;
        result.count = rc;
        return result;
    }
    return failed_step(session->ssl, rc);
}

TlsIoResult tls_write(TlsSession* session, const uint8_t* bytes, std::size_t size) {
    if (session == nullptr || session->ssl == nullptr || bytes == nullptr)
        return failed_step(nullptr, 0);
    const int cap = size > static_cast<std::size_t>(INT_MAX) ? INT_MAX : static_cast<int>(size);
    const int rc = g_ssl.write(session->ssl, bytes, cap);
    if (rc > 0) {
        TlsIoResult result;
        result.kind = TlsIoResult::Kind::ready;
        result.count = rc;
        return result;
    }
    return failed_step(session->ssl, rc);
}

HttpsExchange fetch_https_hop(
    const Request& /*request*/,
    const oa::formats::url::Url& /*url*/,
    Method /*method*/,
    const std::vector<uint8_t>& /*body*/,
    std::string_view /*user_agent*/,
    BodySink& /*sink*/,
    const FetchClock& /*clock*/
) {
    HttpsExchange exchange;
    exchange.failure = Failure::https_unsupported;
    exchange.detail = "This system cannot use HTTPS";
    return exchange;
}

} // namespace oa::netgame::http
