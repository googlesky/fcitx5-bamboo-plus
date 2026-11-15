/*
 * SPDX-FileCopyrightText: 2022-2022 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */
package main

import (
	/*
		#include <stdint.h>
		#include <stdbool.h>
		typedef const char cchar;
		typedef struct {
			bool autoNonVnRestore;
			bool ddFreeStyle;
			bool macroEnabled;
			bool autoCapitalizeMacro;
			bool spellCheckWithDicts;
			const char *outputCharset;
			bool modernStyle;
			bool freeMarking;
			char **spellCheckExceptions;
			int standaloneW;
			bool quickDouble;
			bool quickStart;
			bool quickEnd;
		} FcitxBambooEngineOption;
	*/
	"C"
	"bamboo-core"
	"os/signal"
	"runtime/cgo"
	"syscall"
	"unsafe"
)
import (
	"bufio"
	"log"
	"os"
	"sort"
	"strings"
)

// A panic escaping a cgo call would take the whole fcitx5 process down.
func recoverPanic(where string) {
	if r := recover(); r != nil {
		log.Printf("bamboo: recovered from panic in %s: %v", where, r)
	}
}

//export Init
func Init() {
	signal.Ignore(syscall.SIGPIPE)
}

// capitalize: a word starting with this key starts a sentence.
//
//export EngineProcessKeyEvent
func EngineProcessKeyEvent(engine uintptr, keyVal, state uint32, surrounding, capitalize bool) bool {
	defer recoverPanic("EngineProcessKeyEvent")
	bambooEngine, ok := cgo.Handle(engine).Value().(*FcitxBambooEngine)
	if !ok {
		return false
	}
	return bambooEngine.processKeyEvent(keyVal, state, surrounding, capitalize)
}

// Before the key: takes the word right before the cursor back into the
// engine when the key edits it, see editWordBeforeCursor.
//
//export EngineEditWord
func EngineEditWord(engine uintptr, before *C.cchar, keyVal, state uint32, surrounding bool) {
	defer recoverPanic("EngineEditWord")
	if bambooEngine, ok := cgo.Handle(engine).Value().(*FcitxBambooEngine); ok {
		bambooEngine.editWordBeforeCursor(C.GoString(before), keyVal, state, surrounding)
	}
}

// Whether the key belongs to the word being typed (VIQR's '~' tone key), so
// that a hotkey on the same key does not steal it mid-word.
//
//export EngineIsTypingKey
func EngineIsTypingKey(engine uintptr, keyVal, state uint32) bool {
	defer recoverPanic("EngineIsTypingKey")
	bambooEngine, ok := cgo.Handle(engine).Value().(*FcitxBambooEngine)
	if !ok {
		return false
	}
	return bambooEngine.getRawKeyLen() > 0 && isValidState(state) && bambooEngine.preeditor.CanProcessKey(rune(keyVal))
}

// The word being typed in surrounding text mode as the application should
// show it, for the C++ side to check before editing.
//
//export EngineSurroundingWord
func EngineSurroundingWord(engine uintptr) *C.char {
	defer recoverPanic("EngineSurroundingWord")
	bambooEngine, ok := cgo.Handle(engine).Value().(*FcitxBambooEngine)
	if !ok {
		return nil
	}
	return C.CString(bambooEngine.encodeText(bambooEngine.bsText))
}

// Restores the key strokes of the current word right away, returns whether
// there was anything to restore.
//
//export EngineRestoreKeyStrokes
func EngineRestoreKeyStrokes(engine uintptr, surrounding bool) bool {
	defer recoverPanic("EngineRestoreKeyStrokes")
	bambooEngine, ok := cgo.Handle(engine).Value().(*FcitxBambooEngine)
	return ok && bambooEngine.restoreKeyStrokes(surrounding)
}

//export EnginePullPreedit
func EnginePullPreedit(engine uintptr) *C.char {
	defer recoverPanic("EnginePullPreedit")
	bambooEngine, ok := cgo.Handle(engine).Value().(*FcitxBambooEngine)
	if !ok {
		return nil
	}
	return C.CString(bambooEngine.preeditText)
}

