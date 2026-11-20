// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/netgame/http/form.hpp"

#include <string_view>

namespace oa::netgame::http {
namespace {

constexpr std::string_view kLine = "\r\n";
constexpr std::string_view kBoundaryStem = "oaR20ShareBoundary";

/// Appends a text.
///
/// @param[in,out] out the body
/// @param text the text
void append_text(std::vector<uint8_t>& out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
}

/// Tells whether the bytes hold the needle.
///
/// @param bytes the bytes
/// @param needle the text
/// @return true when they do
bool holds(std::span<const uint8_t> bytes, std::string_view needle) {
    if (needle.empty() || bytes.size() < needle.size())
        return false;
    const auto* data = bytes.data();
    const std::size_t last = bytes.size() - needle.size();
    for (std::size_t at = 0; at <= last; ++at) {
        if (std::string_view(reinterpret_cast<const char*>(data + at), needle.size()) == needle)
            return true;
    }
    return false;
}

/// Tells whether a field holds the boundary.
///
/// @param field the field
/// @param boundary the boundary
/// @return true when it does
bool field_holds(const FormField& field, std::string_view boundary) {
    if (field.name.find(boundary) != std::string::npos)
        return true;
    if (field.filename.find(boundary) != std::string::npos)
        return true;
    if (field.content_type.find(boundary) != std::string::npos)
        return true;
    if (!field.file && field.text.find(boundary) != std::string::npos)
        return true;
    if (field.file && holds(field.bytes, boundary))
        return true;
    return false;
}

/// A boundary that no field holds.
///
/// @param fields the fields
/// @return the boundary
std::string choose_boundary(std::span<const FormField> fields) {
    std::string boundary(kBoundaryStem);
    for (int suffix = 0; suffix < 64; ++suffix) {
        if (suffix > 0)
            boundary = std::string(kBoundaryStem) + std::to_string(suffix);
        bool found = false;
        for (const FormField& field : fields) {
            if (field_holds(field, boundary))
                found = true;
        }
        if (!found)
            return boundary;
    }
    return boundary;
}

} // namespace

FormBody encode_form(std::span<const FormField> fields) {
    const std::string boundary = choose_boundary(fields);
    FormBody body;
    body.content_type = "multipart/form-data; boundary=" + boundary;
    for (const FormField& field : fields) {
        append_text(body.bytes, "--");
        append_text(body.bytes, boundary);
        append_text(body.bytes, kLine);
        append_text(body.bytes, "Content-Disposition: form-data; name=\"");
        append_text(body.bytes, field.name);
        append_text(body.bytes, "\"");
        if (field.file) {
            const std::string_view filename =
                field.filename.empty() ? std::string_view{"replay.oademo"} : field.filename;
            const std::string_view type = field.content_type.empty()
                                              ? std::string_view{"application/octet-stream"}
                                              : field.content_type;
            append_text(body.bytes, "; filename=\"");
            append_text(body.bytes, filename);
            append_text(body.bytes, "\"");
            append_text(body.bytes, kLine);
            append_text(body.bytes, "Content-Type: ");
            append_text(body.bytes, type);
            append_text(body.bytes, kLine);
            append_text(body.bytes, kLine);
            body.bytes.insert(body.bytes.end(), field.bytes.begin(), field.bytes.end());
            append_text(body.bytes, kLine);
        } else {
            append_text(body.bytes, kLine);
            append_text(body.bytes, kLine);
            append_text(body.bytes, field.text);
            append_text(body.bytes, kLine);
        }
    }
    append_text(body.bytes, "--");
    append_text(body.bytes, boundary);
    append_text(body.bytes, "--");
    append_text(body.bytes, kLine);
    return body;
}

} // namespace oa::netgame::http
