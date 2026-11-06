// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Slot 32, the fetch queue and the fetch worker's loop. The worker creates
// http.log afresh but writes no trace of the fetches to it.

#include "http_fetch.h"

#include "lobby.h"
#include "lobby_handlers.h"
#include "protocol.h"
#include "slots.h"
#include "state.h"

#include <cctype>
#include <charconv>
#include <cstddef>
#include <new>
#include <system_error>
#include <stdio.h>
#include <string.h>

namespace {

constexpr char kCommandHttpRequestComplete[] = "httprequestcomplete";
constexpr char kKeyUrl[] = "URL";
constexpr char kKeyFile[] = "File";
constexpr char kKeyStatus[] = "Status";

// The only status the worker posts; the game receives it as 0.
constexpr char kStatusDone[] = "A";
constexpr char kStatusBase = 'A';

// URL parsing: the scheme prefix up to its second slash, compared without
// case; a port is at most 19 digits.
constexpr char kSchemeHttp[] = "http:/";
constexpr int32_t kPortDigitLimit = 0x13;
constexpr int32_t kHostLimit = 0x100;

// The request, formatted into at most 0x200 bytes (0x1ff sent when longer).
constexpr char kRequestFormat[] = "GET %s HTTP/1.0\r\nAccept: */*\r\nUser-Agent: Mozilla/3.0 "
                                  "(compatible; getpage 1.0)\r\nHost: %s:%d\r\n\r\n";
constexpr int32_t kRequestLimit = 0x200;
// What a URL that ends after its host asks for.
constexpr char kRootPath[] = "/";

// The response is read 0xfff bytes at a time; a status of 400 or more
// fails the fetch, and Content-Length (matched with case) sizes progress.
constexpr int32_t kReceiveChunk = 0xfff;
constexpr int32_t kFirstFailingStatus = 400;
constexpr char kContentLengthHeader[] = "Content-Length:";
constexpr int32_t kHeaderWordLimit = 0x1ff;
static_assert(kHeaderWordLimit == 511, "a header word holds 511 characters");

/// Copies one word of at most `width` characters, skipping leading whitespace.
///
/// @param text text to read
/// @param[out] out buffer of at least `width + 1` bytes
/// @param width most characters copied
/// @return the text after the word, or null when there is no word
const char* scan_word(const char* text, char* out, std::size_t width) {
    if (text == nullptr)
        return nullptr;
    while (*text != '\0' && std::isspace(static_cast<unsigned char>(*text)))
        ++text;
    if (*text == '\0')
        return nullptr;
    std::size_t count = 0;
    while (*text != '\0' && !std::isspace(static_cast<unsigned char>(*text)) && count < width) {
        out[count++] = *text++;
    }
    out[count] = '\0';
    return text;
}

/// Reads one decimal integer, skipping leading whitespace and an optional sign.
///
/// @param text text to read
/// @param[out] out the integer
/// @return the text after the integer, or null when it is not an integer
const char* scan_int(const char* text, int* out) {
    if (text == nullptr)
        return nullptr;
    while (*text != '\0' && std::isspace(static_cast<unsigned char>(*text)))
        ++text;
    if (*text == '\0')
        return nullptr;
    const char* digits = text;
    if (*text == '+' || *text == '-')
        ++digits;
    if (*digits == '\0' || !std::isdigit(static_cast<unsigned char>(*digits)))
        return nullptr;
    const char* end = digits;
    while (std::isdigit(static_cast<unsigned char>(*end)))
        ++end;
    int value = 0;
    const char* parsed_from = *text == '+' ? digits : text;
    const auto parsed = std::from_chars(parsed_from, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
        return nullptr;
    *out = value;
    return end;
}

constexpr uint32_t kPercentLimit = 100;

// Port of the last URL that named one; a URL without a port uses it. It
// starts at 80 and holds 16 signed bits, and a parsed port is stored before
// it is checked, so ports above 32767 are rejected.
int16_t g_http_port = 80;

// Where a parsed URL points.
struct HttpTarget {
    char host[kHostLimit]{};
    char path[kRoverFetchTextLimit]{};
};

bool equals_ignoring_case(const char* text, const char* literal) {
    for (;; ++text, ++literal) {
        char a = *text >= 'A' && *text <= 'Z' ? static_cast<char>(*text - 'A' + 'a') : *text;
        char b =
            *literal >= 'A' && *literal <= 'Z' ? static_cast<char>(*literal - 'A' + 'a') : *literal;
        if (a != b)
            return false;
        if (a == '\0')
            return true;
    }
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

/// Splits a URL into host, port and path.
///
/// Only http may be named as a scheme; without one the URL starts with the host. A port must be
/// followed by the path's slash; a URL without a port keeps the last one parsed.
///
/// @param url URL text; only its first 259 characters are read
/// @param[out] out_target receives the host and the path
/// @return false for another scheme, a host of 256 characters or more, or a port that is not
///     positive or not followed by a slash
/// @quirk The parsed port is stored in the 16-bit port before it is checked, so ports above 32767
///     are rejected.
bool parse_url(const char* url, HttpTarget* out_target) {
    char copy[kRoverFetchTextLimit];
    rover_copy_bounded(copy, url, sizeof copy);
    const char* source = copy;
    char* slash = strchr(copy, '/');
    if (slash && slash[1] == '/') {
        slash[1] = '\0';
        if (!equals_ignoring_case(copy, kSchemeHttp))
            return false;
        source = slash + 2;
        while (*source == '/')
            ++source;
    }
    int32_t host_length = 0;
    while (*source != '\0' && *source != '/' && *source != ':') {
        if (host_length == kHostLimit - 1)
            return false;
        out_target->host[host_length++] = *source++;
    }
    out_target->host[host_length] = '\0';
    if (*source == ':') {
        ++source;
        char digits[kPortDigitLimit + 1] = {};
        int32_t count = 0;
        while (is_digit(*source) && count < kPortDigitLimit)
            digits[count++] = *source++;
        g_http_port = static_cast<int16_t>(rover_decimal_value(digits));
        if (g_http_port <= 0 || *source != '/')
            return false;
    }
    rover_copy_bounded(out_target->path, source, sizeof out_target->path);
    return true;
}

// What the response reader tracks for progress.
struct Progress {
    RoverHttpProgressFn callback{};
    uint32_t content_length{};
    uint32_t received{};
    uint32_t percent{};
};

void report_progress(Progress& progress, int32_t received) {
    if (!progress.callback || progress.content_length == 0)
        return;
    progress.received += static_cast<uint32_t>(received);
    uint32_t percent = progress.received * 100u / progress.content_length;
    if (static_cast<int32_t>(percent) > static_cast<int32_t>(kPercentLimit))
        percent = kPercentLimit;
    if (percent != progress.percent) {
        progress.percent = percent;
        progress.callback(static_cast<int32_t>(percent));
    }
}

// Scans the first chunk's headers from text for Content-Length (while
// progress wants it) and returns where the body starts: after the line that
// follows a "\n\r", or the chunk's end when there is none.
char* find_body(char* text, char* end, Progress& progress) {
    bool sized = progress.callback == nullptr;
    for (int32_t left = static_cast<int32_t>(end - text); left != 0; --left, ++text) {
        if (text[0] == '\n' && text[1] == '\r') {
            do
                ++text;
            while (*text != '\0' && *text != '\n');
            return text < end ? text + 1 : end;
        }
        if (sized)
            continue;
        char word[kHeaderWordLimit + 1];
        int value = 0;
        const char* after_word = scan_word(text, word, static_cast<std::size_t>(kHeaderWordLimit));
        if (after_word != nullptr && scan_int(after_word, &value) != nullptr &&
            strcmp(kContentLengthHeader, word) == 0) {
            sized = true;
            progress.content_length = static_cast<uint32_t>(value);
        }
    }
    return end;
}

/// Reads the response to the end and saves the body.
///
/// The first chunk's status must be below 400, and its headers' Content-Length, when progress is
/// wanted, sizes the progress reports. Without a file 3.1c writes the body to standard output; here
/// it is dropped.
///
/// @param socket connected socket carrying the response
/// @param destination file to create for the body, or "" for none
/// @param callback progress callback taking whole percentages, or null
/// @return true when the peer closed the stream after a good status, false on a receive error or a
///     failing status
bool receive_response(RoverSocket socket, const char* destination, RoverHttpProgressFn callback) {
    const RoverPlatform* platform = rover_platform();
    RoverFile* file = destination[0] != '\0' ? platform->file_create(destination) : nullptr;
    Progress progress;
    progress.callback = callback;
    char buffer[kReceiveChunk + 1];
    bool succeeded = false;
    for (int32_t chunk = 1;; ++chunk) {
        int32_t received = platform->tcp_recv(socket, buffer, kReceiveChunk);
        report_progress(progress, received);
        if (received <= 0) {
            succeeded = received == 0;
            break;
        }
        buffer[received] = '\0';
        char* end = buffer + received;
        char* body = buffer;
        if (chunk == 1) {
            char* status = buffer;
            while (*status != '\0' && *status != ' ')
                ++status;
            while (*status == ' ')
                ++status;
            if (rover_decimal_value(status) >= kFirstFailingStatus)
                break;
            body = find_body(status, end, progress);
        }
        if (file && end > body)
            platform->file_write(file, body, static_cast<int32_t>(end - body));
    }
    if (file)
        platform->file_close(file);
    return succeeded;
}

/// Connects to the target, sends the GET (for "/" when the URL names no path) and saves the
/// response.
///
/// @param target parsed host and path; the port is the last one a URL named
/// @param destination file to create for the body, or "" for none
/// @param progress progress callback taking whole percentages, or null
/// @return true when the response was received in full, false when any step fails
bool fetch(const HttpTarget& target, const char* destination, RoverHttpProgressFn progress) {
    const RoverPlatform* platform = rover_platform();
    if (!platform->net_startup())
        return false;
    bool fetched = false;
    uint16_t port = static_cast<uint16_t>(g_http_port);
    RoverSocket socket = platform->tcp_connect(target.host, port);
    if (socket != OA_ROVER_SOCKET_INVALID) {
        const char* path = target.path[0] != '\0' ? target.path : kRootPath;
        char request[kRequestLimit];
        int length =
            snprintf(request, sizeof request, kRequestFormat, path, target.host, g_http_port);
        if (length < 0 || length >= kRequestLimit)
            length = kRequestLimit - 1;
        if (platform->tcp_send(socket, request, length) == length)
            fetched = receive_response(socket, destination, progress);
        platform->tcp_close(socket);
    }
    platform->net_cleanup();
    return fetched;
}

/// Posts the completion of a fetch for slot 5: its URL, file and status 'A', whether or not the
/// fetch succeeded.
///
/// @param[in,out] state session block whose inbound queue receives the record
/// @param job the finished fetch
void post_completion(RoverState& state, const RoverFetchJob& job) {
    RoverRecord* record = rover_record_create();
    if (!record)
        return;
    RoverEntry* entry = rover_record_add_entry(*record, false, kRoverClientService);
    if (!entry) {
        rover_record_destroy(&record);
        return;
    }
    rover_entry_set_string(*entry, kRoverKeyCommand, kCommandHttpRequestComplete);
    rover_entry_set_string(*entry, kKeyUrl, job.url);
    rover_entry_set_string(*entry, kKeyFile, job.destination);
    rover_entry_set_string(*entry, kKeyStatus, kStatusDone);
    rover_queue_push(state.inbound, record);
}

/// Appends a fetch to the queue; the first one waiting sets the queue's event.
///
/// @param[in,out] queue fetch queue
/// @param job heap-allocated fetch; the queue takes ownership
void push_job(RoverFetchQueue& queue, RoverFetchJob* job) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(queue.lock);
    job->next = nullptr;
    if (queue.last)
        queue.last->next = job;
    else
        queue.first = job;
    queue.last = job;
    if (++queue.count == 1)
        platform->event_set(queue.ready);
    platform->lock_leave(queue.lock);
}

/// Takes the oldest fetch out of the queue; taking the last resets the event.
///
/// @param[in,out] queue fetch queue
/// @param[out] out_job receives the fetch, or an empty one when none waits
void pop_job(RoverFetchQueue& queue, RoverFetchJob* out_job) {
    const RoverPlatform* platform = rover_platform();
    *out_job = RoverFetchJob{};
    platform->lock_enter(queue.lock);
    if (RoverFetchJob* job = queue.first) {
        queue.first = job->next;
        if (!queue.first)
            queue.last = nullptr;
        *out_job = *job;
        out_job->next = nullptr;
        delete job;
        if (--queue.count == 0)
            platform->event_reset(queue.ready);
    }
    platform->lock_leave(queue.lock);
}

} // namespace

bool rover_fetch_queue_init(RoverFetchQueue& queue) {
    queue.lock = rover_platform()->lock_create();
    return queue.lock != nullptr;
}

void rover_fetch_queue_teardown(RoverFetchQueue& queue) {
    const RoverPlatform* platform = rover_platform();
    while (RoverFetchJob* job = queue.first) {
        queue.first = job->next;
        delete job;
    }
    queue.last = nullptr;
    queue.count = 0;
    if (queue.lock)
        platform->lock_destroy(queue.lock);
    if (queue.ready)
        platform->event_destroy(queue.ready);
    queue.lock = nullptr;
    queue.ready = nullptr;
}

void rover_serve_fetches(RoverState& state) {
    const RoverPlatform* platform = rover_platform();
    RoverEvent* const events[] = {state.fetches.ready, state.fetch_stop_event};
    constexpr int32_t kQueueSignalled = 0;
    constexpr int32_t kStopSignalled = 1;
    for (;;) {
        int32_t signalled = platform->event_wait_any(events, 2, OA_ROVER_WAIT_FOREVER);
        if (signalled == kStopSignalled) {
            --state.worker_count;
            return;
        }
        if (signalled != kQueueSignalled)
            return;
        RoverFetchJob job;
        pop_job(state.fetches, &job);
        HttpTarget target;
        if (!parse_url(job.url, &target))
            continue;
        fetch(target, job.destination, job.progress);
        post_completion(state, job);
    }
}

bool rover_accept_http_complete(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!rover_command_is(command, kCommandHttpRequestComplete))
        return false;
    const RoverField* url = rover_entry_find(entry, kKeyUrl);
    const RoverField* file = rover_entry_find(entry, kKeyFile);
    const RoverField* status = rover_entry_find(entry, kKeyStatus);
    RoverHttpCompleteFn complete = handlers.lobby.http_complete;
    if (url && file && status && complete) {
        complete(
            rover_field_text(*url),
            rover_field_text(*file),
            rover_field_text(*status)[0] - kStatusBase
        );
    }
    return true;
}

bool ROVER_CALL rover_fetch_http_file(
    const char* url,
    const char* dest_path,
    RoverHttpCompleteFn complete,
    RoverHttpProgressFn progress
) {
    if (!g_rover_state)
        return false;
    RoverState& state = *g_rover_state;
    RoverHandlerSet& handlers = state.handlers;
    if (rover_handler_is_armed(handlers, RoverHandlerId::HttpRequestComplete))
        rover_handler_disarm(handlers, RoverHandlerId::HttpRequestComplete);
    handlers.lobby.http_complete = complete;
    rover_handler_arm(handlers, RoverHandlerId::HttpRequestComplete);
    RoverFetchJob* job = new (std::nothrow) RoverFetchJob{};
    if (!job)
        return true;
    rover_copy_bounded(job->url, url ? url : kRoverNoText, sizeof job->url);
    rover_copy_bounded(
        job->destination, dest_path ? dest_path : kRoverNoText, sizeof job->destination
    );
    job->progress = progress;
    push_job(state.fetches, job);
    return true;
}
