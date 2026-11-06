// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/contents.hpp"

#include "text.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace oa::boneyards::screens {
namespace {

std::int32_t decode_int(const char* text) {
    return text != nullptr ? std::atoi(text) : 0;
}

constexpr const char* kRemotePrefix = "#Remote#";
constexpr std::size_t kRemotePrefixLength = 8;
constexpr const char* kHttpPrefix = "http://";
constexpr std::size_t kHttpPrefixLength = 7;

} // namespace

void contents_init_category(
    ContentsCategory* c, const char* id, const char* order, const char* name
) {
    std::memset(c, 0, sizeof *c);
    c->id = decode_int(id);
    c->order = decode_int(order);
    copy_bounded(c->name, sizeof c->name, name);
}

bool contents_insert_category(Contents* contents, const ContentsCategory* category) {
    if (contents->category_count >= kMaxContentsCategories)
        return false;
    std::size_t at = 0;
    while (at < contents->category_count && contents->categories[at].order <= category->order)
        ++at;
    for (std::size_t i = contents->category_count; i > at; --i)
        contents->categories[i] = contents->categories[i - 1];
    contents->categories[at] = *category;
    ++contents->category_count;
    return true;
}

bool contents_insert_headline(ContentsCategory* category, const ContentsHeadline* headline) {
    if (category->headline_count >= kMaxContentsHeadlines)
        return false;
    std::size_t at = 0;
    while (at < category->headline_count && category->headlines[at].order <= headline->order)
        ++at;
    for (std::size_t i = category->headline_count; i > at; --i)
        category->headlines[i] = category->headlines[i - 1];
    category->headlines[at] = *headline;
    ++category->headline_count;
    return true;
}

void contents_apply_field(Contents* contents, const ContentsField* f) {
    if (f->kind == nullptr)
        return;
    if (std::strcmp(f->kind, "category") == 0) {
        ContentsCategory scratch;
        contents_init_category(&scratch, f->id, f->order, f->name);
        contents_insert_category(contents, &scratch);
        return;
    }
    if (std::strcmp(f->kind, "headline") != 0)
        return;
    const std::int32_t parent = decode_int(f->parent);
    for (std::size_t i = 0; i < contents->category_count; ++i) {
        ContentsCategory* c = &contents->categories[i];
        if (c->id != parent)
            continue;
        ContentsHeadline h{};
        h.id = decode_int(f->id);
        h.order = decode_int(f->order);
        copy_bounded(h.title, sizeof h.title, f->title);
        copy_bounded(h.locator, sizeof h.locator, f->locator);
        contents_insert_headline(c, &h);
        return;
    }
}

std::size_t contents_build_rows(
    Contents* contents,
    bool select_first,
    ContentsRow* rows,
    std::size_t capacity,
    char* first_locator,
    std::size_t locator_capacity
) {
    std::size_t n = 0;
    for (std::size_t ci = 0; ci < contents->category_count; ++ci) {
        ContentsCategory* c = &contents->categories[ci];
        if (select_first && ci == 0)
            c->expanded = true;
        if (n < capacity)
            rows[n] = ContentsRow{true, ci, 0};
        ++n;
        if (!c->expanded)
            continue;
        for (std::size_t hi = 0; hi < c->headline_count; ++hi) {
            if (n < capacity)
                rows[n] = ContentsRow{false, ci, hi};
            ++n;
            if (select_first && ci == 0 && hi == 0 && first_locator != nullptr)
                copy_bounded(first_locator, locator_capacity, c->headlines[hi].locator);
        }
    }
    return n;
}

LocatorKind resolve_document_locator(
    const BoneyardsService* service,
    std::int32_t server_type,
    char* locator,
    std::size_t capacity,
    bool rewrite
) {
    char url[0x400];
    const char* target = locator;
    if (std::strncmp(locator, kRemotePrefix, kRemotePrefixLength) == 0) {
        char host[0x100];
        std::uint16_t port = 0;
        if (!service->content_server(service->user, server_type, host, sizeof host, &port))
            return LocatorKind::unavailable;
        std::snprintf(
            url,
            sizeof url,
            "http://%s:%d%s",
            host,
            static_cast<int>(port),
            locator + kRemotePrefixLength
        );
        if (rewrite)
            copy_bounded(locator, capacity, url);
        target = url;
    } else if (std::strncmp(locator, kHttpPrefix, kHttpPrefixLength) != 0) {
        return LocatorKind::local;
    }
    char temp[0x104]{};
    const ServiceResult result = service->fetch_file(service->user, target, temp, sizeof temp);
    return result == ServiceResult::pending || result == ServiceResult::ok
               ? LocatorKind::remote
               : LocatorKind::unavailable;
}

