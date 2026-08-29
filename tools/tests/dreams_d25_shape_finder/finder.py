#!/usr/bin/env python3
"""Replay real Dreams d25db925 inputs and show selector candidates one at a time."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import shutil
import struct
import sys
import tempfile
from array import array
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence


SHADER_HASH = 0xD25DB925
SCHEMA = 1
SHARED_KEYS = ("draw", "indices", "b1", "b2", "transform", "userdata")
COLORS = ("#101820", "#b05a24", "#176b87", "#7a3e9d", "#2f7d32", "#a11f32")
RASTER_COLORS = (
    (36, 76, 112),
    (176, 90, 36),
    (23, 107, 135),
    (122, 62, 157),
    (47, 125, 50),
    (161, 31, 50),
    (198, 145, 46),
    (65, 91, 72),
)
RASTER_BACKGROUND = (245, 245, 242)
LIVE_FINGERPRINT_FIELDS = (
    ("commands_address", "commands_base", 0xFFFFFFFFFFFFFFFF),
    ("count_address", "count_address", 0xFFFFFFFFFFFFFFFF),
    ("stride", "commands_stride", 0xFFFFFFFF),
    ("max_count", "max_count", 0xFFFFFFFF),
    ("selector_base", "selector_base", 0xFFFFFFFFFFFFFFFF),
    ("selector_size", "selector_size", 0xFFFFFFFFFFFFFFFF),
    ("transform_address", "transform_address", 0xFFFFFFFFFFFFFFFF),
)


class BundleError(ValueError):
    """The bundle cannot be used for an exact replay."""


class CandidateError(ValueError):
    """One candidate cannot be replayed with the captured fixture."""


def load_live_fingerprint_manifest(path: Path | str) -> dict[str, int]:
    """Read only cheap, immutable D25 draw identity fields from a capture manifest TSV."""
    path = Path(path).resolve()
    if not path.is_file():
        raise BundleError(f"live fingerprint manifest does not exist: {path}")
    metadata: dict[str, str] = {}
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line:
            continue
        fields = line.split("\t", 1)
        if line_number == 1 and fields in (["field", "value"], ["key", "value"]):
            continue
        if len(fields) != 2 or not fields[0]:
            raise BundleError(f"invalid live fingerprint manifest line {line_number}")
        if fields[0] in metadata:
            raise BundleError(f"duplicate live fingerprint manifest field: {fields[0]}")
        metadata[fields[0]] = fields[1]

    missing = [source for _target, source, _limit in LIVE_FINGERPRINT_FIELDS if source not in metadata]
    if missing:
        raise BundleError(
            "live fingerprint manifest is missing required fields: " + ", ".join(missing)
        )
    fingerprint: dict[str, int] = {}
    for target, source, limit in LIVE_FINGERPRINT_FIELDS:
        try:
            value = int(metadata[source], 0)
        except ValueError as exc:
            raise BundleError(f"live fingerprint field {source} is not an integer") from exc
        if value < 0 or value > limit:
            raise BundleError(f"live fingerprint field {source} is outside its unsigned range")
        fingerprint[target] = value
    return fingerprint


@dataclass(frozen=True)
class Draw:
    index_count: int
    instance_count: int
    first_index: int
    vertex_offset: int
    first_instance: int


@dataclass(frozen=True)
class Candidate:
    name: str
    path: Path
    relative_path: str
    sha256: str
    selectors: tuple[int, ...]
    config: dict[str, Any]
    provenance: dict[str, Any]


@dataclass(frozen=True)
class Fixture:
    draws: tuple[Draw, ...]
    command_indices: tuple[tuple[int, ...], ...]
    topology: str
    primitive_restart: bool
    primitive_restart_index: int
    b1_words: tuple[int, ...]
    b2_words: tuple[int, ...]
    b2_slots: dict[int, int] | None
    transform: tuple[float, ...]
    userdata: tuple[int, ...]
    cull_mode: str
    front_face: str
    clip_z: str


@dataclass(frozen=True)
class Bundle:
    root: Path
    manifest_path: Path
    document: dict[str, Any]
    shared_paths: dict[str, Path]
    fixture: Fixture
    candidates: tuple[Candidate, ...]


@dataclass(frozen=True)
class InstanceGeometry:
    command: int
    selector: int
    positions: tuple[tuple[float, float, float, float] | None, ...]
    triangles: tuple[tuple[int, int, int], ...]


@dataclass(frozen=True)
class ReplayResult:
    candidate: Candidate
    instances: tuple[InstanceGeometry, ...]
    cull_mode: str
    front_face: str
    clip_z: str


@dataclass(frozen=True)
class ShapeFraming:
    center_x: float
    center_y: float
    span_x: float
    span_y: float


@dataclass(frozen=True)
class ProjectedTriangle:
    points: tuple[
        tuple[float, float, float],
        tuple[float, float, float],
        tuple[float, float, float],
    ]
    color_index: int


@dataclass(frozen=True)
class RasterImage:
    width: int
    height: int
    pixels: bytes


@dataclass(frozen=True)
class CandidateGroup:
    primary: Candidate
    equivalents: tuple[Candidate, ...]


def deduplicate_candidates(candidates: Sequence[Candidate]) -> tuple[CandidateGroup, ...]:
    """Group exact selector payload duplicates while retaining their original order/metadata."""
    groups: list[list[Candidate]] = []
    payload_to_group: dict[bytes, int] = {}
    for candidate in candidates:
        payload = candidate.path.read_bytes()
        group_index = payload_to_group.get(payload)
        if group_index is None:
            payload_to_group[payload] = len(groups)
            groups.append([candidate])
        else:
            groups[group_index].append(candidate)
    return tuple(CandidateGroup(group[0], tuple(group)) for group in groups)


def _u32(value: int) -> int:
    return value & 0xFFFFFFFF


def _f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _fmul(a: float, b: float) -> float:
    return _f32(a * b)


def _fadd(a: float, b: float) -> float:
    return _f32(a + b)


COORD_SCALE = struct.unpack("<f", struct.pack("<I", 0x38000080))[0]


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _require_dict(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise BundleError(f"{label} must be a JSON object")
    return value


def _require_int(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise BundleError(f"{label} must be an integer")
    return value


def _resolve_file(root: Path, entry: Any, label: str) -> tuple[Path, str, str]:
    item = _require_dict(entry, label)
    relative = item.get("path")
    expected = item.get("sha256")
    if not isinstance(relative, str) or not relative:
        raise BundleError(f"{label}.path must be a non-empty string")
    if not isinstance(expected, str) or len(expected) != 64:
        raise BundleError(f"{label}.sha256 must be a 64-character SHA-256 digest")
    try:
        int(expected, 16)
    except ValueError as exc:
        raise BundleError(f"{label}.sha256 is not hexadecimal") from exc
    path = (root / relative).resolve()
    try:
        path.relative_to(root)
    except ValueError as exc:
        raise BundleError(f"{label}.path escapes the bundle directory") from exc
    if not path.is_file():
        raise BundleError(f"missing required file: {path}")
    actual = _sha256(path)
    if actual != expected.lower():
        raise BundleError(f"SHA-256 mismatch for {path}: expected {expected.lower()}, got {actual}")
    return path, relative, actual


def _u32_words(data: bytes, label: str) -> tuple[int, ...]:
    if len(data) % 4:
        raise BundleError(f"{label} is not a whole number of little-endian u32 values")
    return struct.unpack(f"<{len(data) // 4}I", data) if data else ()


def _parse_shader_hash(value: Any) -> int:
    if isinstance(value, int) and not isinstance(value, bool):
        return value
    if isinstance(value, str):
        try:
            return int(value, 0)
        except ValueError as exc:
            raise BundleError("shader_hash is not an integer") from exc
    raise BundleError("shader_hash must be an integer or hexadecimal string")


def _decode_indices(data: bytes, index_type: str) -> tuple[int, ...]:
    sizes = {"uint16": (2, "H"), "uint32": (4, "I")}
    if index_type not in sizes:
        raise BundleError("index_type must be 'uint16' or 'uint32'")
    size, code = sizes[index_type]
    if len(data) % size:
        raise BundleError(f"indices.bin length is not divisible by {size}")
    return struct.unpack(f"<{len(data) // size}{code}", data) if data else ()


def _parse_index_slices(
    table_path: Path,
    data: bytes,
    index_type: str,
    draws: Sequence[Draw],
) -> tuple[tuple[int, ...], ...]:
    size = 2 if index_type == "uint16" else 4
    output: list[tuple[int, ...] | None] = [None] * len(draws)
    with table_path.open("r", encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames is None:
            raise BundleError("index-slices.tsv has no header")
        for row_number, row in enumerate(reader, 2):
            def field(*names: str) -> str:
                for name in names:
                    value = row.get(name)
                    if value is not None and value != "":
                        return value
                raise BundleError(f"index-slices.tsv row {row_number} is missing {'/'.join(names)}")

            try:
                command = int(field("command", "command_index"), 0)
                byte_offset = int(field("byte_offset", "offset_bytes", "blob_offset"), 0)
                count = int(field("index_count", "count"), 0)
            except ValueError as exc:
                raise BundleError(f"index-slices.tsv row {row_number} has a non-integer field") from exc
            if command < 0 or command >= len(draws) or output[command] is not None:
                raise BundleError(f"index-slices.tsv row {row_number} has invalid/duplicate command {command}")
            if byte_offset < 0 or byte_offset % size or count < 0:
                raise BundleError(f"index-slices.tsv row {row_number} has an invalid range")
            end = byte_offset + count * size
            if end > len(data):
                raise BundleError(f"index-slices.tsv row {row_number} exceeds index-slices.bin")
            if count != draws[command].index_count:
                raise BundleError(
                    f"index slice {command} count {count} does not match draw count {draws[command].index_count}"
                )
            output[command] = _decode_indices(data[byte_offset:end], index_type)
    for command, draw in enumerate(draws):
        if draw.index_count and draw.instance_count and output[command] is None:
            raise BundleError(f"index-slices.tsv has no row for active command {command}")
        if output[command] is None:
            output[command] = ()
    return tuple(output)  # type: ignore[arg-type]


def load_bundle(path: Path | str) -> Bundle:
    manifest_path = Path(path).resolve()
    if manifest_path.is_dir():
        manifest_path = manifest_path / "bundle.json"
    if not manifest_path.is_file():
        raise BundleError(f"missing bundle manifest: {manifest_path}")
    root = manifest_path.parent.resolve()
    try:
        document = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise BundleError(f"cannot read {manifest_path}: {exc}") from exc
    document = _require_dict(document, "bundle")
    if document.get("schema") != SCHEMA:
        raise BundleError(f"bundle schema must be {SCHEMA}")
    if _parse_shader_hash(document.get("shader_hash")) != SHADER_HASH:
        raise BundleError("bundle is not for Dreams vertex shader 0xd25db925")

    files = _require_dict(document.get("files"), "files")
    missing_keys = [key for key in SHARED_KEYS if key not in files]
    if missing_keys:
        raise BundleError(f"files is missing: {', '.join(missing_keys)}")
    shared_paths: dict[str, Path] = {}
    for key in SHARED_KEYS:
        shared_paths[key], _, _ = _resolve_file(root, files[key], f"files.{key}")
    for key in ("index_slices", "b2_record_ids"):
        if key in files:
            shared_paths[key], _, _ = _resolve_file(root, files[key], f"files.{key}")

    draw_data = shared_paths["draw"].read_bytes()
    if not draw_data or len(draw_data) % 20:
        raise BundleError("draw.bin must contain complete 20-byte VkDrawIndexedIndirectCommand records")
    draws = tuple(
        Draw(*struct.unpack_from("<IIIiI", draw_data, offset))
        for offset in range(0, len(draw_data), 20)
    )
    if not any(draw.index_count and draw.instance_count for draw in draws):
        raise BundleError("captured indirect commands contain no active indexed draw")

    index_type = document.get("index_type")
    if not isinstance(index_type, str):
        raise BundleError("index_type is required")
    index_data = shared_paths["indices"].read_bytes()
    if "index_slices" in shared_paths:
        command_indices = _parse_index_slices(shared_paths["index_slices"], index_data, index_type, draws)
    else:
        indices = _decode_indices(index_data, index_type)
        indices_first_index = _require_int(document.get("indices_first_index"), "indices_first_index")
        if indices_first_index < 0:
            raise BundleError("indices_first_index cannot be negative")
        command_indices_list: list[tuple[int, ...]] = []
        for ordinal, draw in enumerate(draws):
            if not draw.index_count or not draw.instance_count:
                command_indices_list.append(())
                continue
            begin = draw.first_index - indices_first_index
            if begin < 0 or begin + draw.index_count > len(indices):
                raise BundleError(f"indices.bin does not contain indexed command {ordinal}")
            command_indices_list.append(indices[begin : begin + draw.index_count])
        command_indices = tuple(command_indices_list)

    topology = document.get("topology")
    if topology not in ("triangle_list", "triangle_strip", "triangle_fan"):
        raise BundleError("topology must be triangle_list, triangle_strip, or triangle_fan")
    primitive_restart = document.get("primitive_restart")
    if not isinstance(primitive_restart, bool):
        raise BundleError("primitive_restart must be true or false")
    default_restart = 0xFFFF if index_type == "uint16" else 0xFFFFFFFF
    primitive_restart_index = document.get("primitive_restart_index", default_restart)
    primitive_restart_index = _require_int(primitive_restart_index, "primitive_restart_index")
    if primitive_restart_index < 0 or primitive_restart_index > default_restart:
        raise BundleError("primitive_restart_index does not fit index_type")

    b1_words = _u32_words(shared_paths["b1"].read_bytes(), "b1-packed-vertices.bin")
    if len(b1_words) == 0 or len(b1_words) % 2:
        raise BundleError("b1-packed-vertices.bin must contain complete 8-byte packed vertices")
    b2_words = _u32_words(shared_paths["b2"].read_bytes(), "b2-records.bin")
    if len(b2_words) == 0 or len(b2_words) % 32:
        raise BundleError("b2-records.bin must contain complete 128-byte records")
    b2_slots: dict[int, int] | None = None
    if "b2_record_ids" in shared_paths:
        record_ids = _u32_words(shared_paths["b2_record_ids"].read_bytes(), "b2-record-ids.bin")
        if len(record_ids) != len(b2_words) // 32:
            raise BundleError("b2-record-ids.bin must contain one ID per captured b2 record")
        if len(set(record_ids)) != len(record_ids):
            raise BundleError("b2-record-ids.bin contains duplicate selector IDs")
        b2_slots = {record_id: slot for slot, record_id in enumerate(record_ids)}

    transform_data = shared_paths["transform"].read_bytes()
    if len(transform_data) != 64:
        raise BundleError("transform.bin must be the exact resolved 64-byte transform")
    transform = struct.unpack("<16f", transform_data)
    if not all(math.isfinite(value) for value in transform):
        raise BundleError("transform.bin contains non-finite values")
    userdata = _u32_words(shared_paths["userdata"].read_bytes(), "userdata-effective.bin")
    if len(userdata) != 5:
        raise BundleError("userdata-effective.bin must contain exactly SGPR0..SGPR4 (20 bytes)")

    raster_state_value = document.get("raster_state", {})
    raster_state = _require_dict(raster_state_value, "raster_state")
    cull_mode = raster_state.get("cull_mode", "none")
    front_face = raster_state.get("front_face", "counter_clockwise")
    clip_z = raster_state.get("clip_z", "minus_one_to_one")
    if cull_mode not in ("none", "front", "back", "front_and_back"):
        raise BundleError("raster_state.cull_mode must be none, front, back, or front_and_back")
    if front_face not in ("clockwise", "counter_clockwise"):
        raise BundleError("raster_state.front_face must be clockwise or counter_clockwise")
    if clip_z not in ("minus_one_to_one", "zero_to_one"):
        raise BundleError("raster_state.clip_z must be minus_one_to_one or zero_to_one")

    raw_candidates = document.get("candidates")
    if not isinstance(raw_candidates, list) or not raw_candidates:
        raise BundleError("bundle must contain at least one selector candidate")
    candidates: list[Candidate] = []
    names: set[str] = set()
    for ordinal, raw in enumerate(raw_candidates):
        item = _require_dict(raw, f"candidates[{ordinal}]")
        name = item.get("name")
        if not isinstance(name, str) or not name.strip():
            raise BundleError(f"candidates[{ordinal}].name must be non-empty")
        if name in names:
            raise BundleError(f"duplicate candidate name: {name}")
        names.add(name)
        config = _require_dict(item.get("config"), f"candidates[{ordinal}].config")
        provenance = _require_dict(item.get("provenance"), f"candidates[{ordinal}].provenance")
        if not config or not provenance:
            raise BundleError(f"candidate {name} must record non-empty config and provenance")
        missing_b2 = config.get("missing_b2")
        if missing_b2 is not None and (
            name != "current-corrupt-bound-data" or missing_b2 != "robust_zero_128"
        ):
            raise BundleError(
                "robust-zero missing b2 records are allowed only for current-corrupt-bound-data"
            )
        candidate_path, relative, digest = _resolve_file(
            root, item.get("selectors"), f"candidates[{ordinal}].selectors"
        )
        selectors = _u32_words(candidate_path.read_bytes(), f"candidate {name} selectors")
        if not selectors:
            raise BundleError(f"candidate {name} has no selector words")
        candidates.append(Candidate(name, candidate_path, relative, digest, selectors, config, provenance))

    return Bundle(root, manifest_path, document, shared_paths, Fixture(
        draws, command_indices, topology, primitive_restart, primitive_restart_index,
        b1_words, b2_words, b2_slots, transform, userdata, cull_mode, front_face, clip_z,
    ), tuple(candidates))


def _record_float(words: Sequence[int], slot: int, offset: int) -> float:
    return struct.unpack("<f", struct.pack("<I", words[slot * 32 + offset]))[0]


def _local_coordinate(value: int) -> float:
    return _fadd(_fmul(COORD_SCALE, _f32(float(value))), -1.0)


def _position(
    fixture: Fixture,
    selector: int,
    vertex_index: int,
    robust_zero_missing_b2: bool = False,
) -> tuple[float, float, float, float]:
    vertex_word = _u32(_u32(fixture.userdata[3]) + _u32(vertex_index))
    word_offset = vertex_word * 2
    if word_offset + 1 >= len(fixture.b1_words):
        raise CandidateError(f"vertex {vertex_word} is outside b1-packed-vertices.bin")
    packed0, packed1 = fixture.b1_words[word_offset : word_offset + 2]
    x = _local_coordinate(packed0 & 0xFFFF)
    y = _local_coordinate((packed0 >> 16) & 0xFFFF)
    z = _local_coordinate(packed1 & 0xFFFF)

    if fixture.b2_slots is None:
        slot = selector
        if slot * 32 + 31 >= len(fixture.b2_words):
            if robust_zero_missing_b2:
                slot = -1
            else:
                raise CandidateError(f"selector {selector} is outside contiguous b2-records.bin")
    else:
        slot = fixture.b2_slots.get(selector, -1)
        if slot < 0 and not robust_zero_missing_b2:
            raise CandidateError(f"selector {selector} is absent from sparse b2-record-ids.bin")
    r = (
        (lambda _offset: 0.0)
        if slot < 0
        else (lambda offset: _record_float(fixture.b2_words, slot, offset))
    )
    px = _fadd(_fmul(z, r(18)), _fadd(_fmul(y, r(15)), _fadd(_fmul(x, r(12)), r(21))))
    py = _fadd(_fmul(z, r(19)), _fadd(_fmul(y, r(16)), _fadd(_fmul(x, r(13)), r(22))))
    pz = _fadd(_fmul(z, r(20)), _fadd(_fmul(y, r(17)), _fadd(_fmul(x, r(14)), r(23))))
    if not all(math.isfinite(value) for value in (px, py, pz)):
        raise CandidateError(f"selector {selector} produces a non-finite object position")

    t = fixture.transform
    output = tuple(
        _fadd(_fmul(t[8 + component], pz), _fadd(
            _fmul(t[4 + component], py), _fadd(_fmul(t[component], px), t[12 + component])
        ))
        for component in range(4)
    )
    if not all(math.isfinite(value) for value in output):
        raise CandidateError(f"selector {selector} produces a non-finite Position0")
    return output  # type: ignore[return-value]


def _triangles(indices: Sequence[int | None], topology: str) -> tuple[tuple[int, int, int], ...]:
    output: list[tuple[int, int, int]] = []
    runs: list[list[int]] = [[]]
    for ordinal, value in enumerate(indices):
        if value is None:
            if runs[-1]:
                runs.append([])
        else:
            runs[-1].append(ordinal)
    for run in runs:
        if topology == "triangle_list":
            raw = (tuple(run[i : i + 3]) for i in range(0, len(run) - 2, 3))
        elif topology == "triangle_strip":
            raw = (
                (run[i + 1], run[i], run[i + 2]) if i & 1 else (run[i], run[i + 1], run[i + 2])
                for i in range(len(run) - 2)
            )
        else:
            raw = ((run[0], run[i], run[i + 1]) for i in range(1, len(run) - 1)) if run else ()
        for triangle in raw:
            a, b, c = triangle
            values = (indices[a], indices[b], indices[c])
            if len(set(values)) == 3:
                output.append((a, b, c))
    return tuple(output)


def replay_candidate(bundle: Bundle, candidate: Candidate) -> ReplayResult:
    fixture = bundle.fixture
    robust_zero_missing_b2 = candidate.config.get("missing_b2") == "robust_zero_128"
    instances: list[InstanceGeometry] = []
    for command_ordinal, draw in enumerate(fixture.draws):
        if not draw.index_count or not draw.instance_count:
            continue
        draw_indices = fixture.command_indices[command_ordinal]
        marked: tuple[int | None, ...] = tuple(
            None if fixture.primitive_restart and value == fixture.primitive_restart_index else value
            for value in draw_indices
        )
        triangles = _triangles(marked, fixture.topology)
        if not triangles:
            raise CandidateError(f"indexed command {command_ordinal} produces no non-degenerate triangles")
        for local_instance in range(draw.instance_count):
            gl_instance = _u32(draw.first_instance + local_instance)
            selector_index = _u32(fixture.userdata[4] + gl_instance)
            if selector_index >= len(candidate.selectors):
                raise CandidateError(
                    f"command {command_ordinal} instance selector index {selector_index} "
                    f"is outside candidate {candidate.name}"
                )
            selector = candidate.selectors[selector_index]
            positions: list[tuple[float, float, float, float] | None] = []
            for index in marked:
                if index is None:
                    positions.append(None)
                    continue
                gl_vertex = _u32(index + draw.vertex_offset)
                positions.append(
                    _position(fixture, selector, gl_vertex, robust_zero_missing_b2)
                )
            instances.append(InstanceGeometry(command_ordinal, selector, tuple(positions), triangles))
    if not instances:
        raise CandidateError("captured commands produce no geometry")
    return ReplayResult(
        candidate,
        tuple(instances),
        fixture.cull_mode,
        fixture.front_face,
        fixture.clip_z,
    )


def prepare_candidates(bundle: Bundle) -> tuple[tuple[ReplayResult, ...], dict[str, str]]:
    results: list[ReplayResult] = []
    rejected: dict[str, str] = {}
    for candidate in bundle.candidates:
        try:
            result = replay_candidate(bundle, candidate)
            _project(result)
            results.append(result)
        except CandidateError as exc:
            rejected[candidate.name] = str(exc)
    if not results:
        details = "; ".join(f"{name}: {reason}" for name, reason in rejected.items())
        raise BundleError(f"no candidate can be replayed from the captured data ({details})")
    return tuple(results), rejected


def save_winner(
    bundle: Bundle,
    candidate: Candidate,
    destination: Path | str,
    equivalents: Sequence[Candidate] = (),
) -> Path:
    destination = Path(destination).resolve()
    if destination.exists():
        raise FileExistsError(f"refusing to overwrite existing winner: {destination}")
    destination.mkdir(parents=True)
    shared_dir = destination / "shared"
    candidate_dir = destination / "candidate"
    shared_dir.mkdir()
    candidate_dir.mkdir()

    output = dict(bundle.document)
    output_files: dict[str, dict[str, str]] = {}
    for key, source in bundle.shared_paths.items():
        target = shared_dir / source.name
        shutil.copy2(source, target)
        output_files[key] = {
            "path": target.relative_to(destination).as_posix(),
            "sha256": _sha256(target),
        }
    selector_target = candidate_dir / "b0-selectors.bin"
    shutil.copy2(candidate.path, selector_target)
    selected = {
        "name": candidate.name,
        "selectors": {
            "path": selector_target.relative_to(destination).as_posix(),
            "sha256": _sha256(selector_target),
        },
        "config": candidate.config,
        "provenance": candidate.provenance,
    }
    output["files"] = output_files
    output["candidates"] = [selected]
    output["selected_candidate"] = candidate.name
    if equivalents:
        output["byte_identical_candidates"] = [
            {
                "name": item.name,
                "sha256": item.sha256,
                "config": item.config,
                "provenance": item.provenance,
            }
            for item in equivalents
        ]
    output["source_bundle_sha256"] = _sha256(bundle.manifest_path)
    (destination / "bundle.json").write_text(
        json.dumps(output, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return destination


def _clip_polygon(
    polygon: Sequence[tuple[float, float, float, float]],
    distance,
) -> list[tuple[float, float, float, float]]:
    if not polygon:
        return []
    output: list[tuple[float, float, float, float]] = []
    previous = polygon[-1]
    previous_distance = distance(previous)
    previous_inside = previous_distance >= 0.0
    for current in polygon:
        current_distance = distance(current)
        current_inside = current_distance >= 0.0
        if current_inside != previous_inside:
            denominator = previous_distance - current_distance
            if denominator != 0.0:
                amount = previous_distance / denominator
                output.append(tuple(
                    previous[index] + amount * (current[index] - previous[index])
                    for index in range(4)
                ))  # type: ignore[arg-type]
        if current_inside:
            output.append(current)
        previous = current
        previous_distance = current_distance
        previous_inside = current_inside
    return output


def project_triangles(result: ReplayResult) -> tuple[ProjectedTriangle, ...]:
    projected: list[ProjectedTriangle] = []
    clip_nonprojectable = (
        result.candidate.name == "current-corrupt-bound-data"
        and result.candidate.config.get("missing_b2") == "robust_zero_128"
    )
    for instance_index, instance in enumerate(result.instances):
        for triangle in instance.triangles:
            clip_points = [instance.positions[index] for index in triangle]
            invalid = any(
                point is None
                or point[3] == 0.0
                or not all(math.isfinite(component) for component in point)
                for point in clip_points
            )
            if invalid:
                if clip_nonprojectable:
                    continue
                raise CandidateError("Position0 is nonprojectable")
            polygon = list(clip_points)  # type: ignore[arg-type]
            planes = [
                lambda vertex: vertex[3] - 1e-8,
            ]
            if result.clip_z == "zero_to_one":
                planes.extend((lambda vertex: vertex[2], lambda vertex: vertex[3] - vertex[2]))
            else:
                planes.extend((
                    lambda vertex: vertex[2] + vertex[3],
                    lambda vertex: vertex[3] - vertex[2],
                ))
            for plane in planes:
                polygon = _clip_polygon(polygon, plane)
                if len(polygon) < 3:
                    break
            if len(polygon) < 3:
                continue
            ndc = [
                (vertex[0] / vertex[3], vertex[1] / vertex[3], vertex[2] / vertex[3])
                for vertex in polygon
            ]
            if not all(math.isfinite(component) for point in ndc for component in point):
                if clip_nonprojectable:
                    continue
                raise CandidateError("Position0 cannot be projected")
            color_index = instance.command * 131 + instance_index
            for fan_index in range(1, len(ndc) - 1):
                projected.append(ProjectedTriangle(
                    (ndc[0], ndc[fan_index], ndc[fan_index + 1]), color_index
                ))
    if not projected and not clip_nonprojectable:
        raise CandidateError("candidate has no projectable triangles")
    return tuple(projected)


def _project(result: ReplayResult) -> list[tuple[int, list[tuple[float, float]]]]:
    return [
        (triangle.color_index, [(point[0], point[1]) for point in triangle.points])
        for triangle in project_triangles(result)
    ]


def shared_shape_framing(result: ReplayResult, padding: float = 0.12) -> ShapeFraming:
    """Build the one framing used for every candidate from candidate 1 only."""
    triangles = _project(result)
    if not triangles:
        return ShapeFraming(0.0, 0.0, 2.0, 2.0)
    points = [point for _instance, triangle in triangles for point in triangle]
    min_x = min(point[0] for point in points)
    max_x = max(point[0] for point in points)
    min_y = min(point[1] for point in points)
    max_y = max(point[1] for point in points)
    factor = 1.0 + padding * 2.0
    return ShapeFraming(
        (min_x + max_x) * 0.5,
        (min_y + max_y) * 0.5,
        max((max_x - min_x) * factor, 1e-7),
        max((max_y - min_y) * factor, 1e-7),
    )


def rasterize_projected(
    triangles: Sequence[ProjectedTriangle],
    framing: ShapeFraming,
    width: int,
    height: int,
    *,
    cull_mode: str = "none",
    front_face: str = "counter_clockwise",
) -> RasterImage:
    if width < 2 or height < 2:
        raise ValueError("raster dimensions must be at least 2x2")
    pixels = bytearray(RASTER_BACKGROUND * (width * height))
    depth = array("f", [math.inf]) * (width * height)
    scale = max(min((width - 8) / framing.span_x, (height - 8) / framing.span_y), 1e-7)

    def screen(point: tuple[float, float, float]) -> tuple[float, float, float]:
        return (
            width * 0.5 + (point[0] - framing.center_x) * scale,
            height * 0.5 - (point[1] - framing.center_y) * scale,
            point[2],
        )

    def edge(
        left: tuple[float, float, float],
        right: tuple[float, float, float],
        x: float,
        y: float,
    ) -> float:
        return (x - left[0]) * (right[1] - left[1]) - (y - left[1]) * (right[0] - left[0])

    for triangle in triangles:
        a_ndc, b_ndc, c_ndc = triangle.points
        area_ndc = (
            (b_ndc[0] - a_ndc[0]) * (c_ndc[1] - a_ndc[1])
            - (b_ndc[1] - a_ndc[1]) * (c_ndc[0] - a_ndc[0])
        )
        if abs(area_ndc) < 1e-14:
            continue
        is_front = area_ndc > 0.0 if front_face == "counter_clockwise" else area_ndc < 0.0
        if cull_mode == "front_and_back" or (cull_mode == "front" and is_front) or (
            cull_mode == "back" and not is_front
        ):
            continue
        a, b, c = screen(a_ndc), screen(b_ndc), screen(c_ndc)
        area = edge(a, b, c[0], c[1])
        if abs(area) < 1e-10:
            continue
        min_x = max(0, int(math.floor(min(a[0], b[0], c[0]))))
        max_x = min(width - 1, int(math.ceil(max(a[0], b[0], c[0]))))
        min_y = max(0, int(math.floor(min(a[1], b[1], c[1]))))
        max_y = min(height - 1, int(math.ceil(max(a[1], b[1], c[1]))))
        if min_x > max_x or min_y > max_y:
            continue
        color = RASTER_COLORS[triangle.color_index % len(RASTER_COLORS)]
        for py in range(min_y, max_y + 1):
            sample_y = py + 0.5
            row = py * width
            for px in range(min_x, max_x + 1):
                sample_x = px + 0.5
                weight_a = edge(b, c, sample_x, sample_y)
                weight_b = edge(c, a, sample_x, sample_y)
                weight_c = edge(a, b, sample_x, sample_y)
                if area > 0.0:
                    if weight_a < 0.0 or weight_b < 0.0 or weight_c < 0.0:
                        continue
                elif weight_a > 0.0 or weight_b > 0.0 or weight_c > 0.0:
                    continue
                z = (weight_a * a[2] + weight_b * b[2] + weight_c * c[2]) / area
                offset = row + px
                if z >= depth[offset]:
                    continue
                depth[offset] = z
                byte_offset = offset * 3
                pixels[byte_offset : byte_offset + 3] = bytes(color)
    return RasterImage(width, height, bytes(pixels))


def rasterize_result(
    result: ReplayResult,
    framing: ShapeFraming,
    width: int,
    height: int,
) -> RasterImage:
    return rasterize_projected(
        project_triangles(result),
        framing,
        width,
        height,
        cull_mode=result.cull_mode,
        front_face=result.front_face,
    )


class AtomicLiveControl:
    """Publish the emulator's text control and its raw candidate without partial files."""

    def __init__(self, path: Path | str, fingerprint: dict[str, int] | None = None) -> None:
        self.path = Path(path).resolve()
        self.candidate_path = self.path.with_name(f"{self.path.name}.candidate.bin")
        self.generation = 0
        self._owned: dict[Path, tuple[tuple[int, int, int, int], str]] = {}
        fingerprint = fingerprint or {}
        allowed = {target for target, _source, _limit in LIVE_FINGERPRINT_FIELDS}
        unknown = set(fingerprint) - allowed
        if unknown:
            raise BundleError(
                "unsupported live fingerprint fields: " + ", ".join(sorted(unknown))
            )
        self.fingerprint = dict(fingerprint)

    def _atomic_write(self, path: Path, payload: bytes) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
        )
        temporary = Path(temporary_name)
        try:
            with os.fdopen(descriptor, "wb") as output:
                output.write(payload)
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary, path)
        except BaseException:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass
            raise
        stat = path.stat()
        identity = stat.st_dev, stat.st_ino, stat.st_mtime_ns, stat.st_size
        self._owned[path] = identity, hashlib.sha256(payload).hexdigest()

    def _control_payload(self, generation: int) -> bytes:
        # The C++ parser rejects unknown fields but accepts comments. Alternating comment lengths
        # guarantee that its mtime-or-size poll detects every atomic control replacement.
        refresh = "." if generation & 1 else ".."
        lines = ["schema=1", "enabled=1", f"candidate={self.candidate_path}"]
        decimal_fields = {"stride", "max_count"}
        for target, _source, _limit in LIVE_FINGERPRINT_FIELDS:
            if target not in self.fingerprint:
                continue
            value = self.fingerprint[target]
            rendered = str(value) if target in decimal_fields else f"0x{value:x}"
            lines.append(f"{target}={rendered}")
        lines.append(f"# refresh={refresh}")
        return ("\n".join(lines) + "\n").encode("utf-8")

    def write(self, payload: bytes) -> None:
        next_generation = self.generation + 1
        # The emulator must never observe a control generation that points at an incomplete payload.
        self._atomic_write(self.candidate_path, payload)
        self._atomic_write(self.path, self._control_payload(next_generation))
        self.generation = next_generation

    def _remove_if_owned(self, path: Path) -> None:
        ownership = self._owned.pop(path, None)
        if ownership is None:
            return
        owned_identity, owned_digest = ownership
        try:
            stat = path.stat()
            if (stat.st_dev, stat.st_ino, stat.st_mtime_ns, stat.st_size) != owned_identity:
                return
            if _sha256(path) != owned_digest:
                return
            path.unlink()
        except OSError:
            pass

    def remove_if_owned(self) -> None:
        # Removing the control first disables the emulator before its candidate disappears.
        self._remove_if_owned(self.path)
        self._remove_if_owned(self.candidate_path)


