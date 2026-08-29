"""Strict loader and candidate-frame builder for an aligned Dreams capture."""

from __future__ import annotations

import dataclasses
import json
import math
import struct
from pathlib import Path
from typing import Mapping

from ordered_replay import (
    ALIGNED_CHAIN_REQUIRED,
    Pair,
    ReplayScenario,
    candidates_for,
    file_sha256,
    replay,
    replay_d8,
)


@dataclasses.dataclass(frozen=True)
class AlignedCapture:
    directory: Path
    metadata: Mapping[str, object]
    gds_pre: tuple[int, ...]
    gds_post_d8: tuple[int, ...]
    gds_post_list: tuple[int, ...]
    d8_input: tuple[int, ...]
    d8_output_selectors: tuple[int, ...]
    lookup: tuple[Pair, ...]
    actual_output: tuple[Pair, ...]
    valid_reference_ids: frozenset[int]
    candidate_records: tuple[Pair, ...]
    candidate_interfaces: tuple[tuple[float, ...], ...]
    baseline_interface: tuple[float, ...]
    baseline_validity: tuple[int, ...]
    indices: tuple[int, ...]
    draw_instance_count: int
    vertices_per_instance: int
    attributes_per_vertex: int


@dataclasses.dataclass(frozen=True)
class CandidateFrame:
    name: str
    configuration: Mapping[str, object]
    pairs: tuple[Pair, ...]
    interfaces: tuple[tuple[float, ...], ...]
    indices: tuple[int, ...]
    vertices_per_instance: int
    attributes_per_vertex: int
    diagnostics: Mapping[str, object]

    def positions(self) -> tuple[tuple[tuple[float, float, float, float], ...], ...]:
        result = []
        for interface in self.interfaces:
            points = []
            for vertex in range(self.vertices_per_instance):
                offset = vertex * self.attributes_per_vertex * 4
                points.append(tuple(interface[offset : offset + 4]))
            result.append(tuple(points))
        return tuple(result)


@dataclasses.dataclass(frozen=True)
class FrameBuildReport:
    frames: tuple[CandidateFrame, ...]
    rejected: tuple[Mapping[str, object], ...]


def _read_u32(path: Path) -> tuple[int, ...]:
    data = path.read_bytes()
    if len(data) % 4:
        raise ValueError(f"{path} is not a whole number of little-endian u32 values")
    return tuple(value[0] for value in struct.iter_unpack("<I", data))


def _read_pairs(path: Path) -> tuple[Pair, ...]:
    data = path.read_bytes()
    if len(data) % 8:
        raise ValueError(f"{path} is not a whole number of 8-byte records")
    return tuple(Pair(*value) for value in struct.iter_unpack("<II", data))


def _read_f32(path: Path) -> tuple[float, ...]:
    data = path.read_bytes()
    if len(data) % 4:
        raise ValueError(f"{path} is not float32-aligned")
    return tuple(value[0] for value in struct.iter_unpack("<f", data))


def _require_integer(metadata: Mapping[str, object], name: str, minimum: int = 0) -> int:
    value = metadata.get(name)
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum:
        raise ValueError(f"dispatch.json field {name!r} must be an integer >= {minimum}")
    return value


def _verify_manifest(directory: Path, metadata: Mapping[str, object]) -> None:
    manifest = metadata.get("files")
    if not isinstance(manifest, dict):
        raise ValueError("dispatch.json must contain a files object with byte counts and SHA-256 values")
    for name in ALIGNED_CHAIN_REQUIRED:
        if name == "dispatch.json":
            continue
        entry = manifest.get(name)
        if not isinstance(entry, dict):
            raise ValueError(f"dispatch.json files object is missing {name!r}")
        expected_bytes = entry.get("bytes")
        expected_hash = entry.get("sha256")
        if not isinstance(expected_bytes, int) or expected_bytes < 0:
            raise ValueError(f"dispatch.json has an invalid byte count for {name}")
        if not isinstance(expected_hash, str) or len(expected_hash) != 64:
            raise ValueError(f"dispatch.json has an invalid SHA-256 for {name}")
        path = directory / name
        actual_bytes = path.stat().st_size
        if actual_bytes != expected_bytes:
            raise ValueError(f"{path} has {actual_bytes} bytes; dispatch.json records {expected_bytes}")
        actual_hash = file_sha256(path)
        if actual_hash.lower() != expected_hash.lower():
            raise ValueError(f"{path} SHA-256 does not match dispatch.json")