// Without a preedit (surrounding text mode) the word is already in the
// application, committing it again would duplicate it.
//
//export EngineCommitPreedit
func EngineCommitPreedit(engine uintptr) {
	defer recoverPanic("EngineCommitPreedit")
	bambooEngine, ok := cgo.Handle(engine).Value().(*FcitxBambooEngine)
	if !ok {
		return
	}
	if bambooEngine.preeditText == "" {
		bambooEngine.commitPreeditAndReset("")
		return
	}
	bambooEngine.commitPreeditAndReset(bambooEngine.getPreeditString())
}

//export EnginePullCommit
func EnginePullCommit(engine uintptr) *C.char {
	defer recoverPanic("EnginePullCommit")
	bambooEngine, ok := cgo.Handle(engine).Value().(*FcitxBambooEngine)
	if !ok {
		return nil
	}
	return C.CString(bambooEngine.takeCommitText())
}

// Number of characters before the cursor to delete before committing.
//
//export EnginePullDeleteCount
func EnginePullDeleteCount(engine uintptr) int32 {
	defer recoverPanic("EnginePullDeleteCount")
	bambooEngine, ok := cgo.Handle(engine).Value().(*FcitxBambooEngine)
	if !ok {
		return 0
	}
	return int32(bambooEngine.takeDeleteCount())
}

//export EngineSetOption
func EngineSetOption(engine uintptr, option *C.FcitxBambooEngineOption) {
	defer recoverPanic("EngineSetOption")
	bambooEngine, ok := cgo.Handle(engine).Value().(*FcitxBambooEngine)
	if !ok {
		return
	}
	bambooEngine.autoNonVnRestore = bool(option.autoNonVnRestore)
	bambooEngine.ddFreeStyle = bool(option.ddFreeStyle)
	bambooEngine.macroEnabled = bool(option.macroEnabled)
	bambooEngine.autoCapitalizeMacro = bool(option.autoCapitalizeMacro)
	bambooEngine.spellCheckWithDicts = bool(option.spellCheckWithDicts)
	bambooEngine.outputCharset = C.GoString(option.outputCharset)
	bambooEngine.spellCheckExceptions = nil
	if option.spellCheckExceptions != nil {
		words := (*[1<<20 - 1]*C.char)(unsafe.Pointer(option.spellCheckExceptions))
		for i := 0; words[i] != nil; i++ {
			if word := strings.ToLower(strings.TrimSpace(C.GoString(words[i]))); word != "" {
				bambooEngine.spellCheckExceptions = append(bambooEngine.spellCheckExceptions, word)
			}
		}
	}
	flags := bamboo.EstdFlags
	if option.modernStyle {
		flags &= ^bamboo.EstdToneStyle
	} else {
		flags |= bamboo.EstdToneStyle
	}

	if option.freeMarking {
		flags |= bamboo.EfreeToneMarking
	} else {
		flags &= ^bamboo.EfreeToneMarking
	}
	bambooEngine.flags = flags
	bambooEngine.setStandaloneW(int(option.standaloneW), flags)
	bambooEngine.preeditor.SetFlag(flags)
	bambooEngine.quickDouble = bool(option.quickDouble)
	bambooEngine.quickStart = bool(option.quickStart)
	bambooEngine.quickEnd = bool(option.quickEnd)
}

//export NewEngine
func NewEngine(name *C.cchar, dictHandle uintptr, tableHandle uintptr) uintptr {
	defer recoverPanic("NewEngine")
	dict, ok := cgo.Handle(dictHandle).Value().(*map[string]bool)
	if !ok {
		return 0
	}

	table, ok := cgo.Handle(tableHandle).Value().(*MacroTable)
	if !ok {
		return 0
	}

	imName := C.GoString(name)
	var inputMethod = bamboo.ParseInputMethod(bamboo.InputMethodDefinitions, imName)
	return uintptr(cgo.NewHandle(newFcitxBambooEngine(inputMethod, *dict, table)))
}

