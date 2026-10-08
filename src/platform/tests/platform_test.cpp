// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/base/threads.hpp"
#include "oa/platform/allocator.hpp"
#include "oa/platform/app_loop.hpp"
#include "oa/platform/crash.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/lock.hpp"
#include "oa/platform/memory_status.hpp"
#include "oa/platform/perf.hpp"
#include "oa/platform/system.hpp"
#include "oa/test/scratch_directory.hpp"

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <new>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void test_allocator() {
    using namespace oa::platform;
    const HeapStats before = heap_stats();
    auto* block = static_cast<uint8_t*>(heap_alloc(24, "test block"));
    check(block != nullptr, "allocation succeeds");
    if (block == nullptr)
        return;
    check(heap_block_size(block) == 24, "block size recorded");
    check(std::strcmp(heap_block_tag(block), "test block") == 0, "block tag recorded");
    for (int i = 0; i < 24; ++i) {
        block[i] = static_cast<uint8_t>(i);
    }
    auto* grown = static_cast<uint8_t*>(heap_resize(block, 64));
    check(grown != nullptr && heap_block_size(grown) == 64, "resize grows");
    if (grown == nullptr) {
        heap_free(block);
        return;
    }
    check(grown[23] == 23, "resize keeps prefix");
    check(heap_alloc(max_block_size + 1) == nullptr, "oversized request refused");
    check(heap_alloc_zeroed(SIZE_MAX / 2, 4) == nullptr, "overflowing count refused");
    auto* zeroed = static_cast<uint8_t*>(heap_alloc_zeroed(4, 8));
    check(zeroed && zeroed[0] == 0 && zeroed[31] == 0, "zeroed block");
    char* copy = heap_duplicate_string("savemouse");
    check(copy && std::strcmp(copy, "savemouse") == 0, "duplicate string");
    check(heap_duplicate_string(nullptr) == nullptr, "duplicate null");
    check(heap_stats().live_blocks == before.live_blocks + 3, "live block count");
    heap_free(grown);
    heap_free(zeroed);
    heap_free(copy);
    heap_free(nullptr);
    check(heap_stats().live_bytes == before.live_bytes, "all bytes returned");
}

void test_lock_reentry() {
    using namespace oa::platform;
    TokenLock lock;
    token_lock_reset(&lock);
    const TokenLockHold outer = token_lock_enter(&lock, 0x4d41494e);
    check(outer.previous == 0, "first entry acquires");
    const TokenLockHold inner = token_lock_enter(&lock, 0x4d41494e);
    check(inner.previous == 0x4d41494e, "same token re-enters without acquiring");
    token_lock_leave(&lock, &inner);
    check(lock.owner.load() == 0x4d41494e, "nested leave keeps the lock");
    token_lock_leave(&lock, &outer);
    check(lock.word.load() == 0 && lock.owner.load() == 0, "outer leave releases");
}

/// One thread's share of the lock-contention test.
struct ContentionRun {
    oa::platform::TokenLock* lock{};
    int* counter{};
    int which{};
};

/// Increments the shared count under the token lock, as the contention test's threads do.
void contention_worker(void* argument) {
    auto& run = *static_cast<ContentionRun*>(argument);
    for (int i = 0; i < 2000; ++i) {
        const oa::platform::TokenLockHold hold =
            oa::platform::token_lock_enter(run.lock, 0x100 + run.which);
        ++*run.counter;
        oa::platform::token_lock_leave(run.lock, &hold);
    }
}

void test_lock_contention() {
    using namespace oa::platform;
    TokenLock lock;
    token_lock_reset(&lock);
    int counter = 0;
    ContentionRun runs[4]{};
    oa::base::threads::Thread threads[4]{};
    for (int t = 0; t < 4; ++t) {
        runs[t] = ContentionRun{&lock, &counter, t};
        check(
            oa::base::threads::start_thread(threads[t], contention_worker, &runs[t]),
            "contention thread starts"
        );
    }
    for (auto& thread : threads)
        oa::base::threads::join_thread(thread);
    check(counter == 8000, "lock serialises increments");
}

