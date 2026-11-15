/*
 * SPDX-FileCopyrightText: 2022-2022 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */
#ifndef _FCITX5_BAMBOO_BAMBOOCONFIG_H_
#define _FCITX5_BAMBOO_BAMBOOCONFIG_H_

#include <algorithm>
#include <cstddef>
#include <fcitx-config/configuration.h>
#include <fcitx-config/enum.h>
#include <fcitx-config/option.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/stringutils.h>
#include <string>
#include <utility>
#include <vector>

namespace fcitx {

struct InputMethodConstrain;
struct InputMethodAnnotation;
using InputMethodOption =
    Option<std::string, InputMethodConstrain, DefaultMarshaller<std::string>,
           InputMethodAnnotation>;

struct StringListAnnotation : public EnumAnnotation {
    void setList(std::vector<std::string> list) { list_ = std::move(list); }
    const auto &list() { return list_; }
    void dumpDescription(RawConfig &config) const {
        EnumAnnotation::dumpDescription(config);
        for (size_t i = 0; i < list_.size(); i++) {
            config.setValueByPath("Enum/" + std::to_string(i), list_[i]);
        }
    }

protected:
    std::vector<std::string> list_;
};

struct InputMethodAnnotation : public StringListAnnotation {
    void dumpDescription(RawConfig &config) const {
        StringListAnnotation::dumpDescription(config);
        config.setValueByPath("LaunchSubConfig", "True");
        for (size_t i = 0; i < list_.size(); i++) {
            config.setValueByPath(
                "SubConfigPath/" + std::to_string(i),
                stringutils::concat("fcitx://config/addon/bamboo/macro/",
                                    list_[i]));
        }
    }
};

struct InputMethodConstrain {
    using Type = std::string;

    InputMethodConstrain(const InputMethodOption *option) : option_(option) {}

