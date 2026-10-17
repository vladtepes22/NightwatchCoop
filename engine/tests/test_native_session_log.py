"""Native output survives GUI launches and the previous session remains available."""
from paths import ROOT
from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import run_windows


class NativeSessionLogTests(unittest.TestCase):
    def test_explicit_native_output_and_previous_session(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            old = folder / 'native_session.log'
            old.write_text('previous game output', encoding='utf-8')

            def fake_game(command, **kwargs):
                self.assertEqual(command, ['probe.exe', 'game.bin'])
                self.assertIs(kwargs['stdout'], kwargs['stderr'])
                kwargs['stdout'].write('Party: map changed\nParty perf: tick timings\n')
                return 0

            with patch.dict(os.environ, {'BB_CRASH_DIR': directory}), patch.object(run_windows, 'LOG_STREAM', None), \
                    patch.object(run_windows.subprocess, 'call', side_effect=fake_game), redirect_stdout(io.StringIO()) as output:
                self.assertEqual(run_windows.run_probe(['probe.exe', 'game.bin']), 0)
            self.assertIn('Runtime log:', output.getvalue())
            self.assertIn('Party perf: tick timings', old.read_text(encoding='utf-8'))
            self.assertEqual((folder / 'previous_native_session.log').read_text(encoding='utf-8'), 'previous game output')

    def test_existing_combined_log_is_used(self):
        stream = io.StringIO()
        with patch.object(run_windows, 'LOG_STREAM', stream), patch.object(run_windows.subprocess, 'call', return_value=7) as call:
            self.assertEqual(run_windows.run_probe(['probe.exe']), 7)
            self.assertIs(call.call_args.kwargs['stdout'], stream)
            self.assertIs(call.call_args.kwargs['stderr'], stream)


if __name__ == '__main__':
    unittest.main()
