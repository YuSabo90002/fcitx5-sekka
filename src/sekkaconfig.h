// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// sekkaconfig.h - Sekka configuration definitions
// Uses fcitx5's configuration framework (FCITX_CONFIGURATION)
#ifndef SEKKA_CONFIG_H
#define SEKKA_CONFIG_H

#include <fcitx-config/configuration.h>
#include <fcitx-config/enum.h>
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

} // namespace fcitx

#endif // SEKKA_CONFIG_H
