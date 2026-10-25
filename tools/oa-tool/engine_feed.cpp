// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// engine-feed write, sign and check. The feed is accepted only by the same
// check the game uses. Nothing here is fetched except check --url.

#include "arguments.hpp"
#include "command.hpp"
#include "files.hpp"
#include "passphrase.hpp"

#include "oa/base/sha256.hpp"
#include "oa/base/signing/ed25519.hpp"
#include "oa/base/signing/sealed_key.hpp"
#include "oa/data/catalogue/catalogue.hpp"
#include "oa/data/catalogue/check.hpp"
#include "oa/data/engine_feed/feed.hpp"
#include "oa/data/registry/trust.hpp"
#include "oa/formats/json.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/formats/url.hpp"
#include "oa/netgame/http/client.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::tool {
namespace {

namespace fs = std::filesystem;
namespace signing = oa::base::signing;
namespace sha256 = oa::base::sha256;
namespace catalogue = oa::data::catalogue;
namespace feed = oa::data::engine_feed;
namespace registry = oa::data::registry;
namespace json = oa::formats::json;
namespace oamod = oa::formats::oamod;
namespace url = oa::formats::url;
namespace http = oa::netgame::http;

constexpr int64_t max_release_number = static_cast<int64_t>(uint64_t{1} << 53);
constexpr uint64_t max_file_bytes = uint64_t{1} << 40;

void report_usage(Output& output, std::string_view command, std::string_view problem) {
    output.err << "oa-tool engine-feed " << command << ": " << problem << '\n';
    output.err << "run 'oa-tool help engine-feed " << command << "'\n";
}

fs::path path_from(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string utf8_of(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

std::span<const uint8_t> as_bytes(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

const std::string* required_option(
    const Arguments& parsed, Output& output, std::string_view command, std::string_view name
) {
    const std::string* value = parsed.value(name);
    if (value == nullptr || value->empty()) {
        report_usage(output, command, "option '--" + std::string(name) + "' needs a value");
        return nullptr;
    }
    return value;
}

std::optional<fs::path>
passphrase_path(const Arguments& parsed, Output& output, std::string_view command, bool& usage) {
    const std::string* value = parsed.value("passphrase-file");
    if (value == nullptr)
        return std::nullopt;
    if (value->empty()) {
        report_usage(output, command, "option '--passphrase-file' needs a value");
        usage = true;
        return std::nullopt;
    }
    return path_from(*value);
}

/// True when `text` is `[a-z0-9]+`.
bool channel_name(std::string_view text) {
    if (text.empty())
        return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        const bool letter = byte >= 'a' && byte <= 'z';
        const bool digit = byte >= '0' && byte <= '9';
        if (!letter && !digit)
            return false;
    }
    return true;
}

/// True when `text` is `[a-z0-9]+(-[a-z0-9_]+)*`.
bool package_id_text(std::string_view text) {
    std::size_t index = 0;
    auto piece = [&](bool allow_underscore) {
        const std::size_t start = index;
        while (index < text.size()) {
            const unsigned char byte = static_cast<unsigned char>(text[index]);
            const bool letter = byte >= 'a' && byte <= 'z';
            const bool digit = byte >= '0' && byte <= '9';
            const bool underscore = allow_underscore && byte == '_';
            if (!letter && !digit && !underscore)
                break;
            ++index;
        }
        return index > start;
    };
    if (!piece(false))
        return false;
    while (index < text.size()) {
        if (text[index] != '-')
            return false;
        ++index;
        if (!piece(true))
            return false;
    }
    return true;
}

/// Reads a whole number with no leading zero. `zero_ok` allows a lone 0.
bool parse_whole(std::string_view text, int64_t most, bool zero_ok, int64_t& value) {
    if (text.empty())
        return false;
    if (text == "0") {
        if (!zero_ok)
            return false;
        value = 0;
        return true;
    }
    if (text.front() == '0')
        return false;
    int64_t built = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        if (byte < '0' || byte > '9')
            return false;
        const int64_t piece = static_cast<int64_t>(byte - '0');
        if (built > (std::numeric_limits<int64_t>::max() - piece) / 10)
            return false;
        built = built * 10 + piece;
    }
    if (built < 1 || built > most)
        return false;
    value = built;
    return true;
}

/// An http or https address whose path ends in `/` and which has no query.
std::optional<url::Url> read_mirror(std::string_view text) {
    const std::optional<url::Url> parsed = url::parse_http_url(text);
    if (!parsed || url::has_query(*parsed))
        return std::nullopt;
    if (parsed->target.empty() || parsed->target.back() != '/')
        return std::nullopt;
    return parsed;
}

/// One package file the command was given, named but not yet hashed.
struct NamedFile {
    fs::path path;
    std::string id;
    bool installer = false;
};

/// Reads a release file's name. The version in the name must be `version`.
std::optional<NamedFile>
parse_release_file(const fs::path& path, std::string_view version, std::string& problem) {
    const std::string name = utf8_of(path.filename());
    constexpr std::string_view prefix = "open-annihilation-";
    if (!name.starts_with(prefix)) {
        problem = name + " is not a release file";
        return std::nullopt;
    }
    std::string_view rest(name.data() + prefix.size(), name.size() - prefix.size());
    if (!rest.empty() && rest.front() == 'v')
        rest.remove_prefix(1);
    const bool same_version =
        rest.starts_with(version) && rest.size() > version.size() && rest[version.size()] == '-';
    if (!same_version) {
        const std::size_t dash = rest.find('-');
        const std::string_view head = dash == std::string_view::npos ? rest : rest.substr(0, dash);
        if (oamod::parse_engine_version(head))
            problem = name + " names another version";
        else
            problem = name + " is not a release file";
        return std::nullopt;
    }
    rest.remove_prefix(version.size() + 1);
    NamedFile named;
    named.path = path;
    constexpr std::string_view zip_ext = ".zip";
    constexpr std::string_view setup_ext = "-setup.exe";
    if (rest.ends_with(zip_ext)) {
        named.id = std::string(rest.substr(0, rest.size() - zip_ext.size()));
        named.installer = false;
    } else if (rest.ends_with(setup_ext)) {
        named.id = std::string(rest.substr(0, rest.size() - setup_ext.size()));
        named.installer = true;
    } else if (rest == "macos-universal.pkg") {
        named.id = "macos-universal";
        named.installer = true;
    } else {
        problem = name + " is not a release file";
        return std::nullopt;
    }
    if (!package_id_text(named.id)) {
        problem = name + " is not a release file";
        return std::nullopt;
    }
    return named;
}

/// The size and SHA-256 of one package file, read in pieces.
feed::PackageFile hash_file(const fs::path& path) {
    std::error_code io;
    const auto bytes = fs::file_size(path, io);
    if (io)
        throw Failure("cannot read " + utf8_of(path));
    if (bytes == 0 || bytes > max_file_bytes)
        throw Failure(utf8_of(path.filename()) + " is not a size from 1 to 2^40");
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw Failure("cannot read " + utf8_of(path));
    sha256::Hasher hasher;
    std::array<char, 64 * 1024> buffer{};
    uint64_t read = 0;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            const auto* data = reinterpret_cast<const uint8_t*>(buffer.data());
            sha256::update(hasher, std::span<const uint8_t>(data, static_cast<std::size_t>(got)));
            read += static_cast<uint64_t>(got);
        }
    }
    if (in.bad() || read != bytes)
        throw Failure("cannot read " + utf8_of(path));
    feed::PackageFile file;
    file.file = utf8_of(path.filename());
    file.size = static_cast<uint64_t>(bytes);
    file.sha256 = sha256::finish(hasher);
    return file;
}

