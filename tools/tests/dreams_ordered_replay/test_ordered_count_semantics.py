from __future__ import annotations

import re
import unittest
from pathlib import Path

from ordered_count_semantics import (
    U32_MASK,
    decode_ordered_count_control,
    exclusive_prefix,
    first_valid_increment,
    flatten_workgroup_id,
    ordered_counter_dword,
    ordered_token,
    ticketed_prefix,
    unflatten_workgroup_id,
)


SOURCE_ROOT = Path(__file__).resolve().parents[3]


class OrderedCountControlTests(unittest.TestCase):
    def test_all_operation_and_wave_flag_combinations_decode(self) -> None:
        for operation in range(4):
            for flags in range(4):
                # Reserved OFFSET1 bits must not leak into the packed control.
                offset1 = 0xC0 | (operation << 4) | flags
                decoded = decode_ordered_count_control(0x5C, offset1)
                self.assertEqual(decoded.packed, 0x5C00 | (flags << 2) | operation)
                self.assertEqual(decoded.operation, operation)
                self.assertEqual(decoded.wave_release, bool(flags & 1))
                self.assertEqual(decoded.wave_done, bool(flags & 2))
                self.assertEqual(decoded.offset_bytes, 0x5C)

    def test_dreams_traversal_controls_select_the_captured_counters(self) -> None:
        controls = (0x0804, 0x1804, 0x0C04, 0x100C)
        expected = (0x142, 0x146, 0x143, 0x144)
        self.assertEqual(
            tuple(ordered_counter_dword(0x500, control) for control in controls), expected
        )
        self.assertTrue(all(control & 0x4 for control in controls))
        self.assertEqual(tuple(bool(control & 0x8) for control in controls), (False, False, False, True))

    def test_byte_offset_is_converted_to_dwords_after_aligning_m0_base(self) -> None:
        decoded = decode_ordered_count_control(0x18, 0x01)
        self.assertEqual(decoded.packed, 0x1804)
        self.assertEqual(ordered_counter_dword(0x503, decoded.packed), 0x146)


class OrderedTicketTests(unittest.TestCase):
    def test_increment_comes_from_first_valid_lane(self) -> None:
        values = (91, 7, 13, 21)
        self.assertEqual(first_valid_increment(values, (False, True, True, False)), 7)

    def test_empty_exec_still_contributes_zero(self) -> None:
        self.assertEqual(first_valid_increment((91, 7, 13, 21), (False,) * 4), 0)

    def test_host_arrival_order_cannot_change_ticket_order(self) -> None:
        payloads = (3, 0, 2, 1, 5, 0, 4)
        expected = exclusive_prefix(payloads, initial=17)
        for schedule in (
            tuple(range(len(payloads))),
            tuple(reversed(range(len(payloads)))),
            (3, 0, 6, 2, 5, 1, 4),
        ):
            actual = ticketed_prefix(payloads, schedule, initial=17)
            self.assertEqual(actual.prefixes, expected.prefixes)
            self.assertEqual(actual.final_counter, expected.final_counter)
        self.assertGreater(ticketed_prefix(payloads, tuple(reversed(range(7)))).attempts, 7)

    def test_low_11_bit_token_wrap_does_not_become_the_scratch_index(self) -> None:
        group_count = 3942
        payloads = tuple(1 if (index * 17) % 23 < 7 else 0 for index in range(group_count))
        tokens = tuple(ordered_token(index) for index in range(group_count))
        self.assertEqual(tokens[:4], (0, 1, 2, 3))
        self.assertEqual(tokens[2048:2052], (0, 1, 2, 3))
        self.assertEqual(len(set(tokens)), 2048)

        full_order = exclusive_prefix(payloads)
        token_sorted_order = sorted(range(group_count), key=lambda index: (tokens[index], index))
        token_sorted_payloads = tuple(payloads[index] for index in token_sorted_order)
        self.assertNotEqual(exclusive_prefix(token_sorted_payloads).prefixes, full_order.prefixes)

    def test_prefix_recurrence_is_exclusive_and_wraps_as_u32(self) -> None:
        result = exclusive_prefix((2, 0, 5, 1), initial=U32_MASK - 1)
        self.assertEqual(result.prefixes, (U32_MASK - 1, 0, 0, 5))
        self.assertEqual(result.final_counter, 6)

    def test_gpu_resident_group_count_clamps_the_scanned_prefix(self) -> None:
        result = exclusive_prefix((4, 3, 2, 1), initial=9, count=min(2, 4))
        self.assertEqual(result.prefixes, (9, 13))
        self.assertEqual(result.final_counter, 16)


