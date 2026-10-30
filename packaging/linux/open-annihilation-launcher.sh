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

# An unconfirmed install leaves update-starts. Three starts that have not
# reached the main menu point current at the other versions/ folder.
if [ -f "$here/update-starts" ]; then
    starts=0
    read -r starts < "$here/update-starts" || starts=0
    case "$starts" in
        ''|*[!0-9]*)
            starts=0
            ;;
    esac
    starts=$((starts + 1))
    printf '%s\n' "$starts" > "$here/update-starts"
    if [ "$starts" -ge 3 ]; then
        current_name=
        if [ -L "$here/current" ]; then
            current_target=$(readlink "$here/current" || true)
            if [ -n "$current_target" ]; then
                current_name=$(basename "$current_target")
            fi
        fi
        kept=
        if [ -d "$here/versions" ]; then
            for entry in "$here"/versions/*; do
                if [ -d "$entry" ]; then
                    name=$(basename "$entry")
                    if [ -n "$name" ] && [ "$name" != "$current_name" ]; then
                        kept=$name
                        break
                    fi
                fi
            done
        fi
        if [ -n "$kept" ] && [ -n "$current_name" ]; then
            ln -s "versions/$kept" "$here/rollback-link"
            mv -T "$here/rollback-link" "$here/current"
            rm -f "$here/update-starts"
            game=$here/current/open-annihilation
            exec "$game" "$@" --rolled-back-from "$current_name" --roll-back-reason failed-starts
        fi
    fi
fi

if [ "$swapped" -eq 1 ] && [ -n "$old" ]; then
    exec "$game" "$@" --updated-from "$old"
fi
exec "$game" "$@"