/// The feed file, when it is at most the feed's cap. A missing file is a failure.
std::vector<uint8_t> read_capped_feed(const fs::path& path) {
    std::error_code io;
    const auto size = fs::file_size(path, io);
    if (io)
        throw Failure("cannot read the feed");
    if (size > feed::max_feed_bytes)
        throw Failure("the feed is larger than 256 KiB");
    return read_file(path);
}

/// Notes from a JSON object of language to lines. The feed check is the backstop.
std::vector<feed::Notes> read_notes_file(const fs::path& path) {
    const std::vector<uint8_t> bytes = read_file(path);
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    json::JsonError json_error;
    const std::optional<json::Json> root = json::parse_json(text, json_error);
    if (!root || root->type() != json::JsonType::object)
        throw Failure("notes is not an object of language to lines");
    std::vector<feed::Notes> notes;
    const auto names = root->names();
    const auto values = root->values();
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (values[index].type() != json::JsonType::array)
            throw Failure("notes is not an object of language to lines");
        feed::Notes one;
        one.language = names[index];
        for (const json::Json& element : values[index].elements()) {
            const std::string* line = element.string();
            if (line == nullptr)
                throw Failure("notes is not an object of language to lines");
            one.lines.push_back(*line);
        }
        notes.push_back(std::move(one));
    }
    return notes;
}

