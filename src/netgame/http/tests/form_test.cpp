// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A share's multipart body: the replay, the title and who can see it.

#include "oa/netgame/http/form.hpp"
#include "oa/test/check.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace {

using oa::netgame::http::FormBody;
using oa::netgame::http::FormField;

std::string as_text(const std::vector<uint8_t>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void three_parts() {
    FormField replay;
    replay.name = "replay";
    replay.filename = "replay.oademo";
    replay.content_type = "application/octet-stream";
    replay.file = true;
    replay.bytes = {'O', 'A', 'D', '1'};
    FormField title;
    title.name = "title";
    title.text = "Show Down";
    FormField visibility;
    visibility.name = "visibility";
    visibility.text = "public";
    const FormField fields[] = {replay, title, visibility};
    const FormBody body = oa::netgame::http::encode_form(fields);
    OA_CHECK(body.content_type.find("multipart/form-data; boundary=") == 0);
    const std::string text = as_text(body.bytes);
    OA_CHECK(text.find("name=\"replay\"") != std::string::npos);
    OA_CHECK(text.find("filename=\"replay.oademo\"") != std::string::npos);
    OA_CHECK(text.find("Content-Type: application/octet-stream") != std::string::npos);
    OA_CHECK(text.find("OAD1") != std::string::npos);
    OA_CHECK(text.find("name=\"title\"") != std::string::npos);
    OA_CHECK(text.find("Show Down") != std::string::npos);
    OA_CHECK(text.find("name=\"visibility\"") != std::string::npos);
    OA_CHECK(text.find("public") != std::string::npos);
    const std::string marker = "\r\n--";
    OA_CHECK(text.find(marker) != std::string::npos);
    OA_CHECK(text.size() >= 4 && text.ends_with("--\r\n"));
}

void a_boundary_in_the_file_is_not_the_boundary() {
    FormField replay;
    replay.name = "replay";
    replay.file = true;
    const std::string planted = "oaR20ShareBoundary";
    replay.bytes.assign(planted.begin(), planted.end());
    const FormField fields[] = {replay};
    const FormBody body = oa::netgame::http::encode_form(fields);
    const std::string marker = "boundary=";
    const std::size_t at = body.content_type.find(marker);
    OA_CHECK(at != std::string::npos);
    if (at == std::string::npos)
        return;
    const std::string boundary = body.content_type.substr(at + marker.size());
    OA_CHECK(boundary != planted);
    OA_CHECK(boundary.find(planted) == 0);
    const std::string text = as_text(body.bytes);
    OA_CHECK(text.find("--" + boundary + "\r\n") != std::string::npos);
}

} // namespace

int main() {
    three_parts();
    a_boundary_in_the_file_is_not_the_boundary();
    return oa::test::check_exit_status();
}