def load_aligned_capture(directory: Path) -> AlignedCapture:
    directory = directory.resolve()
    missing = [str(directory / name) for name in ALIGNED_CHAIN_REQUIRED if not (directory / name).is_file()]
    if missing:
        raise FileNotFoundError(
            "cannot render ordered-count candidates from this capture; missing aligned file(s): "
            + ", ".join(missing)
        )
    metadata = json.loads((directory / "dispatch.json").read_text(encoding="utf-8"))
    if not isinstance(metadata, dict) or metadata.get("schema") != 1:
        raise ValueError("dispatch.json must be an object with schema=1")
    shader = metadata.get("shader")
    if shader not in {"7aa", "016"}:
        raise ValueError("dispatch.json shader must be '7aa' or '016'")
    _verify_manifest(directory, metadata)

    active_count = _require_integer(metadata, "active_count")
    vertices = _require_integer(metadata, "vertices_per_instance", 1)
    attributes = _require_integer(metadata, "attributes_per_vertex", 1)
    index_size = _require_integer(metadata, "index_size", 1)
    if attributes < 1 or index_size not in {2, 4}:
        raise ValueError("dispatch.json has an unsupported attribute count or index size")

    d8_input = _read_u32(directory / "d8-input.bin")
    selectors = _read_u32(directory / "d8-output-selectors.bin")
    lookup = _read_pairs(directory / "7aa-lookup.bin")
    actual_output = _read_pairs(directory / "7aa-output.bin")
    valid_refs = frozenset(_read_u32(directory / "valid-reference-ids.bin"))
    candidate_records = _read_pairs(directory / "candidate-records.bin")
    candidate_values = _read_f32(directory / "candidate-interface.bin")
    gds_pre = _read_u32(directory / "gds-pre.bin")
    gds_post_d8 = _read_u32(directory / "gds-post-d8.bin")
    gds_post_list = _read_u32(directory / "gds-post-7aa.bin")
    required_gds_index = 352 if shader == "7aa" else 354
    if len(gds_pre) <= required_gds_index or len(gds_post_d8) <= 350 or len(gds_post_list) <= required_gds_index:
        raise ValueError("captured GDS files do not include the ordered-counter dwords")
    values_per_geometry = vertices * attributes * 4
    if len(candidate_values) != len(candidate_records) * values_per_geometry:
        raise ValueError(
            f"{directory / 'candidate-interface.bin'} has {len(candidate_values)} float32 values; "
            f"expected {len(candidate_records) * values_per_geometry} from candidate-records.bin"
        )
    candidate_interfaces = tuple(
        tuple(candidate_values[index : index + values_per_geometry])
        for index in range(0, len(candidate_values), values_per_geometry)
    )
    if any(not math.isfinite(value) for value in candidate_values):
        raise ValueError(f"{directory / 'candidate-interface.bin'} contains NaN or infinity")

    draw_data = (directory / "draw-indirect.bin").read_bytes()
    if len(draw_data) != 20:
        raise ValueError(f"{directory / 'draw-indirect.bin'} must be exactly 20 bytes")
    _index_count, draw_instances, _first_index, _vertex_offset, _first_instance = struct.unpack(
        "<IIIII", draw_data
    )
    baseline_interface = _read_f32(directory / "vs370-interface.bin")
    expected_baseline_values = draw_instances * values_per_geometry
    if len(baseline_interface) != expected_baseline_values:
        raise ValueError(
            f"{directory / 'vs370-interface.bin'} has {len(baseline_interface)} float32 values; "
            f"expected {expected_baseline_values} for the captured draw"
        )
    baseline_validity = _read_u32(directory / "vs370-validity.bin")
    if len(baseline_validity) != draw_instances * vertices:
        raise ValueError(f"{directory / 'vs370-validity.bin'} does not cover every baseline vertex")
    if active_count > len(selectors) or active_count > len(d8_input):
        raise ValueError("active_count exceeds d8-input.bin or d8-output-selectors.bin")
    if draw_instances > len(actual_output):
        raise ValueError("draw instance count exceeds 7aa-output.bin records")
    index_bytes = (directory / "indices.bin").read_bytes()
    if len(index_bytes) % index_size:
        raise ValueError(f"{directory / 'indices.bin'} does not match index_size")
    index_format = "<H" if index_size == 2 else "<I"
    indices = tuple(value[0] for value in struct.iter_unpack(index_format, index_bytes))

    return AlignedCapture(
        directory=directory,
        metadata=metadata,
        gds_pre=gds_pre,
        gds_post_d8=gds_post_d8,
        gds_post_list=gds_post_list,
        d8_input=d8_input,
        d8_output_selectors=selectors,
        lookup=lookup,
        actual_output=actual_output,
        valid_reference_ids=valid_refs,
        candidate_records=candidate_records,
        candidate_interfaces=candidate_interfaces,
        baseline_interface=baseline_interface,
        baseline_validity=baseline_validity,
        indices=indices,
        draw_instance_count=draw_instances,
        vertices_per_instance=vertices,
        attributes_per_vertex=attributes,
    )


