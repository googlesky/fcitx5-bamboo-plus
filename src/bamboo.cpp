/*
 * SPDX-FileCopyrightText: 2022-2022 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */

#include "bamboo.h"
#include "bambooconfig.h"
#include <algorithm>
#include <clipboard_public.h>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/charutils.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/macros.h>
#include <fcitx-utils/misc.h>
#include <fcitx-utils/standardpaths.h>
#include <fcitx-utils/stringutils.h>
#include <fcitx-utils/textformatflags.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/action.h>
#include <fcitx/addoninstance.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/globalconfig.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputpanel.h>
#include <fcitx/menu.h>
#include <fcitx/statusarea.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>
#include <fcitx/userinterfacemanager.h>
#include <fcntl.h>
#include <memory>
#include <notifications_public.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace fcitx {

namespace {

constexpr std::string_view MacroPrefix = "macro/";
constexpr std::string_view InputMethodActionPrefix = "bamboo-input-method-";
constexpr std::string_view CharsetActionPrefix = "bamboo-charset-";
// How long a key waits for the application to report our last edit, in
// microseconds. Chrome reports within a few milliseconds, on a loaded
// machine within hundreds.
constexpr uint64_t HeldKeyTimeout = 150000;
constexpr uint64_t HeldKeyMaxWait = 1000000;
const std::string CustomKeymapFile = "conf/bamboo-custom-keymap.conf";
const std::string AppModeFile = "conf/bamboo-app-mode.conf";

FCITX_DEFINE_LOG_CATEGORY(bamboo, "bamboo");

std::string macroFile(std::string_view imName) {
    return stringutils::concat("conf/bamboo-macro-", imName, ".conf");
}

uintptr_t newMacroTable(const BambooMacroTable &macroTable) {
    std::vector<char *> charArray;
    for (const auto &keymap : *macroTable.macros) {
        charArray.push_back(const_cast<char *>(keymap.key->data()));
        charArray.push_back(const_cast<char *>(keymap.value->data()));
    }
    charArray.push_back(nullptr);
    return NewMacroTable(charArray.data());
}

// At a field or line start, or after '.', '!' or '?' and spaces.
bool startsSentence(std::string_view text) {
    const auto end = text.find_last_not_of(" \t");
    if (end == std::string_view::npos || text[end] == '\n') {
        return true;
    }
    return end + 1 < text.size() &&
           std::string_view(".!?").find(text[end]) != std::string_view::npos;
}

// Normalization drops Shift from "~", accept keys saved either way.
bool checkHotkey(const KeyEvent &keyEvent, const KeyList &keys) {
    return keyEvent.key().checkKeyList(keys) ||
           keyEvent.rawKey().checkKeyList(keys);
}

class InputModeCandidateWord : public CandidateWord {
public:
    InputModeCandidateWord(BambooEngine *engine, BambooInputMode mode)
        : CandidateWord(Text(BambooInputModeI18NAnnotation::toString(mode))),
          engine_(engine), mode_(mode) {}

    void select(InputContext *inputContext) const override {
        engine_->setInputMode(inputContext, mode_);
    }

private:
    BambooEngine *engine_;
    BambooInputMode mode_;
};

// One line of at most 40 characters.
std::string preview(std::string_view text) {
    std::string line;
    size_t count = 0;
    for (auto chr : utf8::MakeUTF8CharRange(text)) {
        if (count++ == 40) {
            line += "…";
            break;
        }
        line += chr == '\n' ? std::string(" ") : utf8::UCS4ToUTF8(chr);
    }
    return line;
}

std::string convertLabel(const std::string &kind) {
    if (kind == "retype") {
        return _("Typed again with the input method");
    }
    if (kind == "plain") {
        return _("Without diacritics");
    }
    if (kind == "upper") {
        return _("Upper case");
    }
    if (kind == "lower") {
        return _("Lower case");
    }
    if (kind == "title") {
        return _("Capitalized words");
    }
    return stringutils::concat(_("From"), " ", kind);
}

// A conversion offered by the convert key.
class ConvertCandidateWord : public CandidateWord {
public:
    ConvertCandidateWord(BambooState *state, std::string result,
                         const std::string &label)
        : CandidateWord(Text(preview(result))), state_(state),
          result_(std::move(result)) {
        setComment(Text(label));
    }

    void select(InputContext *inputContext) const override;

private:
    BambooState *state_;
    std::string result_;
};

// array is nullptr when the Go side recovered from a panic.
std::vector<std::string> convertToStringList(char **array) {
    std::vector<std::string> result;
    if (!array) {
        return result;
    }
    for (int i = 0; array[i]; i++) {
        result.push_back(array[i]);
        free(array[i]);
    }
    free(array);
    return result;
}

} // namespace

#define FCITX_BAMBOO_DEBUG() FCITX_LOGC(bamboo, Debug)
#define FCITX_BAMBOO_WARN() FCITX_LOGC(bamboo, Warn)

class BambooState final : public InputContextProperty {
public:
    BambooState(BambooEngine *engine, InputContext *ic)
        : engine_(engine), ic_(ic) {
        setEngine();
    }

    ~BambooState() {}

    void setEngine() {
        bambooEngine_.reset();

        if (*engine_->config().inputMethod == "Custom") {
            std::vector<char *> charArray;
            for (const auto &keymap : *engine_->customKeymap().customKeymap) {
                charArray.push_back(const_cast<char *>(keymap.key->data()));
                charArray.push_back(const_cast<char *>(keymap.value->data()));
            }
            charArray.push_back(nullptr);
            bambooEngine_.reset(NewCustomEngine(charArray.data(),
                                                engine_->dictionary(),
                                                engine_->macroTable()));
        } else {
            bambooEngine_.reset(NewEngine(engine_->config().inputMethod->data(),
                                          engine_->dictionary(),
                                          engine_->macroTable()));
        }
        if (!bambooEngine_) {
            FCITX_BAMBOO_WARN() << "Failed to create engine for input method "
                                << *engine_->config().inputMethod;
        }
        setOption();
    }

    void setOption() {
        if (!bambooEngine_) {
            return;
        }
        std::vector<char *> exceptions;
        for (const auto &word : *engine_->config().spellCheckExceptions) {
            exceptions.push_back(const_cast<char *>(word.data()));
        }
        exceptions.push_back(nullptr);
        FcitxBambooEngineOption option = {
            .autoNonVnRestore = *engine_->config().autoNonVnRestore,
            .ddFreeStyle = true,
            .macroEnabled = *engine_->config().macro,
            .autoCapitalizeMacro = *engine_->config().capitalizeMacro,
            .spellCheckWithDicts = *engine_->config().spellCheck,
            .outputCharset = engine_->config().outputCharset->data(),
            .modernStyle = *engine_->config().modernStyle,
            .freeMarking = *engine_->config().freeMarking,
            .spellCheckExceptions = exceptions.data(),
            .standaloneW =
                static_cast<int>(*engine_->config().quickTyping->standaloneW),
            .quickDouble = *engine_->config().quickTyping->doubleConsonants,
            .quickStart = *engine_->config().quickTyping->startConsonants,
            .quickEnd = *engine_->config().quickTyping->endConsonants,
        };
        EngineSetOption(bambooEngine_.handle(), &option);
    }

    // Qt Quick reports its password fields as sensitive only, not hidden:
    // fcitx5 keeps the input method there, and Qt Quick draws the preedit
    // unmasked. Wayland frontends mark every field of Chrome's incognito
    // windows sensitive too, only fcitx5-qt's sensitive fields count.
    static bool qtPasswordField(CapabilityFlags flags) {
        return flags.test(CapabilityFlag::GetIMInfoOnFocus) &&
               flags.test(CapabilityFlag::Sensitive) &&
               !flags.test(CapabilityFlag::Password);
    }
    static bool passwordField(CapabilityFlags flags) {
        return flags.test(CapabilityFlag::Password) || qtPasswordField(flags);
    }
    bool passwordField() const { return passwordField(ic_->capabilityFlags()); }

