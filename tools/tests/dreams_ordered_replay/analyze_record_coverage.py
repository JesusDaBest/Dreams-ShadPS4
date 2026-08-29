#!/usr/bin/env python3
"""Correlate exact replay pixel coverage with captured B535 membership rows."""

from __future__ import annotations

import argparse
import csv
import json
import struct
from pathlib import Path

import numpy as np


def read_pairs(path: Path) -> list[tuple[int, int]]:
    data = path.read_bytes()
    if len(data) % 8:
        raise ValueError(f"{path} is not a U32x2 record list")
    return list(struct.iter_unpack("<II", data))


def read_manifest(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "\t" in line:
            key, value = line.split("\t", 1)
            values[key] = value
    return values


def read_depth_mask(path: Path, before: np.ndarray, height: int, pitch: int, width: int) -> np.ndarray:
    current = np.fromfile(path, dtype="<u4")
    expected = height * pitch
    if current.size != expected:
        raise ValueError(f"{path} contains {current.size} depth pixels; expected {expected}")
    return current.reshape(height, pitch)[:, :width] != before


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path, help="exact vs370 occurrence directory")
    parser.add_argument("membership", type=Path, help="b535-membership.tsv")
    parser.add_argument("--limit", type=int, default=30)
    args = parser.parse_args()

    bundle = args.bundle.resolve()
    values = read_manifest(bundle / "manifest.tsv")
    width = int(values["pre_depth_width"], 0)
    height = int(values["pre_depth_height"], 0)
    pitch = int(values["pre_depth_row_length"], 0)
    before = np.fromfile(bundle / "pre-depth.bin", dtype="<u4").reshape(height, pitch)[:, :width]
    baseline = read_depth_mask(bundle / "post-depth.bin", before, height, pitch, width)

    inputs = read_pairs(bundle / "b535-input-records.bin")
    current = set(read_pairs(bundle / "b3-list.bin"))
    candidate_dir = bundle / "candidate-results"
    masks = [
        read_depth_mask(
            candidate_dir / f"candidate-{index:04d}-depth.bin", before, height, pitch, width
        )
        for index in range(len(inputs))
    ]
    union = np.logical_or.reduce(masks)
    missing = union & ~baseline

    roles: dict[tuple[int, int], dict[str, str]] = {}
    with args.membership.open(encoding="utf-8", newline="") as source:
        for row in csv.DictReader(source, delimiter="\t"):
            if int(row["valid"], 0) & 1:
                roles[(int(row["identity"], 0), int(row["record"], 0))] = row

    rows: list[dict[str, object]] = []
    for index, (pair, mask) in enumerate(zip(inputs, masks)):
        role = roles.get(pair, {})
        rows.append(
            {
                "index": index,
                "pair": f"{pair[0]:08x}:{pair[1]:08x}",
                "current": pair in current,
                "pixels": int(mask.sum()),
                "fills_missing": int((mask & missing).sum()),
                "role": role.get("role_name", "unmapped"),
                "pass": int(role.get("pass", "0"), 0),
                "workgroup": int(role.get("workgroup", "0"), 0),
                "lane": int(role.get("lane", "0"), 0),
                "flags": role.get("flags", ""),
            }
        )

    remaining = missing.copy()
    greedy: list[dict[str, object]] = []
    while remaining.any():
        gains = np.fromiter((int((mask & remaining).sum()) for mask in masks), dtype=np.int64)
        best_index = int(gains.argmax())
        gain = int(gains[best_index])
        if gain == 0:
            break
        greedy.append({**rows[best_index], "gain": gain})
        remaining &= ~masks[best_index]

    aggregate: dict[str, dict[str, int]] = {}
    for row in rows:
        role_name = str(row["role"])
        group = aggregate.setdefault(
            role_name, {"records": 0, "current": 0, "pixels_sum": 0, "missing_hits": 0}
        )
        group["records"] += 1
        group["current"] += int(bool(row["current"]))
        group["pixels_sum"] += int(row["pixels"])
        group["missing_hits"] += int(row["fills_missing"])

    report = {
        "input_records": len(inputs),
        "membership_rows": len(roles),
        "matched_inputs": sum(pair in roles for pair in inputs),
        "baseline_pixels": int(baseline.sum()),
        "union_pixels": int(union.sum()),
        "missing_pixels": int(missing.sum()),
        "remaining_after_greedy": int(remaining.sum()),
        "role_aggregate": aggregate,
        "greedy": greedy,
        "top_noncurrent": sorted(
            (row for row in rows if not row["current"]),
            key=lambda row: (-int(row["fills_missing"]), int(row["index"])),
        )[: max(args.limit, 0)],
    }
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
