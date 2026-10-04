# fcitx5-bamboo-plus

Vietnamese input method for [Fcitx 5](https://fcitx-im.org), built on
[bamboo-core](https://github.com/BambooEngine/bamboo-core). It is
[fcitx5-bamboo](https://github.com/fcitx/fcitx5-bamboo) with the typing
experience of [ibus-bamboo](https://github.com/BambooEngine/ibus-bamboo),
typing without underline included, and features of UniKey and OpenKey.

It installs as the input method *Bamboo*, like fcitx5-bamboo, which it
replaces.

## Features

All of fcitx5-bamboo: Telex, VNI, VIQR, Microsoft layout, Telex 2, Telex W and
their combinations, custom keymaps, spell check, macros and legacy output
charsets (TCVN3, VNI Windows, VISCII…).

### Typing modes, from ibus-bamboo

Each application remembers its mode. Press `~` to choose the mode of the
current application (press it twice to type `~`):

1. **Preedit**: the word being typed is shown as preedit.
2. **Surrounding Text** ("sửa lỗi gạch chân"): the word is never underlined.
   - Where the application reports the text around the cursor (GTK
     applications, Firefox, Wayland applications) the word is typed and fixed
     in place. When that text is not what was typed, a new word starts instead
     of deleting text.
   - An address bar's suggestion, selected after the cursor, is replaced as
     you type and taken by Return.
   - Qt applications show the word as preedit without underline, since
     fcitx5-qt reports its text now and then only.
   - Elsewhere the word shows in Fcitx's window: Chromium, Electron and CEF
     applications on XWayland, which underline any preedit, and terminals.
     The option "Surrounding Text in Wayland apps without text: edit with
     BackSpace", off by default, types the word into Wayland terminals on
     KWin instead, fixing it with BackSpace keys.
3. **Plain Preedit**: the word as preedit without underline, for
   applications that draw it as told (Qt and GTK applications).
4. **Input Method Window**: the word shows in Fcitx's window, the
   application gets whole words. It suits any application.
5. **BackSpace**: the word is typed into the application and fixed with
   BackSpace keys, as typed by hand, never waiting for the application to
   report its text. The keys must reach the application in order with the
   text: on KWin, Chromium-based applications and terminals such as
   Alacritty keep that order, Qt and GTK applications on Wayland text input
   do not. It suits Chrome on a loaded machine, where Surrounding Text waits
   for Chrome's late reports; its address bar, which shows its suggestion in
   its report only, gets Surrounding Text in this mode and in Input Method
   Window where that edits its text in place. Through fcitx5-qt it is for
   terminals: Konsole, also in Yakuake and the terminal panels of Dolphin and
   Kate, which reports no text and asks for neither capitals nor
   predictions, and those flagged as terminals in the modes per application.
   DEL characters go with the text, and the terminal needs
   `FCITX_QT_USE_SYNC=1` in its environment, as in
   `Exec=env FCITX_QT_USE_SYNC=1 konsole` in a copy of its desktop file, or
   fcitx5-qt hands it other keys late, a word typed right after Return
   possibly before it. A Qt field asking for neither capitals nor
   predictions, such as a user name, counts as a terminal until such a
   field of its window reports its text, which Qt Widgets do on a click:
   before that, after a window switch, the first letter typed there goes
   in alone. Other Qt applications get plain preedit, the rest the word in
   Fcitx's window.
6. **Exclude**: no Vietnamese in this application.

The default mode, the modes per application and the key are in the
configuration, with modes per kind of application for those without their
own, by how they reach Fcitx: Qt applications through fcitx5-qt
(`QT_IM_MODULE=fcitx`), Qt terminals among them (Konsole), applications
through fcitx5-gtk (`GTK_IM_MODULE=fcitx`: GTK applications, Chromium and
Electron on X11), Wayland applications through KWin (Chromium and Electron on
Wayland, Alacritty, Qt and GTK applications without Fcitx's modules) and X11
applications through XIM. Others, through IBus or the input method of other
compositors, take the default mode. The label of the input method tells what
the mode does where it does something else, as "VNI (Surrounding Text →
Input Method Window)" in Alacritty. An application whose late reports keep
keys waiting, five in a minute, gets a notification, once, suggesting the
modes that do not wait: BackSpace for Chromium-based applications, Input
Method Window for any.

### More, from UniKey, OpenKey and others

- Email, number and phone fields get the keys untouched. The input method
  label is VI, or EN when keys go straight to the application.
- Password fields get the keys untouched, as Fcitx gives them. Qt Quick
  reports its own (the lock screen, polkit and Wi-Fi dialogs) as sensitive
  only, which Fcitx leaves to the input method, and shows the word typed
  there unmasked: with `QT_IM_MODULE=fcitx` they get the keys untouched too.
  Through Wayland text input, IBus or XIM they cannot be told from other
  fields (Chrome marks every field of its incognito windows sensitive).
  Fcitx's "Allow input method in the password field" brings Vietnamese to
  password fields, the word masked as Fcitx masks it. Terminals cannot tell
  a password prompt: switch to English for `sudo`.
- In terminals and code editors, Escape switches to English as vim needs:
  applications reporting themselves as terminals, or flagged so in the modes
  per application.
- Words kept by spell check, for proper nouns like Krông.
- After BackSpace, arrows or a click, a key right after a word edits it like
  one being typed: "viêt", BackSpace over the space, then `j` gives "việt".
  It needs the application to report its text (GTK applications through
  fcitx5-gtk), and is left out on Wayland frontends.
- Quick Typing, off by default: what a lone `w` types (ư, or w at word start
  like UniKey with "Process W at word begin" off) and OpenKey's quick
  consonants: cc→ch gg→gi kk→kh nn→ng pp→ph qq→qu tt→th, f→ph j→gi w→qu at
  word start, g→ng h→nh k→ch at word end. They apply only when the keys typed
  make no Vietnamese word, yet English words such as "bag" still become
  "bang".
- Capitalize the first letter of sentences, off by default.
- `Control+Shift+F6` converts the selection, else the word before the cursor,
  else the primary selection, like UniKey's toolkit: typed again with the
  input method (for text typed with it off), without diacritics, upper or
  lower case, capitalized words, or from a legacy charset. Qt applications
  take no deletion while a key is handled: there the word before the cursor
  is left out.

Fcitx 5 has the rest of ibus-bamboo: Unicode input with `Control+Shift+U`,
emoji and character search with `Control+Alt+Shift+U`, and its trigger key to
switch to English.

## Install

### Arch Linux

```sh
git clone https://github.com/googlesky/fcitx5-bamboo-plus.git
cd fcitx5-bamboo-plus/packaging/arch
makepkg -si
```

The package `fcitx5-bamboo-plus-git` builds the latest commit, runs the tests
and replaces `fcitx5-bamboo`. Log out and back in to use it. Run `makepkg -si`
again to update; it writes the version into the PKGBUILD, so run
`git checkout PKGBUILD && git pull` first.

### From source

Requires Fcitx 5.1.23 or newer with its development files, Extra CMake
Modules, CMake, gettext and Go.

```sh
git clone --recursive https://github.com/googlesky/fcitx5-bamboo-plus.git
cd fcitx5-bamboo-plus
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

## Setup

Add *Bamboo* to the input methods in Fcitx's configuration
(`fcitx5-configtool`), its options are behind the configure button there.
Settings saved by fcitx5-bamboo are kept; otherwise the defaults follow
ibus-bamboo: macros, dictionary spell check and underline are off.

On KDE Plasma Wayland, choose Fcitx 5 as the virtual keyboard in System
Settings, and run Chromium and Electron applications on Wayland
(`--ozone-platform=wayland --enable-wayland-ime`) to type in them without
underline.

## Development

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The Fcitx addon is C++ (`src/`), the engine Go (`bamboo/`, built as a C
archive) around bamboo-core, a submodule kept as upstream has it. The tests
type through Fcitx's test frontend (`test/testbamboo.cpp`) and test the
engine (`bamboo/engine_test.go`). Format with `clang-format` and `gofmt`.

Changes of fcitx5-bamboo come in from its main branch:

```sh
git remote add upstream https://github.com/fcitx/fcitx5-bamboo.git
git fetch upstream
git merge upstream/main
```

Report problems in the
[issues](https://github.com/googlesky/fcitx5-bamboo-plus/issues).

## License

LGPL-2.1-or-later, see [LICENSES](LICENSES). Based on fcitx5-bamboo by the
Fcitx team and on bamboo-core (MIT) by Luong Thanh Lam.
