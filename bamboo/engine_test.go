/*
 * SPDX-FileCopyrightText: 2026 googlesky
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */

package main

import (
	"bamboo-core"
	"math/rand"
	"sort"
	"strings"
	"testing"
	"time"
)

const (
	keyReturn   = 0xff0d
	keyControlL = 0xffe3
	keyLeft     = 0xff51
)

// testApp plays the C++ side plus the application: deletions, commits and
// the keys the engine lets through end up in text. Preedit mode keeps the
// word in e.preeditText, surrounding text mode edits text directly.
type testApp struct {
	e           *FcitxBambooEngine
	surrounding bool
	editWord    bool // like the C++ side with EditWordBeforeCursor
	capitalize  bool // the next key starts a sentence
	text        []rune
}

func newTestApp(imName string, macros [][2]string, surrounding bool) *testApp {
	var im = bamboo.ParseInputMethod(bamboo.InputMethodDefinitions, imName)
	var e = newFcitxBambooEngine(im, map[string]bool{}, newMacroTable(macros))
	e.macroEnabled = macros != nil
	e.autoCapitalizeMacro = true
	return &testApp{e: e, surrounding: surrounding}
}

// The restore key, as the C++ side handles it.
func (a *testApp) restoreKeyStrokes() {
	a.e.restoreKeyStrokes(a.surrounding)
	var n = a.e.takeDeleteCount()
	a.text = append(a.text[:len(a.text)-n], []rune(a.e.takeCommitText())...)
}

func (a *testApp) press(keyVal, state uint32) bool {
	if a.editWord {
		a.e.editWordBeforeCursor(string(a.text), keyVal, state, a.surrounding)
	}
	var handled = a.e.processKeyEvent(keyVal, state, a.surrounding, a.capitalize)
	a.capitalize = false
	var n = a.e.takeDeleteCount()
	if n > len(a.text) {
		panic("deleting more than the text")
	}
	a.text = append(a.text[:len(a.text)-n], []rune(a.e.takeCommitText())...)
	if !handled && keyVal == FcitxBackSpace && state&FcitxControlMask != 0 {
		for len(a.text) > 0 && a.text[len(a.text)-1] != ' ' {
			a.text = a.text[:len(a.text)-1]
		}
	}
	if !handled && state&(FcitxControlMask|FcitxMod1Mask|FcitxMod4Mask) == 0 {
		switch {
		case keyVal == FcitxBackSpace && len(a.text) > 0:
			a.text = a.text[:len(a.text)-1]
		case keyVal == keyReturn:
			a.text = append(a.text, '\n')
		case keyVal == FcitxTab:
			a.text = append(a.text, '\t')
		case keyVal >= 0x20 && keyVal < 0x7f:
			a.text = append(a.text, rune(keyVal))
		}
	}
	return handled
}

// typeString types ASCII keys, \b \t \n \x01 being BackSpace, Tab, Return
// and Left.
func (a *testApp) typeString(s string) {
	for _, c := range s {
		var keyVal = map[rune]uint32{'\b': FcitxBackSpace, '\t': FcitxTab, '\n': keyReturn, '\x01': keyLeft}[c]
		if keyVal == 0 {
			keyVal = uint32(c)
		}
		a.press(keyVal, 0)
	}
}

