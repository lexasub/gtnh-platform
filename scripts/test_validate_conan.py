#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import validate_conan as m


class ConanGuard(unittest.TestCase):
    def test_clean_repo(self) -> None:
        self.assertEqual(m.check_cmake_has_conan(), [])

    def test_catches_missing_conan_entry(self) -> None:
        with mock.patch.object(m, "_conan_packages", return_value={"asio"}), \
             mock.patch.object(m, "_cmake_packages", return_value={"asio", "mysterylib"}):
            problems = m.check_cmake_has_conan()
            self.assertTrue(any("mysterylib" in p for p in problems))

    def test_alias_mapping(self) -> None:
        with mock.patch.object(m, "_conan_packages", return_value={"glfw"}), \
             mock.patch.object(m, "_cmake_packages", return_value={"glfw3"}):
            self.assertEqual(m.check_cmake_has_conan(), [])


if __name__ == "__main__":
    unittest.main()
