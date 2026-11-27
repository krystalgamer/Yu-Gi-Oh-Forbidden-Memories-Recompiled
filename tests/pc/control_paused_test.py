#!/usr/bin/env python3
"""The control channel while the game is paused (src/pc/debug/control.c,
serve_paused; notes/agent-control.md, "While the game is paused"): CTest's.
Skipped (77) without the game (YFM_EXECUTABLE, else tmp/pc/game32/memories-pc
[.exe]) or the disc (MEMORIES_DISC).

The game runs headless on the real clock with no client (MEMORIES_CONTROL_WAIT=0)
and stops its clock at frame 60 (MEMORIES_CRASH_TEST=pause@60), as a player's
pause does. A client that comes only now, the case that went unanswered, is
taken by the pause's wait:

  1. `info`, `step 0` and `peek` are answered, and the game stands at the
     same frame and VBlank through all of it (nothing ran), also a second
     later.
  2. A line that does not parse is answered "err".
  3. `step 1` is not answered: it needs the game to run, so it waits.
  4. The client closes; a second takes its place at once; the game still
     stands at the same frame (the step that waited did not run).
  5. A client sends `step 1` (held), then `quit` in a later segment (held
     behind the step), then closes: it is dropped all the same, and the next
     client is answered (reading stops at a held line, and a one-byte peek
     says "not gone" while unread bytes wait).
  6. `quit` is answered, and the game ends with exit code 0.

Three more games:
  - A client attached before the pause, with a step in flight: its `quit`
    gets the step's reply first (an "err"), then its own "ok", and the game
    ends with exit code 0.
  - MEMORIES_CRASH_TEST=pausewait@60 pauses inside VSync(1000), not VSync(0)'s
    own wait: `info` is answered and `quit` ends the game with exit code 0 (it
    used to spin at full speed, never reaching VSync's exit).

  - MEMORIES_CRASH_TEST=pause@600 pauses a game that polls VSync(-1) or
    VSync(1) instead of waiting for a VBlank (the intro movie's wait for
    sectors; it had no answer, and `quit` was never seen): `info` and `step 0`
    are answered, nothing moves, and `quit` ends the game with exit code 0
    within seconds (Platform_PollTime serves the pause's hook at rate 0, and
    VSync(-1) ends in LibEtc_ExitIfQuit).

What this does not cover: a window's pause (the pause loop of pace(), which
a headless game never runs, and the window events the poll now pumps), and a
frame step."""
from __future__ import annotations

import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/pc"))
import yfm_control  # noqa: E402

SKIP = 77
EXE = Path(os.environ.get("YFM_EXECUTABLE") or yfm_control.EXECUTABLE)
DISC = os.environ.get("MEMORIES_DISC", "")
SETTINGS = "mod.3d-monsters=0\nmod.hand-camera=0\n"


class Client:
    def __init__(self, port: int):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.buffer = b""

    def send(self, line: str) -> None:
        self.socket.sendall(line.encode() + b"\n")

    def reply(self, timeout: float = 10.0) -> str | None:
        """The next reply line, or None when none comes in `timeout` seconds."""
        self.socket.settimeout(timeout)
        while b"\n" not in self.buffer:
            try:
                chunk = self.socket.recv(4096)
            except socket.timeout:
                return None
            if not chunk:
                raise AssertionError("the game closed the channel")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return line.decode()

    def ask(self, line: str) -> str:
        self.send(line)
        answer = self.reply()
        assert answer is not None, f"no reply to {line!r} while the game is paused"
        return answer

    def close(self) -> None:
        self.socket.close()


def where(reply: str) -> tuple[int, int]:
    assert reply.startswith("ok frame "), reply
    fields = reply.split()
    return int(fields[fields.index("frame") + 1]), int(fields[fields.index("vblank") + 1])


class Game:
    """One headless game on the real clock with the control channel up and no
    wait for a first client, paused by MEMORIES_CRASH_TEST=`crash`@60."""

    def __init__(self, crash: str, frame: int = 60):
        self.crash = crash
        self.work = Path(tempfile.mkdtemp(prefix="control-paused-"))
        settings = self.work / "settings.txt"
        settings.write_text(SETTINGS, encoding="utf-8")
        (self.work / "user").mkdir()
        self.port = yfm_control.free_port()
        env = {key: value for key, value in os.environ.items() if not key.startswith("MEMORIES_")}
        env.update(MEMORIES_DISC=DISC, MEMORIES_HEADLESS="1", MEMORIES_NO_AUDIO="1", SDL_AUDIODRIVER="dummy",
                   MEMORIES_NO_GAMEPAD="1", MEMORIES_NO_UPDATE_CHECK="1", MEMORIES_WATCHDOG="0",
                   MEMORIES_SHOW_HUD="0", MEMORIES_USER_DIR=str(self.work / "user"), MEMORIES_SETTINGS=str(settings),
                   MEMORIES_CONTROL=str(self.port), MEMORIES_CONTROL_WAIT="0", MEMORIES_CRASH_TEST=f"{crash}@{frame}")
        command, wine = yfm_control.launcher(EXE)
        self.log_path = self.work / "game.log"
        with self.log_path.open("wb") as log:
            self.process = subprocess.Popen(command, cwd=ROOT, env={**env, **wine}, stdout=log,
                                            stderr=subprocess.STDOUT)
        self.clients: list[Client] = []

    def log(self) -> str:
        return self.log_path.read_text(encoding="utf-8", errors="replace")

    def connect(self) -> Client:
        client = Client(self.port)
        self.clients.append(client)
        return client

    def wait_for(self, text: str, what: str) -> None:
        deadline = time.monotonic() + 120
        while text not in self.log():
            assert self.process.poll() is None, f"the game ended with {self.process.returncode} {what}; see {self.log_path}"
            assert time.monotonic() < deadline, f"the game never {what}; see {self.log_path}"
            time.sleep(0.1)

    def wait_paused(self) -> None:
        self.wait_for(f"crash test: {self.crash} now", "paused")
        time.sleep(0.5)   # in the pause's wait by now

    def replacement(self) -> tuple[Client, str]:
        """A new client, once the one that left has been dropped: the first
        `info` it is answered (an "err busy" means the game still holds the
        old one)."""
        deadline = time.monotonic() + 10
        while True:
            client = self.connect()
            answer = client.ask("info")
            if not answer.startswith("err busy"):
                return client, answer
            assert time.monotonic() < deadline, "the client that left was never dropped"
            client.close()
            time.sleep(0.2)

    def end(self, expect_code: int = 0) -> None:
        code = self.process.wait(timeout=60)
        assert code == expect_code, f"the game ended with {code}; see {self.log_path}"

    def close(self) -> None:
        for client in self.clients:
            client.close()
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()


