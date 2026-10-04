/*
 * SPDX-FileCopyrightText: 2026 googlesky
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */
#include <cstdint>
#include <ctime>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/eventloopinterface.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/macros.h>
#include <fcitx-utils/stringutils.h>
#include <fcitx-utils/testing.h>
#include <fcitx-utils/textformatflags.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/action.h>
#include <fcitx/addonmanager.h>
#include <fcitx/event.h>
#include <fcitx/globalconfig.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>
#include <fcitx/userinterfacemanager.h>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace fcitx;

namespace {

// Stands in for an application with the cursor at the end of its text:
// applies what fcitx sends, handles the keys fcitx lets through and reports
// its text as surrounding text, unless told not to like a Wayland terminal.
class FakeEditor : public InputContext {
public:
    FakeEditor(Instance *instance, const std::string &program,
               CapabilityFlags caps, bool reportSurrounding = true,
               const char *frontend = "bambootest")
        : InputContext(instance->inputContextManager(), program),
          reportSurrounding_(reportSurrounding), frontend_(frontend) {
        created();
        setCapabilityFlags(caps);
        syncSurrounding();
        focusIn();
        instance->setCurrentInputMethod(this, "bamboo", true);
    }
    ~FakeEditor() override { destroy(); }

    const char *frontend() const override { return frontend_; }

    // Returns whether fcitx filtered the key.
    bool press(const Key &key) {
        if (qt_) {
            // fcitx5-qt updates the hints before every key, dropping the
            // flag, and reports the text again once the key is handled.
            setCapabilityFlags(
                capabilityFlags().unset(CapabilityFlag::SurroundingText));
            pressing_ = true;
        }
        KeyEvent event(this, key);
        const bool filtered = keyEvent(event);
        pressing_ = false;
        if (filtered) {
            if (qt_) {
                syncSurrounding();
            }
            return true;
        }
        if (key.check(FcitxKey_BackSpace)) {
            // BackSpace takes the suggestion away first.
            if (!suggestion_.empty()) {
                suggestion_.clear();
            } else if (!text_.empty()) {
                text_.pop_back();
            }
        } else if (key.check(FcitxKey_Return)) {
            text_.insert(text_.end(), suggestion_.begin(), suggestion_.end());
            suggestion_.clear();
            text_.push_back('\n');
        } else if (!key.states().testAny(KeyStates{
                       KeyState::Ctrl, KeyState::Alt, KeyState::Super})) {
            // Control characters like Escape's are not typed.
            if (auto chr = Key::keySymToUnicode(key.sym()); chr >= 0x20) {
                text_.push_back(chr);
                suggest();
            }
        }
        syncSurrounding();
        return false;
    }

    // Like an address bar: after each change the application suggests how
    // the text goes on, selected after the cursor. Return takes it.
    void setSuggestion(const std::string &suggestion) {
        completion_.clear();
        for (auto c : utf8::MakeUTF8CharRange(suggestion)) {
            completion_.push_back(c);
        }
        suggest();
        syncSurrounding();
    }

    // The application edits its text on its own, like an autocorrection.
    void replaceText(const std::string &text) {
        text_.clear();
        commitStringImpl(text);
    }

    // Selects the last n characters: typing replaces them.
    void selectBack(size_t n) {
        anchor_ = text_.size() - n;
        syncSurrounding();
    }

    // Like a terminal: DEL deletes the character before the cursor.
    void setTerminal() { terminal_ = true; }

    // Like fcitx5-qt, focus going to a field of the window, which reports
    // its text then (Qt Widgets on a click) or not (window switches,
    // Konsole). The flag goes with the hints updated after focus.
    void focusQt(bool report) {
        qt_ = true;
        focusOut();
        focusIn();
        if (report) {
            syncSurrounding();
        }
        setCapabilityFlags(
            capabilityFlags().unset(CapabilityFlag::SurroundingText));
    }

    // Text after the cursor the application does not select: a Multiline
    // field reporting more of its paragraph than what selectBack selected.
    void setTextAfterCursor(const std::string &text) {
        after_.clear();
        for (auto c : utf8::MakeUTF8CharRange(text)) {
            after_.push_back(c);
        }
        syncSurrounding();
    }

    // Like an application reporting its text late, if at all.
    void setReportSurrounding(bool report) { reportSurrounding_ = report; }
    void report() { syncSurrounding(); }
    void reportText(const std::string &text) {
        surroundingText().setText(text, utf8::length(text), utf8::length(text));
        updateSurroundingText();
    }

    // Types ASCII keys one by one.
    void type(std::string_view keys) {
        for (char c : keys) {
            press(Key(static_cast<KeySym>(c)));
        }
    }

    std::string text() const {
        std::string result;
        for (auto c : text_) {
            result += utf8::UCS4ToUTF8(c);
        }
        for (auto c : suggestion_) {
            result += utf8::UCS4ToUTF8(c);
        }
        return result;
    }
    std::string preedit() { return inputPanel().clientPreedit().toString(); }
    // The most characters committed at once.
    size_t longestCommit() const { return longestCommit_; }
    int forwardedKeys() const { return forwardedKeys_; }
    // Preedit shown in fcitx5's window.
    std::string panelPreedit() { return inputPanel().preedit().toString(); }

protected:
    void commitStringImpl(const std::string &str) override {
        if (utf8::length(str) > longestCommit_) {
            longestCommit_ = utf8::length(str);
        }
        if (anchor_ < text_.size()) {
            text_.resize(anchor_);
        }
        anchor_ = NoSelection;
        for (auto c : utf8::MakeUTF8CharRange(str)) {
            if (terminal_ && c == 0x7f) {
                if (!text_.empty()) {
                    text_.pop_back();
                }
            } else {
                text_.push_back(c);
            }
        }
        suggest();
        syncSurrounding();
    }
    void deleteSurroundingTextImpl(int offset, unsigned int size) override {
        FCITX_ASSERT(offset == -static_cast<int>(size) && size <= text_.size())
            << "bad delete " << offset << " " << size << " on " << text();
        // Wayland frontends count bytes in their copy of the text, and
        // Chrome deletes around the text it reported last: nothing when it
        // is not the text anymore, nor around its address bar's suggestion,
        // a selection it takes from its anchor.
        if (std::string_view(frontend_).starts_with("wayland")) {
            if (!suggestion_.empty()) {
                return;
            }
            std::string before;
            for (auto c : text_) {
                before += utf8::UCS4ToUTF8(c);
            }
            const auto &copy = surroundingText().text();
            if (!surroundingText().isValid() ||
                copy.substr(0, utf8::ncharByteLength(
                                   copy.begin(), surroundingText().cursor())) !=
                    before) {
                return;
            }
        }
        // Chrome deletes the selection too.
        text_.resize(text_.size() - size);
        suggest();
        syncSurrounding();
    }
    // Like KWin handing a forwarded key to the application.
    void forwardKeyImpl(const ForwardKeyEvent &event) override {
        if (!event.isRelease()) {
            forwardedKeys_++;
        }
        if (!event.isRelease() && event.rawKey().check(FcitxKey_BackSpace)) {
            // BackSpace takes the suggestion away first.
            if (!suggestion_.empty()) {
                suggestion_.clear();
            } else if (!text_.empty()) {
                text_.pop_back();
            }
            syncSurrounding();
        }
    }
    void updatePreeditImpl() override {}

private:
    void suggest() {
        suggestion_ = text_.empty() ? std::vector<uint32_t>{} : completion_;
    }

    void syncSurrounding() {
        if (reportSurrounding_ && !pressing_) {
            if (qt_) {
                setCapabilityFlags(capabilityFlags() |
                                   CapabilityFlag::SurroundingText);
            }
            auto anchor = text_.size() + suggestion_.size();
            if (anchor_ < text_.size()) {
                anchor = anchor_;
            }
            std::string full = text();
            for (auto c : after_) {
                full += utf8::UCS4ToUTF8(c);
            }
            surroundingText().setText(full, text_.size(), anchor);
            updateSurroundingText();
        }
    }

    static constexpr size_t NoSelection = -1;
    std::vector<uint32_t> text_;
    std::vector<uint32_t> suggestion_; // selected after the cursor
    std::vector<uint32_t> completion_;
    std::vector<uint32_t> after_; // unselected, after the cursor
    size_t anchor_ = NoSelection;
    size_t longestCommit_ = 0;
    int forwardedKeys_ = 0;
    bool terminal_ = false;
    bool qt_ = false;
    bool pressing_ = false;
    bool reportSurrounding_;
    const char *frontend_;
};

const CapabilityFlags PreeditCaps{CapabilityFlag::Preedit,
                                  CapabilityFlag::SurroundingText};

// Counts the updates of the status area, on which panels ask for the input
// method's label again.
class StatusUpdates {
public:
    explicit StatusUpdates(Instance *instance)
        : watcher_(instance->watchEvent(
              EventType::InputContextUpdateUI, EventWatcherPhase::Default,
              [this](Event &event) {
                  if (static_cast<InputContextUpdateUIEvent &>(event)
                          .component() == UserInterfaceComponent::StatusArea) {
                      count++;
                  }
              })) {}

    int count = 0;

private:
    std::unique_ptr<HandlerTableEntry<EventHandler>> watcher_;
};

// Runs steps one after another, each the given milliseconds after the one
// before: the input method's timers fire in between.
class TimedSteps {
public:
    void add(uint64_t milliseconds, std::function<void()> step) {
        steps_.emplace_back(milliseconds * 1000, std::move(step));
    }
    void run(Instance *instance) {
        timer_ = instance->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + steps_[0].first, 1,
            [this](EventSourceTime *timer, uint64_t) {
                steps_[next_++].second();
                if (next_ < steps_.size()) {
                    timer->setTime(now(CLOCK_MONOTONIC) + steps_[next_].first);
                    timer->setOneShot();
                }
                return true;
            });
        timer_->setOneShot();
    }

private:
    std::vector<std::pair<uint64_t, std::function<void()>>> steps_;
    size_t next_ = 0;
    std::unique_ptr<EventSourceTime> timer_;
};

// Sub configs load partially: a missing list node keeps the old list.
void clearList(AddonInstance *bamboo, const std::string &path,
               const std::string &list) {
    RawConfig config;
    config.get(list, true);
    bamboo->setSubConfig(path, config);
}

void setup(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo", true);
    FCITX_ASSERT(bamboo);
    auto group = instance->inputMethodManager().currentGroup();
    group.inputMethodList().clear();
    group.inputMethodList().push_back(InputMethodGroupItem("keyboard-us"));
    group.inputMethodList().push_back(InputMethodGroupItem("bamboo"));
    group.setDefaultInputMethod("");
    instance->inputMethodManager().setGroup(group);
}

void testPreedit(Instance *instance) {
    FakeEditor editor(instance, "testapp", PreeditCaps);
    editor.type("tieengs");
    FCITX_ASSERT(editor.preedit() == "tiếng") << editor.preedit();
    FCITX_ASSERT(editor.text().empty()) << editor.text();
    editor.type(" vieetj");
    FCITX_ASSERT(editor.text() == "tiếng ") << editor.text();
    FCITX_ASSERT(editor.preedit() == "việt") << editor.preedit();
    editor.press(Key(FcitxKey_Return));
    FCITX_ASSERT(editor.text() == "tiếng việt\n") << editor.text();
    FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
    // Switching input method commits the word being typed once.
    editor.type("nam");
    instance->setCurrentInputMethod(&editor, "keyboard-us", true);
    FCITX_ASSERT(editor.text() == "tiếng việt\nnam") << editor.text();
    FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
}

