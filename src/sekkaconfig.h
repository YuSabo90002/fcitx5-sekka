// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// sekkaconfig.h - Sekka configuration definitions
// Uses fcitx5's configuration framework (FCITX_CONFIGURATION)
#ifndef SEKKA_CONFIG_H
#define SEKKA_CONFIG_H

#include <string>
#include <string_view>

#include <fcitx-config/configuration.h>
#include <fcitx-config/enum.h>
#include <fcitx-config/option.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/i18nstring.h>
#include <fcitx-utils/key.h>

namespace fcitx {

/// Configuration items of the Sekka input method
FCITX_CONFIGURATION(
    SekkaConfig,
    /// Path of the dictionary file. Empty (the default) means: search the standard fcitx5
    /// data directories for sekka/master-dict.db - $XDG_DATA_HOME/fcitx5 first, then the
    /// fcitx5 directories under $XDG_DATA_DIRS (PATH-01). A path entered here is used as it
    /// is, without searching (PATH-04 / D-126); if it fails to load, Sekka does not fall
    /// back to searching. A value that exactly matches `kLegacyDefaultDictionaryPath`
    /// below (the v1.0 fixed default) is read back as empty by
    /// `applyLegacyDictionaryPathDefault` (D-121~D-123), so environments that still carry
    /// that string in `sekka.conf` switch to the search on their next config load.
    fcitx::OptionWithAnnotation<std::string, fcitx::ToolTipAnnotation> dictionaryPath{
        this, "DictionaryPath", _("Dictionary path"), "", {}, {},
        fcitx::ToolTipAnnotation(_(
            "Leave empty to find sekka/master-dict.db automatically in the fcitx5 data "
            "directories ($XDG_DATA_HOME/fcitx5 first, then the fcitx5 directories under "
            "$XDG_DATA_DIRS). A path entered here is used as it is, without searching."))};
    /// Path of the user dictionary. Empty (the default) means: use the fcitx5 user data
    /// directory - $XDG_DATA_HOME/fcitx5/sekka/user-dict.db, or
    /// ~/.local/share/fcitx5/sekka/user-dict.db when $XDG_DATA_HOME is not set (D-128: this
    /// has been the default behavior since v1.0, but v1.0 never explained it in the UI).
    fcitx::OptionWithAnnotation<std::string, fcitx::ToolTipAnnotation> userDictionaryPath{
        this, "UserDictionaryPath", _("User dictionary path"), "", {}, {},
        fcitx::ToolTipAnnotation(_(
            "Leave empty to use the default location, sekka/user-dict.db in the fcitx5 "
            "user data directory ($XDG_DATA_HOME/fcitx5, or ~/.local/share/fcitx5 when "
            "XDG_DATA_HOME is not set)."))};
    /// The conversion trigger key (default: Ctrl+J).
    fcitx::Option<fcitx::KeyList> triggerKey{
        this, "TriggerKey", _("Conversion key"),
        {fcitx::Key("Control+j")}};);

// D-121~D-123: the v1.0 fixed default for `DictionaryPath`. Kept only as a comparison
// target for `isLegacyDefaultDictionaryPath`/`applyLegacyDictionaryPathDefault` below -
// never used as a default value itself any more. This is the only place in
// `fcitx5-sekka/src` where this literal string appears; every other reference in this
// codebase goes through this constant by name, never by repeating the string.
inline constexpr std::string_view kLegacyDefaultDictionaryPath = "/usr/share/fcitx5/sekka/master-dict.db";

// D-121: exact match only, no normalization. Trimming whitespace, collapsing a doubled
// slash, folding case, or treating a differently-prefixed or suffixed path as "close
// enough" would all treat some user's deliberately-chosen explicit path as if it were
// unset - PATH-04 promises an explicit path is used as-is, and a near-miss match would
// silently hide a typo instead of surfacing it. Ordinary distro users who upgrade from
// v1.0 get the benefit of `XDG_DATA_HOME` priority (PATH-03) precisely because this
// comparison is exact: nothing here inspects whether the path actually exists on disk.
inline bool isLegacyDefaultDictionaryPath(std::string_view path) {
    return path == kLegacyDefaultDictionaryPath;
}

// D-122: rewrites `config.dictionaryPath` to the empty string in memory only, when (and
// only when) its current value exactly matches the v1.0 default. Returns true when it made
// the replacement, false otherwise (including when called again on an already-empty
// value - the second call is a no-op). This function never touches `sekka.conf` itself;
// the caller decides whether/when to persist, and `readSekkaConfig()` in sekka.cpp
// deliberately never does. That keeps the configuration UI showing blank (auto-search)
// right after the rewrite, and blank is exactly what gets written the next time the user
// (or configtool) saves - no separate migration step, no file rewritten behind the user's
// back on every startup.
inline bool applyLegacyDictionaryPathDefault(SekkaConfig &config) {
    if (!isLegacyDefaultDictionaryPath(config.dictionaryPath.value())) {
        return false;
    }
    config.dictionaryPath.setValue(std::string());
    return true;
}

} // namespace fcitx

#endif // SEKKA_CONFIG_H
