#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 user@droplet /absolute/path/to/project/releases" >&2
    exit 2
fi

cd "$(dirname "$0")/.."
test -s dist/firmware.bin || { echo "Build the firmware first: pio run" >&2; exit 1; }

# Constrain arguments before using them in a remote shell command.
case "$1" in ''|-*|*[!a-zA-Z0-9_.@-]*) echo "Invalid SSH destination" >&2; exit 2 ;; esac
case "$2" in /*) ;; *) echo "Remote directory must be absolute" >&2; exit 2 ;; esac
case "$2" in *[!a-zA-Z0-9_./-]*) echo "Remote directory must not contain spaces or shell characters" >&2; exit 2 ;; esac

remote_dir=$2
destination=$1
temporary=$(ssh "$1" "mkdir -p '$remote_dir' && mktemp '$remote_dir/.firmware.XXXXXXXX'")
cleanup() {
    ssh "$destination" "rm -f '$temporary'" >/dev/null 2>&1 || true
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
scp dist/firmware.bin "$1:$temporary"
expected=$(python3 -c 'import hashlib; print(hashlib.sha256(open("dist/firmware.bin", "rb").read()).hexdigest())')
ssh "$1" "printf '%s  %s\n' '$expected' '$temporary' | sha256sum -c - && chmod 644 '$temporary' && mv -f '$temporary' '$remote_dir/firmware.bin'"
echo "Published ETag: \"$expected\""
