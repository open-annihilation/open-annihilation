// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Signing in to GOG with the code its page gave, asking where the
// Commander Pack's Windows installer is, and downloading it, on one thread
// (gog_download.hpp).

#include "oa/app/gog_download.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/formats/json.hpp"
#include "oa/netgame/http/client.hpp"
#include "oa/platform/files.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <span>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace oa::app::gog {

namespace bundles = oa::data::content_bundles;
namespace fs = std::filesystem;
namespace http = oa::netgame::http;
namespace json = oa::formats::json;

namespace {

/// The client GOG Galaxy signs in as, which GOG's sign-in page accepts.
constexpr std::string_view client_id = "46899977096215655";
/// That client's published secret, sent with the code.
constexpr std::string_view client_secret =
    "9d85c43b1482497dbbce61f6e4aa173a433796eeae2ca8c5f6129f2dc4de46d9";
/// Where GOG's sign-in page ends, with the code in its query.
constexpr std::string_view landing_address_text = "https://embed.gog.com/on_login_success?origin=client";
/// The query name of the code on that address.
constexpr std::string_view code_name = "code=";
/// The fewest characters a code has; GOG's are much longer.
constexpr std::size_t least_code_length = 16;
/// The most bytes of an answer that is not the installer.
constexpr uint64_t most_answer_bytes = uint64_t{1} << 20;
/// The most bytes the installer may have.
constexpr uint64_t most_installer_bytes = uint64_t{8} << 30;
/// Follows the installer's name while it is incomplete.
constexpr std::string_view part_suffix = ".part";
/// The longest file name taken from GOG's address.
constexpr std::size_t most_name_length = 160;
/// Hosts the installer's address may lead to.
constexpr std::string_view download_hosts[] = {".gog.com", ".gog-cdn.com", ".gogcdn.net",
    ".gog-statics.com"};

/// Tells whether a character may stand in a code.
bool code_character(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
}

/// Writes a query value with every byte but the unreserved ones escaped.
std::string escaped(std::string_view text) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) != 0 || c == '-' || c == '.' || c == '_' || c == '~') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 0xF]);
        }
    }
    return out;
}

/// Reads one hex digit; -1 for any other character.
int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/// The last part of an address's path, unescaped, when it is a plain file
/// name: letters, digits and . _ - ( ) only, not starting with a dot.
///
/// @param address an absolute address
/// @return the name; empty when it is none
std::string file_name_of(std::string_view address) {
    const std::size_t query = address.find_first_of("?#");
    if (query != std::string_view::npos)
        address = address.substr(0, query);
    const std::size_t slash = address.rfind('/');
    if (slash == std::string_view::npos)
        return {};
    const std::string_view last = address.substr(slash + 1);
    std::string name;
    for (std::size_t at = 0; at < last.size(); ++at) {
        char c = last[at];
        if (c == '%' && at + 2 < last.size()) {
            const int high = hex_value(last[at + 1]);
            const int low = hex_value(last[at + 2]);
            if (high < 0 || low < 0)
                return {};
            c = static_cast<char>(high * 16 + low);
            at += 2;
        }
        const bool plain = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' ||
                           c == '_' || c == '-' || c == '(' || c == ')';
        if (!plain)
            return {};
        name.push_back(c);
    }
    if (name.empty() || name.front() == '.' || name.size() > most_name_length)
        return {};
    return name;
}

/// Tells whether two names are the same, ignoring the case of ASCII letters.
bool same_name(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

/// The Windows installer the setup copies from, when `name` and `size` are its.
const bundles::Installer* known_installer(std::string_view name, uint64_t size) noexcept {
    for (const bundles::Installer& installer : bundles::installers())
        if (installer.kind == bundles::InstallerKind::windows && installer.size == size &&
            same_name(installer.file_name, name))
            return &installer;
    return nullptr;
}

/// A string member of a JSON object; null when it has none.
const std::string* text_of(const json::Json& object, std::string_view name) {
    const json::Json* member = object.find(name);
    return member != nullptr ? member->string() : nullptr;
}

} // namespace

bool available() noexcept {
    return http::tls_available();
}

std::string sign_in_address() {
    std::string address = "https://auth.gog.com/auth?client_id=";
    address += client_id;
    address += "&redirect_uri=";
    address += escaped(landing_address_text);
    address += "&response_type=code&layout=client2";
    return address;
}