void test_thread_and_clock() {
    using namespace oa::platform;
    std::atomic<int> ran{0};
    const uint32_t start = tick_ms();
    check(
        start_thread(
            [](void* argument) { static_cast<std::atomic<int>*>(argument)->store(1); }, 0x8000, &ran
        ),
        "thread starts"
    );
    for (int i = 0; i < 200 && ran.load() == 0; ++i) {
        sleep_ms(5);
    }
    check(ran.load() == 1, "thread body runs");
    check(static_cast<uint32_t>(tick_ms() - start) < 5000, "tick count advances modestly");
    check(!perf_device_available(), "no perf device");
    uint64_t value = 7;
    check(
        !perf_read_event_counter(0, &value) && value == 7,
        "perf read refused without touching output"
    );
    const uint64_t first = perf_now_ns();
    check(perf_now_ns() >= first, "perf clock monotonic");
}

std::string last_error;

void test_error_sink() {
    using namespace oa::platform;
    set_error_sink([](const char* message) { last_error = message; });
    show_error_message("disk full");
    check(last_error == "disk full", "error sink receives message");
    set_error_sink(nullptr);
}

void test_files() {
    using namespace oa::platform;
    const auto path = oa::test::make_scratch_directory("oa-platform-files-test") / "files.bin";
    const Files files = stdio_files();
    FileHandle* out = files.open(files.context, path.string().c_str(), "wb");
    check(out != nullptr, "open for write");
    const char payload[] = "0123456789";
    check(files.write(files.context, out, payload, 1, 10) == 10, "write");
    files.close(files.context, out);
    FileHandle* in = files.open(files.context, path.string().c_str(), "rb");
    check(in != nullptr, "open for read");
    check(files.seek(files.context, in, 4, SeekOrigin::begin) == 0, "seek");
    char buffer[4]{};
    check(files.read(files.context, in, buffer, 2, 2) == 2, "read elements");
    check(std::memcmp(buffer, "4567", 4) == 0, "read content");
    check(files.tell(files.context, in) == 8, "tell");
    check(
        files.seek(files.context, in, -1, SeekOrigin::end) == 0 &&
            files.tell(files.context, in) == 9,
        "seek end"
    );
    files.close(files.context, in);
    check(
        files.open(files.context, (path.string() + ".missing").c_str(), "rb") == nullptr,
        "missing file"
    );
    std::filesystem::remove_all(path.parent_path());
    check(log_message("platform-shims log %d\n", 1) > 0, "log writes");
}

void test_open_file() {
    using namespace oa::platform;
    const auto directory = oa::test::make_scratch_directory("oa-platform-open-file");
    // A name outside the narrow code pages of Windows opens by its path.
    const auto path = directory / std::filesystem::path(u8"r\u00e9sum\u00e9-\u65e5\u672c.txt");
    std::FILE* out = open_file(path, "wb");
    check(out != nullptr, "a file of any name opens for writing");
    if (out != nullptr) {
        check(std::fwrite("abc", 1, 3, out) == 3, "the file takes its bytes");
        std::fclose(out);
    }
    // Another stream reads the file while one still has it open to append.
    std::FILE* writer = open_file(path, "ab");
    std::FILE* reader = open_file(path, "rb");
    check(writer != nullptr && reader != nullptr, "an open file opens again for reading");
    char contents[4]{};
    check(
        reader != nullptr && std::fread(contents, 1, 3, reader) == 3 &&
            std::memcmp(contents, "abc", 3) == 0,
        "the second stream reads what the first wrote"
    );
    if (reader != nullptr)
        std::fclose(reader);
    if (writer != nullptr)
        std::fclose(writer);
    check(open_file(directory / "missing.txt", "rb") == nullptr, "a missing file does not open");
    const std::string narrow = (directory / "narrow.txt").string();
    std::FILE* narrow_file = open_file(narrow.c_str(), "wb");
    check(narrow_file != nullptr, "a file opens by its narrow path");
    if (narrow_file != nullptr)
        std::fclose(narrow_file);
    std::error_code removal;
    std::filesystem::remove_all(directory, removal);
}