    // What Konsole's TerminalDisplay asks for: neither capitals nor
    // predictions. Password and URL fields ask the same.
    static bool terminalHints(CapabilityFlags flags) {
        return flags.test(CapabilityFlag::NoAutoUpperCase) &&
               flags.test(CapabilityFlag::NoSpellCheck) &&
               !flags.testAny(CapabilityFlag::PasswordOrSensitive) &&
               !flags.test(CapabilityFlag::Url);
    }

    // Addresses and numbers are never Vietnamese. URL fields are left alone:
    // browsers' address bars are searched in Vietnamese. Password fields get
    // the keys as typed, as fcitx5 gives them unless told otherwise.
    bool excludedField() const {
        if (passwordField() && !engine_->instance()
                                    ->globalConfig()
                                    .allowInputMethodForPassword()) {
            return true;
        }
        return *engine_->config().autoExcludeFields &&
               ic_->capabilityFlags().testAny(CapabilityFlags{
                   CapabilityFlag::Email, CapabilityFlag::Digit,
                   CapabilityFlag::Number, CapabilityFlag::Dialable});
    }

    // The mode of the program, Exclude in excluded fields.
    BambooInputMode effectiveMode() const {
        return excludedField() ? BambooInputMode::Exclude
                               : engine_->inputMode(ic_);
    }

    // How the word being typed shows.
    enum class Method {
        Exclude,
        Preedit,
        PlainPreedit,
        PanelPreedit,
        Surrounding,
        BackSpaces
    };

    Method method() const {
        const auto mode = effectiveMode();
        // An address bar shows its suggestion in its report only, and
        // composing whole words loses Return to it: where its text is edited
        // in place it gets Surrounding Text, which waits for the report.
        if ((mode == BambooInputMode::BackSpace ||
             mode == BambooInputMode::InputMethodWindow) &&
            ic_->capabilityFlags().test(CapabilityFlag::Url) &&
            surroundingTextMethod() == Method::Surrounding) {
            return Method::Surrounding;
        }
        switch (mode) {
        case BambooInputMode::Exclude:
            return Method::Exclude;
        case BambooInputMode::Preedit:
            return Method::Preedit;
        case BambooInputMode::PlainPreedit:
            return Method::PlainPreedit;
        case BambooInputMode::InputMethodWindow:
            return Method::PanelPreedit;
        case BambooInputMode::BackSpace:
            // KWin hands keys we forward to the application in order with
            // our commits. fcitx5-qt hands them over after commits: DEL
            // characters go with the commits instead, which terminals take
            // for BackSpace, Konsole reporting no text; other Qt
            // applications get plain preedit. Elsewhere words go in whole.
            if (ic_->frontendName() == "wayland") {
                return Method::BackSpaces;
            }
            if (ic_->capabilityFlags().test(CapabilityFlag::GetIMInfoOnFocus)) {
                return engine_->isQtTerminal(ic_) ? Method::BackSpaces
                                                  : Method::PlainPreedit;
            }
            return Method::PanelPreedit;
        case BambooInputMode::SurroundingText:
            break;
        }
        return surroundingTextMethod();
    }

    // Surrounding Text mode never underlines the word: the application's
    // text is edited where it can be, else the word is plain preedit where
    // the client draws it as told (Qt), else it shows in fcitx5's window
    // (Chromium, Firefox and terminals underline any preedit).
    Method surroundingTextMethod() const {
        // fcitx5-qt reports surrounding text on some updates only, words
        // would change methods as it comes and goes.
        if (ic_->capabilityFlags().test(CapabilityFlag::GetIMInfoOnFocus)) {
            return Method::PlainPreedit;
        }
        // Deleting blindly would corrupt text: Wayland frontends claim the
        // capability for clients that send no surrounding text. A selection
        // after the cursor is an address bar's suggestion: our commits
        // replace it, Chrome deletes it with the text before it, and Return
        // takes it as long as the word goes in key by key.
        // Terminals report no text, KWin passes on an empty one for them.
        const auto &surroundingText = ic_->surroundingText();
        if (ic_->capabilityFlags().test(CapabilityFlag::SurroundingText) &&
            surroundingText.isValid() &&
            surroundingText.cursor() <= surroundingText.anchor() &&
            !engine_->isTerminal(ic_)) {
            return Method::Surrounding;
        }
        // KWin hands keys we forward to the application in order with our
        // commits, whether the application keeps that order is its business.
        if (*engine_->config().waylandBackSpace &&
            ic_->frontendName() == "wayland") {
            return Method::BackSpaces;
        }
        return Method::PanelPreedit;
    }

    // The typing mode doing what method() picks, for the label.
    BambooInputMode methodMode() const {
        switch (method()) {
        case Method::Exclude:
            return BambooInputMode::Exclude;
        case Method::Preedit:
            return BambooInputMode::Preedit;
        case Method::PlainPreedit:
            return BambooInputMode::PlainPreedit;
        case Method::PanelPreedit:
            return BambooInputMode::InputMethodWindow;
        case Method::Surrounding:
            return BambooInputMode::SurroundingText;
        case Method::BackSpaces:
            return BambooInputMode::BackSpace;
        }
        return BambooInputMode::Preedit;
    }

    void keyEvent(KeyEvent &keyEvent) {
        // Ignore all key release.
        if (!bambooEngine_ || keyEvent.isRelease()) {
            return;
        }
        if (!heldKeys_.empty()) {
            // Keys keep their order behind a held one.
            if (holdable(keyEvent.rawKey())) {
                heldKeys_.push_back(keyEvent.rawKey());
                keyEvent.filterAndAccept();
                return;
            }
            // Others would lose their modifiers waiting, the held keys are
            // typed first.
            typeHeldKeys();
        }
        if (!processKey(keyEvent)) {
            heldKeys_.push_back(keyEvent.rawKey());
            keyEvent.filterAndAccept();
            waitForReport();
        }
    }

    // Returns false when the key has to wait for the application to report
    // our last edit, see waitForReport.
    bool processKey(KeyEvent &keyEvent, bool mayWait = true) {
        const bool restoreKey =
            keyEvent.key().checkKeyList(*engine_->config().restoreKeyStroke);
        // Like ibus-bamboo, a lone Shift or CapsLock must not end the word.
        const auto sym = keyEvent.rawKey().sym();
        if (!restoreKey &&
            (sym == FcitxKey_Shift_L || sym == FcitxKey_Shift_R ||
             sym == FcitxKey_Caps_Lock)) {
            return true;
        }
        // VIQR types tones with '.' and '?', they end no sentence then.
        const bool typingKey = EngineIsTypingKey(bambooEngine_.handle(), sym,
                                                 keyEvent.rawKey().states());
        // Typing fast, the application may report its text late: its text is
        // only trusted when reported after our last change and the last key
        // it got, see changeApplicationText.
        const bool fresh = surroundingFresh_;
        // BackSpace taking the one letter of a word reported leaves the
        // text ending with the separator before it.
        const bool lastLetter = keyEvent.key().check(FcitxKey_BackSpace) &&
                                utf8::length(surroundingWord()) == 1 &&
                                surroundingInSync(surroundingWord());
        processing_ = true;
        const bool handled = handleKey(keyEvent, restoreKey, fresh, mayWait);
        processing_ = false;
        if (!handled) {
            return false;
        }
        if (!keyEvent.filtered()) {
            surroundingFresh_ = false;
            if (surroundingWord().empty() && !lastLetter) {
                separator_ = holdable(keyEvent.rawKey())
                                 ? utf8::UCS4ToUTF8(Key::keySymToUnicode(sym))
                                 : "";
            }
        }
        if (lastMethod_ == Method::Surrounding ||
            lastMethod_ == Method::BackSpaces) {
            noteEdit();
        }
        lastKeyToApp_ = !keyEvent.filtered();
        sentenceKeys_ =
            typingKey ? SentenceKeys::Other : nextSentenceKeys(keyEvent.key());
        return true;
    }

    // Remembers how the text before the cursor ends once the application has
    // our last edits, see waitForReport.
    void noteEdit() {
        noteTail(stringutils::concat(separator_, surroundingWord()));
    }

    void noteTail(std::string tail) {
        if (tail.empty() ||
            (!editTails_.empty() && editTails_.back() == tail)) {
            return;
        }
        editTails_.push_back(std::move(tail));
        if (editTails_.size() > 16) {
            editTails_.pop_front();
        }
    }