// Cases ported from ibus-bamboo engine_test.go (TestPreeditEngine).
func TestPreeditEngine(t *testing.T) {
	var vn = [][2]string{{"vn", "việt nam"}}
	var arrow = [][2]string{{"->", "arrow"}}
	var star = [][2]string{{"csao", "✪"}, {"csao2", "✬"}}
	for _, tc := range []struct {
		name     string
		macros   [][2]string
		keys     string
		preedits []string    // expected preedit after each key of keys
		after    [][2]uint32 // key events sent after keys
		text     string      // application text at the end
	}{
		{name: "control_a", after: [][2]uint32{{keyControlL, 0}, {'a', FcitxControlMask}}},
		{name: "macro_control_a", macros: arrow, after: [][2]uint32{{keyControlL, 0}, {'a', FcitxControlMask}}},
		{name: "duowidro", keys: "duowidro", preedits: []string{"d", "du", "duo", "dươ", "dươi", "đươi", "đưởi", "đuổi"}},
		{name: "duowidro_enter", keys: "duowidro", after: [][2]uint32{{keyReturn, 0}}, text: "đuổi\n"},
		{name: "macro_vowl_space", macros: vn, keys: "vowl ", preedits: []string{"v", "vo", "vơ", "vơl", ""},
			after: [][2]uint32{{keyControlL, 0}}, text: "vowl "},
		{name: "macro_vowl_enter", macros: vn, keys: "vowl", after: [][2]uint32{{keyReturn, 0}}, text: "vowl\n"},
		{name: "macro_duowidro_enter", macros: vn, keys: "duowidro", after: [][2]uint32{{keyReturn, 0}}, text: "đuổi\n"},
		{name: "workaround_spreadsheet_number_enter", keys: "12", preedits: []string{"", ""},
			after: [][2]uint32{{keyReturn, 0}}, text: "12\n"},
		{name: "macro_vn_dot", macros: vn, keys: "vn.", preedits: []string{"v", "vn", ""}, text: "việt nam."},
		{name: "macro_vn_comma_space", macros: vn, keys: "vn, ", after: [][2]uint32{{keyReturn, 0}}, text: "việt nam, \n"},
		{name: "macro_vn_enter", macros: vn, keys: "vn", after: [][2]uint32{{keyReturn, 0}}, text: "việt nam\n"},
		{name: "macro_arrow_dot", macros: arrow, keys: "->.", preedits: []string{"-", "->", ""}, text: "arrow."},
		{name: "macro_arrow_enter", macros: arrow, keys: "->", after: [][2]uint32{{keyReturn, 0}}, text: "arrow\n"},
		{name: "macro_csao_space", macros: star, keys: "csao ", text: "✪ "},
		{name: "macro_csao2_enter", macros: star, keys: "csao2", after: [][2]uint32{{keyReturn, 0}}, text: "✬\n"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			a := newTestApp("Telex", tc.macros, false)
			for i, c := range tc.keys {
				a.press(uint32(c), 0)
				if i < len(tc.preedits) && a.e.preeditText != tc.preedits[i] {
					t.Errorf("after %q: preedit = %q, want %q", tc.keys[:i+1], a.e.preeditText, tc.preedits[i])
				}
			}
			for _, k := range tc.after {
				a.press(k[0], k[1])
			}
			if string(a.text) != tc.text {
				t.Errorf("text = %q, want %q", string(a.text), tc.text)
			}
		})
	}
}

// Macros take the date and time as Vietnamese write them.
func TestMacroDateTime(t *testing.T) {
	defer func(f func() time.Time) { now = f }(now)
	now = func() time.Time { return time.Date(2026, 11, 15, 9, 5, 0, 0, time.Local) }
	a := newTestApp("Telex", [][2]string{{"nay", "hôm nay $DATE lúc $TIME"}}, false)
	for _, c := range "nay " {
		a.press(uint32(c), 0)
	}
	if want := "hôm nay 15/11/2026 lúc 09:05 "; string(a.text) != want {
		t.Errorf("text = %q, want %q", string(a.text), want)
	}
}

func TestSuperShortcutIsNotTyped(t *testing.T) {
	a := newTestApp("Telex", nil, false)
	if a.press('a', FcitxMod4Mask) || a.e.preeditText != "" || len(a.text) != 0 {
		t.Errorf("Super+a was typed: preedit %q, text %q", a.e.preeditText, string(a.text))
	}
}

// Macro keys follow ibus-bamboo: case-insensitive only with auto capitalize.
func TestMacroCase(t *testing.T) {
	for _, tc := range []struct {
		name       string
		macros     [][2]string
		autoCap    bool
		keys, text string
	}{
		{"upper_key_lower_typed", [][2]string{{"VN", "Việt Nam"}}, true, "vn ", "việt nam "},
		{"lower_key_upper_typed", [][2]string{{"vn", "Việt Nam"}}, true, "VN ", "VIỆT NAM "},
		{"no_autocap_exact_only", [][2]string{{"vn", "Việt Nam"}}, false, "VN ", "VN "},
		{"no_autocap_exact", [][2]string{{"vn", "Việt Nam"}}, false, "vn ", "Việt Nam "},
		{"prefix_ignores_case", [][2]string{{"a1", "xyz"}}, true, "A1 ", "xyz "},
		{"empty_key_ignored", [][2]string{{"", "oops"}, {" ", "oops"}}, true, "a ", "a "},
		{"empty_value_ignored", [][2]string{{"vn", ""}}, true, "vn ", "vn "},
		{"exact_key_first", [][2]string{{"ms", "Microsoft"}, {"MS", "Mississippi"}}, true, "ms MS ", "microsoft MISSISSIPPI "},
	} {
		t.Run(tc.name, func(t *testing.T) {
			a := newTestApp("Telex", tc.macros, false)
			a.e.autoCapitalizeMacro = tc.autoCap
			a.typeString(tc.keys)
			if string(a.text) != tc.text {
				t.Errorf("text = %q, want %q", string(a.text), tc.text)
			}
		})
	}
}