void test_longest_path() {
    using namespace oa::platform;
#if defined(_WIN32)
    check(
        longest_path() == (long_paths_turned_off() ? 259U : 32767U),
        "Windows opens 259 characters, or 32,767 with long paths turned on"
    );
#elif defined(__APPLE__)
    check(longest_path() == 1023 && !long_paths_turned_off(), "macOS opens 1,023 bytes");
#elif defined(__linux__)
    check(longest_path() == 4095 && !long_paths_turned_off(), "Linux opens 4,095 bytes");
#endif
    // A file whose path passes the game's own 256-byte fields opens and
    // reads back, where the system opens paths that long.
    if (longest_path() < 400)
        return;
    const auto root = oa::test::make_scratch_directory("oa-platform-long-path");
    auto directory = root;
    while (directory.native().size() < 300)
        directory /= std::string(60, 'p');
    std::error_code made;
    std::filesystem::create_directories(directory, made);
    check(!made, "a folder more than 300 characters deep is made");
    const auto path = directory / "deep.txt";
    std::FILE* out = open_file(path, "wb");
    check(out != nullptr, "a file more than 300 characters deep opens for writing");
    if (out != nullptr) {
        check(std::fwrite("abc", 1, 3, out) == 3, "the deep file takes its bytes");
        std::fclose(out);
    }
    const std::string narrow = path.string();
    std::FILE* in = open_file(narrow.c_str(), "rb");
    char contents[4]{};
    check(
        in != nullptr && std::fread(contents, 1, 3, in) == 3 &&
            std::memcmp(contents, "abc", 3) == 0,
        "the deep file reads back by its narrow path"
    );
    if (in != nullptr)
        std::fclose(in);
    std::error_code removal;
    std::filesystem::remove_all(root, removal);
}

void test_environment_value() {
    using namespace oa::platform;
    check(
        !environment_value("OA_PLATFORM_TEST_VARIABLE_NEVER_SET").has_value(),
        "an unset variable has no value"
    );
    const auto search_path = environment_value("PATH");
    check(search_path.has_value() && !search_path->empty(), "PATH has its value");
}

void test_grouped_decimal() {
    using namespace oa::platform;
    char text[32];
    const auto grouped = [&](uint64_t value, const char* expected) {
        return format_grouped_decimal(value, text, sizeof text) == std::strlen(expected) &&
               std::strcmp(text, expected) == 0;
    };
    check(grouped(0, "0"), "zero");
    check(grouped(999, "999"), "three digits");
    check(grouped(1000, "1,000"), "thousands comma");
    check(grouped(1234567, "1,234,567"), "two commas");
    check(grouped(UINT64_MAX, "18,446,744,073,709,551,615"), "widest value");
    check(format_grouped_decimal(1000, text, 5) == 0, "no room for the terminator");
}

struct SampleScript {
    oa::platform::MemorySample next{};
    int calls = 0;
};

bool scripted_sample(void* context, oa::platform::MemorySample* out) {
    auto& script = *static_cast<SampleScript*>(context);
    ++script.calls;
    *out = script.next;
    return true;
}

