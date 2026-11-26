#!/usr/bin/env python3
"""The player's replays against the game (notes/replays.md): CTest's, as CI
has no disc to run tools/pc/replay.py with and tests/pc/replays holds no
state. Skipped (77) without the game (YFM_EXECUTABLE, else
tmp/pc/game32/memories-pc[.exe]) or the disc (MEMORIES_DISC).

Headless, on the virtual clock, with SDL's dummy sound:

  1. A recording made from boot plays back with every frame hash the same.
  2. A recording started and stopped at run time (the control channel's
     `record`, as Game > Record replay does) is a .yfmreplay with its start
     state, the player's progress, an E line and no C line; a second
     `record` is refused while it runs, and `record stop` when none does.
     Along the way the game saves at the shop (gameplay_inputs.py's save).
  3. That replay plays back with every frame hash the same, and the save it
     makes goes to the scratch progress: the player's own saves hash the same
     before and after.
  4. The same replay with a C line in it is refused: nothing plays. So is one
     whose start state does not load (damaged here): no frame is played onto
     the game booting without it.
  5. Game > Record replay from boot... (the control channel's `record_boot`,
     which does what the menu's item does after its question): the game
     restarts and records from its first frame. The .yfmreplay has no start
     state at all (replay.json says the start is boot) and still carries the
     player's progress; the channel refuses it while a recording runs and for
     a name that is not a .yfmreplay. Its play, by File > Play replay...'s own
     path (MEMORIES_PLAY), has every frame hash as recorded and leaves the
     player's saves as they were.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/pc"))
import replay  # noqa: E402
import yfm_control  # noqa: E402
from gameplay_inputs import inputs  # noqa: E402

SKIP = 77
EXE = Path(os.environ.get("YFM_EXECUTABLE") or yfm_control.EXECUTABLE)
DISC = os.environ.get("MEMORIES_DISC", "")
SETTINGS = "mod.3d-monsters=0\nmod.hand-camera=0\n"


def save_timeline() -> str:
    """tools/pc/test_arm64_save_load.py's: the campaign's opening, then a save at the shop."""
    opening = [part for part in inputs().split(",") if int(part.split(":")[0]) <= 4906]
    events = [(5600, "0040"), (5900, "0020"), (6200, "0020"), (6500, "4000")]
    events += [(f, "4000") for f in (6800, 7100, 7400, 7800, 8200, 8600)]
    events += [(f + 6, "0000") for f, _ in list(events)]
    return ",".join(opening + [f"{f}:{b}" for f, b in sorted(events)])


def environment(user: Path, **extra: str) -> dict[str, str]:
    env = {key: value for key, value in os.environ.items() if not key.startswith("MEMORIES_")}
    user.mkdir(parents=True, exist_ok=True)
    settings = user.parent / f"{user.name}-settings.txt"
    settings.write_text(SETTINGS, encoding="utf-8")
    env.update(MEMORIES_DISC=DISC, MEMORIES_HEADLESS="1", MEMORIES_DETERMINISTIC="1", SDL_AUDIODRIVER="dummy",
               MEMORIES_NO_GAMEPAD="1", MEMORIES_NO_UPDATE_CHECK="1", MEMORIES_WATCHDOG="0",
               MEMORIES_SHOW_HUD="0", MEMORIES_USER_DIR=str(user), MEMORIES_SETTINGS=str(settings))
    env.update(extra)
    return env


def run_game(env: dict[str, str], log: Path, timeout: float = 900) -> str:
    command, wine = yfm_control.launcher(EXE)
    with log.open("wb") as out:
        code = subprocess.run(command, cwd=ROOT, env={**env, **wine}, stdout=out, stderr=subprocess.STDOUT,
                              timeout=timeout).returncode
    text = log.read_text(encoding="utf-8", errors="replace")
    assert code == 0, f"the game ended with {code}; see {log}"
    return text


def checked(expected_folder: Path, actual: Path, what: str) -> int:
    """replay.py's check: every frame hash of the recording, in the play."""
    expected = replay.Replay(expected_folder)
    assert replay.check(expected, actual), f"{what}: the play differs (see above)"
    count = len(replay.parse(expected.file(expected.meta["recording"]))["H"])
    print(f"player_replay_test: {what}: {count} frames as recorded")
    return count


