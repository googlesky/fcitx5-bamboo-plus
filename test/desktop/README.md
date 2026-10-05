# Typing into real applications

`desktop_test.py` types VNI into Chrome, Chrome's address bar, a GTK entry,
Qt Quick fields, a Qt Widgets field, its convert key and sentence
capitalization, and a terminal application in Alacritty and in Konsole,
through a nested KWin with fcitx5 as its input method, and checks the text
they get. Keys come fast and overlap, like a person rolling them: 40 down to
5 ms apart.

It is what found these bugs, which the unit tests model where they can:

- Chrome reports its text late, typing fast, and deletes around the text it
  reported last: "nguòi7" for "người", "baiài" for "bài".
- Chrome's address bar drops deletions around its inline suggestion.
- Chrome reports a selection before the cursor: typed over, the first key
  went in with BackSpace keys and the word ended there, "d9i" gave "d9i".
- Alacritty sends a commit longer than a character as a bracketed paste, which
  Claude Code drops and opencode reorders when more keys follow.
- Qt Quick reports password fields as sensitive only, so fcitx5 leaves them
  to the input method, and shows their preedit unmasked: the password showed
  as it was typed, and changed where it read as Vietnamese.
- fcitx5-qt hands the keys the input method lets through to the application
  later than our commits: typed fast into Konsole, "tôi" came out "t ôi".
- fcitx5-qt drops the surrounding text capability before every key: Qt
  fields in the BackSpace mode were taken for terminals, DEL characters typed
  into them.
- A modifier pressed alone, the convert key's Control, marked the
  application's text stale: fcitx5-qt reports none then, and the convert
  key took the primary selection instead of the field's.

Nothing reaches the desktop: KWin renders to a virtual output and runs on a
D-Bus of its own, fcitx5 gets a configuration of its own, Chrome a profile of
its own and no host but localhost.

## Running

Needs `kwin_wayland`, `fcitx5`, `wayland-scanner`, a C compiler, `uv`, and the
applications tested: `google-chrome-stable`, `zenity`, `qml6` and `kdialog`
with fcitx5-qt, `alacritty`, `konsole` and `tmux`.

```sh
# All tests, with the libbamboo.so of a build:
uv run --with websocket-client test/desktop/desktop_test.py --addon-dir build/src
# Some of them, with the installed addon, keeping the logs:
uv run --with websocket-client test/desktop/desktop_test.py --keep chrome omnibox
```

`fakekeys.c` sends the keys through KWin's `org_kde_kwin_fake_input`, which
the nested KWin allows with `KWIN_WAYLAND_NO_PERMISSION_CHECKS=1`.
`fake-input.xml` comes from plasma-wayland-protocols.
