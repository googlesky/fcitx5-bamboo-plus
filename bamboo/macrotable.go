/*
 * SPDX-FileCopyrightText: 2022-2022 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */
package main

import "strings"

// Like ibus-bamboo, keys are case-insensitive only when auto capitalization is
// on, so both spellings are kept.
type MacroTable struct {
	mTable map[string]string
	lTable map[string]string
}

// Blank keys and values are dropped: a blank key would match an empty word.
func newMacroTable(entries [][2]string) *MacroTable {
	var table = &MacroTable{mTable: map[string]string{}, lTable: map[string]string{}}
	for _, entry := range entries {
		var key = strings.TrimSpace(entry[0])
		if key == "" || entry[1] == "" || strings.ContainsAny(key, " \t\n") {
			continue
		}
		table.mTable[key] = entry[1]
		table.lTable[strings.ToLower(key)] = entry[1]
	}
	return table
}

func (e *MacroTable) HasKey(key string, ignoreCase bool) bool {
	return e.GetText(key, ignoreCase) != ""
}

func (e *MacroTable) GetText(key string, ignoreCase bool) string {
	if text := e.mTable[key]; text != "" || !ignoreCase {
		return text
	}
	return e.lTable[strings.ToLower(key)]
}

func (e *MacroTable) HasPrefix(key string, ignoreCase bool) bool {
	var table = e.mTable
	if ignoreCase {
		table, key = e.lTable, strings.ToLower(key)
	}
	for k := range table {
		if strings.HasPrefix(k, key) {
			return true
		}
	}
	return false
}