void testRestoreKeyStroke(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig config;
    config.setValueByPath("RestoreKeyStroke/0", "Shift+space");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        // Nothing to restore: the key belongs to the application.
        FCITX_ASSERT(!editor.press(Key("Shift+space")));
        editor.type("tooi");
        FCITX_ASSERT(editor.preedit() == "tôi") << editor.preedit();
        FCITX_ASSERT(editor.press(Key("Shift+space")));
        FCITX_ASSERT(editor.preedit() == "tooi") << editor.preedit();
        // The next key must not be swallowed.
        editor.type("a");
        FCITX_ASSERT(editor.preedit() == "tooia") << editor.preedit();
        editor.type(" ");
        FCITX_ASSERT(editor.text() == " tooia ") << editor.text();
    }
    // Lock keys are ignored, unless they are the restore key.
    config.setValueByPath("RestoreKeyStroke/0", "Shift+Caps_Lock");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("tooi");
        FCITX_ASSERT(editor.press(Key(FcitxKey_Caps_Lock, KeyState::Shift)));
        FCITX_ASSERT(editor.preedit() == "tooi") << editor.preedit();
    }
    RawConfig reset;
    reset.get("RestoreKeyStroke", true);
    bamboo->setConfig(reset);
}

void testLockKeysKeepWord(Instance *instance) {
    FakeEditor editor(instance, "testapp", PreeditCaps);
    editor.type("tie");
    FCITX_ASSERT(!editor.press(Key(FcitxKey_Caps_Lock)));
    FCITX_ASSERT(!editor.press(Key(FcitxKey_Shift_L)));
    FCITX_ASSERT(editor.text().empty()) << editor.text();
    FCITX_ASSERT(editor.preedit() == "tie") << editor.preedit();
    editor.press(Key(FcitxKey_Return));
    FCITX_ASSERT(editor.text() == "tie\n") << editor.text();
}

// The tray toggle is ibus-bamboo's "spell check": restoring invalid words.
void testSpellCheckAction(Instance *instance) {
    auto *action =
        instance->userInterfaceManager().lookupAction("bamboo-spell-check");
    FCITX_ASSERT(action);
    FakeEditor editor(instance, "testapp", PreeditCaps);
    editor.type("text ");
    action->activate(&editor);
    editor.type("text ");
    action->activate(&editor);
    editor.type("text ");
    FCITX_ASSERT(editor.text() == "text tẽt text ") << editor.text();
}

void testInputModes(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig appModes;
    appModes.setValueByPath("AppMode/0/Program", "surrounding");
    appModes.setValueByPath("AppMode/0/Mode", "Surrounding Text");
    appModes.setValueByPath("AppMode/1/Program", "excluded");
    appModes.setValueByPath("AppMode/1/Mode", "Exclude");
    bamboo->setSubConfig("app_modes", appModes);
    RawConfig config;
    config.setValueByPath("RestoreKeyStroke/0", "Shift+space");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "surrounding", PreeditCaps);
        editor.type("tieengs vieetj hoafn");
        FCITX_ASSERT(editor.text() == "tiếng việt hoàn") << editor.text();
        FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
        // The tone moves back, the application must not get this backspace.
        FCITX_ASSERT(editor.press(Key(FcitxKey_BackSpace)));
        FCITX_ASSERT(editor.text() == "tiếng việt hòa") << editor.text();
        editor.type(" tooi");
        FCITX_ASSERT(editor.press(Key("Shift+space")));
        FCITX_ASSERT(editor.text() == "tiếng việt hòa tooi") << editor.text();
        // Leaving the input method must not commit the word a second time.
        instance->setCurrentInputMethod(&editor, "keyboard-us", true);
        FCITX_ASSERT(editor.text() == "tiếng việt hòa tooi") << editor.text();
    }
    {
        // Without surrounding text support the word shows in fcitx5's
        // window: Chromium and the like underline any preedit.
        FakeEditor editor(instance, "surrounding",
                          CapabilityFlags{CapabilityFlag::Preedit});
        editor.type("tieengs");
        FCITX_ASSERT(editor.panelPreedit() == "tiếng") << editor.panelPreedit();
        FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
        FCITX_ASSERT(editor.text().empty()) << editor.text();
    }
    {
        // Wayland frontends claim surrounding text for every client, a
        // client that sends none gets the word in fcitx5's window too.
        FakeEditor editor(instance, "surrounding", PreeditCaps, false);
        editor.type("tieengs");
        FCITX_ASSERT(editor.panelPreedit() == "tiếng") << editor.panelPreedit();
        FCITX_ASSERT(editor.text().empty()) << editor.text();
    }
    {
        // Text changed behind the engine's back must not be deleted.
        FakeEditor editor(instance, "surrounding", PreeditCaps);
        editor.type("to");
        editor.replaceText("tx");
        editor.type("o");
        FCITX_ASSERT(editor.text() == "txo") << editor.text();
    }
    {
        // The word after it starts right there, whatever came before.
        FakeEditor editor(instance, "surrounding", PreeditCaps);
        editor.type("abc to");
        editor.replaceText("abc tx");
        editor.type("oo");
        FCITX_ASSERT(editor.text() == "abc txô") << editor.text();
    }
    for (const char *late : {"nguoi", "ngu"}) {
        // Typing fast, Chrome reports our last edits late, as the word was
        // or halfway through an edit: keys wait for the report. It deletes
        // around the text it reported last, "bài" gave "baiài", and a new
        // word typed "nguòi7" for "người" in VNI.
        FakeEditor editor(instance, "surrounding", PreeditCaps, true,
                          "wayland");
        editor.type("nguoi");
        editor.setReportSurrounding(false);
        editor.type("f");
        editor.reportText(late);
        editor.type("w dd");
        FCITX_ASSERT(editor.text() == "nguòi") << late << " " << editor.text();
        editor.setReportSurrounding(true);
        editor.report();
        FCITX_ASSERT(editor.text() == "người đ")
            << late << " " << editor.text();
    }
    {
        // KWin would forward a held shortcut without its modifiers: the
        // held keys are typed first, as they are, and it goes through.
        FakeEditor editor(instance, "surrounding", PreeditCaps, true,
                          "wayland");
        editor.type("abc");
        editor.setReportSurrounding(false);
        editor.type("de");
        FCITX_ASSERT(editor.text() == "abcd") << editor.text();
        FCITX_ASSERT(!editor.press(Key("Control+v")));
        FCITX_ASSERT(editor.text() == "abcde") << editor.text();
    }
    {
        // Return forces held keys out while Chrome is behind: they start a
        // new word after our last edit, a tone key deletes nothing through
        // the stale report.
        FakeEditor editor(instance, "surrounding", PreeditCaps, true,
                          "wayland");
        editor.type("ab xa");
        editor.setReportSurrounding(false);
        editor.type("aas");
        editor.press(Key(FcitxKey_Return));
        FCITX_ASSERT(editor.text() == "ab xâas\n") << editor.text();
    }
    {
        // A late report of "cho " does not pass for the next word "o".
        FakeEditor editor(instance, "surrounding", PreeditCaps, true,
                          "wayland");
        editor.type("cho");
        editor.setReportSurrounding(false);
        editor.type(" oo");
        FCITX_ASSERT(editor.text() == "cho o") << editor.text();
        editor.setReportSurrounding(true);
        editor.report();
        FCITX_ASSERT(editor.text() == "cho ô") << editor.text();
    }
    {
        // A word ends the way it started: the application's text is not
        // edited once its surrounding text is gone.
        FakeEditor editor(instance, "surrounding", PreeditCaps);
        editor.type("to");
        editor.setCapabilityFlags(CapabilityFlags{CapabilityFlag::Preedit});
        editor.type("o");
        FCITX_ASSERT(editor.text() == "to") << editor.text();
        FCITX_ASSERT(editor.panelPreedit() == "o") << editor.panelPreedit();
        editor.press(Key(FcitxKey_Return));
        FCITX_ASSERT(editor.text() == "too\n") << editor.text();
    }
    {
        FakeEditor editor(instance, "surrounding",
                          CapabilityFlags{CapabilityFlag::Preedit});
        editor.type("to");
        editor.setCapabilityFlags(PreeditCaps);
        editor.type("o");
        FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
        FCITX_ASSERT(editor.text() == "too") << editor.text();
    }
    {
        FakeEditor editor(instance, "excluded", PreeditCaps);
        FCITX_ASSERT(!editor.press(Key(FcitxKey_t)));
        editor.type("ieengs");
        FCITX_ASSERT(editor.text() == "tieengs") << editor.text();
    }
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("tieengs");
        FCITX_ASSERT(editor.preedit() == "tiếng") << editor.preedit();
    }
    config.setValueByPath("DefaultInputMode", "Surrounding Text");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("tieengs");
        FCITX_ASSERT(editor.text() == "tiếng") << editor.text();
    }
    RawConfig reset;
    reset.setValueByPath("DefaultInputMode", "Preedit");
    reset.get("RestoreKeyStroke", true);
    bamboo->setConfig(reset);
    clearList(bamboo, "app_modes", "AppMode");
}

// A key right after a word edits it like one being typed, once the
// application reported the word after a key it handled itself.
void testEditWordBeforeCursor(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig appModes;
    appModes.setValueByPath("AppMode/0/Program", "surrounding");
    appModes.setValueByPath("AppMode/0/Mode", "Surrounding Text");
    bamboo->setSubConfig("app_modes", appModes);
    for (const auto *program : {"testapp", "surrounding"}) {
        FakeEditor editor(instance, program, PreeditCaps);
        editor.type("vieet ");
        editor.press(Key(FcitxKey_BackSpace));
        editor.type("j ");
        FCITX_ASSERT(editor.text() == "việt ") << program << editor.text();
        // A click resets the input method, then the application reports.
        editor.reset();
        editor.replaceText("xin chao");
        editor.type("f ");
        FCITX_ASSERT(editor.text() == "xin chào ") << program << editor.text();
        // Until it reports, the text from before the click is not trusted.
        editor.replaceText("xin chao");
        editor.setReportSurrounding(false);
        editor.reset();
        editor.replaceText("xin ");
        editor.type("f ");
        FCITX_ASSERT(editor.text() == "xin f ") << program << editor.text();
        editor.setReportSurrounding(true);
        editor.replaceText("xin chào ");
        // The application did not report the last BackSpace yet.
        editor.type("tooi ");
        editor.press(Key(FcitxKey_BackSpace));
        editor.setReportSurrounding(false);
        editor.press(Key(FcitxKey_BackSpace));
        editor.type("s ");
        FCITX_ASSERT(editor.text() == "xin chào tôs ")
            << program << editor.text();
    }
    {
        // Typing fast, a report of the word may come after the space.
        FakeEditor editor(instance, "surrounding", PreeditCaps);
        editor.type("toi ");
        editor.reportText("toi");
        editor.type("s");
        FCITX_ASSERT(editor.text() == "toi s") << editor.text();
    }
    {
        // Wayland frontends answer from a copy of the text.
        FakeEditor editor(instance, "testapp", PreeditCaps, true, "wayland_v2");
        editor.type("vieet ");
        editor.press(Key(FcitxKey_BackSpace));
        editor.type("j ");
        FCITX_ASSERT(editor.text() == "viêtj ") << editor.text();
    }
    RawConfig config;
    config.setValueByPath("EditWordBeforeCursor", "False");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("vieet ");
        editor.press(Key(FcitxKey_BackSpace));
        editor.type("j ");
        FCITX_ASSERT(editor.text() == "viêtj ") << editor.text();
    }
    config.setValueByPath("EditWordBeforeCursor", "True");
    bamboo->setConfig(config);
    clearList(bamboo, "app_modes", "AppMode");
}

