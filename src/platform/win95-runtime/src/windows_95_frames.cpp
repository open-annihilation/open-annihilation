// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Registers this program's frame information when the MinGW toolchain the
// Windows 95 build uses left it unregistered, so that a thrown C++ exception
// can be caught. A build for a later Windows has the toolchain register it,
// and the registration below stands aside where the look-up already works;
// this module is built only for Windows 95, where it does not.

#include <windows.h>

#include <cstdint>
#include <cstring>

// libgcc's unwinder, which the C++ run-time library calls to run a handler.
extern "C" void* _Unwind_Find_FDE(const void* at, void* bases);
extern "C" void __register_frame_info(const void* begin, void* object);

namespace {

/// The bytes a PE image keeps a section's name in, which is one byte short of
/// the name `.eh_frame` and the reason the section is found as `.eh_fram`.
constexpr int kSectionNameBytes = 8;
/// The first eight bytes of the section holding the frame descriptors, which
/// is the whole of the name where the image truncated it and the start of it
/// where the image kept it.
constexpr const char* const kFrameSectionName = ".eh_fram";
/// Room for the object libgcc fills in when the frame information is
/// registered. Its size is libgcc's, not this program's, so it is generous.
char frame_object[512];

/// Whether the unwinder can find the frame descriptor for an address, which is
/// all it needs before a handler can be run. This is libgcc's own exported
/// lookup, so the answer is its rather than a stand-in's.
[[nodiscard]] bool frame_descriptor_found(uintptr_t address) noexcept {
    void* bases[3] = {nullptr, nullptr, nullptr};
    return _Unwind_Find_FDE(reinterpret_cast<const void*>(address), bases) != nullptr;
}

/// Finds the program's frame descriptors in its own image.
///
/// @param[out] begin receives the section's address
/// @param[out] size receives the section's length in bytes
/// @return whether the image has such a section
bool frame_section(const unsigned char** begin, unsigned* size) noexcept {
    const auto* image = reinterpret_cast<const unsigned char*>(GetModuleHandleA(nullptr));
    if (image == nullptr)
        return false;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* windows = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
    if (windows->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(windows);
    for (int index = 0; index < windows->FileHeader.NumberOfSections; ++index, ++section) {
        if (std::strncmp(
                reinterpret_cast<const char*>(section->Name), kFrameSectionName, kSectionNameBytes
            ) != 0) {
            continue;
        }
        if (section->Misc.VirtualSize == 0)
            return false;
        *begin = image + section->VirtualAddress;
        *size = section->Misc.VirtualSize;
        return true;
    }
    return false;
}

} // namespace

/// Registers this program's frame information when the toolchain left it
/// unregistered, so that a thrown C++ exception can be caught.
///
/// The C++ run-time library runs a handler by calling libgcc's unwinder, which
/// looks up the frame descriptor of each address on the way down. It finds them
/// through a table that `__register_frame_info` fills, and the start-up the
/// MinGW toolchain supplies here never calls that function. The table stays
/// empty, every lookup fails, and libgcc aborts the process rather than running
/// the handler — measured on this toolchain: `_Unwind_Find_FDE` reports nothing
/// for an address inside the program's own `main`, and a `throw` inside a `try`
/// ends the process with exit code 3 without the `catch` having run. Registering
/// the region makes the same lookup find the descriptor and the handler run.
///
/// It registers only where the lookup is already failing, so a toolchain whose
/// start-up does call the function is left alone, and it runs as an early
/// constructor so that a throw during any later start-up is caught too.
__attribute__((constructor(101))) void register_frame_information() noexcept {
    if (frame_descriptor_found(reinterpret_cast<uintptr_t>(&register_frame_information)))
        return;

    const unsigned char* begin = nullptr;
    unsigned size = 0;
    if (!frame_section(&begin, &size))
        return;
    __register_frame_info(begin, frame_object);
}
