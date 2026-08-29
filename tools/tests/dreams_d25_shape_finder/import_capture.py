#!/usr/bin/env python3
"""Import one canonical shadPS4 D25 capture into a validated finder bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path
from typing import Any, Sequence

from finder import load_bundle, prepare_candidates


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _read_metadata(capture: Path) -> dict[str, str]:
    path = capture / "manifest.tsv"
    if not path.is_file():
        raise ValueError(f"missing capture metadata: {path}")
    output: dict[str, str] = {}
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line == "key\tvalue":
            continue
        fields = line.split("\t", 1)
        if len(fields) != 2 or not fields[0]:
            raise ValueError(f"invalid manifest.tsv line {line_number}")
        output[fields[0]] = fields[1]
    return output


def _first(capture: Path, names: Sequence[str], label: str, required: bool = True) -> Path | None:
    for name in names:
        path = capture / name
        if path.is_file():
            return path
    if required:
        raise ValueError(f"capture is missing {label}: expected one of {', '.join(names)}")
    return None


def _parse_int(metadata: dict[str, str], names: Sequence[str], label: str) -> int:
    for name in names:
        if name in metadata:
            try:
                return int(metadata[name], 0)
            except ValueError as exc:
                raise ValueError(f"capture {name} is not an integer") from exc
    raise ValueError(f"capture manifest is missing {label}")


def _parse_bool(value: str) -> bool:
    lowered = value.strip().lower()
    if lowered in ("1", "true", "yes"):
        return True
    if lowered in ("0", "false", "no"):
        return False
    raise ValueError(f"not a boolean: {value}")


def _copy_entry(source: Path, destination: Path, relative: str) -> dict[str, str]:
    target = destination / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    return {"path": relative.replace("\\", "/"), "sha256": _sha256(target)}


def _write_entry(data: bytes, destination: Path, relative: str) -> dict[str, str]:
    target = destination / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    return {"path": relative.replace("\\", "/"), "sha256": _sha256(target)}


def import_capture(capture: Path | str, destination: Path | str) -> Path:
    capture = Path(capture).resolve()
    destination = Path(destination).resolve()
    if not capture.is_dir():
        raise ValueError(f"capture directory does not exist: {capture}")
    if destination.exists():
        raise FileExistsError(f"refusing to overwrite existing destination: {destination}")
    metadata = _read_metadata(capture)
    if not any(name in metadata for name in ("d25_vertex_hash", "vs_hash", "shader_hash")):
        if metadata.get("d25_draw_schema") != "d25db925-v1":
            raise ValueError(
                "capture lacks fixed D25 producer evidence (d25_vertex_hash or d25_draw_schema=d25db925-v1)"
            )
        shader_hash = 0xD25DB925
    else:
        shader_hash = _parse_int(
            metadata, ("d25_vertex_hash", "vs_hash", "shader_hash"), "D25 vertex hash"
        )
    if shader_hash != 0xD25DB925:
        raise ValueError(f"capture shader is 0x{shader_hash:08x}, not D25 0xd25db925")

    draw = _first(capture, ("draw.bin", "indirect-commands.bin"), "indirect commands")
    slice_table = _first(capture, ("index-slices.tsv",), "index slice table", required=False)
    if slice_table is not None:
        indices = _first(capture, ("index-slices.bin",), "per-command index slices")
    else:
        indices = _first(capture, ("indices.bin",), "indices")
    b1 = _first(capture, ("b1-packed-vertices.bin", "b1-full.bin"), "packed D25 b1 vertices")
    b2 = _first(capture, ("b2-records.bin",), "D25 b2 records")
    transform = _first(capture, ("transform.bin", "vs-transform.bin"), "resolved D25 transform")
    userdata = _first(
        capture, ("userdata-effective.bin", "vs-userdata-effective.bin"), "effective D25 userdata"
    )
    b2_ids = _first(capture, ("b2-record-ids.bin",), "sparse b2 record IDs", required=False)
    current = _first(
        capture,
        (
            "b0-current-corrupt.bin",
            "b0-selectors-post-alias.bin",
            "b0-post-image-alias-bound-data.bin",
        ),
        "current corrupt bound b0 selectors",
    )
    pre_alias = _first(
        capture,
        ("b0-pre-image-alias-compute-output.bin", "b0-selectors-gpu-pre-alias.bin"),
        "pre-image-alias compute-output selectors",
        required=False,
    )
    producer_1e7 = _first(
        capture,
        ("producer-1e7-b0-post.bin",),
        "exact producer 1e7 post-dispatch selectors",
        required=False,
    )
    post_alias = _first(
        capture,
        ("b0-post-image-alias-bound-data.bin", "b0-selectors-post-alias.bin"),
        "post-image-alias bound selectors",
        required=False,
    )

    index_size = _parse_int(metadata, ("index_size",), "index_size")
    if index_size not in (2, 4):
        raise ValueError("capture index_size must be 2 or 4")
    topology = metadata.get("topology")
    if topology is None:
        primitive_type = _parse_int(metadata, ("primitive_type",), "primitive_type/topology")
        topology = {4: "triangle_list", 5: "triangle_fan", 6: "triangle_strip"}.get(primitive_type)
    if topology not in ("triangle_list", "triangle_strip", "triangle_fan"):
        raise ValueError("capture topology is not a supported triangle topology")
    restart_value = metadata.get("primitive_restart_enable", "0")
    primitive_restart = _parse_bool(restart_value)
    restart_index = int(metadata.get("primitive_restart_index", "0xffff" if index_size == 2 else "0xffffffff"), 0)

    destination.mkdir(parents=True)
    raw_commands = draw.read_bytes()  # type: ignore[union-attr]
    command_stride = int(metadata.get("commands_stride", "20"), 0)
    if command_stride < 20:
        raise ValueError("captured indirect command stride is smaller than 20 bytes")
    command_count = int(metadata.get("draw_count", str(len(raw_commands) // command_stride)), 0)
    if command_count < 1 or len(raw_commands) < command_count * command_stride:
        raise ValueError("captured indirect command stride/count is inconsistent with its bytes")
    tight_commands = b"".join(
        raw_commands[index * command_stride : index * command_stride + 20]
        for index in range(command_count)
    )
    files: dict[str, dict[str, str]] = {
        "draw": _write_entry(tight_commands, destination, "shared/draw.bin"),
        "indices": _copy_entry(indices, destination, "shared/index-slices.bin" if slice_table else "shared/indices.bin"),  # type: ignore[arg-type]
        "b1": _copy_entry(b1, destination, "shared/b1-packed-vertices.bin"),  # type: ignore[arg-type]
        "b2": _copy_entry(b2, destination, "shared/b2-records.bin"),  # type: ignore[arg-type]
        "transform": _copy_entry(transform, destination, "shared/transform.bin"),  # type: ignore[arg-type]
        "userdata": _copy_entry(userdata, destination, "shared/userdata-effective.bin"),  # type: ignore[arg-type]
    }
    if slice_table is not None:
        files["index_slices"] = _copy_entry(slice_table, destination, "shared/index-slices.tsv")
    if b2_ids is not None:
        files["b2_record_ids"] = _copy_entry(b2_ids, destination, "shared/b2-record-ids.bin")

    candidates: list[dict[str, Any]] = []
    seen_sources: set[Path] = set()

    def add_candidate(
        name: str,
        source: Path,
        phase: str,
        *,
        robust_zero_missing_b2: bool = False,
    ) -> None:
        resolved = source.resolve()
        if resolved in seen_sources:
            return
        seen_sources.add(resolved)
        ordinal = len(candidates)
        entry = _copy_entry(source, destination, f"candidates/{ordinal:03d}-{name}.bin")
        config: dict[str, str] = {"kind": "raw_u32"}
        if robust_zero_missing_b2:
            config["missing_b2"] = "robust_zero_128"
        candidates.append({
            "name": name,
            "selectors": entry,
            "config": config,
            "provenance": {
                "capture_phase": phase,
                "source_file": source.name,
                "source_sha256": _sha256(source),
            },
        })

    # Accuracy gate: the exact currently-corrupt bound bytes are always candidate zero.
    add_candidate(
        "current-corrupt-bound-data",
        current,  # type: ignore[arg-type]
        "current-corrupt-bound-data",
        robust_zero_missing_b2=True,
    )
    if producer_1e7 is not None:
        add_candidate("producer-1e7-b0-post", producer_1e7, "producer-1e7-post-dispatch")
    if pre_alias is not None:
        add_candidate(
            "pre-image-alias-compute-output", pre_alias, "pre-image-alias-compute-output"
        )
    if post_alias is not None:
        add_candidate("post-image-alias-bound-data", post_alias, "post-image-alias-bound-data")

    document: dict[str, Any] = {
        "schema": 1,
        "shader_hash": "0xd25db925",
        "index_type": "uint16" if index_size == 2 else "uint32",
        "topology": topology,
        "primitive_restart": primitive_restart,
        "primitive_restart_index": restart_index,
        "files": files,
        "candidates": candidates,
        "capture_metadata_sha256": _sha256(capture / "manifest.tsv"),
    }
    if slice_table is None:
        document["indices_first_index"] = int(metadata.get("indices_first_index", "0"), 0)
    manifest = destination / "bundle.json"
    manifest.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    bundle = load_bundle(manifest)
    valid, rejected = prepare_candidates(bundle)
    if not valid or valid[0].candidate.name != "current-corrupt-bound-data":
        reason = rejected.get("current-corrupt-bound-data", "not first")
        raise ValueError(f"current corrupt accuracy-gate candidate is not replayable: {reason}")
    return manifest


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args(argv)
    try:
        manifest = import_capture(args.capture, args.destination)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
