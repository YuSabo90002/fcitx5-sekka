// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// sekka.cpp - implementation of the Sekka engine and context
#include "sekka.h"

#include "focuspolicy.h"
#include "sekkacandidatelist.h"
#include "sekkadictpath.h"

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/standardpaths.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/userinterfacemanager.h>

#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace fcitx {

namespace {

// D-122 / 06-RESEARCH.md Pattern 3 & Pitfall 3: the single entry point that (re)builds
// `config_` from `conf/sekka.conf`. `SekkaEngine`'s constructor does not go through
// `reloadConfig()` - it always read the file itself inline - so before this function
// existed there were two independent call sites that each had to remember to apply the
// D-121~D-123 legacy-default rewrite; missing either one meant the rewrite worked at
// startup but not on reload, or vice versa. Routing both the constructor and
// `reloadConfig()` through this one function instead makes that impossible: the rewrite is
// applied every time `config_` is repopulated from the file, with no separate call to
// forget. `setConfig()` is unaffected by this consolidation - it already ends in
// `reloadConfig()` (see below), so it goes through this function too, but it never calls
// `fcitx::readAsIni` itself and this function never writes to `sekka.conf` (D-122: the
// rewrite lives in memory only; nothing here calls `safeSaveAsIni`).
void readSekkaConfig(SekkaConfig &config) {
    fcitx::readAsIni(config, "conf/sekka.conf");
    if (applyLegacyDictionaryPathDefault(config)) {
        FCITX_INFO() << "DictionaryPath was the v1.0 default (" << kLegacyDefaultDictionaryPath
                     << "); treating it as unset and searching the standard data "
                        "directories instead. sekka.conf is not rewritten.";
    }
}

} // namespace

// === SekkaState implementation ===

SekkaState::SekkaState(SekkaEngine *engine, InputContext &ic)
    : engine_(engine), ic_(ic), ctx_(sekka_context_new()) {
    if (ctx_) {
        engine_->applyDictionaries(ctx_);
    }
}

SekkaState::~SekkaState() {
    if (ctx_) {
        sekka_context_free(ctx_);
        ctx_ = nullptr;
    }
}

