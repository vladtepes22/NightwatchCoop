"""Files written whole or not at all (bbport.ini, launcher_settings.json, party.json ...).

A temporary file beside the target, flushed and fsynced, then os.replace: a full drive or a crash halfway
leaves the old file as it was, and a failed write removes its temporary file. A file another program has
open for a moment (an antivirus, a sync tool, the game reading it) is tried again over about a second.
The target is only ever replaced, never opened for writing: a read-only target keeps its old content
(PermissionError on Windows) and the caller decides what to do.

Idea and retry wording from Supermedo's launcher (launcher/bbport_textfile.py, commits 72097e6e and
f66abc20 of Supermedo/bloodborne_pc, GPL-2.0-or-later, author nick24051998); rewritten for this tree.
"""
import os
import time
from pathlib import Path

REPLACE_TRIES = 20
REPLACE_WAIT = 0.05


def _write(path, content, mode, encoding, newline=None):
    path = Path(path)
    temp = path.with_name(path.name + '.tmp')
    try:
        with open(temp, mode, encoding=encoding, newline=newline) as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        for attempt in range(1, REPLACE_TRIES + 1):
            try:
                os.replace(temp, path)
                return
            except PermissionError:
                if attempt == REPLACE_TRIES:
                    raise
                time.sleep(REPLACE_WAIT)
    except BaseException:
        try:
            temp.unlink()
        except OSError:
            pass
        raise


def write_text(path, text, encoding='utf-8', newline=None):
    """The whole text or nothing. `newline` as open(): '' keeps the text's line ends as given."""
    _write(path, text, 'w', encoding, newline)


def write_bytes(path, data):
    """write_text() for a file that is no text."""
    _write(path, data, 'wb', None)
