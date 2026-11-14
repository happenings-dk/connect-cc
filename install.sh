#!/bin/sh
# Bootstrap from an authenticated checkout; no shell profile is modified.
set -eu
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$source_dir"
exec go run ./cmd/connectcpp setup --source "$source_dir" "$@"
