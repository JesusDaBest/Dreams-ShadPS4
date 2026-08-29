"""Strict status and control-file handling for the exact replay viewer."""

from __future__ import annotations

import json
import math
import os
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


SCHEMA = 1
STATUS_NAME = "status.json"
IMAGE_NAMES = (
    "candidate_near_a.png",
    "candidate_near_b.png",
    "reference_a.png",
    "reference_b.png",
)
MARKER_NAMES = {
    "start_search": "start-search.request.json",
    "stop_search": "stop-search.request.json",
    "save_winner": "save-winner.request.json",
}


class StatusError(ValueError):
    """The replay status is absent, malformed, or unsafe to use."""


class ActionRefused(RuntimeError):
    """A control action is not valid for the reported replay state."""


@dataclass(frozen=True)
class CandidateStatus:
    label: str
    score: float | None
    settings: dict[str, Any]


@dataclass(frozen=True)
class WinnerStatus:
    saved: bool
    count: int
    path: Path | None


@dataclass(frozen=True)
class ReplayStatus:
    root: Path
    parity_verified: bool
    search_running: bool
    candidate: CandidateStatus
    winner: WinnerStatus
    message: str

    @property
    def can_start_search(self) -> bool:
        return self.parity_verified and not self.search_running

    @property
    def can_stop_search(self) -> bool:
        return self.search_running

    @property
    def can_save_winner(self) -> bool:
        return self.parity_verified and bool(self.candidate.label)


def _require_object(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise StatusError(f"{label} must be a JSON object")
    return value


def _require_bool(value: Any, label: str) -> bool:
    if type(value) is not bool:
        raise StatusError(f"{label} must be true or false")
    return value


def _require_string(value: Any, label: str) -> str:
    if not isinstance(value, str):
        raise StatusError(f"{label} must be a string")
    return value


def _resolve_inside(root: Path, relative: str, label: str) -> Path:
    if not relative:
        raise StatusError(f"{label} must not be empty")
    candidate = (root / relative).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as exc:
        raise StatusError(f"{label} escapes the replay directory") from exc
    return candidate


def resolve_replay_root(argument: str | Path) -> Path:
    """Accept a replay directory or its exact status.json path."""
    path = Path(argument).expanduser().resolve()
    if path.name.lower() == STATUS_NAME:
        root = path.parent
    elif path.suffix.lower() == ".json":
        raise StatusError(f"expected a replay directory or a file named {STATUS_NAME}")
    else:
        root = path
    if not root.is_dir():
        raise StatusError(f"replay directory does not exist: {root}")
    return root


def image_paths(root: str | Path) -> dict[str, Path]:
    root_path = Path(root).resolve()
    return {name: (root_path / name) for name in IMAGE_NAMES}


def load_status(argument: str | Path) -> ReplayStatus:
    root = resolve_replay_root(argument)
    status_path = root / STATUS_NAME
    if not status_path.is_file():
        raise StatusError(f"missing {STATUS_NAME}: {status_path}")
    try:
        document = json.loads(status_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise StatusError(f"cannot read {STATUS_NAME}: {exc}") from exc
    document = _require_object(document, "status")

    schema = document.get("schema")
    if type(schema) is not int or schema != SCHEMA:
        raise StatusError(f"status.schema must be {SCHEMA}")
    parity_verified = _require_bool(document.get("parity_verified"), "status.parity_verified")
    search_running = _require_bool(document.get("search_running", False), "status.search_running")

    candidate_value = _require_object(document.get("candidate"), "status.candidate")
    candidate_label = _require_string(candidate_value.get("label"), "status.candidate.label")
    candidate_score_value = candidate_value.get("score")
    if candidate_score_value is None:
        candidate_score = None
    elif isinstance(candidate_score_value, bool) or not isinstance(candidate_score_value, (int, float)):
        raise StatusError("status.candidate.score must be a finite number or null")
    else:
        candidate_score = float(candidate_score_value)
        if not math.isfinite(candidate_score):
            raise StatusError("status.candidate.score must be finite")
    candidate_settings = _require_object(candidate_value.get("settings"), "status.candidate.settings")

    winner_value = _require_object(document.get("winner", {}), "status.winner")
    winner_saved = _require_bool(winner_value.get("saved", False), "status.winner.saved")
    winner_count = winner_value.get("count", 0)
    if type(winner_count) is not int or winner_count < 0:
        raise StatusError("status.winner.count must be a non-negative integer")
    winner_path_value = winner_value.get("path")
    if winner_path_value is None:
        winner_path = None
    else:
        winner_path = _resolve_inside(
            root, _require_string(winner_path_value, "status.winner.path"), "status.winner.path"
        )
    if winner_saved and winner_path is None:
        raise StatusError("status.winner.path is required when status.winner.saved is true")

    message = _require_string(document.get("message", ""), "status.message")
    return ReplayStatus(
        root=root,
        parity_verified=parity_verified,
        search_running=search_running,
        candidate=CandidateStatus(candidate_label, candidate_score, dict(candidate_settings)),
        winner=WinnerStatus(winner_saved, winner_count, winner_path),
        message=message,
    )


def _check_action(status: ReplayStatus, action: str) -> None:
    if action not in MARKER_NAMES:
        raise ActionRefused(f"unknown replay action: {action}")
    if action == "start_search" and not status.can_start_search:
        if not status.parity_verified:
            raise ActionRefused("search is locked until exact replay parity is verified")
        raise ActionRefused("search is already running")
    if action == "stop_search" and not status.can_stop_search:
        raise ActionRefused("search is not running")
    if action == "save_winner" and not status.can_save_winner:
        if not status.parity_verified:
            raise ActionRefused("winner saving is locked until exact replay parity is verified")
        raise ActionRefused("there is no current candidate to save")


def write_action_marker(status: ReplayStatus, action: str) -> Path:
    """Atomically publish a request; never pretend the renderer accepted it."""
    _check_action(status, action)
    control_dir = status.root / "control"
    control_dir.mkdir(parents=True, exist_ok=True)
    marker_path = control_dir / MARKER_NAMES[action]
    temporary_path = control_dir / f".{MARKER_NAMES[action]}.{os.getpid()}.{time.time_ns()}.tmp"
    payload = {
        "schema": SCHEMA,
        "action": action,
        "requested_at_unix_ns": time.time_ns(),
        "candidate_label": status.candidate.label,
    }
    try:
        temporary_path.write_text(
            json.dumps(payload, sort_keys=True, separators=(",", ":")) + "\n",
            encoding="utf-8",
        )
        os.replace(temporary_path, marker_path)
    finally:
        try:
            temporary_path.unlink()
        except FileNotFoundError:
            pass
    return marker_path