void SekkaState::keyEvent(KeyEvent &event) {
    if (!ctx_) {
        return;
    }

    // Ignore key releases.
    if (event.isRelease()) {
        return;
    }

    auto key = event.key();

    // A lone modifier key press is not even passed to libsekka (libsekka would also
    // return 0, but it is simply not passed at all; D-03).
    if (key.isModifier()) {
        return;
    }

    uint32_t keysym = key.sym();
    uint32_t modifiers = 0;

    // Translate the Ctrl/Alt/Super modifiers (the same bit values as fcitx5's KeyState).
    if (key.states().test(KeyState::Ctrl)) {
        modifiers |= 0x4; // the Ctrl bit
    }
    if (key.states().test(KeyState::Alt)) {
        modifiers |= 0x8; // the Alt bit
    }
    if (key.states().test(KeyState::Super)) {
        modifiers |= 0x40; // the Super bit
    }

    // D-185/D-186 (Phase 11): `KeyEvent::key()` is `rawKey().normalize()`, and
    // `Key::normalize()` (fcitx-utils) folds a lowercase letter to uppercase whenever a
    // modifier other than Shift is held, so the key of Alt+d reads as Alt+D; committing
    // that would turn Vim's `dd` into `DD`. `rawKey()` is the key after keyboard-layout
    // conversion and before normalization, and its keysym already carries Shift and
    // Caps Lock as the layout produced them (Alt+Shift+d gives `D`, Alt+Shift+; gives
    // `colon`), which is exactly the character D-186 wants. Keys held with Ctrl or
    // Super keep the normalized keysym as before (libsekka's `ctrl_letter` accepts
    // both cases).
    if ((modifiers & 0x8) && !(modifiers & (0x4 | 0x40))) {
        keysym = event.rawKey().sym();
    }

    // D-108/D-111: the trigger key decision happens here (after the early return for
    // isModifier() and before anything is passed to libsekka). The trigger is checked
    // first, so assigning TriggerKey to an ordinary key that would otherwise land in the
    // romaji buffer (`space` on its own, say) is not rejected and does work as the trigger
    // (D-111). Keys that do not match flow to `sekka_context_process_key_event` as before.
    // That else branch is the one existing call path, and neither the signature nor the way
    // `sekka_context_process_key_event` is called changes (D-110).
    //
    // `normalize()` on both sides (a mandatory pre-step, discovered while debugging on a
    // real machine): the documentation comment of `Key::normalize()`
    // (`fcitx-utils/key.h:150-157`) explains that "Shift + a-z folds to A-Z, so keys in the
    // config need not care about case", but real-machine logs showed that making it work
    // requires normalizing **both** sides: the raw key of `Ctrl+j` arriving from the
    // frontend is already normalized - `Key::toString()` prints it as "Control+J"
    // (uppercase) - whereas `config().triggerKey.value()` keeps the case as written in the
    // ini file (both the default and the `sekka.conf` in this repository use the lowercase
    // "Control+j"). `Key::check()` is an exact match, so the uppercase "Control+J" and the
    // lowercase "Control+j" do not match (`check()==0` was confirmed in a real-machine
    // log). Folding the config side to uppercase with `tk.normalize()` makes them match
    // (`check()==1`). `keysym`/`modifiers` (computed above and passed through to the
    // non-trigger path) stay at their raw values - normalization is used only for the
    // decision.
    int consumed;
    const auto &configuredTriggerKeys = engine_->config().triggerKey.value();
    std::vector<Key> normalizedTriggerKeys;
    normalizedTriggerKeys.reserve(configuredTriggerKeys.size());
    for (const auto &configuredKey : configuredTriggerKeys) {
        normalizedTriggerKeys.push_back(configuredKey.normalize());
    }
    if (key.normalize().checkKeyList(normalizedTriggerKeys)) {
        consumed = sekka_context_trigger(ctx_);
    } else {
        consumed =
            sekka_context_process_key_event(ctx_, keysym, modifiers, 0);
    }

    if (consumed) {
        event.filterAndAccept();
        checkAndCommit();
        updatePreedit();
        // D-158 (Phase 9) already turns printable keys (0x20-0x7E, no Ctrl/Alt/Super) into
        // input libsekka consumes and appends to the committed text, so those never reach
        // this point any more. What is forwarded here is only: a non-printable key that
        // committed the romaji or the on-screen candidate as it is (D-03), an "other key"
        // not in D-09's reselection table, and any key held with Ctrl/Alt/Super (D-162). A
        // BackSpace after conversion is never forwarded (D-160/D-161) - it reverts to the
        // raw romaji instead.
        //
        // D-185 (Phase 11): a printable key held with Alt alone is also consumed and
        // committed as text with the Alt removed, so what is forwarded here is now only:
        // a non-printable key (Alt held or not, D-187), an "other key" not in D-09's
        // reselection table after the candidate is committed, and a key held with Ctrl
        // or Super (Alt or not, D-188).
        //
        // A 2026-09-27 real-machine log (D-155) confirmed only that fcitx5's own send
        // order to the Wayland compositor - commit_string, then commit(), then the
        // forwarded key - held in every case examined; a follow-up retest found the
        // earlier "commit lost" symptom reproduces only inside Claude Code's Alacritty
        // chat input, not with cat/nano/gedit, so this comment no longer assumes that
        // fcitx5's send order guarantees the client has finished applying the committed
        // string before the forwarded key arrives.
        if (sekka_context_take_forward_key(ctx_)) {
            ic_.forwardKey(event.rawKey(), false);
        }
    }
}

