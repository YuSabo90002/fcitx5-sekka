// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// sekkadictpath.cpp - master/user dictionary path resolution through fcitx::StandardPaths
//
// RED-phase stub (06-01 Task 1): these bodies are intentionally incomplete placeholders so
// that `SekkaDictPathTest.*` fails on its assertions before the real implementation lands.
#include "sekkadictpath.h"

namespace fcitx {

MasterDictionaryCandidates
resolveMasterDictionaryCandidates(const std::string & /*configuredPath*/,
                                   const StandardPathsProvider & /*paths*/) {
    // Stub: never distinguishes explicit vs. search mode, so both RED tests fail.
    return {};
}

DictCandidateOutcome dictCandidateOutcomeFromError(int /*sekkaDictError*/) {
    // Stub: always reports the same outcome.
    return DictCandidateOutcome::Other;
}

DictCandidateWalkResult
walkDictionaryCandidates(const std::vector<std::filesystem::path> & /*candidates*/,
                          const DictCandidateLoader & /*tryLoad*/) {
    // Stub: never attempts anything, so `adopted` is always empty.
    return {};
}

} // namespace fcitx
