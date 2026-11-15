package main

import (
	"bamboo-core"
	"testing"
)

// What NewCustomEngine does with its C array, minus cgo.
func customEngineForTest(def map[string]string) *FcitxBambooEngine {
	var flat []string
	for k, v := range def {
		flat = append(flat, k, v)
	}
	var defs = map[string]bamboo.InputMethodDefinition{"Custom": map[string]string{}}
	for i := 0; i < len(flat); i += 2 {
		addCustomKey(defs["Custom"], flat[i], flat[i+1])
	}
	return newFcitxBambooEngine(bamboo.ParseInputMethod(defs, "Custom"), map[string]bool{}, newMacroTable(nil))
}

// a macro key with an inner space swallows the text typed before it.
func TestMacroKeyWithSpace(t *testing.T) {
	for _, surrounding := range []bool{false, true} {
		a := newTestApp("Telex", [][2]string{{"a b", "A B"}}, surrounding)
		a.typeString("a x ")
		if got := string(a.text) + a.e.preeditText; got != "a x " {
			t.Errorf("surrounding %v: %q, want %q", surrounding, got, "a x ")
		}
	}
}

// the restore key does not stick: a later tone key makes the word
// Vietnamese again.
func TestRestoreKeySticks(t *testing.T) {
	for _, surrounding := range []bool{false, true} {
		for _, spell := range []bool{true, false} {
			for _, c := range []struct{ before, after, want string }{
				{"doo", "r ", "door "},
				{"xoong", "s ", "xoongs "},
				{"vieetj", "s ", "vieetjs "},
			} {
				a := newTestApp("Telex", nil, surrounding)
				a.e.autoNonVnRestore = spell
				a.typeString(c.before)
				a.restoreKeyStrokes()
				a.typeString(c.after)
				if got := string(a.text); got != c.want {
					t.Errorf("surrounding %v spell %v %s+restore+%q: %q, want %q", surrounding, spell, c.before, c.after, got, c.want)
				}
			}
		}
	}
}

// custom keymap keys are used as written: an upper case key never
// fires, a leading space makes Space a typing key.
func TestCustomKeymapKeys(t *testing.T) {
	for _, key := range []string{"D", " d"} {
		a := &testApp{e: customEngineForTest(map[string]string{"a": "A_Â", key: "D_Đ"})}
		a.typeString("dda xin")
		if got := string(a.text) + a.e.preeditText; got != "đa xin" {
			t.Errorf("key %q: %q, want %q", key, got, "đa xin")
		}
	}
}