void testStandaloneW(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig config;
    config.setValueByPath("QuickTyping/StandaloneW", "Ư, but W at word start");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("w");
        FCITX_ASSERT(editor.preedit() == "w") << editor.preedit();
        editor.type(" nhwng ");
        FCITX_ASSERT(editor.text() == "w nhưng ") << editor.text();
    }
    config.setValueByPath("QuickTyping/StandaloneW",
                          "As the input method does");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("nhwng ");
        FCITX_ASSERT(editor.text() == "nhwng ") << editor.text();
    }
}

void testQuickTyping(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig config;
    config.setValueByPath("QuickTyping/EndConsonants", "True");
    config.setValueByPath("QuickTyping/DoubleConsonants", "True");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("dog ccaf happy ");
        FCITX_ASSERT(editor.text() == "dong chà happy ") << editor.text();
    }
    config.setValueByPath("QuickTyping/EndConsonants", "False");
    config.setValueByPath("QuickTyping/DoubleConsonants", "False");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("dog ");
        FCITX_ASSERT(editor.text() == "dog ") << editor.text();
    }
}

void testCapitalizeSentences(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig config;
    config.setValueByPath("CapitalizeSentences", "True");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("xin chaof. tooi ddi");
        editor.press(Key(FcitxKey_Return));
        editor.type("abc! hey? ghi ");
        FCITX_ASSERT(editor.text() == "Xin chào. Tôi đi\nAbc! Hey? Ghi ")
            << editor.text();
        editor.replaceText("");
        editor.type("vnexpress.net ");
        FCITX_ASSERT(editor.text() == "Vnexpress.net ") << editor.text();
    }
    {
        // fcitx5-qt drops SurroundingText before every key: a Qt field's
        // own report after a click still tells capitalize() the text.
        FakeEditor editor(instance, "testapp",
                          CapabilityFlags{CapabilityFlag::Preedit,
                                          CapabilityFlag::GetIMInfoOnFocus},
                          true, "dbus");
        editor.replaceText("abc. ");
        editor.focusQt(true);
        editor.type("x");
        FCITX_ASSERT(editor.preedit() == "X") << editor.preedit();
    }
    {
        // Focus going to another field of the window while another input
        // method types: the text known is the old field's.
        FakeEditor editor(instance, "testapp",
                          CapabilityFlags{CapabilityFlag::Preedit,
                                          CapabilityFlag::GetIMInfoOnFocus},
                          true, "dbus");
        editor.replaceText("abc. ");
        editor.focusQt(true);
        instance->setCurrentInputMethod(&editor, "keyboard-us", true);
        editor.focusQt(false);
        instance->setCurrentInputMethod(&editor, "bamboo", true);
        editor.type("y");
        FCITX_ASSERT(editor.preedit() == "y") << editor.preedit();
    }
    {
        // Without its text, keys tell a sentence start, not a field start.
        FakeEditor editor(instance, "testapp",
                          CapabilityFlags{CapabilityFlag::Preedit});
        editor.type("abc. hey ghi");
        FCITX_ASSERT(editor.text() + editor.preedit() == "abc. Hey ghi")
            << editor.text();
        // A click may have moved the cursor anywhere.
        editor.type(". ");
        editor.reset();
        editor.type("jkl ");
        FCITX_ASSERT(editor.text() == "abc. Hey ghi. jkl ") << editor.text();
    }
    {
        // A text reported late is not trusted.
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("abc. ");
        editor.setReportSurrounding(false);
        editor.type("hey ghi ");
        FCITX_ASSERT(editor.text() == "Abc. Hey ghi ") << editor.text();
    }
    for (const auto flag :
         {CapabilityFlag::Terminal, CapabilityFlag::NoAutoUpperCase}) {
        FakeEditor editor(instance, "testapp",
                          CapabilityFlags{CapabilityFlag::Preedit,
                                          CapabilityFlag::SurroundingText,
                                          flag});
        editor.type("abc. hey ");
        FCITX_ASSERT(editor.text() == "abc. hey ") << editor.text();
    }
    {
        // Our commit when leaving the input method is not reported yet.
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("Hello. abc");
        editor.setReportSurrounding(false);
        instance->setCurrentInputMethod(&editor, "keyboard-us", true);
        instance->setCurrentInputMethod(&editor, "bamboo", true);
        editor.type("d ");
        FCITX_ASSERT(editor.text() == "Hello. Abcd ") << editor.text();
    }
    RawConfig macros;
    macros.setValueByPath("Macro/0/Key", "vn");
    macros.setValueByPath("Macro/0/Value", "việt nam");
    bamboo->setSubConfig("macro/Telex", macros);
    config.setValueByPath("Macro", "True");
    for (const auto *capitalizeMacro : {"True", "False"}) {
        config.setValueByPath("CapitalizeMacro", capitalizeMacro);
        bamboo->setConfig(config);
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("abc. vn ");
        FCITX_ASSERT(editor.text() == "Abc. Việt nam ") << editor.text();
    }
    clearList(bamboo, "macro/Telex", "Macro");
    config.setValueByPath("Macro", "False");
    config.setValueByPath("CapitalizeMacro", "True");
    config.setValueByPath("InputMethod", "VIQR");
    bamboo->setConfig(config);
    {
        // VIQR types tones with '.' and '?'.
        FakeEditor editor(instance, "testapp",
                          CapabilityFlags{CapabilityFlag::Preedit});
        editor.type("ma. ba? ca");
        FCITX_ASSERT(editor.text() + editor.preedit() == "mạ bả ca")
            << editor.text();
    }
    config.setValueByPath("InputMethod", "Telex");
    config.setValueByPath("CapitalizeSentences", "False");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("abc. hey ");
        FCITX_ASSERT(editor.text() == "abc. hey ") << editor.text();
    }
}

// UniKey toolkit's conversions, with the text as the application shows it.
void testConvert(Instance *instance) {
    const Key convertKey("Control+Shift+F6");
    FakeEditor editor(instance, "testapp", PreeditCaps);
    auto list = [&editor]() { return editor.inputPanel().candidateList(); };
    // Typed with the input method off.
    editor.replaceText("xin chaof");
    FCITX_ASSERT(editor.press(convertKey));
    FCITX_ASSERT(list() && list()->size() == 3 &&
                 list()->candidate(0).text().toString() == "chào" &&
                 list()->candidate(1).text().toString() == "CHAOF")
        << (list() ? list()->size() : 0);
    FCITX_ASSERT(editor.press(Key(FcitxKey_1)));
    FCITX_ASSERT(!list());
    FCITX_ASSERT(editor.text() == "xin chào") << editor.text();
    // The word being typed.
    editor.type(" vieejt");
    FCITX_ASSERT(editor.press(convertKey));
    FCITX_ASSERT(list() && list()->candidate(1).text().toString() == "VIỆT");
    editor.press(Key(FcitxKey_2));
    FCITX_ASSERT(editor.text() == "xin chào VIỆT") << editor.text();
    // The selection, Return takes the first.
    editor.replaceText("abc Tieengs Vieejt");
    editor.selectBack(14);
    FCITX_ASSERT(editor.press(convertKey));
    FCITX_ASSERT(editor.press(Key(FcitxKey_Return)));
    FCITX_ASSERT(editor.text() == "abc Tiếng Việt") << editor.text();
    // Pressed again, or Escape, it closes.
    FCITX_ASSERT(editor.press(convertKey) && list());
    FCITX_ASSERT(editor.press(convertKey) && !list());
    FCITX_ASSERT(editor.press(convertKey) && list());
    FCITX_ASSERT(editor.press(Key(FcitxKey_Escape)) && !list());
    FCITX_ASSERT(editor.text() == "abc Tiếng Việt") << editor.text();
    editor.replaceText("123");
    FCITX_ASSERT(editor.press(convertKey) && !list());
    FCITX_ASSERT(editor.text() == "123") << editor.text();
    {
        // Wayland frontends delete through a copy of the text.
        FakeEditor wayland(instance, "testapp", PreeditCaps, true,
                           "wayland_v2");
        wayland.replaceText("xin chaof");
        FCITX_ASSERT(wayland.press(convertKey) &&
                     !wayland.inputPanel().candidateList());
        wayland.selectBack(5);
        FCITX_ASSERT(wayland.press(convertKey) &&
                     wayland.inputPanel().candidateList());
    }
    // Chosen with the mouse, then the application reports late.
    editor.replaceText("abc vieejt");
    FCITX_ASSERT(editor.press(convertKey) && list());
    editor.setReportSurrounding(false);
    list()->candidate(0).select(&editor);
    FCITX_ASSERT(editor.text() == "abc việt") << editor.text();
    FCITX_ASSERT(editor.press(convertKey) && !list());
    FCITX_ASSERT(editor.text() == "abc việt") << editor.text();
    editor.setReportSurrounding(true);
    editor.replaceText("123");
    // An application reporting its text after our commit.
    editor.type(" ");
    editor.setReportSurrounding(false);
    editor.type("vieejt");
    FCITX_ASSERT(editor.press(convertKey) && list());
    editor.press(Key(FcitxKey_2));
    FCITX_ASSERT(editor.text() == "123 VIỆT") << editor.text();
    {
        // fcitx5-qt drops SurroundingText before every key: a single-line
        // Qt field's own report still lets the selection convert.
        FakeEditor qt(instance, "testapp",
                      PreeditCaps | CapabilityFlag::GetIMInfoOnFocus, true,
                      "dbus");
        qt.replaceText("viet nam");
        qt.focusQt(true);
        qt.selectBack(3);
        FCITX_ASSERT(qt.press(convertKey) && qt.inputPanel().candidateList());
        FCITX_ASSERT(qt.press(Key(FcitxKey_1)));
        FCITX_ASSERT(qt.text() == "viet NAM") << qt.text();
    }
    {
        // The word before the cursor still needs the capability itself:
        // fcitx5-qt drops deletions sent while handling a key.
        FakeEditor qt(instance, "testapp",
                      PreeditCaps | CapabilityFlag::GetIMInfoOnFocus, true,
                      "dbus");
        qt.replaceText("viet");
        qt.focusQt(true);
        FCITX_ASSERT(qt.press(convertKey) && !qt.inputPanel().candidateList());
        FCITX_ASSERT(qt.text() == "viet") << qt.text();
    }
    {
        // fcitx5-qt reports a Multiline field's current paragraph only, and
        // maps a selection reaching its end to the end of what it reports
        // (QString::left clamps): such a selection may be only part of the
        // real one, so it is not used.
        FakeEditor qt(instance, "testapp",
                      PreeditCaps | CapabilityFlag::GetIMInfoOnFocus |
                          CapabilityFlag::Multiline,
                      true, "dbus");
        qt.replaceText("abc def");
        qt.focusQt(true);
        qt.selectBack(3);
        FCITX_ASSERT(qt.press(convertKey) && !qt.inputPanel().candidateList());
        FCITX_ASSERT(qt.text() == "abc def") << qt.text();
    }
    {
        // A Multiline selection strictly inside the reported text converts.
        FakeEditor qt(instance, "testapp",
                      PreeditCaps | CapabilityFlag::GetIMInfoOnFocus |
                          CapabilityFlag::Multiline,
                      true, "dbus");
        qt.replaceText("abc ghi");
        qt.focusQt(true);
        qt.selectBack(3);
        qt.setTextAfterCursor(" jkl");
        FCITX_ASSERT(qt.press(convertKey));
        auto candidates = qt.inputPanel().candidateList();
        FCITX_ASSERT(candidates &&
                     candidates->candidate(0).text().toString() == "GHI")
            << (candidates ? candidates->size() : 0);
        qt.press(Key(FcitxKey_Escape));
        // Nor one from the start of the reported text.
        qt.replaceText("ghi");
        qt.selectBack(3);
        FCITX_ASSERT(qt.press(convertKey) && !qt.inputPanel().candidateList());
    }
    {
        // Our commit of a pending word replaces a selection reported
        // earlier: the primary selection, still holding what we replaced,
        // must not be offered too.
        FakeEditor pending(instance, "testapp", PreeditCaps);
        pending.replaceText("abc def");
        pending.type(" vieejt");
        pending.selectBack(4);
        pending.setReportSurrounding(false);
        std::ostringstream log;
        Log::setLogStream(log);
        const bool filtered = pending.press(convertKey);
        Log::setLogStream(std::cerr);
        FCITX_ASSERT(filtered);
        FCITX_ASSERT(!pending.inputPanel().candidateList());
        FCITX_ASSERT(pending.text() == "abc việt") << pending.text();
        FCITX_ASSERT(log.str().find("converting the primary selection") ==
                     std::string::npos)
            << log.str();
    }
    {
        // Nothing known of the text still tries the primary selection.
        FakeEditor bare(instance, "testapp", CapabilityFlag::Preedit);
        bare.replaceText("123");
        std::ostringstream log;
        Log::setLogStream(log);
        const bool filtered = bare.press(convertKey);
        Log::setLogStream(std::cerr);
        FCITX_ASSERT(filtered);
        FCITX_ASSERT(!bare.inputPanel().candidateList());
        FCITX_ASSERT(log.str().find("converting the primary selection") !=
                     std::string::npos)
            << log.str();
    }
}

