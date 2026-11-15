#!/bin/bash
# Copyright (c) 2026 Jurjen Stellingwerff
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# screen's macOS window: mac_check.c shows a frame, reads the window's image back, posts keys
# and clicks into its own AppKit queue and prints what the shim made of them.  Exit 0 when every
# line is as expected.  It needs a window server (not an ssh session).
#   screen/desktop/mac_check.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=c99 -Wall -Wextra -O1 -o "$out/check" "$here/mac_check.c" || exit 2
"$out/check"