func TestCommitUsesOutputCharset(t *testing.T) {
	a := newTestApp("Telex", nil, false)
	a.e.outputCharset = "TCVN3 (ABC)"
	a.typeString("tieengs vieetj\n")
	if want := bamboo.Encode("TCVN3 (ABC)", "tiếng việt") + "\n"; string(a.text) != want {
		t.Errorf("text = %q, want %q", string(a.text), want)
	}
}

// A Go panic inside a cgo call would kill fcitx5.
func TestExportsSurviveInvalidHandle(t *testing.T) {
	if EngineProcessKeyEvent(0xdead, 'a', 0, false, false) {
		t.Error("invalid engine handled a key")
	}
	ResetEngine(0xdead)
	DeleteObject(0xdead)
}

func TestSurroundingEngine(t *testing.T) {
	for _, tc := range []struct {
		name, keys, text string
		macros           [][2]string
		charset          string
		noSpellCheck     bool
	}{
		{name: "words", keys: "tieengs vieetj\n", text: "tiếng việt\n"},
		{name: "backspace_last_char", keys: "tieengs\bg", text: "tiếng"},
		{name: "backspace_moves_tone", keys: "hoafn\b", text: "hòa"},
		{name: "backspace_moves_tone_no_spell_check", keys: "hoafn\b", text: "hòa", noSpellCheck: true},
		{name: "backspace_whole_word", keys: "dd\b\bx", text: "x"},
		{name: "restore_invalid_word", keys: "text ", text: "text "},
		{name: "undo_mark", keys: "aaa ", text: "aa "},
		{name: "number", keys: "12", text: "12"},
		{name: "macro", keys: "vn, ", text: "việt nam, ", macros: [][2]string{{"vn", "việt nam"}}},
		{name: "charset", keys: "tieengs", text: bamboo.Encode("TCVN3 (ABC)", "tiếng"), charset: "TCVN3 (ABC)"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			a := newTestApp("Telex", tc.macros, true)
			if tc.charset != "" {
				a.e.outputCharset = tc.charset
			}
			a.e.autoNonVnRestore = !tc.noSpellCheck
			a.typeString(tc.keys)
			if string(a.text) != tc.text {
				t.Errorf("text = %q, want %q", string(a.text), tc.text)
			}
			if a.e.preeditText != "" {
				t.Errorf("preedit = %q in surrounding text mode", a.e.preeditText)
			}
		})
	}
}

// Proper nouns and jargon the spell check would restore to keys.
func TestSpellCheckExceptions(t *testing.T) {
	for _, surrounding := range []bool{false, true} {
		a := newTestApp("Telex", nil, surrounding)
		a.typeString("Kroong ")
		if string(a.text) != "Kroong " {
			t.Errorf("surrounding %v: without exception %q", surrounding, string(a.text))
		}
		a = newTestApp("Telex", nil, surrounding)
		a.e.spellCheckExceptions = []string{"krông"}
		a.typeString("Kroong KROONG Kroongs text ")
		if string(a.text) != "Krông KRÔNG Kroongs text " {
			t.Errorf("surrounding %v: with exception %q", surrounding, string(a.text))
		}
	}
}

// Ctrl+BackSpace deletes a whole word in the application, it must end the
// word like any other shortcut.
func TestControlBackSpaceEndsWord(t *testing.T) {
	for _, surrounding := range []bool{false, true} {
		a := newTestApp("Telex", nil, surrounding)
		a.typeString("abc tien")
		if a.press(FcitxBackSpace, FcitxControlMask) {
			t.Errorf("surrounding %v: Ctrl+BackSpace did not reach the application", surrounding)
		}
		a.typeString("e")
		if string(a.text)+a.e.preeditText != "abc e" {
			t.Errorf("surrounding %v: text %q preedit %q", surrounding, string(a.text), a.e.preeditText)
		}
	}
}

