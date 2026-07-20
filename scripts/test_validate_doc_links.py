#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import validate_doc_links as m

FAKE = [pathlib.Path("docs/README.md")]


class DocLinksGuard(unittest.TestCase):
    def test_clean_repo(self) -> None:
        self.assertEqual(m.check_doc_links(), [])

    def test_catches_broken_link(self) -> None:
        with mock.patch.object(m, "tracked", return_value=FAKE), \
             mock.patch.object(m, "read", lambda _p: "see [here](nonexistent.md) for details"):
            self.assertTrue(m.check_doc_links())

    def test_passes_valid_link(self) -> None:
        with mock.patch.object(m, "tracked", return_value=FAKE), \
             mock.patch.object(m, "read", lambda _p: "see [here](https://example.com) for details"):
            self.assertEqual(m.check_doc_links(), [])

    def test_ignores_anchors(self) -> None:
        with mock.patch.object(m, "tracked", return_value=FAKE), \
             mock.patch.object(m, "read", lambda _p: "see [here](#section) for details"):
            self.assertEqual(m.check_doc_links(), [])


if __name__ == "__main__":
    unittest.main()
