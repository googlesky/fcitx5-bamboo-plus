/*
 * SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
 * SPDX-FileCopyrightText: 2022-2022 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */
package main

import (
	"bamboo-core"
	"strings"
	"time"
	"unicode"
	"unicode/utf8"
)

type FcitxBambooEngine struct {
	preeditor               bamboo.IEngine
	inputMethod             bamboo.InputMethod // before the options, see setStandaloneW
	flags                   uint
	standaloneW             int
	quickDouble             bool // see quickWord
	quickStart              bool
	quickEnd                bool
	englishWord             bool // the restore key made the word English
	capitalized             bool // the word starts a sentence, see encodeText
	retypedWord             string
	retypedKeys             string // see retype
	macroTable              *MacroTable
	dictionary              map[string]bool
	autoNonVnRestore        bool
	ddFreeStyle             bool
	macroEnabled            bool
	autoCapitalizeMacro     bool
	lastKeyWithShift        bool
	spellCheckWithDicts     bool
	spellCheckExceptions    []string // lower case
	preeditText             string
	pendingCommit           string
	pendingDelete           int
	bsText                  string // surrounding text mode, see updatePreviousText
	madeUpKeys              bool   // the word was typed again with them, see retype
	shouldRestoreKeyStrokes bool
	outputCharset           string
}

// The C++ side applies the user options right after creation.
func newFcitxBambooEngine(inputMethod bamboo.InputMethod, dictionary map[string]bool, table *MacroTable) *FcitxBambooEngine {
	return &FcitxBambooEngine{
		preeditor:        bamboo.NewEngine(inputMethod, bamboo.EstdFlags),
		inputMethod:      inputMethod,
		flags:            bamboo.EstdFlags,
		macroTable:       table,
		dictionary:       dictionary,
		autoNonVnRestore: true,
		ddFreeStyle:      true,
		outputCharset:    "Unicode",
	}
}

const (
	FcitxShiftMask   = 1 << 0
	FcitxLockMask    = 1 << 1
	FcitxControlMask = 1 << 2
	FcitxMod1Mask    = 1 << 3
	FcitxMod4Mask    = 1 << 6

	/* The next few modifiers are used by XKB so we skip to the end.
	 * Bits 15 - 23 are currently unused. Bit 29 is used internally.
	 */

	FcitxForwardMask = 1 << 25
	FcitxIgnoredMask = FcitxForwardMask

	FcitxSuperMask = 1 << 26
	FcitxHyperMask = 1 << 27
	FcitxMetaMask  = 1 << 28
)
const (
	FcitxBackSpace = 0xff08
	FcitxSpace     = 0x020
	FcitxTab       = 0xff09
)

// capitalize: a word starting with this key starts a sentence.
func (e *FcitxBambooEngine) processKeyEvent(keyVal, state uint32, surrounding, capitalize bool) bool {
	if e.getRawKeyLen() == 0 {
		e.resetWordFlags()
		e.capitalized = capitalize
	}
	if e.isWordStartW(keyVal, state) {
		return e.typeWordStartW(keyVal, state, surrounding)
	}
	if surrounding {
		return e.bsProcessKeyEvent(keyVal, state)
	}
	return e.preeditProcessKeyEvent(keyVal, state)
}

func (e *FcitxBambooEngine) resetWordFlags() {
	e.madeUpKeys = false
	e.englishWord = false
	e.capitalized = false
}

// Restores the key strokes of the current word right away, returns whether
// there was anything to restore.
func (e *FcitxBambooEngine) restoreKeyStrokes(surrounding bool) bool {
	if e.getRawKeyLen() == 0 {
		return false
	}
	if e.madeUpKeys {
		// A word taken back restores as it was, then the keys typed after.
		var keys, text = e.getProcessedString(bamboo.EnglishMode), e.getPreeditString()
		if strings.HasPrefix(keys, e.retypedKeys) {
			text = e.retypedWord + keys[len(e.retypedKeys):]
		}
		e.preeditor.Reset()
		for _, key := range text {
			e.preeditor.ProcessKey(key, bamboo.EnglishMode)
		}
		e.madeUpKeys = false
	}
	e.shouldRestoreKeyStrokes = true
	newText, _ := e.getCommitText(0, 0)
	if surrounding {
		e.updatePreviousText(newText)
	} else {
		e.updatePreedit(newText)
	}
	return true
}

func (e *FcitxBambooEngine) preeditProcessKeyEvent(keyVal uint32, state uint32) bool {
	var rawKeyLen = e.getRawKeyLen()
	var keyRune = rune(keyVal)
	var oldText = e.getPreeditString()
	defer e.updateLastKeyWithShift(keyVal, state)

	if !e.shouldRestoreKeyStrokes {
		if !e.preeditor.CanProcessKey(keyRune) && rawKeyLen == 0 && !e.macroEnabled {
			// don't process special characters if rawKeyLen == 0,
			// workaround for Chrome's address bar and Google SpreadSheets
			return false
		}
	}

	// Ctrl+BackSpace and the like delete more than a character: they end the
	// word like other shortcuts.
	if keyVal == FcitxBackSpace && isValidState(state) {
		if e.runeCount() == 1 {
			e.commitPreeditAndReset("")
			return true
		}
		if rawKeyLen > 0 {
			e.preeditor.RemoveLastChar(true)
			e.updatePreedit(e.getPreeditString())
			return true
		} else {
			return false
		}
	}
	if keyVal == FcitxTab {
		if ok, macText := e.getMacroText(); ok {
			e.commitPreeditAndReset(macText)
		} else {
			e.commitPreeditAndReset(e.getComposedString(oldText))
			return false
		}
		return true
	}

	newText, isWordBreakRune := e.getCommitText(keyVal, state)
	isPrintableKey := e.isPrintableKey(state, keyVal)
	if isWordBreakRune {
		e.commitPreeditAndReset(newText)
		return isPrintableKey
	}
	e.updatePreedit(newText)
	return isPrintableKey
}