// Surrounding text mode must show exactly what preedit mode shows (commits
// plus preedit) after every key, whatever the input method and options.
func TestModesAgree(t *testing.T) {
	var keys = []rune("aeiouydwsfrxjztnhgcmqplkvbAEOUDWS  ,.'`~^+([]{}0123456789\b\b\b\t\n\x01")
	var ims []string
	for name := range bamboo.InputMethodDefinitions {
		ims = append(ims, name)
	}
	sort.Strings(ims)
	var dict = map[string]bool{"việt": true, "tiếng": true, "tôi": true}
	var options = []struct {
		restore, dict, modern, editWord, quick bool
		charset                                string
	}{
		{true, false, false, false, false, "Unicode"},
		{false, false, false, false, false, "Unicode"},
		{true, true, true, false, false, "Unicode"},
		{true, false, false, false, false, "TCVN3 (ABC)"},
		{true, false, true, false, false, "VIQR"},
		{true, false, false, true, false, "Unicode"},
		{false, false, true, true, false, "Unicode"},
		{true, false, false, false, true, "Unicode"},
		{true, true, true, true, true, "Unicode"},
		{false, false, false, false, true, "TCVN3 (ABC)"},
	}
	for _, im := range ims {
		for _, o := range options {
			for _, macros := range [][][2]string{nil, {{"vn", "việt nam"}, {"->", "→"}}} {
				for seed := int64(0); seed < 40; seed++ {
					var r = rand.New(rand.NewSource(seed))
					var p, b = newTestApp(im, macros, false), newTestApp(im, macros, true)
					p.editWord, b.editWord = o.editWord, o.editWord
					for _, e := range []*FcitxBambooEngine{p.e, b.e} {
						e.quickDouble, e.quickStart, e.quickEnd = o.quick, o.quick, o.quick
					}
					for _, e := range []*FcitxBambooEngine{p.e, b.e} {
						e.autoNonVnRestore, e.spellCheckWithDicts, e.dictionary = o.restore, o.dict, dict
						e.outputCharset = o.charset
						if o.modern {
							e.preeditor.SetFlag(bamboo.EstdFlags &^ bamboo.EstdToneStyle)
						}
					}
					var typed string
					for i := 0; i < 14; i++ {
						var key = string(keys[r.Intn(len(keys))])
						typed += key
						p.typeString(key)
						b.typeString(key)
						if string(p.text)+p.e.preeditText != string(b.text) {
							t.Fatalf("%s %+v macros %v keys %q: preedit mode %q+%q, surrounding %q",
								im, o, macros != nil, typed, string(p.text), p.e.preeditText, string(b.text))
						}
					}
				}
			}
		}
	}
}

// A key right after a word edits it like one being typed.
func TestEditWordBeforeCursor(t *testing.T) {
	for _, tc := range []struct {
		name, im, text, keys, want string
	}{
		{name: "tone", text: "xin chao", keys: "f", want: "xin chào"},
		{name: "after_backspace", keys: "vieet \bj ", want: "việt "},
		{name: "tone_moves", text: "hòa", keys: "s", want: "hóa"},
		{name: "mark", text: "tuong", keys: "w", want: "tương"},
		{name: "upper_case", text: "VIêt", keys: "j", want: "VIệt"},
		{name: "after_punctuation", text: "(viet", keys: "j", want: "(viẹt"},
		{name: "vni", im: "VNI", text: "viet", keys: "65", want: "việt"},
		{name: "other_tone_style", text: "hoà", keys: "s", want: "hoàs"},
		{name: "not_vietnamese", text: "hello", keys: "s", want: "hellos"},
		{name: "longer_than_a_word", text: "xnghieng", keys: "s", want: "xnghiengs"},
		{name: "not_a_typing_key", text: "viet", keys: "1", want: "viet1"},
		{name: "made_up_keys_never_restored", text: "việt", keys: "f ", want: "việtf "},
		{name: "next_word_restored", text: "việt", keys: "f class ", want: "việtf class "},
		{name: "english_word_goes_on", text: "te", keys: "xt ", want: "text "},
	} {
		for _, surrounding := range []bool{false, true} {
			var im = tc.im
			if im == "" {
				im = "Telex"
			}
			a := newTestApp(im, nil, surrounding)
			a.editWord = true
			a.text = []rune(tc.text)
			a.typeString(tc.keys)
			if got := string(a.text) + a.e.preeditText; got != tc.want {
				t.Errorf("%s surrounding %v: got %q, want %q", tc.name, surrounding, got, tc.want)
			}
		}
	}
}