/// One GET, at most max_feed_bytes. A body past that prints too_large and sets `refused`.
std::vector<uint8_t>
fetch_bytes(http::Client& client, const std::string& address, Output& output, bool& refused) {
    http::Request request;
    request.method = http::Method::get;
    request.url = address;
    request.limits.max_body_bytes = feed::max_feed_bytes;
    std::vector<uint8_t> body;
    const http::Response response = client.fetch(request, body);
    if (response.failure == http::Failure::too_large) {
        output.out << feed::verdict_text(feed::Verdict::too_large) << '\n';
        refused = true;
        return {};
    }
    if (response.failure != http::Failure::none)
        throw Failure(std::string(http::failure_text(response.failure)));
    if (response.status != 200)
        throw Failure("the server answered " + std::to_string(response.status));
    return body;
}

/// Prints the verdict and returns the failed exit. A usable feed is not passed here.
int print_refusal(Output& output, const feed::Checked& checked) {
    output.out << feed::verdict_text(checked.verdict) << '\n';
    return exit_failed;
}

/// Prints the signature and one line for each release.
void print_feed(Output& output, const feed::Checked& checked) {
    output.out << "good signature by " << checked.signed_by << '\n';
    for (const feed::Release& release : checked.feed->releases) {
        output.out << release.release << ' ' << release.version_text << ' ' << release.date
                   << " from " << oamod::engine_version_text(release.from);
        for (const feed::Package& package : release.packages)
            output.out << ' ' << package.id << ' ' << package.zip.size;
        output.out << '\n';
    }
}

} // namespace

