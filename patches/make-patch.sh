#!/usr/bin/env bash
# Writes patches/<submodule>.patch, the difference between the commit external/<submodule> is pinned at and
# its current files. CMake applies the patches when configuring (see string_utils_apply_patches in
# cmake/string_utils.cmake).
#
#   <path to string-utils>/patches/make-patch.sh <submodule> [<file>...]
#
# Run it from the root of the project the patch belongs to, e.g. patches/make-patch.sh libsais in string-utils or
# external/string-utils/patches/make-patch.sh <submodule> in a project that uses string-utils.
#
# <submodule> is the path below external/ (also of a nested submodule, e.g. sdsl-lite/external/libdivsufsort). The
# patch contains only the files it contained before and the given <file>s (paths relative to the
# submodule). Files that do not differ from the pinned commit are omitted; an empty patch is removed.
set -euo pipefail

root=$(pwd)
submodule=${1:?"usage: $0 <submodule below external/> [<file>...]"}
shift
dir="$root/external/$submodule"

if [ ! -d "$dir" ]; then
    echo "$0: external/$submodule does not exist; run this script from the root of the project the patch belongs to" >&2
    exit 1
fi
patch="$root/patches/$submodule.patch"

files=()
if [ -f "$patch" ]; then
    while IFS= read -r -d '' entry; do
        files+=("${entry#*$'\t'*$'\t'}") # <added> TAB <deleted> TAB <path>
    done < <(git apply --numstat -z "$patch")
fi
files+=("$@")
if [ ${#files[@]} -eq 0 ]; then
    echo "$0: no files given; specify the files of external/$submodule to include in the patch" >&2
    exit 1
fi

# use a temporary index (the index of the submodule is not modified)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export GIT_INDEX_FILE="$tmp/index"
git -C "$dir" read-tree HEAD

# core.fileMode=false: ignore changes of the executable bit
git -C "$dir" --literal-pathspecs -c core.fileMode=false add --force -- "${files[@]}"
git -C "$dir" -c core.fileMode=false diff --cached --binary --no-color --no-ext-diff --no-textconv \
    --no-renames --src-prefix=a/ --dst-prefix=b/ HEAD > "$tmp/patch"

if [ -s "$tmp/patch" ]; then
    mkdir -p "$(dirname "$patch")"
    mv "$tmp/patch" "$patch"
else
    rm -f "$patch"
fi
