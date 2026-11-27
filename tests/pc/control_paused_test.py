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
  5. `quit` is answered, and the game ends with exit code 0.

What this does not cover: a window's pause (the pause loop of pace(), which
a headless game never runs), a frame step, a client attached before the
pause (its step in flight is answered first)."""
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


def main() -> int:
    if not EXE.is_file() or not DISC or not Path(DISC).is_file():
        print("control_paused_test: skipped: no game or no disc")
        return SKIP
    work = Path(tempfile.mkdtemp(prefix="control-paused-"))
    settings = work / "settings.txt"
    settings.write_text(SETTINGS, encoding="utf-8")
    (work / "user").mkdir()
    port = yfm_control.free_port()
    env = {key: value for key, value in os.environ.items() if not key.startswith("MEMORIES_")}
    env.update(MEMORIES_DISC=DISC, MEMORIES_HEADLESS="1", MEMORIES_NO_AUDIO="1", SDL_AUDIODRIVER="dummy",
               MEMORIES_NO_GAMEPAD="1", MEMORIES_NO_UPDATE_CHECK="1", MEMORIES_WATCHDOG="0",
               MEMORIES_SHOW_HUD="0", MEMORIES_USER_DIR=str(work / "user"), MEMORIES_SETTINGS=str(settings),
               MEMORIES_CONTROL=str(port), MEMORIES_CONTROL_WAIT="0", MEMORIES_CRASH_TEST="pause@60")
    command, wine = yfm_control.launcher(EXE)
    log_path = work / "game.log"
    with log_path.open("wb") as log:
        game = subprocess.Popen(command, cwd=ROOT, env={**env, **wine}, stdout=log, stderr=subprocess.STDOUT)
    clients: list[Client] = []
    try:
        deadline = time.monotonic() + 120
        while "crash test: pause now" not in log_path.read_text(encoding="utf-8", errors="replace"):
            assert game.poll() is None, f"the game ended with {game.returncode} before it paused; see {log_path}"
            assert time.monotonic() < deadline, f"the game never paused; see {log_path}"
            time.sleep(0.1)
        time.sleep(0.5)   # in the pause's wait by now

        client = Client(port)
        clients.append(client)
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
        client = Client(port)
        clients.append(client)
        deadline = time.monotonic() + 10
        while True:
            answer = client.ask("info")
            if not answer.startswith("err busy"):
                break
            assert time.monotonic() < deadline, "the first client was never dropped"
            client.close()
            time.sleep(0.2)
            client = Client(port)
            clients.append(client)
        assert where(answer) == (frame, vblank), "the step that waited ran"

        assert client.ask("quit") == "ok"
        code = game.wait(timeout=60)
        assert code == 0, f"the game ended with {code}; see {log_path}"
    finally:
        for client in clients:
            client.close()
        if game.poll() is None:
            game.kill()
            game.wait()
    print(f"control_paused_test: a paused game answered info, step 0 and peek at frame {frame}, held a step, "
          "and quit ended it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