    // KWin sends a deletion and the commit after it in turn: the
    // application may report the text in between.
    void noteDeletion(int count) {
        if (editTails_.empty()) {
            return;
        }
        auto tail = editTails_.back();
        for (; count > 0 && !tail.empty(); count--) {
            auto last = tail.size();
            while (last > 0 && (tail[--last] & 0xc0) == 0x80) {
            }
            tail.resize(last);
            noteTail(tail);
        }
    }

    // Chrome reports in order: once it reports the word, a text as one of
    // our older edits left it is a click or the application's change.
    void pruneTails() {
        wordStart_.reset();
        editTails_.clear();
        noteEdit();
    }

    // Whether the application reports its text as one of our last edits left
    // it, or as it was before the word: it is behind, its text not changed.
    bool reportBehind() const {
        const auto before = textBeforeCursor();
        if (wordStart_ && before == *wordStart_) {
            return true;
        }
        return std::ranges::any_of(editTails_, [before](const auto &tail) {
            return before.ends_with(tail);
        });
    }

    // A Qt Quick field turning into a password field or back mid-word: the
    // word goes in now, rather than stay shown unmasked, or masked, until
    // the next key. fcitx5 handles its Password flag itself, which Wayland
    // frontends drop and restore when a client resets.
    void capabilityChanged(CapabilityFlags oldFlags) {
        if (qtPasswordField(oldFlags) !=
            qtPasswordField(ic_->capabilityFlags())) {
            commitBuffer();
        }
        refreshLabel();
    }

    // The label tells what the mode does in the field, see
    // BambooEngine::subMode: it changes with the field's flags and reports,
    // and the panel asks for it again then. It asks as a field gets focus,
    // before KWin passes on its text.
    void refreshLabel() {
        if (!ic_->hasFocus() ||
            engine_->instance()->inputMethodEngine(ic_) != engine_) {
            return;
        }
        auto label = currentLabel();
        if (label != label_) {
            label_ = std::move(label);
            ic_->updateUserInterface(UserInterfaceComponent::StatusArea);
        }
    }
    void labelShown() { label_ = currentLabel(); }

    // Whether the focused field reported its text since it got focus, see
    // BambooEngine::isQtTerminal.
    bool textReported() const { return textReported_; }
    // A field with Konsole's hints reported its text in this input context,
    // one per window with fcitx5-qt: never cleared, see
    // BambooEngine::isQtTerminal.
    bool hintedTextReported() const { return hintedTextReported_; }
    // True when the application's text is known: fcitx5-qt drops the
    // SurroundingText capability before every key, but its last report
    // stays current once the focused field has made one.
    bool textKnown() const {
        const auto flags = ic_->capabilityFlags();
        return ic_->surroundingText().isValid() &&
               (flags.test(CapabilityFlag::SurroundingText) ||
                (flags.test(CapabilityFlag::GetIMInfoOnFocus) &&
                 textReported_));
    }
    void focusIn() {
        textReported_ = false;
        labelShown();
    }

    void surroundingTextUpdated() {
        surroundingFresh_ = true;
        textReported_ = true;
        if (terminalHints(ic_->capabilityFlags())) {
            hintedTextReported_ = true;
        }
        if (processing_ || releasing_ || !bambooEngine_) {
            return;
        }
        refreshLabel();
        // KWin refreshes the text on every key.
        const auto &surroundingText = ic_->surroundingText();
        auto report =
            std::make_tuple(surroundingText.text(), surroundingText.cursor(),
                            surroundingText.anchor());
        if (report == lastReport_) {
            return;
        }
        lastReport_ = std::move(report);
        const auto word = surroundingWord();
        const bool inSync = surroundingInSync(word);
        if (inSync && !word.empty()) {
            trustReports_ = true;
            timeouts_ = 0;
            pruneTails();
        }
        if (!heldKeys_.empty()) {
            if (inSync) {
                releaseHeldKeys(0);
            } else if (doneRequests_ < 3) {
                // This one came before the done we asked for, the report
                // after our edit may be held back still.
                doneRequests_++;
                requestDone();
            }
        }
    }

    // Types the held keys in order until one has to wait again. The first
    // forced ones go on anyway, a late report taken for a change of the
    // application like before keys waited.
    void releaseHeldKeys(size_t forced) {
        releasing_ = true;
        while (!heldKeys_.empty() && bambooEngine_) {
            const Key key = heldKeys_.front();
            heldKeys_.pop_front();
            KeyEvent event(ic_, key);
            if (!processKey(event, forced == 0)) {
                heldKeys_.push_front(key);
                releasing_ = false;
                waitForReport();
                return;
            }
            if (forced > 0) {
                forced--;
            }
            if (!event.filtered()) {
                typeForApplication(key);
            }
        }
        releasing_ = false;
        heldKeys_.clear();
        if (heldTimeout_) {
            heldTimeout_->setEnabled(false);
        }
    }

    bool handleKey(KeyEvent &keyEvent, bool restoreKey, bool fresh,
                   bool mayWait) {
        const auto sym = keyEvent.rawKey().sym();
        if (pickerOpen_ && pickerKeyEvent(keyEvent)) {
            return true;
        }
        // A mode of the program can't bring Vietnamese to an excluded field.
        // Password fields take these keys as typed: '~' goes into passwords,
        // and converting would show text from elsewhere.
        if (checkHotkey(keyEvent, *engine_->config().inputModeSwitchKey) &&
            !ic_->program().empty() && !excludedField() && !passwordField() &&
            !EngineIsTypingKey(bambooEngine_.handle(), sym,
                               keyEvent.rawKey().states())) {
            openPicker();
            keyEvent.filterAndAccept();
            return true;
        }
        if (checkHotkey(keyEvent, *engine_->config().convertKey) &&
            !passwordField()) {
            openConvert(fresh);
            keyEvent.filterAndAccept();
            return true;
        }
        const auto method = this->method();
        // Keys typed into password and sensitive fields stay out of logs.
        FCITX_BAMBOO_DEBUG()
            << "key "
            << (ic_->capabilityFlags().testAny(
                    CapabilityFlag::PasswordOrSensitive)
                    ? Key()
                    : keyEvent.key())
            << " program " << ic_->program() << " frontend "
            << ic_->frontendName() << " caps 0x" << std::hex
            << static_cast<uint64_t>(ic_->capabilityFlags()) << std::dec
            << " method " << static_cast<int>(method) << " surrounding "
            << ic_->surroundingText().isValid() << " "
            << ic_->surroundingText().cursor() << "/"
            << ic_->surroundingText().anchor() << " fresh " << fresh;
        // A word ends the way it started. Typed with BackSpace keys it is in
        // the application already, and goes on through the surrounding text
        // once that has it: Chrome reports a selection in a text field as
        // selected before the cursor, the first key typed over it goes in
        // with BackSpace keys.
        if (method != lastMethod_) {
            if (lastMethod_ != Method::BackSpaces ||
                method != Method::Surrounding) {
                commitBuffer();
            }
            lastMethod_ = method;
        }
        if (method == Method::Exclude) {
            return true;
        }
        // Like VNIKey's vim mode: normal mode commands need plain keys.
        if (keyEvent.key().check(FcitxKey_Escape) &&
            *engine_->config().terminalEscape && engine_->isTerminal(ic_)) {
            commitBuffer();
            // Deactivating re-enters this state, process nothing after it.
            engine_->instance()->deactivate();
            return true;
        }
        const bool surrounding =
            method == Method::Surrounding || method == Method::BackSpaces;
        // Typed with BackSpace keys, the word goes on through the surrounding
        // text once the application reports it, see reportBehind: what came
        // before it, nothing before the application's first report. Typed
        // over a selection before the cursor, it comes after what precedes
        // the selection, which our last key does not tell: see
        // surroundingInSync.
        if (method == Method::BackSpaces && surroundingWord().empty()) {
            const auto &surroundingText = ic_->surroundingText();
            if (surroundingText.anchor() < surroundingText.cursor()) {
                separator_.clear();
            }
            wordStart_ = std::string(textBefore(
                std::min(surroundingText.cursor(), surroundingText.anchor())));
        }
        if (method == Method::Surrounding &&
            !surroundingInSync(surroundingWord())) {
            // Chrome reports its text late, see waitForReport.
            if (mayWait && trustReports_ && holdable(keyEvent.rawKey()) &&
                ic_->frontendName() == "wayland") {
                return false;
            }
            // The application changed the word (autocorrection, a click):
            // start a new word rather than delete what is not ours. Behind
            // (keys forced through, reports not trusted) it comes after our
            // last edit, else after the text reported.
            FCITX_BAMBOO_DEBUG() << "surrounding text changed, new word";
            separator_ = lastCharacter(
                reportBehind()
                    ? stringutils::concat(separator_, surroundingWord())
                    : std::string(textBeforeCursor()));
            ResetEngine(bambooEngine_.handle());
        }
        if (method == Method::Surrounding) {
            if (surroundingWord().empty()) {
                // What the word comes after, see reportBehind.
                wordStart_ = std::string(textBeforeCursor());
            } else {
                pruneTails();
            }
        }

        if (restoreKey) {
            // With nothing to restore the key belongs to the application.
            if (EngineRestoreKeyStrokes(bambooEngine_.handle(), surrounding)) {
                keyEvent.filterAndAccept();
                flush();
            }
            return true;
        }

        // The word before the cursor is only edited after a key the
        // application handled itself (BackSpace, arrows): typing fast, our
        // own text may not be reported yet. Wayland frontends answer from a
        // copy of the text that may lag.
        if (fresh && lastKeyToApp_ && *engine_->config().editWordBeforeCursor &&
            !ic_->frontendName().starts_with("wayland") &&
            ic_->capabilityFlags().test(CapabilityFlag::SurroundingText) &&
            ic_->surroundingText().cursor() ==
                ic_->surroundingText().anchor()) {
            EngineEditWord(bambooEngine_.handle(),
                           std::string(textBeforeCursor()).c_str(), sym,
                           keyEvent.rawKey().states(), surrounding);
        }
        if (EngineProcessKeyEvent(
                bambooEngine_.handle(), sym, keyEvent.rawKey().states(),
                surrounding,
                capitalize(sym, keyEvent.rawKey().states(), fresh))) {
            keyEvent.filterAndAccept();
        }
        flush();
        return true;
    }

