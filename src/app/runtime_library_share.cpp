// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Sign-in to Core Prime, and sharing one replay. The session token stays in
// this shelf for the run. It is never written, logged or put in an address.
// The password lives only until the login body is built.

#include "library_host.hpp"

#include "oa/app/content/service.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/base/threads.hpp"
#include "oa/data/replays/feed.hpp"
#include "oa/data/replays/interface_words.hpp"
#include "oa/data/replays/share.hpp"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/tad.hpp"
#include "oa/netgame/http/client.hpp"
#include "oa/netgame/http/form.hpp"
#include "oa/platform/files.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/replays.hpp"
#include "oa/ui/library/text.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace fs = std::filesystem;
namespace http = oa::netgame::http;
namespace library = oa::ui::library;
namespace oademo = oa::formats::oademo;
namespace platform = oa::platform;
namespace replay_data = oa::data::replays;
namespace tad = oa::formats::tad;

/// The worker's free functions cannot name Runtime's private members.
struct CoreShareGate {
    static const std::string& configured_api(const Runtime& runtime) {
        return runtime.options_.core_prime_api;
    }

    static const fs::path& user_folder(const Runtime& runtime) { return runtime.user_folder_; }
};

namespace {

constexpr uint64_t kShareAnswerBytes = 256ULL * 1024ULL;

std::string strip_slash(std::string text) {
    while (!text.empty() && text.back() == '/')
        text.pop_back();
    return text;
}

std::string feed_origin_of(const Runtime& runtime) {
    const std::string& configured = CoreShareGate::configured_api(runtime);
    if (configured.empty())
        return std::string(replay_data::core_prime_feed_origin);
    return strip_slash(configured);
}

bool same_name(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        unsigned char a = static_cast<unsigned char>(left[index]);
        unsigned char b = static_cast<unsigned char>(right[index]);
        if (a >= 'A' && a <= 'Z')
            a = static_cast<unsigned char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = static_cast<unsigned char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

std::optional<fs::path> recording_file(const fs::path& recordings, std::string_view path) {
    fs::path file = recordings;
    std::size_t at = 0;
    while (at <= path.size()) {
        const std::size_t slash = path.find('/', at);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        const std::string_view part = path.substr(at, end - at);
        if (part.empty() || part == "." || part == ".." ||
            part.find('\\') != std::string_view::npos || part.find(':') != std::string_view::npos)
            return std::nullopt;
        file /= platform::path_from_utf8(std::string(part));
        if (slash == std::string_view::npos)
            break;
        at = slash + 1;
    }
    return file;
}

std::optional<int64_t> retry_seconds(const http::Response& response) {
    const std::string* header = response.header("Retry-After");
    if (header == nullptr || header->empty())
        return std::nullopt;
    int64_t value = 0;
    for (const char byte : *header) {
        if (byte < '0' || byte > '9')
            return std::nullopt;
        value = value * 10 + (byte - '0');
    }
    return value;
}

std::string session_cookie(const http::Response& response) {
    std::vector<std::string_view> values;
    for (const http::Header& header : response.headers) {
        if (same_name(header.name, "Set-Cookie"))
            values.push_back(header.value);
    }
    return replay_data::cp_session_token(values);
}

void wipe_bytes(std::vector<uint8_t>& bytes) {
    std::fill(bytes.begin(), bytes.end(), uint8_t{0});
    bytes.clear();
}

std::string looked_up(const library::Library& model, std::string_view sentence) {
    using replay_data::account_cannot_share;
    using replay_data::already_on_core_prime;
    using replay_data::file_unreadable;
    using replay_data::only_a_player_can_replace;
    using replay_data::replay_larger_than_the_limit;
    using replay_data::shared_for_today;
    using replay_data::shared_on_core_prime;
    using replay_data::sign_in_to_share;
    using replay_data::session_not_started;
    using replay_data::share_not_answered;
    using replay_data::uploads_not_available;
    if (sentence == shared_on_core_prime)
        return std::string(library::shown(model.text, library::words::share_shared));
    if (sentence == already_on_core_prime)
        return std::string(library::shown(model.text, library::words::share_already));
    if (sentence == sign_in_to_share)
        return std::string(library::shown(model.text, library::words::share_sign_in_again));
    if (sentence == account_cannot_share)
        return std::string(library::shown(model.text, library::words::share_account));
    if (sentence == only_a_player_can_replace)
        return std::string(library::shown(model.text, library::words::share_not_in_battle));
    if (sentence == replay_larger_than_the_limit)
        return std::string(library::shown(model.text, library::words::share_too_large));
    if (sentence == shared_for_today)
        return std::string(library::shown(model.text, library::words::share_today));
    if (sentence == uploads_not_available)
        return std::string(library::shown(model.text, library::words::share_uploads_off));
    if (sentence == session_not_started)
        return std::string(library::shown(model.text, library::words::sign_in_no_session));
    if (sentence == share_not_answered)
        return std::string(library::shown(model.text, library::words::share_no_answer));
    if (sentence == library::words::core_offline)
        return std::string(library::shown(model.text, library::words::core_offline));
    if (sentence == file_unreadable)
        return std::string(library::shown(model.text, file_unreadable));
    if (sentence == library::words::sign_in_needs_secure)
        return std::string(library::shown(model.text, library::words::sign_in_needs_secure));
    return std::string(sentence);
}

std::string share_sentence(
    const library::Library& model, std::string_view sentence, std::optional<int64_t> retry_after
) {
    if (sentence == replay_data::shared_for_today && retry_after.has_value()) {
        const std::string base = looked_up(model, sentence);
        const std::string again = library::filled(
            model.text, library::words::core_try_again, {{"seconds", std::to_string(*retry_after)}}
        );
        return base + " " + again;
    }
    return looked_up(model, sentence);
}

bool packets_match(std::span<const uint8_t> source, std::span<const uint8_t> exported_archive) {
    const oademo::ReadResult exported = oademo::read(exported_archive);
    if (!exported.ok())
        return false;
    const tad::ParseResult from_export = tad::parse(exported.replay->recording);
    const tad::ParseResult from_source = tad::parse(source);
    if (!from_export.ok() || !from_source.ok())
        return false;
    if (tad::find_sector(*from_export.demo, tad::SectorType::player_address) != nullptr)
        return false;
    return from_export.demo->packets.size() == from_source.demo->packets.size();
}

} // namespace

/// One sign-in or one share, off the main thread.
struct CoreShareShelf {
    enum class AskKind : uint8_t { login, upload };

