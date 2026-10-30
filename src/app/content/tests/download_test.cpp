// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The download queue against a local fixture: resume, a match, a challenge,
// mirrors and the saved queue.
#include "oa/app/content/download_api.hpp"

#include "oa/app/content/downloads.hpp"
#include "oa/app/content/service.hpp"
#include "oa/app/content/settings.hpp"
#include "oa/base/sha256.hpp"
#include "oa/base/signing/ed25519.hpp"
#include "oa/base/threads.hpp"
#include "oa/data/catalogue/check.hpp"
#include "oa/data/registry/descriptor.hpp"
#include "oa/formats/url.hpp"
#include "oa/netgame/http_fixture/server.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace content = oa::app::content;
namespace catalogue = oa::data::catalogue;
namespace registry = oa::data::registry;
namespace fixture = oa::netgame::http_fixture;
namespace sha = oa::base::sha256;
namespace signing = oa::base::signing;
namespace threads = oa::base::threads;
namespace url = oa::formats::url;
namespace fs = std::filesystem;

/// 2026-01-01T00:00:00Z.
constexpr int64_t kEpoch = 1767225600;

/// An install ID the key request can carry.
constexpr std::string_view kInstallId = "7f3a90d2-4c1b-4e8a-9d07-c91e00000001";

/// A second install ID, used when the first is replaced.
constexpr std::string_view kInstallIdNext = "a1b2c3d4-e5f6-4789-a012-3456789abcde";

/// How long a state, a request or a saved file may take to appear. The queue
/// publishes those from its own thread, and a machine running many tests at
/// once can leave that thread waiting. Five and eight seconds ran out before
/// the challenge state was published (the check used to sit at the challenge
/// wait below). A wait returns as soon as the condition holds, so a quiet
/// machine still finishes in the time the work itself takes. The same bound
/// is what the longer checks in this file already used.
constexpr uint32_t kBusyWaitMs = 20000;

std::atomic<int64_t> g_now{kEpoch};

/// The test clock, in seconds since 1970.
int64_t test_clock() {
    return g_now.load();
}

/// Waits until `pred` is true, or the bound passes.
template <typename Pred>
bool wait_until(Pred pred, uint32_t bound_ms) {
    uint32_t slept = 0;
    while (slept <= bound_ms) {
        if (pred())
            return true;
        if (slept == bound_ms)
            break;
        const uint32_t slice = bound_ms - slept > 20 ? 20 : bound_ms - slept;
        threads::sleep_ms(slice);
        slept += slice;
    }
    return pred();
}

/// Lines the queue and the service logged.
struct Logs {
    threads::Mutex mutex;
    std::string text;
};

void remember_log(void* context, std::string_view line) {
    auto* logs = static_cast<Logs*>(context);
    threads::LockGuard guard(logs->mutex);
    logs->text.append(line);
    logs->text.push_back('\n');
}

std::string logs_text(Logs& logs) {
    threads::LockGuard guard(logs.mutex);
    return logs.text;
}

/// A scratch directory removed when the test ends.
struct Scratch {
    fs::path root;

    explicit Scratch(std::string_view name) : root(oa::test::make_scratch_directory(name)) {
        fs::create_directory(root / "data");
        fs::create_directory(root / "player");
        fs::create_directory(root / "builtin");
    }

    ~Scratch() {
        std::error_code error;
        fs::remove_all(root, error);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    fs::path data() const { return root / "data"; }

    fs::path player() const { return root / "player"; }

    fs::path builtin() const { return root / "builtin"; }
};

/// One signing key made from a fixed seed. The secret is wiped on destruction.
struct TestKey {
    signing::SecretKey secret{};
    registry::RegistryKey key{};

    TestKey(std::string id, uint8_t fill) {
        std::array<uint8_t, 32> seed{};
        seed.fill(fill);
        key.id = std::move(id);
        signing::key_pair_from_seed(seed, secret, key.key);
        seed.fill(0);
    }

    ~TestKey() { secret.fill(0); }

    TestKey(const TestKey&) = delete;
    TestKey& operator=(const TestKey&) = delete;
};

/// A catalogue and the detached signature over those exact bytes.
struct SignedBytes {
    std::vector<uint8_t> catalogue;
    std::vector<uint8_t> signature;
};

/// The package bytes a catalogue names.
struct PackageFile {
    std::vector<uint8_t> bytes;
    sha::Digest digest{};
    std::string hex;
    std::string path;
};

/// Bytes of `size` whose SHA-256 the catalogue can name.
PackageFile package_file(std::size_t size, uint8_t fill) {
    PackageFile file;
    file.bytes.resize(size);
    for (std::size_t index = 0; index < size; ++index)
        file.bytes[index] = static_cast<uint8_t>(fill + (index % 251));
    file.digest = sha::digest_of(std::span<const uint8_t>(file.bytes.data(), file.bytes.size()));
    const auto hex = sha::to_hex(file.digest);
    file.hex.assign(hex.begin(), hex.end());
    file.path = "/v1/p/" + file.hex + ".oamod";
    return file;
}

/// One package listed in a catalogue.
struct Listed {
    const PackageFile* file = nullptr;
    std::string id = "ridge";
    std::string name = "Ridge";
    int release = 1;
};

/// Builds one catalogue document.
std::string catalogue_json(
    const std::string& registry_id,
    int sequence,
    const registry::RegistryKey* publish,
    std::span<const Listed> packages
) {
    std::string json = "{\"catalogue\":1,\"registry\":\"";
    json += registry_id;
    json += "\",\"sequence\":";
    json += std::to_string(sequence);
    json += ",\"generated\":\"2026-01-01T00:00:00Z\",\"expires\":\"2026-02-01T00:00:00Z\"";
    if (publish != nullptr) {
        json += ",\"keys\":[{\"id\":\"";
        json += publish->id;
        json += "\",\"public\":\"";
        json += std::string(signing::public_key_text(publish->key).view());
        json += "\"}]";
    }
    json += ",\"packages\":[";
    bool first = true;
    for (const Listed& listed : packages) {
        if (!first)
            json += ',';
        first = false;
        json += "{\"kind\":\"oamod\",\"id\":\"";
        json += listed.id;
        json += "\",\"name\":\"";
        json += listed.name;
        json += "\",\"version\":\"1\",\"revision\":1,\"release\":";
        json += std::to_string(listed.release);
        json += ",\"size\":";
        json += std::to_string(listed.file->bytes.size());
        json += ",\"sha256\":\"";
        json += listed.file->hex;
        json += "\",\"file\":\"";
        json += listed.file->path;
        json += "\"}";
    }
    json += "]}";
    return json;
}

/// Signs a catalogue with a test key.
SignedBytes sign_json(std::string_view json, const TestKey& key) {
    SignedBytes out;
    out.catalogue.assign(json.begin(), json.end());
    const signing::Signature signature = signing::sign(key.secret, out.catalogue);
    const catalogue::SignatureLine line{key.key.id, signature};
    const std::string text = catalogue::signature_file_text(line);
    out.signature.assign(text.begin(), text.end());
    return out;
}

bool write_bytes(const fs::path& path, std::span<const uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    return static_cast<bool>(out);
}

std::vector<uint8_t> file_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Builds a descriptor. A null key leaves the registry unsigned.
registry::Descriptor descriptor_for(
    std::string id,
    std::string name,
    std::string catalogue_url,
    const TestKey* key,
    std::vector<std::string> mirrors = {}
) {
    registry::Descriptor descriptor;
    descriptor.id = std::move(id);
    descriptor.name = std::move(name);
    const auto parsed_catalogue = url::parse_http_url(catalogue_url);
    OA_CHECK(parsed_catalogue.has_value());
    if (parsed_catalogue)
        descriptor.catalogue = *parsed_catalogue;
    for (const std::string& mirror : mirrors) {
        const auto parsed = url::parse_http_url(mirror);
        OA_CHECK(parsed.has_value());
        if (parsed)
            descriptor.mirrors.push_back(*parsed);
    }
    if (key != nullptr)
        descriptor.keys.push_back(key->key);
    return descriptor;
}

bool install_builtin(
    const Scratch& scratch, const std::string& filename, const registry::Descriptor& descriptor
) {
    const std::string text = registry::descriptor_text(descriptor);
    const bool wrote = write_bytes(
        scratch.builtin() / filename,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size())
    );
    OA_CHECK(wrote);
    return wrote;
}

bool listening(fixture::Server& server) {
    std::string error;
    const bool started = server.start(&error);
    if (!started)
        std::fprintf(stderr, "fixture did not start: %s\n", error.c_str());
    OA_CHECK(started);
    return started;
}

void serve_document(fixture::Server& server, const std::string& path, const SignedBytes& document) {
    server.serve_bytes(path, document.catalogue, "application/json");
    server.serve_bytes(path + ".sig", document.signature, "text/plain");
}

/// Files the hand-off was given.
struct Handed {
    threads::Mutex mutex;
    std::vector<uint64_t> items;
    std::vector<fs::path> files;
    std::vector<content::InstallOrigin> origins;
};

bool record_handoff(
    void* context, uint64_t item, const fs::path& file, const content::InstallOrigin& origin
) {
    auto* handed = static_cast<Handed*>(context);
    threads::LockGuard guard(handed->mutex);
    handed->items.push_back(item);
    handed->files.push_back(file);
    handed->origins.push_back(origin);
    return true;
}

std::size_t handed_count(Handed& handed) {
    threads::LockGuard guard(handed.mutex);
    return handed.items.size();
}