void SekkaState::reset(bool commitPending) {
    // D-14/D-15: the rule for handling uncommitted text splits symmetrically depending on
    // whether this was called for a focus change, an input method switch or an explicit
    // reset.
    // Only when `commitPending` is true (D-15: an input method switch within the same
    // application, or an explicit reset) are the unsent staged candidate and the candidate
    // selected during reselection reflected into the committed output before the state is
    // cleared.
    // When `commitPending` is false (D-14: a real focus loss, where the destination could
    // be another application) nothing is reflected into the committed output; the state is
    // discarded and only the preedit is cleared. This is the design that avoids the
    // upstream fcitx5 bug where commitStringDelegate ignores its IC argument (G-01.1-4) by
    // never calling commitString in the first place.
    // Clearing the libsekka-side context itself and resetting the panel always happen,
    // whether or not anything was committed.
    // During word registration (Phase 10), an explicit reset or an input method switch
    // commits only the outermost reading that the application shows (D-181), and a real
    // focus loss discards every registration step without committing anything (D-180);
    // the click path of the candidate window keeps using sekka_context_confirm_candidate.
    if (ctx_) {
        if (commitPending) {
            sekka_context_finalize_for_reset(ctx_);
            checkAndCommit();
        }
        sekka_context_reset(ctx_);
    }
    ic_.inputPanel().reset();
    ic_.updatePreedit();
    ic_.updateUserInterface(UserInterfaceComponent::InputPanel);
}

void SekkaState::updatePreedit() {
    if (!ctx_) {
        return;
    }

    // Phase 10 (D-170): while registering, the client preedit and the popup
    // are built together instead of either/or - delegate entirely.
    if (sekka_context_is_registering(ctx_)) {
        updateRegistrationPanel();
        return;
    }

    char *preeditStr = sekka_context_get_preedit(ctx_);
    if (preeditStr) {
        auto &inputPanel = ic_.inputPanel();
        inputPanel.reset();

        std::string preedit(preeditStr);
        sekka_free_string(preeditStr);

        if (!preedit.empty()) {
            Text preeditText;
            preeditText.append(preedit);
            preeditText.setCursor(preedit.size());
            if (ic_.capabilityFlags().test(CapabilityFlag::Preedit)) {
                ic_.inputPanel().setClientPreedit(preeditText);
            } else {
                ic_.inputPanel().setPreedit(preeditText);
            }
        }

        attachCandidateList();

        ic_.updatePreedit();
        ic_.updateUserInterface(UserInterfaceComponent::InputPanel);
    }
}

void SekkaState::attachCandidateList() {
    // The candidate window is shown only during reselection (candidate count > 0) (D-04/D-08).
    if (sekka_context_get_candidate_count(ctx_) > 0) {
        // D-116: this callback runs after a click commit of a candidate
        // (SekkaCandidateWord::select() -> selectAt()). Calling the same two steps in
        // the same order as after `consumed` in keyEvent() (around lines 78-79 of this
        // file) keeps the output path of a click commit identical to the output path of
        // key input.
        ic_.inputPanel().setCandidateList(
            std::make_unique<SekkaCandidateList>(
                ctx_, [this] {
                    checkAndCommit();
                    updatePreedit();
                }));
    }
}

void SekkaState::updateRegistrationPanel() {
    auto &inputPanel = ic_.inputPanel();
    inputPanel.reset();

    // D-170: the application's own input position (client preedit) shows
    // only the outermost step's reading, underlined and with no label and no
    // in-progress word - `setCursor` is deliberately not called here, so the
    // text cursor stays on the popup side rather than jumping to the client
    // preedit. The user's input history never reaches a log (D-102/D-105):
    // these three getters are only ever handed to `Text`/`std::string`, never
    // printed.
    char *readingStr = sekka_context_get_registration_reading(ctx_);
    std::string reading(readingStr ? readingStr : "");
    if (readingStr) {
        sekka_free_string(readingStr);
    }

    char *promptStr = sekka_context_get_registration_prompt(ctx_);
    std::string prompt(promptStr ? promptStr : "");
    if (promptStr) {
        sekka_free_string(promptStr);
    }

    char *wordStr = sekka_context_get_preedit(ctx_);
    std::string word(wordStr ? wordStr : "");
    if (wordStr) {
        sekka_free_string(wordStr);
    }

    if (ic_.capabilityFlags().test(CapabilityFlag::Preedit)) {
        Text clientPreeditText;
        clientPreeditText.append(reading, TextFormatFlag::Underline);
        ic_.inputPanel().setClientPreedit(clientPreeditText);

        Text auxUpText;
        auxUpText.append(prompt);
        ic_.inputPanel().setAuxUp(auxUpText);

        Text wordText;
        wordText.append(word);
        wordText.setCursor(word.size());
        ic_.inputPanel().setPreedit(wordText);
    } else {
        // D-170 Claude's Discretion: without client-preedit support, the
        // reading has nowhere of its own to go, so it is folded into auxUp
        // ahead of the label/prompt instead of being dropped.
        Text auxUpText;
        auxUpText.append(reading + " " + prompt);
        ic_.inputPanel().setAuxUp(auxUpText);

        Text wordText;
        wordText.append(word);
        wordText.setCursor(word.size());
        ic_.inputPanel().setPreedit(wordText);
    }

    attachCandidateList();

    ic_.updatePreedit();
    ic_.updateUserInterface(UserInterfaceComponent::InputPanel);
}

