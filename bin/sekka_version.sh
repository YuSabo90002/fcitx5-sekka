#!/bin/sh
# Run from the repository root and print the libsekka version CI should build against.
# Helper for release automation and CI (the same practice as fcitx5-cskk).
set -e
sed -n -e "s/# GITHUB_ACTION_BUILD_SEKKA_VERSION=\(.*\)/\1/p" CMakeLists.txt