void test_memory_status() {
    using namespace oa::platform;
    SampleScript script;
    script.next.mapped = 0x3000;
    script.next.working_set = 0x2000;
    script.next.private_resident = 0x1000;
    script.next.shared_resident = 0x1000;
    script.next.page_tables = 0x400000;
    MemoryStatusReport report;
    char text[512];
    const std::size_t length =
        format_memory_status(report, scripted_sample, &script, text, sizeof text);
    const char expected[] = "\r\nMapped now:                 12,288\r\n"
                            "Mapped peak:                12,288\r\n"
                            "Resident now:                8,192\r\n"
                            "Resident peak:               8,192\r\n"
                            "Resident private:            4,096\r\n"
                            "Resident shared:             4,096\r\n"
                            "Page table size:         4,194,304\r\n";
    check(
        length == std::strlen(expected) && std::strcmp(text, expected) == 0,
        "working-set report text"
    );
    check(
        script.calls == 1 && report.refresh_countdown == memory_status_calls_per_sample,
        "first call samples"
    );

    // The next nine calls reuse the sample; the tenth takes a new one.
    script.next.mapped = 0x1000;
    script.next.working_set = 0x6000;
    for (int call = 0; call < 9; ++call)
        (void)format_memory_status(report, scripted_sample, &script, text, sizeof text);
    check(script.calls == 1, "nine calls between samples");
    (void)format_memory_status(report, scripted_sample, &script, text, sizeof text);
    check(script.calls == 2, "tenth call samples again");
    check(report.peak_mapped == 0x3000 && report.peak_working_set == 0x6000, "peaks only rise");
    check(
        std::strstr(
            text,
            "Mapped now:                  4,096\r\n"
            "Mapped peak:                12,288"
        ) != nullptr,
        "current and peak mapped memory"
    );

    // Without a working set the report ends with the code size, or nothing.
    MemoryStatusReport bare;
    SampleScript code_only;
    code_only.next.mapped = 0x2000;
    code_only.next.code = 0x1000;
    (void)format_memory_status(bare, scripted_sample, &code_only, text, sizeof text);
    check(
        std::strcmp(
            text,
            "\r\nMapped now:                  8,192\r\n"
            "Mapped peak:                 8,192\r\n"
            "Code size:                   4,096\r\n"
        ) == 0,
        "code line without a working set"
    );
    MemoryStatusReport empty;
    SampleScript nothing;
    (void)format_memory_status(empty, scripted_sample, &nothing, text, sizeof text);
    check(
        std::strcmp(
            text,
            "\r\nMapped now:                      0\r\n"
            "Mapped peak:                     0\r\n"
        ) == 0,
        "no code line for zero code"
    );
    check(
        format_memory_status(report, scripted_sample, &script, text, 16) == 0 && text[0] == '\0',
        "report that does not fit"
    );

    MemorySample host{};
    if (sample_process_memory(nullptr, &host))
        check(host.working_set != 0, "host reports a working set");
}

// The blocks test_host_heap_use allocates, and the bytes of each: small
// enough that the allocator keeps the memory they free for later blocks.
constexpr std::size_t heap_use_block_count = 128;
constexpr std::size_t heap_use_block_bytes = std::size_t{32} * 1024;

// Where test_host_heap_use keeps its blocks, through which the compiler cannot
// drop the allocations.
char* heap_use_blocks[heap_use_block_count]{};

void test_host_heap_use() {
    using namespace oa::platform;
    HostHeapUse before{};
    if (!sample_host_heap_use(&before)) {
        check(before.bytes == 0 && before.blocks == 0, "no heap use is reported as zero");
        std::puts("platform shims: the host reports no heap use; its checks are skipped");
        return;
    }
    check(before.bytes != 0, "the heap holds this program's blocks");
    // Blocks allocated and filled are counted until they are freed; the free
    // memory the allocator keeps afterwards is not.
    constexpr std::size_t all_bytes = heap_use_block_count * heap_use_block_bytes;
    for (auto*& block : heap_use_blocks) {
        block = new char[heap_use_block_bytes];
        std::memset(block, 1, heap_use_block_bytes);
    }
    HostHeapUse held{};
    check(sample_host_heap_use(&held), "the heap's use is reported again");
    check(held.bytes >= before.bytes + all_bytes, "live blocks are counted");
    check(
        held.blocks == 0 || held.blocks >= before.blocks + heap_use_block_count,
        "live blocks are counted one by one"
    );
    for (auto*& block : heap_use_blocks) {
        delete[] block;
        block = nullptr;
    }
    HostHeapUse after{};
    check(sample_host_heap_use(&after), "the heap's use is reported a third time");
    check(after.bytes + all_bytes <= held.bytes, "freed blocks are no longer counted");
    check(sample_host_heap_use(nullptr) == false, "no sample without somewhere to put it");
}

// A process status text as Linux writes it, cut to the lines around the
// fields the memory guard reads.
constexpr std::string_view sample_proc_status = "Name:\tgame\n"
                                                "VmPeak:\t 4194304 kB\n"
                                                "VmSize:\t 4194304 kB\n"
                                                "VmRSS:\t  262144 kB\n"
                                                "RssAnon:\t  196608 kB\n"
                                                "RssFile:\t   65536 kB\n"
                                                "RssShmem:\t       0 kB\n"
                                                "VmSwap:\t    8192 kB\n"
                                                "Threads:\t8\n";