// The restore key shows a word taken back as it was, then the keys typed.
func TestEditWordRestoreKey(t *testing.T) {
	for _, surrounding := range []bool{false, true} {
		a := newTestApp("Telex", nil, surrounding)
		a.editWord = true
		a.text = []rune("xin việt")
		a.typeString("z")
		a.restoreKeyStrokes()
		if got := string(a.text) + a.e.preeditText; got != "xin việtz" {
			t.Errorf("surrounding %v: %q", surrounding, got)
		}
		// Words before leave no trace on the one taken back.
		a = newTestApp("Telex", nil, surrounding)
		a.capitalize = true
		a.typeString("chaof ")
		a.editWord = true
		a.text = append(a.text, []rune("viêt")...)
		if a.typeString("j "); string(a.text) != "Chào việt " {
			t.Errorf("surrounding %v: %q", surrounding, string(a.text))
		}
		a = newTestApp("Telex", nil, surrounding)
		a.e.quickEnd = true
		a.typeString("tooi")
		a.restoreKeyStrokes()
		a.typeString(" ")
		a.editWord = true
		a.text = append(a.text, []rune("ca")...)
		a.typeString("g ")
		if got := string(a.text); got != "tooi cang " {
			t.Errorf("surrounding %v: %q", surrounding, got)
		}
	}
}

// Preedit mode moves the word into the preedit, surrounding text mode
// replaces its changed tail.
func TestEditWordBeforeCursorOutput(t *testing.T) {
	a := newTestApp("Telex", nil, false)
	if n := a.e.editWordBeforeCursor("xin toi", 's', 0, false); n != 3 ||
		a.e.takeDeleteCount() != 3 || a.e.preeditText != "toi" {
		t.Errorf("preedit mode: took %d, preedit %q", n, a.e.preeditText)
	}
	a = newTestApp("Telex", nil, true)
	a.e.editWordBeforeCursor("xin toi", 's', 0, true)
	a.e.processKeyEvent('s', 0, true, false)
	if n, commit := a.e.takeDeleteCount(), a.e.takeCommitText(); n != 2 || commit != "ói" {
		t.Errorf("surrounding mode: deleted %d, committed %q", n, commit)
	}
	a = newTestApp("Telex", nil, false)
	a.typeString("vie")
	if a.e.editWordBeforeCursor("toi", 's', 0, false) != 0 {
		t.Error("took a word while composing")
	}
	a = newTestApp("Telex", nil, false)
	a.e.outputCharset = "TCVN3 (ABC)"
	if a.e.editWordBeforeCursor("toi", 's', 0, false) != 0 {
		t.Error("took a word with a legacy charset")
	}
	a = newTestApp("Telex", nil, false)
	if a.e.editWordBeforeCursor("toi", 's', FcitxControlMask, false) != 0 {
		t.Error("took a word for a shortcut")
	}
}

// What a w with nothing to mark types, UniKey's "process W at word begin".
func TestStandaloneW(t *testing.T) {
	for _, tc := range []struct {
		name, im string
		w        int
		keys     string
		preedits []string
		text     string
	}{
		{name: "telex", im: "Telex", w: standaloneWDefault, keys: "w", preedits: []string{"w"}},
		{name: "telex_always", im: "Telex", w: standaloneWAlways, keys: "w", preedits: []string{"ư"}},
		{name: "telex_always_word", im: "Telex", w: standaloneWAlways, keys: "nhwng ", text: "nhưng "},
		{name: "telex_always_mark", im: "Telex", w: standaloneWAlways, keys: "tuwong ", text: "tương "},
		{name: "telex_not_at_start", im: "Telex", w: standaloneWNotAtWordStart, keys: "wnhw", preedits: []string{"w"}},
		{name: "telex_not_at_start_word", im: "Telex", w: standaloneWNotAtWordStart, keys: "nhwng web ", text: "nhưng web "},
		{name: "telex_w", im: "Telex W", w: standaloneWDefault, keys: "w", preedits: []string{"ư"}},
		{name: "telex_w_not_at_start", im: "Telex W", w: standaloneWNotAtWordStart, keys: "wa", preedits: []string{"w", "wa"}},
		{name: "telex_w_not_at_start_upper", im: "Telex 2", w: standaloneWNotAtWordStart, keys: "W", preedits: []string{"W"}},
		{name: "vni", im: "VNI", w: standaloneWAlways, keys: "w", preedits: []string{"w"}},
	} {
		for _, surrounding := range []bool{false, true} {
			a := newTestApp(tc.im, nil, surrounding)
			a.e.setStandaloneW(tc.w, bamboo.EstdFlags)
			for i, c := range tc.keys {
				a.press(uint32(c), 0)
				var shown = a.e.preeditText
				if surrounding {
					shown = string(a.text)
				}
				if i < len(tc.preedits) && shown != tc.preedits[i] {
					t.Errorf("%s surrounding %v: after %q shows %q, want %q", tc.name, surrounding, tc.keys[:i+1], shown, tc.preedits[i])
				}
			}
			if tc.text != "" && string(a.text) != tc.text {
				t.Errorf("%s surrounding %v: text %q, want %q", tc.name, surrounding, string(a.text), tc.text)
			}
		}
	}
}