void SekkaState::checkAndCommit() {
    if (!ctx_) {
        return;
    }

    char *output = sekka_context_poll_output(ctx_);
    if (output) {
        std::string commitStr(output);
        sekka_free_string(output);

        if (!commitStr.empty()) {
            ic_.commitString(commitStr);
        }
        // The panel is neither reset nor is the preedit updated here: the updatePreedit()
        // called right afterwards always rebuilds the panel.
    }
}

// === SekkaEngine implementation ===

SekkaEngine::SekkaEngine(Instance *instance)
    : instance_(instance),
      factory_(
          [this](InputContext &ic) { return new SekkaState(this, ic); }) {
    // Registering the factory creates the SekkaState of any existing input context, so the
    // configuration and the dictionaries are loaded first.
    readSekkaConfig(config_);
    openDictionaries();
    instance_->inputContextManager().registerProperty("sekkaState",
                                                       &factory_);
}

SekkaEngine::~SekkaEngine() { closeDictionaries(); }

void SekkaEngine::activate(const InputMethodEntry & /*entry*/,
                            InputContextEvent &event) {
    auto *ic = event.inputContext();
    auto *state = ic->propertyFor(&factory_);
    if (state) {
        // Right after focus-in there is no pending state to commit, so this is always
        // false (outside the scope of D-14/D-15).
        state->reset(false);
    }
}

void SekkaEngine::deactivate(const InputMethodEntry & /*entry*/,
                              InputContextEvent &event) {
    // For observing D-11: records which focus event type deactivate was called for. It
    // never prints anything the user typed (romaji, candidates or committed text).
    FCITX_DEBUG() << "SekkaEngine::deactivate event.type()="
                  << static_cast<int>(event.type());
    auto *ic = event.inputContext();
    auto *state = ic->propertyFor(&factory_);
    if (state) {
        // D-14/D-15: event.type() distinguishes a real focus loss (do not commit) from an
        // input method switch within the same application (do commit). For an unexpected
        // EventType, commitPendingOnDeactivate falls back to the safe side.
        state->reset(commitPendingOnDeactivate(event.type()));
    }
}

void SekkaEngine::keyEvent(const InputMethodEntry & /*entry*/,
                            KeyEvent &event) {
    auto *ic = event.inputContext();
    auto *state = ic->propertyFor(&factory_);
    if (state) {
        state->keyEvent(event);
    }
}

void SekkaEngine::reset(const InputMethodEntry & /*entry*/,
                         InputContextEvent &event) {
    // For observing D-11: records which focus event type reset was called for. This log
    // exists to confirm on a real machine whether an in-application click (RESEARCH.md Open
    // Questions Q1) reaches here, and never prints anything the user typed.
    FCITX_DEBUG() << "SekkaEngine::reset event.type()="
                  << static_cast<int>(event.type());
    auto *ic = event.inputContext();
    auto *state = ic->propertyFor(&factory_);
    if (state) {
        // For an explicit reset (InputContextReset) the fcitx5 core guarantees
        // ic->hasFocus(), so the destination is always the same application. It is treated
        // like D-15 and always commits.
        state->reset(true);
    }
}