/// One signed registry on a fixture, and the service that has fetched it.
struct Session {
    fixture::Server server;
    fixture::Server mirror;
    Scratch scratch;
    TestKey key;
    PackageFile package;
    std::optional<PackageFile> extra;
    Logs logs;
    Handed handed;
    std::unique_ptr<content::Service> service;

    bool tokens = false;
    bool use_mirror = false;
    bool serve_package = true;
    registry::InstallIdUse install_id = registry::InstallIdUse::none;
    int release = 1;
    int sequence = 1;
    std::string registry_id = "ridge";
    std::string package_id = "ridge";
    std::string extra_id = "vale";

    Session(std::string scratch_name, std::size_t size, uint8_t fill)
        : scratch(std::move(scratch_name)), key("release-2026", fill),
          package(package_file(size, fill)) {}

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    fs::path downloads() const { return content::downloads_folder(scratch.data()); }

    fs::path final_path() const { return downloads() / (package.hex + ".oamod"); }

    fs::path part_path() const { return downloads() / (package.hex + ".oamod.part"); }

    /// Serves the catalogue, starts the service and waits until the package is listed.
    bool open() {
        g_now = kEpoch;
        if (!listening(server))
            return false;
        if (use_mirror && !listening(mirror))
            return false;
        std::error_code error;
        fs::create_directories(downloads(), error);
        const std::string catalogue_path = "/v1/catalogue.json";
        std::vector<Listed> listed;
        listed.push_back(Listed{&package, package_id, "Ridge", release});
        if (extra)
            listed.push_back(Listed{&*extra, extra_id, "Vale", release});
        const SignedBytes document =
            sign_json(catalogue_json(registry_id, sequence, &key.key, listed), key);
        serve_document(server, catalogue_path, document);
        if (use_mirror)
            serve_document(mirror, catalogue_path, document);
        std::vector<std::string> mirrors;
        if (use_mirror)
            mirrors.push_back(mirror.url(catalogue_path));
        registry::Descriptor descriptor = descriptor_for(
            registry_id, "Ridge", server.url(catalogue_path), &key, std::move(mirrors)
        );
        if (tokens) {
            descriptor.mode = registry::DownloadMode::tokens;
            const auto api = url::parse_http_url(server.url("/api/v1"));
            OA_CHECK(api.has_value());
            if (!api)
                return false;
            descriptor.api = *api;
            descriptor.install_id = install_id;
        }
        if (!install_builtin(scratch, registry_id + ".yaml", descriptor))
            return false;
        if (serve_package)
            server.serve_bytes(package.path, package.bytes);
        if (extra)
            server.serve_bytes(extra->path, extra->bytes);
        content::ServiceOptions options;
        options.data_folder = scratch.data();
        options.player_folder = scratch.player();
        options.builtin_folder = scratch.builtin();
        options.log = remember_log;
        options.log_context = &logs;
        options.clock = test_clock;
        service = std::make_unique<content::Service>(std::move(options));
        service->start();
        const bool ready = wait_until(
            [&] {
                const auto snapshot = service->snapshot();
                if (!snapshot)
                    return false;
                for (const content::Entry& entry : snapshot->entries) {
                    if (entry.package != nullptr && entry.package->id == package_id &&
                        entry.package->release == release)
                        return true;
                }
                return false;
            },
            kBusyWaitMs
        );
        if (!ready) {
            const auto snapshot = service->snapshot();
            if (snapshot) {
                for (const content::RegistryView& view : snapshot->registries)
                    std::fprintf(
                        stderr,
                        "registry %s status %d error %s\n",
                        view.registry.descriptor.id.c_str(),
                        static_cast<int>(view.status),
                        view.last_error.c_str()
                    );
            }
            std::fprintf(stderr, "catalogue was not ready\n%s\n", logs_text(logs).c_str());
            OA_CHECK(false);
            return false;
        }
        server.clear_log();
        if (use_mirror)
            mirror.clear_log();
        return true;
    }

    content::DownloadsOptions queue_options() {
        content::DownloadsOptions options;
        options.data_folder = scratch.data();
        options.language = "en";
        options.handoff = record_handoff;
        options.handoff_context = &handed;
        options.log = remember_log;
        options.log_context = &logs;
        options.clock = test_clock;
        options.retry_base_ms = 40;
        options.poll_ms = 40;
        return options;
    }

    std::optional<uint64_t>
    enqueue(content::Downloads& downloads, content::DownloadReason reason, std::string* why) {
        const auto snapshot = service->snapshot();
        if (!snapshot)
            return std::nullopt;
        std::optional<content::DownloadTarget> target =
            content::download_target(*snapshot, registry_id, package_id, why);
        if (!target)
            return std::nullopt;
        return downloads.queue(std::move(*target), reason, why);
    }
};

fixture::Reply json_reply(int status, std::string body) {
    fixture::Reply reply;
    reply.status = status;
    reply.body.assign(body.begin(), body.end());
    reply.headers.push_back({"Content-Type", "application/json"});
    return reply;
}

/// A 201 whose package address is this server's copy of the file.
fixture::Reply
issued(const fixture::Server& server, const PackageFile& package, std::string_view expires) {
    const std::string body = std::string("{\"download\":\"ridge-dl-1\",\"expires\":\"") +
                             std::string(expires) + "\",\"url\":\"" + server.url(package.path) +
                             "\"}";
    return json_reply(201, body);
}

int count_method(const fixture::Server& server, std::string_view method, std::string_view path) {
    int count = 0;
    for (const fixture::LoggedRequest& request : server.requests()) {
        if (request.method == method && request.target == path)
            ++count;
    }
    return count;
}

std::string
body_at(const fixture::Server& server, std::string_view method, std::string_view path, int index) {
    int seen = 0;
    for (const fixture::LoggedRequest& request : server.requests()) {
        if (request.method != method || request.target != path)
            continue;
        if (seen == index)
            return std::string(request.body.begin(), request.body.end());
        ++seen;
    }
    return {};
}

int count_results(const fixture::Server& server) {
    int count = 0;
    for (const fixture::LoggedRequest& request : server.requests()) {
        if (request.method == "POST" && request.target.find("/result") != std::string::npos)
            ++count;
    }
    return count;
}

std::string result_body(const fixture::Server& server) {
    std::string last;
    for (const fixture::LoggedRequest& request : server.requests()) {
        if (request.method == "POST" && request.target.find("/result") != std::string::npos)
            last.assign(request.body.begin(), request.body.end());
    }
    return last;
}

bool saw_range(const fixture::Server& server, std::string_view value) {
    for (const fixture::LoggedRequest& request : server.requests()) {
        const std::string* range = request.header("Range");
        if (range != nullptr && *range == value)
            return true;
    }
    return false;
}

/// True when a resume asked for a whole number of steps.
bool saw_step_range(const fixture::Server& server) {
    for (const fixture::LoggedRequest& request : server.requests()) {
        const std::string* range = request.header("Range");
        if (range == nullptr || range->size() < 8 || range->compare(0, 6, "bytes=") != 0)
            continue;
        if (range->back() != '-')
            continue;
        uint64_t start = 0;
        bool digits = false;
        bool ok = true;
        for (std::size_t index = 6; index + 1 < range->size(); ++index) {
            const char character = (*range)[index];
            if (character < '0' || character > '9') {
                ok = false;
                break;
            }
            digits = true;
            start = start * 10 + static_cast<uint64_t>(character - '0');
        }
        if (ok && digits && start > 0 && start % content::download_step_bytes == 0)
            return true;
    }
    return false;
}

std::optional<content::DownloadView> view_of(content::Downloads& downloads, uint64_t id) {
    for (const content::DownloadView& view : downloads.view()) {
        if (view.item == id)
            return view;
    }
    return std::nullopt;
}

bool wait_state(
    content::Downloads& downloads, uint64_t id, content::DownloadState state, uint32_t bound_ms
) {
    return wait_until(
        [&] {
            const auto view = view_of(downloads, id);
            return view.has_value() && view->state == state;
        },
        bound_ms
    );
}

/// The lines the queue wrote for failed downloads, in order.
std::vector<std::string> failure_lines(Logs& logs) {
    std::vector<std::string> lines;
    const std::string text = logs_text(logs);
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = text.find('\n', at);
        std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (line.rfind("download of ", 0) == 0)
            lines.push_back(std::move(line));
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    return lines;
}

/// Waits for the failure's log line, and checks that no second one follows.
///
/// @return the line, or empty when none came
std::string one_failure_line(Logs& logs) {
    OA_CHECK(wait_until([&] { return !failure_lines(logs).empty(); }, kBusyWaitMs));
    threads::sleep_ms(200);
    const std::vector<std::string> lines = failure_lines(logs);
    if (lines.size() != 1)
        std::fprintf(stderr, "failure lines: %zu\n%s\n", lines.size(), logs_text(logs).c_str());
    OA_CHECK(lines.size() == 1);
    return lines.empty() ? std::string() : lines.front();
}

/// Checks that a log line holds a text, and shows the line when it does not.
void expect_in(const std::string& line, std::string_view text) {
    const bool found = line.find(text) != std::string::npos;
    if (!found)
        std::fprintf(
            stderr,
            "wanted \"%.*s\" in: %s\n",
            static_cast<int>(text.size()),
            text.data(),
            line.c_str()
        );
    OA_CHECK(found);
}

/// Checks that a log line leaves a text out.
void expect_not_in(const std::string& line, std::string_view text) {
    const bool found = line.find(text) != std::string::npos;
    if (found)
        std::fprintf(
            stderr,
            "did not want \"%.*s\" in: %s\n",
            static_cast<int>(text.size()),
            text.data(),
            line.c_str()
        );
    OA_CHECK(!found);
}