def _geometry_bank(capture: AlignedCapture) -> dict[Pair, tuple[float, ...]]:
    bank: dict[Pair, tuple[float, ...]] = {}
    for pair, interface in zip(capture.candidate_records, capture.candidate_interfaces):
        existing = bank.get(pair)
        if existing is not None and existing != interface:
            raise ValueError(
                "candidate-records.bin contains one record identity with conflicting captured geometry"
            )
        bank[pair] = interface
    return bank


def _scenario_for(capture: AlignedCapture, selectors: tuple[int, ...]) -> ReplayScenario:
    metadata = capture.metadata
    active_count = int(metadata["active_count"])
    shader = str(metadata["shader"])
    group_size = int(metadata.get("group_size", 64))
    arrival_order = tuple(int(value) for value in metadata.get("arrival_order", ()))
    initial_a = int(metadata.get("initial_a", 0))
    initial_b = int(metadata.get("initial_b", active_count))
    counter_indices = (351, 352) if shader == "7aa" else (353, 354)
    expected_a = metadata.get("expected_counter_a", capture.gds_post_list[counter_indices[0]])
    expected_b = metadata.get("expected_counter_b", capture.gds_post_list[counter_indices[1]])
    common = dict(
        name=f"aligned_{shader}",
        shader=shader,
        active_count=active_count,
        arrival_order=arrival_order,
        group_size=group_size,
        initial_a=initial_a,
        initial_b=initial_b,
        output_capacity=len(capture.actual_output),
        expected_counter_a=int(expected_a) if isinstance(expected_a, int) else None,
        expected_counter_b=int(expected_b) if isinstance(expected_b, int) else None,
        expected_draw_count=capture.draw_instance_count,
        valid_ref_ids=capture.valid_reference_ids,
        # Unique keys avoid claiming stable-key evidence that the capture did
        # not decode. d8 itself performs the proven low-ten-bit stable check.
        stable_keys=tuple(range(active_count)),
    )
    if shader == "7aa":
        return ReplayScenario(
            **common,
            selectors=selectors[:active_count],
            lookup=tuple(capture.lookup),
        )
    return ReplayScenario(
        **common,
        direct_records=tuple(capture.lookup[:active_count]),
    )


