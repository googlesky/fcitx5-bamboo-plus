#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 googlesky
#
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Types VNI into real applications through a nested KWin and fcitx5 with
the bamboo addon, as fast as a person rolling keys, and checks their text.

Nothing reaches the desktop: KWin renders to a virtual output and runs on a
D-Bus of its own, fcitx5 gets a configuration of its own, Chrome resolves no
host but localhost. See README.md.
"""

import argparse
import ast
import contextlib
import functools
import http.server
import json
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))

# Milliseconds between key presses, key hold, jitter of both.
SPEEDS = [(40, 70, 10), (20, 55, 8), (10, 45, 5), (5, 30, 3)]

CODES = {c: k for c, k in zip("1234567890", range(2, 12))}
CODES.update({c: k for c, k in zip("qwertyuiop", range(16, 26))})
CODES.update({c: k for c, k in zip("asdfghjkl", range(30, 39))})
CODES.update({c: k for c, k in zip("zxcvbnm", range(44, 51))})
CODES[" "] = 57
SHIFT, CTRL, L, RETURN, TAB = 42, 29, 38, 28, 15
END, LEFT, F6 = 107, 105, 64

FCITX_PROFILE = """[Groups/0]
Name=Default
Default Layout=us
DefaultIM=bamboo

[Groups/0/Items/0]
Name=keyboard-us
Layout=

[Groups/0/Items/1]
Name=bamboo
Layout=

