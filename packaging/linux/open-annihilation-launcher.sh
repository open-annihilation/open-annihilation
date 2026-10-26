#!/bin/sh
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Starts the version `current` names. When `next` is a link to one folder
# under `versions/`, that link is renamed over `current` first. Nothing here
# is deleted: an older version stays until the update helper removes it.

set -eu

here=$(dirname "$(readlink -f "$0")")

swapped=0
old=

if [ -L "$here/next" ]; then
    next_dir=$(readlink -f "$here/next" 2>/dev/null || true)
    versions_dir=$(readlink -f "$here/versions" 2>/dev/null || true)
    if [ -n "$next_dir" ] && [ -n "$versions_dir" ] && [ -d "$next_dir" ]; then
        case "$next_dir" in
            "$versions_dir"/*)
                rest=${next_dir#"$versions_dir"/}
                case "$rest" in
                    */*)
                        ;;
                    "")
                        ;;
                    *)
                        if [ -L "$here/current" ]; then
                            old_target=$(readlink "$here/current" || true)
                            if [ -n "$old_target" ]; then
                                old=$(basename "$old_target")
                            fi
                        fi
                        mv -T "$here/next" "$here/current"
                        swapped=1
                        ;;
                esac
                ;;
        esac
    fi
fi

if [ ! -e "$here/current" ] && [ ! -L "$here/current" ]; then
    only=
    count=0
    if [ -d "$here/versions" ]; then
        for entry in "$here"/versions/*; do
            if [ -d "$entry" ]; then
                count=$((count + 1))
                only=$entry
            fi
        done
    fi
    if [ "$count" -eq 1 ]; then
        ln -s "versions/$(basename "$only")" "$here/current"
    fi
fi

game=$here/current/open-annihilation
if [ "$swapped" -eq 1 ] && [ -n "$old" ]; then
    exec "$game" "$@" --updated-from "$old"
fi
exec "$game" "$@"
