#!/usr/bin/env python3
"""Deterministic, offline replay for the Dreams ordered visibility-list chain.

This module intentionally has no shadPS4 imports and never starts the game.  It
models the finite set of address, ordering, and counter interpretations that
have actually been at issue in the Dreams investigation.  A candidate is only
accepted when it preserves all observable invariants; matching a final count is
not sufficient.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import hashlib
import json
import math
import os
import re
import struct
import sys
from pathlib import Path
from typing import Iterable, Mapping, Sequence


U32_MASK = 0xFFFFFFFF
MAX_GUEST_RECORDS = 1 << 20


@dataclasses.dataclass(frozen=True, order=True)
class Pair:
    """One 8-byte Dreams visibility record."""

    lo: int
    hi: int

    def __post_init__(self) -> None:
        object.__setattr__(self, "lo", self.lo & U32_MASK)
        object.__setattr__(self, "hi", self.hi & U32_MASK)

    @property
    def flags(self) -> int:
        return (self.hi >> 16) & 3

    @property
    def low24_ref(self) -> int:
        return self.lo & 0xFFFFFF

    def as_hex(self) -> tuple[str, str]:
        return f"0x{self.lo:08x}", f"0x{self.hi:08x}"


@dataclasses.dataclass(frozen=True)
class CandidateSemantics:
    """A deliberately finite interpretation of the guest operations."""

    name: str
    description: str
    shader: str
    address_mode: str = "guest_dword"
    wave_order: str = "creation"
    lane_order: str = "lane"
    counter_seed: str = "guest"
    post_order: str = "none"


def candidates_for(shader: str) -> tuple[CandidateSemantics, ...]:
    """Return the finite, evidence-motivated candidate set.

    The alternatives correspond to real implementation ambiguities: buffer
    offsets expressed as dwords versus bytes/descriptor elements, dispatch
    completion versus creation order, lane-prefix order, and GDS counter seeds.
    This is not an unconstrained parameter search.
    """

    if shader == "d8":
        return (
            CandidateSemantics(
                "ps4_exact",
                "Ten stable low-bit partitions in workgroup creation and lane-prefix order",
                "d8",
            ),
            CandidateSemantics(
                "completion_order",
                "Each pass reserves one-bit partitions in observed workgroup completion order",
                "d8",
                wave_order="arrival",
            ),
            CandidateSemantics(
                "reverse_workgroups",
                "Each pass reserves one-bit partitions in reverse workgroup order",
                "d8",
                wave_order="reverse",
            ),
            CandidateSemantics(
                "reverse_lanes",
                "Each wave computes its one-bit prefix in reverse lane order",
                "d8",
                lane_order="reverse",
            ),
            CandidateSemantics(
                "unstable_equal_keys",
                "Correct ten-pass output followed by an unstable reorder of equal radix keys",
                "d8",
                post_order="unstable_equal_keys",
            ),
        )

    common = (
        CandidateSemantics(
            "ps4_exact",
            "Guest dword addressing, workgroup creation order, lane-stable prefix, guest counter seeds",
            shader,
        ),
        CandidateSemantics(
            "completion_order",
            "Correct addresses but workgroups reserve space in observed completion order",
            shader,
            wave_order="arrival",
        ),
        CandidateSemantics(
            "reverse_workgroups",
            "Correct addresses but workgroups reserve space in reverse order",
            shader,
            wave_order="reverse",
        ),
        CandidateSemantics(
            "reverse_lanes",
            "Correct addresses but the ballot prefix is interpreted in reverse lane order",
            shader,
            lane_order="reverse",
        ),
        CandidateSemantics(
            "zero_both_counters",
            "Both output streams start at zero instead of the guest-provided second-stream seed",
            shader,
            counter_seed="both_zero",
        ),
        CandidateSemantics(
            "swapped_counter_seeds",
            "The first and second ordered counters use each other's initial ranges",
            shader,
            counter_seed="swapped",
        ),
        CandidateSemantics(
            "unstable_equal_keys",
            "Correct compaction followed by an unstable equal-key reorder",
            shader,
            post_order="unstable_equal_keys",
        ),
    )
    if shader == "016":
        return common
    if shader != "7aa":
        raise ValueError(f"unsupported shader family: {shader!r}")
    return common + (
        CandidateSemantics(
            "byte_offset_confusion",
            "The shader's dword offset is incorrectly consumed as a byte offset",
            shader,
            address_mode="guest_value_as_bytes",
        ),
        CandidateSemantics(
            "descriptor_element_confusion",
            "The shader's dword offset is incorrectly scaled again by the 8-byte descriptor stride",
            shader,
            address_mode="guest_value_as_element",
        ),
        CandidateSemantics(
            "unshifted_selector",
            "The packed selector is used directly instead of applying the guest's right shift by ten",
            shader,
            address_mode="unshifted_selector",
        ),
    )


@dataclasses.dataclass(frozen=True)
class ReplayScenario:
    name: str
    shader: str
    active_count: int
    lookup: tuple[Pair | None, ...] = ()
    selectors: tuple[int, ...] = ()
    direct_records: tuple[Pair, ...] = ()
    arrival_order: tuple[int, ...] = ()
    group_size: int = 64
    initial_a: int = 0
    initial_b: int | None = None
    output_capacity: int | None = None
    expected_counter_a: int | None = None
    expected_counter_b: int | None = None
    expected_output: Mapping[int, Pair] = dataclasses.field(default_factory=dict)
    expected_draw_count: int | None = None
    valid_ref_ids: frozenset[int] | None = None
    stable_keys: tuple[int, ...] = ()
    spatial_by_ref: Mapping[int, tuple[float, float, float]] = dataclasses.field(default_factory=dict)
    spatial_max_jump: float | None = None

    def __post_init__(self) -> None:
        if self.shader not in {"7aa", "016"}:
            raise ValueError("shader must be '7aa' or '016'")
        if self.active_count < 0:
            raise ValueError("active_count cannot be negative")
        if self.group_size <= 0 or self.group_size > 64:
            raise ValueError("group_size must be between 1 and 64")
        available = len(self.selectors) if self.shader == "7aa" else len(self.direct_records)
        if self.active_count > available:
            raise ValueError(
                f"active_count={self.active_count} exceeds captured {self.shader} inputs={available}"
            )
        if self.stable_keys and len(self.stable_keys) < self.active_count:
            raise ValueError("stable_keys does not cover active inputs")


@dataclasses.dataclass(frozen=True)
class OutputWrite:
    slot: int
    source_index: int
    stream: str
    pair: Pair
    stable_key: int


@dataclasses.dataclass(frozen=True)
class ReplayMetrics:
    holes: int = 0
    slot_collisions: int = 0
    duplicate_refs: int = 0
    invalid_refs: int = 0
    count_mismatch: int = 0
    output_mismatch: int = 0
    stable_order_breaks: int = 0
    spatial_invalid: int = 0
    spatial_large_jumps: int = 0

    @property
    def score(self) -> int:
        return (
            self.invalid_refs * 100_000
            + self.spatial_invalid * 100_000
            + self.slot_collisions * 50_000
            + self.holes * 25_000
            + self.count_mismatch * 5_000
            + self.output_mismatch * 1_000
            + self.duplicate_refs * 500
            + self.stable_order_breaks * 100
            + self.spatial_large_jumps * 10
        )

    @property
    def rejected(self) -> bool:
        return any(
            (
                self.holes,
                self.slot_collisions,
                self.duplicate_refs,
                self.invalid_refs,
                self.count_mismatch,
                self.output_mismatch,
                self.stable_order_breaks,
                self.spatial_invalid,
            )
        )


@dataclasses.dataclass(frozen=True)
class ReplayResult:
    candidate: CandidateSemantics
    metrics: ReplayMetrics
    counter_a: int
    counter_b: int
    output: Mapping[int, Pair]
    writes: tuple[OutputWrite, ...]
    reasons: tuple[str, ...]

    def as_dict(self) -> dict[str, object]:
        return {
            "candidate": dataclasses.asdict(self.candidate),
            "accepted": not self.metrics.rejected,
            "score": self.metrics.score,
            "metrics": dataclasses.asdict(self.metrics),
            "counter_a": self.counter_a,
            "counter_b": self.counter_b,
            "reasons": list(self.reasons),
            "written_slots": len(self.output),
        }


@dataclasses.dataclass(frozen=True)
class D8Item:
    value: int
    source_index: int

    def __post_init__(self) -> None:
        object.__setattr__(self, "value", self.value & U32_MASK)


@dataclasses.dataclass(frozen=True)
class D8PassMetrics:
    pass_index: int
    ones: int
    holes: int
    slot_collisions: int
    out_of_range: int
    counter_mismatch: int


@dataclasses.dataclass(frozen=True)
class D8ReplayResult:
    candidate: CandidateSemantics
    output: tuple[D8Item | None, ...]
    passes: tuple[D8PassMetrics, ...]
    output_mismatch: int
    stable_order_breaks: int

    @property
    def score(self) -> int:
        return (
            sum(item.out_of_range for item in self.passes) * 100_000
            + sum(item.slot_collisions for item in self.passes) * 50_000
            + sum(item.holes for item in self.passes) * 25_000
            + sum(item.counter_mismatch for item in self.passes) * 5_000
            + self.output_mismatch * 1_000
            + self.stable_order_breaks * 100
        )

    @property
    def rejected(self) -> bool:
        return self.score != 0

    def as_dict(self) -> dict[str, object]:
        return {
            "candidate": dataclasses.asdict(self.candidate),
            "accepted": not self.rejected,
            "score": self.score,
            "passes": [dataclasses.asdict(item) for item in self.passes],
            "output_mismatch": self.output_mismatch,
            "stable_order_breaks": self.stable_order_breaks,
        }


def _d8_groups(item_count: int, group_size: int, candidate: CandidateSemantics,
               arrival_order: Sequence[int]) -> list[int]:
    group_count = (item_count + group_size - 1) // group_size
    creation = list(range(group_count))
    if candidate.wave_order == "creation":
        return creation
    if candidate.wave_order == "reverse":
        return list(reversed(creation))
    if candidate.wave_order == "arrival":
        if arrival_order:
            if sorted(arrival_order) != creation:
                raise ValueError("d8 arrival_order must contain every workgroup exactly once")
            return list(arrival_order)
        return list(reversed(creation))
    raise ValueError(f"unsupported d8 wave order {candidate.wave_order!r}")


def replay_d8(
    values: Sequence[int],
    candidate: CandidateSemantics,
    *,
    pass_count: int = 10,
    group_size: int = 64,
    arrival_order: Sequence[int] = (),
) -> D8ReplayResult:
    """Replay d8's ten stable one-bit partitions.

    The output-index equations mirror the decoded guest IR:

      zero: source_index - global_one_prefix
      one:  (active_count - total_ones) + global_one_prefix

    A workgroup prefix assigned in host arrival order therefore causes real
    holes/collisions; it cannot be excused merely because the final DOC counter
    still equals total_ones.
    """

    if candidate.shader != "d8":
        raise ValueError("d8 replay requires a d8 candidate")
    if not 1 <= pass_count <= 32:
        raise ValueError("pass_count must be between 1 and 32")
    if not 1 <= group_size <= 64:
        raise ValueError("group_size must be between 1 and 64")

    current: list[D8Item | None] = [D8Item(value, index) for index, value in enumerate(values)]
    pass_metrics: list[D8PassMetrics] = []
    for pass_index in range(pass_count):
        active_count = len(current)
        bit_set = [item is not None and bool((item.value >> pass_index) & 1) for item in current]
        total_ones = sum(bit_set)
        counter = 0
        output: list[D8Item | None] = [None] * active_count
        collisions = 0
        out_of_range = 0
        groups = _d8_groups(active_count, group_size, candidate, arrival_order)
        for group in groups:
            first = group * group_size
            lanes = list(range(first, min(first + group_size, active_count)))
            if candidate.lane_order == "reverse":
                lanes.reverse()
            elif candidate.lane_order != "lane":
                raise ValueError(f"unsupported d8 lane order {candidate.lane_order!r}")
            selected = [index for index in lanes if bit_set[index]]
            wave_base = counter
            counter += len(selected)
            one_prefixes = {index: wave_base + offset for offset, index in enumerate(selected)}
            # A zero lane uses the count of selected lanes before it. With the
            # exact lane prefix that is wave_base plus the number of lower lane
            # IDs. Wrong lane ordering deliberately changes this value.
            for index in lanes:
                item = current[index]
                if item is None:
                    continue
                if bit_set[index]:
                    one_prefix = one_prefixes[index]
                    destination = active_count - total_ones + one_prefix
                else:
                    earlier_selected = sum(
                        1 for selected_index in selected if lanes.index(selected_index) < lanes.index(index)
                    )
                    one_prefix = wave_base + earlier_selected
                    destination = index - one_prefix
                if not 0 <= destination < active_count:
                    out_of_range += 1
                    continue
                if output[destination] is not None:
                    collisions += 1
                output[destination] = item
        holes = sum(item is None for item in output)
        pass_metrics.append(
            D8PassMetrics(
                pass_index=pass_index,
                ones=total_ones,
                holes=holes,
                slot_collisions=collisions,
                out_of_range=out_of_range,
                counter_mismatch=abs(counter - total_ones),
            )
        )
        current = output

    if candidate.post_order == "unstable_equal_keys":
        grouped: dict[int, list[D8Item]] = collections.defaultdict(list)
        for item in current:
            if item is not None:
                grouped[item.value & ((1 << pass_count) - 1)].append(item)
        reordered: list[D8Item] = []
        for key in sorted(grouped):
            reordered.extend(reversed(grouped[key]))
        current = reordered + [None] * (len(current) - len(reordered))
    elif candidate.post_order != "none":
        raise ValueError(f"unsupported d8 post order {candidate.post_order!r}")

    expected = sorted(
        (D8Item(value, index) for index, value in enumerate(values)),
        key=lambda item: item.value & ((1 << pass_count) - 1),
    )
    output_mismatch = sum(
        actual != wanted for actual, wanted in zip(current, expected)
    ) + abs(len(current) - len(expected))
    stable_breaks = 0
    by_key: dict[int, list[int]] = collections.defaultdict(list)
    for item in current:
        if item is not None:
            by_key[item.value & ((1 << pass_count) - 1)].append(item.source_index)
    stable_breaks = sum(_inversion_count(indices) for indices in by_key.values())
    return D8ReplayResult(candidate, tuple(current), tuple(pass_metrics), output_mismatch, stable_breaks)


def synthetic_d8_values() -> tuple[int, ...]:
    # Repeated low-ten-bit keys span all three waves. The upper bits make every
    # payload distinct, so a stable-order failure cannot hide as equal data.
    return tuple(((index + 1) << 10) | ((index * 37 + 11) % 23) for index in range(137))


def rank_d8_candidates(values: Sequence[int] | None = None) -> list[D8ReplayResult]:
    values = synthetic_d8_values() if values is None else values
    group_count = (len(values) + 63) // 64
    arrival = tuple(([2, 0, 1] if group_count == 3 else reversed(range(group_count))))
    results = [
        replay_d8(values, candidate, arrival_order=arrival) for candidate in candidates_for("d8")
    ]
    return sorted(results, key=lambda result: (result.rejected, result.score, result.candidate.name))


def _selector_index(selector: int, mode: str) -> int | None:
    selector &= U32_MASK
    shifted = selector >> 10
    guest_dword_offset = shifted * 2
    if mode == "guest_dword":
        return guest_dword_offset // 2
    if mode == "guest_value_as_bytes":
        # A U32x2 load needs an aligned 8-byte record.  Treating the guest's
        # dword offset as bytes makes most selectors unaligned.
        if guest_dword_offset % 8:
            return None
        return guest_dword_offset // 8
    if mode == "guest_value_as_element":
        return guest_dword_offset
    if mode == "unshifted_selector":
        return selector
    raise ValueError(f"unknown address mode {mode!r}")


def _seed_counters(scenario: ReplayScenario, candidate: CandidateSemantics) -> tuple[int, int]:
    guest_b = scenario.active_count if scenario.initial_b is None else scenario.initial_b
    if candidate.counter_seed == "guest":
        return scenario.initial_a, guest_b
    if candidate.counter_seed == "both_zero":
        return 0, 0
    if candidate.counter_seed == "swapped":
        return guest_b, scenario.initial_a
    raise ValueError(f"unknown counter seed {candidate.counter_seed!r}")


def _ordered_groups(scenario: ReplayScenario, candidate: CandidateSemantics) -> list[int]:
    group_count = (scenario.active_count + scenario.group_size - 1) // scenario.group_size
    creation = list(range(group_count))
    if candidate.wave_order == "creation":
        return creation
    if candidate.wave_order == "reverse":
        return list(reversed(creation))
    if candidate.wave_order == "arrival":
        if scenario.arrival_order:
            if sorted(scenario.arrival_order) != creation:
                raise ValueError("arrival_order must contain each active workgroup exactly once")
            return list(scenario.arrival_order)
        return list(reversed(creation))
    raise ValueError(f"unknown wave order {candidate.wave_order!r}")


def _resolve_inputs(
    scenario: ReplayScenario, candidate: CandidateSemantics
) -> tuple[list[Pair | None], int]:
    resolved: list[Pair | None] = []
    invalid = 0
    for source_index in range(min(scenario.active_count, MAX_GUEST_RECORDS)):
        if scenario.shader == "016":
            resolved.append(scenario.direct_records[source_index])
            continue
        lookup_index = _selector_index(scenario.selectors[source_index], candidate.address_mode)
        if lookup_index is None or lookup_index < 0 or lookup_index >= len(scenario.lookup):
            resolved.append(None)
            invalid += 1
            continue
        pair = scenario.lookup[lookup_index]
        if pair is None:
            invalid += 1
        resolved.append(pair)
    return resolved, invalid


def _stable_key(scenario: ReplayScenario, source_index: int, pair: Pair) -> int:
    if scenario.stable_keys:
        return scenario.stable_keys[source_index]
    # This fallback is only a deterministic grouping key.  A real capture must
    # supply its decoded key before stable-sort scoring can be considered proof.
    return pair.hi & 0xFFFF


def _inversion_count(values: Sequence[int]) -> int:
    return sum(1 for i, left in enumerate(values) for right in values[i + 1 :] if left > right)


def replay(scenario: ReplayScenario, candidate: CandidateSemantics) -> ReplayResult:
    if candidate.shader != scenario.shader:
        raise ValueError("candidate and scenario shader families differ")

    resolved, invalid_refs = _resolve_inputs(scenario, candidate)
    counter_a, counter_b = _seed_counters(scenario, candidate)
    stream_starts = {"a": counter_a, "b": counter_b}
    writes: list[OutputWrite] = []

    for group in _ordered_groups(scenario, candidate):
        first = group * scenario.group_size
        lanes = list(range(first, min(first + scenario.group_size, scenario.active_count)))
        if candidate.lane_order == "reverse":
            lanes.reverse()
        elif candidate.lane_order != "lane":
            raise ValueError(f"unknown lane order {candidate.lane_order!r}")

        selected_a = [i for i in lanes if resolved[i] is not None and (resolved[i].flags & 1)]
        selected_b = [i for i in lanes if resolved[i] is not None and resolved[i].flags > 1]
        base_a, base_b = counter_a, counter_b
        counter_a += len(selected_a)
        counter_b += len(selected_b)
        for offset, source_index in enumerate(selected_a):
            pair = resolved[source_index]
            assert pair is not None
            writes.append(
                OutputWrite(
                    base_a + offset,
                    source_index,
                    "a",
                    pair,
                    _stable_key(scenario, source_index, pair),
                )
            )
        for offset, source_index in enumerate(selected_b):
            pair = resolved[source_index]
            assert pair is not None
            writes.append(
                OutputWrite(
                    base_b + offset,
                    source_index,
                    "b",
                    pair,
                    _stable_key(scenario, source_index, pair),
                )
            )

    if candidate.post_order == "unstable_equal_keys":
        reordered: list[OutputWrite] = []
        for stream in ("a", "b"):
            members = [write for write in writes if write.stream == stream]
            slots = sorted(write.slot for write in members)
            members.sort(key=lambda write: (write.stable_key, -write.source_index))
            reordered.extend(dataclasses.replace(write, slot=slot) for slot, write in zip(slots, members))
        writes = reordered
    elif candidate.post_order != "none":
        raise ValueError(f"unknown post order {candidate.post_order!r}")

    output: dict[int, Pair] = {}
    collisions = 0
    for write in writes:
        if scenario.output_capacity is not None and not (0 <= write.slot < scenario.output_capacity):
            invalid_refs += 1
            continue
        if write.slot in output:
            collisions += 1
        output[write.slot] = write.pair

    intended_slots: set[int] = set()
    intended_slots.update(range(stream_starts["a"], counter_a))
    intended_slots.update(range(stream_starts["b"], counter_b))
    holes = len(intended_slots.difference(output))

    if scenario.valid_ref_ids is not None:
        invalid_refs += sum(write.pair.low24_ref not in scenario.valid_ref_ids for write in writes)

    expected_multiplicity = collections.Counter(pair.low24_ref for pair in scenario.expected_output.values())
    actual_multiplicity = collections.Counter(pair.low24_ref for pair in output.values())
    duplicate_refs = 0
    if expected_multiplicity:
        duplicate_refs = sum(
            max(0, count - expected_multiplicity.get(reference, 0))
            for reference, count in actual_multiplicity.items()
        )

    count_mismatch = 0
    if scenario.expected_counter_a is not None:
        count_mismatch += abs(counter_a - scenario.expected_counter_a)
    if scenario.expected_counter_b is not None:
        count_mismatch += abs(counter_b - scenario.expected_counter_b)
    if scenario.expected_draw_count is not None:
        count_mismatch += abs(counter_a - scenario.expected_draw_count)

    output_mismatch = 0
    if scenario.expected_output:
        expected_slots = set(scenario.expected_output)
        actual_slots = set(output)
        output_mismatch += len(expected_slots.symmetric_difference(actual_slots))
        output_mismatch += sum(
            output.get(slot) != expected for slot, expected in scenario.expected_output.items() if slot in output
        )

    stable_breaks = 0
    for stream in ("a", "b"):
        members = sorted((write for write in writes if write.stream == stream), key=lambda write: write.slot)
        by_key: dict[int, list[int]] = collections.defaultdict(list)
        for write in members:
            by_key[write.stable_key].append(write.source_index)
        stable_breaks += sum(_inversion_count(indices) for indices in by_key.values())

    spatial_invalid = 0
    spatial_large_jumps = 0
    if scenario.spatial_by_ref:
        prior: tuple[float, float, float] | None = None
        for slot in sorted(output):
            reference = output[slot].low24_ref
            point = scenario.spatial_by_ref.get(reference)
            if point is None or len(point) != 3 or not all(math.isfinite(value) for value in point):
                spatial_invalid += 1
                continue
            if prior is not None and scenario.spatial_max_jump is not None:
                distance = math.dist(prior, point)
                if distance > scenario.spatial_max_jump:
                    spatial_large_jumps += 1
            prior = point

    metrics = ReplayMetrics(
        holes=holes,
        slot_collisions=collisions,
        duplicate_refs=duplicate_refs,
        invalid_refs=invalid_refs,
        count_mismatch=count_mismatch,
        output_mismatch=output_mismatch,
        stable_order_breaks=stable_breaks,
        spatial_invalid=spatial_invalid,
        spatial_large_jumps=spatial_large_jumps,
    )
    reasons = tuple(
        f"{name}={value}"
        for name, value in dataclasses.asdict(metrics).items()
        if value
    )
    return ReplayResult(candidate, metrics, counter_a, counter_b, output, tuple(writes), reasons)


def rank_candidates(scenario: ReplayScenario) -> list[ReplayResult]:
    results = [replay(scenario, candidate) for candidate in candidates_for(scenario.shader)]
    return sorted(results, key=lambda result: (result.metrics.rejected, result.metrics.score, result.candidate.name))


def _reference_output(records: Sequence[Pair], active_count: int) -> tuple[dict[int, Pair], int, int]:
    stream_a = [pair for pair in records[:active_count] if pair.flags & 1]
    stream_b = [pair for pair in records[:active_count] if pair.flags > 1]
    output = {index: pair for index, pair in enumerate(stream_a)}
    output.update({active_count + index: pair for index, pair in enumerate(stream_b)})
    return output, len(stream_a), active_count + len(stream_b)


def synthetic_scenario(shader: str = "7aa") -> ReplayScenario:
    """A multi-wave fixture that independently encodes the known guest result."""

    active = 137
    records = [
        Pair(0xC0000000 | (1000 + index), ((1, 2, 3, 0)[index % 4] << 16) | (index % 9))
        for index in range(active)
    ]
    expected, expected_a, expected_b = _reference_output(records, active)
    stable_keys = tuple(index % 5 for index in range(active))
    valid_refs = frozenset(pair.low24_ref for pair in records)
    points = {
        pair.low24_ref: (float(index), float(index % 7), float(index % 3))
        for index, pair in enumerate(records)
    }
    arrival = (2, 0, 1)
    if shader == "016":
        return ReplayScenario(
            name="synthetic_016_three_wave",
            shader="016",
            active_count=active,
            direct_records=tuple(records),
            arrival_order=arrival,
            expected_counter_a=expected_a,
            expected_counter_b=expected_b,
            expected_output=expected,
            expected_draw_count=expected_a,
            output_capacity=active * 2,
            valid_ref_ids=valid_refs,
            stable_keys=stable_keys,
            spatial_by_ref=points,
        )

    lookup: list[Pair | None] = [None] * (active * 3 + 2)
    selectors: list[int] = []
    for index, pair in enumerate(records):
        lookup_index = index * 3 + 1
        lookup[lookup_index] = pair
        selectors.append((lookup_index << 10) | ((index * 19 + 3) & 0x3FF))
    return ReplayScenario(
        name="synthetic_7aa_three_wave",
        shader="7aa",
        active_count=active,
        lookup=tuple(lookup),
        selectors=tuple(selectors),
        arrival_order=arrival,
        expected_counter_a=expected_a,
        expected_counter_b=expected_b,
        expected_output=expected,
        expected_draw_count=expected_a,
        output_capacity=active * 2,
        valid_ref_ids=valid_refs,
        stable_keys=stable_keys,
        spatial_by_ref=points,
    )


_CANDIDATE_RE = re.compile(
    r"candidate #(?P<dispatch>\d+)\[(?P<sample>\d+)\].*?selector=0x(?P<selector>[0-9a-f]+)"
    r" source_index=(?P<source>\d+) valid=(?P<valid>true|false)"
    r" values=0x(?P<lo>[0-9a-f]+),0x(?P<hi>[0-9a-f]+) flags=(?P<flags>\d+)",
    re.IGNORECASE,
)
_PRE_RE = re.compile(
    r"visibility list pre #(?P<dispatch>\d+).*?shader=0x(?P<shader>[0-9a-f]+).*?active=(?P<active>\d+)"
    r".*?gds_count\[(?P<count_index>\d+)\]=(?P<count>\d+)"
    r".*?gds_a\[(?P<a_index>\d+)\]=(?P<a>\d+)"
    r".*?gds_b\[(?P<b_index>\d+)\]=(?P<b>\d+)",
    re.IGNORECASE,
)
_POST_RE = re.compile(
    r"visibility list post #(?P<dispatch>\d+).*?gds_a\[(?P<a_index>\d+)\]=(?P<a0>\d+)->(?P<a1>\d+)"
    r".*?gds_b\[(?P<b_index>\d+)\]=(?P<b0>\d+)->(?P<b1>\d+)"
    r".*?changed=(?P<changed>\d+)",
    re.IGNORECASE,
)
_CONSUME_RE = re.compile(
    r"visibility list consume #(?P<dispatch>\d+).*?instances=(?P<instances>\d+)"
    r".*?invalid_lookup_ids=(?P<invalid>\d+)",
    re.IGNORECASE,
)


@dataclasses.dataclass
class VisibilityLogDispatch:
    dispatch: int
    shader: str | None = None
    active: int | None = None
    gds_pre: tuple[int, int] | None = None
    gds_post: tuple[int, int] | None = None
    changed: int | None = None
    instances: int | None = None
    invalid_lookup_ids: int | None = None
    samples: list[dict[str, object]] = dataclasses.field(default_factory=list)

    @property
    def sampled_mapping_valid(self) -> bool:
        return all(
            bool(sample["valid"]) and (int(sample["selector"]) >> 10) == int(sample["source_index"])
            for sample in self.samples
        )


def parse_visibility_log(path: Path) -> list[VisibilityLogDispatch]:
    dispatches: dict[int, VisibilityLogDispatch] = {}
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = _CANDIDATE_RE.search(line)
            if match:
                number = int(match["dispatch"])
                item = dispatches.setdefault(number, VisibilityLogDispatch(number))
                item.samples.append(
                    {
                        "sample": int(match["sample"]),
                        "selector": int(match["selector"], 16),
                        "source_index": int(match["source"]),
                        "valid": match["valid"].lower() == "true",
                        "pair": Pair(int(match["lo"], 16), int(match["hi"], 16)),
                        "logged_flags": int(match["flags"]),
                    }
                )
                continue
            match = _PRE_RE.search(line)
            if match:
                number = int(match["dispatch"])
                item = dispatches.setdefault(number, VisibilityLogDispatch(number))
                item.shader = match["shader"].lower()
                item.active = int(match["active"])
                item.gds_pre = int(match["a"]), int(match["b"])
                continue
            match = _POST_RE.search(line)
            if match:
                number = int(match["dispatch"])
                item = dispatches.setdefault(number, VisibilityLogDispatch(number))
                item.gds_post = int(match["a1"]), int(match["b1"])
                item.changed = int(match["changed"])
                continue
            match = _CONSUME_RE.search(line)
            if match:
                number = int(match["dispatch"])
                item = dispatches.setdefault(number, VisibilityLogDispatch(number))
                item.instances = int(match["instances"])
                item.invalid_lookup_ids = int(match["invalid"])
    return [dispatches[number] for number in sorted(dispatches)]


def _parse_tsv(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    with path.open("r", encoding="utf-8", errors="strict") as handle:
        for line_number, line in enumerate(handle, 1):
            line = line.rstrip("\r\n")
            if not line:
                continue
            pieces = line.split("\t", 1)
            if len(pieces) != 2 or (line_number == 1 and pieces == ["key", "value"]):
                continue
            result[pieces[0]] = pieces[1]
    return result


@dataclasses.dataclass(frozen=True)
class ReferenceMapping:
    name: str
    word: int
    mask: int
    shift: int = 0

    def extract(self, pair: Pair) -> int:
        value = pair.lo if self.word == 0 else pair.hi
        return (value >> self.shift) & self.mask


REFERENCE_MAPPINGS = (
    ReferenceMapping("second_word_32", 1, U32_MASK),
    ReferenceMapping("second_word_low24", 1, 0xFFFFFF),
    ReferenceMapping("first_word_32", 0, U32_MASK),
    ReferenceMapping("first_word_low24", 0, 0xFFFFFF),
    ReferenceMapping("second_word_low16", 1, 0xFFFF),
    ReferenceMapping("first_word_low16", 0, 0xFFFF),
)


@dataclasses.dataclass(frozen=True)
class ReferenceMappingResult:
    mapping: ReferenceMapping
    references: tuple[int, ...]
    invalid: int
    duplicate_refs: int

    @property
    def score(self) -> int:
        return self.invalid * 1000 + self.duplicate_refs


@dataclasses.dataclass(frozen=True)
class Vs370Capture:
    directory: Path
    manifest: Mapping[str, str]
    pairs: tuple[Pair, ...]
    object_keys: frozenset[int]
    indices: tuple[int, ...]
    interface: tuple[float, ...]
    validity: tuple[int, ...]

    @property
    def instances(self) -> int:
        return int(self.manifest["instances"], 0)

    @property
    def vertices_per_instance(self) -> int:
        if self.instances == 0:
            return 0
        float4_attributes = 4
        values_per_vertex = float4_attributes * 4
        return len(self.interface) // self.instances // values_per_vertex

    def positions(self) -> tuple[tuple[tuple[float, float, float, float], ...], ...]:
        result: list[tuple[tuple[float, float, float, float], ...]] = []
        attributes = 4
        components = 4
        vertices = self.vertices_per_instance
        for instance in range(self.instances):
            points: list[tuple[float, float, float, float]] = []
            for vertex in range(vertices):
                offset = ((instance * vertices + vertex) * attributes) * components
                points.append(tuple(self.interface[offset : offset + 4]))
            result.append(tuple(points))
        return tuple(result)

    def spatial_metrics(self) -> dict[str, int | float]:
        invalid = 0
        degenerate = 0
        max_abs_ndc = 0.0
        for instance in self.positions():
            projected: list[tuple[float, float, float]] = []
            for x, y, z, w in instance:
                if not all(math.isfinite(value) for value in (x, y, z, w)) or w == 0.0:
                    invalid += 1
                    continue
                projected.append((x / w, y / w, z / w))
                max_abs_ndc = max(max_abs_ndc, abs(x / w), abs(y / w), abs(z / w))
            if projected and any(
                max(point[axis] for point in projected) == min(point[axis] for point in projected)
                for axis in (0, 1)
            ):
                degenerate += 1
        return {
            "invalid_vertices": invalid,
            "degenerate_instances": degenerate,
            "max_abs_projected_coordinate": max_abs_ndc,
        }

    def reference_candidates(self) -> list[ReferenceMappingResult]:
        results: list[ReferenceMappingResult] = []
        for mapping in REFERENCE_MAPPINGS:
            references = tuple(mapping.extract(pair) for pair in self.pairs)
            invalid = sum(reference not in self.object_keys for reference in references)
            duplicate_refs = len(references) - len(set(references))
            results.append(ReferenceMappingResult(mapping, references, invalid, duplicate_refs))
        return sorted(results, key=lambda result: (result.score, result.mapping.name))


VS370_REQUIRED = (
    "manifest.tsv",
    "draw.bin",
    "indices.bin",
    "b3-list.bin",
    "b0-object-keys.bin",
    "vs-interface.bin",
    "vs-validity.bin",
)


def load_vs370_capture(directory: Path) -> Vs370Capture:
    directory = directory.resolve()
    missing = [str(directory / name) for name in VS370_REQUIRED if not (directory / name).is_file()]
    if missing:
        raise FileNotFoundError("cannot reconstruct captured geometry; missing: " + ", ".join(missing))
    manifest = _parse_tsv(directory / "manifest.tsv")
    instances = int(manifest["instances"], 0)
    pair_bytes = (directory / "b3-list.bin").read_bytes()
    if len(pair_bytes) != instances * 8:
        raise ValueError(
            f"{directory / 'b3-list.bin'} has {len(pair_bytes)} bytes; expected {instances * 8}"
        )
    pairs = tuple(Pair(*values) for values in struct.iter_unpack("<II", pair_bytes))
    key_bytes = (directory / "b0-object-keys.bin").read_bytes()
    if len(key_bytes) % 4:
        raise ValueError(f"{directory / 'b0-object-keys.bin'} is not a whole number of u32 keys")
    object_keys = frozenset(value[0] for value in struct.iter_unpack("<I", key_bytes))
    index_bytes = (directory / "indices.bin").read_bytes()
    index_size = int(manifest["index_size"], 0)
    if index_size not in {2, 4} or len(index_bytes) % index_size:
        raise ValueError(f"invalid index_size or byte count in {directory / 'indices.bin'}")
    index_format = "<H" if index_size == 2 else "<I"
    indices = tuple(value[0] for value in struct.iter_unpack(index_format, index_bytes))
    interface_bytes = (directory / "vs-interface.bin").read_bytes()
    if len(interface_bytes) % 4:
        raise ValueError(f"{directory / 'vs-interface.bin'} is not float32-aligned")
    interface = tuple(value[0] for value in struct.iter_unpack("<f", interface_bytes))
    validity_bytes = (directory / "vs-validity.bin").read_bytes()
    if len(validity_bytes) % 4:
        raise ValueError(f"{directory / 'vs-validity.bin'} is not u32-aligned")
    validity = tuple(value[0] for value in struct.iter_unpack("<I", validity_bytes))
    capture = Vs370Capture(directory, manifest, pairs, object_keys, indices, interface, validity)
    expected_values = capture.instances * capture.vertices_per_instance * 4 * 4
    if len(capture.interface) != expected_values or capture.vertices_per_instance == 0:
        raise ValueError(f"inconsistent VS interface dimensions in {directory}")
    return capture


ALIGNED_CHAIN_REQUIRED = (
    "dispatch.json",
    "gds-pre.bin",
    "d8-input.bin",
    "d8-output-selectors.bin",
    "gds-post-d8.bin",
    "7aa-lookup.bin",
    "7aa-output.bin",
    "gds-post-7aa.bin",
    "draw-indirect.bin",
    "indices.bin",
    "valid-reference-ids.bin",
    "candidate-records.bin",
    "candidate-interface.bin",
    "vs370-interface.bin",
    "vs370-validity.bin",
)


def inventory_capture_root(root: Path) -> dict[str, object]:
    root = root.resolve()
    visibility_logs = sorted(root.glob("*vs370*shad_log.txt")) + sorted(root.glob("*vs370*.txt"))
    # Preserve order but remove duplicates caused by the two patterns.
    visibility_logs = list(dict.fromkeys(visibility_logs))
    vs_roots = sorted((root / "captures").glob("vs370-interface-*")) if (root / "captures").is_dir() else []
    vs_occurrences: list[dict[str, object]] = []
    for vs_root in vs_roots:
        for occurrence in sorted(vs_root.glob("occ*")):
            missing = [name for name in VS370_REQUIRED if not (occurrence / name).is_file()]
            vs_occurrences.append(
                {
                    "path": str(occurrence),
                    "complete": not missing,
                    "missing": missing,
                }
            )
    count_chain_roots = sorted(root.rglob("count-chain-populated-*"))
    count_chain_complete = []
    for directory in count_chain_roots:
        required = ("manifest.tsv", "visibility-counts-pre902.bin", "metadata-pre902.bin", "indirect-commands.bin")
        count_chain_complete.append(
            {
                "path": str(directory),
                "complete": all((directory / name).is_file() for name in required),
                "missing": [name for name in required if not (directory / name).is_file()],
            }
        )

    aligned_roots: list[Path] = []
    for dispatch_file in root.rglob(ALIGNED_CHAIN_REQUIRED[0]):
        directory = dispatch_file.parent
        if any((directory / name).exists() for name in ALIGNED_CHAIN_REQUIRED[1:]):
            aligned_roots.append(directory)
    aligned = []
    for directory in sorted(set(aligned_roots)):
        missing = [name for name in ALIGNED_CHAIN_REQUIRED if not (directory / name).is_file()]
        aligned.append({"path": str(directory), "complete": not missing, "missing": missing})

    has_complete_aligned = any(item["complete"] for item in aligned)
    return {
        "capture_root": str(root),
        "exists": root.is_dir(),
        "visibility_summary_logs": [str(path) for path in visibility_logs],
        "vs370_occurrences": vs_occurrences,
        "count_chain_902": count_chain_complete,
        "aligned_d8_7aa_vs370": aligned,
        "data_sufficiency": {
            "deterministic_semantic_regressions": True,
            "post_vs_actual_geometry_viewer": any(item["complete"] for item in vs_occurrences),
            "full_real_chain_candidate_selection": has_complete_aligned,
            "conclusion": (
                "sufficient for the complete aligned replay"
                if has_complete_aligned
                else "insufficient for a unique real d8→7aa/016→VS370 winner"
            ),
            "missing_aligned_capture_files": [] if has_complete_aligned else list(ALIGNED_CHAIN_REQUIRED),
        },
    }


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _print_ranked(results: Sequence[ReplayResult], as_json: bool) -> None:
    if as_json:
        print(json.dumps([result.as_dict() for result in results], indent=2))
        return
    for result in results:
        status = "ACCEPT" if not result.metrics.rejected else "reject"
        reasons = ", ".join(result.reasons) if result.reasons else "all invariants preserved"
        print(f"{status:6} {result.candidate.name:30} score={result.metrics.score:7}  {reasons}")


def _cmd_inventory(args: argparse.Namespace) -> int:
    report = inventory_capture_root(args.capture_root)
    print(json.dumps(report, indent=2))
    return 0 if report["exists"] else 2


def _cmd_synthetic(args: argparse.Namespace) -> int:
    if args.shader == "d8":
        results = rank_d8_candidates()
        if args.json:
            print(json.dumps([result.as_dict() for result in results], indent=2))
        else:
            for result in results:
                status = "ACCEPT" if not result.rejected else "reject"
                pass_holes = sum(item.holes for item in result.passes)
                pass_collisions = sum(item.slot_collisions for item in result.passes)
                print(
                    f"{status:6} {result.candidate.name:30} score={result.score:7}  "
                    f"holes={pass_holes}, collisions={pass_collisions}, "
                    f"output_mismatch={result.output_mismatch}, stable_breaks={result.stable_order_breaks}"
                )
        accepted = [result for result in results if not result.rejected]
        return 0 if len(accepted) == 1 and accepted[0].candidate.name == "ps4_exact" else 1
    results = rank_candidates(synthetic_scenario(args.shader))
    _print_ranked(results, args.json)
    accepted = [result for result in results if not result.metrics.rejected]
    return 0 if len(accepted) == 1 and accepted[0].candidate.name == "ps4_exact" else 1


def _cmd_log(args: argparse.Namespace) -> int:
    parsed = parse_visibility_log(args.log)
    serializable = []
    for dispatch in parsed:
        item = dataclasses.asdict(dispatch)
        for sample in item["samples"]:
            pair = sample["pair"]
            if isinstance(pair, dict):
                sample["pair"] = [f"0x{pair['lo']:08x}", f"0x{pair['hi']:08x}"]
        item["sampled_mapping_valid"] = dispatch.sampled_mapping_valid
        serializable.append(item)
    print(json.dumps(serializable, indent=2))
    return 0


def _cmd_vs(args: argparse.Namespace) -> int:
    capture = load_vs370_capture(args.occurrence)
    result = {
        "directory": str(capture.directory),
        "instances": capture.instances,
        "vertices_per_instance": capture.vertices_per_instance,
        "spatial": capture.spatial_metrics(),
        "reference_candidates": [
            {
                "name": item.mapping.name,
                "references": [f"0x{value:x}" for value in item.references],
                "invalid": item.invalid,
                "duplicates": item.duplicate_refs,
                "score": item.score,
            }
            for item in capture.reference_candidates()
        ],
        "proof_limit": (
            "This post-VS fixture can score downstream reference mappings and decoded positions, "
            "but it cannot choose d8/7aa ordering without the aligned upstream files listed by inventory."
        ),
    }
    print(json.dumps(result, indent=2))
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    inventory_parser = subparsers.add_parser("inventory", help="report capture availability and exact gaps")
    inventory_parser.add_argument("--capture-root", type=Path, required=True)
    inventory_parser.set_defaults(func=_cmd_inventory)

    synthetic_parser = subparsers.add_parser("synthetic", help="run deterministic rejection regressions")
    synthetic_parser.add_argument("--shader", choices=("d8", "7aa", "016"), default="7aa")
    synthetic_parser.add_argument("--json", action="store_true")
    synthetic_parser.set_defaults(func=_cmd_synthetic)

    log_parser = subparsers.add_parser("log", help="parse a captured 7aa visibility-list log")
    log_parser.add_argument("log", type=Path)
    log_parser.set_defaults(func=_cmd_log)

    vs_parser = subparsers.add_parser("vs370", help="score one real post-VS occurrence")
    vs_parser.add_argument("occurrence", type=Path)
    vs_parser.set_defaults(func=_cmd_vs)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
