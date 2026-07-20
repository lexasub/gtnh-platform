#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import validate_ports as m

FAKE = [pathlib.Path("fake.cpp")]


class PortsGuard(unittest.TestCase):
    def test_clean_repo(self) -> None:
        self.assertEqual(m.check_env_ports_documented(), [])

    def test_catches_undocumented_port(self) -> None:
        with mock.patch.object(m, "_env_ports", return_value={"ROUTER_PORT": "4000"}), \
             mock.patch.object(m, "_code_ports", return_value={"4000": ["a.cpp:1"], "9999": ["b.cpp:1"]}):
            problems = m.check_env_ports_documented()
            self.assertTrue(any("9999" in p for p in problems))


if __name__ == "__main__":
    unittest.main()
