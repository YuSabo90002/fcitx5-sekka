// SPDX-FileCopyrightText: 2026 yuta <yusabo90002@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

// sekka.h - definitions of the Sekka engine and context classes
#ifndef SEKKA_H
#define SEKKA_H

#include <string>
#include <vector>

// notifications_public.h - the optional dependency interface to the notifications
// addon (D-105). The `INTERFACE_INCLUDE_DIRECTORIES` of
// `Fcitx5::Module::Notifications` (an INTERFACE library) points at the header
// directory of the notifications module itself
// (`.../include/Fcitx5/Module/fcitx-module/notifications`), so the long form
// `<fcitx-module/notifications/notifications_public.h>` does not resolve in this build
// (measured; Fcitx5ModuleNotificationsConfig.cmake). The short form is used.
#include <notifications_public.h>
#include <fcitx-utils/inputbuffer.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>

#include "sekkaconfig.h"
#include "sekkadictpath.h"

// Forward declarations of the libsekka C ABI
//
// This file is hand-written. The cbindgen-generated header
// (`.sekka-prefix/include/sekka/sekka.h`) is neither referenced by the C++ side nor
// synchronized automatically, so whenever the C ABI changes this must be fixed by hand
// (02.1-RESEARCH.md Pitfall 1).
extern "C" {
struct SekkaContextFfi;
struct SekkaDictionaryFfi;

// The three kinds of dictionary load failure (D-64, the detection side of SC6). The
// values must match the SEKKA_DICT_* constants in libsekka/src/capi.rs (changing only
// one side would still compile against the old signature).
enum SekkaDictError {
    SEKKA_DICT_OK = 0,
    SEKKA_DICT_ERROR_INVALID_ARG = 1,
    SEKKA_DICT_ERROR_NOT_FOUND = 2,
    SEKKA_DICT_ERROR_UNREADABLE = 3,
    SEKKA_DICT_ERROR_CORRUPT = 4,
    SEKKA_DICT_ERROR_OTHER = 5,
};

SekkaContextFfi *sekka_context_new(void);
void sekka_context_free(SekkaContextFfi *ctx);
void sekka_context_reset(SekkaContextFfi *ctx);
int sekka_context_process_key_event(SekkaContextFfi *ctx, uint32_t keysym,
                                     uint32_t modifiers, int is_release);
int sekka_context_trigger(SekkaContextFfi *ctx);
char *sekka_context_get_preedit(SekkaContextFfi *ctx);
int sekka_context_get_preedit_cursor_pos(SekkaContextFfi *ctx);
char *sekka_context_poll_output(SekkaContextFfi *ctx);
int sekka_context_get_candidate_count(SekkaContextFfi *ctx);
int sekka_context_get_candidates(SekkaContextFfi *ctx, char **candidates,
                                  int max_count, int offset);
void sekka_context_select_candidate(SekkaContextFfi *ctx, int index);
void sekka_context_confirm_candidate(SekkaContextFfi *ctx);
int sekka_context_get_candidate_index(SekkaContextFfi *ctx);
int sekka_context_take_forward_key(SekkaContextFfi *ctx);
// Word registration (Phase 10, D-170).
int sekka_context_is_registering(SekkaContextFfi *ctx);
char *sekka_context_get_registration_reading(SekkaContextFfi *ctx);
char *sekka_context_get_registration_prompt(SekkaContextFfi *ctx);
void sekka_context_finalize_for_reset(SekkaContextFfi *ctx);
void sekka_free_candidate_list(char **candidates, int count);
void sekka_free_string(char *str);
const char *sekka_get_version(void);

SekkaDictionaryFfi *sekka_file_dict_new(const char *path,
                                         const char *encoding);
SekkaDictionaryFfi *sekka_file_dict_new_with_error(const char *path,
                                                    const char *encoding,
                                                    int *out_error);
SekkaDictionaryFfi *sekka_user_dict_new(const char *path,
                                         const char *encoding);
void sekka_free_dictionary(SekkaDictionaryFfi *dict);
void sekka_context_set_dictionaries(SekkaContextFfi *ctx,
                                     SekkaDictionaryFfi **dicts, int count);
int sekka_context_save_dictionaries(SekkaContextFfi *ctx);
int sekka_dictionary_save(SekkaDictionaryFfi *dict);
}

