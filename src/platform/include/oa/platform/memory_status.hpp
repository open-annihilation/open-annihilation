// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Memory report of the developer statistics, sampled from the host's process
// memory counters, and the system's memory sampled for the memory guard.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace oa::platform {

// One sample of the process's memory, in bytes. Counters the host cannot
// report stay zero.
struct MemorySample {
    uint64_t mapped{};           // committed memory
    uint64_t code{};             // committed executable memory
    uint64_t working_set{};      // resident memory; 0 when the host has no working set
    uint64_t private_resident{}; // resident pages no other process maps
    uint64_t shared_resident{};
    uint64_t page_tables{};
    // The most committed memory and the largest working set the host has seen
    // since the process started; 0 when the host keeps no such peak.
    uint64_t peak_mapped{};
    uint64_t peak_working_set{};
};

// Report state kept between calls.
struct MemoryStatusReport {
    MemorySample sample{};
    uint64_t peak_mapped{};
    uint64_t peak_working_set{};
    int32_t refresh_countdown{-1}; // calls left before the next sample; negative samples at once
};

// Calls a sample serves: the report samples on its first call and on every
// tenth call after a sample.
inline constexpr int32_t memory_status_calls_per_sample = 10;

// Source of samples; false leaves the previous sample in place.
using MemorySampler = bool (*)(void* context, MemorySample* out);

/// Samples this process's memory counters from the host.
///
/// @param context unused; present to match MemorySampler
/// @param[out] out counters the host reports; the rest stay zero
/// @return false when the host cannot report any counters
bool sample_process_memory(void* context, MemorySample* out) noexcept;

/// The host allocator's in-use counts for this process.
struct HostHeapUse {
    uint64_t bytes{}; ///< allocator-reported capacity in use, including allocation padding
    /// blocks allocated and not yet freed; 0 where the host does not count them
    uint64_t blocks{};
};

/// Samples the host allocator's in-use counts, rather than requested payload
/// sizes. Capacity can exceed the request: on macOS a smaller live allocation
/// can reuse an oversized freed block without shrinking it. Changing capacity
/// alone therefore does not establish that earlier owners kept their blocks.
/// Free memory held for later allocations is excluded, except on Linux where
/// the GNU C library also counts each thread's freed cached blocks as in use.
/// That cache fills over many allocations, and the tunable
/// glibc.malloc.tcache_count=0 turns it off.
///
/// Every heap of the process is counted on Windows, every malloc zone on macOS
/// and iOS, and every arena of the GNU C library on Linux; under the address
/// sanitizer, its own allocator's count. Every call reads the heap afresh,
/// allocates nothing and keeps no state. On Windows it calls nothing newer
/// than Windows XP SP3.
///
/// @param[out] out the sample; zero when the host reports nothing
/// @return false when the host does not report its heap's use
bool sample_host_heap_use(HostHeapUse* out) noexcept;

/// The system's own judgment of how short memory is, where it gives one.
enum class MemoryPressure : uint8_t {
    unknown, ///< the system gives no judgment
    normal,  ///< memory is not short
    /// the system is reclaiming and compressing memory, which it also does
    /// under ordinary load with plenty free
    warning,
    critical, ///< memory is exhausted: the system is paging hard or ending processes
};

/// One sample of the system's memory and this process's share of it, for the
/// memory guard of the accelerated tier. A figure the system does not report
/// keeps its `known` flag false.
struct SystemMemorySample {
    uint64_t physical{}; ///< installed physical memory, in bytes; 0 when not reported
    /// Physical memory free for new allocations without paging, in bytes,
    /// counting what the system can reclaim at once; on macOS a whole
    /// percentage of physical memory.
    uint64_t available{};
    /// This process's private committed memory, in bytes: what it has
    /// allocated for itself, resident or paged out, never its whole address
    /// space or the files and libraries it maps.
    uint64_t committed{};
    /// Page faults of this process that had to read from disk, since it
    /// started.
    uint64_t hard_faults{};
    MemoryPressure pressure{MemoryPressure::unknown};
    bool available_known{};
    bool committed_known{};
    bool hard_faults_known{};
};

/// Samples the system's free physical memory, its memory pressure, and this
/// process's private committed memory and hard page faults.
///
/// Free physical memory is reported on Windows, Linux and macOS, the memory
/// pressure level on macOS, and hard page faults on macOS, Linux and other
/// POSIX systems; the pressure, then the hard page faults, stand in for free
/// memory where it is not reported. Every call reads the system afresh and
/// keeps no state. On Windows it calls nothing newer than Windows XP SP3.
///
/// @param[out] out the sample; figures the system does not report stay
///     unknown
/// @return true when any figure was reported
bool sample_system_memory(SystemMemorySample* out) noexcept;

/// Reads one memory figure from the text of a Linux status file
/// (`/proc/meminfo`, `/proc/self/status`): the line that starts with the
/// field's name and a colon, then a decimal count of kibibytes and "kB".
///
/// @param text the file's text
/// @param field the field's name, without its colon
/// @return the figure in bytes, or nothing when the field is missing, is not
///     a count of kibibytes or does not fit 64 bits
std::optional<uint64_t> proc_memory_field(std::string_view text, std::string_view field) noexcept;

/// Reads a process's private committed memory from the text of its Linux
/// status file: its resident anonymous memory and the memory it has in swap,
/// never its address space, which graphics drivers and thread arenas inflate.
///
/// @param text the text of `/proc/<pid>/status`
/// @return `RssAnon` plus `VmSwap` in bytes (`VmSwap` taken as 0 when
///     missing), or nothing when `RssAnon` is missing or malformed
std::optional<uint64_t> committed_from_proc_status(std::string_view text) noexcept;

/// Formats a value as decimal digits grouped in threes by commas ("1,234,567").
///
/// @param value value to format
/// @param[out] out text buffer
/// @param capacity bytes of out
/// @return the text length, or 0 when the text and its terminator do not fit
size_t format_grouped_decimal(uint64_t value, char* out, size_t capacity) noexcept;

/// Writes the memory report text, sampling when the countdown runs out.
///
/// The peaks are raised from the sample: from its current counters and from
/// the peaks the host keeps. The text is mapped memory and its
/// peak, then either the working-set breakdown or, when no working set was
/// ever seen, the code size.
///
/// @param[in,out] report state kept between calls
/// @param sampler source of samples
/// @param context value passed to sampler
/// @param[out] out text buffer
/// @param capacity bytes of out
/// @return the text length, or 0 when it does not fit
size_t format_memory_status(
    MemoryStatusReport& report, MemorySampler sampler, void* context, char* out, size_t capacity
) noexcept;

} // namespace oa::platform