def _make_frame(
    capture: AlignedCapture,
    name: str,
    configuration: Mapping[str, object],
    result,
    bank: Mapping[Pair, tuple[float, ...]],
) -> CandidateFrame | Mapping[str, object]:
    if result.metrics.rejected:
        return {
            "name": name,
            "reason": "invariant rejection",
            "metrics": dataclasses.asdict(result.metrics),
        }
    pairs = tuple(result.output.get(slot) for slot in range(capture.draw_instance_count))
    if any(pair is None for pair in pairs):
        return {"name": name, "reason": "output hole inside draw instance range"}
    concrete_pairs = tuple(pair for pair in pairs if pair is not None)
    missing_geometry = sorted({pair.as_hex() for pair in concrete_pairs if pair not in bank})
    if missing_geometry:
        return {
            "name": name,
            "reason": "candidate geometry was not captured",
            "missing_records": missing_geometry,
            "required_file": str(capture.directory / "candidate-interface.bin"),
        }
    interfaces = tuple(bank[pair] for pair in concrete_pairs)
    return CandidateFrame(
        name=name,
        configuration=configuration,
        pairs=concrete_pairs,
        interfaces=interfaces,
        indices=capture.indices,
        vertices_per_instance=capture.vertices_per_instance,
        attributes_per_vertex=capture.attributes_per_vertex,
        diagnostics=result.as_dict(),
    )


def build_candidate_frames(capture: AlignedCapture) -> FrameBuildReport:
    bank = _geometry_bank(capture)
    frames: list[CandidateFrame] = []
    rejected: list[Mapping[str, object]] = []
    scenario = _scenario_for(capture, capture.d8_output_selectors)

    for list_candidate in candidates_for(str(capture.metadata["shader"])):
        result = replay(scenario, list_candidate)
        built = _make_frame(
            capture,
            f"captured d8 + {list_candidate.name}",
            {"d8": "captured_output", "visibility_list": dataclasses.asdict(list_candidate)},
            result,
            bank,
        )
        (frames if isinstance(built, CandidateFrame) else rejected).append(built)

    if capture.metadata["shader"] == "7aa":
        pass_count = int(capture.metadata.get("d8_pass_count", 10))
        group_size = int(capture.metadata.get("group_size", 64))
        arrival = tuple(int(value) for value in capture.metadata.get("arrival_order", ()))
        exact_list = candidates_for("7aa")[0]
        for d8_candidate in candidates_for("d8"):
            if d8_candidate.name == "ps4_exact":
                # The exact/exact configuration is already represented above
                # with the captured pass-9 output.
                continue
            d8_result = replay_d8(
                capture.d8_input[: int(capture.metadata["active_count"])],
                d8_candidate,
                pass_count=pass_count,
                group_size=group_size,
                arrival_order=arrival,
            )
            if d8_result.rejected or any(item is None for item in d8_result.output):
                rejected.append(
                    {
                        "name": f"{d8_candidate.name} + ps4_exact",
                        "reason": "d8 invariant rejection",
                        "d8": d8_result.as_dict(),
                    }
                )
                continue
            selectors = tuple(item.value for item in d8_result.output if item is not None)
            d8_scenario = _scenario_for(capture, selectors)
            result = replay(d8_scenario, exact_list)
            built = _make_frame(
                capture,
                f"{d8_candidate.name} + ps4_exact",
                {"d8": dataclasses.asdict(d8_candidate), "visibility_list": dataclasses.asdict(exact_list)},
                result,
                bank,
            )
            (frames if isinstance(built, CandidateFrame) else rejected).append(built)

    # Repeated configs can produce exactly the same record sequence. Showing
    # those as different pictures would be misleading, so retain one.
    unique: list[CandidateFrame] = []
    seen_sequences: set[tuple[Pair, ...]] = set()
    for frame in frames:
        if frame.pairs in seen_sequences:
            rejected.append({"name": frame.name, "reason": "identical reconstruction already listed"})
            continue
        seen_sequences.add(frame.pairs)
        unique.append(frame)
    return FrameBuildReport(tuple(unique), tuple(rejected))