void SekkaEngine::reloadConfig() {
    readSekkaConfig(config_);
    reloadDictionaries();
}

// D-104: takes in the `RawConfig` delivered by `Controller1.SetConfig` and merges it into
// the existing `reloadConfig()` path (save to file plus `reloadDictionaries()`). No new
// reset path and no new save path are added - both "applying a dictionary path change
// without a restart" and the `reset(true)` discipline (commit, then clear; D-15) that
// `reloadDictionaries()` goes through keep working through this existing path.
//
// The second argument `partial=true` of `config_.load(config, true)` exists so that the
// E2E tests can send a single item while configtool sends the full configuration
// (unspecified items keep their current values).
void SekkaEngine::setConfig(const RawConfig &config) {
    config_.load(config, true);
    if (!fcitx::safeSaveAsIni(config_, "conf/sekka.conf")) {
        // The same discipline as D-102: readings, words and candidate lists are never
        // printed; only the path goes into the warning log. A save failure does not stop
        // input.
        FCITX_WARN() << "failed to save the Sekka configuration: conf/sekka.conf";
    }
    reloadConfig();
}

// D-98 (2) (the save() hook only): writes the user dictionary to disk whenever the fcitx5
// core calls AddonInstance::save() - on shutdown, on the idle save timer (the default
// AutoSavePeriod is 30 minutes) or on an explicit save request over D-Bus. sled already
// applies the default flush_every_ms (500 ms) inside UserDict::open and flushes when the Db
// is dropped, so this hook adds nothing to durability itself (04-SAVE-TIMING.md). What it
// does add is explicit timing control and observability of save failures (D-102).
//
// D-102: even for a non-zero return value it throws nothing, only writes a warning log, and
// never stops input (it blocks neither the commit, the conversion nor the preedit path;
// D-35). Notifying the user (the notifications addon integration) is the job of FR-009 /
// Phase 5 (D-65) and is not done here. The master dictionary is read-only and is therefore
// not saved (calling it would be meaningless and would only add another call path).
void SekkaEngine::save() {
    if (!userDict_) {
        return;
    }
    int rc = sekka_dictionary_save(userDict_);
    if (rc != 0) {
        // Nothing from the user's input history (readings, words, candidate lists) goes in.
        // Only the dictionary path and the return value go into the warning log.
        FCITX_WARN() << "failed to save the user dictionary: " << userDictPath_
                     << " (code=" << rc << ")";
    }
}

void SekkaEngine::reloadDictionaries() {
    auto &icManager = instance_->inputContextManager();

    // sled cannot open the same dictionary twice, so every context's reference is dropped
    // before it is closed.
    //
    // reset(true) must always run before dropping them (01.2-REVIEW WR-03 / 01.4-REVIEW
    // CR-01). `sekka_context_set_dictionaries` calls `SekkaContext::reset()` internally and
    // unconditionally discards both the unsent staged candidate (the word Ctrl-J placed in
    // the preedit) and the romaji buffer. A word being typed in another window would be lost
    // without a sound the moment a dictionary path is saved in the settings dialog, so the
    // same discipline as D-15 (an explicit state change within the same application, so
    // commit and then clear) applies here too. The discarding side (D-14) is not used
    // because saving settings is an explicit user action rather than a focus loss, and the
    // destination input context has not changed to another application.
    icManager.foreach([this](InputContext *ic) {
        auto *state = ic->propertyFor(&factory_);
        if (state) {
            state->reset(true);
            sekka_context_set_dictionaries(state->context(), nullptr, 0);
        }
        return true;
    });
    closeDictionaries();
    openDictionaries();
    icManager.foreach([this](InputContext *ic) {
        auto *state = ic->propertyFor(&factory_);
        if (state) {
            applyDictionaries(state->context());
        }
        return true;
    });
}