int run_engine_feed_write(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {
        {"version", true, false},
        {"release", true, false},
        {"date", true, false},
        {"from", true, false},
        {"page", true, false},
        {"mirror", true, true},
        {"notes", true, false},
        {"rollout", true, false},
        {"online-min", true, false},
        {"channel", true, false},
        {"previous", true, false},
        {"out", true, false},
    };
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed) {
        report_usage(output, "write", problem);
        return exit_usage;
    }
    const std::string* version_text = required_option(*parsed, output, "write", "version");
    const std::string* date_text = required_option(*parsed, output, "write", "date");
    const std::string* from_text = required_option(*parsed, output, "write", "from");
    const std::string* page_text = required_option(*parsed, output, "write", "page");
    const std::string* out_text = required_option(*parsed, output, "write", "out");
    if (version_text == nullptr || date_text == nullptr || from_text == nullptr ||
        page_text == nullptr || out_text == nullptr)
        return exit_usage;
    const auto mirrors_found = parsed->values.find("mirror");
    if (mirrors_found == parsed->values.end() || mirrors_found->second.empty()) {
        report_usage(output, "write", "pass at least one --mirror");
        return exit_usage;
    }
    if (parsed->positional.empty()) {
        report_usage(output, "write", "pass at least one package");
        return exit_usage;
    }

    const std::optional<oamod::EngineVersion> version = oamod::parse_engine_version(*version_text);
    if (!version)
        throw Failure("version is not a version");
    int64_t release_value = 0;
    if (const std::string* release_text = parsed->value("release")) {
        if (!parse_whole(*release_text, max_release_number, false, release_value))
            throw Failure("release is not a whole number from 1 to 2^53");
    } else {
        const std::optional<int64_t> derived = feed::release_number(*version);
        if (!derived)
            throw Failure("pass --release; this version has no release number");
        release_value = *derived;
    }
    if (!catalogue::parse_day(*date_text))
        throw Failure("date is not a day");
    const std::optional<oamod::EngineVersion> from = oamod::parse_engine_version(*from_text);
    if (!from)
        throw Failure("from is not a version");
    if (!url::parse_http_url(*page_text))
        throw Failure("page is not an http or https address");

    std::vector<url::Url> mirrors;
    for (const std::string& mirror_text : mirrors_found->second) {
        const std::optional<url::Url> mirror = read_mirror(mirror_text);
        if (!mirror)
            throw Failure("a mirror is not an http or https address ending in / with no query");
        mirrors.push_back(*mirror);
    }

    std::string channel(feed::stable_channel);
    if (const std::string* channel_text = parsed->value("channel")) {
        if (!channel_name(*channel_text))
            throw Failure("channel is not lower-case letters and digits");
        channel = *channel_text;
    }
    int32_t rollout = 100;
    if (const std::string* rollout_text = parsed->value("rollout")) {
        int64_t rollout_value = 0;
        if (!parse_whole(*rollout_text, 100, true, rollout_value))
            throw Failure("rollout is not a whole number from 0 to 100");
        rollout = static_cast<int32_t>(rollout_value);
    }
    std::optional<oamod::EngineVersion> online_min;
    if (const std::string* online_text = parsed->value("online-min")) {
        online_min = oamod::parse_engine_version(*online_text);
        if (!online_min)
            throw Failure("online_min is not a version");
    }

    std::vector<NamedFile> named;
    for (const std::string& package_text : parsed->positional) {
        std::string name_problem;
        const std::optional<NamedFile> one =
            parse_release_file(path_from(package_text), *version_text, name_problem);
        if (!one)
            throw Failure(name_problem);
        named.push_back(*one);
    }

    feed::Feed written;
    written.channel = channel;
    written.online_min = online_min;
    feed::Release release;
    release.release = release_value;
    release.version = *version;
    release.version_text = *version_text;
    release.date = *date_text;
    release.from = *from;
    release.rollout = rollout;
    release.page = *page_text;
    if (const std::string* notes_text = parsed->value("notes"))
        release.notes = read_notes_file(path_from(*notes_text));

    for (const NamedFile& file : named) {
        std::size_t slot = release.packages.size();
        for (std::size_t index = 0; index < release.packages.size(); ++index) {
            if (release.packages[index].id == file.id) {
                slot = index;
                break;
            }
        }
        const bool fresh = slot == release.packages.size();
        if (fresh) {
            feed::Package created;
            created.id = file.id;
            created.mirrors = mirrors;
            release.packages.push_back(std::move(created));
        }
        const feed::PackageFile hashed = hash_file(file.path);
        if (file.installer) {
            if (release.packages[slot].installer)
                throw Failure("two installers for " + file.id);
            release.packages[slot].installer = hashed;
        } else if (!release.packages[slot].zip.file.empty()) {
            throw Failure("two zips for " + file.id);
        } else {
            release.packages[slot].zip = hashed;
        }
    }
    for (const feed::Package& package : release.packages) {
        if (package.zip.file.empty())
            throw Failure("an installer has no zip of its id");
    }
    written.releases.push_back(std::move(release));

    if (const std::string* previous_text = parsed->value("previous")) {
        const std::vector<uint8_t> previous_bytes = read_capped_feed(path_from(*previous_text));
        std::string error;
        const std::optional<feed::Feed> earlier = feed::read_feed(previous_bytes, &error);
        if (!earlier)
            throw Failure(error);
        if (earlier->channel != written.channel)
            throw Failure("the earlier feed is for another channel");
        const int64_t newest = written.releases[0].release;
        for (const feed::Release& candidate : earlier->releases) {
            if (candidate.release > newest)
                throw Failure("a previous release is newer");
            if (candidate.release == newest)
                continue;
            if (written.releases.size() >= 5)
                break;
            written.releases.push_back(candidate);
        }
    }

    const std::string text = feed::feed_text(written);
    if (text.size() > feed::max_feed_bytes)
        throw Failure("the feed is larger than 256 KiB");
    std::string error;
    if (!feed::read_feed(as_bytes(text), &error))
        throw Failure(error);

    const fs::path out_path = path_from(*out_text);
    write_file(out_path, as_bytes(text));
    const std::vector<uint8_t> written_bytes = read_file(out_path);
    if (!feed::read_feed(written_bytes, &error)) {
        std::error_code ignored;
        fs::remove(out_path, ignored);
        throw Failure(error);
    }
    output.out << "wrote " << utf8_of(out_path) << " release " << release_value << '\n';
    return exit_done;
}