namespace fcitx {

class SekkaEngine;

/// State of a Sekka input context
///
/// Inherits fcitx5's InputContextProperty and manages the Sekka conversion state of
/// each input context. It owns a libsekka SekkaContextFfi.
class SekkaState : public InputContextProperty {
public:
    explicit SekkaState(SekkaEngine *engine, InputContext &ic);
    ~SekkaState() override;

    /// Processes a key event
    void keyEvent(KeyEvent &event);

    /// Resets the context state. Only when `commitPending` is true does it commit the
    /// uncommitted text before clearing the state (D-14 / D-15)
    void reset(bool commitPending);

    /// Returns the libsekka context
    SekkaContextFfi *context() const { return ctx_; }

private:
    /// Updates the preedit display
    void updatePreedit();

    /// Updates the display while word registration is active (D-170,
    /// Phase 10). Sets the application's own input position (client
    /// preedit) to just the outermost step's reading, and the popup (auxUp +
    /// preedit) to the registration label/prompt and the word being
    /// assembled, together instead of the usual either/or split.
    void updateRegistrationPanel();

    /// Attaches the candidate window to the input panel when there are
    /// candidates to show (D-116). Factored out of `updatePreedit()` so
    /// `updateRegistrationPanel()` can reuse the same candidate-list wiring
    /// (the body and the callback lambda are unchanged either way).
    void attachCandidateList();

    /// Checks for committed output and sends it to the application
    void checkAndCommit();

    SekkaEngine *engine_;
    InputContext &ic_;
    SekkaContextFfi *ctx_;
};

/// The Sekka input method engine
///
/// Inherits fcitx5's InputMethodEngineV2 and manages the lifecycle of the Sekka input
/// method.
class SekkaEngine : public InputMethodEngineV2 {
public:
    SekkaEngine(Instance *instance);
    ~SekkaEngine() override;

    /// Called when the input method is activated
    void activate(const InputMethodEntry &entry,
                  InputContextEvent &event) override;

    /// Called when the input method is deactivated
    void deactivate(const InputMethodEntry &entry,
                    InputContextEvent &event) override;

    /// Processes a key event
    void keyEvent(const InputMethodEntry &entry, KeyEvent &event) override;

    /// Resets the context state
    void reset(const InputMethodEntry &entry,
               InputContextEvent &event) override;

    /// Reloads the configuration
    void reloadConfig() override;

    /// Saves the addon's data (called on fcitx5 shutdown, on the idle save timer and on
    /// an explicit save request)
    void save() override;

    /// Returns the configuration object (used by `Controller1.GetConfig`, D-104).
    /// The default implementation of `AddonInstance` returns `nullptr`, so without this
    /// `GetConfig` fails with "Failed to get addon config.".
    const Configuration *getConfig() const override { return &config_; }

    /// Receives a configuration, saves it and then reloads it (used by
    /// `Controller1.SetConfig`, D-104). Implemented in `sekka.cpp`. No per-input-method
    /// configuration (a custom override of `getConfigForInputMethod`) is provided, since
    /// Sekka has only one input method.
    void setConfig(const RawConfig &config) override;

    /// Returns a reference to the configuration object
    const SekkaConfig &config() const { return config_; }

    /// Returns a reference to the fcitx5 instance
    Instance *instance() { return instance_; }

    /// Sets the loaded dictionaries on a context
    void applyDictionaries(SekkaContextFfi *ctx) const;

private:
    /// Opens the dictionaries according to the configuration
    void openDictionaries();

    /// Frees the dictionary handles
    void closeDictionaries();

    /// Reopens the dictionaries and sets them again on every input context
    void reloadDictionaries();

