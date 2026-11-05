"""tools/latency_digest.py: BB_LATENCY_LOG lines (gpu/shim/bbport_latency.cpp) summed over a run."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[2] / 'tools'
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import latency_digest as ld


def line(n, age, present, flip=None, queue=None):
    text = (f"latency: n={n} pad_age_us avg/p95/max={age}/{age * 2}/{age * 3} "
            f"read_to_present_ms avg/p95/max={present:.2f}/{present * 2:.2f}/{present * 3:.2f} reads={n + 1}")
    if flip is not None:
        text += (f" read_to_flip_ms avg/p95/max={flip:.2f}/{flip:.2f}/{flip:.2f}"
                 f" flip_to_present_ms avg/p95/max={queue:.2f}/{queue * 2:.2f}/{queue * 2:.2f}")
    return text


class LatencyDigestTests(unittest.TestCase):
    def test_skips_warmup_and_short_seconds_takes_worst_tails(self):
        lines = [line(60, a, b) for a, b in [(900, 40), (1000, 20), (1200, 22), (1100, 21)]]
        lines.insert(2, line(3, 1, 1))  # a loading second: fewer than 10 frames
        summary = ld.summarise(ld.parse("\n".join(lines)), skip=1)
        self.assertEqual(summary["seconds"], 3)
        self.assertAlmostEqual(summary["pad_age_us_avg"], 1100.0)
        self.assertAlmostEqual(summary["read_to_present_ms_avg"], 21.0)
        self.assertEqual(summary["pad_age_us_max"], 3600.0)
        self.assertIsNone(ld.summarise([], 0))

    def test_reads_the_engine_line_with_flip_and_queue(self):
        # The exact format bbport_latency.cpp prints, inside a log line with a prefix.
        text = "[gpu] " + line(60, 500, 19.0, flip=12.5, queue=6.5) + "\n" + line(60, 700, 21.0, flip=13.5, queue=7.5)
        summary = ld.summarise(ld.parse(text), skip=0)
        self.assertAlmostEqual(summary["read_to_flip_ms_avg"], 13.0)
        self.assertAlmostEqual(summary["flip_to_present_ms_avg"], 7.0)
        self.assertAlmostEqual(summary["flip_to_present_ms_p95"], 15.0)

    def test_cli_says_how_to_get_lines(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "session.log"
            log.write_text("no probe here\n", encoding="utf-8")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(ld.main([str(log)]), 1)
            self.assertIn("BB_LATENCY_LOG=1", out.getvalue())


if __name__ == "__main__":
    unittest.main()
