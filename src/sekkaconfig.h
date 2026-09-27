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
    /// Path of the dictionary file.
    fcitx::Option<std::string> dictionaryPath{
        this, "DictionaryPath", _("Dictionary path"),
        "/usr/share/fcitx5/sekka/master-dict.db"};
    /// Path of the user dictionary.
    fcitx::Option<std::string> userDictionaryPath{
        this, "UserDictionaryPath", _("User dictionary path"), ""};
    /// The conversion trigger key (default: Ctrl+J).
    fcitx::Option<fcitx::KeyList> triggerKey{
        this, "TriggerKey", _("Conversion key"),
        {fcitx::Key("Control+j")}};);

// RED (06-03 Task 1): the v1.0 default dictionary path, and stub declarations of the
// D-121~D-123 legacy-default comparison/rewrite so that config_test.cpp compiles and
// fails on real assertions rather than a compile error. GREEN (step 2 of this task)
// replaces the stub bodies below and the FCITX_CONFIGURATION block above.
inline constexpr std::string_view kLegacyDefaultDictionaryPath =
    "/usr/share/fcitx5/sekka/master-dict.db";

inline bool isLegacyDefaultDictionaryPath(std::string_view /*path*/) {
    return false; // stub: never matches yet.
}

inline bool applyLegacyDictionaryPathDefault(SekkaConfig & /*config*/) {
    return false; // stub: never rewrites yet.
}

} // namespace fcitx

#endif // SEKKA_CONFIG_H
