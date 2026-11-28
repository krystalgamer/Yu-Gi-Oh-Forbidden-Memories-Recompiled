"""Check named RAM reads on the native macOS ARM64 control channel."""

import argparse
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/pc"))
from yfm_control import ControlError, Game


def reject(game, command, reason):
    try:
        game.command(command)
    except ControlError as error:
        assert reason in str(error), error
    else:
        raise AssertionError(f"control command unexpectedly succeeded: {command}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "tmp/pc/macos/memories-arm64")
    parser.add_argument("--disc", type=Path, required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="yfm-arm64-symbols-") as folder:
        out = Path(folder)
        mods = out / "mods"
        mods.mkdir()
        with Game(args.exe, out=out, mods_dir=mods,
                  env={"MEMORIES_DISC": str(args.disc.resolve())}) as game:
            mode = "D_8009B26C"
            assert game.address(mode) == 0x8009B26C
            assert game.u8(mode) == game.u8(0x8009B26C) == game.info()["mode"]

            native = "gDuel_wEquipDefenseExtra"
            assert game.locate(native) == "@" + native
            assert game.u16(native) == 0
            assert game.u8(native, offset=1) == 0
            reject(game, f"peek @{native}+2 1", "past its end")
            reject(game, "peek @ControlProtocol_GuestRange 1", "no variable")

            assert game.u16("Controls_Actions") == 1  # the Select action mask
            reject(game, "poke @Controls_Actions 00", "read-only")

    print("ARM64 named reads: guest pin, native value, bounds, functions, and read-only protection passed")


if __name__ == "__main__":
    main()