/// How many times a text appears in another, ignoring ASCII case.
std::size_t count_folded(std::string_view text, std::string_view part) {
    const auto fold = [](std::string_view in) {
        std::string out(in);
        for (char& character : out)
            if (character >= 'A' && character <= 'Z')
                character = static_cast<char>(character - 'A' + 'a');
        return out;
    };
    const std::string haystack = fold(text);
    const std::string needle = fold(part);
    std::size_t count = 0;
    if (needle.empty())
        return 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size()))
        ++count;
    return count;
}

/// Checks that a failed item's detail and message name its reason once.
void expect_reason_once(const content::DownloadView& view) {
    const std::string reason = content::failure_text(view.failure);
    const std::string message = content::failure_message(view, nullptr);
    OA_CHECK(!message.empty());
    OA_CHECK(count_folded(view.detail, reason) == 0);
    if (count_folded(message, reason) > 1)
        std::fprintf(stderr, "repeated reason: %s\n", message.c_str());
    OA_CHECK(count_folded(message, reason) <= 1);
}

/// A web page a firewall sends in place of what was asked, with its header.
///
/// @param status 405 for a captcha, 202 for a challenge
/// @param action the firewall's word, or empty for a page with no header
fixture::Reply browser_page(int status, std::string_view action) {
    fixture::Reply reply;
    reply.status = status;
    reply.headers.push_back({"Content-Type", "text/html; charset=UTF-8"});
    if (!action.empty())
        reply.headers.push_back({"x-amzn-waf-action", std::string(action)});
    const std::string page = "<!DOCTYPE html><html><head><title>Human Verification</title></head>"
                             "<body><div id=\"captcha-container\"></div></body></html>";
    reply.body.assign(page.begin(), page.end());
    return reply;
}

/// Serves the package in pieces, honouring one Range, so a test can pause it.
void serve_paced(
    fixture::Server& server, const PackageFile& package, std::size_t piece, uint32_t delay_ms
) {
    server.handle("GET", package.path, [=](const fixture::LoggedRequest& request) {
        fixture::Reply reply;
        reply.piece_bytes = piece;
        reply.piece_delay_ms = delay_ms;
        const std::string* range = request.header("Range");
        uint64_t start = 0;
        bool partial = false;
        if (range != nullptr && range->compare(0, 6, "bytes=") == 0) {
            partial = true;
            for (std::size_t index = 6; index < range->size(); ++index) {
                const char character = (*range)[index];
                if (character < '0' || character > '9')
                    break;
                start = start * 10 + static_cast<uint64_t>(character - '0');
            }
        }
        if (partial && start < package.bytes.size()) {
            reply.status = 206;
            reply.body.assign(
                package.bytes.begin() + static_cast<std::ptrdiff_t>(start), package.bytes.end()
            );
            reply.headers.push_back(
                {"Content-Range",
                 "bytes " + std::to_string(start) + "-" + std::to_string(package.bytes.size() - 1) +
                     "/" + std::to_string(package.bytes.size())}
            );
        } else {
            reply.status = 200;
            reply.body = package.bytes;
        }
        return reply;
    });
}

std::string expected_key(
    const Session& session, std::optional<std::string> install, content::DownloadReason reason
) {
    content::KeyRequest request;
    request.install = std::move(install);
    request.package = session.package_id;
    request.kind = catalogue::Kind::oamod;
    request.release = session.release;
    request.sha256 = session.package.digest;
    request.engine = std::string(content::engine_version());
    request.platform = std::string(content::platform_name());
    request.arch = std::string(content::arch_name());
    request.language = "en";
    request.reason = reason;
    return content::key_request_body(request);
}

void finish(content::Downloads& downloads, uint64_t id, content::InstallOutcome outcome) {
    OA_CHECK(wait_state(downloads, id, content::DownloadState::installing, 20000));
    if (outcome == content::InstallOutcome::installed)
        g_now = kEpoch + 5;
    downloads.report_install(id, outcome);
}

void test_direct_large() {
    Session session("f10-direct", (3u << 20) + 123, 0x31);
    if (!session.open())
        return;
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    OA_CHECK(!downloads.busy());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::repair, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(downloads.busy());
    finish(downloads, *id, content::InstallOutcome::installed);
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::done, kBusyWaitMs));
    OA_CHECK(!downloads.busy());
    OA_CHECK(!fs::exists(session.part_path()));
    OA_CHECK(file_bytes(session.final_path()) == session.package.bytes);
    OA_CHECK(handed_count(session.handed) == 1);
    threads::LockGuard guard(session.handed.mutex);
    OA_CHECK(session.handed.origins[0].registry == "ridge");
    OA_CHECK(session.handed.origins[0].key == "ridge");
    OA_CHECK(session.handed.origins[0].release == 1);
    OA_CHECK(session.handed.origins[0].kind == catalogue::Kind::oamod);
    OA_CHECK(session.handed.origins[0].sha256 == session.package.digest);
    OA_CHECK(session.handed.origins[0].reason == content::DownloadReason::repair);
}

void test_tokens_outcome(content::InstallOutcome outcome, std::string_view word, bool check_body) {
    Session session(std::string("f10-tokens-") + std::string(word), 80, 0x32);
    session.tokens = true;
    session.install_id = registry::InstallIdUse::required;
    if (!session.open())
        return;
    session.server.handle("POST", "/api/v1/downloads", [&](const fixture::LoggedRequest&) {
        return issued(session.server, session.package, "2026-02-01T00:00:00Z");
    });
    session.server.handle(
        "POST", "/api/v1/downloads/ridge-dl-1/result", [](const fixture::LoggedRequest&) {
            return json_reply(204, "");
        }
    );
    content::Downloads downloads(session.queue_options());
    downloads.set_install_id(session.registry_id, std::string(kInstallId));
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    finish(downloads, *id, outcome);
    const content::DownloadState want =
        outcome == content::InstallOutcome::installed   ? content::DownloadState::done
        : outcome == content::InstallOutcome::set_aside ? content::DownloadState::cancelled
                                                        : content::DownloadState::failed;
    OA_CHECK(wait_state(downloads, *id, want, kBusyWaitMs));
    OA_CHECK(wait_until([&] { return count_results(session.server) == 1; }, kBusyWaitMs));
    const std::string result = result_body(session.server);
    if (check_body) {
        const std::string wanted = std::string("{\"result\":\"") + std::string(word) +
                                   "\",\"bytes\":" + std::to_string(session.package.bytes.size()) +
                                   ",\"seconds\":5}";
        if (result != wanted)
            std::fprintf(stderr, "result %s\nwanted %s\n", result.c_str(), wanted.c_str());
        OA_CHECK(result == wanted);
        const std::string sent = body_at(session.server, "POST", "/api/v1/downloads", 0);
        const std::string expect =
            expected_key(session, std::string(kInstallId), content::DownloadReason::update);
        if (sent != expect)
            std::fprintf(stderr, "key\n%s\nwanted\n%s\n", sent.c_str(), expect.c_str());
        OA_CHECK(sent == expect);
    } else {
        OA_CHECK(
            result.find(std::string("\"result\":\"") + std::string(word) + "\"") !=
            std::string::npos
        );
    }
    // The worker marks the download done, then posts its result: wait for the post.
    OA_CHECK(wait_until([&] { return count_results(session.server) == 1; }, kBusyWaitMs));
    OA_CHECK(count_results(session.server) == 1);
}

void test_cut_then_range() {
    Session session("f10-cut", (3u << 20) + 123, 0x33);
    if (!session.open())
        return;
    fixture::Reply cut;
    cut.status = 200;
    cut.body = session.package.bytes;
    cut.cut_after = (2u << 20) + 10;
    session.server.override_next(session.package.path, std::move(cut), 1);
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    finish(downloads, *id, content::InstallOutcome::installed);
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::done, 20000));
    OA_CHECK(saw_range(session.server, "bytes=2097152-"));
    OA_CHECK(file_bytes(session.final_path()) == session.package.bytes);
    OA_CHECK(!fs::exists(session.part_path()));
}

void test_whole_answer_to_range() {
    Session session("f10-whole", (3u << 20) + 123, 0x34);
    session.serve_package = false;
    if (!session.open())
        return;
    fixture::Reply cut;
    cut.status = 200;
    cut.body = session.package.bytes;
    cut.cut_after = (2u << 20) + 10;
    fixture::Reply whole;
    whole.status = 200;
    whole.body = session.package.bytes;
    session.server.override_next(session.package.path, std::move(cut), 1);
    session.server.override_next(session.package.path, std::move(whole), 1);
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    finish(downloads, *id, content::InstallOutcome::installed);
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::done, 20000));
    OA_CHECK(saw_range(session.server, "bytes=2097152-"));
    OA_CHECK(file_bytes(session.final_path()) == session.package.bytes);
}