// Surrounding Text mode never underlines the word being typed.
void testNoUnderline(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig appModes;
    appModes.setValueByPath("AppMode/0/Program", "surrounding");
    appModes.setValueByPath("AppMode/0/Mode", "Surrounding Text");
    bamboo->setSubConfig("app_modes", appModes);
    RawConfig config;
    config.setValueByPath("DisplayUnderline", "True");
    bamboo->setConfig(config);
    const auto underlined = [](FakeEditor &editor) {
        return editor.inputPanel().clientPreedit().formatAt(0).test(
            TextFormatFlag::Underline);
    };
    {
        // Preedit mode keeps the underline asked for.
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("tieengs");
        FCITX_ASSERT(editor.preedit() == "tiếng" && underlined(editor));
    }
    {
        // Qt draws the preedit as told: plain, whatever its surrounding text
        // does.
        FakeEditor editor(instance, "surrounding",
                          PreeditCaps | CapabilityFlag::GetIMInfoOnFocus);
        editor.type("tieengs");
        FCITX_ASSERT(editor.preedit() == "tiếng" && !underlined(editor))
            << editor.preedit();
        FCITX_ASSERT(editor.text().empty()) << editor.text();
    }
    {
        // Address bars suggest how the text goes on, selected after the
        // cursor: the word goes in key by key so that Return takes the
        // suggestion.
        FakeEditor editor(instance, "surrounding",
                          PreeditCaps | CapabilityFlag::Url);
        editor.setSuggestion("book.com");
        editor.type("face");
        FCITX_ASSERT(editor.text() == "facebook.com") << editor.text();
        editor.press(Key(FcitxKey_Return));
        FCITX_ASSERT(editor.text() == "facebook.com\n") << editor.text();
    }
    for (const char *frontend : {"bambootest", "wayland"}) {
        // Tones replace letters before the suggestion. Chrome's address bar
        // takes BackSpace for them: it drops deletions around the
        // suggestion, "bài" gave "baiài".
        FakeEditor editor(instance, "surrounding", PreeditCaps, true, frontend);
        editor.setSuggestion(".vn");
        editor.type("vieetj");
        FCITX_ASSERT(editor.text() == "việt.vn") << frontend << editor.text();
        editor.type(" nam");
        FCITX_ASSERT(editor.text() == "việt nam.vn")
            << frontend << editor.text();
    }
    config.setValueByPath("DisplayUnderline", "False");
    config.setValueByPath("WaylandBackSpace", "True");
    bamboo->setConfig(config);
    {
        // KWin hands keys we forward to the application in order with our
        // commits: a terminal gets the word as text.
        FakeEditor editor(instance, "surrounding", PreeditCaps, false,
                          "wayland");
        editor.type("vieetj tieengs");
        editor.press(Key(FcitxKey_BackSpace));
        editor.type("g ");
        FCITX_ASSERT(editor.text() == "việt tiếng ") << editor.text();
        FCITX_ASSERT(editor.panelPreedit().empty()) << editor.panelPreedit();
    }
    {
        // KWin passes on an empty surrounding text for terminals, which
        // report none.
        FakeEditor editor(instance, "surrounding",
                          PreeditCaps | CapabilityFlag::Terminal, false,
                          "wayland");
        editor.surroundingText().setText("", 0, 0);
        editor.updateSurroundingText();
        editor.type("chuwowng trinhf ");
        FCITX_ASSERT(editor.text() == "chương trình ") << editor.text();
        // Terminals like Alacritty paste longer commits, and applications
        // such as Claude Code lose a paste that more keys follow.
        FCITX_ASSERT(editor.longestCommit() == 1) << editor.longestCommit();
    }
    {
        // Typed over a selection Chrome reports before the cursor, the first
        // key goes in with BackSpace keys, the word going on.
        FakeEditor editor(instance, "surrounding", PreeditCaps, true,
                          "wayland");
        editor.type("chao ban");
        editor.selectBack(3);
        editor.type("ddi ");
        FCITX_ASSERT(editor.text() == "chao đi ") << editor.text();
    }
    {
        // The word comes after what precedes the selection, not after the
        // last key typed before it.
        FakeEditor editor(instance, "surrounding", PreeditCaps, false,
                          "wayland");
        editor.type("abc.");
        editor.setReportSurrounding(true);
        editor.selectBack(1);
        editor.type("ddi ");
        FCITX_ASSERT(editor.text() == "abcđi ") << editor.text();
        // Or the application reported it before our last edit.
        editor.selectBack(1);
        editor.setReportSurrounding(false);
        editor.type("d vi");
        editor.setReportSurrounding(true);
        editor.report();
        editor.type("eet");
        FCITX_ASSERT(editor.text() == "abcđid viêt") << editor.text();
    }
    {
        // Other Wayland frontends are not KWin's.
        FakeEditor editor(instance, "surrounding", PreeditCaps, false,
                          "wayland_v2");
        editor.type("vieetj");
        FCITX_ASSERT(editor.panelPreedit() == "việt") << editor.panelPreedit();
    }
    config.setValueByPath("WaylandBackSpace", "False");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "surrounding", PreeditCaps, false,
                          "wayland");
        editor.type("vieetj");
        FCITX_ASSERT(editor.panelPreedit() == "việt") << editor.panelPreedit();
        FCITX_ASSERT(editor.text().empty()) << editor.text();
    }
    {
        // A whole word goes into a terminal character by character too.
        FakeEditor editor(instance, "surrounding",
                          PreeditCaps | CapabilityFlag::Terminal, false,
                          "wayland");
        editor.type("vieetj ");
        FCITX_ASSERT(editor.text() == "việt ") << editor.text();
        FCITX_ASSERT(editor.longestCommit() == 1) << editor.longestCommit();
    }
    clearList(bamboo, "app_modes", "AppMode");
}

void testSpellCheckExceptions(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig config;
    config.setValueByPath("SpellCheckExceptions/0", "Krông");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("Kroong text ");
        FCITX_ASSERT(editor.text() == "Krông text ") << editor.text();
    }
    RawConfig reset;
    reset.get("SpellCheckExceptions", true);
    bamboo->setConfig(reset);
}

// In terminals and code editors Escape leaves Vietnamese, like VNIKey's vim
// mode: vim's normal mode needs plain keys.
void testTerminalEscape(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig appModes;
    appModes.setValueByPath("AppMode/0/Program", "vimapp");
    appModes.setValueByPath("AppMode/0/Mode", "Preedit");
    appModes.setValueByPath("AppMode/0/Terminal", "True");
    bamboo->setSubConfig("app_modes", appModes);
    {
        FakeEditor editor(instance, "vimapp", PreeditCaps);
        editor.type("vieetj");
        FCITX_ASSERT(!editor.press(Key(FcitxKey_Escape)));
        FCITX_ASSERT(editor.text() == "việt") << editor.text();
        FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
        FCITX_ASSERT(instance->inputMethod(&editor) == "keyboard-us");
        editor.type("dd");
        FCITX_ASSERT(editor.text() == "việtdd") << editor.text();
    }
    {
        FakeEditor editor(instance, "vimapp", PreeditCaps);
        FCITX_ASSERT(!editor.press(Key(FcitxKey_Escape)));
        FCITX_ASSERT(instance->inputMethod(&editor) == "keyboard-us");
    }
    {
        // Terminals reported by the application itself.
        FakeEditor editor(instance, "testapp",
                          PreeditCaps | CapabilityFlag::Terminal);
        FCITX_ASSERT(!editor.press(Key(FcitxKey_Escape)));
        FCITX_ASSERT(instance->inputMethod(&editor) == "keyboard-us");
    }
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("vieetj");
        FCITX_ASSERT(!editor.press(Key(FcitxKey_Escape)));
        FCITX_ASSERT(editor.text() == "việt") << editor.text();
        FCITX_ASSERT(instance->inputMethod(&editor) == "bamboo");
    }
    RawConfig config;
    config.setValueByPath("TerminalEscape", "False");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "vimapp", PreeditCaps);
        FCITX_ASSERT(!editor.press(Key(FcitxKey_Escape)));
        FCITX_ASSERT(instance->inputMethod(&editor) == "bamboo");
    }
    config.setValueByPath("TerminalEscape", "True");
    bamboo->setConfig(config);
    clearList(bamboo, "app_modes", "AppMode");
}

// The panel shows EN whenever keys go straight to the application.
void testModeLabel(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    auto *engine = instance->inputMethodEngine("bamboo");
    const auto *entry = instance->inputMethodManager().entry("bamboo");
    FCITX_ASSERT(engine && entry);
    RawConfig appModes;
    appModes.setValueByPath("AppMode/0/Program", "surrounding");
    appModes.setValueByPath("AppMode/0/Mode", "Surrounding Text");
    appModes.setValueByPath("AppMode/1/Program", "excluded");
    appModes.setValueByPath("AppMode/1/Mode", "Exclude");
    bamboo->setSubConfig("app_modes", appModes);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        FCITX_ASSERT(engine->subModeLabel(*entry, editor) == "VI");
        FCITX_ASSERT(engine->subMode(*entry, editor) == "Telex")
            << engine->subMode(*entry, editor);
    }
    {
        FakeEditor editor(instance, "surrounding", PreeditCaps);
        FCITX_ASSERT(engine->subMode(*entry, editor) ==
                     "Telex (Surrounding Text)")
            << engine->subMode(*entry, editor);
    }
    {
        // What Surrounding Text does where the text is not edited in place.
        // Panels split the status kimpanel sends them on colons.
        FakeEditor editor(instance, "surrounding",
                          PreeditCaps | CapabilityFlag::GetIMInfoOnFocus);
        FCITX_ASSERT(engine->subMode(*entry, editor) ==
                     "Telex (Surrounding Text → Plain Preedit)")
            << engine->subMode(*entry, editor);
    }
    {
        FakeEditor editor(instance, "surrounding", PreeditCaps, false,
                          "bambootest");
        FCITX_ASSERT(engine->subMode(*entry, editor) ==
                     "Telex (Surrounding Text → Input Method Window)")
            << engine->subMode(*entry, editor);
    }
    {
        FakeEditor editor(instance, "excluded", PreeditCaps);
        FCITX_ASSERT(engine->subModeLabel(*entry, editor) == "EN");
    }
    {
        FakeEditor editor(instance, "testapp",
                          PreeditCaps | CapabilityFlag::Email);
        FCITX_ASSERT(engine->subModeLabel(*entry, editor) == "EN");
    }
    clearList(bamboo, "app_modes", "AppMode");
}

