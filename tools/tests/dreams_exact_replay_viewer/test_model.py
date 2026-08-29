from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from model import (  # noqa: E402
    ActionRefused,
    StatusError,
    load_status,
    resolve_replay_root,
    write_action_marker,
)


def status_document(*, parity: bool, running: bool = False, label: str = "candidate-1") -> dict:
    return {
        "schema": 1,
        "parity_verified": parity,
        "search_running": running,
        "candidate": {"label": label, "score": 0.25, "settings": {"gather_winding": "native"}},
        "winner": {"saved": False, "count": 0, "path": None},
        "message": "ready",
    }


class StatusTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_status(self, document: dict) -> Path:
        path = self.root / "status.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        return path

    def test_directory_and_exact_status_path_resolve_to_same_root(self) -> None:
        status_path = self.write_status(status_document(parity=False))
        self.assertEqual(resolve_replay_root(self.root), self.root)
        self.assertEqual(resolve_replay_root(status_path), self.root)

    def test_other_json_name_is_refused(self) -> None:
        other = self.root / "other.json"
        other.write_text("{}", encoding="utf-8")
        with self.assertRaisesRegex(StatusError, "status.json"):
            resolve_replay_root(other)

    def test_unverified_parity_disables_search_and_save(self) -> None:
        self.write_status(status_document(parity=False))
        status = load_status(self.root)
        self.assertFalse(status.can_start_search)
        self.assertFalse(status.can_save_winner)
        with self.assertRaisesRegex(ActionRefused, "parity"):
            write_action_marker(status, "start_search")
        self.assertFalse((self.root / "control").exists())

    def test_verified_parity_enables_search_and_writes_atomic_marker(self) -> None:
        self.write_status(status_document(parity=True))
        status = load_status(self.root)
        self.assertTrue(status.can_start_search)
        marker = write_action_marker(status, "start_search")
        self.assertEqual(marker, self.root / "control" / "start-search.request.json")
        payload = json.loads(marker.read_text(encoding="utf-8"))
        self.assertEqual(payload["action"], "start_search")
        self.assertEqual(payload["candidate_label"], "candidate-1")
        self.assertEqual(list(marker.parent.glob("*.tmp")), [])

    def test_running_search_only_enables_stop(self) -> None:
        self.write_status(status_document(parity=True, running=True))
        status = load_status(self.root)
        self.assertFalse(status.can_start_search)
        self.assertTrue(status.can_stop_search)
        marker = write_action_marker(status, "stop_search")
        self.assertEqual(marker.name, "stop-search.request.json")

    def test_parity_must_be_an_explicit_boolean(self) -> None:
        document = status_document(parity=False)
        document["parity_verified"] = 1
        self.write_status(document)
        with self.assertRaisesRegex(StatusError, "true or false"):
            load_status(self.root)

    def test_winner_path_cannot_escape_replay_directory(self) -> None:
        document = status_document(parity=True)
        document["winner"] = {"saved": True, "count": 1, "path": "../outside"}
        self.write_status(document)
        with self.assertRaisesRegex(StatusError, "escapes"):
            load_status(self.root)

    def test_non_finite_score_is_refused(self) -> None:
        document = status_document(parity=True)
        document["candidate"]["score"] = float("nan")
        self.write_status(document)
        with self.assertRaisesRegex(StatusError, "finite"):
            load_status(self.root)


if __name__ == "__main__":
    unittest.main()
