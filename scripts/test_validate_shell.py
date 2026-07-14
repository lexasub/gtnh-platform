#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import validate_shell as m
from _lib import ROOT

FAKE = [pathlib.Path("fake.sh")]


class ShellStrictMode(unittest.TestCase):
    def test_clean_repo(self) -> None:
        self.assertEqual(m.check_shell_strict_mode(), [])

    def test_catches_missing_set_e(self) -> None:
        with mock.patch.object(m, "tracked", return_value=FAKE), \
             mock.patch.object(m, "read", lambda _p: "#!/bin/bash\nset -uo pipefail\necho hi"):
            self.assertTrue(m.check_shell_strict_mode())

    def test_catches_missing_pipefail(self) -> None:
        with mock.patch.object(m, "tracked", return_value=FAKE), \
             mock.patch.object(m, "read", lambda _p: "#!/bin/bash\nset -eu\necho hi"):
            self.assertTrue(m.check_shell_strict_mode())

    def test_passes_with_both(self) -> None:
        with mock.patch.object(m, "tracked", return_value=FAKE), \
             mock.patch.object(m, "read", lambda _p: "#!/bin/bash\nset -euo pipefail\necho hi"):
            self.assertEqual(m.check_shell_strict_mode(), [])

    def test_no_strict_override(self) -> None:
        with mock.patch.object(m, "tracked", return_value=FAKE), \
             mock.patch.object(m, "read", lambda _p: "#!/bin/bash\n# @no-strict\necho hi"):
            self.assertEqual(m.check_shell_strict_mode(), [])

    def test_empty_file(self) -> None:
        with mock.patch.object(m, "tracked", return_value=FAKE), \
             mock.patch.object(m, "read", lambda _p: ""):
            self.assertEqual(m.check_shell_strict_mode(), [])


if __name__ == "__main__":
    unittest.main()