class WorkgroupOrderTests(unittest.TestCase):
    def test_non_cubic_3d_dispatch_is_x_fast_and_round_trips(self) -> None:
        dimensions = (3, 2, 4)
        coordinates = [
            (x, y, z)
            for z in range(dimensions[2])
            for y in range(dimensions[1])
            for x in range(dimensions[0])
        ]
        self.assertEqual(
            [flatten_workgroup_id(item, dimensions) for item in coordinates],
            list(range(24)),
        )
        self.assertEqual(
            [unflatten_workgroup_id(index, dimensions) for index in range(24)], coordinates
        )

    def test_3d_collect_uses_flat_creation_slots_not_completion_order(self) -> None:
        dimensions = (3, 2, 2)
        completion_order = (11, 2, 7, 0, 9, 4, 1, 10, 6, 3, 8, 5)
        creation_payloads = tuple((index * 5 + 3) % 7 for index in range(12))
        scratch: list[int | None] = [None] * 12
        for completed in completion_order:
            coordinate = unflatten_workgroup_id(completed, dimensions)
            scratch[flatten_workgroup_id(coordinate, dimensions)] = creation_payloads[completed]
        self.assertEqual(tuple(scratch), creation_payloads)
        scanned = exclusive_prefix(tuple(int(value) for value in scratch))
        ticketed = ticketed_prefix(creation_payloads, completion_order)
        self.assertEqual(scanned.prefixes, ticketed.prefixes)
        self.assertEqual(scanned.final_counter, ticketed.final_counter)


class ProductionSourceContractTests(unittest.TestCase):
    def test_frontend_makes_empty_exec_lane_selection_defined(self) -> None:
        source = (
            SOURCE_ROOT / "src/shader_recompiler/frontend/translate/data_share.cpp"
        ).read_text(encoding="utf-8")
        compact = re.sub(r"\s+", " ", source)
        self.assertIn("const IR::U1 has_active_lane = ir.GroupAny(ir.GetExec());", compact)
        self.assertIn("ir.Select(has_active_lane, ir.GetExec(), ir.Imm1(true))", compact)
        self.assertIn(
            "ir.Select(has_active_lane, ir.ReadLane(value, first_active_lane), ir.Imm32(0))",
            compact,
        )

    def test_host_shader_performs_the_same_exclusive_recurrence(self) -> None:
        source = (
            SOURCE_ROOT / "src/video_core/host_shaders/dreams_ordered_prefix.comp"
        ).read_text(encoding="utf-8")
        compact = re.sub(r"\s+", " ", source)
        snippets = (
            "uint running = gds[stream.y];",
            "for (uint group = 0; group < count; ++group)",
            "gds[entry + 1] = running;",
            "running += payload;",
            "gds[stream.y] = running;",
        )
        positions = [compact.index(snippet) for snippet in snippets]
        self.assertEqual(positions, sorted(positions))

    def test_spirv_backend_flattens_workgroups_x_fast(self) -> None:
        source = (
            SOURCE_ROOT / "src/shader_recompiler/backend/spirv/spirv_emit_context.cpp"
        ).read_text(encoding="utf-8")
        compact = re.sub(r"\s+", " ", source)
        self.assertIn("OpIMul(U32[1], workgroup_y, num_workgroups_x)", compact)
        self.assertIn(
            "OpIMul(U32[1], workgroup_z, OpIMul(U32[1], num_workgroups_x, num_workgroups_y))",
            compact,
        )


if __name__ == "__main__":
    unittest.main()
