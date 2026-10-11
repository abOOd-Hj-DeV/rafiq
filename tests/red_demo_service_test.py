import importlib.util
import pathlib
import sys
import unittest
from unittest.mock import patch

TOOLS = pathlib.Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location("red_demo_service", TOOLS / "k230_red_demo.py")
demo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(demo)


class RedDemoServiceTest(unittest.TestCase):
    def event(self, **changes):
        return {"event": "red", "pts_us": 42, "red_fraction": 0.80,
                "detected_at_us": 1000, **changes}

    def test_fresh_red_only(self):
        self.assertTrue(demo.eligible_event(self.event(), 1000))
        self.assertTrue(demo.eligible_event(self.event(), 501000))
        self.assertFalse(demo.eligible_event(self.event(), 501001))
        self.assertFalse(demo.eligible_event(self.event(), 999))
        for changes in ({"event": "ready"}, {"red_fraction": 0.799},
                        {"red_fraction": float("nan")}, {"red_fraction": 1.1},
                        {"pts_us": -1}, {"pts_us": True}, {"detected_at_us": None}):
            self.assertFalse(demo.eligible_event(self.event(**changes), 1000))
        self.assertFalse(demo.eligible_event(None, 1000))

    def test_target_is_fixed(self):
        self.assertEqual(demo.TARGET, "dev.k230.red_demo")

    def test_wrong_or_multiple_phones_fail_before_starting_capture(self):
        for response in ("List of devices attached\nother device\n",
                         "List of devices attached\nexpected device\nother device\n"):
            with patch("builtins.open", unittest.mock.mock_open()), \
                 patch.object(demo.fcntl, "flock"), patch.object(demo.PID.__class__, "write_text"), \
                 patch.object(demo.PID.__class__, "unlink"), patch.object(demo.signal, "signal"), \
                 patch.object(demo, "little", return_value=response), \
                 patch.object(demo, "stop_little_bridge"), patch.object(demo, "stop_inspector"), \
                 patch.object(demo.subprocess, "Popen") as launch:
                with self.assertRaisesRegex(RuntimeError, "explicitly selected"):
                    demo.run("expected")
                launch.assert_not_called()


if __name__ == "__main__":
    unittest.main()
