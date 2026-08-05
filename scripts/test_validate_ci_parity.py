#!/usr/bin/env python3
"""Unit tests for validate_ci_parity.py — both directions:
   catches a parity violation AND does not false-positive on the real repo.
"""
from __future__ import annotations

import pathlib
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import validate_ci_parity as m


class PreCommitParity(unittest.TestCase):
    def test_clean_repo(self) -> None:
        self.assertEqual(m.check_precommit_parity(), [])

    def test_catches_validator_not_wired(self) -> None:
        with mock.patch.object(m, "_validator_names", return_value=["validate_ghost.py"]):
            problems = m.check_precommit_parity()
            self.assertTrue(any("validate_ghost.py" in p for p in problems))

    def test_catches_missing_precommit_file(self) -> None:
        with mock.patch.object(m, "PRE_COMMIT", m.ROOT / "nope-missing.yml"):
            self.assertTrue(m.check_precommit_parity())

    def test_catches_precommit_referencing_missing_script(self) -> None:
        fake = m.ROOT / ".pre-commit-config.yaml"
        cfg = "\n".join(
            line for line in fake.read_text().splitlines()
            if "python3 scripts/" not in line
        ) + "\n        entry: python3 scripts/validate_nope.py\n"
        with mock.patch.object(m, "_validator_names", return_value=[]), \
             mock.patch.object(m, "read", lambda _p: cfg):
            problems = m.check_precommit_parity()
            self.assertTrue(any("validate_nope.py" in p for p in problems))


class GuardTests(unittest.TestCase):
    def test_clean_repo(self) -> None:
        self.assertEqual(m.check_guard_tests(), [])

    def test_catches_missing_test(self) -> None:
        with mock.patch.object(m, "_validator_names", return_value=["validate_ghost.py"]):
            problems = m.check_guard_tests()
            self.assertTrue(any("validate_ghost.py" in p for p in problems))


class CiDelegation(unittest.TestCase):
    def test_clean_repo(self) -> None:
        self.assertEqual(m.check_ci_delegates_to_gate(), [])

    def test_catches_reimplementation_without_gate(self) -> None:
        body = "name: X\nsteps:\n  - run: cmake --build build -j 4\n  - run: ctest\n"
        with mock.patch.object(m, "WORKFLOWS", [pathlib.Path("workflow.yml")]), \
             mock.patch.object(m, "read", lambda _p: body):
            self.assertTrue(m.check_ci_delegates_to_gate())

    def test_allows_delegating_workflow(self) -> None:
        body = "name: X\nsteps:\n  - run: bash ci/check.sh cpp\n"
        with mock.patch.object(m, "WORKFLOWS", [pathlib.Path("workflow.yml")]), \
             mock.patch.object(m, "read", lambda _p: body):
            self.assertEqual(m.check_ci_delegates_to_gate(), [])


if __name__ == "__main__":
    unittest.main()
