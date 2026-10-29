// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Systems, and builds, without a browser to embed: no window opens.

#include "oa/platform/web_sign_in.hpp"

namespace oa::platform::web_sign_in {

struct Window::Shared {};

bool available() noexcept {
    return false;
}

Window::Window() = default;

Window::~Window() = default;

bool Window::open(const Request&) {
    return false;
}

void Window::close() {
}

State Window::state() const {
    return State::idle;
}

std::string Window::landed_address() const {
    return {};
}

} // namespace oa::platform::web_sign_in