void test_package_rekey() {
    Session session("f10-rekey", 64, 0x35);
    session.tokens = true;
    session.install_id = registry::InstallIdUse::required;
    if (!session.open())
        return;
    std::atomic<int> posts{0};
    threads::Mutex gate_mutex;
    threads::ConditionVariable gate;
    bool release = false;
    session.server.handle("POST", "/api/v1/downloads", [&](const fixture::LoggedRequest&) {
        posts.fetch_add(1);
        return issued(session.server, session.package, "2026-02-01T00:00:00Z");
    });
    session.server.handle("GET", session.package.path, [&](const fixture::LoggedRequest&) {
        if (posts.load() == 1) {
            threads::LockGuard guard(gate_mutex);
            gate.wait(gate_mutex, [&] { return release; });
            fixture::Reply reply;
            reply.status = 403;
            return reply;
        }
        fixture::Reply reply;
        reply.status = 200;
        reply.body = session.package.bytes;
        return reply;
    });
    session.server.handle(
        "POST", "/api/v1/downloads/ridge-dl-1/result", [](const fixture::LoggedRequest&) {
            return json_reply(204, "");
        }
    );
    content::Downloads downloads(session.queue_options());

    /// Unblocks the fixture if the test leaves before the package answer.
    struct ReleaseGate {
        threads::Mutex& mutex;
        threads::ConditionVariable& gate;
        bool& release;

        ReleaseGate(threads::Mutex& mutex_in, threads::ConditionVariable& gate_in, bool& release_in)
            : mutex(mutex_in), gate(gate_in), release(release_in) {}

        ~ReleaseGate() {
            threads::LockGuard guard(mutex);
            release = true;
            gate.notify_all();
        }
    } release_gate{gate_mutex, gate, release};

    downloads.set_install_id(session.registry_id, std::string(kInstallId));
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_until(
        [&] { return count_method(session.server, "GET", session.package.path) >= 1; }, kBusyWaitMs
    ));
    downloads.set_install_id(session.registry_id, std::string(kInstallIdNext));
    {
        threads::LockGuard guard(gate_mutex);
        release = true;
        gate.notify_all();
    }
    finish(downloads, *id, content::InstallOutcome::installed);
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::done, kBusyWaitMs));
    OA_CHECK(count_method(session.server, "POST", "/api/v1/downloads") == 2);
    OA_CHECK(
        body_at(session.server, "POST", "/api/v1/downloads", 0).find(kInstallId) !=
        std::string::npos
    );
    const std::string second = body_at(session.server, "POST", "/api/v1/downloads", 1);
    OA_CHECK(
        second ==
        expected_key(session, std::string(kInstallIdNext), content::DownloadReason::update)
    );
    // The worker marks the download done, then posts its result: wait for the post.
    OA_CHECK(wait_until([&] { return count_results(session.server) == 1; }, kBusyWaitMs));
    OA_CHECK(count_results(session.server) == 1);
}

void test_key_expires_soon() {
    Session session("f10-expiry", 40, 0x36);
    session.tokens = true;
    session.install_id = registry::InstallIdUse::required;
    if (!session.open())
        return;
    std::atomic<int> posts{0};
    session.server.handle("POST", "/api/v1/downloads", [&](const fixture::LoggedRequest&) {
        const int which = posts.fetch_add(1);
        const char* expires = which == 0 ? "2026-01-01T00:00:10Z" : "2026-02-01T00:00:00Z";
        return issued(session.server, session.package, expires);
    });
    content::Downloads downloads(session.queue_options());
    downloads.set_install_id(session.registry_id, std::string(kInstallId));
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_until(
        [&] { return count_method(session.server, "GET", session.package.path) >= 1; }, kBusyWaitMs
    ));
    OA_CHECK(count_method(session.server, "POST", "/api/v1/downloads") == 2);
    finish(downloads, *id, content::InstallOutcome::installed);
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::done, kBusyWaitMs));
}

void test_challenge_solved() {
    Session session("f10-solved", 48, 0x37);
    session.tokens = true;
    session.install_id = registry::InstallIdUse::none;
    if (!session.open())
        return;
    std::atomic<int> polls{0};
    std::atomic<bool> solved{false};
    session.server.handle("POST", "/api/v1/downloads", [&](const fixture::LoggedRequest&) {
        if (!solved.load()) {
            return json_reply(
                428,
                "{\"error\":\"challenge_required\",\"message\":\"confirm you are a person\","
                "\"challenge\":{\"code\":\"HXQ7-4KP2\","
                "\"verify\":\"https://downloads.example.net/verify?c=HXQ7-4KP2\","
                "\"expires\":\"2026-02-01T00:00:00Z\",\"poll\":3}}"
            );
        }
        return issued(session.server, session.package, "2026-02-01T00:00:00Z");
    });
    session.server.handle(
        "GET", "/api/v1/challenges/HXQ7-4KP2", [&](const fixture::LoggedRequest&) {
            const int count = polls.fetch_add(1);
            if (count >= 1)
                solved.store(true);
            const char* state = count >= 1 ? "solved" : "pending";
            return json_reply(
                200,
                std::string("{\"code\":\"HXQ7-4KP2\",\"state\":\"") + state +
                    "\",\"expires\":\"2026-02-01T00:00:00Z\"}"
            );
        }
    );
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::challenge, kBusyWaitMs));
    const auto shown = view_of(downloads, *id);
    OA_CHECK(shown.has_value() && shown->challenge.has_value());
    if (shown && shown->challenge) {
        OA_CHECK(shown->challenge->code == "HXQ7-4KP2");
        OA_CHECK(shown->challenge->address == "downloads.example.net/verify");
    }
    finish(downloads, *id, content::InstallOutcome::installed);
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::done, kBusyWaitMs));
    OA_CHECK(polls.load() >= 2);
    OA_CHECK(count_method(session.server, "GET", "/api/v1/challenges/HXQ7-4KP2") >= 2);
}

void test_challenge_expired() {
    Session session("f10-expired", 32, 0x38);
    session.tokens = true;
    session.use_mirror = true;
    if (!session.open())
        return;
    session.server.handle("POST", "/api/v1/downloads", [](const fixture::LoggedRequest&) {
        return json_reply(
            428,
            "{\"error\":\"challenge_required\",\"message\":\"confirm you are a person\","
            "\"challenge\":{\"code\":\"HXQ7-4KP2\","
            "\"verify\":\"https://downloads.example.net/verify?c=HXQ7-4KP2\","
            "\"expires\":\"2026-02-01T00:00:00Z\",\"poll\":3}}"
        );
    });
    session.server.handle("GET", "/api/v1/challenges/HXQ7-4KP2", [](const fixture::LoggedRequest&) {
        return json_reply(
            200,
            "{\"code\":\"HXQ7-4KP2\",\"state\":\"expired\",\"expires\":\"2026-02-01T00:00:00Z\"}"
        );
    });
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::failed, kBusyWaitMs));
    const auto view = view_of(downloads, *id);
    OA_CHECK(view.has_value());
    if (view)
        OA_CHECK(view->failure == content::DownloadFailure::challenge_expired);
    OA_CHECK(count_results(session.server) == 0);
    OA_CHECK(session.mirror.requests().empty());
    OA_CHECK(count_method(session.server, "GET", session.package.path) == 0);
}

void test_challenge_cancel() {
    Session session("f10-cancel-check", 32, 0x39);
    session.tokens = true;
    if (!session.open())
        return;
    std::atomic<int> polls{0};
    session.server.handle("POST", "/api/v1/downloads", [](const fixture::LoggedRequest&) {
        return json_reply(
            428,
            "{\"error\":\"challenge_required\",\"message\":\"confirm you are a person\","
            "\"challenge\":{\"code\":\"HXQ7-4KP2\","
            "\"verify\":\"https://downloads.example.net/verify?c=HXQ7-4KP2\","
            "\"expires\":\"2026-02-01T00:00:00Z\",\"poll\":3}}"
        );
    });
    session.server.handle(
        "GET", "/api/v1/challenges/HXQ7-4KP2", [&](const fixture::LoggedRequest&) {
            polls.fetch_add(1);
            return json_reply(
                200,
                "{\"code\":\"HXQ7-4KP2\",\"state\":\"pending\",\"expires\":\"2026-02-01T00:00:"
                "00Z\"}"
            );
        }
    );
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::challenge, kBusyWaitMs));
    downloads.cancel(*id);
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::cancelled, kBusyWaitMs));
    // A poll already on the wire when the cancel lands may still arrive. Wait
    // until the count has stayed still for six poll times (40 ms apart here).
    // A fixed sleep ended before that poll on a busy machine, and counted it
    // as a poll that kept going.
    const int cancelled_at = polls.load();
    int settled = cancelled_at;
    auto quiet_since = std::chrono::steady_clock::now();
    OA_CHECK(wait_until(
        [&] {
            const int polls_now = polls.load();
            if (polls_now != settled) {
                settled = polls_now;
                quiet_since = std::chrono::steady_clock::now();
                return false;
            }
            return std::chrono::steady_clock::now() - quiet_since >= std::chrono::milliseconds(240);
        },
        kBusyWaitMs
    ));
    OA_CHECK(settled <= cancelled_at + 1);
    OA_CHECK(polls.load() == settled);
    OA_CHECK(count_results(session.server) == 0);
}

void test_not_offered(int status, const char* error) {
    Session session(std::string("f10-offer-") + error, 32, 0x41);
    session.tokens = true;
    session.use_mirror = true;
    session.serve_package = false;
    if (!session.open())
        return;
    session.server.handle("POST", "/api/v1/downloads", [=](const fixture::LoggedRequest&) {
        return json_reply(
            status, std::string("{\"error\":\"") + error + "\",\"message\":\"not this release\"}"
        );
    });
    content::Downloads downloads(session.queue_options());
    downloads.set_install_id(session.registry_id, std::string(kInstallId));
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::failed, kBusyWaitMs));
    const auto view = view_of(downloads, *id);
    OA_CHECK(view.has_value());
    if (view)
        OA_CHECK(view->failure == content::DownloadFailure::not_offered);
    threads::sleep_ms(200);
    OA_CHECK(count_method(session.server, "POST", "/api/v1/downloads") == 1);
    OA_CHECK(session.mirror.requests().empty());
}

