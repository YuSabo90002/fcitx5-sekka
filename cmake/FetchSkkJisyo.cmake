# FetchSkkJisyo.cmake
#
# Module that downloads SKK-JISYO.L (from skk-dev/dict) and verifies its integrity.
# It is `include(FetchSkkJisyo)`d only from inside the `if(SEKKA_BUILD_DICT)` block of
# `fcitx5-sekka/CMakeLists.txt` (D-76: since that option is off by default, this file is
# not read at all in the default configuration).
#
# Why the source is pinned to a fixed commit hash rather than a branch name such as master
# (D-63): following a branch would let the dictionary source change silently on every
# build. To keep it always possible to identify exactly which revision of SKK-JISYO.L a
# generated master-dict.db came from, the commit is pinned and the downloaded content is
# verified by SHA256. A download whose hash does not match is stopped as a configure error
# by `file(DOWNLOAD ... EXPECTED_HASH ...)`. The EXPECTED_HASH must not be removed to
# loosen the verification (a prohibition of this plan).
#
# How this relates to FR-010 (the runtime requirement that "no external server is contacted
# during conversion"): this download runs *at build time*, exactly once, on the developer's
# or packager's machine, as a CMake configure step taken only when -DSEKKA_BUILD_DICT=ON is
# given explicitly; it is not part of the conversion path (the runtime path where
# fcitx5-sekka is converting the user's input into kana and kanji). What FR-010 forbids is
# communication *during conversion*, not the use of external data to generate the
# dictionary at build time. The build machine and the end user's runtime environment are
# different things, and the generated master-dict.db is distributed and installed into the
# end user's environment as an entirely local file.
#
# Alternative source when the default one (raw.githubusercontent.com) is down:
# https://skk-dev.github.io/dict/SKK-JISYO.L.gz (over GitHub Pages, gzip-compressed;
# decompress it and point `SEKKA_SKKJISYO_SOURCE` at the local path).

set(SEKKA_SKKJISYO_COMMIT "14a1df7ec8f84410f5fb006978cf00878768150a" CACHE STRING
    "Commit hash of SKK-JISYO.L inside skk-dev/dict to fetch (D-63: a fixed commit, not a branch name)")
set(SEKKA_SKKJISYO_SHA256 "c791f578d1b4040fce282db29bc22b2cc7ea46f83e269fab2e0fa779e2967e40"
    "Expected SHA256 of SKK-JISYO.L (measured at the commit above; confirmed 2026-09-23)")
set(SEKKA_SKKJISYO_URL_BASE "https://raw.githubusercontent.com/skk-dev/dict" CACHE STRING
    "Base URL to fetch SKK-JISYO.L from (/<commit>/SKK-JISYO.L is appended)")
set(SEKKA_SKKJISYO_SOURCE "" CACHE FILEPATH
    "For offline builds: local path of an already fetched SKK-JISYO.L (when set, the download is skipped entirely. The offline escape hatch of D-63)")

if(SEKKA_SKKJISYO_SOURCE)
    # The offline escape hatch: skip the download entirely and use the given local file as it is.
    if(NOT EXISTS "${SEKKA_SKKJISYO_SOURCE}")
        message(FATAL_ERROR
            "the file given in SEKKA_SKKJISYO_SOURCE was not found: "
            "${SEKKA_SKKJISYO_SOURCE}")
    endif()
    set(_skkjisyo_raw "${SEKKA_SKKJISYO_SOURCE}")
else()
    set(_skkjisyo_raw "${CMAKE_BINARY_DIR}/SKK-JISYO.L")
    file(DOWNLOAD
        "${SEKKA_SKKJISYO_URL_BASE}/${SEKKA_SKKJISYO_COMMIT}/SKK-JISYO.L"
        "${_skkjisyo_raw}"
        EXPECTED_HASH "SHA256=${SEKKA_SKKJISYO_SHA256}"
        TLS_VERIFY ON
        STATUS _download_status)
    list(GET _download_status 0 _download_code)
    if(NOT _download_code EQUAL 0)
        list(GET _download_status 1 _download_message)
        message(FATAL_ERROR
            "failed to fetch SKK-JISYO.L (${_download_message}). "
            "In an offline environment, or when raw.githubusercontent.com is unreachable, "
            "obtain SKK-JISYO.L yourself and reconfigure with "
            "-DSEKKA_SKKJISYO_SOURCE=<local path>. "
            "Alternative source: https://skk-dev.github.io/dict/SKK-JISYO.L.gz (needs decompressing)")
    endif()
endif()
