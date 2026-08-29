"""Small, deterministic model of Liverpool ``DS_ORDERED_COUNT`` ordering.

This module is deliberately independent of shadPS4 and Vulkan.  It exists to
make the low-level contract executable: instruction control decoding, logical
wave tickets, exclusive-prefix recurrence, and x-fast 3D workgroup order.
"""

from __future__ import annotations

import dataclasses
from collections.abc import Sequence


U32_MASK = 0xFFFFFFFF


@dataclasses.dataclass(frozen=True)
class OrderedCountControl:
    packed: int
    operation: int
    wave_release: bool
    wave_done: bool
    offset_bytes: int


def decode_ordered_count_control(offset0: int, offset1: int) -> OrderedCountControl:
    """Decode the two DS instruction offset bytes as shadPS4's frontend does.

    ``OFFSET0`` is the byte offset.  ``OFFSET1[5:4]`` selects the operation,
    while bits 0 and 1 become WAVE_RELEASE and WAVE_DONE respectively.  The
    other OFFSET1 bits are reserved and do not enter the packed control.
    """

    if not 0 <= offset0 <= 0xFF or not 0 <= offset1 <= 0xFF:
        raise ValueError("DS offsets must each fit in one byte")
    operation = (offset1 >> 4) & 0x3
    wave_release = bool(offset1 & 0x1)
    wave_done = bool(offset1 & 0x2)
    packed = (offset0 << 8) | ((offset1 & 0x3) << 2) | operation
    return OrderedCountControl(packed, operation, wave_release, wave_done, offset0)


def ordered_counter_dword(m0_base_dwords: int, packed_control: int) -> int:
    """Resolve the dword counter selected by M0 and DS OFFSET0."""

    return (m0_base_dwords & 0xFFFC) + (((packed_control >> 8) & 0xFF) >> 2)


def ordered_token(logical_index: int, token_bits: int = 11) -> int:
    """Return the guest-visible wrapped token for a logical creation index."""

    if logical_index < 0:
        raise ValueError("logical_index cannot be negative")
    if not 1 <= token_bits <= 32:
        raise ValueError("token_bits must be between 1 and 32")
    return logical_index & ((1 << token_bits) - 1)


def flatten_workgroup_id(
    workgroup_id: tuple[int, int, int], dimensions: tuple[int, int, int]
) -> int:
    """Flatten a 3D dispatch in Liverpool/Vulkan x-fast creation order."""

    x, y, z = workgroup_id
    dim_x, dim_y, dim_z = dimensions
    if min(dim_x, dim_y, dim_z) <= 0:
        raise ValueError("dispatch dimensions must be positive")
    if not (0 <= x < dim_x and 0 <= y < dim_y and 0 <= z < dim_z):
        raise ValueError("workgroup_id is outside the dispatch")
    return x + y * dim_x + z * dim_x * dim_y


def unflatten_workgroup_id(index: int, dimensions: tuple[int, int, int]) -> tuple[int, int, int]:
    """Inverse of :func:`flatten_workgroup_id`."""

    dim_x, dim_y, dim_z = dimensions
    total = dim_x * dim_y * dim_z
    if min(dim_x, dim_y, dim_z) <= 0:
        raise ValueError("dispatch dimensions must be positive")
    if not 0 <= index < total:
        raise ValueError("workgroup index is outside the dispatch")
    plane = dim_x * dim_y
    z, in_plane = divmod(index, plane)
    y, x = divmod(in_plane, dim_x)
    return x, y, z


@dataclasses.dataclass(frozen=True)
class PrefixResult:
    prefixes: tuple[int, ...]
    final_counter: int


def exclusive_prefix(
    payloads: Sequence[int], initial: int = 0, *, count: int | None = None
) -> PrefixResult:
    """Mirror the host prefix shader's unsigned-32-bit recurrence."""

    if count is None:
        count = len(payloads)
    if not 0 <= count <= len(payloads):
        raise ValueError("count must select a prefix of payloads")
    running = initial & U32_MASK
    prefixes: list[int] = []
    for payload in payloads[:count]:
        if payload < 0:
            raise ValueError("ordered-count payloads cannot be negative")
        prefixes.append(running)
        running = (running + payload) & U32_MASK
    return PrefixResult(tuple(prefixes), running)


@dataclasses.dataclass(frozen=True)
class TicketedPrefixResult(PrefixResult):
    attempts: int


def ticketed_prefix(
    payloads: Sequence[int], attempt_order: Sequence[int], initial: int = 0
) -> TicketedPrefixResult:
    """Model waves retrying until ``state.turn`` equals their logical ticket.

    ``attempt_order`` represents host/GPU arrival order.  It must contain every
    logical ticket exactly once and is repeated until all tickets succeed.  The
    result therefore depends only on logical creation order, not arrival order.
    """

    count = len(payloads)
    if sorted(attempt_order) != list(range(count)):
        raise ValueError("attempt_order must contain every logical ticket exactly once")
    if any(payload < 0 for payload in payloads):
        raise ValueError("ordered-count payloads cannot be negative")

    unresolved = set(range(count))
    prefixes: list[int | None] = [None] * count
    turn = 0
    running = initial & U32_MASK
    attempts = 0
    while unresolved:
        progressed = False
        for ticket in attempt_order:
            attempts += 1
            if ticket not in unresolved or ticket != turn:
                continue
            prefixes[ticket] = running
            running = (running + payloads[ticket]) & U32_MASK
            unresolved.remove(ticket)
            turn += 1
            progressed = True
        if not progressed:
            raise RuntimeError("ticket schedule cannot advance")

    return TicketedPrefixResult(tuple(int(value) for value in prefixes), running, attempts)