void test_client_fault(int status) {
    Session session("f10-fault-" + std::to_string(status), 32, static_cast<uint8_t>(status));
    session.tokens = true;
    session.use_mirror = true;
    session.serve_package = false;
    if (!session.open())
        return;
    session.server.handle("POST", "/api/v1/downloads", [=](const fixture::LoggedRequest&) {
        return json_reply(
            status,
            "{\"error\":\"bad_request\",\"message\":\"the package is "
            "missing\",\"field\":\"package\"}"
        );
    });
    content::Downloads downloads(session.queue_options());
    downloads.set_install_id(session.registry_id, std::string(kInstallId));
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::failed, kBusyWaitMs));
    const auto view = view_of(downloads, *id);
    OA_CHECK(view.has_value());
    if (view)
        OA_CHECK(view->failure == content::DownloadFailure::refused);
    threads::sleep_ms(200);
    OA_CHECK(count_method(session.server, "POST", "/api/v1/downloads") == 1);
    OA_CHECK(session.mirror.requests().empty());
    OA_CHECK(logs_text(session.logs).find("package") != std::string::npos);
}

void test_rate_limit() {
    Session session("f10-rate", 32, 0x42);
    session.tokens = true;
    if (!session.open())
        return;
    std::atomic<int> posts{0};
    threads::Mutex mutex;
    std::vector<std::chrono::steady_clock::time_point> times;
    session.server.handle("POST", "/api/v1/downloads", [&](const fixture::LoggedRequest&) {
        {
            threads::LockGuard guard(mutex);
            times.push_back(std::chrono::steady_clock::now());
        }
        if (posts.fetch_add(1) == 0) {
            fixture::Reply reply =
                json_reply(429, "{\"error\":\"rate_limited\",\"message\":\"wait\"}");
            reply.headers.push_back({"Retry-After", "1"});
            return reply;
        }
        return issued(session.server, session.package, "2026-02-01T00:00:00Z");
    });
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_until([&] { return posts.load() >= 2; }, kBusyWaitMs));
    std::chrono::steady_clock::time_point first{};
    std::chrono::steady_clock::time_point second{};
    {
        threads::LockGuard guard(mutex);
        OA_CHECK(times.size() >= 2);
        if (times.size() >= 2) {
            first = times[0];
            second = times[1];
        }
    }
    const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(second - first).count();
    if (gap < 800)
        std::fprintf(stderr, "retry gap %lld ms\n", static_cast<long long>(gap));
    OA_CHECK(gap >= 800);
    finish(downloads, *id, content::InstallOutcome::installed);
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::done, kBusyWaitMs));
}

void test_unavailable_then_mirror() {
    Session session("f10-mirror", 70, 0x43);
    session.tokens = true;
    session.use_mirror = true;
    session.serve_package = false;
    if (!session.open())
        return;
    session.mirror.serve_bytes(session.package.path, session.package.bytes);
    std::atomic<int> posts{0};
    session.server.handle("POST", "/api/v1/downloads", [&](const fixture::LoggedRequest&) {
        posts.fetch_add(1);
        return json_reply(503, "{\"error\":\"unavailable\",\"message\":\"later\"}");
    });
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    finish(downloads, *id, content::InstallOutcome::installed);
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::done, kBusyWaitMs));
    OA_CHECK(posts.load() == 4);
    OA_CHECK(count_method(session.mirror, "GET", session.package.path) >= 1);
    OA_CHECK(file_bytes(session.final_path()) == session.package.bytes);
}

void test_refusal_skips_mirror() {
    Session session("f10-no-mirror", 32, 0x44);
    session.tokens = true;
    session.use_mirror = true;
    session.serve_package = false;
    if (!session.open())
        return;
    session.server.handle("POST", "/api/v1/downloads", [](const fixture::LoggedRequest&) {
        return json_reply(403, "{\"error\":\"refused\",\"message\":\"no\"}");
    });
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::failed, kBusyWaitMs));
    threads::sleep_ms(200);
    OA_CHECK(count_method(session.server, "POST", "/api/v1/downloads") == 1);
    OA_CHECK(session.mirror.requests().empty());
    // The registry's message is the detail, named once, and the line says
    // what the key request was answered.
    const auto view = view_of(downloads, *id);
    OA_CHECK(view.has_value());
    if (view) {
        OA_CHECK(view->detail == "no");
        OA_CHECK(
            content::failure_message(*view, nullptr) == "The registry refused the download (no)"
        );
        expect_reason_once(*view);
    }
    const std::string line = one_failure_line(session.logs);
    expect_in(line, "failed: the registry refused the download (no); key request: ");
    expect_in(line, "/api/v1/downloads answered 403, application/json, ");
}

void test_wrong_hash_once() {
    Session session("f10-hash-once", (1u << 20) + 50, 0x45);
    if (!session.open())
        return;
    std::vector<uint8_t> wrong = session.package.bytes;
    wrong[0] = static_cast<uint8_t>(wrong[0] + 1);
    fixture::Reply once;
    once.status = 200;
    once.body = std::move(wrong);
    session.server.override_next(session.package.path, std::move(once), 1);
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    finish(downloads, *id, content::InstallOutcome::installed);
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::done, 20000));
    OA_CHECK(count_method(session.server, "GET", session.package.path) >= 2);
    OA_CHECK(file_bytes(session.final_path()) == session.package.bytes);
}

void test_wrong_hash_twice() {
    Session session("f10-hash-twice", (1u << 20) + 50, 0x46);
    session.tokens = true;
    session.install_id = registry::InstallIdUse::required;
    session.serve_package = false;
    if (!session.open())
        return;
    std::vector<uint8_t> wrong = session.package.bytes;
    wrong[0] = static_cast<uint8_t>(wrong[0] + 1);
    fixture::Reply bad;
    bad.status = 200;
    bad.body = wrong;
    session.server.override_next(session.package.path, bad, 1);
    session.server.override_next(session.package.path, std::move(bad), 1);
    session.server.handle("POST", "/api/v1/downloads", [&](const fixture::LoggedRequest&) {
        return issued(session.server, session.package, "2026-02-01T00:00:00Z");
    });
    session.server.handle(
        "POST", "/api/v1/downloads/ridge-dl-1/result", [](const fixture::LoggedRequest&) {
            return json_reply(204, "");
        }
    );
    content::Downloads downloads(session.queue_options());
    downloads.set_install_id(session.registry_id, std::string(kInstallId));
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::failed, 20000));
    const auto view = view_of(downloads, *id);
    OA_CHECK(view.has_value());
    if (view) {
        OA_CHECK(view->failure == content::DownloadFailure::hash_mismatch);
        expect_reason_once(*view);
    }
    OA_CHECK(wait_until([&] { return count_results(session.server) == 1; }, kBusyWaitMs));
    OA_CHECK(result_body(session.server).find("\"result\":\"failed\"") != std::string::npos);
    // The file whose SHA-256 is not the signed catalogue's is logged once,
    // with what the package address answered.
    const std::string line = one_failure_line(session.logs);
    expect_in(line, "failed: the download was not the file the catalogue promised; ");
    expect_in(line, "package: " + session.server.url(session.package.path) + " answered 200, ");
}

void test_wrong_size() {
    Session session("f10-size", 100, 0x47);
    if (!session.open())
        return;
    std::vector<uint8_t> wrong = session.package.bytes;
    wrong.push_back(9);
    wrong.push_back(8);
    session.server.serve_bytes(session.package.path, wrong);
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::failed, kBusyWaitMs));
    const auto view = view_of(downloads, *id);
    OA_CHECK(view.has_value());
    if (view) {
        if (view->failure != content::DownloadFailure::server_file_differs)
            std::fprintf(
                stderr,
                "size failure %d %s\n",
                static_cast<int>(view->failure),
                view->detail.c_str()
            );
        OA_CHECK(view->failure == content::DownloadFailure::server_file_differs);
    }
    std::error_code error;
    if (fs::exists(session.part_path(), error))
        OA_CHECK(fs::file_size(session.part_path(), error) <= session.package.bytes.size());
    OA_CHECK(!fs::exists(session.final_path()));
}

void test_match_pauses() {
    Session session("f10-match", (3u << 20) + 123, 0x48);
    session.extra = package_file(64, 0x49);
    if (!session.open())
        return;
    // 128 KiB each 40 ms: after the first step, about 640 ms of the file remain.
    serve_paced(session.server, session.package, 128 * 1024, 40);
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    const auto snapshot = session.service->snapshot();
    std::optional<content::DownloadTarget> other =
        content::download_target(*snapshot, session.registry_id, session.extra_id, &why);
    OA_CHECK(other.has_value());
    if (!other)
        return;
    const auto second = downloads.queue(std::move(*other), content::DownloadReason::update, &why);
    OA_CHECK(second.has_value());
    // The file keeps going after the first step. Pause from the poll that sees
    // that step, so a stalled test thread cannot watch the download finish first.
    OA_CHECK(wait_until(
        [&] {
            const auto view = view_of(downloads, *id);
            if (!view || view->state != content::DownloadState::downloading ||
                view->done < content::download_step_bytes)
                return false;
            downloads.set_match_running(true);
            return true;
        },
        kBusyWaitMs
    ));
    downloads.set_match_running(true);
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::paused_for_match, kBusyWaitMs));
    const auto paused = view_of(downloads, *id);
    OA_CHECK(paused.has_value());
    if (paused) {
        OA_CHECK(paused->done % content::download_step_bytes == 0);
        OA_CHECK(paused->done > 0);
    }
    std::error_code error;
    const auto part_size = fs::file_size(session.part_path(), error);
    OA_CHECK(!error);
    OA_CHECK(part_size % content::download_step_bytes == 0);
    const auto waiting = view_of(downloads, *second);
    OA_CHECK(waiting.has_value());
    if (waiting)
        OA_CHECK(waiting->state == content::DownloadState::waiting);
    threads::sleep_ms(150);
    const auto still = view_of(downloads, *second);
    OA_CHECK(still.has_value() && still->state == content::DownloadState::waiting);
    downloads.set_match_running(false);
    finish(downloads, *id, content::InstallOutcome::installed);
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::done, 20000));
    OA_CHECK(saw_step_range(session.server));
    finish(downloads, *second, content::InstallOutcome::installed);
    OA_CHECK(wait_state(downloads, *second, content::DownloadState::done, kBusyWaitMs));
}