def progress_digest(user: Path) -> dict[str, str]:
    """The player's progress files (paths.h, Paths_IsProgress) by SHA-256."""
    found = {}
    for name in ("saves", "cards", "packs", "duelists", "decks", "tower", "memcard1.mcd", "memcard2.mcd"):
        path = user / name
        files = [path] if path.is_file() else sorted(p for p in path.rglob("*") if p.is_file()) if path.is_dir() else []
        for file in files:
            found[str(file.relative_to(user))] = hashlib.sha256(file.read_bytes()).hexdigest()
    return found


def from_boot(work: Path) -> None:
    recording, played = work / "boot.txt", work / "boot-played.txt"
    presses = "700:0008,706:0000,800:4000,806:0000,950:4000,956:0000"
    run_game(environment(work / "boot-user", MEMORIES_RECORD=str(recording), MEMORIES_INPUT=presses,
                         MEMORIES_DUMP_FRAME="1500", MEMORIES_DUMP_PATH=str(work / "boot.ppm")), work / "boot.log")
    folder = work / "boot-replay"
    replay.package_recording(folder, recording, None, 1, None, "player_replay_test")
    run_game(environment(work / "boot-play-user", MEMORIES_PLAY=str(recording), MEMORIES_RECORD=str(played)),
             work / "boot-play.log")
    assert checked(folder, played, "from boot") > 500


def wait_for_package(game: yfm_control.Game, file: Path, timeout: float = 600) -> None:
    """The restarted game (no client follows it: Linux execs the same process, Windows starts another and
    ends this one) ends by itself at MEMORIES_DUMP_FRAME and packs the file; its parts folder goes last."""
    parts = Path(str(file) + ".parts")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if file.is_file() and not parts.exists():
            break
        time.sleep(0.5)
    else:
        raise AssertionError(f"{file} was not made; see {game.log_path}")
    try:
        code = game.process.wait(timeout=120)
    except subprocess.TimeoutExpired:
        raise AssertionError(f"the restarted game did not end; see {game.log_path}") from None
    assert code == 0, f"the restarted game ended with {code}; see {game.log_path}"


def record_from_boot_item(work: Path) -> Path:
    """The control channel's `record_boot`, from a game that is well into its run."""
    file = work / "boot-item.yfmreplay"
    user = work / "boot-item-player"
    (user / "decks").mkdir(parents=True)
    (user / "decks/DEADBEEF.txt").write_text("player_replay_test\n")
    presses = "700:0008,706:0000,800:4000,806:0000,950:4000,956:0000"
    # The restarted game has the old one's environment (a restart keeps it, MEMORIES_CONTROL aside): it
    # replays these presses from its first frame and ends, closing the recording, at frame 1500.
    game = yfm_control.Game(EXE, out=work / "boot-item", env={"MEMORIES_INPUT": presses,
                                                              "MEMORIES_USER_DIR": str(user),
                                                              "MEMORIES_DUMP_FRAME": "1500",
                                                              "MEMORIES_DUMP_PATH": str(work / "boot-item.ppm"),
                                                              "SDL_AUDIODRIVER": "dummy"},
                            settings={"mod.3d-monsters": 0, "mod.hand-camera": 0})
    try:
        game.wait_until(lambda g: g.info()["frame"] >= 1000, 3000, what="frame 1000")
        try:
            game.command(f"record_boot {work / 'not-a-replay.txt'}")
            raise AssertionError("a recording from boot to a name that is not a .yfmreplay was taken")
        except yfm_control.ControlError as refused:
            assert ".yfmreplay" in str(refused), refused
        assert not (user / "replays/next-record.txt").exists(), "a refused request was left for the next start"
        game.command(f"record_boot {file}")   # "ok", then the restart
        wait_for_package(game, file)
    finally:
        game.close()
    with zipfile.ZipFile(file) as archive:
        names = set(archive.namelist())
        lines = archive.read("recording.txt").decode().splitlines()
        meta = json.loads(archive.read("replay.json"))
    for name in ("replay.json", "recording.txt"):
        assert name in names, f"{file} has no {name}: {sorted(names)}"
    states = sorted(name for name in names if name.endswith(".state"))
    assert not states, f"a recording from boot holds a state: {states}"
    assert meta["start"] == {"kind": "boot"} and meta.get("player") is True, meta
    assert "progress/decks/DEADBEEF.txt" in names, f"{file} has not the player's progress: {sorted(names)}"
    assert "start boot" in lines[:3] and lines[-1].startswith("E "), lines[:3] + lines[-2:]
    assert not any(line.startswith(("C ", "S ")) for line in lines), "a player's recording has a C or S line"
    frames = [line for line in lines if line.startswith("H ")]
    assert len(frames) > 500 and frames[0].split()[1] in ("0", "1"), \
        f"the recording does not start at the game's first frame: {frames[:1]} ({len(frames)} frames)"
    assert not (user / "replays/next-record.txt").exists(), "the request was not taken at the restart"
    print(f"player_replay_test: recorded {file.name} from boot through the restart: {len(frames)} frames, "
          f"{len(names)} files, no state")
    return file