// The clock, for tests to fix.
var now = time.Now

func (e *FcitxBambooEngine) expandMacro(str string) string {
	var macroText = e.macroTable.GetText(str, e.autoCapitalizeMacro)
	// Before the case, which would turn the placeholders into "$date".
	if strings.Contains(macroText, "$") {
		var t = now()
		macroText = strings.NewReplacer("$DATE", t.Format("02/01/2006"), "$TIME", t.Format("15:04")).Replace(macroText)
	}
	if e.autoCapitalizeMacro {
		switch determineMacroCase(str) {
		case VnCaseAllSmall:
			macroText = strings.ToLower(macroText)
		case VnCaseAllCapital:
			macroText = strings.ToUpper(macroText)
		}
	}
	return macroText
}

func (e *FcitxBambooEngine) updatePreedit(processedStr string) {
	e.preeditText = e.encodeText(processedStr)
}

func (e *FcitxBambooEngine) getBambooInputMode() bamboo.Mode {
	if e.shouldFallbackToEnglish(false) {
		return bamboo.EnglishMode
	}
	return bamboo.VietnameseMode
}

func (e *FcitxBambooEngine) shouldFallbackToEnglish(checkVnRune bool) bool {
	if !e.autoNonVnRestore || e.madeUpKeys {
		return false
	}
	var vnSeq = e.getProcessedString(bamboo.VietnameseMode | bamboo.LowerCase)
	var vnRunes = []rune(vnSeq)
	if len(vnRunes) == 0 || e.isSpellCheckException(vnSeq, false) {
		return false
	}
	if ok, _ := e.getMacroText(); ok {
		return false
	}
	// we want to allow dd even in non-vn sequence, because dd is used a lot in abbreviation
	if e.ddFreeStyle && !bamboo.HasAnyVietnameseVower(vnSeq) &&
		(vnRunes[len(vnRunes)-1] == 'd' || strings.ContainsRune(vnSeq, 'đ')) {
		return false
	}
	if checkVnRune && !bamboo.HasAnyVietnameseRune(vnSeq) {
		return false
	}
	if _, ok := e.quickWord(false); ok {
		return false
	}
	return !e.preeditor.IsValid(false)
}

func (e *FcitxBambooEngine) mustFallbackToEnglish() bool {
	if !e.autoNonVnRestore || e.madeUpKeys {
		return false
	}
	var vnSeq = e.getProcessedString(bamboo.VietnameseMode | bamboo.LowerCase)
	var vnRunes = []rune(vnSeq)
	if len(vnRunes) == 0 || e.isSpellCheckException(vnSeq, true) {
		return false
	}
	// we want to allow dd even in non-vn sequence, because dd is used a lot in abbreviation
	if e.ddFreeStyle && strings.ContainsRune(vnSeq, 'đ') {
		return false
	}
	if _, ok := e.quickWord(true); ok {
		return false
	}
	if e.spellCheckWithDicts {
		return !e.dictionary[vnSeq]
	}
	return !e.preeditor.IsValid(true)
}

// Words the user keeps whatever the spell check says. A complete word must
// match exactly; while typing, a beginning without tones and marks does, as
// "kro" is typed before "krô".
func (e *FcitxBambooEngine) isSpellCheckException(vnSeq string, complete bool) bool {
	for _, word := range e.spellCheckExceptions {
		if word == vnSeq || !complete && strings.HasPrefix(removeDiacritics(word), removeDiacritics(vnSeq)) {
			return true
		}
	}
	return false
}

func (e *FcitxBambooEngine) getComposedString(oldText string) string {
	if quick, ok := e.quickWord(true); ok {
		return quick
	}
	if bamboo.HasAnyVietnameseRune(oldText) && e.mustFallbackToEnglish() {
		return e.getProcessedString(bamboo.EnglishMode)
	}
	return oldText
}

// What the application shows of text: in the output charset, upper case
// first when the word starts a sentence.
func (e *FcitxBambooEngine) encodeText(text string) string {
	if first, size := utf8.DecodeRuneInString(text); e.capitalized && unicode.IsLower(first) {
		text = string(unicode.ToUpper(first)) + text[size:]
	}
	return bamboo.Encode(e.outputCharset, text)
}

func (e *FcitxBambooEngine) getProcessedString(mode bamboo.Mode) string {
	return e.preeditor.GetProcessedString(mode)
}

func (e *FcitxBambooEngine) getPreeditString() string {
	if e.macroEnabled {
		var text = e.getProcessedString(bamboo.PunctuationMode)
		if quick, ok := e.quickWord(false); ok && !e.macroTable.HasPrefix(text, e.autoCapitalizeMacro) {
			return quick
		}
		return text
	}
	if e.shouldFallbackToEnglish(true) {
		return e.getProcessedString(bamboo.EnglishMode)
	}
	if quick, ok := e.quickWord(false); ok {
		return quick
	}
	return e.getProcessedString(bamboo.VietnameseMode)
}

func (e *FcitxBambooEngine) commitPreeditAndReset(s string) {
	e.commitText(s)
	e.preeditText = ""
	e.bsText = ""
	e.preeditor.Reset()
}

func (e *FcitxBambooEngine) commitText(str string) {
	e.pendingCommit += e.encodeText(str)
}

func (e *FcitxBambooEngine) takeCommitText() string {
	var text = e.pendingCommit
	e.pendingCommit = ""
	return text
}
