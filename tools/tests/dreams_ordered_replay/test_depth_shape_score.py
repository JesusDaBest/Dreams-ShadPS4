from __future__ import annotations

import math
import struct
import tempfile
import unittest
from pathlib import Path

from depth_shape_score import (
    DepthMask,
    depth_change_mask,
    rank_depth_candidates,
    rank_depth_output_directory,
    score_depth_mask,
)


def _mask(width: int, height: int, foreground: set[tuple[int, int]]) -> DepthMask:
    pixels = bytes((x, y) in foreground for y in range(height) for x in range(width))
    return DepthMask(width, height, pixels)


def _rectangle(left: int, top: int, right: int, bottom: int) -> set[tuple[int, int]]:
    return {(x, y) for y in range(top, bottom + 1) for x in range(left, right + 1)}


def _depth_from_mask(mask: DepthMask) -> bytes:
    values = [0x3F800000 if pixel else 0 for pixel in mask.pixels]
    return struct.pack(f"<{len(values)}I", *values)


class DepthShapeScoreTests(unittest.TestCase):
    def test_raw_depth_comparison_honors_row_padding(self) -> None:
        width, height, pitch = 3, 2, 5
        before = bytearray(pitch * height * 4)
        after = bytearray(before)
        struct.pack_into("<I", after, (1 * pitch + 2) * 4, 0x80000000)
        # A changed padding pixel is deliberately ignored.
        struct.pack_into("<I", after, (0 * pitch + 4) * 4, 0x7FC00001)
        result = depth_change_mask(before, after, width, height, row_length=pitch)
        self.assertEqual(sum(result.pixels), 1)
        self.assertEqual(result.pixels[1 * width + 2], 1)

    def test_foreground_uses_eight_connected_components(self) -> None:
        result = score_depth_mask(_mask(4, 4, {(1, 1), (2, 2)}))
        self.assertEqual(result.component_count, 1)
        self.assertEqual(result.largest_component_pixels, 2)
        self.assertEqual(result.disconnected_pixels, 0)

    def test_enclosed_background_is_counted_as_a_hole(self) -> None:
        ring = _rectangle(1, 1, 5, 5) - _rectangle(2, 2, 4, 4)
        result = score_depth_mask(_mask(7, 7, ring))
        self.assertEqual(result.component_count, 1)
        self.assertEqual(result.hole_pixels, 9)
        self.assertGreater(result.hole_ratio, 0.0)

    def test_filled_rectangle_has_exact_convex_fill_and_box_boundary(self) -> None:
        result = score_depth_mask(_mask(16, 12, _rectangle(3, 2, 12, 9)))
        self.assertEqual(result.changed_pixels, 80)
        self.assertEqual(result.hull_area, 80.0)
        self.assertEqual(result.convex_fill_ratio, 1.0)
        self.assertEqual(result.hole_pixels, 0)
        self.assertAlmostEqual(result.cuboid_boundary_score, 0.0)
        self.assertAlmostEqual(result.score, 0.0)

    def test_disconnected_and_hollow_shapes_rank_below_a_filled_box(self) -> None:
        width, height = 24, 18
        full = _mask(width, height, _rectangle(5, 4, 18, 13))
        hollow_pixels = _rectangle(5, 4, 18, 13) - _rectangle(8, 7, 15, 10)
        hollow = _mask(width, height, hollow_pixels)
        fragments = _mask(
            width,
            height,
            _rectangle(5, 4, 10, 13) | _rectangle(14, 4, 18, 13),
        )
        before = bytes(width * height * 4)
        ranked = rank_depth_candidates(
            before,
            {
                "hollow": _depth_from_mask(hollow),
                "fragments": _depth_from_mask(fragments),
                "full": _depth_from_mask(full),
            },
            width,
            height,
        )
        self.assertEqual(ranked[0].name, "full")
        by_name = {item.name: item.metrics for item in ranked}
        self.assertGreater(by_name["hollow"].hole_pixels, 0)
        self.assertGreater(by_name["fragments"].disconnected_pixels, 0)
        self.assertGreater(by_name["hollow"].score, by_name["full"].score)
        self.assertGreater(by_name["fragments"].score, by_name["full"].score)

    def test_projected_hexagonal_box_scores_better_than_triangle(self) -> None:
        width, height = 30, 24
        # Convex, centrally symmetric six-edge silhouette.
        hexagon = {
            (x, y)
            for y in range(4, 20)
            for x in range(3, 27)
            if x >= 7 - (y - 4) // 2
            and x <= 22 + (y - 4) // 2
            and x >= 3 + (y - 12) // 2
            and x <= 26 - (y - 12) // 2
        }
        triangle = {
            (x, y)
            for y in range(4, 20)
            for x in range(3, 27)
            if abs(x - 15) <= (y - 4) // 2
        }
        hex_score = score_depth_mask(_mask(width, height, hexagon))
        triangle_score = score_depth_mask(_mask(width, height, triangle))
        self.assertLess(hex_score.cuboid_boundary_score, triangle_score.cuboid_boundary_score)

    def test_empty_candidate_is_ranked_last(self) -> None:
        width, height = 8, 8
        before = bytes(width * height * 4)
        full = _depth_from_mask(_mask(width, height, _rectangle(2, 2, 5, 5)))
        ranked = rank_depth_candidates(
            before,
            (("empty", before), ("full", full)),
            width,
            height,
        )
        self.assertEqual([item.name for item in ranked], ["full", "empty"])
        self.assertTrue(math.isinf(ranked[-1].metrics.score))

    def test_invalid_depth_size_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            depth_change_mask(bytes(4), bytes(4), 2, 1)

    def test_candidate_output_directory_is_ranked_from_replay_bundles(self) -> None:
        width, height = 20, 16
        before = bytes(width * height * 4)
        full = _depth_from_mask(_mask(width, height, _rectangle(4, 3, 15, 12)))
        hollow = _depth_from_mask(
            _mask(
                width,
                height,
                _rectangle(4, 3, 15, 12) - _rectangle(7, 6, 12, 9),
            )
        )
        manifest = (
            f"pre_depth_width\t{width}\n"
            f"pre_depth_height\t{height}\n"
            f"pre_depth_row_length\t{width}\n"
            f"pre_depth_layers\t1\n"
            f"post_depth_width\t{width}\n"
            f"post_depth_height\t{height}\n"
            f"post_depth_row_length\t{width}\n"
            f"post_depth_layers\t1\n"
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name, output in (("hollow", hollow), ("full", full)):
                bundle = root / name
                bundle.mkdir()
                (bundle / "manifest.tsv").write_text(manifest, encoding="utf-8")
                (bundle / "pre-depth.bin").write_bytes(before)
                (bundle / "replay-depth.bin").write_bytes(output)

            ranked = rank_depth_output_directory(root)

        self.assertEqual([candidate.name for candidate in ranked], ["full", "hollow"])
        self.assertEqual(ranked[0].metrics.changed_pixels, 120)

    def test_candidate_directory_rejects_different_starting_depth(self) -> None:
        width, height = 4, 4
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for index in range(2):
                bundle = root / f"candidate-{index}"
                bundle.mkdir()
                before = bytearray(width * height * 4)
                before[0] = index
                (bundle / "pre-depth.bin").write_bytes(before)
                (bundle / "replay-depth.bin").write_bytes(before)
            with self.assertRaisesRegex(ValueError, "pre-depth inputs differ"):
                rank_depth_output_directory(root, width=width, height=height)


if __name__ == "__main__":
    unittest.main()