void test_one_at_a_time() {
    Session session("f10-serial", 90, 0x4a);
    session.extra = package_file(70, 0x4b);
    if (!session.open())
        return;
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto first = session.enqueue(downloads, content::DownloadReason::update, &why);
    const auto snapshot = session.service->snapshot();
    auto other = content::download_target(*snapshot, session.registry_id, session.extra_id, &why);
    OA_CHECK(first.has_value() && other.has_value());
    if (!first || !other)
        return;
    const auto second = downloads.queue(std::move(*other), content::DownloadReason::install, &why);
    OA_CHECK(second.has_value());
    OA_CHECK(wait_state(downloads, *first, content::DownloadState::installing, kBusyWaitMs));
    const auto held = view_of(downloads, *second);
    OA_CHECK(held.has_value() && held->state == content::DownloadState::waiting);
    OA_CHECK(downloads.busy());
    downloads.report_install(*first, content::InstallOutcome::installed);
    OA_CHECK(wait_state(downloads, *first, content::DownloadState::done, kBusyWaitMs));
    finish(downloads, *second, content::InstallOutcome::installed);
    OA_CHECK(wait_state(downloads, *second, content::DownloadState::done, kBusyWaitMs));
    OA_CHECK(!downloads.busy());
}

void test_resume_after_restart() {
    // Three steps, served 128 KiB each 40 ms. A match pauses the worker on the
    // poll that first sees a whole step, so the part is still there when the
    // queue stops: the rest of the file would otherwise end about 640 ms later.
    Session session("f10-restart", (3u << 20) + 80, 0x4c);
    if (!session.open())
        return;
    serve_paced(session.server, session.package, 128 * 1024, 40);
    {
        content::Downloads downloads(session.queue_options());
        downloads.start(*session.service->snapshot());
        std::string why;
        const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
        OA_CHECK(id.has_value());
        if (!id)
            return;
        OA_CHECK(wait_until(
            [&] {
                std::error_code error;
                if (!fs::is_regular_file(session.part_path(), error) || error)
                    return false;
                const auto size = fs::file_size(session.part_path(), error);
                if (error || size < content::download_step_bytes)
                    return false;
                downloads.set_match_running(true);
                return true;
            },
            kBusyWaitMs
        ));
    }
    OA_CHECK(wait_until(
        [&] {
            std::error_code error;
            return fs::is_regular_file(session.part_path(), error) && !error &&
                   fs::file_size(session.part_path(), error) >= content::download_step_bytes;
        },
        kBusyWaitMs
    ));
    OA_CHECK(fs::is_regular_file(session.part_path()));
    session.server.clear_log();
    content::Downloads again(session.queue_options());
    again.start(*session.service->snapshot());
    OA_CHECK(wait_until([&] { return !again.view().empty(); }, kBusyWaitMs));
    OA_CHECK(!again.view().empty());
    if (again.view().empty())
        return;
    const uint64_t id = again.view().front().item;
    finish(again, id, content::InstallOutcome::installed);
    OA_CHECK(wait_state(again, id, content::DownloadState::done, 20000));
    OA_CHECK(saw_step_range(session.server));
    OA_CHECK(file_bytes(session.final_path()) == session.package.bytes);
}

/// A saved queue names a mutator. The kind is read, and the row is dropped
/// because the empty catalogue has no such release. The file itself is read.
void test_saved_oamut_kind() {
    Scratch scratch("u03-oamut");
    std::error_code error;
    const fs::path queue =
        content::downloads_folder(scratch.data()) / std::string(content::queue_file_name);
    fs::create_directories(queue.parent_path(), error);
    OA_CHECK(!error);
    {
        std::ofstream out(queue);
        out << "queue: 1\n"
               "items:\n"
               "  - registry: \"check-builtin\"\n"
               "    key: \"fast-builders\"\n"
               "    kind: \"oamut\"\n"
               "    release: 1\n"
               "    sha256: "
               "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"\n"
               "    reason: \"install\"\n";
        OA_CHECK(static_cast<bool>(out));
    }
    Logs logs;
    content::DownloadsOptions options;
    options.data_folder = scratch.data();
    options.log = remember_log;
    options.log_context = &logs;
    content::Downloads downloads(options);
    downloads.start(content::Snapshot{});
    OA_CHECK(wait_until(
        [&] { return logs_text(logs).find("no longer has that release") != std::string::npos; },
        kBusyWaitMs
    ));
    const std::string text = logs_text(logs);
    OA_CHECK(text.find("no longer has that release") != std::string::npos);
    OA_CHECK(text.find("could not be read") == std::string::npos);
}

void test_drop_changed_release() {
    Session session("f10-drop", 60, 0x4d);
    if (!session.open())
        return;
    {
        content::Downloads downloads(session.queue_options());
        downloads.start(*session.service->snapshot());
        std::string why;
        const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
        OA_CHECK(id.has_value());
        const fs::path queue = session.downloads() / std::string(content::queue_file_name);
        OA_CHECK(wait_until(
            [&] {
                const std::vector<uint8_t> bytes = file_bytes(queue);
                const std::string text(bytes.begin(), bytes.end());
                return text.find(session.package_id) != std::string::npos;
            },
            kBusyWaitMs
        ));
    }
    Listed listed{&session.package, session.package_id, "Ridge", 2};
    const SignedBytes document = sign_json(
        catalogue_json(session.registry_id, 2, &session.key.key, std::span(&listed, 1)), session.key
    );
    serve_document(session.server, "/v1/catalogue.json", document);
    session.service->refresh(content::RefreshReason::check_now);
    OA_CHECK(wait_until(
        [&] {
            const auto snapshot = session.service->snapshot();
            if (!snapshot)
                return false;
            for (const content::Entry& entry : snapshot->entries) {
                if (entry.package != nullptr && entry.package->id == session.package_id)
                    return entry.package->release == 2;
            }
            return false;
        },
        kBusyWaitMs
    ));
    Logs fresh;
    content::DownloadsOptions options = session.queue_options();
    options.log_context = &fresh;
    content::Downloads again(options);
    again.start(*session.service->snapshot());
    OA_CHECK(wait_until(
        [&] { return logs_text(fresh).find("no longer has that release") != std::string::npos; },
        kBusyWaitMs
    ));
    OA_CHECK(logs_text(fresh).find("no longer has that release") != std::string::npos);
    OA_CHECK(again.view().empty());
}

void test_saved_before_start() {
    // A package queued while the worker waits for work is in the saved queue
    // before the worker starts it, so the queue keeps it while the installer
    // holds it.
    Session session("f10-saved", 40, 0x57);
    if (!session.open())
        return;
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    // Time for the worker to read the saved queue and wait for work.
    threads::sleep_ms(200);
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::install, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::installing, kBusyWaitMs));
    const std::vector<uint8_t> bytes =
        file_bytes(session.downloads() / std::string(content::queue_file_name));
    OA_CHECK(std::string(bytes.begin(), bytes.end()).find(session.package_id) != std::string::npos);
    finish(downloads, *id, content::InstallOutcome::installed);
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::done, kBusyWaitMs));
}

void test_install_id_off() {
    fixture::Server server;
    if (!listening(server))
        return;
    Scratch scratch("f10-id-off");
    content::DownloadTarget target;
    target.registry = "ridge";
    target.registry_name = "Ridge";
    target.key = "ridge";
    target.name = "Ridge";
    target.kind = catalogue::Kind::oamod;
    target.release = 1;
    target.size = 32;
    target.mode = registry::DownloadMode::tokens;
    target.install_id = registry::InstallIdUse::required;
    const auto api = url::parse_http_url(server.url("/api/v1"));
    const auto file = url::parse_http_url(server.url("/v1/p/file.oamod"));
    OA_CHECK(api.has_value() && file.has_value());
    if (!api || !file)
        return;
    target.api = *api;
    target.files.push_back(*file);
    Logs logs;
    Handed handed;
    content::DownloadsOptions options;
    options.data_folder = scratch.data();
    options.language = "en";
    options.handoff = record_handoff;
    options.handoff_context = &handed;
    options.log = remember_log;
    options.log_context = &logs;
    options.clock = test_clock;
    options.retry_base_ms = 40;
    content::Downloads downloads(std::move(options));
    downloads.start(content::Snapshot{});
    std::string why;
    OA_CHECK(!downloads.queue(target, content::DownloadReason::update, &why));
    OA_CHECK(why == content::failure_text(content::DownloadFailure::install_id_off));
    downloads.set_match_running(true);
    downloads.set_install_id("ridge", std::string(kInstallId));
    const auto id = downloads.queue(target, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    downloads.set_install_id("ridge", std::nullopt);
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::failed, kBusyWaitMs));
    const auto view = view_of(downloads, *id);
    OA_CHECK(view.has_value());
    if (view) {
        OA_CHECK(view->failure == content::DownloadFailure::install_id_off);
        expect_reason_once(*view);
    }
    OA_CHECK(server.requests().empty());
    // A failure the main thread made is logged too, with no answer to name.
    const std::string line = one_failure_line(logs);
    expect_in(line, "failed: downloads are off for this registry");
    expect_not_in(line, "answered");
}

