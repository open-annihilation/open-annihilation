// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A multipart/form-data body. The client posts the bytes as a request body.
// Nothing here opens a connection.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::netgame::http {

/// One field of a form. A file field carries bytes; a text field carries text.
struct FormField {
    /// The field's name, as the disposition names it.
    std::string name{};
    /// The file's name. Empty for a text field.
    std::string filename{};
    /// The file's type. Empty sends `application/octet-stream` for a file.
    std::string content_type{};
    /// A text field's value. Ignored for a file.
    std::string text{};
    /// A file field's bytes. Ignored for a text field.
    std::vector<uint8_t> bytes{};
    /// True for a file field.
    bool file{false};
};

/// A form ready to post.
struct FormBody {
    /// `multipart/form-data` and the boundary.
    std::string content_type{};
    /// The body, including the closing boundary.
    std::vector<uint8_t> bytes{};
};

/// Writes a multipart body.
///
/// Each field is one part, in the order given, separated by CRLF. A file part
/// names its file and a content type. A text part is the text as given. The
/// boundary is chosen so that it does not occur in any part. An empty list
/// is still a complete body.
///
/// @param fields the fields
/// @return the body and its content type
[[nodiscard]] FormBody encode_form(std::span<const FormField> fields);

} // namespace oa::netgame::http