    // Applies the engine output in order: deletion, commit, preedit.
    void flush() {
        const int count = EnginePullDeleteCount(bambooEngine_.handle());
        UniqueCPtr<char> commit(EnginePullCommit(bambooEngine_.handle()));
        if (lastMethod_ == Method::Surrounding ||
            lastMethod_ == Method::BackSpaces) {
            noteDeletion(count);
        }
        changeApplicationText(count, commit ? commit.get() : "",
                              lastMethod_ == Method::BackSpaces);
        // The word ended: the next one comes after what we committed last.
        if (commit && commit.get()[0] && surroundingWord().empty()) {
            separator_ = lastCharacter(commit.get());
        }

        ic_->inputPanel().reset();
        UniqueCPtr<char> preedit(EnginePullPreedit(bambooEngine_.handle()));
        if (preedit && preedit.get()[0]) {
            std::string_view preeditView = preedit.get();
            Text text;
            TextFormatFlags format;
            if (lastMethod_ == Method::Preedit &&
                *engine_->config().displayUnderline) {
                format = TextFormatFlag::Underline;
            }
            if (utf8::validate(preeditView)) {
                std::string shown(preeditView);
                if (qtPasswordField(ic_->capabilityFlags()) &&
                    !engine_->instance()
                         ->globalConfig()
                         .showPreeditForPassword()) {
                    // fcitx5 masks the preedit of the password fields it
                    // knows of as it sends it, the word kept for its commit
                    // when focus goes. Here clients commit the preedit they
                    // show then, the dots must not be.
                    shown.clear();
                    for (auto i = utf8::length(preeditView); i > 0; i--) {
                        shown += "\xe2\x80\xa2";
                    }
                    format |= TextFormatFlag::DontCommit;
                }
                text.append(std::move(shown), format);
            }
            text.setCursor(text.textLength());

            if (lastMethod_ != Method::PanelPreedit &&
                ic_->capabilityFlags().test(CapabilityFlag::Preedit)) {
                ic_->inputPanel().setClientPreedit(text);
            } else {
                ic_->inputPanel().setPreedit(text);
            }
        }
        ic_->updatePreedit();
        ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
    }

    void reset() {
        // A click moved the cursor, or focus came back.
        lastKeyToApp_ = true;
        surroundingFresh_ = false;
        sentenceKeys_ = SentenceKeys::Other;
        pickerOpen_ = false;
        // Typed where the cursor was, focus or cursor gone elsewhere.
        heldKeys_.clear();
        separator_.clear();
        editTails_.clear();
        wordStart_.reset();
        if (heldTimeout_) {
            heldTimeout_->setEnabled(false);
        }
        ic_->inputPanel().reset();
        if (bambooEngine_) {
            ResetEngine(bambooEngine_.handle());
        }
        ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
        ic_->updatePreedit();
        // The configuration changed, or the field.
        refreshLabel();
    }

    // Leaving the input method, the keys typed with it are typed.
    void typeHeldKeys() { releaseHeldKeys(heldKeys_.size()); }

    // Returns what was committed.
    std::string commitBuffer() {
        pickerOpen_ = false;
        separator_.clear();
        ic_->inputPanel().reset();
        std::string committed;
        if (bambooEngine_) {
            // The reason that we do not commit here is we want to force the
            // behavior. When client get unfocused, the framework will try to
            // commit the string.
            EngineCommitPreedit(bambooEngine_.handle());
            UniqueCPtr<char> commit(EnginePullCommit(bambooEngine_.handle()));
            if (commit && commit.get()[0]) {
                committed = commit.get();
                changeApplicationText(0, committed);
            }
        }
        ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
        ic_->updatePreedit();
        return committed;
    }

    // UniKey toolkit: conversions of the selection, else of the word before
    // the cursor, else of the primary selection, to replace it.
    void openConvert(bool fresh) {
        const auto committed = commitBuffer();
        const auto &surroundingText = ic_->surroundingText();
        std::string text;
        convertDelete_ = 0;
        if (fresh && textKnown()) {
            if (surroundingText.cursor() != surroundingText.anchor()) {
                const auto low = std::min(surroundingText.cursor(),
                                          surroundingText.anchor());
                const auto high = std::max(surroundingText.cursor(),
                                           surroundingText.anchor());
                // fcitx5-qt reports a Multiline field's current paragraph
                // only, and maps a selection end past it to the paragraph's
                // end (QString::left clamps), other widgets maybe to its
                // start: such a selection may be only part of the real one,
                // left to the primary selection below.
                const bool paragraphClamped =
                    ic_->capabilityFlags().test(
                        CapabilityFlags{CapabilityFlag::GetIMInfoOnFocus,
                                        CapabilityFlag::Multiline}) &&
                    (low == 0 || high >= utf8::length(surroundingText.text()));
                // Our commit replaced it.
                if (committed.empty() && !paragraphClamped) {
                    text = surroundingText.selectedText();
                }
            } else if (ic_->capabilityFlags().test(
                           CapabilityFlag::SurroundingText) &&
                       !ic_->frontendName().starts_with("wayland")) {
                // fcitx5-qt drops a deletion sent while handling a key, so
                // this still needs the capability itself: a known text is
                // not enough to risk deleting the word.
                // Wayland frontends delete through a copy of the text that
                // may lag. This is the text as the application shows it once
                // it has our commit, unless it reported its text already.
                const auto before = stringutils::concat(
                    textBeforeCursor(), surroundingFresh_ ? "" : committed);
                const auto space = before.find_last_of(" \t\n");
                text =
                    before.substr(space == std::string::npos ? 0 : space + 1);
                convertDelete_ = utf8::length(text);
            }
        }
        if (text.empty() || convertDelete_ > 100) {
            text.clear();
            convertDelete_ = 0;
            if (auto *clipboard = engine_->clipboard()) {
                text = clipboard->call<IClipboard::primary>(ic_);
            }
        }
        // Keys wait while converting: a whole document would freeze them.
        if (const auto length = utf8::lengthValidated(text);
            length != utf8::INVALID_LENGTH && length > 5000) {
            engine_->instance()->showCustomInputMethodInformation(
                ic_, _("Too long to convert"));
            return;
        }
        std::vector<std::string> conversions;
        if (!text.empty() && utf8::validate(text)) {
            conversions = convertToStringList(
                EngineTextTransforms(bambooEngine_.handle(), text.c_str()));
        }
        if (conversions.empty()) {
            engine_->instance()->showCustomInputMethodInformation(
                ic_, _("Nothing to convert"));
            return;
        }
        auto candidates = std::make_unique<CommonCandidateList>();
        candidates->setLayoutHint(CandidateLayoutHint::Vertical);
        candidates->setPageSize(10);
        std::vector<std::string> labels;
        for (int i = 1; i <= 10; i++) {
            labels.push_back(stringutils::concat(i % 10, ". "));
        }
        candidates->setLabels(labels);
        for (size_t i = 0; i + 1 < conversions.size(); i += 2) {
            candidates->append<ConvertCandidateWord>(
                this, conversions[i + 1], convertLabel(conversions[i]));
        }
        candidates->setGlobalCursorIndex(0);
        ic_->inputPanel().setAuxUp(Text(_("Convert")));
        ic_->inputPanel().setCandidateList(std::move(candidates));
        ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
        pickerOpen_ = true;
    }

