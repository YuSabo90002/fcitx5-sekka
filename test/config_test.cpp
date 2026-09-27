// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// config_test.cpp - GoogleTest for the D-121..D-123 legacy default rewrite and the D-128
// tooltips, verified against `SekkaConfig` and its `RawConfig` representation only. Touches
// neither the filesystem nor fcitx5 itself.

#include <gtest/gtest.h>

#include "sekkaconfig.h"

#include <fcitx-config/iniparser.h>
#include <fcitx-config/rawconfig.h>

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

TEST(SekkaConfigTest, DictionaryPathDefaultsToEmptyAndDiffersFromTheLegacyPath) {
    fcitx::SekkaConfig config;
    EXPECT_EQ(config.dictionaryPath.value(), "");
    EXPECT_TRUE(config.dictionaryPath.isDefault());
    EXPECT_NE(config.dictionaryPath.value(),
              std::string(fcitx::kLegacyDefaultDictionaryPath));
}

TEST(SekkaConfigTest, UserDictionaryPathDefaultsToEmpty) {
    fcitx::SekkaConfig config;
    EXPECT_EQ(config.userDictionaryPath.value(), "");
}

TEST(SekkaConfigTest, LegacyDefaultIsReadAsEmpty) {
    fcitx::RawConfig raw;
    raw.setValueByPath("DictionaryPath",
                       std::string(fcitx::kLegacyDefaultDictionaryPath));
    fcitx::SekkaConfig config;
    config.load(raw);
    // load() alone must not perform the rewrite - only applyLegacyDictionaryPathDefault()
    // does (D-121/D-123: the two are separate steps).
    EXPECT_EQ(config.dictionaryPath.value(),
              std::string(fcitx::kLegacyDefaultDictionaryPath));

    EXPECT_TRUE(fcitx::applyLegacyDictionaryPathDefault(config));
    EXPECT_EQ(config.dictionaryPath.value(), "");
    EXPECT_TRUE(config.dictionaryPath.isDefault());
}

TEST(SekkaConfigTest, NearMissPathsAreKept) {
    const std::string legacy(fcitx::kLegacyDefaultDictionaryPath);
    const std::vector<std::string> nearMisses = {
        legacy + "/",                                        // trailing slash
        legacy + " ",                                         // trailing whitespace
        " " + legacy,                                         // leading whitespace
        "/usr/share/fcitx5/sekka//master-dict.db",            // double slash
        "/usr/local/share/fcitx5/sekka/master-dict.db",       // different prefix
        legacy + ".bak",                                      // suffix
        "/USR/share/fcitx5/sekka/master-dict.db",             // case difference
        std::string(),                                        // empty string
    };
    for (const auto &value : nearMisses) {
        SCOPED_TRACE(value);
        fcitx::SekkaConfig config;
        config.dictionaryPath.setValue(value);
        EXPECT_FALSE(fcitx::applyLegacyDictionaryPathDefault(config));
        EXPECT_EQ(config.dictionaryPath.value(), value);
    }
}

TEST(SekkaConfigTest, ApplyingTwiceIsANoOp) {
    fcitx::SekkaConfig config;
    config.dictionaryPath.setValue(std::string(fcitx::kLegacyDefaultDictionaryPath));

    ASSERT_TRUE(fcitx::applyLegacyDictionaryPathDefault(config));
    EXPECT_EQ(config.dictionaryPath.value(), "");

    // Second call: the value is already "" (not the legacy default any more), so it must
    // report no-op and leave the value untouched.
    EXPECT_FALSE(fcitx::applyLegacyDictionaryPathDefault(config));
    EXPECT_EQ(config.dictionaryPath.value(), "");
}

TEST(SekkaConfigTest, LegacyMappingIsSavedAsEmptyOnTheNextSave) {
    fcitx::SekkaConfig config;
    config.dictionaryPath.setValue(std::string(fcitx::kLegacyDefaultDictionaryPath));
    ASSERT_TRUE(fcitx::applyLegacyDictionaryPathDefault(config));

    fcitx::RawConfig out;
    config.save(out);

    const std::string *saved = out.valueByPath("DictionaryPath");
    ASSERT_NE(saved, nullptr);
    EXPECT_EQ(*saved, "");

    std::ostringstream stream;
    ASSERT_TRUE(fcitx::writeAsIni(out, stream));
    const std::string ini = stream.str();

    // D-121 premise note (06-03-PLAN.md <objective>): record the exact line fcitx5 5.1.16
    // writes for a `DictionaryPath` that equals its own default value, so the SUMMARY can
    // report whether this fcitx5 writes it as an active `DictionaryPath=` line or comments
    // it out as `# DictionaryPath=` (06-RESEARCH.md Pitfall 4's correction is about a newer
    // fcitx5 than this development machine's 5.1.16).
    std::istringstream iniStream(ini);
    std::string line;
    std::string dictionaryPathLine;
    while (std::getline(iniStream, line)) {
        if (line.find("DictionaryPath") != std::string::npos) {
            dictionaryPathLine = line;
            break;
        }
    }
    RecordProperty("DictionaryPathLineInWrittenIni", dictionaryPathLine.c_str());

    // Whether active or commented out, the legacy path string itself must never appear -
    // the rewritten value is always the empty string, never the old fixed path.
    EXPECT_EQ(ini.find(std::string(fcitx::kLegacyDefaultDictionaryPath)), std::string::npos);
}

TEST(SekkaConfigTest, PathOptionsHaveTooltipsAndKeepTheirLabels) {
    fcitx::SekkaConfig config;
    fcitx::RawConfig desc;
    config.dumpDescription(desc);

    const std::string *dictionaryTooltip =
        desc.valueByPath("SekkaConfig/DictionaryPath/Tooltip");
    ASSERT_NE(dictionaryTooltip, nullptr);
    EXPECT_FALSE(dictionaryTooltip->empty());
    EXPECT_NE(dictionaryTooltip->find("sekka/master-dict.db"), std::string::npos);

    const std::string *userDictionaryTooltip =
        desc.valueByPath("SekkaConfig/UserDictionaryPath/Tooltip");
    ASSERT_NE(userDictionaryTooltip, nullptr);
    EXPECT_FALSE(userDictionaryTooltip->empty());
    EXPECT_NE(userDictionaryTooltip->find("sekka/user-dict.db"), std::string::npos);

    const std::string *dictionaryDescription =
        desc.valueByPath("SekkaConfig/DictionaryPath/Description");
    ASSERT_NE(dictionaryDescription, nullptr);
    EXPECT_EQ(*dictionaryDescription, "Dictionary path");

    const std::string *userDictionaryDescription =
        desc.valueByPath("SekkaConfig/UserDictionaryPath/Description");
    ASSERT_NE(userDictionaryDescription, nullptr);
    EXPECT_EQ(*userDictionaryDescription, "User dictionary path");

    // TriggerKey carries no ToolTipAnnotation - its dumpDescription() must not write a
    // Tooltip key at all.
    const std::string *triggerTooltip =
        desc.valueByPath("SekkaConfig/TriggerKey/Tooltip");
    EXPECT_EQ(triggerTooltip, nullptr);
}
