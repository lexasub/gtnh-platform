#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import validate_forbidden as m
from _lib import iter_code_cstyle

FAKE = [pathlib.Path("fake.cpp")]


class ForbiddenTokens(unittest.TestCase):
    def test_clean_repo(self) -> None:
        self.assertEqual(m.check_forbidden_tokens(), [])

    def test_catches_console_log(self) -> None:
        with mock.patch.object(m, "tracked", return_value=FAKE), \
             mock.patch.object(m, "read", lambda _p: 'int x = 1;\nconsole.log(x);'):
            self.assertTrue(m.check_forbidden_tokens())

    def test_catches_eval(self) -> None:
        with mock.patch.object(m, "tracked", return_value=FAKE), \
             mock.patch.object(m, "read", lambda _p: 'eval("code");'):
            self.assertTrue(m.check_forbidden_tokens())

    def test_no_false_positive_in_comment(self) -> None:
        with mock.patch.object(m, "tracked", return_value=FAKE), \
             mock.patch.object(m, "read", lambda _p: '// never use console.log here\nint x = 1;'):
            self.assertEqual(m.check_forbidden_tokens(), [])

    def test_no_false_positive_in_block_comment(self) -> None:
        with mock.patch.object(m, "tracked", return_value=FAKE), \
             mock.patch.object(m, "read", lambda _p: '/* eval is bad */\nint x = 1;'):
            self.assertEqual(m.check_forbidden_tokens(), [])


class CStyleStripper(unittest.TestCase):
    @staticmethod
    def _blanked(src: str) -> str:
        return "\n".join(code for _, code in iter_code_cstyle(src))

    def test_line_comment_blanked(self) -> None:
        self.assertNotIn("console.log", self._blanked("int x = 1; // never console.log here"))

    def test_block_comment_blanked(self) -> None:
        self.assertNotIn("eval", self._blanked("/* eval is bad */ int y = 2;"))

    def test_string_literal_kept(self) -> None:
        self.assertIn("eval", self._blanked('const char* s = "eval";'))


if __name__ == "__main__":
    unittest.main()