    void commitConversion(const std::string &text) {
        closePicker();
        changeApplicationText(convertDelete_, text);
        convertDelete_ = 0;
    }

    // ibus-bamboo's Shift+~ table choosing the typing mode of the program.
    void openPicker() {
        commitBuffer();
        auto candidates = std::make_unique<CommonCandidateList>();
        candidates->setLayoutHint(CandidateLayoutHint::Vertical);
        candidates->setPageSize(8);
        const auto current = engine_->inputMode(ic_);
        std::vector<std::string> labels;
        int cursor = 0;
        for (size_t i = 0; i < BambooInputModeI18NAnnotation::enumLength; i++) {
            const auto mode = static_cast<BambooInputMode>(i);
            if (mode == current) {
                cursor = static_cast<int>(labels.size());
            }
            labels.push_back(mode == current
                                 ? "*. "
                                 : std::to_string(labels.size() + 1) + ". ");
            candidates->append<InputModeCandidateWord>(engine_, mode);
        }
        candidates->setLabels(labels);
        candidates->setCursorIndex(cursor);
        ic_->inputPanel().setAuxUp(Text(
            stringutils::concat(_("Typing mode for"), " ", ic_->program())));
        ic_->inputPanel().setCandidateList(std::move(candidates));
        ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
        pickerOpen_ = true;
    }

    void closePicker() {
        pickerOpen_ = false;
        ic_->inputPanel().reset();
        ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
    }

private:
    // The input method's name with the typing mode and what it does, for
    // label_: refreshLabel and labelShown must agree on it.
    std::tuple<std::string, BambooInputMode, BambooInputMode>
    currentLabel() const {
        return {*engine_->config().inputMethod, effectiveMode(), methodMode()};
    }

    // Deletes count characters before the cursor, with BackSpace keys or
    // through the surrounding text, then commits text. What the application
    // reported is stale until it reports again.
    void changeApplicationText(int count, std::string text,
                               bool backSpaces = false) {
        if (count <= 0 && text.empty()) {
            return;
        }
        surroundingFresh_ = false;
        const auto &surroundingText = ic_->surroundingText();
        // Chrome takes a deletion from the anchor of the selection, and
        // drops it: its address bar's suggestion is selected after the
        // cursor. BackSpace takes the suggestion, then the characters, in
        // order with commits.
        const bool suggestion =
            ic_->frontendName() == "wayland" && surroundingText.isValid() &&
            surroundingText.anchor() > surroundingText.cursor();
        if (backSpaces && ic_->frontendName() != "wayland") {
            text.insert(0, std::max(count, 0), '\x7f');
        } else if (backSpaces) {
            for (int i = 0; i < count + (count > 0 && suggestion); i++) {
                ic_->forwardKey(Key(FcitxKey_BackSpace));
            }
        } else if (count > 0) {
            if (suggestion) {
                for (int i = 0; i <= count; i++) {
                    ic_->forwardKey(Key(FcitxKey_BackSpace));
                }
            } else {
                ic_->deleteSurroundingText(-count, count);
            }
        }
        if (text.empty()) {
            return;
        }
        // Terminals like Alacritty paste a commit longer than a character,
        // and applications such as Claude Code lose a paste that more keys
        // follow: characters go in one by one like typed ones.
        if (engine_->isTerminal(ic_) && utf8::validate(text)) {
            for (auto chr : utf8::MakeUTF8StringViewRange(text)) {
                ic_->commitString(std::string(chr));
            }
        } else {
            ic_->commitString(text);
        }
    }

    // Keys since a sentence ended: '.', '!' or '?', then spaces or Return.
    enum class SentenceKeys { Other, End, Start };

    SentenceKeys nextSentenceKeys(const Key &key) const {
        if (key.states().testAny(
                KeyStates{KeyState::Ctrl, KeyState::Alt, KeyState::Super})) {
            return SentenceKeys::Other;
        }
        switch (key.sym()) {
        case FcitxKey_period:
        case FcitxKey_exclam:
        case FcitxKey_question:
            return SentenceKeys::End;
        case FcitxKey_space:
            return sentenceKeys_ == SentenceKeys::Other ? SentenceKeys::Other
                                                        : SentenceKeys::Start;
        case FcitxKey_Return:
        case FcitxKey_KP_Enter:
            return SentenceKeys::Start;
        default:
            return SentenceKeys::Other;
        }
    }

    // A lowercase letter starting a word at the start of a sentence. The
    // application's text tells it best, then the keys typed.
    bool capitalize(KeySym sym, KeyStates states, bool fresh) const {
        if (!*engine_->config().capitalizeSentences || sym < FcitxKey_a ||
            sym > FcitxKey_z ||
            states.testAny(
                KeyStates{KeyState::Ctrl, KeyState::Alt, KeyState::Super}) ||
            engine_->isTerminal(ic_) ||
            ic_->capabilityFlags().testAny(CapabilityFlags{
                CapabilityFlag::NoAutoUpperCase, CapabilityFlag::Url}) ||
            EngineIsTypingKey(bambooEngine_.handle(), sym, states)) {
            return false;
        }
        if (fresh && textKnown() &&
            ic_->surroundingText().cursor() ==
                ic_->surroundingText().anchor()) {
            return startsSentence(textBeforeCursor());
        }
        return sentenceKeys_ == SentenceKeys::Start;
    }

    static std::string lastCharacter(std::string_view text) {
        auto last = text.size();
        while (last > 0 && (text[--last] & 0xc0) == 0x80) {
        }
        return std::string(text.substr(last));
    }

    // Empty when the application reports no text.
    std::string_view textBeforeCursor() const {
        return textBefore(ic_->surroundingText().cursor());
    }
    std::string_view textBefore(unsigned int position) const {
        const auto &surroundingText = ic_->surroundingText();
        const auto &text = surroundingText.text();
        const auto length = utf8::lengthValidated(text);
        if (!surroundingText.isValid() || length == utf8::INVALID_LENGTH ||
            position > length) {
            return {};
        }
        return std::string_view(text).substr(
            0, utf8::ncharByteLength(text.begin(), position));
    }

    // The word as the application shows it.
    std::string surroundingWord() const {
        UniqueCPtr<char> word(EngineSurroundingWord(bambooEngine_.handle()));
        return word ? word.get() : "";
    }

    // Whether the text before the cursor ends with the word, and what came
    // before it: a late report of "cho " passes for "o" otherwise.
    bool surroundingInSync(const std::string &word) const {
        return word.empty() || textBeforeCursor().ends_with(
                                   stringutils::concat(separator_, word));
    }