// A memory information text as Linux writes it, cut to its first lines.
constexpr std::string_view sample_proc_meminfo = "MemTotal:        8045756 kB\n"
                                                 "MemFree:          512000 kB\n"
                                                 "MemAvailable:    4022878 kB\n"
                                                 "Buffers:          102400 kB\n"
                                                 "HugePages_Total:       0\n";

void test_proc_memory_fields() {
    using namespace oa::platform;
    constexpr uint64_t kib = 1024;
    check(proc_memory_field(sample_proc_meminfo, "MemTotal") == 8045756 * kib, "total memory read");
    check(
        proc_memory_field(sample_proc_meminfo, "MemAvailable") == 4022878 * kib,
        "available memory read"
    );
    check(
        !proc_memory_field(sample_proc_meminfo, "Mem").has_value(),
        "a field's name must be followed by its colon"
    );
    check(
        !proc_memory_field(sample_proc_meminfo, "HugePages_Total").has_value(),
        "a count without kB is not a memory figure"
    );
    check(!proc_memory_field(sample_proc_meminfo, "SwapTotal").has_value(), "missing field");
    check(!proc_memory_field(sample_proc_meminfo, "").has_value(), "empty field name");
    check(!proc_memory_field("", "MemTotal").has_value(), "empty text");
    check(proc_memory_field("RssAnon:\t12 kB\r\n", "RssAnon") == 12 * kib, "line ended by CR LF");
    check(proc_memory_field("RssAnon:\t12 kB", "RssAnon") == 12 * kib, "last line without LF");
    check(!proc_memory_field("RssAnon:\t kB\n", "RssAnon").has_value(), "no digits");
    check(!proc_memory_field("RssAnon:\t12 MB\n", "RssAnon").has_value(), "another unit");
    check(!proc_memory_field("RssAnon:\t-12 kB\n", "RssAnon").has_value(), "a negative count");
    check(
        proc_memory_field("RssAnon:\t18014398509481983 kB\n", "RssAnon") ==
            uint64_t{18014398509481983} * kib,
        "largest count that fits 64 bits as bytes"
    );
    check(
        !proc_memory_field("RssAnon:\t18014398509481984 kB\n", "RssAnon").has_value(),
        "count too large as bytes"
    );
    check(
        !proc_memory_field("RssAnon:\t99999999999999999999 kB\n", "RssAnon").has_value(),
        "count too large for 64 bits"
    );

    // Private committed memory: resident anonymous memory plus swap, never
    // the address space (VmSize) or the resident file pages.
    check(
        committed_from_proc_status(sample_proc_status) == (196608 + 8192) * kib,
        "committed memory is RssAnon plus VmSwap"
    );
    check(
        committed_from_proc_status("RssAnon:\t 4096 kB\n") == 4096 * kib,
        "missing VmSwap counts as none"
    );
    check(
        !committed_from_proc_status("VmSize:\t 4096 kB\nVmSwap:\t 0 kB\n").has_value(),
        "no committed memory without RssAnon"
    );
    check(
        !committed_from_proc_status("RssAnon:\t18014398509481983 kB\nVmSwap:\t1 kB\n").has_value(),
        "a sum too large for 64 bits"
    );
}

void test_app_loop() {
    using namespace oa::platform;
    check(!finished_stream_sweep_due(1099, 1000), "99 ms is not enough");
    check(finished_stream_sweep_due(1100, 1000), "100 ms sweeps");
    check(finished_stream_sweep_due(5, 0xFFFFFFA0u), "sweep across the clock wrap");
    check(!finished_stream_sweep_due(1000, 1100), "clock stepping back waits");
    check(!application_waits_for_events(true, false, false), "active application ticks");
    check(application_waits_for_events(false, false, false), "inactive application waits");
    check(!application_waits_for_events(false, true, false), "live multiplayer game runs on");
    check(!application_waits_for_events(false, false, true), "online session runs on");
    using Action = MusicFocusAction;
    check(music_focus_action(false, true, false) == Action::park, "inactive open player parks");
    check(music_focus_action(false, false, true) == Action::none, "closed player stays closed");
    check(music_focus_action(true, false, true) == Action::resume, "parked player resumes");
    check(music_focus_action(true, false, false) == Action::none, "player closed elsewhere");
    check(music_focus_action(true, true, true) == Action::none, "open player needs nothing");
}

