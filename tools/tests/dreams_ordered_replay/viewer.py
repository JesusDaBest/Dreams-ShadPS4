#!/usr/bin/env python3
"""One-at-a-time viewer for real VS370 capture candidates.

The window draws only positions and indices present in the selected capture.
It does not synthesize a sculpt surface or approximate missing flecks.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import json
import math
import tkinter as tk
from pathlib import Path
from tkinter import messagebox
from typing import Sequence

from aligned_capture import CandidateFrame, build_candidate_frames, load_aligned_capture
from ordered_replay import (
    ALIGNED_CHAIN_REQUIRED,
    ReferenceMappingResult,
    Vs370Capture,
    file_sha256,
    load_vs370_capture,
)


COLORS = (
    "#176b87",
    "#b05a24",
    "#477d31",
    "#7252a8",
    "#b43c64",
    "#607078",
)


def winner_document(capture: Vs370Capture, candidate: ReferenceMappingResult) -> dict[str, object]:
    source_names = (
        "manifest.tsv",
        "draw.bin",
        "indices.bin",
        "b3-list.bin",
        "b0-object-keys.bin",
        "vs-interface.bin",
        "vs-validity.bin",
    )
    return {
        "schema": 1,
        "accepted_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "capture": str(capture.directory),
        "scope": "VS370 downstream reference-field interpretation",
        "candidate": dataclasses.asdict(candidate.mapping),
        "decoded_references": [f"0x{value:x}" for value in candidate.references],
        "invalid_references": candidate.invalid,
        "duplicate_references": candidate.duplicate_refs,
        "spatial_metrics": capture.spatial_metrics(),
        "source_sha256": {
            name: file_sha256(capture.directory / name) for name in source_names
        },
        "not_proven": (
            "This choice does not select d8/7aa ordered-count semantics. That requires an aligned "
            "upstream capture containing the files reported by the inventory command."
        ),
    }


def write_winner(path: Path, document: dict[str, object]) -> None:
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def aligned_winner_document(capture, frame: CandidateFrame) -> dict[str, object]:
    return {
        "schema": 1,
        "accepted_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "capture": str(capture.directory),
        "scope": "aligned d8 to visibility-list to VS370 reconstruction",
        "candidate_name": frame.name,
        "configuration": frame.configuration,
        "diagnostics": frame.diagnostics,
        "record_sequence": [list(pair.as_hex()) for pair in frame.pairs],
        "source_sha256": {
            name: file_sha256(capture.directory / name) for name in ALIGNED_CHAIN_REQUIRED
        },
        "render_limit": (
            "The viewer uses captured post-VS geometry and captured indices. It does not emulate "
            "fragment shading, fleck textures, blending, or depth precision."
        ),
    }


def _triangle_edges(indices: Sequence[int], vertex_count: int) -> tuple[tuple[int, int], ...]:
    """Decode the captured 14-index cube strip into unique wireframe edges."""

    edges: set[tuple[int, int]] = set()
    for offset in range(2, len(indices)):
        triangle = (indices[offset - 2], indices[offset - 1], indices[offset])
        if len(set(triangle)) != 3 or any(index >= vertex_count for index in triangle):
            continue
        for left, right in ((triangle[0], triangle[1]), (triangle[1], triangle[2]), (triangle[2], triangle[0])):
            edges.add(tuple(sorted((left, right))))
    return tuple(sorted(edges))


class _FixedVsDiagnosticWindow:
    """Disabled: fixed post-VS positions cannot choose upstream semantics."""

    def __init__(self, capture: Vs370Capture, winner_path: Path) -> None:
        raise RuntimeError(
            "VS-only captures cannot drive the Yes/No visual chooser; use --list for diagnostics "
            "or provide the exact aligned files reported by the capture loader"
        )
        self.capture = capture
        self.winner_path = winner_path
        self.candidates = capture.reference_candidates()
        if not self.candidates:
            raise ValueError("the capture produced no finite reference-field candidates")
        spatial = capture.spatial_metrics()
        if spatial["invalid_vertices"]:
            raise ValueError(
                f"cannot render {capture.directory / 'vs-interface.bin'}: "
                f"{spatial['invalid_vertices']} captured vertices are invalid"
            )
        self.index = 0
        self.root = tk.Tk()
        self.root.title("Dreams capture check")
        self.root.minsize(820, 680)
        self.root.configure(bg="#f3f0e8")

        self.heading = tk.Label(
            self.root,
            text="Does this captured reconstruction match?",
            font=("Segoe UI", 18, "bold"),
            bg="#f3f0e8",
            fg="#1c2930",
        )
        self.heading.pack(pady=(18, 2))
        self.explanation = tk.Label(
            self.root,
            text=(
                "Only the positions and index order saved by the capture are drawn. "
                "No sculpt geometry or surface detail is invented."
            ),
            font=("Segoe UI", 10),
            bg="#f3f0e8",
            fg="#394951",
            wraplength=740,
        )
        self.explanation.pack(pady=(0, 10))
        self.canvas = tk.Canvas(
            self.root,
            width=760,
            height=460,
            bg="#d8e5ea",
            highlightthickness=1,
            highlightbackground="#95a7ad",
        )
        self.canvas.pack(padx=24)
        self.option_label = tk.Label(
            self.root,
            font=("Segoe UI", 12, "bold"),
            bg="#f3f0e8",
            fg="#1c2930",
        )
        self.option_label.pack(pady=(12, 2))
        self.detail_label = tk.Label(
            self.root,
            font=("Segoe UI", 10),
            bg="#f3f0e8",
            fg="#394951",
            wraplength=740,
        )
        self.detail_label.pack()
        button_row = tk.Frame(self.root, bg="#f3f0e8")
        button_row.pack(pady=16)
        self.no_button = tk.Button(
            button_row,
            text="No — show the next one",
            command=self.reject_current,
            font=("Segoe UI", 11),
            padx=18,
            pady=7,
        )
        self.no_button.pack(side=tk.LEFT, padx=8)
        self.yes_button = tk.Button(
            button_row,
            text="Yes — save this one",
            command=self.accept_current,
            font=("Segoe UI", 11, "bold"),
            padx=18,
            pady=7,
            bg="#2d7b55",
            fg="white",
            activebackground="#256848",
        )
        self.yes_button.pack(side=tk.LEFT, padx=8)
        self.draw_current()

    def _projected_points(self) -> list[list[tuple[float, float]]]:
        projected: list[list[tuple[float, float]]] = []
        for instance in self.capture.positions():
            points: list[tuple[float, float]] = []
            for x, y, _z, w in instance:
                if w == 0.0 or not all(math.isfinite(value) for value in (x, y, w)):
                    raise ValueError("captured VS position contains a non-projectable vertex")
                points.append((x / w, y / w))
            projected.append(points)
        return projected

    def draw_current(self) -> None:
        candidate = self.candidates[self.index]
        self.canvas.delete("all")
        projected = self._projected_points()
        flat = [point for instance in projected for point in instance]
        minimum_x = min(point[0] for point in flat)
        maximum_x = max(point[0] for point in flat)
        minimum_y = min(point[1] for point in flat)
        maximum_y = max(point[1] for point in flat)
        width = max(maximum_x - minimum_x, 1e-9)
        height = max(maximum_y - minimum_y, 1e-9)
        scale = min(680.0 / width, 380.0 / height)
        origin_x = (760.0 - width * scale) / 2.0
        origin_y = (460.0 - height * scale) / 2.0

        def screen(point: tuple[float, float]) -> tuple[float, float]:
            return (
                origin_x + (point[0] - minimum_x) * scale,
                460.0 - (origin_y + (point[1] - minimum_y) * scale),
            )

        edges = _triangle_edges(self.capture.indices, self.capture.vertices_per_instance)
        for instance_index, points in enumerate(projected):
            reference = candidate.references[instance_index]
            valid = reference in self.capture.object_keys
            color = COLORS[reference % len(COLORS)] if valid else "#c62828"
            for left, right in edges:
                x1, y1 = screen(points[left])
                x2, y2 = screen(points[right])
                self.canvas.create_line(x1, y1, x2, y2, fill=color, width=3 if valid else 2)
            center_x = sum(screen(point)[0] for point in points) / len(points)
            center_y = sum(screen(point)[1] for point in points) / len(points)
            label = f"{instance_index + 1}" if valid else f"{instance_index + 1}?"
            self.canvas.create_text(
                center_x,
                center_y,
                text=label,
                fill="#17242a" if valid else "#8b1515",
                font=("Segoe UI", 11, "bold"),
            )

        self.option_label.configure(text=f"Option {self.index + 1} of {len(self.candidates)}")
        matching = len(candidate.references) - candidate.invalid
        self.detail_label.configure(
            text=(
                f"{matching} of {len(candidate.references)} captured references match the captured object list. "
                f"Red outlines with a ? are unmatched."
            )
        )

    def reject_current(self) -> None:
        if self.index + 1 >= len(self.candidates):
            self.no_button.configure(state=tk.DISABLED)
            messagebox.showinfo(
                "No more options",
                "Every captured interpretation was rejected. Nothing was saved.",
                parent=self.root,
            )
            return
        self.index += 1
        self.draw_current()

    def accept_current(self) -> None:
        candidate = self.candidates[self.index]
        write_winner(self.winner_path, winner_document(self.capture, candidate))
        messagebox.showinfo(
            "Saved",
            f"This choice was saved to:\n{self.winner_path.resolve()}",
            parent=self.root,
        )
        self.root.destroy()

    def run(self) -> None:
        self.root.mainloop()


class AlignedCandidateViewer:
    """Shows only candidate-specific geometry derived from an aligned capture."""

    def __init__(self, capture, frames: Sequence[CandidateFrame], winner_path: Path) -> None:
        if not frames:
            raise ValueError("no candidate preserved enough invariants and captured geometry to display")
        self.capture = capture
        self.frames = tuple(frames)
        self.winner_path = winner_path
        self.index = 0
        self.root = tk.Tk()
        self.root.title("Dreams shape check")
        self.root.minsize(820, 680)
        self.root.configure(bg="#f3f0e8")
        tk.Label(
            self.root,
            text="Does this reconstruction match?",
            font=("Segoe UI", 18, "bold"),
            bg="#f3f0e8",
            fg="#1c2930",
        ).pack(pady=(18, 2))
        tk.Label(
            self.root,
            text=(
                "Every line below comes from candidate-specific captured vertex data. "
                "Surface shading is not approximated."
            ),
            font=("Segoe UI", 10),
            bg="#f3f0e8",
            fg="#394951",
            wraplength=740,
        ).pack(pady=(0, 10))
        self.canvas = tk.Canvas(
            self.root,
            width=760,
            height=460,
            bg="#d8e5ea",
            highlightthickness=1,
            highlightbackground="#95a7ad",
        )
        self.canvas.pack(padx=24)
        self.option_label = tk.Label(
            self.root,
            font=("Segoe UI", 12, "bold"),
            bg="#f3f0e8",
            fg="#1c2930",
        )
        self.option_label.pack(pady=(12, 2))
        self.detail_label = tk.Label(
            self.root,
            font=("Segoe UI", 10),
            bg="#f3f0e8",
            fg="#394951",
            wraplength=740,
        )
        self.detail_label.pack()
        button_row = tk.Frame(self.root, bg="#f3f0e8")
        button_row.pack(pady=16)
        self.no_button = tk.Button(
            button_row,
            text="No — show the next one",
            command=self.reject_current,
            font=("Segoe UI", 11),
            padx=18,
            pady=7,
        )
        self.no_button.pack(side=tk.LEFT, padx=8)
        tk.Button(
            button_row,
            text="Yes — save this one",
            command=self.accept_current,
            font=("Segoe UI", 11, "bold"),
            padx=18,
            pady=7,
            bg="#2d7b55",
            fg="white",
            activebackground="#256848",
        ).pack(side=tk.LEFT, padx=8)
        self.draw_current()

    @staticmethod
    def _project(frame: CandidateFrame) -> list[list[tuple[float, float]]]:
        projected: list[list[tuple[float, float]]] = []
        for instance in frame.positions():
            points = []
            for x, y, _z, w in instance:
                if w == 0.0 or not all(math.isfinite(value) for value in (x, y, w)):
                    raise ValueError("candidate-interface.bin contains a non-projectable position")
                points.append((x / w, y / w))
            projected.append(points)
        return projected

    def draw_current(self) -> None:
        frame = self.frames[self.index]
        self.canvas.delete("all")
        projected = self._project(frame)
        flat = [point for instance in projected for point in instance]
        if not flat:
            raise ValueError("the candidate has no captured positions")
        min_x = min(point[0] for point in flat)
        max_x = max(point[0] for point in flat)
        min_y = min(point[1] for point in flat)
        max_y = max(point[1] for point in flat)
        width = max(max_x - min_x, 1e-9)
        height = max(max_y - min_y, 1e-9)
        scale = min(680.0 / width, 380.0 / height)
        origin_x = (760.0 - width * scale) / 2.0
        origin_y = (460.0 - height * scale) / 2.0

        def screen(point: tuple[float, float]) -> tuple[float, float]:
            return (
                origin_x + (point[0] - min_x) * scale,
                460.0 - (origin_y + (point[1] - min_y) * scale),
            )

        edges = _triangle_edges(frame.indices, frame.vertices_per_instance)
        for draw_ordinal, (pair, points) in enumerate(zip(frame.pairs, projected), 1):
            color = COLORS[pair.low24_ref % len(COLORS)]
            for left, right in edges:
                x1, y1 = screen(points[left])
                x2, y2 = screen(points[right])
                self.canvas.create_line(x1, y1, x2, y2, fill=color, width=3)
            center_x = sum(screen(point)[0] for point in points) / len(points)
            center_y = sum(screen(point)[1] for point in points) / len(points)
            self.canvas.create_text(
                center_x,
                center_y,
                text=str(draw_ordinal),
                fill="#17242a",
                font=("Segoe UI", 9, "bold"),
            )
        self.option_label.configure(text=f"Option {self.index + 1} of {len(self.frames)}")
        self.detail_label.configure(
            text=f"Reconstructed {len(frame.pairs)} captured pieces. No means show exactly one next option."
        )

    def reject_current(self) -> None:
        if self.index + 1 >= len(self.frames):
            self.no_button.configure(state=tk.DISABLED)
            messagebox.showinfo(
                "No more options",
                "Every distinct reconstruction was rejected. Nothing was saved.",
                parent=self.root,
            )
            return
        self.index += 1
        self.draw_current()

    def accept_current(self) -> None:
        frame = self.frames[self.index]
        write_winner(self.winner_path, aligned_winner_document(self.capture, frame))
        messagebox.showinfo(
            "Saved",
            f"This choice was saved to:\n{self.winner_path.resolve()}",
            parent=self.root,
        )
        self.root.destroy()

    def run(self) -> None:
        self.root.mainloop()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True, help="aligned capture directory")
    parser.add_argument("--winner", type=Path, required=True, help="JSON file written only after Yes")
    parser.add_argument(
        "--list",
        action="store_true",
        help="inspect a VS-only occurrence without opening a window or claiming a visual winner",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.list:
        capture = load_vs370_capture(args.capture)
        print(
            json.dumps(
                [
                    {
                        "option": index + 1,
                        "matching_references": len(item.references) - item.invalid,
                        "total_references": len(item.references),
                        "invalid_references": item.invalid,
                    }
                    for index, item in enumerate(capture.reference_candidates())
                ],
                indent=2,
            )
        )
        return 0
    capture = load_aligned_capture(args.capture)
    report = build_candidate_frames(capture)
    if not report.frames:
        details = json.dumps(list(report.rejected), indent=2)
        raise ValueError("no honest candidate reconstruction can be displayed:\n" + details)
    AlignedCandidateViewer(capture, report.frames, args.winner).run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