[GroupOrder]
0=Default
"""
# Shift taps switch to English by default, the first key typed is one.
FCITX_CONFIG = "[Hotkey]\nAltTriggerKeys=\n\n[Behavior]\nActiveByDefault=True\n"
BAMBOO_CONFIG = """InputMethod=VNI
DefaultInputMode="Surrounding Text"
WaylandBackSpace=True
"""
APP_MODES = {"konsole": "BackSpace"}

# Logs the input of a terminal application that set the modes Claude Code
# sets, bracketed paste among them.
RAWLOG = r"""
import os, sys, termios, time, tty
fd = sys.stdin.fileno()
old = termios.tcgetattr(fd)
tty.setraw(fd)
os.write(1, b"\x1b[>5u\x1b[>4;2m\x1b[?2004h\x1b[?1004h")
with open(sys.argv[1], "ab", buffering=0) as log:
    try:
        while True:
            data = os.read(fd, 4096)
            if not data or b"\x03" in data:
                break
            log.write(repr(data).encode() + b"\n")
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
"""

# A name and a password in Qt Quick: what the password field shows as
# preedit, and both texts on Return there.
QML_LOGIN = """import QtQuick
import QtQuick.Controls
ApplicationWindow {
    visible: true
    Column {
        TextField {
            id: name
            KeyNavigation.tab: password
            Component.onCompleted: forceActiveFocus()
        }
        TextField {
            id: password
            echoMode: TextInput.Password
            onPreeditTextChanged: if (preeditText) console.warn("PREEDIT[" + preeditText + "]")
            onAccepted: { console.warn("TEXT[" + name.text + "|" + text + "]"); Qt.quit() }
        }
    }
}
"""


def key_events(text, gap, hold, jitter, rng):
    """fakekeys arguments typing text: presses gap ms apart, held hold ms
    so they overlap, a key up before it goes down again as on a keyboard. A
    lone Shift first: KWin activates the input method on the first key."""
    presses, at = [], 0
    for c in text:
        presses.append((at, CODES[c], hold + rng.randint(-jitter, jitter)))
        at += max(1, gap + rng.randint(-jitter, jitter))
    events = []
    for i, (at, code, held) in enumerate(presses):
        up = at + max(5, held)
        again = [p for p, k, _ in presses[i + 1 :] if k == code]
        if again:
            up = min(up, again[0] - 1)
        events += [(at, 0, "d", code), (max(at + 1, up), 1, "u", code)]
    events.sort()
    now, out = 0, [f"d{SHIFT}", "w30", f"u{SHIFT}", "w200"]
    for at, _, kind, code in events:
        if at > now:
            out.append(f"w{at - now}")
            now = at
        out.append(f"{kind}{code}")
    return out


class Session:
    """A private D-Bus, KWin on a virtual output, fcitx5 its input method."""

    def __init__(self, work, addon_dir, fcitx_extra="", bamboo_extra=""):
        self.work = work
        self.addon_dir = addon_dir
        self.processes = []
        self.bus_pid = None
        self.app_modes = dict(APP_MODES)
        self.fcitx_extra = fcitx_extra
        self.bamboo_extra = bamboo_extra

    def __enter__(self):
        os.makedirs(self.work)
        config = os.path.join(self.work, "config", "fcitx5")
        os.makedirs(os.path.join(config, "conf"), exist_ok=True)
        for name, text in [
            ("profile", FCITX_PROFILE),
            ("config", FCITX_CONFIG + self.fcitx_extra),
            ("conf/bamboo.conf", BAMBOO_CONFIG + self.bamboo_extra),
        ]:
            with open(os.path.join(config, name), "w") as f:
                f.write(text)
        self.write_app_modes()
        self.fakekeys = self.build_fakekeys()
        self.env = dict(os.environ)
        for name in ["DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "XAUTHORITY"]:
            self.env.pop(name, None)
        for name in ["CONFIG", "DATA", "CACHE", "STATE"]:
            self.env[f"XDG_{name}_HOME"] = os.path.join(self.work, name.lower())
        self.env.update(
            KWIN_WAYLAND_NO_PERMISSION_CHECKS="1",
            GTK_IM_MODULE="fcitx",
            QT_IM_MODULE="fcitx",
            XMODIFIERS="@im=fcitx",
        )
        if self.addon_dir:
            self.env["FCITX_ADDON_DIRS"] = f"{os.path.abspath(self.addon_dir)}:/usr/lib/fcitx5"
        bus = subprocess.run(
            ["dbus-daemon", "--session", "--fork", "--print-address=1", "--print-pid=1"],
            capture_output=True, text=True, check=True, env=self.env,
        ).stdout.split("\n")
        self.env["DBUS_SESSION_BUS_ADDRESS"], self.bus_pid = bus[0], int(bus[1])
        wrapper = os.path.join(self.work, "fcitx5.sh")
        with open(wrapper, "w") as f:
            log = os.path.join(self.work, "fcitx5.log")
            f.write(f"#!/bin/sh\nexec fcitx5 --verbose='*=4,bamboo=5' > '{log}' 2>&1\n")
        os.chmod(wrapper, 0o755)
        socket = f"wl-{os.path.basename(os.path.dirname(self.work))}-{os.path.basename(self.work)}"
        kwin = self.spawn(["kwin_wayland", "--virtual", "--no-lockscreen", "--socket", socket,
                           "--width", "1280", "--height", "900", "--inputmethod", wrapper], "kwin")
        socket_path = os.path.join(os.environ["XDG_RUNTIME_DIR"], socket)
        for _ in range(80):
            if os.path.exists(socket_path) or kwin.poll() is not None:
                break
            time.sleep(0.25)
        if kwin.poll() is not None or not os.path.exists(socket_path):
            raise RuntimeError("KWin did not start, see kwin.log")
        self.env["WAYLAND_DISPLAY"] = socket
        self.wait_fcitx()
        time.sleep(1)
        return self

    def __exit__(self, *exc):
        for process in reversed(self.processes):
            with contextlib.suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGTERM)
        for process in self.processes:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        if self.bus_pid:
            with contextlib.suppress(ProcessLookupError):
                os.kill(self.bus_pid, signal.SIGTERM)
        # The bus starts services, portals, ksecretd, kdeconnectd, which
        # outlive it: whatever runs with this session's environment goes.
        marker = f"XDG_DATA_HOME={self.env['XDG_DATA_HOME']}\0".encode()
        for sig in [signal.SIGTERM, signal.SIGKILL]:
            left = False
            for entry in os.listdir("/proc"):
                if not entry.isdigit() or int(entry) == os.getpid():
                    continue
                with contextlib.suppress(OSError):
                    with open(f"/proc/{entry}/environ", "rb") as f:
                        if marker in f.read():
                            os.kill(int(entry), sig)
                            left = True
            if not left:
                break
            time.sleep(2)

    def build_fakekeys(self):
        out = os.path.join(self.work, "fakekeys")
        xml = os.path.join(HERE, "fake-input.xml")
        header = os.path.join(self.work, "fake-input-client-protocol.h")
        code = os.path.join(self.work, "fake-input-protocol.c")
        subprocess.run(["wayland-scanner", "client-header", xml, header], check=True)
        subprocess.run(["wayland-scanner", "private-code", xml, code], check=True)
        flags = subprocess.run(["pkg-config", "--cflags", "--libs", "wayland-client"],
                               capture_output=True, text=True, check=True).stdout.split()
        subprocess.run(["cc", "-O1", "-I", self.work, "-o", out,
                        os.path.join(HERE, "fakekeys.c"), code, *flags], check=True)
        return out

    def spawn(self, argv, name, stdin=subprocess.DEVNULL):
        log = open(os.path.join(self.work, f"{name}.log"), "w")
        process = subprocess.Popen(argv, env=self.env, stdout=log, stderr=subprocess.STDOUT,
                                   stdin=stdin, start_new_session=True)
        self.processes.append(process)
        return process

    def keys(self, args):
        subprocess.run([self.fakekeys, *args], env=self.env, check=True)

    def write_app_modes(self):
        path = os.path.join(self.work, "config", "fcitx5", "conf", "bamboo-app-mode.conf")
        with open(path, "w") as f:
            for i, (program, mode) in enumerate(self.app_modes.items()):
                f.write(f'[AppMode/{i}]\nProgram={program}\nMode="{mode}"\n\n')

    def add_app_mode(self, program, mode):
        """Gives program a typing mode, read again by fcitx5."""
        self.app_modes[program] = mode
        self.write_app_modes()
        subprocess.run(["dbus-send", "--session", "--print-reply", "--dest=org.fcitx.Fcitx5",
                        "/controller", "org.fcitx.Fcitx.Controller1.ReloadAddonConfig",
                        "string:bamboo"], env=self.env, check=True, capture_output=True)

    def wait_fcitx(self, timeout=60):
        """Waits until KWin's fcitx5 has its name on the bus: a call to it
        before would have the bus start another, without the display."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            reply = subprocess.run(
                ["dbus-send", "--session", "--print-reply", "--dest=org.freedesktop.DBus",
                 "/org/freedesktop/DBus", "org.freedesktop.DBus.NameHasOwner",
                 "string:org.fcitx.Fcitx5"], env=self.env, capture_output=True, text=True).stdout
            if "boolean true" in reply:
                return
            time.sleep(0.2)
        raise RuntimeError("fcitx5 did not start, see fcitx5.log")

    def log_mark(self):
        """Where the fcitx5 log ends now, for methods."""
        return os.path.getsize(os.path.join(self.work, "fcitx5.log"))

    def methods(self, program, since):
        """The ways bamboo typed the keys of program since the mark, as
        BambooState::Method numbers: whether its typing mode applied."""
        with open(os.path.join(self.work, "fcitx5.log"), errors="replace") as f:
            f.seek(since)
            return set(int(m) for m in re.findall(
                rf" program {re.escape(program)} frontend \S+ caps 0x[0-9a-f]+ method (\d)",
                f.read()))

    def wait_focus(self, program, timeout=30):
        """Waits until fcitx5 has the focused input context of program: on a
        loaded machine applications take seconds to start."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            info = subprocess.run(
                ["dbus-send", "--session", "--print-reply", "--dest=org.fcitx.Fcitx5",
                 "/controller", "org.fcitx.Fcitx.Controller1.DebugInfo"],
                env=self.env, capture_output=True, text=True).stdout
            if any(f"program:{program} " in line and "focus:1" in line
                   for line in info.splitlines()):
                return
            time.sleep(0.2)
        raise RuntimeError(f"{program} got no focus")


class Chrome:
    """Chrome in the session, driven through the DevTools protocol."""

    PORT = 9333

    def __init__(self, session):
        import websocket  # uv run --with websocket-client

        profile = os.path.join(session.work, "chrome")
        session.spawn(["google-chrome-stable", f"--user-data-dir={profile}", "--no-first-run",
                       "--no-default-browser-check", "--disable-sync", "--password-store=basic",
                       "--ozone-platform=wayland", "--enable-wayland-ime",
                       f"--remote-debugging-port={self.PORT}", "--disable-background-networking",
                       "--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE localhost, "
                       "EXCLUDE *.localhost", "about:blank"], "chrome")
        for _ in range(60):
            with contextlib.suppress(OSError, StopIteration):
                page = self.page()
                break
            time.sleep(0.5)
        else:
            raise RuntimeError("Chrome did not start, see chrome.log")
        self.ws = websocket.create_connection(page["webSocketDebuggerUrl"], suppress_origin=True)
        self.id = 0

    def page(self):
        targets = json.load(urllib.request.urlopen(f"http://127.0.0.1:{self.PORT}/json"))
        return next(t for t in targets if t["type"] == "page")

    def call(self, method, **params):
        self.id += 1
        self.ws.send(json.dumps({"id": self.id, "method": method, "params": params}))
        while True:
            reply = json.loads(self.ws.recv())
            if reply.get("id") == self.id:
                return reply.get("result", {})

    def js(self, expression):
        return self.call("Runtime.evaluate", expression=expression)["result"].get("value")


def run_cases(name, cases, runs, attempt):
    """attempt(keys, speed, rng) returns the text the application got."""
    rng = random.Random(1)
    failures = 0
    for keys, want in cases:
        for speed in SPEEDS:
            bad = []
            for _ in range(runs):
                got = attempt(keys, speed, rng)
                if got != want:
                    bad.append(got)
            failures += len(bad)
            status = "ok" if not bad else f"FAIL {bad[:3]!r}"
            print(f"{name} {keys!r} gap {speed[0]} ms: {runs - len(bad)}/{runs} {status}",
                  flush=True)
    return failures


def check_methods(session, name, program, since, want, need=frozenset()):
    """A failure unless bamboo typed the keys of program since the mark the
    ways in want only, and need among them: its typing mode applied."""
    got = session.methods(program, since)
    ok = bool(got) and got <= want and need <= got
    print(f"{name} methods {sorted(got)}: "
          f"{'ok' if ok else f'FAIL, not within {sorted(want)} with {sorted(need)}'}",
          flush=True)
    return 0 if ok else 1


def settled(read, still=1.0, timeout=10):
    """What read returns once it stops changing: on a loaded machine Chrome
    takes long to apply the keys."""
    value, since, deadline = read(), time.monotonic(), time.monotonic() + timeout
    while time.monotonic() < deadline and time.monotonic() - since < still:
        time.sleep(0.2)
        if (now := read()) != value:
            value, since = now, time.monotonic()
    return value


# BambooState::Method
SURROUNDING, BACKSPACES, PLAIN_PREEDIT = 4, 5, 2


def type_into_textarea(session, chrome, runs, name, methods, selection_methods):
    chrome.call("Page.navigate", url="data:text/html,<textarea id=t autofocus></textarea>")
    time.sleep(2)
    mark = session.log_mark()

    def attempt(keys, speed, rng):
        chrome.js("t.value = ''; t.focus(); 1")
        time.sleep(0.3)
        session.keys(key_events(keys + " ", *speed, rng) + ["w500"])
        return settled(lambda: chrome.js("t.value").strip())

    def over_selection(keys, speed, rng):
        # Chrome reports a selection before the cursor: the first key goes in
        # with BackSpace keys, the word going on.
        chrome.js("t.value = 'chao ban'; t.focus(); t.setSelectionRange(5, 8); 1")
        time.sleep(0.3)
        session.keys(key_events(keys + " ", *speed, rng) + ["w500"])
        return settled(lambda: chrome.js("t.value").strip())

    failures = run_cases(name, [
        ("toi6 d9ang hoc5 bai2 hat1 nguoi72 viet65 nam truong72 d9uoc75",
         "tôi đang học bài hát người việt nam trường được"),
        ("nguoi27 d9i truong72 viet65 khong6", "người đi trường việt không"),
    ], runs, attempt) + check_methods(session, name, "google-chrome", mark, methods)
    mark = session.log_mark()
    # The point of the case: the first key over the selection with BackSpace.
    need = {BACKSPACES} if BACKSPACES in selection_methods else set()
    return failures + run_cases(f"{name} selection", [
        ("d9i hoc5", "chao đi học"),
    ], runs, over_selection) + check_methods(session, name, "google-chrome", mark,
                                             selection_methods, need)


def type_into_address_bar(session, chrome, runs, name, methods):
    # Hosts visited, typed: their names complete inline as their start is
    # typed, a suggestion selected after the cursor.
    server = http.server.ThreadingHTTPServer(
        ("127.0.0.1", 0), functools.partial(http.server.SimpleHTTPRequestHandler,
                                            directory=session.work))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    port = server.server_address[1]
    for host in ["baihat.localhost", "vietnamnet.localhost", "facebook.localhost"]:
        for _ in range(3):
            chrome.call("Page.navigate", url=f"http://{host}:{port}/", transitionType="typed")
            time.sleep(1)
    # A page whose address Control+L selects, the first key replacing it.
    # Chrome reports it selected after the cursor, as its suggestion.
    start = f"http://start.localhost:{port}/"
    mark = session.log_mark()

    def attempt(keys, speed, rng):
        # On a loaded machine the page loading can take the focus back from
        # the address bar: nothing gets the keys, which tells nothing.
        for tries in range(3):
            if tries:
                print(f"{name} {keys!r}: no navigation, again", flush=True)
            chrome.call("Page.navigate", url=start)
            time.sleep(0.8)
            focus = [f"d{CTRL}", "w20", f"d{L}", "w20", f"u{L}", "w10", f"u{CTRL}", "w300"]
            session.keys(focus + key_events(keys, *speed, rng)[4:]
                         + ["w300", f"d{RETURN}", "w30", f"u{RETURN}", "w100"])
            for _ in range(100):
                if (url := chrome.page()["url"]) != start:
                    url = urllib.parse.urlparse(url)
                    return urllib.parse.parse_qs(url.query).get("q", [url.hostname])[0]
                time.sleep(0.1)
        return None

    failures = run_cases(name, [
        ("bai2 hat1 viet65 nam", "bài hát việt nam"),
        # The first key goes on with the next ones.
        ("d9i hoc5", "đi học"),
        # Return takes the suggestion.
        ("face", "facebook.localhost"),
    ], runs, attempt)
    server.shutdown()
    return failures + check_methods(session, name, "google-chrome", mark, methods)


def test_chrome(session, runs):
    return type_into_textarea(session, Chrome(session), runs, "chrome",
                              {SURROUNDING}, {SURROUNDING, BACKSPACES})


def test_omnibox(session, runs):
    return type_into_address_bar(session, Chrome(session), runs, "omnibox", {SURROUNDING})


def test_chrome_backspace(session, runs):
    """Chrome in the BackSpace mode, which never waits for its reports: its
    address bar gets Surrounding Text for the suggestion."""
    session.add_app_mode("google-chrome", "BackSpace")
    chrome = Chrome(session)
    return (type_into_textarea(session, chrome, runs, "chrome backspace",
                               {BACKSPACES}, {BACKSPACES})
            + type_into_address_bar(session, chrome, runs, "omnibox backspace", {SURROUNDING}))


def test_gtk(session, runs):
    def attempt(keys, speed, rng):
        zenity = subprocess.Popen(["zenity", "--entry", "--text", "test"], env=session.env,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        session.wait_focus("zenity")
        session.keys(key_events(keys, *speed, rng) + ["w300", f"d{RETURN}", "w30", f"u{RETURN}"])
        try:
            return zenity.communicate(timeout=5)[0].rstrip("\n")
        except subprocess.TimeoutExpired:
            zenity.kill()
            return None

    return run_cases("gtk", [
        ("nguoi27 d9i truong72 viet65 khong6", "người đi trường việt không"),
    ], runs, attempt)


def test_qtquick(session, runs):
    """Qt Quick through fcitx5-qt: a name, then a password. Qt Quick reports
    password fields as sensitive only, which fcitx5 leaves to the input
    method, and shows their preedit unmasked: the keys go in as typed there,
    nothing shown."""
    path = os.path.join(session.work, "login.qml")
    with open(path, "w") as f:
        f.write(QML_LOGIN)

    def attempt(keys, speed, rng):
        name, password = keys.split("|")
        app = subprocess.Popen(["qml6", path], stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, text=True,
                               env=dict(session.env, QT_FORCE_STDERR_LOGGING="1"))
        session.wait_focus("qml")
        # fcitx5-qt reports the field Tab focused after keys already queued,
        # a race fcitx5 has with its own password fields: the password waits.
        session.keys(key_events(name, *speed, rng)
                     + ["w100", f"d{TAB}", "w30", f"u{TAB}", "w300"]
                     + key_events(password, *speed, rng)[4:]
                     + ["w300", f"d{RETURN}", "w30", f"u{RETURN}"])
        try:
            log = app.communicate(timeout=5)[1]
        except subprocess.TimeoutExpired:
            app.kill()
            app.wait()
            return None
        if shown := re.findall(r"PREEDIT\[(.*)\]", log):
            return f"shown {shown}"
        text = re.findall(r"TEXT\[(.*)\]", log)
        return text[-1] if text else None

    return run_cases("qtquick", [
        ("nguoi27 d9i truong72 viet65 khong6|hoang1995 tuan1",
         "người đi trường việt không|hoang1995 tuan1"),
    ], runs, attempt)


def test_kdialog(session, runs):
    """A Qt Widgets field in the BackSpace mode, through fcitx5-qt, which
    drops the surrounding text capability before every key: plain preedit,
    no DEL characters as for Konsole."""
    session.add_app_mode("kdialog", "BackSpace")
    mark = session.log_mark()

    def attempt(keys, speed, rng):
        app = subprocess.Popen(["kdialog", "--inputbox", "test"], env=session.env,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        session.wait_focus("kdialog")
        session.keys(key_events(keys, *speed, rng) + ["w300", f"d{RETURN}", "w30", f"u{RETURN}"])
        try:
            return app.communicate(timeout=5)[0].rstrip("\n")
        except subprocess.TimeoutExpired:
            app.kill()
            app.wait()
            return None

    return run_cases("kdialog", [
        ("nguoi27 d9i truong72 viet65 khong6", "người đi trường việt không"),
    ], runs, attempt) + check_methods(session, "kdialog", "kdialog", mark, {PLAIN_PREEDIT})


def test_qt_text(session, runs):
    """A Qt Widgets field through fcitx5-qt, which drops the surrounding
    text capability before every key, its text read all the same: a
    sentence is capitalized from it after a cursor move, and the convert
    key takes its selection, the key's Control, pressed alone first, not
    marking the text stale. The session disables the clipboard addon, so
    only the reported selection can convert, and turns CapitalizeSentences
    on."""

    def capitalize(keys, speed, rng):
        app = subprocess.Popen(["kdialog", "--inputbox", "test", "Hello. "],
                               env=session.env, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, text=True)
        session.wait_focus("kdialog")
        session.keys([f"d{END}", "w30", f"u{END}", "w200"] + key_events(keys, *speed, rng)
                     + ["w300", f"d{RETURN}", "w30", f"u{RETURN}"])
        try:
            return app.communicate(timeout=5)[0].rstrip()
        except subprocess.TimeoutExpired:
            app.kill()
            app.wait()
            return None

    def convert(keys, speed, rng):
        app = subprocess.Popen(["kdialog", "--inputbox", "test", "chao viet65"],
                               env=session.env, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, text=True)
        session.wait_focus("kdialog")
        # Select "viet65", then the convert key: Control pressed alone
        # first, as it is on the way to Control+Shift+F6.
        events = [f"d{END}", "w30", f"u{END}", "w200", f"d{SHIFT}"]
        for _ in range(6):
            events += ["w40", f"d{LEFT}", "w20", f"u{LEFT}"]
        events += [f"u{SHIFT}", "w200",
                   f"d{CTRL}", "w30", f"d{SHIFT}", "w30", f"d{F6}", "w20", f"u{F6}",
                   "w30", f"u{SHIFT}", "w30", f"u{CTRL}", "w300",
                   # Takes the first conversion, the input method's retype.
                   f"d{RETURN}", "w30", f"u{RETURN}", "w300",
                   # Closes the dialog.
                   f"d{RETURN}", "w30", f"u{RETURN}"]
        session.keys(events)
        try:
            return app.communicate(timeout=5)[0].rstrip()
        except subprocess.TimeoutExpired:
            app.kill()
            app.wait()
            return None

    return (run_cases("qt-text capitalize", [("x ", "Hello. X")], runs, capitalize)
            + run_cases("qt-text convert", [("", "chao việt")], runs, convert))


def type_into_terminal(session, runs, name, program, terminal):
    """Types into an application setting Claude Code's terminal modes, in
    tmux, in a terminal started with the command line terminal: the text
    arrives as typed, fixed as typed, never as a bracketed paste."""
    mark = session.log_mark()
    rawlog = os.path.join(session.work, "rawlog.py")
    with open(rawlog, "w") as f:
        f.write(RAWLOG)
    log = os.path.join(session.work, "terminal-input.log")
    tmux = ["tmux", "-L", f"bamboo-desktop-test-{name}", "-f", "/dev/null"]
    session.spawn([*terminal, *tmux, "new-session", f"{sys.executable} {rawlog} {log}"], name)
    session.wait_focus(program)
    for _ in range(150):
        panes = subprocess.run([*tmux, "list-panes", "-F", "#{pane_current_command}"],
                               capture_output=True, text=True).stdout
        if "python" in panes:
            break
        time.sleep(0.2)

    def attempt(keys, speed, rng):
        open(log, "w").close()
        session.keys(key_events(" " + keys + " ", *speed, rng) + ["w500"])
        data = b"".join(ast.literal_eval(line) for line in open(log))
        if b"\x1b[200~" in data:
            return "bracketed paste"
        # Fixed as typed, not committed word by word.
        if b"\x7f" not in data:
            return "no DEL"
        text = ""
        for ch in data.decode("utf-8", "replace"):
            text = text[:-1] if ch == "\x7f" else text + ch
        return text.strip()

    failures = run_cases(name, [
        ("toi6 biet61 ro4 nguoi72 viet65 nam", "tôi biết rõ người việt nam"),
    ], runs, attempt)
    subprocess.run([*tmux, "kill-server"], stderr=subprocess.DEVNULL)
    return failures + check_methods(session, name, program, mark, {BACKSPACES})


def test_terminal(session, runs):
    """Alacritty through KWin: words fixed with forwarded BackSpace keys."""
    return type_into_terminal(session, runs, "terminal", "Alacritty", ["alacritty", "-e"])


def test_konsole(session, runs):
    """Konsole in the BackSpace mode, through fcitx5-qt: words fixed with DEL
    characters in the commits, keys handled in order in its sync mode."""
    return type_into_terminal(session, runs, "konsole", "konsole", [
        "env", "FCITX_QT_USE_SYNC=1", "konsole", "--separate", "-e"])


TESTS = {"chrome": test_chrome, "omnibox": test_omnibox,
         "chrome-backspace": test_chrome_backspace, "gtk": test_gtk, "qtquick": test_qtquick,
         "kdialog": test_kdialog, "qt-text": test_qt_text, "terminal": test_terminal,
         "konsole": test_konsole}

# Extra configuration some tests need, appended to FCITX_CONFIG / BAMBOO_CONFIG.
SESSION_OPTIONS = {
    "qt-text": dict(fcitx_extra="\n[Behavior/DisabledAddons]\n0=clipboard\n",
                    bamboo_extra="CapitalizeSentences=True\n"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("tests", nargs="*", help=f"some of {', '.join(TESTS)}; all by default")
    parser.add_argument("--addon-dir", help="directory with the libbamboo.so to test, "
                        "a build's src; the installed one by default")
    parser.add_argument("--runs", type=int, default=3, help="runs per case and speed")
    parser.add_argument("--keep", action="store_true", help="keep the work directory")
    args = parser.parse_args()
    if unknown := set(args.tests) - set(TESTS):
        parser.error(f"unknown tests: {', '.join(unknown)}")
    work = tempfile.mkdtemp(prefix="bamboo-desktop-test-")
    failures = 0
    try:
        for name in args.tests or TESTS:
            with Session(os.path.join(work, name), args.addon_dir,
                         **SESSION_OPTIONS.get(name, {})) as session:
                failures += TESTS[name](session, args.runs)
    finally:
        if args.keep:
            print(f"logs in {work}")
        else:
            # fcitx5 may still write its configuration when it quits.
            for _ in range(5):
                shutil.rmtree(work, ignore_errors=True)
                if not os.path.exists(work):
                    break
                time.sleep(1)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