    bool check(const std::string &name) const {
        // Avoid check during initialization
        const auto &list = option_->annotation().list();
        if (list.empty()) {
            return true;
        }
        return std::find(list.begin(), list.end(), name) != list.end();
    }
    void dumpDescription(RawConfig & /*unused*/) const {}

protected:
    const InputMethodOption *option_;
};

FCITX_CONFIGURATION(BambooKeymap,
                    Option<std::string> key{this, "Key", _("Key"), ""};
                    Option<std::string> value{this, "Value", _("Value"), ""};);

FCITX_CONFIGURATION(
    BambooMacroTable,
    OptionWithAnnotation<std::vector<BambooKeymap>, ListDisplayOptionAnnotation>
        macros{this,
               "Macro",
               _("Macro"),
               {},
               {},
               {},
               ListDisplayOptionAnnotation("Key")};);

FCITX_CONFIGURATION(
    BambooCustomKeymap,
    OptionWithAnnotation<std::vector<BambooKeymap>, ListDisplayOptionAnnotation>
        customKeymap{this,
                     "CustomKeymap",
                     _("Custom Keymap"),
                     {},
                     {},
                     {},
                     ListDisplayOptionAnnotation("Key")};);

using InputMethodOption =
    Option<std::string, InputMethodConstrain, DefaultMarshaller<std::string>,
           InputMethodAnnotation>;

// ibus-bamboo's typing modes ("chế độ gõ"). SurroundingText avoids the
// underline as the application allows: editing the text before the cursor,
// else plain preedit or fcitx5's window. The others are for applications it
// does not suit: preedit without underline, the word in fcitx5's window, the
// word typed and fixed with BackSpace. Exclude leaves the application alone.
// Configurations keep the names, the order is the picker's.
enum class BambooInputMode {
    Preedit,
    SurroundingText,
    PlainPreedit,
    InputMethodWindow,
    BackSpace,
    Exclude
};
FCITX_CONFIG_ENUM_NAME_WITH_I18N(BambooInputMode, N_("Preedit"),
                                 N_("Surrounding Text"), N_("Plain Preedit"),
                                 N_("Input Method Window"), N_("BackSpace"),
                                 N_("Exclude"));

// The typing mode of a kind of application, Default leaving it to the
// default typing mode. The others are BambooInputMode's, in its order.
enum class BambooKindMode {
    Default,
    Preedit,
    SurroundingText,
    PlainPreedit,
    InputMethodWindow,
    BackSpace,
    Exclude
};
FCITX_CONFIG_ENUM_NAME_WITH_I18N(BambooKindMode, N_("Default"), N_("Preedit"),
                                 N_("Surrounding Text"), N_("Plain Preedit"),
                                 N_("Input Method Window"), N_("BackSpace"),
                                 N_("Exclude"));

// Applications by how they reach fcitx5, for those without a mode of their
// own. Others, through IBus or other compositors, take the default mode.
FCITX_CONFIGURATION(
    BambooKindModes,
    OptionWithAnnotation<BambooKindMode, BambooKindModeI18NAnnotation> qtApps{
        this, "QtApplications", _("Qt applications through fcitx5-qt"),
        BambooKindMode::Default};
    OptionWithAnnotation<BambooKindMode, BambooKindModeI18NAnnotation>
        qtTerminals{this, "QtTerminals",
                    _("Qt terminals through fcitx5-qt (Konsole)"),
                    BambooKindMode::Default};
    OptionWithAnnotation<BambooKindMode, BambooKindModeI18NAnnotation> gtkApps{
        this, "GtkApplications",
        _("Applications through fcitx5-gtk (GTK; Chromium on X11)"),
        BambooKindMode::Default};
    OptionWithAnnotation<BambooKindMode, BambooKindModeI18NAnnotation>
        waylandApps{this, "WaylandApplications",
                    _("Wayland applications through KWin (Chromium, "
                      "Alacritty; Qt, GTK without fcitx5's modules)"),
                    BambooKindMode::Default};
    OptionWithAnnotation<BambooKindMode, BambooKindModeI18NAnnotation> x11Apps{
        this, "X11Applications", _("X11 applications through XIM"),
        BambooKindMode::Default};);

FCITX_CONFIGURATION(
    BambooAppMode,
    Option<std::string> program{this, "Program", _("Program"), ""};
    OptionWithAnnotation<BambooInputMode, BambooInputModeI18NAnnotation> mode{
        this, "Mode", _("Typing Mode"), BambooInputMode::Preedit};
    OptionWithAnnotation<bool, ToolTipAnnotation> terminal{
        this,
        "Terminal",
        _("Terminal or code editor"),
        false,
        {},
        {},
        ToolTipAnnotation(_("Escape switches to English, as vim needs, and "
                            "sentences are not capitalized"))};);

FCITX_CONFIGURATION(BambooAppModeList,
                    OptionWithAnnotation<std::vector<BambooAppMode>,
                                         ListDisplayOptionAnnotation>
                        appModes{this,
                                 "AppMode",
                                 _("Typing Mode per Application"),
                                 {},
                                 {},
                                 {},
                                 ListDisplayOptionAnnotation("Program")};);

// What a W with nothing to mark types. Telex W and Telex 2 type Ư, Always
// gives it to every Telex, NotAtWordStart types W at word start like UniKey
// without "Process W at word begin".
enum class BambooStandaloneW { Default, Always, NotAtWordStart };
FCITX_CONFIG_ENUM_NAME_WITH_I18N(BambooStandaloneW,
                                 N_("As the input method does"), N_("Ư"),
                                 N_("Ư, but W at word start"));

// OpenKey's quick typing, used when the keys typed make no Vietnamese word
// but their rewrite does.
FCITX_CONFIGURATION(
    BambooQuickTyping,
    OptionWithAnnotation<BambooStandaloneW, BambooStandaloneWI18NAnnotation>
        standaloneW{this, "StandaloneW", _("W with nothing to mark types"),
                    BambooStandaloneW::Default};
    Option<bool> doubleConsonants{
        this, "DoubleConsonants",
        _("cc→ch, gg→gi, kk→kh, nn→ng, pp→ph, qq→qu, tt→th"), false};
    Option<bool> startConsonants{this, "StartConsonants",
                                 _("f→ph, j→gi, w→qu at word start"), false};
    Option<bool> endConsonants{this, "EndConsonants",
                               _("g→ng, h→nh, k→ch at word end"), false};);

FCITX_CONFIGURATION(
    BambooConfig, KeyListOption restoreKeyStroke{this,
                                                 "RestoreKeyStroke",
                                                 _("Restore Key Stroke"),
                                                 {},
                                                 KeyListConstrain()};
    Option<std::string, InputMethodConstrain, DefaultMarshaller<std::string>,
           InputMethodAnnotation>
        inputMethod{this, "InputMethod", _("Input Method"), "Telex",
                    InputMethodConstrain(&inputMethod)};
    OptionWithAnnotation<std::string, StringListAnnotation> outputCharset{
        this, "OutputCharset", _("Output Charset"), "Unicode"};
    OptionWithAnnotation<BambooInputMode, BambooInputModeI18NAnnotation>
        inputMode{this, "DefaultInputMode", _("Default Typing Mode"),
                  BambooInputMode::Preedit};
    Option<BambooKindModes> kindModes{this, "KindModes",
                                      _("Typing Mode per Kind of Application")};
    SubConfigOption appModes{this, "AppModes", _("Typing Mode per Application"),
                             "fcitx://config/addon/bamboo/app_modes"};
    Option<bool> autoExcludeFields{
        this, "AutoExcludeFields",
        _("Disable Vietnamese in email, number and phone fields"), true};
    OptionWithAnnotation<bool, ToolTipAnnotation> waylandBackSpace{
        this,
        "WaylandBackSpace",
        _("Surrounding Text in Wayland apps without text: edit with BackSpace"),
        false,
        {},
        {},
        ToolTipAnnotation(_("Terminals typing through KWin get the word as "
                            "text instead of in the input method window"))};
    Option<bool> terminalEscape{
        this, "TerminalEscape",
        _("Escape switches to English in terminals and code editors"), true};
    Option<bool> capitalizeSentences{
        this, "CapitalizeSentences",
        _("Capitalize the first letter of sentences"), false};
    Option<bool> doubleSpacePeriod{this, "DoubleSpacePeriod",
                                   _("Double space types a period"), false};
    Option<bool> emDash{this, "EmDash", _("Two hyphens type an em dash"),
                        false};
    OptionWithAnnotation<bool, ToolTipAnnotation> editWordBeforeCursor{
        this,
        "EditWordBeforeCursor",
        _("Edit the word before the cursor"),
        true,
        {},
        {},
        ToolTipAnnotation(_("After BackSpace, arrows or a click, a key right "
                            "after a word edits it like one being typed"))};
    KeyListOption inputModeSwitchKey{
        this,
        "InputModeSwitchKey",
        _("Choose Typing Mode for Application"),
        {Key(FcitxKey_asciitilde)},
        KeyListConstrain(KeyConstrainFlag::AllowModifierLess)};
    // UniKey toolkit's key.
    KeyListOption convertKey{this,
                             "ConvertKey",
                             _("Convert the selection or the word before "
                               "the cursor"),
                             {Key("Control+Shift+F6")},
                             KeyListConstrain()};
    // Defaults follow ibus-bamboo. Spell check means restoring the keys of
    // invalid words, SpellCheck only picks the dictionary over the rules.
    Option<bool> autoNonVnRestore{this, "AutoNonVnRestore",
                                  _("Enable spell check"), true};
    Option<bool> spellCheck{this, "SpellCheck",
                            _("Use dictionary for spell check"), false};
    Option<std::vector<std::string>> spellCheckExceptions{
        this, "SpellCheckExceptions", _("Words kept by spell check"), {}};
    Option<bool> macro{this, "Macro", _("Enable Macro"), false};
    Option<bool> capitalizeMacro{this, "CapitalizeMacro", _("Capitalize Macro"),
                                 true};
    Option<bool> modernStyle{this, "ModernStyle",
                             _("Use oà, _uý (instead of òa, úy)"), false};
    Option<bool> freeMarking{this, "FreeMarking",
                             _("Allow type with more freedom"), true};
    Option<BambooQuickTyping> quickTyping{this, "QuickTyping",
                                          _("Quick Typing")};
    Option<bool> displayUnderline{this, "DisplayUnderline",
                                  _("Underline the preedit text"), false};
    SubConfigOption custumKeymap{this, "CustomKeymap", _("Custom Keymap"),
                                 "fcitx://config/addon/bamboo/custom_keymap"};);
} // namespace fcitx

#endif
