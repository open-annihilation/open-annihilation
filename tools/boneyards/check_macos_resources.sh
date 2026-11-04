#!/usr/bin/env bash
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The macOS bundle's resource check, sourced by tools/release_macos.sh.
# fail and plist_value are that script's. With the module built, the bundle
# also holds coreprime.ccx, the module's language file, the page fonts
# (the faces under the font dependency folder, and the analogue atlas) and,
# in a tree that has data/boneyards/help, the Help pages that replace the
# expansion's, under boneyards/Help.
# With the module off, the lists are the ones the release has always used.

check_macos_resources() {
    local app="$1" build_dir="$2" deps_dir="$3" repo_dir="$4"
    local resources expected shipped_brains module cache
    local names=()
    resources="$(ls -A "$app/Contents/Resources" | LC_ALL=C sort | tr '\n' ' ')"
    shipped_brains=""
    [[ -f "$repo_dir/brains/classic/oabrain.yaml" ]] && shipped_brains="brains"
    cache="$build_dir/CMakeCache.txt"
    [[ -f "$cache" ]] || fail "no CMake cache at $cache; the resource check reads whether the module was built"
    module=0
    if grep -q '^OA_BONEYARDS:BOOL=ON$' "$cache"; then
        module=1
    fi
    names=(ATTRIBUTIONS.md LICENSE fonts languages licenses registries)
    [[ -n "$shipped_brains" ]] && names+=("$shipped_brains")
    [[ "$module" == 1 ]] && names+=(coreprime.ccx)
    [[ "$module" == 1 && -d "$repo_dir/data/boneyards/help" ]] && names+=(boneyards)
    names+=("$(plist_value "$app" CFBundleIconFile)")
    expected="$(printf '%s\n' "${names[@]}" | LC_ALL=C sort | tr '\n' ' ')"
    [[ "$resources" == "$expected" ]] || fail "unexpected files in Contents/Resources: $resources"
    if [[ -n "$shipped_brains" ]]; then
        [[ "$(ls -A "$app/Contents/Resources/brains")" == "classic.oabrain" ]] \
            || fail "Contents/Resources/brains holds more than classic.oabrain"
    fi
    cmp -s "$repo_dir/registries/coreprime.yaml" "$app/Contents/Resources/registries/coreprime.yaml" \
        || fail "Contents/Resources/registries/coreprime.yaml differs from the source"
    check_macos_languages "$app" "$repo_dir" "$module"
    check_macos_fonts "$app" "$repo_dir" "$deps_dir" "$module"
    if [[ "$module" == 1 ]]; then
        [[ -s "$app/Contents/Resources/coreprime.ccx" ]] \
            || fail "Contents/Resources/coreprime.ccx is missing"
        if [[ -d "$repo_dir/data/boneyards/help" ]]; then
            diff -rq "$repo_dir/data/boneyards/help" "$app/Contents/Resources/boneyards/Help" >/dev/null \
                || fail "Contents/Resources/boneyards/Help differs from data/boneyards/help"
        fi
        echo "module files: coreprime.ccx, the module language file, the page fonts, the Help pages"
    fi
}

check_macos_languages() {
    local app="$1" repo_dir="$2" module="$3"
    local dir="$app/Contents/Resources/languages"
    local langs expected file base
    if [[ "$module" == 0 ]]; then
        [[ "$(ls -A "$dir")" == "interface.tdf" ]] \
            || fail "Contents/Resources/languages holds more than interface.tdf"
        cmp -s "$repo_dir/languages/interface.tdf" "$dir/interface.tdf" \
            || fail "Contents/Resources/languages/interface.tdf differs from the source"
        return
    fi
    expected="$(
        find "$repo_dir/languages" -maxdepth 1 -name '*.tdf' -exec basename {} \; | LC_ALL=C sort | tr '\n' ' '
    )"
    langs="$(ls -A "$dir" | LC_ALL=C sort | tr '\n' ' ')"
    [[ "$langs" == "$expected" ]] || fail "Contents/Resources/languages holds $langs, not $expected"
    while IFS= read -r file; do
        [[ -n "$file" ]] || continue
        base="$(basename "$file")"
        cmp -s "$file" "$dir/$base" || fail "Contents/Resources/languages/$base differs from the source"
    done < <(find "$repo_dir/languages" -maxdepth 1 -name '*.tdf' | LC_ALL=C sort)
}