// OpenKey's quick typing: keys that are no Vietnamese word are typed as
// their rewrite when that is one.
func TestQuickTyping(t *testing.T) {
	const double, start, end = 1, 2, 4
	for _, tc := range []struct {
		name       string
		quick      int
		w          int
		macros     [][2]string
		keys, text string
	}{
		{name: "double", quick: double, keys: "ccaf gga kkoong ppas qqaf nnaf tte ", text: "chà gia không phá quà ngà the "},
		{name: "double_upper", quick: double, keys: "CCAF Ccaf ", text: "CHÀ Chà "},
		{name: "double_twice_in_a_row", quick: double, keys: "ccc ", text: "ccc "},
		{name: "english_restored", quick: double | start | end, keys: "happy file account ", text: "happy file account "},
		{name: "beginning_only", quick: double, keys: "ccaks ", text: "ccaks "},
		{name: "start", quick: start, keys: "fair jaf wa Fair FAIR ", text: "phải già qua Phải PHẢI "},
		{name: "start_needs_more", quick: start, keys: "f ", text: "f "},
		{name: "end", quick: end, keys: "dog cagf caks nhah DOG ", text: "dong càng cách nhanh DONG "},
		{name: "end_tab", quick: end, keys: "caks\t", text: "cách\t"},
		{name: "end_consonants_kept", quick: end, keys: "thanh nghieng ", text: "thanh nghieng "},
		{name: "off", keys: "ccaf dog fair ", text: "ccaf dog fair "},
		{name: "valid_word_wins", quick: start, w: standaloneWAlways, keys: "wa ", text: "ưa "},
		{name: "macro_wins", quick: double, macros: [][2]string{{"cc", "xyz"}}, keys: "cc ", text: "xyz "},
	} {
		for _, surrounding := range []bool{false, true} {
			a := newTestApp("Telex", tc.macros, surrounding)
			a.e.quickDouble, a.e.quickStart, a.e.quickEnd = tc.quick&double != 0, tc.quick&start != 0, tc.quick&end != 0
			a.e.setStandaloneW(tc.w, bamboo.EstdFlags)
			a.typeString(tc.keys)
			if string(a.text) != tc.text {
				t.Errorf("%s surrounding %v: text %q, want %q", tc.name, surrounding, string(a.text), tc.text)
			}
		}
	}
}

// Macro keys may put punctuation before the word, quick typing keeps it;
// with macros on the rewrite shows while typing too.
func TestQuickTypingWithMacros(t *testing.T) {
	for _, surrounding := range []bool{false, true} {
		var texts [2]string
		for i, quick := range []bool{false, true} {
			a := newTestApp("Telex", [][2]string{{"->", "→"}}, surrounding)
			a.e.quickDouble = quick
			a.typeString("->cca ")
			texts[i] = string(a.text)
		}
		if texts[0] != texts[1] {
			t.Errorf("surrounding %v: %q with quick typing, %q without", surrounding, texts[1], texts[0])
		}
		a := newTestApp("Telex", [][2]string{{"hn", "Hà Nội"}}, surrounding)
		a.e.quickDouble = true
		if a.typeString("ccaf"); string(a.text)+a.e.preeditText != "chà" {
			t.Errorf("surrounding %v: ccaf shows %q", surrounding, string(a.text)+a.e.preeditText)
		}
		if a.typeString(" hn "); string(a.text) != "chà hà nội " {
			t.Errorf("surrounding %v: %q", surrounding, string(a.text))
		}
	}
}