// Addresses and numbers are never Vietnamese, unlike browsers' URL fields
// where people search.
void testFieldHints(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    {
        FakeEditor editor(instance, "testapp",
                          PreeditCaps | CapabilityFlag::Email);
        editor.type("tuanf@gmail.com");
        FCITX_ASSERT(editor.text() == "tuanf@gmail.com") << editor.text();
        FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
    }
    {
        // The typing mode key is typed there too.
        FakeEditor editor(instance, "testapp",
                          PreeditCaps | CapabilityFlag::Email);
        editor.type("a");
        editor.press(Key(FcitxKey_asciitilde, KeyState::Shift));
        editor.type("b@x.vn");
        FCITX_ASSERT(editor.text() == "a~b@x.vn") << editor.text();
    }
    {
        FakeEditor editor(instance, "testapp",
                          PreeditCaps | CapabilityFlag::Dialable);
        editor.type("0912 dd");
        FCITX_ASSERT(editor.text() == "0912 dd") << editor.text();
    }
    {
        FakeEditor editor(instance, "testapp",
                          PreeditCaps | CapabilityFlag::Url);
        editor.type("tieengs");
        FCITX_ASSERT(editor.preedit() == "tiếng") << editor.preedit();
    }
    {
        // Becoming a number field mid-word ends the word first.
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("vieetj");
        editor.setCapabilityFlags(PreeditCaps | CapabilityFlag::Number);
        editor.type("1");
        FCITX_ASSERT(editor.text() == "việt1") << editor.text();
        FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
    }
    RawConfig config;
    config.setValueByPath("AutoExcludeFields", "False");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp",
                          PreeditCaps | CapabilityFlag::Email);
        editor.type("tieengs");
        FCITX_ASSERT(editor.preedit() == "tiếng") << editor.preedit();
    }
    config.setValueByPath("AutoExcludeFields", "True");
    bamboo->setConfig(config);
}

// fcitx5 types plain keys into password fields. Qt Quick reports its own as
// sensitive only, and draws the preedit there unmasked.
void testPasswordFields(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    auto *engine = instance->inputMethodEngine("bamboo");
    const auto *entry = instance->inputMethodManager().entry("bamboo");
    FCITX_ASSERT(engine && entry);
    // What fcitx5-qt sends for a TextField in Password echo mode, no text.
    const CapabilityFlags qtPassword{
        CapabilityFlag::Preedit, CapabilityFlag::GetIMInfoOnFocus,
        CapabilityFlag::Sensitive, CapabilityFlag::NoSpellCheck,
        CapabilityFlag::NoAutoUpperCase};
    const auto qtText = qtPassword.unset(CapabilityFlag::Sensitive);
    const CapabilityFlags password{CapabilityFlag::Preedit,
                                   CapabilityFlag::Password};
    const Key tilde(FcitxKey_asciitilde, KeyState::Shift);
    const Key convertKey("Control+Shift+F6");
    // Whether the debug log has the key typed.
    const auto logsKeys = [instance](CapabilityFlags caps,
                                     const char *frontend) {
        std::ostringstream log;
        Log::setLogStream(log);
        {
            FakeEditor editor(instance, "testapp", caps, false, frontend);
            editor.type("q");
        }
        Log::setLogStream(std::cerr);
        FCITX_ASSERT(log.str().find("program testapp") != std::string::npos)
            << log.str();
        return log.str().find("key Key(q") != std::string::npos;
    };
    {
        FakeEditor editor(instance, "testapp", qtPassword, false);
        FCITX_ASSERT(engine->subModeLabel(*entry, editor) == "EN");
        editor.type("tieengs");
        FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
        // The typing mode key is typed there too, the convert key goes to
        // the application.
        editor.press(tilde);
        FCITX_ASSERT(!editor.press(convertKey) &&
                     !editor.inputPanel().candidateList());
        FCITX_ASSERT(editor.text() == "tieengs~") << editor.text();
    }
    FCITX_ASSERT(logsKeys(PreeditCaps, "bambootest"));
    FCITX_ASSERT(!logsKeys(qtPassword, "bambootest"));
    {
        // A field becoming a password field mid-word gets the word as it is
        // then, not shown until the next key.
        FakeEditor editor(instance, "testapp", qtText, false);
        editor.type("vieetj");
        editor.setCapabilityFlags(qtPassword);
        FCITX_ASSERT(editor.text() == "việt") << editor.text();
        FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
        editor.type("s");
        FCITX_ASSERT(editor.text() == "việts") << editor.text();
    }
    {
        // Fields of another input method, or not focused, are left alone.
        FakeEditor editor(instance, "testapp", qtText, false);
        instance->setCurrentInputMethod(&editor, "keyboard-us", true);
        editor.inputPanel().setAuxUp(Text("other"));
        editor.setCapabilityFlags(qtPassword);
        FCITX_ASSERT(editor.inputPanel().auxUp().toString() == "other");
    }
    {
        FakeEditor editor(instance, "testapp", qtText, false);
        editor.focusOut();
        editor.inputPanel().setAuxUp(Text("other"));
        editor.setCapabilityFlags(qtPassword);
        FCITX_ASSERT(editor.inputPanel().auxUp().toString() == "other");
    }
    {
        // Chrome marks every field of its incognito windows sensitive.
        FakeEditor editor(instance, "testapp",
                          PreeditCaps | CapabilityFlag::Sensitive, true,
                          "wayland");
        editor.type("tieengs ");
        FCITX_ASSERT(editor.text() == "tiếng ") << editor.text();
    }
    FCITX_ASSERT(!logsKeys(PreeditCaps | CapabilityFlag::Sensitive, "wayland"));
    // Password fields are not a field hint people turn off.
    RawConfig config;
    config.setValueByPath("AutoExcludeFields", "False");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", qtPassword, false);
        editor.type("tieengs");
        FCITX_ASSERT(editor.text() == "tieengs") << editor.text();
    }
    config.setValueByPath("AutoExcludeFields", "True");
    bamboo->setConfig(config);
    // Unless input methods are allowed in password fields.
    RawConfig global;
    global.setValueByPath("Behavior/AllowInputMethodForPassword", "True");
    instance->globalConfig().load(global, true);
    {
        FakeEditor editor(instance, "testapp", qtPassword, false);
        FCITX_ASSERT(engine->subModeLabel(*entry, editor) == "VI");
        editor.type("tieengs");
        // Masked as fcitx5 masks them, the dots never committed.
        const auto &preedit = editor.inputPanel().clientPreedit();
        FCITX_ASSERT(preedit.toString() == "•••••" &&
                     preedit.formatAt(0).test(TextFormatFlag::DontCommit))
            << preedit.toString();
        editor.type(" a");
        editor.press(tilde);
        FCITX_ASSERT(editor.text() == "tiếng a~") << editor.text();
        FCITX_ASSERT(!editor.press(convertKey));
    }
    {
        // In the BackSpace mode too, which gives Konsole DEL characters: Qt
        // Quick's password fields ask for neither capitals nor predictions
        // and report no text either.
        RawConfig appModes;
        appModes.setValueByPath("AppMode/0/Program", "qtbackspace");
        appModes.setValueByPath("AppMode/0/Mode", "BackSpace");
        bamboo->setSubConfig("app_modes", appModes);
        FakeEditor editor(instance, "qtbackspace", qtPassword, false, "dbus");
        editor.focusQt(false);
        editor.type("tieengs");
        FCITX_ASSERT(editor.inputPanel().clientPreedit().toString() ==
                         "•••••" &&
                     editor.text().empty())
            << editor.text();
        clearList(bamboo, "app_modes", "AppMode");
    }
    {
        // Turning normal mid-word, the masked word goes in as typed.
        FakeEditor editor(instance, "testapp", qtPassword, false);
        editor.type("tieengs");
        editor.setCapabilityFlags(qtText);
        FCITX_ASSERT(editor.text() == "tiếng") << editor.text();
        FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
    }
    {
        // fcitx5 masks the preedit of the password fields it knows of as it
        // sends it, and commits the word when focus goes.
        FakeEditor editor(instance, "testapp", password, false);
        editor.type("a");
        editor.press(tilde);
        editor.type("tieengs");
        FCITX_ASSERT(editor.preedit() == "tiếng") << editor.preedit();
        editor.focusOut();
        FCITX_ASSERT(editor.text() == "a~tiếng") << editor.text();
    }
    FCITX_ASSERT(!logsKeys(password, "bambootest"));
    global.setValueByPath("Behavior/ShowPreeditForPassword", "True");
    instance->globalConfig().load(global, true);
    {
        FakeEditor editor(instance, "testapp", qtPassword, false);
        editor.type("tieengs");
        FCITX_ASSERT(editor.preedit() == "tiếng") << editor.preedit();
    }
    global.setValueByPath("Behavior/AllowInputMethodForPassword", "False");
    global.setValueByPath("Behavior/ShowPreeditForPassword", "False");
    instance->globalConfig().load(global, true);
}