check_macos_fonts() {
    local app="$1" repo_dir="$2" deps_dir="$3" module="$4"
    local text_fonts="$deps_dir/text-fonts"
    local shipped_fonts fonts expected_fonts font
    local page_fonts atlas atlas_name relative listed
    # The fonts this version ships, read from the bootstrap's FONT_FILES.
    shipped_fonts="$(
        cd "$repo_dir" && python3 -c 'import sys; sys.path.insert(0, "tools"); import bootstrap_text_fonts as b; print("\n".join(b.FONT_FILES))'
    )"
    fonts="$(ls -A "$app/Contents/Resources/fonts" | LC_ALL=C sort | tr '\n' ' ')"
    if [[ "$module" == 0 ]]; then
        expected_fonts="$(printf '%s\n' "$shipped_fonts" | LC_ALL=C sort | tr '\n' ' ')"
        [[ "$fonts" == "$expected_fonts" ]] || fail "Contents/Resources/fonts holds $fonts, not $expected_fonts"
        while IFS= read -r font; do
            [[ -n "$font" ]] || continue
            cmp -s "$text_fonts/$font" "$app/Contents/Resources/fonts/$font" \
                || fail "Contents/Resources/fonts/$font differs from $text_fonts/$font"
        done <<< "$shipped_fonts"
        echo "the text fonts: $fonts"
        return
    fi
    page_fonts="$deps_dir/fonts"
    [[ -d "$page_fonts/liberation" ]] || fail "page fonts are missing under $page_fonts"
    # The atlas is committed. The package copies that file; it does not draw one.
    atlas="$repo_dir/data/boneyards/fonts/boneyards-analogues.bin"
    [[ -f "$atlas" ]] || fail "the analogue atlas is missing: $atlas"
    atlas_name="$(basename "$atlas")"
    if find "$app/Contents/Resources/fonts" -name 'NotoSansCJK-Medium.ttc' -print | grep -q .; then
        fail "Contents/Resources/fonts holds NotoSansCJK-Medium.ttc, which the package does not ship"
    fi
    expected_fonts="$(
        {
            printf '%s\n' "$shipped_fonts"
            find "$page_fonts" -mindepth 1 -maxdepth 1 -exec basename {} \;
            printf '%s\n' "$atlas_name"
        } | LC_ALL=C sort | tr '\n' ' '
    )"
    [[ "$fonts" == "$expected_fonts" ]] || fail "Contents/Resources/fonts holds $fonts, not $expected_fonts"
    while IFS= read -r font; do
        [[ -n "$font" ]] || continue
        cmp -s "$text_fonts/$font" "$app/Contents/Resources/fonts/$font" \
            || fail "Contents/Resources/fonts/$font differs from $text_fonts/$font"
    done <<< "$shipped_fonts"
    listed="$(cd "$page_fonts" && find . -type f | sed 's#^\./##' | LC_ALL=C sort)"
    while IFS= read -r relative; do
        [[ -n "$relative" ]] || continue
        cmp -s "$page_fonts/$relative" "$app/Contents/Resources/fonts/$relative" \
            || fail "Contents/Resources/fonts/$relative differs from $page_fonts/$relative"
    done <<< "$listed"
    relative="$(cd "$app/Contents/Resources/fonts" && find . -type f ! -name "$atlas_name" | sed 's#^\./##' | LC_ALL=C sort)"
    expected_fonts="$(
        {
            printf '%s\n' "$shipped_fonts"
            printf '%s\n' "$listed"
        } | sed '/^$/d' | LC_ALL=C sort
    )"
    [[ "$relative" == "$expected_fonts" ]] \
        || fail "Contents/Resources/fonts holds files the text fonts and the page fonts do not"
    cmp -s "$atlas" "$app/Contents/Resources/fonts/$atlas_name" \
        || fail "Contents/Resources/fonts/$atlas_name differs from $atlas"
    echo "the text fonts and the page fonts: $fonts"
}
