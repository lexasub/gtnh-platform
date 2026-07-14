#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import validate_headers as m

FAKE = [pathlib.Path("src/fake.h")]


class PragmaOnceGuard(unittest.TestCase):
    def test_clean_repo(self) -> None:
        self.assertEqual(m.check_pragma_once(), [])

    def test_catches_ifndef_guard(self) -> None:
        src = "#ifndef FOO_H\n#define FOO_H\n#endif\n"
        with mock.patch.object(m, "tracked", return_value=FAKE), \
             mock.patch.object(m, "read", lambda _p: src):
            problems = m.check_pragma_once()
            self.assertTrue(any("#ifndef" in p for p in problems))

    def test_passes_pragma_once(self) -> None:
        with mock.patch.object(m, "tracked", return_value=FAKE), \
             mock.patch.object(m, "read", lambda _p: "#pragma once\nint x;"):
            self.assertEqual(m.check_pragma_once(), [])


class UsingNamespaceStdGuard(unittest.TestCase):
    def test_clean_repo(self) -> None:
        self.assertEqual(m.check_no_using_namespace_std(), [])

    def test_catches_using_ns_std(self) -> None:
        with mock.patch.object(m, "tracked", return_value=FAKE), \
             mock.patch.object(m, "read", lambda _p: "using namespace std;\nint x;"):
            self.assertTrue(m.check_no_using_namespace_std())

    def test_no_false_positive_in_comment(self) -> None:
        with mock.patch.object(m, "tracked", return_value=FAKE), \
             mock.patch.object(m, "read", lambda _p: "// using namespace std is bad\nint x;"):
            self.assertEqual(m.check_no_using_namespace_std(), [])


if __name__ == "__main__":
    unittest.main()