std::string_view landing_address() noexcept {
    return landing_address_text;
}

std::optional<std::string> login_code(std::string_view text) {
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && blank(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && blank(text.back()))
        text.remove_suffix(1);
    std::string_view code = text;
    const std::size_t query = text.find('?');
    if (query != std::string_view::npos) {
        std::string_view rest = text.substr(query + 1);
        code = {};
        while (!rest.empty()) {
            const std::size_t end = rest.find_first_of("&#");
            const std::string_view pair = rest.substr(0, end);
            if (pair.starts_with(code_name)) {
                code = pair.substr(code_name.size());
                break;
            }
            if (end == std::string_view::npos || rest[end] == '#')
                break;
            rest.remove_prefix(end + 1);
        }
    }
    if (code.size() < least_code_length || !std::all_of(code.begin(), code.end(), code_character))
        return std::nullopt;
    return std::string(code);
}

std::string_view failure_text(Failure failure) noexcept {
    switch (failure) {
    case Failure::none:
        return {};
    case Failure::sign_in:
        return "GOG did not accept the sign-in. Sign in again.";
    case Failure::not_owned:
        return "This GOG account does not have Total Annihilation: Commander Pack.";
    case Failure::no_installer:
        return "GOG lists no Windows installer for Total Annihilation: Commander Pack.";
    case Failure::no_https:
        return "This system cannot make secure connections, which GOG needs.";
    case Failure::network:
        return "GOG could not be reached. Check the connection and try again.";
    case Failure::unexpected:
        return "GOG's answer could not be read. Try again later.";
    case Failure::unknown_build:
        return "GOG sent a version of the installer that Open Annihilation does not know yet.";
    case Failure::disk:
        return "The installer could not be saved.";
    case Failure::no_space:
        return "The disk has no room for the installer.";
    case Failure::cancelled:
        return "The download was cancelled.";
    }
    return {};
}

struct Download::State {
    mutable std::mutex mutex{};
    Status status{};
    std::atomic<bool> cancel{false};
    std::thread worker{};

    /// Records why the download stopped; a cancel is told as one, whatever failed.
    void fail(Failure failure, std::string detail) {
        std::lock_guard<std::mutex> lock(mutex);
        status.phase = Phase::failed;
        status.failure = cancel.load() ? Failure::cancelled : failure;
        status.detail = std::move(detail);
    }

    void set_phase(Phase phase) {
        std::lock_guard<std::mutex> lock(mutex);
        status.phase = phase;
    }

    void run(const std::string& code, const fs::path& folder);
};

namespace {

/// The failure of a fetch that read no whole answer.
Failure fetch_failure(http::Failure failure) noexcept {
    switch (failure) {
    case http::Failure::https_unsupported:
    case http::Failure::https_needs_tls:
        return Failure::no_https;
    case http::Failure::cancelled:
        return Failure::cancelled;
    case http::Failure::bad_response:
    case http::Failure::too_large:
    case http::Failure::redirect_refused:
    case http::Failure::decode_failed:
    case http::Failure::bad_url:
    case http::Failure::bad_request:
        return Failure::unexpected;
    default:
        return Failure::network;
    }
}

/// The installer's bytes, appended to its part.
class PartSink final : public http::BodySink {
  public:
    /// Appends to `part` from `done` bytes, and tells `status`, under
    /// `mutex`, how far it has got.
    PartSink(std::mutex& mutex, Status& status, fs::path part, uint64_t done)
        : done_(done), mutex_(&mutex), shown_(&status), part_(std::move(part)) {}

    ~PartSink() override { close(); }

    PartSink(const PartSink&) = delete;
    PartSink& operator=(const PartSink&) = delete;

    /// Accepts a 200 from the start or a 206 from where the part ends, and
    /// checks the disk has room for the rest.
    bool begin(const http::Response& head) override {
        uint64_t size = 0;
        if (head.status == 206 && head.content_range && head.content_range->first == done_ &&
            head.content_range->total) {
            size = *head.content_range->total;
        } else if (head.status == 200 && head.content_length) {
            done_ = 0;
            size = *head.content_length;
        } else {
            return false;
        }
        if (size < done_ || size > most_installer_bytes)
            return false;
        std::error_code error;
        const fs::space_info room = fs::space(part_.parent_path(), error);
        if (!error && room.available < size - done_) {
            no_space_ = true;
            return false;
        }
        file_ = oa::platform::open_file(part_, done_ == 0 ? "wb" : "ab");
        if (file_ == nullptr) {
            disk_ = true;
            return false;
        }
        size_ = size;
        final_url_ = head.final_url;
        std::lock_guard<std::mutex> lock(*mutex_);
        shown_->size = size;
        shown_->received = done_;
        return true;
    }