    // Chrome reports its text late, typing fast by several keys, and
    // deletes around the text it reported last, see
    // ZwpTextInputV3Impl::OnDone: deleting before it reports our last edit
    // loses the deletion ("bài" gave "baiài"), and starting a new word types
    // a tone key as a digit ("nguòi7" for "người"). Keys wait for its report
    // a short while, on while it reports the text as one of our last edits
    // left it (a loaded machine, "kiêm3" for "kiểm") but a second at most:
    // then the application is taken to have changed its text (a click), a
    // new word starts. Twice in a row, the application does not report its
    // text right and is not waited for until it does.
    void waitForReport() {
        waitingSince_ = now(CLOCK_MONOTONIC);
        if (!heldTimeout_) {
            heldTimeout_ = engine_->instance()->eventLoop().addTimeEvent(
                CLOCK_MONOTONIC, waitingSince_ + HeldKeyTimeout, 1000,
                [this](EventSourceTime *, uint64_t) {
                    reportOverdue();
                    return true;
                });
        } else {
            heldTimeout_->setTime(waitingSince_ + HeldKeyTimeout);
        }
        heldTimeout_->setOneShot();
        doneRequests_ = 0;
        requestDone();
    }

    void reportOverdue() {
        // A report that came while keys were typed.
        if (surroundingInSync(surroundingWord())) {
            releaseHeldKeys(0);
            return;
        }
        const auto time = now(CLOCK_MONOTONIC);
        if (time < waitingSince_ + HeldKeyMaxWait && reportBehind()) {
            FCITX_BAMBOO_DEBUG() << "report of the word behind, waiting on";
            noteBehindWait();
            heldTimeout_->setTime(std::min(time + HeldKeyTimeout,
                                           waitingSince_ + HeldKeyMaxWait));
            heldTimeout_->setOneShot();
            doneRequests_ = 0;
            requestDone();
            return;
        }
        FCITX_BAMBOO_DEBUG() << "no report of the word in time";
        if (++timeouts_ > 1) {
            trustReports_ = false;
        }
        if (bambooEngine_) {
            ResetEngine(bambooEngine_.handle());
        }
        separator_.clear();
        editTails_.clear();
        wordStart_.reset();
        releaseHeldKeys(0);
    }

    // Five keys in a minute waiting for reports behind: typing waits often,
    // the modes that do not wait are suggested. Address bars wait in them
    // too, and the table of modes needs a program name.
    void noteBehindWait() {
        if (ic_->capabilityFlags().test(CapabilityFlag::Url) ||
            ic_->program().empty() ||
            (!behindWaits_.empty() && behindWaits_.back() == waitingSince_)) {
            return;
        }
        std::erase_if(behindWaits_, [this](uint64_t when) {
            return when + 60000000 < waitingSince_;
        });
        behindWaits_.push_back(waitingSince_);
        if (behindWaits_.size() >= 5) {
            behindWaits_.clear();
            engine_->suggestModesNotWaiting(ic_);
        }
    }

    // Chrome reports the text it holds back only after done, which KWin
    // sends for a preedit update, even an empty one. An empty commit would
    // delete the selection in GTK entries.
    void requestDone() {
        if (ic_->frontendName() == "wayland") {
            ic_->updatePreedit(true);
        }
    }

    // Keys that can wait and be typed later as they were: characters, and a
    // lone Shift or CapsLock. KWin forwards keys without their modifiers.
    static bool holdable(const Key &key) {
        const auto sym = key.sym();
        if (sym == FcitxKey_Shift_L || sym == FcitxKey_Shift_R ||
            sym == FcitxKey_Caps_Lock) {
            return true;
        }
        const auto chr = Key::keySymToUnicode(sym);
        return chr >= 0x20 && chr != 0x7f &&
               !key.states().testAny(KeyStates{KeyState::Ctrl, KeyState::Alt,
                                               KeyState::Super, KeyState::Hyper,
                                               KeyState::Meta});
    }

    // A held key the input method lets through, its event gone: its
    // character goes in in order with our commits.
    void typeForApplication(const Key &key) {
        if (const auto chr = Key::keySymToUnicode(key.sym());
            chr >= 0x20 && chr != 0x7f) {
            changeApplicationText(0, utf8::UCS4ToUTF8(chr));
        }
    }

    // Returns false when the key should go on as normal typing.
    bool pickerKeyEvent(KeyEvent &keyEvent) {
        auto candidates = ic_->inputPanel().candidateList();
        const auto &key = keyEvent.key();
        if (!candidates ||
            checkHotkey(keyEvent, *engine_->config().inputModeSwitchKey)) {
            // Like ibus-bamboo, pressed twice the key reaches the application.
            closePicker();
            return candidates != nullptr;
        }
        if (checkHotkey(keyEvent, *engine_->config().convertKey)) {
            closePicker();
            keyEvent.filterAndAccept();
            return true;
        }
        int index = key.digitSelection();
        if (key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter)) {
            index = candidates->cursorIndex();
        }
        if (index >= 0 && index < candidates->size()) {
            keyEvent.filterAndAccept();
            candidates->candidate(index).select(ic_);
            return true;
        }
        const bool up = key.check(FcitxKey_Up) || key.check(FcitxKey_Left);
        if (up || key.check(FcitxKey_Down) || key.check(FcitxKey_Right)) {
            auto *movable = candidates->toCursorMovable();
            up ? movable->prevCandidate() : movable->nextCandidate();
            ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
            keyEvent.filterAndAccept();
            return true;
        }
        closePicker();
        if (key.check(FcitxKey_Escape)) {
            keyEvent.filterAndAccept();
            return true;
        }
        return false;
    }

    BambooEngine *engine_;
    InputContext *ic_;
    CGoObject bambooEngine_;
    bool pickerOpen_ = false;
    Method lastMethod_ = Method::Preedit;
    bool surroundingFresh_ = false;
    bool lastKeyToApp_ = true;
    bool textReported_ = false;
    bool hintedTextReported_ = false;
    // The input method's name with the typing mode and what it does, as the
    // label last told them, see currentLabel.
    std::tuple<std::string, BambooInputMode, BambooInputMode> label_;
    bool processing_ = false;
    bool releasing_ = false;
    // What the application got right before the word, see
    // surroundingInSync.
    std::string separator_;
    std::tuple<std::string, unsigned int, unsigned int> lastReport_;
    bool trustReports_ = true;
    int timeouts_ = 0;
    int doneRequests_ = 0;
    // Keys waiting for the application to report our last edit, oldest
    // first, see waitForReport.
    std::deque<Key> heldKeys_;
    std::unique_ptr<EventSourceTime> heldTimeout_;
    uint64_t waitingSince_ = 0;
    // When keys waiting for reports behind lately started to, see
    // noteBehindWait.
    std::vector<uint64_t> behindWaits_;
    // How the text before the cursor ends after each of our last edits,
    // oldest first, and what it was before the word, see reportBehind.
    std::deque<std::string> editTails_;
    std::optional<std::string> wordStart_;
    SentenceKeys sentenceKeys_ = SentenceKeys::Other;
    // Characters before the cursor a conversion replaces.
    int convertDelete_ = 0;
};

namespace {
void ConvertCandidateWord::select(InputContext * /*inputContext*/) const {
    state_->commitConversion(result_);
}
} // namespace