def play_from_boot_item(work: Path, file: Path) -> None:
    """The file plays back as File > Play replay... plays it, frame for frame, on a fresh player's folder."""
    user = work / "boot-listener"
    (user / "saves").mkdir(parents=True)
    (user / "saves/slot05.sav").write_bytes(bytes(range(256)) * 32)
    before = progress_digest(user)
    played = work / "boot-item-played.txt"
    text = run_game(environment(user, MEMORIES_PLAY=str(file), MEMORIES_RECORD=str(played)),
                    work / "boot-item-play.log")
    assert "replay: playing" in text, f"the replay did not play; see {work / 'boot-item-play.log'}"
    assert before == progress_digest(user), "the play changed the player's saves"
    assert (user / "replays/.playing/progress/decks/DEADBEEF.txt").is_file(), "the replay's progress was not unpacked"
    count = checked(file, played, "recorded from boot by the restart")
    assert count > 500, count


def record_at_run_time(work: Path) -> Path:
    file = work / "shop.yfmreplay"
    user = work / "player"
    # Something of the player's progress to be copied in: a deck slot file kept
    # beside a save by its token (paths.h), which no save here has.
    (user / "decks").mkdir(parents=True)
    (user / "decks/DEADBEEF.txt").write_text("player_replay_test\n")
    game = yfm_control.Game(EXE, out=work / "record", env={"MEMORIES_INPUT": save_timeline(),
                                                           "MEMORIES_USER_DIR": str(user),
                                                           "SDL_AUDIODRIVER": "dummy"},
                            settings={"mod.3d-monsters": 0, "mod.hand-camera": 0})
    try:
        game.wait_until(lambda g: g.info()["frame"] >= 600, 3000, what="frame 600")
        try:
            game.command("record stop")
            raise AssertionError("record stop with no recording was taken")
        except yfm_control.ControlError as refused:
            assert "not recording" in str(refused), refused
        for _ in range(20):
            try:
                game.command(f"record {file}")
                break
            except yfm_control.ControlError as refused:   # not a state point: the next one
                assert "state point" in str(refused), refused
                game.step(1)
        else:
            raise AssertionError("the recording never started")
        assert "clock virtual" in game.command("info")
        try:
            game.command(f"record {work / 'second.yfmreplay'}")
            raise AssertionError("a second recording was started")
        except yfm_control.ControlError as refused:
            assert "Already recording" in str(refused), refused
        try:
            game.command(f"record_boot {work / 'third.yfmreplay'}")
            raise AssertionError("a recording from boot was started while one runs")
        except yfm_control.ControlError as refused:
            assert "Already recording" in str(refused), refused
        game.wait_until(lambda g: g.info()["frame"] >= 9000, 40000, every=60, what="frame 9000")
        game.command("record stop")
        game.quit()
    finally:
        game.close()
    assert (user / "saves/slot01.sav").is_file(), f"the session did not save at the shop; see {work / 'record'}"
    with zipfile.ZipFile(file) as archive:
        names = set(archive.namelist())
        lines = archive.read("recording.txt").decode().splitlines()
    for name in ("replay.json", "recording.txt", "start.state"):
        assert name in names, f"{file} has no {name}: {sorted(names)}"
    assert "progress/decks/DEADBEEF.txt" in names, f"{file} has not the player's progress: {sorted(names)}"
    assert "start state start.state" in lines and lines[-1].startswith("E "), lines[:3] + lines[-2:]
    assert not any(line.startswith("C ") for line in lines), "a player's recording has a C line"
    print(f"player_replay_test: recorded {file.name}: {sum(l.startswith('H ') for l in lines)} frames, "
          f"{len(names)} files")
    return file