    bool write(std::span<const uint8_t> bytes) override {
        if (bytes.size() > size_ - done_ ||
            std::fwrite(bytes.data(), 1, bytes.size(), file_) != bytes.size()) {
            disk_ = true;
            return false;
        }
        done_ += bytes.size();
        std::lock_guard<std::mutex> lock(*mutex_);
        shown_->received = done_;
        return true;
    }

    /// Closes the part; false when its last bytes could not be written.
    bool close() {
        if (file_ == nullptr)
            return true;
        const bool flushed = std::fflush(file_) == 0;
        const bool closed = std::fclose(file_) == 0;
        file_ = nullptr;
        if (!flushed || !closed)
            disk_ = true;
        return flushed && closed;
    }

    bool disk_{};
    bool no_space_{};
    uint64_t size_{};
    uint64_t done_{};
    std::string final_url_{};

  private:
    std::mutex* mutex_{};
    Status* shown_{};
    fs::path part_{};
    std::FILE* file_{};
};

} // namespace

void Download::State::run(const std::string& code, const fs::path& folder) {
    http::Client client;
    // Fetches one JSON answer. `label` names it in the log without the
    // address, which may carry the code.
    const auto answer = [&](std::string_view label, const std::string& url,
                            std::vector<http::Header> headers,
                            json::Json& out) -> std::optional<int> {
        http::Request request;
        request.url = url;
        request.headers = std::move(headers);
        request.accept_gzip = true;
        request.cancel = &cancel;
        request.limits.max_body_bytes = most_answer_bytes;
        std::vector<uint8_t> body;
        const http::Response response = client.fetch(request, body);
        if (response.failure != http::Failure::none) {
            fail(fetch_failure(response.failure), std::string(label) + ": " + response.detail);
            return std::nullopt;
        }
        json::JsonError error;
        std::optional<json::Json> parsed = json::parse_json(
            std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), error
        );
        if (parsed)
            out = std::move(*parsed);
        return response.status;
    };

    set_phase(Phase::signing_in);
    std::string token_url = "https://auth.gog.com/token?client_id=";
    token_url += client_id;
    token_url += "&client_secret=";
    token_url += client_secret;
    token_url += "&grant_type=authorization_code&code=";
    token_url += escaped(code);
    token_url += "&redirect_uri=";
    token_url += escaped(landing_address_text);
    json::Json token;
    const std::optional<int> token_status = answer("the token request", token_url, {}, token);
    if (!token_status)
        return;
    const std::string* access = text_of(token, "access_token");
    if (*token_status != 200 || access == nullptr || access->empty()) {
        fail(Failure::sign_in, "the token request answered " + std::to_string(*token_status));
        return;
    }
    const std::string bearer = "Bearer " + *access;

    set_phase(Phase::finding);
    json::Json product;
    const std::string product_url = "https://api.gog.com/products/" +
                                    std::string(commander_pack_product) + "?expand=downloads";
    const std::optional<int> product_status = answer("the product", product_url, {}, product);
    if (!product_status)
        return;
    if (*product_status != 200) {
        fail(Failure::unexpected, "the product answered " + std::to_string(*product_status));
        return;
    }
    // The English Windows installer, else any Windows one.
    std::string downlink;
    const json::Json* downloads = product.find("downloads");
    const json::Json* installers = downloads != nullptr ? downloads->find("installers") : nullptr;
    if (installers != nullptr) {
        for (const bool english : {true, false}) {
            for (const json::Json& installer : installers->elements()) {
                const std::string* os = text_of(installer, "os");
                const std::string* language = text_of(installer, "language");
                if (os == nullptr || *os != "windows")
                    continue;
                if (english && (language == nullptr || *language != "en"))
                    continue;
                const json::Json* files = installer.find("files");
                if (files == nullptr || files->elements().size() != 1)
                    continue;
                const std::string* link = text_of(files->elements()[0], "downlink");
                if (link != nullptr)
                    downlink = *link;
                break;
            }
            if (!downlink.empty())
                break;
        }
    }
    if (downlink.empty() || !downlink.starts_with("https://api.gog.com/")) {
        fail(Failure::no_installer, "no single-file Windows installer in " + product_url);
        return;
    }
    json::Json secure;
    const std::optional<int> secure_status =
        answer("the download link", downlink, {{"Authorization", bearer}}, secure);
    if (!secure_status)
        return;
    if (*secure_status == 401 || *secure_status == 403 || *secure_status == 404) {
        fail(Failure::not_owned, "the download link answered " + std::to_string(*secure_status));
        return;
    }
    const std::string* address = text_of(secure, "downlink");
    if (*secure_status != 200 || address == nullptr || !address->starts_with("https://")) {
        fail(Failure::unexpected, "the download link answered " + std::to_string(*secure_status));
        return;
    }
    std::string name = file_name_of(*address);
    if (name.empty()) {
        fail(Failure::unexpected, "no file name in the installer's address");
        return;
    }

    std::error_code error;
    fs::create_directories(folder, error);
    const fs::path whole = folder / path_from_utf8(name);
    const uint64_t had = fs::is_regular_file(whole, error) ? fs::file_size(whole, error) : 0;
    if (!error && known_installer(name, had) != nullptr) {
        std::lock_guard<std::mutex> lock(mutex);
        status.size = had;
        status.received = had;
        status.installer = whole;
        status.phase = Phase::done;
        return;
    }
    const fs::path part = folder / path_from_utf8(name + std::string(part_suffix));
    error.clear();
    const uint64_t done = fs::is_regular_file(part, error) ? fs::file_size(part, error) : 0;

    set_phase(Phase::downloading);
    http::Request request;
    request.url = *address;
    request.cancel = &cancel;
    if (done > 0 && !error)
        request.range_from = done;
    request.limits.max_body_bytes = most_installer_bytes;
    for (const std::string_view host : download_hosts)
        request.redirects.allowed_hosts.emplace_back(host);
    PartSink sink(mutex, status, part, error ? 0 : done);
    const http::Response response = client.fetch(request, sink);
    const bool closed = sink.close();
    if (sink.no_space_) {
        fail(Failure::no_space, "no room for " + std::to_string(sink.size_) + " bytes");
        return;
    }
    if (sink.disk_ || !closed) {
        fail(Failure::disk, "writing " + part.string());
        return;
    }
    if (response.failure == http::Failure::sink_stopped && response.status == 416) {
        // The part is not a beginning of the file the server has: the next
        // try starts it again.
        fs::remove(part, error);
        fail(Failure::network, "the installer answered 416 to the part's range");
        return;
    }
    if (response.failure == http::Failure::sink_stopped) {
        fail(
            response.status == 403 || response.status == 404 ? Failure::network
                                                              : Failure::unexpected,
            "the installer answered " + std::to_string(response.status)
        );
        return;
    }
    if (response.failure != http::Failure::none) {
        fail(fetch_failure(response.failure), "the installer: " + response.detail);
        return;
    }
    if (sink.done_ != sink.size_) {
        fail(Failure::network, "the installer ended early");
        return;
    }
    // A redirect may name the file more exactly than the first address.
    const std::string final_name = file_name_of(sink.final_url_);
    if (!final_name.empty())
        name = final_name;
    const fs::path target = folder / path_from_utf8(name);
    fs::remove(target, error);
    error.clear();
    fs::rename(part, target, error);
    if (error) {
        fail(Failure::disk, "renaming " + part.string() + ": " + error.message());
        return;
    }
    if (known_installer(name, sink.size_) == nullptr) {
        fail(Failure::unknown_build, name + ", " + std::to_string(sink.size_) + " bytes");
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    status.installer = target;
    status.phase = Phase::done;
}

Download::Download() : state_(std::make_shared<State>()) {}

Download::~Download() {
    state_->cancel.store(true);
    if (state_->worker.joinable())
        state_->worker.join();
}

void Download::start(std::string code, fs::path folder) {
    state_->cancel.store(true);
    if (state_->worker.joinable())
        state_->worker.join();
    state_->cancel.store(false);
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->status = Status{};
        state_->status.phase = Phase::signing_in;
    }
    State* state = state_.get();
    state_->worker = std::thread([state, code = std::move(code), folder = std::move(folder)] {
        state->run(code, folder);
    });
}

void Download::cancel() {
    state_->cancel.store(true);
}

Status Download::status() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->status;
}

} // namespace oa::app::gog