    struct Ask {
        AskKind kind{AskKind::login};
        std::string handle{};
        std::string password{};
        std::string path{};
        std::string title{};
        bool listed{true};
        std::string origin{};
        fs::path recordings{};
        bool https_downgraded{};
    };

    struct Done {
        AskKind kind{AskKind::login};
        bool signed_in{};
        bool forget{};
        bool ask_sign_in{};
        /// The player stopped this one. Its sentence is not shown.
        bool cancelled{};
        std::string sentence{};
        std::string id{};
        std::string handle{};
        std::string token{};
        std::string path{};
        std::optional<int64_t> retry_after{};
    };

    base::threads::Mutex mutex{};
    base::threads::ConditionVariable wake{};
    base::threads::Thread thread{};
    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> byte_limit{replay_data::share_byte_limit};
    bool stop{};
    bool started{};
    /// 1 while the worker holds an ask. Guarded by mutex, with waiting.
    int in_flight{};
    std::vector<Ask> waiting{};
    std::vector<Done> done{};
    std::string token{};
    std::string handle{};
    std::string note{};
    std::string note_path{};
    std::string note_id{};
};

/// Wraps a raw `.tad` the way Export does: the engine line, the mod and the content note.
///
/// @param bytes the `.tad`
/// @param file the recording, so the note beside it can be read
/// @param user_folder the player's folder, parent of Recordings
/// @return the `.oademo`, or the sentence when it cannot be written
oademo::WriteResult
wrap_shared_tad(std::span<const uint8_t> bytes, const fs::path& file, const fs::path& user_folder);

namespace {

void wipe_ask(CoreShareShelf::Ask& ask) {
    replay_data::wipe_secret(ask.password);
}

CoreShareShelf::Done
login_done(http::Client& client, CoreShareShelf::Ask& ask, const std::atomic<bool>* cancel) {
    CoreShareShelf::Done done;
    done.kind = CoreShareShelf::AskKind::login;
    done.handle = ask.handle;
    const std::string address = replay_data::login_url(ask.origin);
    const replay_data::ShareTransport transport =
        replay_data::share_transport(address, ask.https_downgraded);
    if (transport != replay_data::ShareTransport::allowed) {
        done.sentence = std::string(library::words::sign_in_needs_secure);
        wipe_ask(ask);
        return done;
    }
    std::string json = replay_data::login_request_body(ask.handle, ask.password);
    wipe_ask(ask);
    http::Request request;
    request.method = http::Method::post;
    request.url = address;
    request.content_type = "application/json";
    request.body.assign(json.begin(), json.end());
    replay_data::wipe_secret(json);
    request.limits.max_body_bytes = kShareAnswerBytes;
    // A redirect would carry the password on, perhaps to plain HTTP: Core
    // Prime's sign-in never redirects, so none is followed.
    request.limits.max_redirects = 0;
    request.timeouts.total_ms = 120000;
    request.cancel = cancel;
    std::vector<uint8_t> body;
    const http::Response response = client.fetch(request, body);
    wipe_bytes(request.body);
    if (response.failure != http::Failure::none || response.status == 0) {
        done.sentence = std::string(library::words::core_offline);
        return done;
    }
    const std::string text(reinterpret_cast<const char*>(body.data()), body.size());
    const replay_data::LoginAnswer answer = replay_data::parse_login_answer(text);
    if (!answer.readable) {
        done.sentence = std::string(library::words::core_offline);
        return done;
    }
    if (!answer.ok) {
        done.sentence = answer.error;
        return done;
    }
    done.token = session_cookie(response);
    if (done.token.empty()) {
        done.sentence = std::string(replay_data::session_not_started);
        return done;
    }
    done.signed_in = true;
    if (!answer.handle.empty())
        done.handle = answer.handle;
    return done;
}

CoreShareShelf::Done refuse_upload(CoreShareShelf::Ask& ask, std::string sentence, bool before) {
    CoreShareShelf::Done done;
    done.kind = CoreShareShelf::AskKind::upload;
    done.path = ask.path;
    done.sentence = std::move(sentence);
    if (before)
        std::cout << "core prime: refused before sending\n";
    return done;
}

CoreShareShelf::Done upload_done(
    http::Client& client,
    CoreShareShelf::Ask& ask,
    std::string token,
    uint64_t limit,
    const std::atomic<bool>* cancel
) {
    CoreShareShelf::Done done;
    done.kind = CoreShareShelf::AskKind::upload;
    done.path = ask.path;
    if (token.empty()) {
        done.forget = true;
        done.ask_sign_in = true;
        done.sentence = std::string(replay_data::sign_in_to_share);
        return done;
    }
    const std::string address = replay_data::upload_url(ask.origin);
    const replay_data::ShareTransport transport =
        replay_data::share_transport(address, ask.https_downgraded);
    if (transport != replay_data::ShareTransport::allowed) {
        done.sentence = std::string(library::words::sign_in_needs_secure);
        replay_data::wipe_secret(token);
        return done;
    }
    const std::optional<fs::path> file = recording_file(ask.recordings, ask.path);
    std::error_code error;
    const uint64_t size = file ? fs::file_size(*file, error) : 0;
    if (!file || error) {
        replay_data::wipe_secret(token);
        return refuse_upload(ask, std::string(replay_data::file_unreadable), false);
    }
    if (size > limit) {
        replay_data::wipe_secret(token);
        return refuse_upload(ask, std::string(replay_data::replay_larger_than_the_limit), true);
    }
    std::ifstream input(*file, std::ios::binary);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    if (!input ||
        !input.read(
            reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        ) ||
        input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        replay_data::wipe_secret(token);
        return refuse_upload(ask, std::string(replay_data::file_unreadable), false);
    }
    std::vector<uint8_t> source_recording;
    oademo::WriteResult exported;
    if (oademo::recognised(bytes)) {
        const oademo::ReadResult read = oademo::read(bytes);
        if (!read.ok()) {
            replay_data::wipe_secret(token);
            return refuse_upload(ask, std::string(replay_data::file_unreadable), false);
        }
        source_recording = read.replay->recording;
        exported = oademo::export_replay(*read.replay);
    } else {
        source_recording = bytes;
        exported = wrap_shared_tad(bytes, *file, ask.recordings.parent_path());
    }
    if (!exported.ok()) {
        replay_data::wipe_secret(token);
        return refuse_upload(
            ask,
            exported.error.empty() ? std::string(replay_data::file_unreadable) : exported.error,
            false
        );
    }
    if (exported.bytes.size() > limit) {
        replay_data::wipe_secret(token);
        return refuse_upload(ask, std::string(replay_data::replay_larger_than_the_limit), true);
    }
    if (!packets_match(source_recording, exported.bytes)) {
        replay_data::wipe_secret(token);
        return refuse_upload(ask, std::string(replay_data::file_unreadable), false);
    }
    std::vector<http::FormField> fields;
    http::FormField replay;
    replay.name = "replay";
    // A fixed name: the player's own file name stays on this computer.
    replay.filename = "replay.oademo";
    replay.file = true;
    replay.content_type = "application/octet-stream";
    replay.bytes = std::move(exported.bytes);
    fields.push_back(std::move(replay));
    if (!ask.title.empty() && replay_data::share_title_ok(ask.title)) {
        http::FormField title;
        title.name = "title";
        title.text = ask.title;
        fields.push_back(std::move(title));
    }
    http::FormField visibility;
    visibility.name = "visibility";
    visibility.text =
        std::string(ask.listed ? replay_data::visibility_public : replay_data::visibility_unlisted);
    fields.push_back(std::move(visibility));
    const http::FormBody form = http::encode_form(fields);
    http::Request request;
    request.method = http::Method::post;
    request.url = address;
    request.content_type = form.content_type;
    request.body = form.bytes;
    http::Header authorization;
    authorization.name = "Authorization";
    authorization.value = "Bearer " + token;
    replay_data::wipe_secret(token);
    request.headers.push_back(std::move(authorization));
    request.limits.max_body_bytes = kShareAnswerBytes;
    // The token is never carried on by a redirect. The file may take minutes
    // on a slow line, which Core Prime allows: only a stall ends it, and
    // leaving the game stops it.
    request.limits.max_redirects = 0;
    request.timeouts.total_ms = 0;
    request.cancel = cancel;
    std::vector<uint8_t> body;
    const http::Response response = client.fetch(request, body);
    for (http::Header& header : request.headers)
        replay_data::wipe_secret(header.value);
    wipe_bytes(request.body);
    const std::string text(reinterpret_cast<const char*>(body.data()), body.size());
    const replay_data::ShareAnswer answer =
        replay_data::interpret_share(response.status, text, retry_seconds(response));
    done.sentence = answer.sentence;
    done.id = answer.id;
    done.forget = answer.forget_token;
    done.ask_sign_in = answer.ask_sign_in;
    done.retry_after = answer.retry_after;
    if (response.failure != http::Failure::none && response.status == 0)
        done.sentence = std::string(library::words::core_offline);
    return done;
}

void share_worker(void* argument) {
    auto& shelf = *static_cast<CoreShareShelf*>(argument);
    http::Client client;
    for (;;) {
        CoreShareShelf::Ask ask;
        std::string token;
        uint64_t limit = replay_data::share_byte_limit;
        {
            base::threads::LockGuard held(shelf.mutex);
            while (!shelf.stop && shelf.waiting.empty())
                shelf.wake.wait(shelf.mutex);
            if (shelf.stop)
                return;
            ask = std::move(shelf.waiting.front());
            shelf.waiting.erase(shelf.waiting.begin());
            token = shelf.token;
            limit = shelf.byte_limit.load();
            shelf.in_flight = 1;
        }
        CoreShareShelf::Done done;
        done.kind = ask.kind;
        done.path = ask.path;
        if (shelf.cancel.load()) {
            wipe_ask(ask);
            replay_data::wipe_secret(token);
            done.cancelled = true;
        } else {
            if (ask.kind == CoreShareShelf::AskKind::login)
                done = login_done(client, ask, &shelf.cancel);
            else
                done = upload_done(client, ask, std::move(token), limit, &shelf.cancel);
            wipe_ask(ask);
        }
        {
            base::threads::LockGuard held(shelf.mutex);
            if (shelf.cancel.load()) {
                replay_data::wipe_secret(done.token);
                done.token.clear();
                done.sentence.clear();
                done.signed_in = false;
                done.cancelled = true;
            }
            shelf.in_flight = 0;
            shelf.cancel.store(false);
            if (shelf.stop) {
                replay_data::wipe_secret(done.token);
                return;
            }
            shelf.done.push_back(std::move(done));
        }
    }
}

bool ensure_share_worker(CoreShareShelf& shelf) {
    base::threads::LockGuard held(shelf.mutex);
    if (shelf.started)
        return true;
    if (!base::threads::start_thread(shelf.thread, share_worker, &shelf))
        return false;
    shelf.started = true;
    return true;
}

/// Stops a sign-in or a share. Waiting asks are dropped. One already in
/// flight is marked cancelled when it finishes, and its sentence is not shown.
///
/// @param shelf the shelf
/// @return true when an in-flight ask will still report
bool cancel_share_work(CoreShareShelf& shelf) {
    base::threads::LockGuard held(shelf.mutex);
    shelf.cancel.store(true);
    for (CoreShareShelf::Ask& ask : shelf.waiting)
        wipe_ask(ask);
    shelf.waiting.clear();
    const bool flying = shelf.in_flight != 0;
    if (!flying)
        shelf.cancel.store(false);
    return flying;
}

bool post_share(CoreShareShelf& shelf, CoreShareShelf::Ask ask) {
    {
        base::threads::LockGuard held(shelf.mutex);
        if (shelf.stop || shelf.cancel.load()) {
            wipe_ask(ask);
            return false;
        }
        shelf.waiting.push_back(std::move(ask));
    }
    shelf.wake.notify_all();
    return true;
}

CoreShareShelf::Ask base_share_ask(Runtime& runtime) {
    CoreShareShelf::Ask ask;
    ask.origin = feed_origin_of(runtime);
    ask.recordings = CoreShareGate::user_folder(runtime) / std::string(recordings_folder_name);
    ask.https_downgraded = oa::app::content::https_read_over_http();
    return ask;
}

} // namespace

void destroy_core_share_shelf(CoreShareShelf* shelf) noexcept {
    if (shelf == nullptr)
        return;
    bool join = false;
    {
        base::threads::LockGuard held(shelf->mutex);
        shelf->stop = true;
        join = shelf->started;
        for (CoreShareShelf::Ask& ask : shelf->waiting)
            wipe_ask(ask);
        shelf->waiting.clear();
    }
    shelf->cancel.store(true);
    shelf->wake.notify_all();
    if (join)
        base::threads::join_thread(shelf->thread);
    replay_data::wipe_secret(shelf->token);
    for (CoreShareShelf::Done& done : shelf->done)
        replay_data::wipe_secret(done.token);
    delete shelf;
}

CoreShareShelf& Runtime::LibraryHost::core_share_shelf() {
    if (!core_share)
        core_share.reset(new CoreShareShelf());
    return *core_share;
}

std::string Runtime::LibraryHost::signed_handle() const {
    if (!core_share)
        return {};
    base::threads::LockGuard held(core_share->mutex);
    if (core_share->token.empty())
        return {};
    return core_share->handle;
}

void Runtime::LibraryHost::set_share_byte_limit(uint64_t bytes) {
    CoreShareShelf& shelf = core_share_shelf();
    shelf.byte_limit.store(bytes);
}

void Runtime::LibraryHost::publish_account(library::ReplayInputs& inputs) {
    inputs.core_sign_in_secure = !oa::app::content::https_read_over_http();
    inputs.core_sign_in_offered = inputs.core_sign_in_secure;
    if (!core_share) {
        inputs.core_signed_in = false;
        inputs.core_handle.clear();
        inputs.core_share_note.clear();
        inputs.core_share_path.clear();
        inputs.core_share_id.clear();
        return;
    }
    base::threads::LockGuard held(core_share->mutex);
    inputs.core_signed_in = !core_share->token.empty();
    inputs.core_handle = core_share->handle;
    inputs.core_share_note = core_share->note;
    inputs.core_share_path = core_share->note_path;
    inputs.core_share_id = core_share->note_id;
}

void Runtime::LibraryHost::pump_core_share(Runtime&) {
    const bool secure = !oa::app::content::https_read_over_http();
    bool changed = false;
    if (model.sign_in.secure != secure) {
        model.sign_in.secure = secure;
        changed = true;
    }
    if (!secure && model.sign_in.open) {
        share_after_sign_in.reset();
        library::close_sign_in(model);
        changed = true;
    }
    // The player closed sign-in without signing in. A success still has the
    // question open when this pump starts, and opens the share below.
    if (!model.sign_in.open)
        share_after_sign_in.reset();
    if (!core_share) {
        if (changed)
            ++model_revision;
        return;
    }
    CoreShareShelf& shelf = *core_share;
    std::vector<CoreShareShelf::Done> ready;
    {
        base::threads::LockGuard held(shelf.mutex);
        ready.swap(shelf.done);
    }
    for (CoreShareShelf::Done& done : ready) {
        changed = true;
        if (done.cancelled) {
            if (done.kind == CoreShareShelf::AskKind::login)
                model.sign_in.busy = false;
            else
                model.share_question.busy = false;
            replay_data::wipe_secret(done.token);
            continue;
        }
        if (done.kind == CoreShareShelf::AskKind::login) {
            model.sign_in.busy = false;
            if (done.signed_in && !done.token.empty()) {
                {
                    base::threads::LockGuard held(shelf.mutex);
                    replay_data::wipe_secret(shelf.token);
                    shelf.token = std::move(done.token);
                    shelf.handle = std::move(done.handle);
                }
                const std::optional<std::string> pending = std::move(share_after_sign_in);
                share_after_sign_in.reset();
                library::close_sign_in(model);
                if (pending.has_value()) {
                    const library::ReplayRow* row = library::find_replay(model, *pending);
                    const std::string title = row == nullptr ? std::string{} : row->replay.map;
                    library::open_share_question(model, *pending, title);
                }
            } else {
                replay_data::wipe_secret(done.token);
                if (!done.sentence.empty())
                    model.sign_in.note = share_sentence(model, done.sentence, std::nullopt);
            }
            continue;
        }
        model.share_question.busy = false;
        if (done.forget) {
            base::threads::LockGuard held(shelf.mutex);
            replay_data::wipe_secret(shelf.token);
            shelf.handle.clear();
            if (model.replays.core_filter == library::CorePrimeFilter::mine)
                library::set_core_filter(model, library::CorePrimeFilter::latest);
        }
        replay_data::wipe_secret(done.token);
        const std::string sentence = share_sentence(model, done.sentence, done.retry_after);
        if (done.ask_sign_in) {
            library::close_share_question(model);
            library::open_sign_in(model);
            model.sign_in.note = sentence;
            continue;
        }
        {
            base::threads::LockGuard held(shelf.mutex);
            shelf.note = sentence;
            shelf.note_path = done.path;
            if (!done.id.empty())
                shelf.note_id = done.id;
        }
        if (!done.id.empty())
            library::close_share_question(model);
        else if (model.share_question.open)
            model.share_question.note = sentence;
    }
    if (changed) {
        publish_account(model.inputs.replays);
        library::update_replays(model);
        ++model_revision;
    }
}

bool Runtime::LibraryHost::take_share_action(
    Runtime& runtime, const library::ScreenResult& result
) {
    if (!result.action)
        return false;
    switch (*result.action) {
    case library::Action::sign_in:
        share_after_sign_in.reset();
        library::open_sign_in(model);
        ++model_revision;
        return false;
    case library::Action::cancel_sign_in:
        if (!core_share || !cancel_share_work(*core_share))
            model.sign_in.busy = false;
        ++model_revision;
        return false;
    case library::Action::cancel_share:
        if (!core_share || !cancel_share_work(*core_share))
            model.share_question.busy = false;
        ++model_revision;
        return false;
    case library::Action::sign_out: {
        share_after_sign_in.reset();
        if (core_share) {
            base::threads::LockGuard held(core_share->mutex);
            replay_data::wipe_secret(core_share->token);
            core_share->handle.clear();
        }
        if (model.replays.core_filter == library::CorePrimeFilter::mine)
            library::set_core_filter(model, library::CorePrimeFilter::latest);
        library::close_sign_in(model);
        library::close_share_question(model);
        publish_account(model.inputs.replays);
        library::update_replays(model);
        ++model_revision;
        return false;
    }
    case library::Action::submit_sign_in: {
        if (!library::sign_in_can_submit(model.sign_in))
            return false;
        CoreShareShelf& shelf = core_share_shelf();
        if (!ensure_share_worker(shelf))
            return false;
        CoreShareShelf::Ask ask = base_share_ask(runtime);
        ask.kind = CoreShareShelf::AskKind::login;
        ask.handle = model.sign_in.handle;
        ask.password = std::move(model.sign_in.password);
        model.sign_in.password.clear();
        model.sign_in.password_caret = 0;
        model.sign_in.busy = true;
        model.sign_in.note.clear();
        if (!post_share(shelf, std::move(ask))) {
            model.sign_in.busy = false;
            return false;
        }
        ++model_revision;
        return false;
    }
    case library::Action::share_replay: {
        if (signed_handle().empty()) {
            library::open_sign_in(model);
            if (model.sign_in.open)
                share_after_sign_in = result.replay;
            else
                share_after_sign_in.reset();
            ++model_revision;
            return false;
        }
        share_after_sign_in.reset();
        const library::ReplayRow* row = library::find_replay(model, result.replay);
        const std::string title = row == nullptr ? std::string{} : row->replay.map;
        library::open_share_question(model, result.replay, title);
        ++model_revision;
        return false;
    }
    case library::Action::submit_share: {
        if (!library::share_can_submit(model.share_question))
            return false;
        CoreShareShelf& shelf = core_share_shelf();
        if (!ensure_share_worker(shelf))
            return false;
        CoreShareShelf::Ask ask = base_share_ask(runtime);
        ask.kind = CoreShareShelf::AskKind::upload;
        ask.path = model.share_question.path;
        ask.title = model.share_question.title;
        ask.listed = model.share_question.listed;
        model.share_question.busy = true;
        model.share_question.note.clear();
        if (!post_share(shelf, std::move(ask))) {
            model.share_question.busy = false;
            return false;
        }
        ++model_revision;
        return false;
    }
    case library::Action::open_shared_replay: {
        std::string id;
        if (model.inputs.replays.core_listing_path == result.replay &&
            model.inputs.replays.core_listing == library::CoreListing::listed)
            id = model.inputs.replays.core_listing_id;
        else if (model.inputs.replays.core_share_path == result.replay)
            id = model.inputs.replays.core_share_id;
        if (!id.empty())
            open_core_replay(runtime, id);
        return false;
    }
    default:
        return false;
    }
}

} // namespace oa::app