int run_engine_feed_sign(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {
        {"key", true, false},
        {"out", true, false},
        {"passphrase-file", true, false},
    };
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed) {
        report_usage(output, "sign", problem);
        return exit_usage;
    }
    if (parsed->positional.size() != 1) {
        report_usage(
            output, "sign", "expected one feed, got " + std::to_string(parsed->positional.size())
        );
        return exit_usage;
    }
    const std::string* key = required_option(*parsed, output, "sign", "key");
    if (key == nullptr)
        return exit_usage;
    const std::string* out = parsed->value("out");
    if (out != nullptr && out->empty()) {
        report_usage(output, "sign", "option '--out' needs a value");
        return exit_usage;
    }
    bool usage = false;
    const std::optional<fs::path> pass_path = passphrase_path(*parsed, output, "sign", usage);
    if (usage)
        return exit_usage;

    const fs::path feed_path = path_from(parsed->positional[0]);
    const std::vector<uint8_t> feed_bytes = read_capped_feed(feed_path);
    std::string error;
    const std::optional<feed::Feed> loaded = feed::read_feed(feed_bytes, &error);
    if (!loaded)
        throw Failure(error);

    const fs::path key_path = path_from(*key);
    std::error_code key_io;
    const auto key_size = fs::file_size(key_path, key_io);
    if (key_io)
        throw Failure("cannot read the key file");
    if (key_size > signing::sealed_key_most_bytes)
        throw Failure(std::string(signing::seal_status_text(signing::SealStatus::not_sealed_key)));
    const std::vector<uint8_t> key_bytes = read_file(key_path);
    signing::SealedKeyInfo info{};
    const signing::SealStatus read = signing::read_sealed_key_info(key_bytes, info);
    if (read != signing::SealStatus::ok)
        throw Failure(std::string(signing::seal_status_text(read)));

    signing::PublicKey public_key{};
    const std::string key_id(info.id.data(), info.id_size);
    const signing::Signature signature =
        sign_with_sealed_key(key_bytes, pass_path, feed_bytes, public_key);

    fs::path sig_path = out == nullptr ? feed_path : path_from(*out);
    if (out == nullptr)
        sig_path += ".sig";
    const std::string text =
        catalogue::signature_file_text(catalogue::SignatureLine{key_id, signature});
    try {
        write_file(sig_path, as_bytes(text));
        const std::vector<uint8_t> written_feed = read_capped_feed(feed_path);
        const std::vector<uint8_t> written_signature = read_file(sig_path);
        const signing::KeyText key_text = signing::public_key_text(public_key);
        registry::PinnedKey pin;
        pin.registry = "command";
        pin.key_id = key_id;
        pin.public_key = key_text.view();
        feed::CheckRequest request;
        request.feed = written_feed;
        request.signature = written_signature;
        request.channel = loaded->channel;
        request.keys = std::span<const registry::PinnedKey>(&pin, 1);
        const feed::Checked checked = feed::check_feed(request);
        if (!checked.usable()) {
            const char* verdict = feed::verdict_text(checked.verdict);
            throw Failure(checked.detail.empty() ? verdict : checked.detail);
        }
    } catch (const Failure&) {
        std::error_code ignored;
        fs::remove(sig_path, ignored);
        throw;
    }
    output.out << "signed " << utf8_of(sig_path) << " with " << key_id << '\n';
    return exit_done;
}