BambooEngine::BambooEngine(Instance *instance)
    : instance_(instance), factory_([this](InputContext &ic) {
          return new BambooState(this, &ic);
      }) {
    Init();
    {
        auto imNames = convertToStringList(GetInputMethodNames());
        imNames.push_back("Custom");
        imNames_ = std::move(imNames);
    }
    if (std::find(imNames_.begin(), imNames_.end(), "Telex") ==
        imNames_.end()) {
        throw std::runtime_error("Failed to find required input method Telex");
    }
    FCITX_BAMBOO_DEBUG() << "Supported input methods: " << imNames_;
    config_.inputMethod.annotation().setList(imNames_);

    auto fd = StandardPaths::global().open(StandardPathsType::PkgData,
                                           "bamboo/vietnamese.cm.dict");
    if (!fd.isValid()) {
        throw std::runtime_error("Failed to load dictionary");
    }
    dictionary_.reset(NewDictionary(fd.release()));

    auto &uiManager = instance_->userInterfaceManager();
    inputMethodAction_ = std::make_unique<SimpleAction>();
    inputMethodAction_->setIcon("document-edit");
    inputMethodAction_->setShortText(_("Input Method"));
    uiManager.registerAction("bamboo-input-method", inputMethodAction_.get());

    inputMethodMenu_ = std::make_unique<Menu>();
    inputMethodAction_->setMenu(inputMethodMenu_.get());
    for (const auto &imName : imNames_) {
        inputMethodSubAction_.emplace_back(std::make_unique<SimpleAction>());
        auto *action = inputMethodSubAction_.back().get();
        action->setShortText(imName);
        action->setCheckable(true);
        uiManager.registerAction(
            stringutils::concat(InputMethodActionPrefix, imName), action);
        connections_.emplace_back(action->connect<SimpleAction::Activated>(
            [this, imName](InputContext *ic) {
                if (config_.inputMethod.value() == imName) {
                    return;
                }
                config_.inputMethod.setValue(imName);
                saveConfig();
                refreshEngine();
                updateInputMethodAction(ic);
            }));

        inputMethodMenu_->addAction(action);
    }

    charsetAction_ = std::make_unique<SimpleAction>();
    charsetAction_->setShortText(_("Output charset"));
    charsetAction_->setIcon("character-set");
    uiManager.registerAction("bamboo-charset", charsetAction_.get());
    charsetMenu_ = std::make_unique<Menu>();
    charsetAction_->setMenu(charsetMenu_.get());

    auto charsets = convertToStringList(GetCharsetNames());
    for (const auto &charset : charsets) {
        charsetSubAction_.emplace_back(std::make_unique<SimpleAction>());
        auto *action = charsetSubAction_.back().get();
        action->setShortText(charset);
        action->setCheckable(true);
        connections_.emplace_back(action->connect<SimpleAction::Activated>(
            [this, charset](InputContext *ic) {
                if (config_.outputCharset.value() == charset) {
                    return;
                }
                config_.outputCharset.setValue(charset);
                saveConfig();
                refreshEngine();
                updateCharsetAction(ic);
            }));
        uiManager.registerAction(
            stringutils::concat(CharsetActionPrefix, charset), action);
        charsetMenu_->addAction(action);
    }
    config_.outputCharset.annotation().setList(charsets);

    spellCheckAction_ = std::make_unique<SimpleAction>();
    spellCheckAction_->setLongText(_("Spell check"));
    spellCheckAction_->setIcon("tools-check-spelling");
    connections_.emplace_back(
        spellCheckAction_->connect<SimpleAction::Activated>(
            [this](InputContext *ic) {
                config_.autoNonVnRestore.setValue(!*config_.autoNonVnRestore);
                saveConfig();
                refreshOption();
                updateSpellAction(ic);
            }));
    uiManager.registerAction("bamboo-spell-check", spellCheckAction_.get());
    macroAction_ = std::make_unique<SimpleAction>();
    macroAction_->setLongText(_("Macro"));
    macroAction_->setIcon("edit-find");
    connections_.emplace_back(macroAction_->connect<SimpleAction::Activated>(
        [this](InputContext *ic) {
            config_.macro.setValue(!*config_.macro);
            saveConfig();
            refreshOption();
            updateMacroAction(ic);
        }));
    uiManager.registerAction("bamboo-macro", macroAction_.get());

    reloadConfig();
    instance_->inputContextManager().registerProperty("bambooState", &factory_);
    eventWatchers_.emplace_back(instance_->watchEvent(
        EventType::InputContextSurroundingTextUpdated,
        EventWatcherPhase::PostInputMethod, [this](Event &event) {
            static_cast<InputContextEvent &>(event)
                .inputContext()
                ->propertyFor(&factory_)
                ->surroundingTextUpdated();
        }));
    eventWatchers_.emplace_back(instance_->watchEvent(
        EventType::InputContextFocusIn, EventWatcherPhase::PreInputMethod,
        [this](Event &event) {
            static_cast<InputContextEvent &>(event)
                .inputContext()
                ->propertyFor(&factory_)
                ->focusIn();
        }));
    eventWatchers_.emplace_back(instance_->watchEvent(
        EventType::InputContextCapabilityChanged,
        EventWatcherPhase::PostInputMethod, [this](Event &event) {
            auto &changed = static_cast<CapabilityChangedEvent &>(event);
            auto *ic = changed.inputContext();
            if (ic->hasFocus() && instance_->inputMethodEngine(ic) == this) {
                ic->propertyFor(&factory_)->capabilityChanged(
                    changed.oldFlags());
            }
        }));
}

void BambooEngine::reloadConfig() {
    readAsIni(config_, "conf/bamboo.conf");
    readAsIni(customKeymap_, CustomKeymapFile);
    readAsIni(appModes_, AppModeFile);
    for (const auto &imName : imNames_) {
        auto &table = macroTables_[imName];
        readAsIni(table, macroFile(imName));
        macroTableObject_[imName].reset(newMacroTable(table));
    }

    populateConfig();
}

const Configuration *BambooEngine::getSubConfig(const std::string &path) const {
    if (path == "custom_keymap") {
        return &customKeymap_;
    }
    if (path == "app_modes") {
        return &appModes_;
    }
    if (path.starts_with(MacroPrefix)) {
        const auto imName = path.substr(MacroPrefix.size());
        if (auto iter = macroTables_.find(imName); iter != macroTables_.end()) {
            return &iter->second;
        }
        return nullptr;
    }
    return nullptr;
}

void BambooEngine::setConfig(const RawConfig &config) {
    config_.load(config, true);
    saveConfig();
    populateConfig();
}

void BambooEngine::populateConfig() {
    refreshEngine();
    refreshOption();
    updateMacroAction(nullptr);
    updateSpellAction(nullptr);
    updateInputMethodAction(nullptr);
    updateCharsetAction(nullptr);
}

void BambooEngine::setSubConfig(const std::string &path,
                                const RawConfig &config) {
    if (path == "custom_keymap") {
        customKeymap_.load(config, true);
        safeSaveAsIni(customKeymap_, CustomKeymapFile);
        refreshEngine();
    } else if (path == "app_modes") {
        appModes_.load(config, true);
        safeSaveAsIni(appModes_, AppModeFile);
        refreshOption();
    } else if (path.starts_with(MacroPrefix)) {
        const auto imName = path.substr(MacroPrefix.size());
        if (auto iter = macroTables_.find(imName); iter != macroTables_.end()) {
            iter->second.load(config, true);
            safeSaveAsIni(iter->second, macroFile(imName));
            macroTableObject_[imName].reset(newMacroTable(iter->second));
            refreshEngine();
        }
    }
}

const BambooAppMode *BambooEngine::appMode(const std::string &program) const {
    if (program.empty()) {
        return nullptr;
    }
    const auto &appModes = *appModes_.appModes;
    auto iter = std::ranges::find_if(appModes, [&program](const auto &appMode) {
        return *appMode.program == program;
    });
    return iter == appModes.end() ? nullptr : &*iter;
}

// A kind's modes are the typing modes after Default, in their order.
constexpr bool kindModesFollowInputModes(size_t i = 0) {
    return i == BambooInputModeI18NAnnotation::enumLength ||
           (stringutils::literalEqual(_BambooKindMode_Names[i + 1],
                                      _BambooInputMode_Names[i]) &&
            kindModesFollowInputModes(i + 1));
}
static_assert(BambooKindModeI18NAnnotation::enumLength ==
                  BambooInputModeI18NAnnotation::enumLength + 1 &&
              kindModesFollowInputModes());

BambooInputMode BambooEngine::inputMode(InputContext *ic) const {
    if (const auto *entry = appMode(ic->program())) {
        return *entry->mode;
    }
    const auto &kinds = *config_.kindModes;
    auto kind = BambooKindMode::Default;
    if (ic->capabilityFlags().test(CapabilityFlag::GetIMInfoOnFocus)) {
        kind = isQtTerminal(ic) ? *kinds.qtTerminals : *kinds.qtApps;
    } else if (ic->frontendName() == "dbus") {
        kind = *kinds.gtkApps;
    } else if (ic->frontendName() == "wayland") {
        kind = *kinds.waylandApps;
    } else if (ic->frontendName() == "xim") {
        kind = *kinds.x11Apps;
    }
    return kind == BambooKindMode::Default
               ? *config_.inputMode
               : static_cast<BambooInputMode>(static_cast<int>(kind) - 1);
}