def paused_game() -> tuple[int, int]:
    """A client that comes after the pause: what it is answered, a client that
    leaves with commands unread, then quit. Returns where the game stood."""
    game = Game("pause")
    try:
        game.wait_paused()
        client = game.connect()
        frame, vblank = where(client.ask("info"))
        assert frame >= 60, frame
        assert where(client.ask("step 0")) == (frame, vblank)
        peeked = client.ask("peek 80010000 4")
        assert peeked.startswith("ok ") and len(peeked) == 3 + 8, peeked
        assert client.ask("no such command").startswith("err"), "a line that does not parse is answered"
        time.sleep(1.0)
        assert where(client.ask("info")) == (frame, vblank), "the paused game ran"

        client.send("step 1")
        assert client.reply(1.5) is None, "a step was answered while the game is paused"
        client.close()

        # A client that left is replaced at once, and the step it left
        # waiting did not run.
        client, answer = game.replacement()
        assert where(answer) == (frame, vblank), "the step that waited ran"

        # A client that sends a step (held), then a quit in a later segment
        # (held behind it), then closes: the game reads on while it holds the
        # step, so it sees the close, drops the client and takes the next.
        client.send("step 1")
        time.sleep(0.3)
        client.send("quit")
        time.sleep(0.3)
        client.close()
        client, answer = game.replacement()
        assert where(answer) == (frame, vblank), "the step or the quit that waited ran"
        assert game.process.poll() is None, "the quit of a client that had left ended the game"

        assert client.ask("quit") == "ok"
        game.end()
        return frame, vblank
    finally:
        game.close()


def step_in_flight() -> None:
    """A client attached before the pause, with a step in flight: its quit is
    answered with the step's reply (an "err") first, then its own "ok"."""
    game = Game("pause", frame=300)   # time for the client to attach and its step to be taken, whatever the load
    try:
        game.wait_for("control: listening on", "listened")
        client = game.connect()
        client.send("step 100000")   # taken at the next VSync(0): far past frame 300
        game.wait_paused()
        assert client.reply(1.0) is None, "a step in flight was answered while the game is paused"
        client.send("quit")
        first, second = client.reply(), client.reply()
        assert first is not None and first.startswith("err"), f"the step's reply is an err first: {first!r}"
        assert second == "ok", f"the quit's own ok comes second: {second!r}"
        game.end()
    finally:
        game.close()


def quit_outside_vsync0() -> None:
    """A pause inside VSync(1000), not VSync(0)'s own wait: info is answered
    and quit ends the game with 0 (it spun for ever)."""
    game = Game("pausewait")
    try:
        game.wait_paused()
        client = game.connect()
        assert client.ask("info").startswith("ok frame "), "info while the game waits in VSync(1000)"
        assert client.ask("quit") == "ok"
        game.end()
    finally:
        game.close()


def quit_while_polling() -> None:
    """A pause at frame 600, where the game polls VSync(-1) or VSync(1)
    rather than waiting for a VBlank: the poll serves the hook and `quit`
    ends the game (a quit at VSync(-1) was never seen)."""
    game = Game("pause", frame=600)
    try:
        game.wait_paused()
        client = game.connect()
        frame, vblank = where(client.ask("info"))
        assert frame >= 600, frame
        assert where(client.ask("step 0")) == (frame, vblank)
        time.sleep(1.0)
        assert where(client.ask("info")) == (frame, vblank), "the paused game ran"
        assert client.ask("quit") == "ok"
        started = time.monotonic()
        game.end()
        assert time.monotonic() - started < 10, "quit took long to end a game that polls"
    finally:
        game.close()


def main() -> int:
    if not EXE.is_file() or not DISC or not Path(DISC).is_file():
        print("control_paused_test: skipped: no game or no disc")
        return SKIP
    frame, _ = paused_game()
    step_in_flight()
    quit_outside_vsync0()
    quit_while_polling()
    print(f"control_paused_test: a paused game answered info, step 0 and peek at frame {frame}, held a step, "
          "dropped a client that left behind it, answered a step in flight with an err before the quit's ok, and "
          "quit ended it, also from a wait outside VSync(0) and from a poll of VSync(-1)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