int run_engine_feed_check(std::span<const std::string> arguments, Output& output) {
    static constexpr OptionSpec options[] = {
        {"sig", true, false},
        {"url", true, false},
        {"channel", true, false},
        {"key", true, true},
    };
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, options, problem);
    if (!parsed) {
        report_usage(output, "check", problem);
        return exit_usage;
    }
    const std::string* url_text = parsed->value("url");
    const std::string* sig_text = parsed->value("sig");
    if (url_text != nullptr && url_text->empty()) {
        report_usage(output, "check", "option '--url' needs a value");
        return exit_usage;
    }
    if (sig_text != nullptr && sig_text->empty()) {
        report_usage(output, "check", "option '--sig' needs a value");
        return exit_usage;
    }
    if (url_text != nullptr && (!parsed->positional.empty() || sig_text != nullptr)) {
        report_usage(output, "check", "--url reads the feed and its signature");
        return exit_usage;
    }
    if (url_text == nullptr && parsed->positional.size() != 1) {
        report_usage(
            output, "check", "expected one feed, got " + std::to_string(parsed->positional.size())
        );
        return exit_usage;
    }
    std::string channel(feed::stable_channel);
    if (const std::string* channel_text = parsed->value("channel")) {
        if (channel_text->empty()) {
            report_usage(output, "check", "option '--channel' needs a value");
            return exit_usage;
        }
        channel = *channel_text;
    }

    std::vector<uint8_t> feed_bytes;
    std::vector<uint8_t> signature_bytes;
    if (url_text != nullptr) {
        http::Client client;
        bool refused = false;
        feed_bytes = fetch_bytes(client, *url_text, output, refused);
        if (refused)
            return exit_failed;
        signature_bytes = fetch_bytes(client, *url_text + ".sig", output, refused);
        if (refused)
            return exit_failed;
    } else {
        const fs::path feed_path = path_from(parsed->positional[0]);
        std::error_code io;
        const auto size = fs::file_size(feed_path, io);
        if (io)
            throw Failure("cannot read the feed");
        if (size > feed::max_feed_bytes) {
            output.out << feed::verdict_text(feed::Verdict::too_large) << '\n';
            return exit_failed;
        }
        feed_bytes = read_file(feed_path);
        const fs::path signature_path =
            sig_text != nullptr ? path_from(*sig_text) : path_from(utf8_of(feed_path) + ".sig");
        std::error_code signature_io;
        const auto signature_size = fs::file_size(signature_path, signature_io);
        if (!signature_io) {
            if (signature_size > catalogue::max_signature_file_bytes)
                signature_bytes.assign(catalogue::max_signature_file_bytes + 1, 'x');
            else
                signature_bytes = read_file(signature_path);
        }
    }

    struct HeldKey {
        std::string id;
        std::string text;
    };

    std::vector<HeldKey> held;
    if (const auto keys = parsed->values.find("key"); keys != parsed->values.end()) {
        for (const std::string& text : keys->second) {
            const std::size_t equals = text.find('=');
            if (equals == std::string::npos || equals == 0)
                throw Failure("a key must be ID=ed25519: and its base64");
            HeldKey held_key;
            held_key.id = text.substr(0, equals);
            held_key.text = text.substr(equals + 1);
            if (!signing::parse_public_key(held_key.text))
                throw Failure("a key must be ID=ed25519: and its base64");
            held.push_back(std::move(held_key));
        }
    }
    std::vector<registry::PinnedKey> pins;
    pins.reserve(held.size());
    for (const HeldKey& held_key : held) {
        registry::PinnedKey pin;
        pin.registry = "command";
        pin.key_id = held_key.id;
        pin.public_key = held_key.text;
        pins.push_back(pin);
    }

    feed::CheckRequest request;
    request.feed = feed_bytes;
    request.signature = signature_bytes;
    request.channel = channel;
    if (!held.empty())
        request.keys = std::span<const registry::PinnedKey>(pins.data(), pins.size());
    const feed::Checked checked = feed::check_feed(request);
    if (!checked.usable())
        return print_refusal(output, checked);
    print_feed(output, checked);
    return exit_done;
}

} // namespace oa::tool