// On a loaded machine Chrome reports our edits later than a key waits, yet
// one by one as they land: keys wait on while it reports the text as one of
// our last edits left it, a second at most.
void testSlowReports(Instance *instance, TimedSteps &steps) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig appModes;
    appModes.setValueByPath("AppMode/0/Program", "surrounding");
    appModes.setValueByPath("AppMode/0/Mode", "Surrounding Text");
    appModes.setValueByPath("AppMode/1/Program", "backspace");
    appModes.setValueByPath("AppMode/1/Mode", "BackSpace");
    bamboo->setSubConfig("app_modes", appModes);
    auto editor = std::make_shared<std::unique_ptr<FakeEditor>>();
    // The tone comes before Chrome reported "ngươi".
    const auto start = [instance, editor]() {
        *editor = std::make_unique<FakeEditor>(instance, "surrounding",
                                               PreeditCaps, true, "wayland");
        auto &e = **editor;
        e.type("nguoi");
        e.setReportSurrounding(false);
        e.type("wf");
        FCITX_ASSERT(e.text() == "ngươi") << e.text();
    };
    steps.add(0, start);
    steps.add(300, [editor]() {
        auto &e = **editor;
        FCITX_ASSERT(e.text() == "ngươi") << e.text();
        e.setReportSurrounding(true);
        e.report();
        FCITX_ASSERT(e.text() == "người") << e.text();
        editor->reset();
    });
    // No report at all: after a second the key starts a new word. One key
    // waiting suggests nothing. Logs go to the stream between steps only,
    // failures need theirs.
    auto log = std::make_shared<std::ostringstream>();
    steps.add(0, [start, log]() {
        start();
        Log::setLogStream(*log);
    });
    steps.add(600, [editor, log]() {
        Log::setLogStream(std::cerr);
        FCITX_ASSERT((*editor)->text() == "ngươi") << (*editor)->text();
        Log::setLogStream(*log);
    });
    steps.add(700, [editor, log]() {
        Log::setLogStream(std::cerr);
        FCITX_ASSERT((*editor)->text() == "ngươif") << (*editor)->text();
        FCITX_ASSERT(log->str().find("waiting on") != std::string::npos &&
                     log->str().find("suggesting") == std::string::npos)
            << log->str();
        editor->reset();
    });
    // The application changed its text: no waiting on.
    steps.add(0, [start, editor]() {
        start();
        (*editor)->reportText("abc");
    });
    steps.add(200, [editor]() {
        FCITX_ASSERT((*editor)->text() == "ngươif") << (*editor)->text();
        editor->reset();
    });
    // A click: the text before the cursor, like after an older edit of
    // ours, is no report behind once Chrome reported the word.
    steps.add(0, [instance, editor]() {
        *editor = std::make_unique<FakeEditor>(instance, "surrounding",
                                               PreeditCaps, true, "wayland");
        auto &e = **editor;
        e.type("xin chao");
        e.setReportSurrounding(false);
        e.reportText("xin ");
        e.type("d");
    });
    steps.add(200, [editor]() {
        FCITX_ASSERT((*editor)->text() == "xin chaod") << (*editor)->text();
        editor->reset();
    });
    // An empty field, the first character not reported yet.
    steps.add(0, [instance, editor]() {
        *editor = std::make_unique<FakeEditor>(instance, "surrounding",
                                               PreeditCaps, true, "wayland");
        (*editor)->setReportSurrounding(false);
        (*editor)->type("dd");
    });
    steps.add(300, [editor]() {
        auto &e = **editor;
        FCITX_ASSERT(e.text() == "d") << e.text();
        e.setReportSurrounding(true);
        e.report();
        FCITX_ASSERT(e.text() == "đ") << e.text();
        editor->reset();
    });
    // BackSpace took the one letter of a word, the next word comes after the
    // same space, Chrome reporting the BackSpace but not the next letter.
    steps.add(0, [instance, editor]() {
        *editor = std::make_unique<FakeEditor>(instance, "surrounding",
                                               PreeditCaps, true, "wayland");
        auto &e = **editor;
        e.type("abc x");
        e.setReportSurrounding(false);
        e.press(Key(FcitxKey_BackSpace));
        e.type("dd");
        e.reportText("abc ");
    });
    steps.add(300, [editor]() {
        auto &e = **editor;
        FCITX_ASSERT(e.text() == "abc d") << e.text();
        e.setReportSurrounding(true);
        e.report();
        FCITX_ASSERT(e.text() == "abc đ") << e.text();
        editor->reset();
    });
    // Reports no longer trusted after two waits in vain, Chrome stuck on an
    // old text: a word starts again after our last edit.
    steps.add(0, [instance, editor]() {
        *editor = std::make_unique<FakeEditor>(instance, "surrounding",
                                               PreeditCaps, true, "wayland");
        auto &e = **editor;
        e.type("q");
        e.setReportSurrounding(false);
        e.reportText("zz1");
        e.type("w");
    });
    steps.add(200, [editor]() {
        (*editor)->reportText("zz2");
        (*editor)->type("e");
    });
    steps.add(250, [editor]() {
        auto &e = **editor;
        e.reportText("za");
        e.type(" aas");
        FCITX_ASSERT(e.text() == "qwe aas") << e.text();
        editor->reset();
    });
    // Reported with our deletion but not the commit after it.
    steps.add(0, [instance, editor]() {
        *editor = std::make_unique<FakeEditor>(instance, "surrounding",
                                               PreeditCaps, true, "wayland");
        auto &e = **editor;
        e.type("nuocw");
        e.setReportSurrounding(false);
        e.type("sj");
        e.reportText("nư");
    });
    steps.add(300, [editor]() {
        auto &e = **editor;
        FCITX_ASSERT(e.text() == "nước") << e.text();
        e.setReportSurrounding(true);
        e.report();
        FCITX_ASSERT(e.text() == "nược") << e.text();
        editor->reset();
    });
    // Typed over a selection with BackSpace keys, then through the
    // surrounding text: a report behind those first keys waits on.
    steps.add(0, [instance, editor]() {
        *editor = std::make_unique<FakeEditor>(
            instance, "backspace", PreeditCaps | CapabilityFlag::Url, true,
            "wayland");
        auto &e = **editor;
        e.type("chao ban");
        e.selectBack(3);
        e.setReportSurrounding(false);
        e.type("gi");
        e.reportText("chao g");
        e.type("f");
        FCITX_ASSERT(e.text() == "chao gi") << e.text();
    });
    steps.add(300, [editor]() {
        auto &e = **editor;
        e.setReportSurrounding(true);
        e.report();
        FCITX_ASSERT(e.text() == "chao gì") << e.text();
        editor->reset();
    });
    // Typed before the application's first report, the first key goes in
    // with BackSpace keys: a first report from before it waits on.
    steps.add(0, [instance, editor]() {
        *editor = std::make_unique<FakeEditor>(
            instance, "backspace", PreeditCaps | CapabilityFlag::Url, false,
            "wayland");
        auto &e = **editor;
        e.type("d");
        e.reportText("");
        e.type("d");
        FCITX_ASSERT(e.text() == "d") << e.text();
    });
    steps.add(300, [editor]() {
        auto &e = **editor;
        e.setReportSurrounding(true);
        e.report();
        FCITX_ASSERT(e.text() == "đ") << e.text();
        editor->reset();
    });
    // Reported with the BackSpace keys of the last edit, not its commit.
    steps.add(0, [instance, editor]() {
        *editor = std::make_unique<FakeEditor>(
            instance, "backspace", PreeditCaps | CapabilityFlag::Url, true,
            "wayland");
        auto &e = **editor;
        e.type("chao ban");
        e.selectBack(3);
        e.setReportSurrounding(false);
        e.type("aa");
        e.reportText("chao ");
        e.type("s");
        FCITX_ASSERT(e.text() == "chao â") << e.text();
    });
    steps.add(300, [editor]() {
        auto &e = **editor;
        e.setReportSurrounding(true);
        e.report();
        FCITX_ASSERT(e.text() == "chao ấ") << e.text();
        editor->reset();
    });
    steps.add(0, [instance, editor]() {
        *editor = std::make_unique<FakeEditor>(
            instance, "backspace", PreeditCaps | CapabilityFlag::Url, true,
            "wayland");
        auto &e = **editor;
        e.type("chao ban");
        e.selectBack(3);
        e.setReportSurrounding(false);
        e.type("nuocwj");
        e.reportText("chao nư");
        e.type("s");
        FCITX_ASSERT(e.text() == "chao nược") << e.text();
    });
    steps.add(300, [editor]() {
        auto &e = **editor;
        e.setReportSurrounding(true);
        e.report();
        FCITX_ASSERT(e.text() == "chao nước") << e.text();
        editor->reset();
    });
    // Five keys in a minute waiting for reports behind: the modes that do
    // not wait are suggested, once for the program.
    const auto open = [instance, editor, log](const char *program,
                                              CapabilityFlags caps) {
        return [instance, editor, log, program, caps]() {
            *editor = std::make_unique<FakeEditor>(instance, program, caps,
                                                   true, "wayland");
            log->str("");
            Log::setLogStream(*log);
        };
    };
    const auto holds = [&steps, editor, log](int count) {
        for (int i = 0; i < count; i++) {
            steps.add(0, [editor, log]() {
                Log::setLogStream(std::cerr);
                auto &e = **editor;
                e.type(" nguoi");
                e.setReportSurrounding(false);
                e.type("wf");
                Log::setLogStream(*log);
            });
            steps.add(300, [editor]() {
                (*editor)->setReportSurrounding(true);
                (*editor)->report();
            });
        }
    };
    // What the suggestion says, none if empty.
    const auto check = [editor, log](std::vector<std::string> wanted) {
        return [editor, log, wanted]() {
            Log::setLogStream(std::cerr);
            FCITX_ASSERT((*editor)->text().ends_with("người người"))
                << (*editor)->text();
            const auto found = log->str().find("suggesting");
            FCITX_ASSERT(wanted.empty() == (found == std::string::npos))
                << log->str();
            for (const auto &text : wanted) {
                FCITX_ASSERT(log->str().find(text, found) != std::string::npos)
                    << text << log->str();
            }
            log->str("");
            Log::setLogStream(*log);
        };
    };
    // Address bars wait in these modes too.
    steps.add(0, open("surrounding", PreeditCaps | CapabilityFlag::Url));
    holds(5);
    steps.add(0, check({}));
    // It names the key opening the table.
    steps.add(0, [bamboo]() {
        RawConfig config;
        config.setValueByPath("InputModeSwitchKey/0", "F2");
        bamboo->setConfig(config);
    });
    steps.add(0, open("surrounding", PreeditCaps));
    holds(4);
    steps.add(0, check({}));
    holds(1);
    steps.add(
        0, check({"surrounding reports its text late",
                  stringutils::concat(
                      "in the table of typing modes (",
                      Key("F2").toString(KeyStringFormat::Localized), ")")}));
    // In another window of the program.
    steps.add(0, open("surrounding", PreeditCaps));
    holds(5);
    steps.add(0, check({}));
    // Without a program name there is no table of modes, without the key
    // the configuration has them.
    steps.add(0, [bamboo]() {
        RawConfig config;
        config.setValueByPath("KindModes/WaylandApplications",
                              "Surrounding Text");
        config.setValueByPath("InputModeSwitchKey", "");
        bamboo->setConfig(config);
    });
    steps.add(0, open("", PreeditCaps));
    holds(5);
    steps.add(0, check({}));
    steps.add(0, open("nokey", PreeditCaps));
    holds(5);
    steps.add(0, check({"Typing Mode per Application in the configuration"}));
    steps.add(0, [bamboo, editor]() {
        Log::setLogStream(std::cerr);
        editor->reset();
        RawConfig config;
        config.setValueByPath("KindModes/WaylandApplications", "Default");
        config.setValueByPath("InputModeSwitchKey/0", "asciitilde");
        bamboo->setConfig(config);
        clearList(bamboo, "app_modes", "AppMode");
    });
}