void ladder_page_locator(const char* base, std::int32_t position, char* out, std::size_t capacity) {
    char page[0x400];
    copy_bounded(page, sizeof page, base);
    std::int32_t at = static_cast<std::int32_t>(std::strlen(page)) - 5;
    if (at < 1)
        at = 0;
    page[at] = static_cast<char>((position - 1) / 100 + '1');
    page[at + 1] = '\0';
    std::snprintf(out, capacity, "%s.htm", page);
}

void ladder_index_locator(const char* page, char first, char* out, std::size_t capacity) {
    char index[0x400];
    copy_bounded(index, sizeof index, page);
    std::int32_t at = static_cast<std::int32_t>(std::strlen(index)) - 5;
    if (at < 1)
        at = 0;
    index[at] = first;
    index[at + 1] = '\0';
    std::snprintf(out, capacity, "%s.byi", index);
}

std::int32_t ladder_index_position(const char* index, std::size_t size, const char* name) {
    std::size_t at = 0;
    const auto next_word = [&](char* word, std::size_t capacity) {
        while (at < size && (index[at] == ' ' || index[at] == '\t' || index[at] == '\n' ||
                             index[at] == '\r' || index[at] == '\v' || index[at] == '\f'))
            ++at;
        std::size_t length = 0;
        while (at < size && index[at] != ' ' && index[at] != '\t' && index[at] != '\n' &&
               index[at] != '\r' && index[at] != '\v' && index[at] != '\f') {
            if (length + 1 < capacity)
                word[length++] = index[at];
            ++at;
        }
        word[length] = '\0';
        return length != 0;
    };
    char word[0x400];
    std::int32_t position = -1;
    while (position == -1 && next_word(word, sizeof word)) {
        if (!equals_nocase(word, name))
            continue;
        if (!next_word(word, sizeof word))
            break;
        position = static_cast<std::int32_t>(std::atoi(word));
    }
    return position;
}

void ladder_not_listed_message(const char* user, char* out, std::size_t capacity) {
    std::snprintf(out, capacity, "User \x93%s\x94 is not listed in this ladder.", user);
}

const char* planet_affiliation_label(char code) {
    switch (code) {
    case 'A':
    case 'a':
        return "Arm";
    case 'C':
    case 'c':
        return "Core";
    case 'I':
    case 'i':
        return "Contested";
    default:
        return nullptr;
    }
}

std::size_t gw_requirement_lines(std::int32_t requirement, const char** lines) {
    static constexpr const char* kDisk =
        "  Insert Total Anihilation Multiplayer Disk (1), and try again";
    static constexpr const char* kCc = "  Have Core Contingency Installed.";
    static constexpr const char* kBt = "  Have Battle Tactics Installed.";
    static constexpr const char* kMap =
        "  Download the specified GW Map from our web site at www.coreprime.net.";
    static constexpr const char* kCcAnd = "  Have Core Contingency Installed, and";
    static constexpr const char* kBtAnd = "  Have Battle Tactics Installed, and";
    const char* first = nullptr;
    const char* second = nullptr;
    int count = 0;
    switch (requirement) {
    case 0:
        first = kDisk;
        count = 1;
        break;
    case 1:
        first = kCc;
        count = 1;
        break;
    case 2:
        first = kBt;
        count = 1;
        break;
    case 3:
        first = kMap;
        count = 1;
        break;
    case 4:
        first = kCcAnd;
        second = kMap;
        count = 2;
        break;
    case 5:
        first = kBtAnd;
        second = kMap;
        count = 2;
        break;
    default:
        break;
    }
    std::size_t n = 0;
    lines[n++] =
        "Although you may play for fun on this planet you are currently unable to participate";
    lines[n++] = "in the Galactic War here or obtain more detailed reconnaissance.";
    if (count > 0) {
        lines[n++] = "For visual contact and to fight for your side on this planet you need to:";
        lines[n++] = first;
    }
    if (count > 1)
        lines[n++] = second;
    lines[n++] = "";
    return n;
}

} // namespace oa::boneyards::screens