void test_no_install_member() {
    Session session("f10-no-id", 36, 0x4e);
    session.tokens = true;
    session.install_id = registry::InstallIdUse::none;
    if (!session.open())
        return;
    session.server.handle("POST", "/api/v1/downloads", [&](const fixture::LoggedRequest&) {
        return issued(session.server, session.package, "2026-02-01T00:00:00Z");
    });
    content::Downloads downloads(session.queue_options());
    downloads.set_install_id(session.registry_id, std::string(kInstallId));
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_until(
        [&] { return count_method(session.server, "POST", "/api/v1/downloads") >= 1; }, kBusyWaitMs
    ));
    const std::string sent = body_at(session.server, "POST", "/api/v1/downloads", 0);
    OA_CHECK(sent.find("install") == std::string::npos);
    OA_CHECK(sent == expected_key(session, std::nullopt, content::DownloadReason::update));
    finish(downloads, *id, content::InstallOutcome::installed);
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::done, kBusyWaitMs));
}

void test_cancel_keeps_part() {
    // Three steps, served 128 KiB each 40 ms: the part holds a whole step for
    // about 640 ms before the download ends, which a busy machine's polls see.
    Session session("f10-cancel", (3u << 20) + 200, 0x4f);
    if (!session.open())
        return;
    serve_paced(session.server, session.package, 128 * 1024, 40);
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    // Cancel from the poll that sees the step. The worker still closes the part
    // after the state is cancelled, so the size is read again after that.
    OA_CHECK(wait_until(
        [&] {
            std::error_code error;
            if (!fs::is_regular_file(session.part_path(), error) || error)
                return false;
            const auto size = fs::file_size(session.part_path(), error);
            if (error || size < content::download_step_bytes)
                return false;
            downloads.cancel(*id);
            return true;
        },
        kBusyWaitMs
    ));
    downloads.cancel(*id);
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::cancelled, kBusyWaitMs));
    OA_CHECK(wait_until(
        [&] {
            std::error_code error;
            if (!fs::is_regular_file(session.part_path(), error) || error)
                return false;
            const auto size = fs::file_size(session.part_path(), error);
            return !error && size >= content::download_step_bytes;
        },
        kBusyWaitMs
    ));
    std::error_code kept_error;
    const auto kept = fs::file_size(session.part_path(), kept_error);
    OA_CHECK(!kept_error);
    OA_CHECK(kept >= content::download_step_bytes);
    session.server.clear_log();
    const auto again = session.enqueue(downloads, content::DownloadReason::update, &why);
    OA_CHECK(again.has_value());
    if (!again)
        return;
    finish(downloads, *again, content::InstallOutcome::installed);
    OA_CHECK(wait_state(downloads, *again, content::DownloadState::done, 20000));
    OA_CHECK(saw_step_range(session.server));
    OA_CHECK(file_bytes(session.final_path()) == session.package.bytes);
}

void test_cancel_registry() {
    fixture::Server server;
    if (!listening(server))
        return;
    Scratch scratch("f10-cancel-reg");
    TestKey key("release-2026", 0x51);
    const PackageFile ridge = package_file(40, 0x51);
    const PackageFile vale = package_file(40, 0x52);
    Listed ridge_listed{&ridge, "ridge", "Ridge", 1};
    Listed vale_listed{&vale, "vale", "Vale", 1};
    const SignedBytes ridge_doc =
        sign_json(catalogue_json("ridge", 1, &key.key, std::span(&ridge_listed, 1)), key);
    const SignedBytes vale_doc =
        sign_json(catalogue_json("vale", 1, &key.key, std::span(&vale_listed, 1)), key);
    serve_document(server, "/ridge.json", ridge_doc);
    serve_document(server, "/vale.json", vale_doc);
    server.serve_bytes(ridge.path, ridge.bytes);
    server.serve_bytes(vale.path, vale.bytes);
    OA_CHECK(install_builtin(
        scratch, "ridge.yaml", descriptor_for("ridge", "Ridge", server.url("/ridge.json"), &key)
    ));
    OA_CHECK(install_builtin(
        scratch, "vale.yaml", descriptor_for("vale", "Vale", server.url("/vale.json"), &key)
    ));
    Logs logs;
    content::ServiceOptions options;
    options.data_folder = scratch.data();
    options.player_folder = scratch.player();
    options.builtin_folder = scratch.builtin();
    options.log = remember_log;
    options.log_context = &logs;
    options.clock = test_clock;
    content::Service service(std::move(options));
    service.start();
    OA_CHECK(wait_until(
        [&] {
            const auto snapshot = service.snapshot();
            return snapshot && snapshot->entries.size() >= 2;
        },
        kBusyWaitMs
    ));
    Handed handed;
    content::DownloadsOptions queue;
    queue.data_folder = scratch.data();
    queue.language = "en";
    queue.handoff = record_handoff;
    queue.handoff_context = &handed;
    queue.log = remember_log;
    queue.log_context = &logs;
    queue.clock = test_clock;
    queue.retry_base_ms = 40;
    content::Downloads downloads(std::move(queue));
    downloads.set_match_running(true);
    downloads.start(*service.snapshot());
    std::string why;
    const auto snapshot = service.snapshot();
    auto ridge_target = content::download_target(*snapshot, "ridge", "ridge", &why);
    auto vale_target = content::download_target(*snapshot, "vale", "vale", &why);
    OA_CHECK(ridge_target.has_value() && vale_target.has_value());
    if (!ridge_target || !vale_target)
        return;
    const auto ridge_id =
        downloads.queue(std::move(*ridge_target), content::DownloadReason::update, &why);
    const auto vale_id =
        downloads.queue(std::move(*vale_target), content::DownloadReason::update, &why);
    OA_CHECK(ridge_id.has_value() && vale_id.has_value());
    downloads.cancel_registry("ridge");
    const auto ridge_view = view_of(downloads, *ridge_id);
    const auto vale_view = view_of(downloads, *vale_id);
    OA_CHECK(ridge_view.has_value() && ridge_view->state == content::DownloadState::cancelled);
    OA_CHECK(vale_view.has_value() && vale_view->state == content::DownloadState::waiting);
    downloads.set_match_running(false);
    finish(downloads, *vale_id, content::InstallOutcome::installed);
    OA_CHECK(wait_state(downloads, *vale_id, content::DownloadState::done, kBusyWaitMs));
    OA_CHECK(view_of(downloads, *ridge_id)->state == content::DownloadState::cancelled);
}

void test_final_already_there() {
    Session session("f10-present", 55, 0x53);
    if (!session.open())
        return;
    OA_CHECK(write_bytes(session.final_path(), session.package.bytes));
    session.server.clear_log();
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::repair, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    finish(downloads, *id, content::InstallOutcome::installed);
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::done, kBusyWaitMs));
    OA_CHECK(session.server.requests().empty());
    OA_CHECK(handed_count(session.handed) == 1);
    threads::LockGuard guard(session.handed.mutex);
    OA_CHECK(session.handed.origins[0].reason == content::DownloadReason::repair);
}

void test_no_space() {
    fixture::Server server;
    if (!listening(server))
        return;
    Scratch scratch("f10-space");
    std::error_code error;
    fs::create_directories(content::downloads_folder(scratch.data()), error);
    content::DownloadTarget target;
    target.registry = "ridge";
    target.key = "ridge";
    target.name = "Ridge";
    target.kind = catalogue::Kind::oamod;
    target.release = 1;
    target.size = std::numeric_limits<uint64_t>::max();
    target.mode = registry::DownloadMode::direct;
    const auto file = url::parse_http_url(server.url("/v1/p/too-big.oamod"));
    OA_CHECK(file.has_value());
    if (!file)
        return;
    target.files.push_back(*file);
    Logs logs;
    Handed handed;
    content::DownloadsOptions options;
    options.data_folder = scratch.data();
    options.language = "en";
    options.handoff = record_handoff;
    options.handoff_context = &handed;
    options.log = remember_log;
    options.log_context = &logs;
    options.clock = test_clock;
    content::Downloads downloads(std::move(options));
    downloads.start(content::Snapshot{});
    server.clear_log();
    std::string why;
    const auto id = downloads.queue(std::move(target), content::DownloadReason::update, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::failed, kBusyWaitMs));
    const auto view = view_of(downloads, *id);
    OA_CHECK(view.has_value());
    if (view)
        OA_CHECK(view->failure == content::DownloadFailure::no_space);
    OA_CHECK(server.requests().empty());
}

} // namespace

