// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// sekka.cpp - implementation of the Sekka engine and context
#include "sekka.h"

#include "focuspolicy.h"
#include "sekkacandidatelist.h"

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/userinterfacemanager.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace fcitx {

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
        // For a non-character key that committed the romaji as it is, and for an "other
        // key" during reselection, the same key is forwarded to the application after the
        // committed output and the preedit update (D-03/D-09; RESEARCH Pattern 2: fcitx5
        // guarantees that the committed string reaches the client before the forwarded
        // key).
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
    if (ctx_) {
        if (commitPending) {
            sekka_context_confirm_candidate(ctx_);
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

        ic_.updatePreedit();
        ic_.updateUserInterface(UserInterfaceComponent::InputPanel);
    }
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
    fcitx::readAsIni(config_, "conf/sekka.conf");
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
    fcitx::readAsIni(config_, "conf/sekka.conf");
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

// D-105 / FR-009: tells the user through the notifications addon that a dictionary failed
// to load. Called from every failure branch of openDictionaries() (the refusal + the three
// kinds of D-64 + default + a user dictionary load failure).
//
// - a failure to notify must never stop input: it throws nothing and ignores the return
//   value (the notification id) (the "a failure does not stop input" discipline of D-105 /
//   D-35 / D-102)
// - the text never contains the user's input history: only the dictionary path and the kind
//   of failure (the discipline of sekka.cpp:262-264 applies here too)
// - notifications is an optional dependency and notifications() can be null (when the
//   notifications addon is disabled or failed to load). When it is null, nothing happens and
//   only the existing FCITX_WARN remains
// - deduplication is limited to "the same (path, body) pair as last time", so notifications
//   do not pile up when reloadDictionaries() is called repeatedly with the same
//   configuration. The caller passes the `dedupSlot` of its own domain, master dictionary or
//   user dictionary. Once a load in that domain succeeds in openDictionaries() the
//   corresponding `dedupSlot` is cleared, so the same failure recurring after an intervening
//   success is notified as a new failure. Because each domain uses its own slot, a success in
//   one domain never affects the suppression state of a failure in the other (05-REVIEW
//   CR-01; with a single shared field, a success on one side would wipe the deduplication key
//   of the other side's failure, which was a bug where the same failure was re-notified on
//   every unrelated settings change).
void SekkaEngine::notifyDictionaryError(std::string &dedupSlot,
                                         const std::string &path,
                                         const std::string &body) {
    auto key = path + "\n" + body;
    if (key == dedupSlot) {
        return;
    }
    auto *n = notifications();
    if (!n) {
        return;
    }
    n->call<fcitx::INotifications::sendNotification>(
        "sekka", 0, "dialog-error", _("Sekka: failed to load a dictionary"), body,
        std::vector<std::string>{}, 5000, fcitx::NotificationActionCallback{},
        fcitx::NotificationClosedCallback{});
    dedupSlot = key;
}

void SekkaEngine::openDictionaries() {
    // Load the master dictionary.
    auto dictPath = config_.dictionaryPath.value();
    if (!dictPath.empty() && ::access(dictPath.c_str(), W_OK) == 0) {
        // 02.1-REVIEW CR-01: the master dictionary is mmapped, and rewriting the file while
        // it is mapped takes the whole fcitx5 process down with SIGBUS (which catch_unwind
        // cannot catch). That safety rests, per the comments, on the deployment being
        // root-owned 644 under package management, but `dictionaryPath` can be changed to an
        // arbitrary path from the settings UI by an unprivileged user, so at the very least a
        // path writable by the running user is refused here (writable by the real UID of the
        // running process = that user could rewrite it while mapped and induce SIGBUS).
        FCITX_WARN() << "refusing to load the master dictionary for safety because its "
                        "path is writable (deploying it root-owned with mode 644 is "
                        "recommended): "
                     << dictPath;
        // notifyDictionaryError() must be called before dictPath.clear() (after the clear
        // the path would be empty and could not go into the notification text).
        // dictPath.clear() itself is not changed here - D-105 explicitly says the behaviour
        // that a refused dictionary is not loaded stays exactly as it is (02.1-REVIEW CR-01,
        // avoiding SIGBUS from a rewrite while mapped).
        notifyDictionaryError(
            lastNotifiedMasterDictError_, dictPath,
            _("refused to load the master dictionary for safety because its path is "
              "writable (deploying it root-owned with mode 644 is recommended): ") +
                dictPath);
        dictPath.clear();
    }
    if (!dictPath.empty()) {
        int dictError = SEKKA_DICT_OK;
        masterDict_ = sekka_file_dict_new_with_error(dictPath.c_str(), "UTF-8",
                                                      &dictError);
        if (!masterDict_) {
            // D-64/SC6: log the three kinds of dictionary load failure distinctly.
            // D-105/FR-009: deliver the same text to the user through the notifications
            // addon (added by the planner; a user dictionary load failure is treated as the
            // same problem alongside the "refusal + three kinds" D-105 enumerates. This is
            // an addition within the same intent, not a narrowing of scope).
            switch (dictError) {
            case SEKKA_DICT_ERROR_NOT_FOUND:
                FCITX_WARN() << "master dictionary not found: " << dictPath;
                notifyDictionaryError(lastNotifiedMasterDictError_, dictPath,
                                      _("master dictionary not found: ") +
                                          dictPath);
                break;
            case SEKKA_DICT_ERROR_UNREADABLE:
                FCITX_WARN()
                    << "cannot open the master dictionary (check the permissions): "
                    << dictPath;
                notifyDictionaryError(
                    lastNotifiedMasterDictError_, dictPath,
                    _("cannot open the master dictionary (check the permissions): ") +
                        dictPath);
                break;
            case SEKKA_DICT_ERROR_CORRUPT:
                FCITX_WARN()
                    << "the master dictionary is corrupt (invalid format or version): "
                    << dictPath;
                notifyDictionaryError(
                    lastNotifiedMasterDictError_, dictPath,
                    _("the master dictionary is corrupt (invalid format or version): ") +
                        dictPath);
                break;
            default:
                FCITX_WARN() << "failed to load the master dictionary: "
                             << dictPath;
                notifyDictionaryError(lastNotifiedMasterDictError_, dictPath,
                                      _("failed to load the master dictionary: ") +
                                          dictPath);
                break;
            }
        } else {
            // 05-REVIEW CR-01: on a successful load, clear only the deduplication state of
            // the master dictionary domain itself. Without the clear, a (path, body) pair
            // that failed previously and recurs after an intervening success would be
            // silently suppressed by notifyDictionaryError() as the same key. The user
            // dictionary side's slot is left untouched (the domains are independent, so a
            // success on one side must not wipe the suppression state of the other side's
            // failure).
            lastNotifiedMasterDictError_.clear();
        }
    }

    // Load the user dictionary.
    auto userDictPath = config_.userDictionaryPath.value();
    if (userDictPath.empty()) {
        // Build the default path.
        auto *home = std::getenv("HOME");
        if (home) {
            userDictPath = std::string(home) +
                           "/.local/share/fcitx5/sekka/user-dict.db";
        }
    }
    if (!userDictPath.empty()) {
        // Create the directory when it does not exist.
        auto parentDir = std::filesystem::path(userDictPath).parent_path();
        if (!parentDir.empty()) {
            std::filesystem::create_directories(parentDir);
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