void test_error_log() {
    using namespace oa::platform;
    const auto scratch = oa::test::make_scratch_directory("oa-platform-error-log");
    // The folder is named in Chinese (U+7528 U+6237) and given in UTF-8, as
    // SDL gives the application's folder.
    const std::string_view hanzi = "\xE7\x94\xA8\xE6\x88\xB7";
    const auto directory = scratch / std::u8string(hanzi.begin(), hanzi.end());
    std::filesystem::create_directories(directory);
    const auto folder = (directory / "").u8string();
    const std::string prefix(folder.begin(), folder.end());
    check(append_error_log(prefix.c_str(), out_of_memory_message), "first report");
    check(append_error_log(prefix.c_str(), out_of_memory_message), "second report appends");
    std::FILE* log = open_file(directory / error_log_file_name, "rb");
    char contents[128]{};
    const std::size_t read = log != nullptr ? std::fread(contents, 1, sizeof contents - 1, log) : 0;
    if (log != nullptr)
        std::fclose(log);
    check(
        read == 2 * (sizeof out_of_memory_message - 1) &&
            std::strcmp(
                contents,
                "Out of memory!\r\nYour hard disk may be full\r\n"
                "Out of memory!\r\nYour hard disk may be full\r\n"
            ) == 0,
        "log holds both reports"
    );
    const std::string missing = (scratch / "missing" / "").string();
    check(!append_error_log(missing.c_str(), "x"), "missing folder fails");
    std::filesystem::remove_all(scratch);
    check(error_log_directory("/games/oa/") == "/games/oa/", "beside the executable");
    check(
        error_log_directory("/Applications/open-annihilation.app/Contents/Resources/") ==
            "/Applications/",
        "beside a bundle, not inside it"
    );
    check(
        error_log_directory("/games/Contents/Resources/") == "/games/Contents/Resources/",
        "a Resources folder outside a bundle is the executable's own"
    );
    check(error_log_directory(nullptr).empty(), "no base path gives the working directory");
}
} // namespace

#ifndef _WIN32
// Runs a crash test in a child process and returns its wait status.
template <typename Crash>
int child_status(Crash crash) {
    std::fflush(nullptr);
    const pid_t child = fork();
    if (child == 0) {
        crash();
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return status;
}

constexpr int kOutOfMemoryStatus = 3;

void test_crash_tests() {
    using namespace oa::platform;
    const int trap = child_status([] { break_into_debugger(); });
    check(WIFSIGNALED(trap) && WTERMSIG(trap) == SIGTRAP, "debug break traps");
    const int divide = child_status([] { raise_divide_fault(); });
    check(WIFSIGNALED(divide) && WTERMSIG(divide) == SIGFPE, "divide fault raises SIGFPE");
    const int heap = child_status([] {
        std::set_new_handler([] { _exit(kOutOfMemoryStatus); });
        exhaust_heap("heap exhaustion test");
    });
    check(
        WIFEXITED(heap) && WEXITSTATUS(heap) == kOutOfMemoryStatus,
        "heap exhaustion runs the handler"
    );
}
#else
void test_crash_tests() {
}
#endif

int main() {
    // Before any test starts a thread: the crash tests fork.
    test_crash_tests();
    test_allocator();
    test_lock_reentry();
    test_lock_contention();
    test_thread_and_clock();
    test_error_sink();
    test_files();
    test_open_file();
    test_longest_path();
    test_environment_value();
    test_grouped_decimal();
    test_memory_status();
    test_host_heap_use();
    test_proc_memory_fields();
    test_app_loop();
    test_error_log();
    if (failures != 0) {
        std::fprintf(stderr, "%d platform check(s) failed\n", failures);
        return 1;
    }
    std::puts("platform shims ok");
    return 0;
}