// fcitx5-qt drops the SurroundingText flag before every key, the text
// comes with the next report: on a change, or as a field gets focus (Qt
// Widgets on a click or Tab, not on a window switch). Konsole reports none,
// see BambooState::terminalHints: a field with its hints counts as a
// terminal until it, or another such field of its window, reports its text.
bool BambooEngine::isQtTerminal(InputContext *ic) const {
    const auto flags = ic->capabilityFlags();
    if (!flags.test(CapabilityFlag::GetIMInfoOnFocus)) {
        return false;
    }
    auto *state = ic->propertyFor(&factory_);
    return isTerminal(ic) ||
           (!state->textReported() && !state->hintedTextReported() &&
            BambooState::terminalHints(flags));
}

bool BambooEngine::isTerminal(const InputContext *ic) const {
    const auto *entry = appMode(ic->program());
    return ic->capabilityFlags().test(CapabilityFlag::Terminal) ||
           (entry && *entry->terminal);
}

void BambooEngine::setInputMode(InputContext *ic, BambooInputMode mode) {
    auto &appModes = *appModes_.appModes.mutableValue();
    auto iter = std::ranges::find_if(appModes, [ic](const auto &appMode) {
        return *appMode.program == ic->program();
    });
    if (iter == appModes.end()) {
        iter = appModes.emplace(appModes.end());
        iter->program.setValue(ic->program());
    }
    iter->mode.setValue(mode);
    safeSaveAsIni(appModes_, AppModeFile);
    auto *state = ic->propertyFor(&factory_);
    state->closePicker();
    state->refreshLabel();
}

std::string BambooEngine::subMode(const fcitx::InputMethodEntry & /*entry*/,
                                  fcitx::InputContext &inputContext) {
    const auto *state = inputContext.propertyFor(&factory_);
    const auto mode = state->effectiveMode();
    if (mode == BambooInputMode::Preedit) {
        return *config_.inputMethod;
    }
    // What the mode does in this field, when it does something else. No
    // colons: kimpanel sends panels its status in fields split on them.
    const auto doing = state->methodMode();
    auto label = BambooInputModeI18NAnnotation::toString(mode);
    if (doing != mode) {
        label = stringutils::concat(
            label, " → ", BambooInputModeI18NAnnotation::toString(doing));
    }
    return stringutils::concat(*config_.inputMethod, " (", label, ")");
}

void BambooEngine::suggestModesNotWaiting(InputContext *ic) {
    if (!suggestedPrograms_.insert(ic->program()).second) {
        return;
    }
    std::string key;
    for (const auto &switchKey : *config_.inputModeSwitchKey) {
        if (key = switchKey.toString(KeyStringFormat::Localized);
            !key.empty()) {
            break;
        }
    }
    // BackSpace keeps the order of keys and text where KWin hands them to
    // Chromium, not to Qt and GTK applications.
    auto text =
        key.empty()
            ? _("%1 reports its text late, typing waits for it. Typing modes "
                "that do not wait, in Typing Mode per Application in the "
                "configuration: BackSpace for Chromium-based applications, "
                "Input Method Window for any.")
            : stringutils::replaceAll(
                  _("%1 reports its text late, typing waits for it. Typing "
                    "modes that do not wait, in the table of typing modes "
                    "(%2): BackSpace for Chromium-based applications, Input "
                    "Method Window for any."),
                  "%2", key);
    text = stringutils::replaceAll(text, "%1", ic->program());
    FCITX_BAMBOO_DEBUG() << "suggesting modes that do not wait: " << text;
    if (auto *notifications = this->notifications()) {
        notifications->call<INotifications::showTip>(
            "bamboo-backspace-mode", _("Bamboo"), "fcitx_bamboo",
            _("Typing waits for the application"), text, -1);
    }
}

std::string BambooEngine::subModeLabelImpl(const InputMethodEntry & /*entry*/,
                                           InputContext &inputContext) {
    return inputContext.propertyFor(&factory_)->effectiveMode() ==
                   BambooInputMode::Exclude
               ? "EN"
               : "VI";
}

void BambooEngine::activate(const InputMethodEntry &entry,
                            InputContextEvent &event) {
    FCITX_UNUSED(entry);
    event.inputContext()->propertyFor(&factory_)->labelShown();
    auto &statusArea = event.inputContext()->statusArea();

    updateMacroAction(event.inputContext());
    updateSpellAction(event.inputContext());
    updateInputMethodAction(event.inputContext());
    updateCharsetAction(event.inputContext());

    statusArea.addAction(StatusGroup::InputMethod, inputMethodAction_.get());
    statusArea.addAction(StatusGroup::InputMethod, charsetAction_.get());
    statusArea.addAction(StatusGroup::InputMethod, spellCheckAction_.get());
    statusArea.addAction(StatusGroup::InputMethod, macroAction_.get());
}

void BambooEngine::deactivate(const InputMethodEntry &entry,
                              InputContextEvent &event) {
    FCITX_UNUSED(entry);
    auto *state = event.inputContext()->propertyFor(&factory_);
    if (event.type() != EventType::InputContextFocusOut) {
        state->typeHeldKeys();
        state->commitBuffer();
    } else {
        state->reset();
    }
}

void BambooEngine::keyEvent(const InputMethodEntry &entry, KeyEvent &keyEvent) {
    FCITX_UNUSED(entry);
    auto *state = keyEvent.inputContext()->propertyFor(&factory_);

    state->keyEvent(keyEvent);
}

void BambooEngine::reset(const InputMethodEntry &entry,
                         InputContextEvent &event) {
    FCITX_UNUSED(entry);
    auto *state = event.inputContext()->propertyFor(&factory_);
    state->reset();
}

void BambooEngine::refreshEngine() {
    FCITX_BAMBOO_DEBUG() << "Refresh engine";
    if (!factory_.registered()) {
        return;
    }

    instance_->inputContextManager().foreach([this](InputContext *ic) {
        auto *state = ic->propertyFor(&factory_);
        state->setEngine();
        if (ic->hasFocus()) {
            state->reset();
        }
        return true;
    });
}

void BambooEngine::refreshOption() {
    if (!factory_.registered()) {
        return;
    }
    instance_->inputContextManager().foreach([this](InputContext *ic) {
        auto *state = ic->propertyFor(&factory_);
        state->setOption();
        if (ic->hasFocus()) {
            state->reset();
        }
        return true;
    });
}

void BambooEngine::updateSpellAction(InputContext *ic) {
    spellCheckAction_->setChecked(*config_.autoNonVnRestore);
    spellCheckAction_->setShortText(*config_.autoNonVnRestore
                                        ? _("Spell Check Enabled")
                                        : _("Spell Check Disabled"));
    if (ic) {
        spellCheckAction_->update(ic);
    }
}

void BambooEngine::updateMacroAction(InputContext *ic) {
    macroAction_->setChecked(*config_.macro);
    macroAction_->setShortText(*config_.macro ? _("Macro Enabled")
                                              : _("Macro Disabled"));
    if (ic) {
        macroAction_->update(ic);
    }
}

void BambooEngine::updateInputMethodAction(InputContext *ic) {
    auto name =
        stringutils::concat(InputMethodActionPrefix, *config_.inputMethod);
    for (const auto &action : inputMethodSubAction_) {
        action->setChecked(action->name() == name);
        if (ic) {
            action->update(ic);
        }
    }
}

void BambooEngine::updateCharsetAction(InputContext *ic) {
    auto name =
        stringutils::concat(CharsetActionPrefix, *config_.outputCharset);
    for (const auto &action : charsetSubAction_) {
        action->setChecked(action->name() == name);
        if (ic) {
            action->update(ic);
        }
    }
}

} // namespace fcitx

FCITX_ADDON_FACTORY_V2(bamboo, fcitx::BambooFactory)