    /// Notifies the user of a dictionary load failure (D-105 / FR-009). Single-notice
    /// version. Its body is a thin delegation to `notifyDictionaryErrors()` with a
    /// one-element batch (`{DictionaryErrorNotice{path, body}}`), so a single failure's
    /// dedup key is identical to the batch version's one-notice key
    /// (`dictionaryNoticeDedupKey`), and the user dictionary side (which never batches)
    /// keeps behaving exactly as before this plan.
    ///
    /// - a failure to notify must never stop input: it throws nothing and ignores the
    ///   return value (the "a failure does not stop input" discipline of D-105 / D-35 /
    ///   D-102)
    /// - the text never contains the user's input history: only the dictionary path and
    ///   the kind of failure (the discipline of sekka.cpp:262-264)
    /// - notifications is an optional dependency and notifications() can be null
    /// - deduplication is limited to "the same (path, body) pair as last time". The
    ///   caller passes the state of its own domain - master dictionary or user
    ///   dictionary - in `dedupSlot`, so the suppression is independent per domain
    ///   (05-REVIEW CR-01: with a single shared field, a success in one domain would wipe
    ///   out the suppression state of a failure in the other, turning into a bug where the
    ///   notification for the same failure is resent on every unrelated settings change).
    void notifyDictionaryError(std::string &dedupSlot, const std::string &path,
                                const std::string &body);

    /// Notifies the user of a batch of dictionary load problems from one
    /// `openDictionaries()` call (D-105 / FR-009 / 06-02 D-127). Used by the master
    /// dictionary domain, which can produce several notices in one call (one per skipped
    /// candidate, plus the D-127 "not found in the search locations" summary).
    ///
    /// - an empty `notices` batch clears `dedupSlot` and sends nothing: that domain's load
    ///   was clean this time, which is the same "success clears the slot" behavior as
    ///   05-REVIEW CR-01's per-domain clearing
    /// - the batch key (`dictionaryNoticeDedupKey(notices)`) is compared against
    ///   `dedupSlot`; an identical key sends nothing, so repeated
    ///   `reloadDictionaries()` calls with the same configuration do not pile up
    ///   notifications for the candidates that keep being skipped (06-CONTEXT.md
    ///   Claude's Discretion)
    /// - `notifications()` being null (the addon disabled or unavailable) means nothing is
    ///   sent and `dedupSlot` is left untouched, exactly like the single-notice version
    /// - each notice in the batch is sent through the same
    ///   `INotifications::sendNotification` call as before; only the body/path per notice
    ///   differs, so no other part of the notification (icon, timeout, actions) changes
    void notifyDictionaryErrors(std::string &dedupSlot,
                                const std::vector<DictionaryErrorNotice> &notices);

    Instance *instance_;
    SekkaConfig config_;
    FactoryFor<SekkaState> factory_;

    /// The master dictionary.
    SekkaDictionaryFfi *masterDict_ = nullptr;
    /// The user dictionary.
    SekkaDictionaryFfi *userDict_ = nullptr;
    /// Path of the user dictionary (the value openDictionaries() actually used; kept so
    /// the warning log of save() can say which file failed to save).
    std::string userDictPath_;

    /// Optional dependency loader for the notifications addon (D-105).
    /// `notifications()` is a member function returning `AddonInstance *` (which can be
    /// null); it expands into the two members `_notificationsFirstCall_` and
    /// `_notifications_`.
    FCITX_ADDON_DEPENDENCY_LOADER(notifications, instance_->addonManager());

    /// The key of the batch of notices last sent about the master dictionary
    /// (`dictionaryNoticeDedupKey`, 06-02). Used for deduplication (D-105). It is cleared
    /// whenever a load in openDictionaries() produces an empty batch (nothing to notify -
    /// everything was clean), so the same failure recurring after an intervening success
    /// counts as a new failure. Success or failure on the user dictionary side does not
    /// affect this field (05-REVIEW CR-01: independent suppression state per domain).
    std::string lastNotifiedMasterDictError_;

    /// The concatenated key of the (path, body) pair last notified about the user
    /// dictionary. Used for deduplication (D-105). It is cleared when the user dictionary
    /// load in openDictionaries() succeeds, so the same failure recurring after an
    /// intervening success counts as a new failure. Success or failure on the master
    /// dictionary side does not affect this field (05-REVIEW CR-01: independent suppression
    /// state per domain).
    std::string lastNotifiedUserDictError_;
};

/// The Sekka addon factory
class SekkaFactory : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override {
        return new SekkaEngine(manager->instance());
    }
};

} // namespace fcitx

#endif // SEKKA_H