// D-105 / FR-009: tells the user through the notifications addon that one or more
// dictionaries failed to load. Called from openDictionaries() for the master dictionary
// domain, which can produce several notices in one call - one per skipped candidate
// (D-124/D-125), plus the D-127 "not found in the search locations" summary - since 06-02
// replaced the old "one candidate = one notification" pipeline with
// `buildMasterDictionaryNotices()`. `notifyDictionaryError()` (below) is the single-notice
// version used by the user dictionary domain, implemented as a one-element-batch delegation
// to this function so its dedup key stays byte-identical to v1.0's.
//
// - a failure to notify must never stop input: it throws nothing and ignores the return
//   value (the notification id) (the "a failure does not stop input" discipline of D-105 /
//   D-35 / D-102)
// - the text never contains the user's input history: only the dictionary path and the kind
//   of failure (the discipline of sekka.cpp:262-264 applies here too), enforced by
//   `buildMasterDictionaryNotices()`'s narrow input type
// - notifications is an optional dependency and notifications() can be null (when the
//   notifications addon is disabled or failed to load). When it is null, nothing happens and
//   only the existing FCITX_WARN remains, and `dedupSlot` is left untouched (matching v1.0)
// - an empty `notices` batch means that domain's load was clean this time (05-REVIEW CR-01's
//   "success clears the slot"), so `dedupSlot` is cleared and nothing is sent
// - deduplication compares the WHOLE BATCH's key (`dictionaryNoticeDedupKey`) against
//   `dedupSlot`, so notifications do not pile up when reloadDictionaries() is called
//   repeatedly with the same configuration (06-CONTEXT.md Claude's Discretion), while a
//   batch that changes in content, count, or order is always sent in full. The caller
//   passes the `dedupSlot` of its own domain, master dictionary or user dictionary, so the
//   suppression is independent per domain (05-REVIEW CR-01: with a single shared field, a
//   success in one domain would wipe out the suppression state of a failure in the other,
//   turning into a bug where the notification for the same failure is resent on every
//   unrelated settings change)
void SekkaEngine::notifyDictionaryErrors(std::string &dedupSlot,
                                          const std::vector<DictionaryErrorNotice> &notices) {
    if (notices.empty()) {
        dedupSlot.clear();
        return;
    }
    auto key = dictionaryNoticeDedupKey(notices);
    if (key == dedupSlot) {
        return;
    }
    auto *n = notifications();
    if (!n) {
        return;
    }
    for (const auto &notice : notices) {
        n->call<fcitx::INotifications::sendNotification>(
            "sekka", 0, "dialog-error", _("Sekka: failed to load a dictionary"),
            notice.body, std::vector<std::string>{}, 5000,
            fcitx::NotificationActionCallback{}, fcitx::NotificationClosedCallback{});
    }
    dedupSlot = key;
}

// Single-notice version, used by the user dictionary domain (which never batches). Its
// body is a delegation to `notifyDictionaryErrors()` with a one-element batch, so its dedup
// key is `path + "\n" + body`, byte-identical to the pre-06-02 key computed here directly.
void SekkaEngine::notifyDictionaryError(std::string &dedupSlot,
                                         const std::string &path,
                                         const std::string &body) {
    notifyDictionaryErrors(dedupSlot, {DictionaryErrorNotice{path, body}});
}

