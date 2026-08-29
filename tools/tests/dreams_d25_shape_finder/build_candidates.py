#!/usr/bin/env python3
"""Build only metadata-authorized D25 selector candidates from captured words."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import struct
import sys
from pathlib import Path
from typing import Any, Sequence


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _safe_name(value: str) -> str:
    result = re.sub(r"[^A-Za-z0-9_.-]+", "-", value).strip("-.")
    if not result:
        raise ValueError("candidate name has no filename-safe characters")
    return result


def _words(data: bytes) -> tuple[int, ...]:
    if not data or len(data) % 4:
        raise ValueError("selector source must contain whole little-endian 32-bit words")
    return struct.unpack(f"<{len(data) // 4}I", data)


def _pack(words: Sequence[int]) -> bytes:
    return struct.pack(f"<{len(words)}I", *(value & 0xFFFFFFFF for value in words))


def _required_u32(source: dict[str, Any], name: str) -> int:
    value = source.get(name)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0 or value > 0xFFFFFFFF:
        raise ValueError(f"{source.get('name', 'source')}.{name} must be a captured u32")
    return value


def _replay_1e7(
    source: dict[str, Any], plan_path: Path
) -> tuple[tuple[int, ...], dict[str, Any], dict[str, Any]]:
    word29_value = source.get("active_b2_word29_path")
    counts_value = source.get("pre902_counts_path")
    if not isinstance(word29_value, str) or not word29_value:
        raise ValueError("d25_1e7_replay requires active_b2_word29_path")
    if not isinstance(counts_value, str) or not counts_value:
        raise ValueError("d25_1e7_replay requires pre902_counts_path")
    word29_path = (plan_path.parent / word29_value).resolve()
    counts_path = (plan_path.parent / counts_value).resolve()
    if not word29_path.is_file() or not counts_path.is_file():
        raise ValueError("d25_1e7_replay input file is missing")
    word29 = _words(word29_path.read_bytes())
    counters = list(_words(counts_path.read_bytes()))
    read_const_2 = _required_u32(source, "read_const_2")
    read_const_12 = _required_u32(source, "read_const_12")
    read_const_13 = _required_u32(source, "read_const_13")
    gds_160 = _required_u32(source, "gds_160")
    stride = read_const_13 & 0x00FFFFFF
    active_count = min(read_const_12, gds_160)
    if stride == 0 or read_const_2 == 0:
        raise ValueError("d25_1e7_replay captured stride and capacity must be nonzero")
    if len(word29) != active_count:
        raise ValueError(
            f"active-b2-word29 has {len(word29)} words but min(ReadConst#12,GDS160) is {active_count}"
        )
    output: list[int | None] = [None] * read_const_2
    for thread_id, value in enumerate(word29):
        byte_8_15 = (value >> 8) & 0xFF
        base = byte_8_15 * 7 + ((value >> 1) & 7) if byte_8_15 < 128 else 896
        mask = (value >> 4) & 0xF
        while mask:
            bit = (mask & -mask).bit_length() - 1
            counter_index = base + bit * stride
            if counter_index >= len(counters):
                raise ValueError(
                    f"pre902 counts do not contain 1e7 counter index {counter_index}"
                )
            old = counters[counter_index]
            counters[counter_index] = (old + 1) & 0xFFFFFFFF
            if old < read_const_2:
                output[old] = thread_id
            mask &= mask - 1
    written = [index for index, value in enumerate(output) if value is not None]
    if not written:
        raise ValueError("d25_1e7_replay produced no selectors")
    last = written[-1]
    if any(value is None for value in output[: last + 1]):
        raise ValueError(
            "d25_1e7_replay output has holes; an exact initial b0 snapshot is required"
        )
    selectors = tuple(value for value in output[: last + 1] if value is not None)
    config = {
        "kind": "d25_1e7_replay",
        "atomic_order": "canonical_sequential_thread_id",
        "confidence": "reconstructed_lower_than_exact_capture",
        "read_const_2": read_const_2,
        "read_const_12": read_const_12,
        "read_const_13": read_const_13,
        "gds_160": gds_160,
        "active_count": active_count,
    }
    inputs = {
        "active_b2_word29_sha256": _sha256(word29_path),
        "active_b2_word29_file": word29_path.name,
        "pre902_counts_sha256": _sha256(counts_path),
        "pre902_counts_file": counts_path.name,
    }
    return selectors, config, inputs


def _interpret(
    source: dict[str, Any], raw: tuple[int, ...]
) -> list[tuple[str, tuple[int, ...], dict[str, Any], int]]:
    storage = source.get("storage")
    if not isinstance(storage, dict):
        raise ValueError("source.storage must be an object")
    kind = storage.get("kind")
    if kind == "u32":
        return [(source["name"], raw, {"kind": "raw_u32"}, 0)]
    if kind == "f32":
        output: list[int] = []
        for bits in raw:
            value = struct.unpack("<f", struct.pack("<I", bits))[0]
            if not math.isfinite(value) or value < 0.0 or value > 4294967295.0:
                raise ValueError(f"{source['name']} contains a float that cannot convert to u32")
            output.append(int(value))
        return [(
            source["name"], tuple(output),
            {"kind": "float32_to_u32", "rounding": "toward_zero"},
            2,
        )]
    if kind == "packed_u32":
        channels = storage.get("channels")
        if not isinstance(channels, list) or not channels:
            raise ValueError("packed_u32 storage requires channels")
        output = []
        for channel in channels:
            if not isinstance(channel, dict):
                raise ValueError("each packed channel must be an object")
            name = channel.get("name")
            shift = channel.get("shift")
            bits = channel.get("bits")
            if not isinstance(name, str) or not name:
                raise ValueError("packed channel name is required")
            if isinstance(shift, bool) or not isinstance(shift, int) or shift < 0 or shift > 31:
                raise ValueError("packed channel shift must be in 0..31")
            if isinstance(bits, bool) or not isinstance(bits, int) or bits < 1 or bits > 32 - shift:
                raise ValueError("packed channel bits do not fit the source word")
            mask = 0xFFFFFFFF if bits == 32 else (1 << bits) - 1
            values = tuple((word >> shift) & mask for word in raw)
            output.append((
                f"{source['name']}:{name}", values,
                {"kind": "packed_u32_channel", "channel": name, "shift": shift, "bits": bits},
                2,
            ))
        return output
    raise ValueError("source.storage.kind must be u32, f32, or packed_u32")


def build(bundle_path: Path | str, plan_path: Path | str) -> dict[str, Any]:
    bundle_path = Path(bundle_path).resolve()
    plan_path = Path(plan_path).resolve()
    document = json.loads(bundle_path.read_text(encoding="utf-8"))
    plan = json.loads(plan_path.read_text(encoding="utf-8"))
    if document.get("schema") != 1 or document.get("shader_hash") not in ("0xd25db925", "0xD25DB925", 0xD25DB925):
        raise ValueError("bundle is not schema 1 for shader 0xd25db925")
    if plan.get("schema") != 1 or not isinstance(plan.get("sources"), list) or not plan["sources"]:
        raise ValueError("plan must be schema 1 with non-empty sources")
    output_dir = bundle_path.parent / "candidates"
    output_dir.mkdir(exist_ok=True)
    ranked_candidates: list[tuple[int, int, int, dict[str, Any]]] = []
    used: set[str] = set()
    for source_ordinal, source in enumerate(plan["sources"]):
        if not isinstance(source, dict):
            raise ValueError("each source must be an object")
        name = source.get("name")
        provenance = source.get("provenance")
        if not isinstance(name, str) or not name:
            raise ValueError("source.name is required")
        if not isinstance(provenance, dict) or not provenance:
            raise ValueError(f"{name} requires non-empty provenance")
        storage = source.get("storage")
        if isinstance(storage, dict) and storage.get("kind") == "d25_1e7_replay":
            values, config, replay_inputs = _replay_1e7(source, plan_path)
            variants = [(name, values, config, 1)]
            source_digest = replay_inputs["active_b2_word29_sha256"]
            source_file = replay_inputs["active_b2_word29_file"]
        else:
            source_path_value = source.get("path")
            if not isinstance(source_path_value, str) or not source_path_value:
                raise ValueError(f"{name} requires a path")
            source_path = (plan_path.parent / source_path_value).resolve()
            if not source_path.is_file():
                raise ValueError(f"missing selector source: {source_path}")
            raw = _words(source_path.read_bytes())
            source_digest = _sha256(source_path)
            source_file = source_path.name
            variants = _interpret(source, raw)
            replay_inputs = {}
        for variant_ordinal, (candidate_name, values, config, rank) in enumerate(variants):
            if candidate_name in used:
                raise ValueError(f"duplicate generated candidate name: {candidate_name}")
            used.add(candidate_name)
            filename = f"{source_ordinal:03d}-{variant_ordinal:02d}-{_safe_name(candidate_name)}.bin"
            target = output_dir / filename
            target.write_bytes(_pack(values))
            exact_provenance = dict(provenance)
            exact_provenance["source_sha256"] = source_digest
            exact_provenance["source_file"] = source_file
            exact_provenance.update(replay_inputs)
            candidate = {
                "name": candidate_name,
                "selectors": {
                    "path": target.relative_to(bundle_path.parent).as_posix(),
                    "sha256": _sha256(target),
                },
                "config": config,
                "provenance": exact_provenance,
            }
            ranked_candidates.append((rank, source_ordinal, variant_ordinal, candidate))
    candidates = [item[3] for item in sorted(ranked_candidates, key=lambda item: item[:3])]
    document["candidates"] = candidates
    bundle_path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return document


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path, help="existing bundle.json with the shared real fixture")
    parser.add_argument("plan", type=Path, help="selector-sources.json")
    args = parser.parse_args(argv)
    try:
        document = build(args.bundle, args.plan)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(f"wrote {len(document['candidates'])} candidates to {args.bundle}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
