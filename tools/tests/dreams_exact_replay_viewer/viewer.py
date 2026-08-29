#!/usr/bin/env python3
"""Four-image viewer for outputs from the real offline Dreams Vulkan replay."""

from __future__ import annotations

import argparse
import json
import sys
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk

try:
    from PIL import Image, ImageTk
except ImportError:  # Tk 8.6 can still display PNGs without Pillow.
    Image = None
    ImageTk = None

try:
    from .model import (
        ActionRefused,
        ReplayStatus,
        StatusError,
        image_paths,
        load_status,
        resolve_replay_root,
        write_action_marker,
    )
except ImportError:
    from model import (  # type: ignore[no-redef]
        ActionRefused,
        ReplayStatus,
        StatusError,
        image_paths,
        load_status,
        resolve_replay_root,
        write_action_marker,
    )


PANEL_WIDTH = 700
PANEL_HEIGHT = 325


class ImagePanel(ttk.LabelFrame):
    def __init__(self, parent: tk.Misc, title: str, fixed: bool) -> None:
        super().__init__(parent, text=title, padding=5)
        self.fixed = fixed
        self.path: Path | None = None
        self.signature: tuple[int, int] | None = None
        self.photo: object | None = None
        self.label = tk.Label(
            self,
            text="Waiting for replay PNG",
            background="#17191d",
            foreground="#d8d8d8",
            width=80,
            height=18,
        )
        self.label.pack(fill="both", expand=True)

    def reload(self, path: Path, force: bool = False) -> None:
        self.path = path
        if self.fixed and self.photo is not None:
            return
        try:
            stat = path.stat()
            signature = (stat.st_mtime_ns, stat.st_size)
        except OSError:
            self.photo = None
            self.signature = None
            self.label.configure(image="", text=f"Missing replay output\n{path.name}")
            return
        if not force and signature == self.signature:
            return
        try:
            if Image is not None and ImageTk is not None:
                with Image.open(path) as source:
                    source.load()
                    rendered = source.copy()
                rendered.thumbnail((PANEL_WIDTH, PANEL_HEIGHT), Image.Resampling.LANCZOS)
                photo = ImageTk.PhotoImage(rendered)
            else:
                photo = tk.PhotoImage(file=str(path))
                factor = max(
                    1,
                    (photo.width() + PANEL_WIDTH - 1) // PANEL_WIDTH,
                    (photo.height() + PANEL_HEIGHT - 1) // PANEL_HEIGHT,
                )
                if factor > 1:
                    photo = photo.subsample(factor, factor)
        except Exception as exc:
            self.photo = None
            self.signature = None
            self.label.configure(image="", text=f"Unreadable replay PNG\n{path.name}\n{exc}")
            return
        self.photo = photo
        self.signature = signature
        self.label.configure(image=photo, text="")


