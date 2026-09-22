"""What the gates need from the machine they run on.

The DSP assembler, vasm/vlink and TOS 4.02 come from an F030MXDRV checkout;
the DSP-calibrated Hatari and TOS 4.04 from an F030Arcade one. Either checkout
may sit under ~/Work or beside this repository, and a Windows host configures
Hatari as build-ucrt64 and links hatari.exe, so every spelling is searched,
as F030MXDRV's own tools/hatari_binary.py does. The environment variables
MXDRV, F030ARCADE and HATARI override the search.

A Windows checkout with core.autocrlf holds text sources with CRLF line ends,
so a record's source hashes are taken over LF line ends: they then name the
committed files on any host.
"""
import hashlib
import os
from pathlib import Path

_REPOSITORY = Path(__file__).resolve().parents[4]
_HATARI_BUILDS = ("build", "build-ucrt64")
_HATARI_NAMES = ("hatari", "hatari.exe")


def _checkout(variable, name):
    override = os.environ.get(variable)
    if override:
        return Path(override)
    candidates = (Path.home() / "Work" / name, _REPOSITORY.parent / name)
    for candidate in candidates:
        if candidate.is_dir():
            return candidate
    return candidates[0]


MXDRV = _checkout("MXDRV", "F030MXDRV")
ARCADE = _checkout("F030ARCADE", "F030Arcade")


def _hatari():
    override = os.environ.get("HATARI")
    if override:
        return Path(override)
    for build in _HATARI_BUILDS:
        for name in _HATARI_NAMES:
            candidate = ARCADE / "third_party/hatari" / build / "src" / name
            if candidate.is_file():
                return candidate
    return ARCADE / "third_party/hatari/build/src/hatari"


HATARI = _hatari()
VASM = MXDRV / "build/tools/vasm/vasmm68k_mot"
VLINK = MXDRV / "build/tools/vlink/vlink"
# The kernel benches boot TOS 4.02; the game needs 4.04, since 4.02 dies in
# its video mode switch.
TOS402 = MXDRV / "third_party/f030dsp3d/tools/tos402.rom"
TOS404 = ARCADE / "third_party/tos/tos404.img"


# Hatari's control FIFO needs Unix domain sockets, which a Windows build of it
# lacks, so a gate that drives the emulator from outside cannot there.
CONTROL_FIFO = os.name != "nt"


def link_directory(link, target):
    """A directory link; on Windows a junction, which needs no privilege.
    Remove a junction with unlink_directory once done: a recursive delete of
    the directory holding it may follow it into the target."""
    if os.name == "nt":
        import _winapi
        _winapi.CreateJunction(str(target), str(link))
    else:
        Path(link).symlink_to(target, target_is_directory=True)


def unlink_directory(link):
    """Removes a link link_directory made on Windows, never its target."""
    if os.name == "nt" and os.path.lexists(link):
        os.rmdir(link)


def program(path):
    """A host program as built: MinGW links name.exe."""
    path = Path(path)
    if not path.is_file() and path.with_name(path.name + ".exe").is_file():
        return path.with_name(path.name + ".exe")
    return path


def source_sha256(path):
    """The SHA-256 of a text source as committed, whatever its line ends here."""
    return hashlib.sha256(Path(path).read_bytes().replace(b"\r\n", b"\n")).hexdigest()
