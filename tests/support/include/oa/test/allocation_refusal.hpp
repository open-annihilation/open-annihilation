// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Which allocations a test that replaces operator new to refuse a chosen one
// may refuse. Visual Studio's standard library, with its iterator checks on
// (its Debug builds), gives every standard container a small bookkeeping
// block from operator new, also in constructors that may not throw, such as
// a vector's default and move constructors. A refusal there ends the program
// at once, before the code under test can recover. Release builds and the
// other standard libraries have no such block, so a test passes these blocks
// by: it neither counts nor refuses them, and refuses the same storage in
// every build.
#pragma once

#include <cstddef>
#include <memory>

namespace oa::test {

/// Tells the standard library's own container bookkeeping from the storage a
/// test means to refuse.
///
/// @param bytes the size operator new was asked for
/// @return true for a block of the bookkeeping's size in a build whose
///         library allocates one for each container; false in every other build
[[nodiscard]] constexpr bool
library_bookkeeping_block([[maybe_unused]] std::size_t bytes) noexcept {
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
    return bytes == sizeof(std::_Container_proxy);
#else
    return false;
#endif
}

} // namespace oa::test