class ReplayViewer:
    def __init__(self, root: tk.Tk, replay_root: Path) -> None:
        self.root = root
        self.replay_root = replay_root
        self.status: ReplayStatus | None = None
        self.last_status_signature: tuple[int, int] | None = None

        root.title("Dreams exact Vulkan replay")
        root.minsize(1060, 720)

        toolbar = ttk.Frame(root, padding=(8, 7))
        toolbar.pack(fill="x")
        ttk.Button(toolbar, text="Reload", command=lambda: self.reload(force=True)).pack(side="left")
        self.start_button = ttk.Button(toolbar, text="Start Search", command=self.start_search)
        self.start_button.pack(side="left", padx=(8, 0))
        self.stop_button = ttk.Button(toolbar, text="Stop Search", command=self.stop_search)
        self.stop_button.pack(side="left", padx=(8, 0))
        self.save_button = ttk.Button(toolbar, text="Save Current", command=self.save_winner)
        self.save_button.pack(side="left", padx=(8, 0))
        self.parity_label = ttk.Label(toolbar, text="PARITY: NOT VERIFIED", foreground="#b00020")
        self.parity_label.pack(side="right")

        grid = ttk.Frame(root, padding=(8, 0, 8, 5))
        grid.pack(fill="both", expand=True)
        grid.columnconfigure(0, weight=1)
        grid.columnconfigure(1, weight=1)
        grid.rowconfigure(0, weight=1)
        grid.rowconfigure(1, weight=1)
        self.panels = {
            "candidate_near_a.png": ImagePanel(grid, "Candidate - view A (changes)", fixed=False),
            "candidate_near_b.png": ImagePanel(grid, "Candidate - view B (changes)", fixed=False),
            "reference_a.png": ImagePanel(grid, "Reference - view A (fixed)", fixed=True),
            "reference_b.png": ImagePanel(grid, "Reference - view B (fixed)", fixed=True),
        }
        self.panels["candidate_near_a.png"].grid(row=0, column=0, sticky="nsew", padx=(0, 4), pady=(0, 4))
        self.panels["candidate_near_b.png"].grid(row=0, column=1, sticky="nsew", padx=(4, 0), pady=(0, 4))
        self.panels["reference_a.png"].grid(row=1, column=0, sticky="nsew", padx=(0, 4), pady=(4, 0))
        self.panels["reference_b.png"].grid(row=1, column=1, sticky="nsew", padx=(4, 0), pady=(4, 0))

        details = ttk.Frame(root, padding=(8, 3, 8, 8))
        details.pack(fill="x")
        self.candidate_label = ttk.Label(details, text="Candidate: -")
        self.candidate_label.pack(anchor="w")
        self.settings_label = ttk.Label(details, text="Settings: {}", wraplength=1400)
        self.settings_label.pack(anchor="w")
        self.winner_label = ttk.Label(details, text="Saved winners: 0")
        self.winner_label.pack(anchor="w")
        self.message_label = ttk.Label(details, text="", wraplength=1400)
        self.message_label.pack(anchor="w")

        self.reload(force=True)
        self.root.after(750, self.poll)

    def _set_buttons(self) -> None:
        status = self.status
        self.start_button.configure(state="normal" if status and status.can_start_search else "disabled")
        self.stop_button.configure(state="normal" if status and status.can_stop_search else "disabled")
        self.save_button.configure(state="normal" if status and status.can_save_winner else "disabled")

    def reload(self, force: bool = False) -> None:
        try:
            status = load_status(self.replay_root)
        except StatusError as exc:
            self.status = None
            self.parity_label.configure(text="PARITY: NOT VERIFIED", foreground="#b00020")
            self.candidate_label.configure(text="Candidate: -")
            self.settings_label.configure(text="Settings: {}")
            self.winner_label.configure(text="Saved winners: 0")
            self.message_label.configure(text=str(exc))
            self._set_buttons()
        else:
            self.status = status
            if status.parity_verified:
                self.parity_label.configure(text="PARITY: VERIFIED", foreground="#137333")
            else:
                self.parity_label.configure(text="PARITY: NOT VERIFIED - SEARCH LOCKED", foreground="#b00020")
            score = "-" if status.candidate.score is None else f"{status.candidate.score:.8g}"
            self.candidate_label.configure(text=f"Candidate: {status.candidate.label or '-'}   Score: {score}")
            settings = json.dumps(status.candidate.settings, sort_keys=True, separators=(", ", ": "))
            self.settings_label.configure(text=f"Settings: {settings}")
            winner_text = f"Saved winners: {status.winner.count}"
            if status.winner.saved and status.winner.path is not None:
                winner_text += f"   Last save: {status.winner.path}"
            self.winner_label.configure(text=winner_text)
            self.message_label.configure(text=status.message)
            self._set_buttons()

        for name, path in image_paths(self.replay_root).items():
            self.panels[name].reload(path, force=force and not self.panels[name].fixed)

    def _request(self, action: str) -> None:
        if self.status is None:
            messagebox.showerror("Replay unavailable", "A valid status.json is required.")
            return
        try:
            marker = write_action_marker(self.status, action)
        except (ActionRefused, OSError) as exc:
            messagebox.showerror("Request refused", str(exc))
            return
        self.message_label.configure(text=f"Request written; waiting for replay process: {marker.name}")
        self._set_buttons()

    def start_search(self) -> None:
        self._request("start_search")

    def stop_search(self) -> None:
        self._request("stop_search")

    def save_winner(self) -> None:
        self._request("save_winner")

    def poll(self) -> None:
        try:
            path = self.replay_root / "status.json"
            stat = path.stat()
            signature = (stat.st_mtime_ns, stat.st_size)
        except OSError:
            signature = None
        if signature != self.last_status_signature:
            self.last_status_signature = signature
            self.reload()
        else:
            # Candidate PNG replacement may happen immediately before status replacement.
            for name in ("candidate_near_a.png", "candidate_near_b.png"):
                self.panels[name].reload(self.replay_root / name)
        self.root.after(750, self.poll)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Display four PNGs emitted by the exact offline Dreams Vulkan replay."
    )
    parser.add_argument("replay_output", help="Replay output directory or its status.json")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        replay_root = resolve_replay_root(args.replay_output)
    except StatusError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    root = tk.Tk()
    ReplayViewer(root, replay_root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