/// A package address that answers with the firewall's captcha: the item
/// fails at once with the check's message and one log line, and nothing is
/// asked again.
void test_check_on_package() {
    Session session("g124-check-package", 64, 0x51);
    session.tokens = true;
    session.install_id = registry::InstallIdUse::required;
    session.serve_package = false;
    if (!session.open())
        return;
    // A signed address, whose query holds the key the log must leave out.
    const std::string query = "?Expires=1767312000&Signature=secret-signature&Key-Pair-Id=K2TEST";
    const std::string signed_url = session.server.url(session.package.path) + query;
    session.server.handle("POST", "/api/v1/downloads", [&](const fixture::LoggedRequest&) {
        return json_reply(
            201,
            "{\"download\":\"ridge-dl-1\",\"expires\":\"2026-02-01T00:00:00Z\",\"url\":\"" +
                signed_url + "\"}"
        );
    });
    session.server.handle(
        "POST", "/api/v1/downloads/ridge-dl-1/result", [](const fixture::LoggedRequest&) {
            return json_reply(204, "");
        }
    );
    session.server.handle("GET", session.package.path, [](const fixture::LoggedRequest&) {
        return browser_page(405, "captcha");
    });
    content::Downloads downloads(session.queue_options());
    downloads.set_install_id(session.registry_id, std::string(kInstallId));
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::install, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::failed, kBusyWaitMs));
    const auto view = view_of(downloads, *id);
    OA_CHECK(view.has_value());
    if (view) {
        OA_CHECK(view->failure == content::DownloadFailure::server_check);
        OA_CHECK(view->detail.empty());
        const std::string message = content::failure_message(*view, nullptr);
        if (message != "Ridge asked for a check the game cannot do. Try again later.")
            std::fprintf(stderr, "check message: %s\n", message.c_str());
        OA_CHECK(message == "Ridge asked for a check the game cannot do. Try again later.");
        expect_reason_once(*view);
    }
    const std::string line = one_failure_line(session.logs);
    expect_in(line, "download of Ridge (ridge ridge, release 1) failed: ");
    expect_in(line, "the server asked for a check the game cannot do; ");
    expect_in(line, "package: " + session.server.url(session.package.path) + " answered 405, ");
    expect_in(line, "text/html; charset=UTF-8, ");
    expect_in(line, "a check meant for a web browser (x-amzn-waf-action: captcha)");
    expect_not_in(line, "Signature");
    expect_not_in(line, "secret-signature");
    expect_not_in(line, "Key-Pair-Id");
    // The check is never answered, nor the package asked for again.
    threads::sleep_ms(200);
    OA_CHECK(count_method(session.server, "GET", session.package.path + query) == 1);
    OA_CHECK(count_method(session.server, "POST", "/api/v1/downloads") == 1);
}

/// A key request the firewall answers with a challenge page: the item fails
/// as a check, logged once, with no second request.
void test_check_on_key_request() {
    Session session("g124-check-key", 48, 0x52);
    session.tokens = true;
    session.serve_package = false;
    if (!session.open())
        return;
    session.server.handle("POST", "/api/v1/downloads", [](const fixture::LoggedRequest&) {
        return browser_page(202, "challenge");
    });
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::install, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::failed, kBusyWaitMs));
    const auto view = view_of(downloads, *id);
    OA_CHECK(view.has_value());
    if (view) {
        OA_CHECK(view->failure == content::DownloadFailure::server_check);
        expect_reason_once(*view);
    }
    const std::string line = one_failure_line(session.logs);
    expect_in(line, "failed: the server asked for a check the game cannot do; key request: ");
    expect_in(line, "/api/v1/downloads answered 202, text/html; charset=UTF-8, ");
    expect_in(line, "(x-amzn-waf-action: challenge)");
    expect_not_in(line, "package: ");
    threads::sleep_ms(200);
    OA_CHECK(count_method(session.server, "POST", "/api/v1/downloads") == 1);
    OA_CHECK(count_method(session.server, "GET", session.package.path) == 0);
}

/// A web page where the package was expected, with no firewall header, is
/// a check too.
void test_page_where_package_expected() {
    Session session("g124-check-page", 40, 0x53);
    session.serve_package = false;
    if (!session.open())
        return;
    const fixture::Reply page = browser_page(200, {});
    session.server.serve_bytes(session.package.path, page.body, "text/html");
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::install, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::failed, kBusyWaitMs));
    const auto view = view_of(downloads, *id);
    OA_CHECK(view.has_value());
    if (view)
        OA_CHECK(view->failure == content::DownloadFailure::server_check);
    const std::string line = one_failure_line(session.logs);
    expect_in(line, "failed: the server asked for a check the game cannot do; package: ");
    expect_in(line, " answered 200, text/html, ");
    expect_in(line, "a check meant for a web browser");
    OA_CHECK(count_method(session.server, "GET", session.package.path) == 1);
    OA_CHECK(!fs::exists(session.final_path()));
}

/// A body shorter than the catalogue's size is logged with its status and length.
void test_short_body() {
    Session session("g124-short", 100, 0x54);
    if (!session.open())
        return;
    const std::vector<uint8_t> shorter(
        session.package.bytes.begin(), session.package.bytes.begin() + 60
    );
    session.server.serve_bytes(session.package.path, shorter);
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::install, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::failed, kBusyWaitMs));
    const auto view = view_of(downloads, *id);
    OA_CHECK(view.has_value());
    if (view) {
        OA_CHECK(view->failure == content::DownloadFailure::server_file_differs);
        expect_reason_once(*view);
    }
    const std::string line = one_failure_line(session.logs);
    expect_in(line, "failed: the server's file is not the size the catalogue promised; package: ");
    expect_in(line, " answered 200, application/octet-stream, 60 bytes");
}

/// A server that cannot be reached is logged with the network failure.
void test_network_error() {
    Session session("g124-network", 50, 0x55);
    if (!session.open())
        return;
    session.server.stop();
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::install, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::failed, kBusyWaitMs));
    const auto view = view_of(downloads, *id);
    OA_CHECK(view.has_value());
    if (view) {
        OA_CHECK(view->failure == content::DownloadFailure::unreachable);
        expect_reason_once(*view);
    }
    const std::string line = one_failure_line(session.logs);
    expect_in(line, "failed: the server could not be reached; package: ");
    expect_in(line, session.package.path + ": the connection could not be opened");
}

/// The installer's reason for a refusal is the detail, the message and the
/// log line's, each once.
void test_install_refusal_reason() {
    Session session("g124-refused", 30, 0x56);
    if (!session.open())
        return;
    content::Downloads downloads(session.queue_options());
    downloads.start(*session.service->snapshot());
    std::string why;
    const auto id = session.enqueue(downloads, content::DownloadReason::install, &why);
    OA_CHECK(id.has_value());
    if (!id)
        return;
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::installing, kBusyWaitMs));
    const std::string reason = "The folder ridge does not hold this map pack. It is left as it is.";
    downloads.report_install(*id, content::InstallOutcome::refused, reason);
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::failed, kBusyWaitMs));
    const auto view = view_of(downloads, *id);
    OA_CHECK(view.has_value());
    if (view) {
        OA_CHECK(view->failure == content::DownloadFailure::install_refused);
        OA_CHECK(view->detail == reason);
        OA_CHECK(content::failure_message(*view, nullptr) == reason);
        // The line GET MAP shows after a colon names the installer's reason too.
        OA_CHECK(content::failure_reason(*view, nullptr) == reason);
        expect_reason_once(*view);
    }
    const std::string line = one_failure_line(session.logs);
    expect_in(line, "failed: the package was refused (" + reason + "); package: ");
    expect_in(line, " answered 200, ");
    OA_CHECK(count_folded(line, reason) == 1);
    // RETRY asks again, and a second failure is logged once more.
    downloads.retry(*id);
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::installing, kBusyWaitMs));
    downloads.report_install(*id, content::InstallOutcome::refused, {});
    OA_CHECK(wait_state(downloads, *id, content::DownloadState::failed, kBusyWaitMs));
    OA_CHECK(wait_until([&] { return failure_lines(session.logs).size() == 2; }, kBusyWaitMs));
    const auto again = view_of(downloads, *id);
    OA_CHECK(again.has_value());
    if (again) {
        OA_CHECK(again->detail.empty());
        OA_CHECK(content::failure_message(*again, nullptr) == "The package was refused");
        OA_CHECK(content::failure_reason(*again, nullptr) == "the package was refused");
    }
}

int main() {
    test_direct_large();
    test_tokens_outcome(content::InstallOutcome::installed, "installed", true);
    test_tokens_outcome(content::InstallOutcome::refused, "refused", false);
    test_tokens_outcome(content::InstallOutcome::set_aside, "cancelled", false);
    test_cut_then_range();
    test_whole_answer_to_range();
    test_package_rekey();
    test_key_expires_soon();
    test_challenge_solved();
    test_challenge_expired();
    test_challenge_cancel();
    test_not_offered(404, "unknown_package");
    test_not_offered(409, "release_mismatch");
    test_not_offered(404, "other_code");
    test_client_fault(400);
    test_client_fault(413);
    test_rate_limit();
    test_unavailable_then_mirror();
    test_refusal_skips_mirror();
    test_wrong_hash_once();
    test_wrong_hash_twice();
    test_wrong_size();
    test_match_pauses();
    test_one_at_a_time();
    test_resume_after_restart();
    test_saved_oamut_kind();
    test_drop_changed_release();
    test_saved_before_start();
    test_install_id_off();
    test_no_install_member();
    test_cancel_keeps_part();
    test_cancel_registry();
    test_final_already_there();
    test_no_space();
    test_check_on_package();
    test_check_on_key_request();
    test_page_where_package_expected();
    test_short_body();
    test_network_error();
    test_install_refusal_reason();
    return oa::test::check_exit_status();
}