class LiveCandidateSession:
    def __init__(self, groups: Sequence[CandidateGroup], control: AtomicLiveControl) -> None:
        if not groups:
            raise BundleError("live control has no unique candidates")
        self.groups = tuple(groups)
        self.control = control
        self.index = 0

    @property
    def current(self) -> CandidateGroup:
        return self.groups[self.index]

    def start(self) -> None:
        self.control.write(self.current.primary.path.read_bytes())

    def advance(self) -> bool:
        if self.index + 1 >= len(self.groups):
            return False
        next_index = self.index + 1
        self.control.write(self.groups[next_index].primary.path.read_bytes())
        self.index = next_index
        return True

    def close(self) -> None:
        self.control.remove_if_owned()


class LiveControlWindow:
    def __init__(
        self,
        bundle: Bundle,
        groups: Sequence[CandidateGroup],
        control_path: Path,
        winner: Path,
        fingerprint: dict[str, int] | None = None,
    ) -> None:
        import tkinter as tk
        from tkinter import messagebox

        self.tk = tk
        self.messagebox = messagebox
        self.bundle = bundle
        self.winner = winner
        self.session = LiveCandidateSession(
            groups, AtomicLiveControl(control_path, fingerprint=fingerprint)
        )
        self.root = tk.Tk()
        try:
            self.session.start()
        except BaseException:
            self.session.close()
            self.root.destroy()
            raise
        self.root.title("Dreams D25 Live Shape Finder")
        self.root.geometry("620x170")
        self.root.resizable(False, False)
        self.label = tk.Label(self.root, font=("Segoe UI", 12), wraplength=580)
        self.label.pack(fill="x", padx=20, pady=(28, 18))
        controls = tk.Frame(self.root)
        controls.pack()
        tk.Button(controls, text="No", width=14, height=2, command=self.no).pack(side="left", padx=8)
        tk.Button(controls, text="Yes", width=14, height=2, command=self.yes).pack(side="left", padx=8)
        self.root.bind("<KeyPress-n>", lambda _event: self.no())
        self.root.bind("<KeyPress-y>", lambda _event: self.yes())
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.update_label()

    def update_label(self) -> None:
        group = self.session.current
        self.label.config(
            text=f"{self.session.index + 1} / {len(self.session.groups)}    {group.primary.name}"
        )

    def no(self) -> None:
        try:
            advanced = self.session.advance()
        except OSError as exc:
            self.messagebox.showerror("Cannot update Dreams", str(exc))
            return
        if not advanced:
            self.messagebox.showinfo("Shape finder", "No candidates left.")
            return
        self.update_label()

    def yes(self) -> None:
        group = self.session.current
        try:
            saved = save_winner(
                self.bundle,
                group.primary,
                self.winner,
                equivalents=group.equivalents,
            )
        except (OSError, ValueError) as exc:
            self.messagebox.showerror("Cannot save winner", str(exc))
            return
        self.session.close()
        self.messagebox.showinfo("Winner saved", str(saved))
        self.root.destroy()

    def close(self) -> None:
        self.session.close()
        self.root.destroy()

    def run(self) -> None:
        self.root.mainloop()