def play_without_touching_the_saves(work: Path, file: Path) -> None:
    user = work / "listener"
    (user / "saves").mkdir(parents=True)
    (user / "saves/slot05.sav").write_bytes(bytes(range(256)) * 32)   # the player's own, which must stay
    before = progress_digest(user)
    played = work / "shop-played.txt"
    text = run_game(environment(user, MEMORIES_PLAY=str(file), MEMORIES_RECORD=str(played)), work / "play.log")
    assert "replay: playing" in text, f"the replay did not play; see {work / 'play.log'}"
    after = progress_digest(user)
    assert before == after, f"the play changed the player's saves: {before} -> {after}"
    scratch = user / "replays/.playing/progress/saves/slot01.sav"
    assert scratch.is_file(), f"the play did not save at the shop (no {scratch})"
    assert (user / "replays/.playing/progress/decks/DEADBEEF.txt").is_file(), "the replay's progress was not unpacked"
    count = checked(file, played, "at run time")
    print(f"player_replay_test: the play saved to the scratch copy; the player's {len(before)} progress files "
          f"hash the same ({count} frames)")


def altered(file: Path, out: Path, name: str, change) -> Path:
    with zipfile.ZipFile(file) as source, zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as copy:
        for item in source.infolist():
            data = source.read(item)
            copy.writestr(item, change(data) if item.filename == name else data)
    return out


def refuse_commands(work: Path, file: Path) -> None:
    def poke(data: bytes) -> bytes:
        lines = data.decode().splitlines()
        first = next(i for i, line in enumerate(lines) if line.startswith("H "))
        lines.insert(first + 1, "C 2 2 poke 80000000 00")
        return ("\n".join(lines) + "\n").encode()
    poked = altered(file, work / "poked.yfmreplay", "recording.txt", poke)
    text = run_game(environment(work / "refuser", MEMORIES_PLAY=str(poked), MEMORIES_DUMP_FRAME="120",
                                MEMORIES_DUMP_PATH=str(work / "refused.ppm")), work / "refused.log")
    assert "holds control commands" in text and "replay: playing" not in text, f"not refused; see {work / 'refused.log'}"
    print("player_replay_test: a replay with a C line is refused")


def refuse_damaged_state(work: Path, file: Path) -> None:
    """A start state that does not load is not played onto a booting game."""
    damaged = altered(file, work / "damaged.yfmreplay", "start.state", lambda data: data[:16] + b"\0" * 4096)
    played = work / "damaged-played.txt"
    text = run_game(environment(work / "damaged-user", MEMORIES_PLAY=str(damaged), MEMORIES_RECORD=str(played),
                                MEMORIES_DUMP_FRAME="400", MEMORIES_DUMP_PATH=str(work / "damaged.ppm")),
                    work / "damaged.log")
    assert "start state could not be loaded" in text, f"not refused; see {work / 'damaged.log'}"
    assert not any(line.startswith("H ") for line in played.read_text().splitlines()), "it played anyway"
    print("player_replay_test: a replay whose start state does not load is stopped")


def main() -> int:
    if not EXE.is_file() or not DISC or not Path(DISC).is_file():
        print("player_replay_test: skipped (needs the game and MEMORIES_DISC)")
        return SKIP
    work = Path(tempfile.mkdtemp(prefix="player-replay-", dir=os.environ.get("PLAYER_REPLAY_TMP")))
    try:
        from_boot(work)
        boot_file = record_from_boot_item(work)
        play_from_boot_item(work, boot_file)
        file = record_at_run_time(work)
        play_without_touching_the_saves(work, file)
        refuse_commands(work, file)
        refuse_damaged_state(work, file)
    except BaseException:
        print(f"player_replay_test: kept {work}")
        raise
    shutil.rmtree(work, ignore_errors=True)
    print("player_replay_test: passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