// A malformed custom keymap entry can make bamboo-core panic, 0 is returned.
//
//export NewCustomEngine
func NewCustomEngine(definition **C.char, dictHandle uintptr, tableHandle uintptr) uintptr {
	defer recoverPanic("NewCustomEngine")
	dict, ok := cgo.Handle(dictHandle).Value().(*map[string]bool)
	if !ok {
		return 0
	}

	table, ok := cgo.Handle(tableHandle).Value().(*MacroTable)
	if !ok {
		return 0
	}
	var definitions = map[string]bamboo.InputMethodDefinition{
		"Custom": map[string]string{},
	}
	def := (*[1<<20 - 1]*C.char)(unsafe.Pointer(definition))

	i := 0
	for def[i] != nil {
		addCustomKey(definitions["Custom"], C.GoString(def[i]), C.GoString(def[i+1]))
		i += 2
	}

	var inputMethod = bamboo.ParseInputMethod(definitions, "Custom")
	return uintptr(cgo.NewHandle(newFcitxBambooEngine(inputMethod, *dict, table)))
}

func addCustomKey(def bamboo.InputMethodDefinition, key, value string) {
	if key = strings.ToLower(strings.TrimSpace(key)); key != "" {
		def[key] = value
	}
}

//export NewMacroTable
func NewMacroTable(definition **C.char) uintptr {
	defer recoverPanic("NewMacroTable")
	var entries [][2]string
	def := (*[1<<20 - 1]*C.char)(unsafe.Pointer(definition))
	for i := 0; def[i] != nil; i += 2 {
		entries = append(entries, [2]string{C.GoString(def[i]), C.GoString(def[i+1])})
	}

	return uintptr(cgo.NewHandle(newMacroTable(entries)))
}

//export DeleteObject
func DeleteObject(handle uintptr) {
	defer recoverPanic("DeleteObject")
	cgo.Handle(handle).Delete()
}

//export ResetEngine
func ResetEngine(engine uintptr) {
	defer recoverPanic("ResetEngine")
	bambooEngine, ok := cgo.Handle(engine).Value().(*FcitxBambooEngine)
	if !ok {
		return
	}
	bambooEngine.commitPreeditAndReset("")
}

func toCStringArray(strs []string) **C.char {

	array := C.malloc(C.size_t(len(strs)+1) * C.size_t(unsafe.Sizeof(uintptr(0))))
	// convert the C array to a Go Array so we can index it
	a := (*[1<<20 - 1]*C.char)(array)

	for idx, substring := range strs {
		a[idx] = C.CString(substring)
	}
	a[len(strs)] = nil
	return (**C.char)(array)
}

//export GetCharsetNames
func GetCharsetNames() **C.char {
	var names = bamboo.GetCharsetNames()
	sort.Strings(names[1:]) // Unicode stays first
	return toCStringArray(names)
}

//export GetInputMethodNames
func GetInputMethodNames() **C.char {
	var names []string
	for imName := range bamboo.InputMethodDefinitions {
		names = append(names, imName)
	}
	sort.Strings(names)
	return toCStringArray(names)
}

// Kind, result, kind, result... of UniKey toolkit's conversions of text,
// see textTransforms.
//
//export EngineTextTransforms
func EngineTextTransforms(engine uintptr, text *C.cchar) **C.char {
	defer recoverPanic("EngineTextTransforms")
	var list []string
	if bambooEngine, ok := cgo.Handle(engine).Value().(*FcitxBambooEngine); ok {
		list = bambooEngine.textTransforms(C.GoString(text))
	}
	return toCStringArray(list)
}

//export NewDictionary
func NewDictionary(fd uintptr) uintptr {
	var data = map[string]bool{}
	f := os.NewFile(fd, "dict")
	defer f.Close()
	rd := bufio.NewReader(f)
	for {
		line, _, err := rd.ReadLine()
		if err != nil {
			break
		}
		if len(line) == 0 {
			continue
		}
		var tmp = []byte(strings.ToLower(string(line)))
		data[string(tmp)] = true
	}
	return uintptr(cgo.NewHandle(&data))
}

func main() {}