void SekkaEngine::openDictionaries() {
    // Load the master dictionary.
    //
    // D-124/PATH-01/PATH-02: resolves either the single explicit `DictionaryPath` (D-126 /
    // PATH-04, no StandardPaths lookup at all) or the full ordered candidate list from
    // `fcitx::StandardPaths::global()`'s PkgData search (user directory -> each
    // XDG_DATA_DIRS prefix -> fcitx5's own built-in pkgdatadir). Passing the global
    // singleton in production and a local instance in tests is the seam
    // `resolveMasterDictionaryCandidates` takes a `StandardPathsProvider` for
    // (06-RESEARCH.md Pitfall 5: the global singleton reads environment variables only once
    // per process).
    auto resolved = resolveMasterDictionaryCandidates(config_.dictionaryPath.value(),
                                                       &StandardPaths::global);

    // The loader captures `this` so a successful load lands directly in `masterDict_`. A
    // failed `sekka_file_dict_new_with_error` call always returns null, so overwriting
    // `masterDict_` on every attempt never leaks a handle from an earlier failed attempt;
    // the walk stops at the first `Loaded` outcome (D-125), so the final value is either
    // the one adopted dictionary or null.
    auto loader = [this](const std::filesystem::path &path) {
        int dictError = SEKKA_DICT_OK;
        masterDict_ =
            sekka_file_dict_new_with_error(path.c_str(), "UTF-8", &dictError);
        if (masterDict_) {
            return static_cast<int>(SEKKA_DICT_OK);
        }
        return dictError != SEKKA_DICT_OK ? dictError
                                          : static_cast<int>(SEKKA_DICT_ERROR_OTHER);
    };
    auto walk = walkDictionaryCandidates(resolved.candidates, loader);

    // D-64/SC6/CR-01/D-127: log (all candidates, full search-location list) and notify
    // (folded to `kMaxShownSearchLocations`) every non-adopted attempt with the exact v1.0
    // wording for its outcome (02.1-REVIEW CR-01's rejection text, and the three kinds of
    // D-64 plus the default), followed - only for a search that adopted nothing - by the
    // 06-02 "not found in the search locations" summary. The per-candidate judgment now
    // lives in `walkDictionaryCandidates` (06-01) and the message assembly in
    // `buildMasterDictionaryNotices` (06-02); this function only supplies the two sets of
    // text (translated for notifications, plain English for the log, matching v1.0
    // wording) and sends the results.
    //
    // Notification text (translated via `_()`; `po/ja.po` carries the Japanese
    // translations). `notFoundInSearchLocations` is new in 06-02 (D-127/PATH-06); the other
    // four reuse v1.0's exact notification wording so existing translations keep applying.
    MasterDictionaryNoticeTexts notifyTexts;
    notifyTexts.writableRefused =
        _("refused to load the master dictionary for safety because its path is writable "
          "(deploying it root-owned with mode 644 is recommended): ");
    notifyTexts.notFound = _("master dictionary not found: ");
    notifyTexts.unreadable = _("cannot open the master dictionary (check the permissions): ");
    notifyTexts.corrupt =
        _("the master dictionary is corrupt (invalid format or version): ");
    notifyTexts.other = _("failed to load the master dictionary: ");
    notifyTexts.notFoundInSearchLocations =
        _("master dictionary not found in the search locations: ");

    // Log text: v1.0's plain English (never translated - matches the pre-06-02 FCITX_WARN
    // wording exactly). `writableRefused` differs slightly from the notification wording
    // ("refused" -> "refusing"), matching v1.0's log line precisely.
    MasterDictionaryNoticeTexts logTexts;
    logTexts.writableRefused =
        "refusing to load the master dictionary for safety because its path is writable "
        "(deploying it root-owned with mode 644 is recommended): ";
    logTexts.notFound = "master dictionary not found: ";
    logTexts.unreadable = "cannot open the master dictionary (check the permissions): ";
    logTexts.corrupt = "the master dictionary is corrupt (invalid format or version): ";
    logTexts.other = "failed to load the master dictionary: ";
    logTexts.notFoundInSearchLocations =
        "master dictionary not found in the search locations: ";

    // Log every searched location in full (no folding) - only the notification is folded
    // to `kMaxShownSearchLocations` (D-127/06-CONTEXT.md Claude's Discretion).
    for (const auto &line :
         buildMasterDictionaryNotices(resolved, walk, logTexts,
                                       std::numeric_limits<std::size_t>::max())) {
        FCITX_WARN() << line.body;
    }
    if (walk.adopted) {
        FCITX_INFO() << "loaded the master dictionary: " << walk.adopted->string();
    }
    // 06-02: send the whole batch for this openDictionaries() call in one shot.
    // `notifyDictionaryErrors` clears `lastNotifiedMasterDictError_` itself when the batch
    // is empty (i.e. the load was clean - equivalent to 05-REVIEW CR-01's "success clears
    // the slot"), so there is no separate explicit clear here.
    notifyDictionaryErrors(lastNotifiedMasterDictError_,
                           buildMasterDictionaryNotices(resolved, walk, notifyTexts,
                                                         kMaxShownSearchLocations));

    // Load the user dictionary.
    //
    // USER-01/USER-02: when UserDictionaryPath is empty, the default is resolved entirely
    // through fcitx::StandardPaths (userDirectory(PkgData) / "sekka/user-dict.db"), which
    // follows XDG_DATA_HOME when it is set (USER-01) and is byte-identical to v1.0's
    // hand-built $HOME/.local/share/fcitx5/sekka/user-dict.db when it is not, so existing
    // learned data keeps loading (USER-02). An empty result (no HOME, no relevant user
    // directory, or a relative one - D-35) means no user dictionary, exactly like v1.0
    // without HOME: this block falls through with userDict_ left null and no notification
    // (v1.0 did not notify for this case either). Explicit (non-empty) values are read as-is
    // and never touch StandardPaths at all (USER-03). Migrating learned data from an old
    // default path is out of scope (REQUIREMENTS.md Out of Scope) - nothing here moves,
    // copies, or deletes a pre-existing user dictionary file.
    auto userDictPath = config_.userDictionaryPath.value();
    if (userDictPath.empty()) {
        userDictPath = defaultUserDictionaryPath(&StandardPaths::global).string();
    }
    if (!userDictPath.empty()) {
        // Create the directory when it does not exist. Uses the non-throwing overload
        // (D-35): a read-only XDG_DATA_HOME (or any other permission failure) must not let a
        // std::filesystem::filesystem_error escape the addon during initialization. On
        // failure, log and continue - the following sekka_user_dict_new() call fails on its
        // own and lands on the existing user-dictionary failure notification path below.
        auto parentDir = std::filesystem::path(userDictPath).parent_path();
        if (!parentDir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parentDir, ec);
            if (ec) {
                FCITX_WARN() << "failed to create the user dictionary directory: "
                             << parentDir.string() << " (" << ec.message() << ")";
            }
        }
        userDict_ = sekka_user_dict_new(userDictPath.c_str(), "UTF-8");
        if (!userDict_) {
            FCITX_WARN() << "failed to load the user dictionary: "
                         << userDictPath;
            // D-105/FR-009: put it on the same notification path as the master dictionary
            // (added by the planner, since a user dictionary failure is the same kind of
            // problem inside the same openDictionaries()). Because the domain differs, the
            // suppression slot is `lastNotifiedUserDictError_` rather than the master
            // dictionary's (05-REVIEW CR-01).
            notifyDictionaryError(lastNotifiedUserDictError_, userDictPath,
                                  _("failed to load the user dictionary: ") +
                                      userDictPath);
        } else {
            // Keep the path actually used, so the warning log of save() can say which file
            // failed to save (D-102).
            userDictPath_ = userDictPath;
            // 05-REVIEW CR-01: on a successful load, clear only the deduplication state of
            // the user dictionary domain itself (the same reason as on the master dictionary
            // side, but the slots are independent: a success on one side must not wipe the
            // suppression state of the other side's failure).
            lastNotifiedUserDictError_.clear();
        }
    }
}

void SekkaEngine::closeDictionaries() {
    if (masterDict_) {
        sekka_free_dictionary(masterDict_);
        masterDict_ = nullptr;
    }
    if (userDict_) {
        sekka_free_dictionary(userDict_);
        userDict_ = nullptr;
        userDictPath_.clear();
    }
}

void SekkaEngine::applyDictionaries(SekkaContextFfi *ctx) const {
    if (!ctx) {
        return;
    }

    // The user dictionary goes first so it takes priority. Ownership of the handles stays with the engine.
    std::vector<SekkaDictionaryFfi *> dicts;
    if (userDict_) {
        dicts.push_back(userDict_);
    }
    if (masterDict_) {
        dicts.push_back(masterDict_);
    }
    sekka_context_set_dictionaries(ctx, dicts.data(),
                                   static_cast<int>(dicts.size()));
}

} // namespace fcitx

// Factory registration through the FCITX_ADDON_FACTORY_V2 macro
FCITX_ADDON_FACTORY_V2(sekka, fcitx::SekkaFactory);