// Typing modes chosen for applications that the automatic Surrounding Text
// mode does not suit.
void testTypingModes(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    auto *engine = instance->inputMethodEngine("bamboo");
    const auto *entry = instance->inputMethodManager().entry("bamboo");
    FCITX_ASSERT(engine && entry);
    RawConfig appModes;
    appModes.setValueByPath("AppMode/0/Program", "plain");
    appModes.setValueByPath("AppMode/0/Mode", "Plain Preedit");
    appModes.setValueByPath("AppMode/1/Program", "window");
    appModes.setValueByPath("AppMode/1/Mode", "Input Method Window");
    appModes.setValueByPath("AppMode/2/Program", "backspace");
    appModes.setValueByPath("AppMode/2/Mode", "BackSpace");
    appModes.setValueByPath("AppMode/3/Program", "qtterminal");
    appModes.setValueByPath("AppMode/3/Mode", "BackSpace");
    appModes.setValueByPath("AppMode/3/Terminal", "True");
    bamboo->setSubConfig("app_modes", appModes);
    RawConfig config;
    config.setValueByPath("DisplayUnderline", "True");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "plain", PreeditCaps);
        FCITX_ASSERT(engine->subMode(*entry, editor) == "Telex (Plain Preedit)")
            << engine->subMode(*entry, editor);
        editor.type("tieengs");
        const auto &preedit = editor.inputPanel().clientPreedit();
        FCITX_ASSERT(preedit.toString() == "tiếng" &&
                     !preedit.formatAt(0).test(TextFormatFlag::Underline))
            << preedit.toString();
        FCITX_ASSERT(editor.text().empty()) << editor.text();
    }
    {
        // Only words reach the application.
        FakeEditor editor(instance, "window", PreeditCaps);
        editor.type("tieengs");
        FCITX_ASSERT(editor.panelPreedit() == "tiếng") << editor.panelPreedit();
        FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
        editor.type(" ");
        FCITX_ASSERT(editor.text() == "tiếng ") << editor.text();
    }
    {
        // KWin hands keys we forward to the application in order with our
        // commits, Chrome's address bar takes its suggestion first.
        FakeEditor editor(instance, "backspace", PreeditCaps, true, "wayland");
        editor.type("vieetj");
        FCITX_ASSERT(editor.text() == "việt") << editor.text();
        FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
        FCITX_ASSERT(editor.forwardedKeys() > 0);
        editor.setSuggestion("nam");
        editor.type(" tieengs");
        FCITX_ASSERT(editor.text() == "việt tiếngnam") << editor.text();
    }
    // What fcitx5-qt sends for a text field, and for Konsole, which reports
    // no text and asks for neither capitals nor predictions.
    const CapabilityFlags qtText{CapabilityFlag::Preedit,
                                 CapabilityFlag::GetIMInfoOnFocus};
    const auto qtTerminal =
        qtText | CapabilityFlag::NoAutoUpperCase | CapabilityFlag::NoSpellCheck;
    for (auto [program, caps, report] :
         {std::tuple{"backspace", qtTerminal, false},
          std::tuple{"qtterminal", qtText, true}}) {
        // fcitx5-qt hands forwarded keys over after commits: in a terminal
        // DEL characters go with them.
        FakeEditor editor(instance, program, caps, report, "dbus");
        editor.focusQt(report);
        editor.setTerminal();
        editor.type("vieetj");
        FCITX_ASSERT(editor.text() == "việt") << program << editor.text();
        FCITX_ASSERT(editor.preedit().empty() && editor.panelPreedit().empty())
            << program;
        editor.type(" tieengs");
        editor.press(Key(FcitxKey_BackSpace));
        editor.type("g ");
        FCITX_ASSERT(editor.text() == "việt tiếng ")
            << program << editor.text();
        FCITX_ASSERT(editor.forwardedKeys() == 0) << editor.forwardedKeys();
    }
    for (auto [caps, report] :
         {std::pair{qtText, true}, std::pair{qtText, false},
          std::pair{qtTerminal, true},
          std::pair{qtText | CapabilityFlag::NoSpellCheck, false},
          std::pair{qtText | CapabilityFlag::NoAutoUpperCase, false},
          std::pair{qtTerminal | CapabilityFlag::Url, false}}) {
        // Other Qt applications take DEL for a character: fields that
        // reported their text since they got focus, or that ask for
        // capitals or predictions, the text unreported after a window
        // switch. URL fields too, Konsole's hints notwithstanding.
        FakeEditor editor(instance, "backspace", caps, true, "dbus");
        editor.focusQt(report);
        editor.type("vieetj");
        FCITX_ASSERT(editor.preedit() == "việt")
            << report << editor.preedit() << editor.text();
        editor.type(" ");
        FCITX_ASSERT(editor.text() == "việt ") << report << editor.text();
    }
    {
        // Konsole's search bar, then its terminal, in one window: the text
        // the bar reported stays, the terminal reports none.
        FakeEditor editor(instance, "backspace", qtText, true, "dbus");
        editor.focusQt(true);
        editor.type("tieengs ");
        FCITX_ASSERT(editor.text() == "tiếng ") << editor.text();
        editor.setCapabilityFlags(qtTerminal);
        editor.setReportSurrounding(false);
        editor.setTerminal();
        editor.focusQt(false);
        editor.type("vieetj");
        FCITX_ASSERT(editor.text() == "tiếng việt") << editor.text();
        FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
    }
    {
        // A user-name field reports once on a click: a later window switch
        // (no report) must not take it for a terminal, Konsole's hints
        // notwithstanding.
        FakeEditor editor(instance, "backspace", qtTerminal, true, "dbus");
        editor.focusQt(true);
        editor.type("vieetj ");
        FCITX_ASSERT(editor.text() == "việt ") << editor.text();
        editor.focusQt(false);
        editor.type("vieetj");
        FCITX_ASSERT(editor.preedit() == "việt") << editor.preedit();
        FCITX_ASSERT(editor.text() == "việt ") << editor.text();
    }
    {
        // An address bar's suggestion shows in the report only: it gets
        // Surrounding Text, which waits for it.
        FakeEditor editor(instance, "backspace",
                          PreeditCaps | CapabilityFlag::Url, true, "wayland");
        editor.setSuggestion("nam");
        editor.type("vie");
        editor.setReportSurrounding(false);
        editor.type("e");
        // The bar stops suggesting before it reports.
        editor.setSuggestion("");
        editor.type("j");
        editor.setReportSurrounding(true);
        editor.report();
        FCITX_ASSERT(editor.text() == "việ") << editor.text();
    }
    {
        // The label tells what the mode does in the field.
        FakeEditor editor(instance, "backspace", PreeditCaps, true,
                          "bambootest");
        FCITX_ASSERT(engine->subMode(*entry, editor) ==
                     "Telex (BackSpace → Input Method Window)")
            << engine->subMode(*entry, editor);
    }
    {
        FakeEditor editor(instance, "backspace",
                          PreeditCaps | CapabilityFlag::Url, true, "wayland");
        FCITX_ASSERT(engine->subMode(*entry, editor) ==
                     "Telex (BackSpace → Surrounding Text)")
            << engine->subMode(*entry, editor);
        // Its text can't be edited in place with a selection before the
        // cursor: the mode does its own.
        editor.type("abc");
        editor.selectBack(2);
        FCITX_ASSERT(engine->subMode(*entry, editor) == "Telex (BackSpace)")
            << engine->subMode(*entry, editor);
    }
    {
        // Typed over an address selected before the cursor, the first key
        // goes in with BackSpace keys, the next ones through the
        // surrounding text, the word going on.
        FakeEditor editor(instance, "backspace",
                          PreeditCaps | CapabilityFlag::Url, true, "wayland");
        editor.replaceText("about:blank");
        editor.selectBack(11);
        editor.type("ddi");
        FCITX_ASSERT(editor.text() == "đi") << editor.text();
        // A selection before the cursor ends the word, typing replaces it.
        editor.type(" vie");
        editor.selectBack(2);
        editor.type("e");
        FCITX_ASSERT(editor.text() == "đi ve") << editor.text();
    }
    {
        // Input Method Window too, words going in whole lose Return to the
        // suggestion.
        FakeEditor editor(instance, "window", PreeditCaps | CapabilityFlag::Url,
                          true, "wayland");
        FCITX_ASSERT(engine->subMode(*entry, editor) ==
                     "Telex (Input Method Window → Surrounding Text)")
            << engine->subMode(*entry, editor);
        editor.type("vieetj");
        FCITX_ASSERT(editor.text() == "việt" && editor.panelPreedit().empty())
            << editor.text() << editor.panelPreedit();
    }
    {
        // A Qt URL field, whose text is never edited in place.
        FakeEditor editor(instance, "window", qtText | CapabilityFlag::Url,
                          true, "dbus");
        editor.focusQt(true);
        FCITX_ASSERT(engine->subMode(*entry, editor) ==
                     "Telex (Input Method Window)")
            << engine->subMode(*entry, editor);
        editor.type("vieetj");
        FCITX_ASSERT(editor.panelPreedit() == "việt") << editor.panelPreedit();
    }
    {
        // The label follows the field's reports and flags. On KWin a field
        // gets focus before its text comes, its text gone with the focus:
        // the panel asks for the label again.
        StatusUpdates updates(instance);
        FakeEditor editor(instance, "backspace",
                          PreeditCaps | CapabilityFlag::Url, true, "wayland");
        const auto label = [engine, entry, &editor]() {
            return engine->subMode(*entry, editor);
        };
        FCITX_ASSERT(label() == "Telex (BackSpace → Surrounding Text)")
            << label();
        editor.focusOut();
        editor.surroundingText().invalidate();
        editor.focusIn();
        FCITX_ASSERT(label() == "Telex (BackSpace)") << label();
        updates.count = 0;
        editor.report();
        FCITX_ASSERT(label() == "Telex (BackSpace → Surrounding Text)")
            << label();
        FCITX_ASSERT(updates.count == 1) << updates.count;
        editor.report();
        FCITX_ASSERT(updates.count == 1) << updates.count;
        editor.setCapabilityFlags(editor.capabilityFlags() |
                                  CapabilityFlag::Email);
        FCITX_ASSERT(engine->subModeLabel(*entry, editor) == "EN");
        FCITX_ASSERT(updates.count == 2) << updates.count;
        // Nothing to tell out of focus or with another input method, the
        // panel asks as the field gets either.
        editor.focusOut();
        editor.setCapabilityFlags(
            editor.capabilityFlags().unset(CapabilityFlag::Email));
        editor.report();
        FCITX_ASSERT(updates.count == 2) << updates.count;
        editor.focusIn();
        instance->setCurrentInputMethod(&editor, "keyboard-us", true);
        updates.count = 0;
        editor.surroundingText().invalidate();
        editor.updateSurroundingText();
        FCITX_ASSERT(updates.count == 0) << updates.count;
        instance->setCurrentInputMethod(&editor, "bamboo", true);
        updates.count = 0;
        editor.report();
        FCITX_ASSERT(updates.count == 1) << updates.count;
    }
    for (const char *frontend : {"bambootest", "wayland_v2"}) {
        // Elsewhere forwarded keys may come after our commits.
        FakeEditor editor(instance, "backspace", PreeditCaps, true, frontend);
        editor.type("vieetj");
        FCITX_ASSERT(editor.panelPreedit() == "việt")
            << frontend << editor.panelPreedit();
        FCITX_ASSERT(editor.text().empty()) << frontend << editor.text();
    }
    config.setValueByPath("DisplayUnderline", "False");
    bamboo->setConfig(config);
    clearList(bamboo, "app_modes", "AppMode");
}