class FinderWindow:
    def __init__(
        self,
        bundle: Bundle,
        results: Sequence[ReplayResult],
        rejected: dict[str, str],
        winner: Path,
    ) -> None:
        import tkinter as tk
        from tkinter import messagebox

        self.tk = tk
        self.messagebox = messagebox
        self.bundle = bundle
        self.results = tuple(results)
        self.rejected = rejected
        self.winner = winner
        self.index = 0
        self.framing = shared_shape_framing(self.results[0])
        self.photo = None
        self.render_cache: dict[tuple[int, int, int], RasterImage] = {}
        self.render_job = None
        self.root = tk.Tk()
        self.root.title("Dreams D25 Shape Finder")
        self.root.geometry("900x700")
        self.label = tk.Label(self.root, font=("Segoe UI", 12))
        self.label.pack(pady=(10, 4))
        self.canvas = tk.Canvas(self.root, bg="#f5f5f2", highlightthickness=0)
        self.canvas.pack(fill="both", expand=True, padx=10, pady=6)
        controls = tk.Frame(self.root)
        controls.pack(pady=(4, 12))
        tk.Button(controls, text="No", width=14, height=2, command=self.no).pack(side="left", padx=8)
        tk.Button(controls, text="Yes", width=14, height=2, command=self.yes).pack(side="left", padx=8)
        self.root.bind("<KeyPress-n>", lambda _event: self.no())
        self.root.bind("<KeyPress-y>", lambda _event: self.yes())
        self.root.bind("<Configure>", lambda _event: self.schedule_draw())
        self.root.after_idle(self.draw)

    def schedule_draw(self) -> None:
        if self.render_job is not None:
            self.root.after_cancel(self.render_job)
        self.render_job = self.root.after(100, self.draw)

    def draw(self) -> None:
        self.render_job = None
        if not self.results:
            return
        result = self.results[self.index]
        self.label.config(
            text=f"Shared shape framing from current capture    |    "
            f"{self.index + 1} / {len(self.results)}    {result.candidate.name}"
            + (f"    ({len(self.rejected)} invalid skipped)" if self.rejected else "")
        )
        self.canvas.delete("all")
        canvas_width = max(self.canvas.winfo_width(), 100)
        canvas_height = max(self.canvas.winfo_height(), 100)
        # Pure-Python z buffering stays responsive at this resolution; Tk scales the
        # completed image 2x for display without changing any geometry or depth result.
        reduction = min(1.0, 320.0 / canvas_width, 240.0 / canvas_height)
        width = max(100, int(canvas_width * reduction))
        height = max(100, int(canvas_height * reduction))
        cache_key = self.index, width, height
        try:
            triangles = project_triangles(result)
            raster = self.render_cache.get(cache_key)
            if raster is None:
                raster = rasterize_projected(
                    triangles,
                    self.framing,
                    width,
                    height,
                    cull_mode=result.cull_mode,
                    front_face=result.front_face,
                )
                self.render_cache[cache_key] = raster
        except CandidateError as exc:
            self.canvas.create_text(20, 20, anchor="nw", text=str(exc), fill="#a11f32")
            return
        ppm = f"P6\n{raster.width} {raster.height}\n255\n".encode("ascii") + raster.pixels
        base_photo = self.tk.PhotoImage(data=ppm, format="PPM")
        zoom = max(
            1,
            min(
                2,
                int(canvas_width // raster.width),
                int(canvas_height // raster.height),
            ),
        )
        self.photo = base_photo if zoom == 1 else base_photo.zoom(zoom, zoom)
        self.canvas.create_image(canvas_width * 0.5, canvas_height * 0.5, image=self.photo)
        if not triangles:
            self.canvas.create_text(
                20,
                20,
                anchor="nw",
                text="All captured primitives are clipped by the current corrupt data.",
                fill="#101820",
            )

    def no(self) -> None:
        if self.index + 1 >= len(self.results):
            self.messagebox.showinfo("Shape finder", "No candidates left.")
            return
        self.index += 1
        self.draw()

    def yes(self) -> None:
        try:
            saved = save_winner(self.bundle, self.results[self.index].candidate, self.winner)
        except (OSError, ValueError) as exc:
            self.messagebox.showerror("Cannot save winner", str(exc))
            return
        self.messagebox.showinfo("Winner saved", str(saved))
        self.root.destroy()

    def run(self) -> None:
        self.root.mainloop()


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path, help="bundle directory or bundle.json")
    parser.add_argument("--winner", type=Path, help="output directory for the exact winning bundle")
    parser.add_argument("--validate", action="store_true", help="validate and replay every candidate without UI")
    parser.add_argument(
        "--live-control",
        type=Path,
        help="atomically publish each raw selector candidate here for the running emulator",
    )
    parser.add_argument(
        "--live-fingerprint-manifest",
        type=Path,
        help="append exact cheap draw-identity fields from a capture manifest.tsv",
    )
    args = parser.parse_args(argv)
    if args.live_fingerprint_manifest is not None and args.live_control is None:
        parser.error("--live-fingerprint-manifest requires --live-control")
    try:
        bundle = load_bundle(args.bundle)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if args.validate:
        try:
            results, rejected = prepare_candidates(bundle)
        except (OSError, ValueError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        print(f"valid={len(results)} rejected={len(rejected)}")
        for name, reason in rejected.items():
            print(f"rejected\t{name}\t{reason}")
        return 0
    winner = args.winner or (bundle.root / "winner.bundle")
    if args.live_control is not None:
        try:
            groups = deduplicate_candidates(bundle.candidates)
            fingerprint = (
                load_live_fingerprint_manifest(args.live_fingerprint_manifest)
                if args.live_fingerprint_manifest is not None
                else None
            )
            LiveControlWindow(
                bundle, groups, args.live_control, winner, fingerprint=fingerprint
            ).run()
        except (OSError, ValueError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        return 0
    try:
        results, rejected = prepare_candidates(bundle)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    FinderWindow(bundle, results, rejected, winner).run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