// The C++ side asks for an upper case first letter at a sentence start:
// the word is typed as usual, what it shows starts upper case.
func TestCapitalizeWord(t *testing.T) {
	for _, tc := range []struct {
		name       string
		macros     [][2]string
		autoCap    bool
		keys, text string
	}{
		{name: "word", keys: "vieetj nam ", text: "Việt nam "},
		{name: "english", keys: "hello ", text: "Hello "},
		{name: "dd", keys: "ddi ", text: "Đi "},
		{name: "macro", macros: [][2]string{{"vn", "việt nam"}}, autoCap: true, keys: "vn ", text: "Việt nam "},
		{name: "macro_exact", macros: [][2]string{{"vn", "việt nam"}}, keys: "vn ", text: "Việt nam "},
	} {
		for _, surrounding := range []bool{false, true} {
			a := newTestApp("Telex", tc.macros, surrounding)
			a.e.autoCapitalizeMacro = tc.autoCap
			a.capitalize = true
			a.typeString(tc.keys)
			if string(a.text) != tc.text {
				t.Errorf("%s surrounding %v: %q, want %q", tc.name, surrounding, string(a.text), tc.text)
			}
		}
	}
}

// What quick typing shows while typing, and the keys it keeps.
func TestQuickTypingLive(t *testing.T) {
	for _, surrounding := range []bool{false, true} {
		shown := func(a *testApp) string {
			return string(a.text) + a.e.preeditText
		}
		a := newTestApp("Telex", nil, surrounding)
		a.e.quickDouble, a.e.quickEnd = true, true
		if a.typeString("cc"); shown(a) != "ch" {
			t.Errorf("surrounding %v: cc shows %q", surrounding, shown(a))
		}
		if a.typeString("af"); shown(a) != "chà" {
			t.Errorf("surrounding %v: ccaf shows %q", surrounding, shown(a))
		}
		a.typeString("\b\b\b\bcag")
		if shown(a) != "cang" {
			t.Errorf("surrounding %v: cag shows %q", surrounding, shown(a))
		}
		if a.typeString("\b"); shown(a) != "ca" {
			t.Errorf("surrounding %v: BackSpace after cang shows %q", surrounding, shown(a))
		}
		a.typeString(" tuwog")
		if !strings.HasSuffix(shown(a), " tương") {
			t.Errorf("surrounding %v: tuwog shows %q", surrounding, shown(a))
		}
		a = newTestApp("Telex", nil, surrounding)
		a.e.quickDouble = true
		a.typeString("ccaf")
		a.restoreKeyStrokes()
		if shown(a) != "ccaf" {
			t.Errorf("surrounding %v: restore key shows %q", surrounding, shown(a))
		}
	}
}

// UniKey toolkit's conversions of a selection.
func TestTextTransforms(t *testing.T) {
	var transforms = func(im, text string) map[string]string {
		var list = newTestApp(im, nil, false).e.textTransforms(text)
		var m = map[string]string{}
		for i := 0; i+1 < len(list); i += 2 {
			m[list[i]] = list[i+1]
		}
		return m
	}
	var got = transforms("Telex", "Tieengs vieejt, class ĐẸP")
	for kind, want := range map[string]string{
		"retype": "Tiếng việt, class ĐẸP",
		"plain":  "Tieengs vieejt, class DEP",
		"upper":  "TIEENGS VIEEJT, CLASS ĐẸP",
		"lower":  "tieengs vieejt, class đẹp",
		"title":  "Tieengs Vieejt, Class Đẹp",
	} {
		if got[kind] != want {
			t.Errorf("%s: %q, want %q", kind, got[kind], want)
		}
	}
	if got = transforms("VNI", "vie65t"); got["retype"] != "việt" {
		t.Errorf("VNI retype: %q", got["retype"])
	}
	for _, cs := range []string{"TCVN3 (ABC)", "VNI Windows", "VIQR"} {
		if got = transforms("Telex", bamboo.Encode(cs, "Tiếng Việt")); got[cs] != "Tiếng Việt" {
			t.Errorf("from %s: %q", cs, got[cs])
		}
	}
	// Only what changes the text is offered.
	got = transforms("Telex", "việt nam")
	for _, kind := range []string{"retype", "lower", "TCVN3 (ABC)"} {
		if _, ok := got[kind]; ok {
			t.Errorf("%s offered: %q", kind, got[kind])
		}
	}
}