// Typing modes per kind of application, for applications without their own.
void testKindModes(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    auto *engine = instance->inputMethodEngine("bamboo");
    const auto *entry = instance->inputMethodManager().entry("bamboo");
    FCITX_ASSERT(engine && entry);
    RawConfig config;
    config.setValueByPath("KindModes/QtTerminals", "BackSpace");
    config.setValueByPath("KindModes/QtApplications", "Plain Preedit");
    config.setValueByPath("KindModes/WaylandApplications",
                          "Input Method Window");
    config.setValueByPath("KindModes/GtkApplications", "Surrounding Text");
    config.setValueByPath("KindModes/X11Applications", "Exclude");
    bamboo->setConfig(config);
    RawConfig appModes;
    appModes.setValueByPath("AppMode/0/Program", "ownmode");
    appModes.setValueByPath("AppMode/0/Mode", "Exclude");
    bamboo->setSubConfig("app_modes", appModes);
    const CapabilityFlags qt{CapabilityFlag::Preedit,
                             CapabilityFlag::GetIMInfoOnFocus};
    const auto mode = [engine, entry](FakeEditor &editor) {
        return engine->subMode(*entry, editor);
    };
    {
        // Konsole reports no text.
        FakeEditor editor(instance, "kindterminal",
                          qt | CapabilityFlag::NoAutoUpperCase |
                              CapabilityFlag::NoSpellCheck,
                          false, "dbus");
        editor.focusQt(false);
        editor.setTerminal();
        FCITX_ASSERT(mode(editor) == "Telex (BackSpace)") << mode(editor);
        editor.type("vieetj");
        FCITX_ASSERT(editor.text() == "việt") << editor.text();
    }
    {
        FakeEditor editor(instance, "kindapp", qt, true, "dbus");
        editor.focusQt(true);
        editor.type("vieetj");
        FCITX_ASSERT(editor.preedit() == "việt" && editor.text().empty())
            << editor.preedit() << editor.text();
        FCITX_ASSERT(mode(editor) == "Telex (Plain Preedit)") << mode(editor);
        // The table opens on it.
        editor.press(Key(FcitxKey_asciitilde, KeyState::Shift));
        auto candidates = editor.inputPanel().candidateList();
        FCITX_ASSERT(candidates && candidates->cursorIndex() == 2 &&
                     candidates->label(2).toString() == "*. ");
        editor.press(Key(FcitxKey_Escape));
    }
    {
        FakeEditor editor(instance, "kindweb", PreeditCaps, true, "wayland");
        FCITX_ASSERT(mode(editor) == "Telex (Input Method Window)")
            << mode(editor);
        // The panel asks for the label again as the configuration changes.
        StatusUpdates updates(instance);
        config.setValueByPath("KindModes/WaylandApplications", "Plain Preedit");
        bamboo->setConfig(config);
        FCITX_ASSERT(mode(editor) == "Telex (Plain Preedit)") << mode(editor);
        FCITX_ASSERT(updates.count == 1) << updates.count;
        // And as the input method does, which the label starts with.
        updates.count = 0;
        config.setValueByPath("InputMethod", "VNI");
        bamboo->setConfig(config);
        FCITX_ASSERT(mode(editor) == "VNI (Plain Preedit)") << mode(editor);
        FCITX_ASSERT(updates.count == 1) << updates.count;
        config.setValueByPath("InputMethod", "Telex");
        bamboo->setConfig(config);
        config.setValueByPath("KindModes/WaylandApplications",
                              "Input Method Window");
        bamboo->setConfig(config);
    }
    {
        // As a Qt field reports its text after focus, the kind it is taken
        // for and the label change; a later focus with no report keeps them.
        StatusUpdates updates(instance);
        FakeEditor editor(instance, "kindlabel",
                          qt | CapabilityFlag::NoAutoUpperCase |
                              CapabilityFlag::NoSpellCheck,
                          false, "dbus");
        editor.focusQt(false);
        FCITX_ASSERT(mode(editor) == "Telex (BackSpace)") << mode(editor);
        editor.setReportSurrounding(true);
        updates.count = 0;
        editor.report();
        FCITX_ASSERT(mode(editor) == "Telex (Plain Preedit)") << mode(editor);
        FCITX_ASSERT(updates.count == 1) << updates.count;
        editor.focusQt(false);
        FCITX_ASSERT(mode(editor) == "Telex (Plain Preedit)") << mode(editor);
    }
    {
        // Konsole's search bar reported its text, then its terminal gets
        // focus: the label the panel asks for then is the terminal's.
        StatusUpdates updates(instance);
        FakeEditor editor(instance, "kindlabel", qt, true, "dbus");
        editor.focusQt(true);
        FCITX_ASSERT(mode(editor) == "Telex (Plain Preedit)") << mode(editor);
        editor.setCapabilityFlags(qt | CapabilityFlag::NoAutoUpperCase |
                                  CapabilityFlag::NoSpellCheck);
        editor.setReportSurrounding(false);
        editor.focusQt(false);
        FCITX_ASSERT(mode(editor) == "Telex (BackSpace)") << mode(editor);
        // Were it ever to report, the label would change again.
        editor.setReportSurrounding(true);
        updates.count = 0;
        editor.report();
        FCITX_ASSERT(mode(editor) == "Telex (Plain Preedit)") << mode(editor);
        FCITX_ASSERT(updates.count == 1) << updates.count;
    }
    {
        // A mode of its own comes first.
        FakeEditor editor(instance, "ownmode", qt, false, "dbus");
        FCITX_ASSERT(engine->subModeLabel(*entry, editor) == "EN");
    }
    {
        // fcitx5-gtk's clients, Chromium on X11 among them.
        FakeEditor editor(instance, "kindgtk", PreeditCaps, true, "dbus");
        FCITX_ASSERT(mode(editor) == "Telex (Surrounding Text)")
            << mode(editor);
    }
    {
        FakeEditor editor(instance, "kindx11", PreeditCaps, true, "xim");
        FCITX_ASSERT(engine->subModeLabel(*entry, editor) == "EN");
    }
    for (const char *frontend : {"ibus", "wayland_v2", "fcitx4"}) {
        // Left to the default typing mode.
        FakeEditor editor(instance, "kindother", PreeditCaps, true, frontend);
        FCITX_ASSERT(mode(editor) == "Telex") << frontend << mode(editor);
    }
    for (const char *kind :
         {"QtTerminals", "QtApplications", "WaylandApplications",
          "GtkApplications", "X11Applications"}) {
        config.setValueByPath(stringutils::concat("KindModes/", kind),
                              "Default");
    }
    bamboo->setConfig(config);
    clearList(bamboo, "app_modes", "AppMode");
}

// ibus-bamboo's Shift+~ table choosing the typing mode of the application.
void testInputModePicker(Instance *instance) {
    const Key tilde(FcitxKey_asciitilde, KeyState::Shift);
    auto *bamboo = instance->addonManager().addon("bamboo");
    {
        FakeEditor editor(instance, "pickerapp", PreeditCaps);
        editor.type("vieetj");
        // Opening it ends the word.
        FCITX_ASSERT(editor.press(tilde));
        FCITX_ASSERT(editor.text() == "việt") << editor.text();
        auto candidates = editor.inputPanel().candidateList();
        FCITX_ASSERT(candidates && candidates->size() == 6);
        FCITX_ASSERT(candidates->label(0).toString() == "*. " &&
                     candidates->label(5).toString() == "6. ");
        // Pressed again it closes and types '~'.
        FCITX_ASSERT(!editor.press(tilde));
        FCITX_ASSERT(!editor.inputPanel().candidateList());
        FCITX_ASSERT(editor.text() == "việt~") << editor.text();

        editor.press(tilde);
        FCITX_ASSERT(editor.press(Key(FcitxKey_2)));
        FCITX_ASSERT(!editor.inputPanel().candidateList());
        editor.type(" tieengs");
        FCITX_ASSERT(editor.text() == "việt~ tiếng") << editor.text();
        FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();

        // The table opens on the current mode.
        auto *engine = instance->inputMethodEngine("bamboo");
        const auto *entry = instance->inputMethodManager().entry("bamboo");
        editor.press(tilde);
        editor.press(Key(FcitxKey_Down));
        FCITX_ASSERT(editor.press(Key(FcitxKey_Return)));
        FCITX_ASSERT(engine->subMode(*entry, editor) == "Telex (Plain Preedit)")
            << engine->subMode(*entry, editor);
        StatusUpdates updates(instance);
        for (auto [key, mode] :
             {std::pair{FcitxKey_4, "Telex (Input Method Window)"},
              std::pair{FcitxKey_5, "Telex (BackSpace → Input Method Window)"},
              std::pair{FcitxKey_3, "Telex (Plain Preedit)"}}) {
            editor.press(tilde);
            updates.count = 0;
            FCITX_ASSERT(editor.press(Key(key)));
            FCITX_ASSERT(engine->subMode(*entry, editor) == mode)
                << engine->subMode(*entry, editor);
            // The panel shows it.
            FCITX_ASSERT(updates.count == 1) << updates.count;
        }
        editor.type(" aa");
        FCITX_ASSERT(editor.text() == "việt~ tiếng ") << editor.text();
        FCITX_ASSERT(editor.preedit() == "â") << editor.preedit();
        editor.press(tilde);
        FCITX_ASSERT(editor.press(Key(FcitxKey_6)));
        editor.type(" aa");
        FCITX_ASSERT(editor.text() == "việt~ tiếng â aa") << editor.text();

        // An excluded application can still get Vietnamese back.
        FCITX_ASSERT(editor.press(tilde));
        FCITX_ASSERT(editor.press(Key(FcitxKey_Escape)));
        FCITX_ASSERT(!editor.inputPanel().candidateList());
        editor.press(tilde);
        editor.press(Key(FcitxKey_1));
        editor.type(" aa");
        FCITX_ASSERT(editor.preedit() == "â") << editor.preedit();
        // Any other key closes it and types as usual.
        editor.press(tilde);
        editor.type("s");
        FCITX_ASSERT(!editor.inputPanel().candidateList());
        FCITX_ASSERT(editor.preedit() == "s") << editor.preedit();
    }
    auto *appModes = bamboo->getSubConfig("app_modes");
    RawConfig saved;
    appModes->save(saved);
    FCITX_ASSERT(saved.valueByPath("AppMode/0/Program") &&
                 *saved.valueByPath("AppMode/0/Program") == "pickerapp");
    FCITX_ASSERT(*saved.valueByPath("AppMode/0/Mode") == "Preedit");
    FCITX_ASSERT(!saved.valueByPath("AppMode/1/Program"));
    {
        // Without a program name there is nothing to remember a mode for.
        FakeEditor editor(instance, "", PreeditCaps);
        FCITX_ASSERT(!editor.press(tilde));
        FCITX_ASSERT(editor.text() == "~") << editor.text();
    }
    RawConfig config;
    config.setValueByPath("InputMethod", "VIQR");
    bamboo->setConfig(config);
    {
        // '~' is VIQR's tone key, it must not open the table mid-word.
        FakeEditor editor(instance, "pickerapp", PreeditCaps);
        editor.type("a");
        FCITX_ASSERT(editor.press(tilde));
        FCITX_ASSERT(!editor.inputPanel().candidateList());
        FCITX_ASSERT(editor.preedit() == "ã") << editor.preedit();
    }
    config.setValueByPath("InputMethod", "Telex");
    bamboo->setConfig(config);
    clearList(bamboo, "app_modes", "AppMode");
}

// bamboo-core panics on this entry (3 targets, 2 results); fcitx5 must live.
void testBrokenCustomKeymap(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig keymap;
    keymap.setValueByPath("CustomKeymap/0/Key", "w");
    keymap.setValueByPath("CustomKeymap/0/Value", "UOA_ƯƠ");
    bamboo->setSubConfig("custom_keymap", keymap);
    RawConfig config;
    config.setValueByPath("InputMethod", "Custom");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("aw");
        FCITX_ASSERT(editor.text() == "aw") << editor.text();
    }
    config.setValueByPath("InputMethod", "Telex");
    bamboo->setConfig(config);
    clearList(bamboo, "custom_keymap", "CustomKeymap");
}

} // namespace

int main() {
    setupTestingEnvironmentPath(TESTING_BINARY_DIR, {"src"}, {"test"});
    char arg0[] = "testbamboo";
    char arg1[] = "--disable=all";
    char arg2[] = "--enable=testim,bamboo";
    char *argv[] = {arg0, arg1, arg2};
    Log::setLogRule("default=5,bamboo=5");
    Instance instance(FCITX_ARRAY_SIZE(argv), argv);
    instance.addonManager().registerDefaultLoader(nullptr);
    TimedSteps steps;
    instance.eventDispatcher().schedule([&instance, &steps]() {
        setup(&instance);
        testPreedit(&instance);
        testRestoreKeyStroke(&instance);
        testLockKeysKeepWord(&instance);
        testBrokenCustomKeymap(&instance);
        testSpellCheckAction(&instance);
        testInputModes(&instance);
        testTypingModes(&instance);
        testKindModes(&instance);
        testInputModePicker(&instance);
        testFieldHints(&instance);
        testPasswordFields(&instance);
        testModeLabel(&instance);
        testTerminalEscape(&instance);
        testSpellCheckExceptions(&instance);
        testEditWordBeforeCursor(&instance);
        testStandaloneW(&instance);
        testQuickTyping(&instance);
        testCapitalizeSentences(&instance);
        testConvert(&instance);
        testNoUnderline(&instance);
        testSlowReports(&instance, steps);
        steps.add(0, [&instance]() {
            instance.eventDispatcher().detach();
            instance.exit();
        });
        steps.run(&instance);
    });
    instance.exec();
    return 0;
}
