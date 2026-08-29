from __future__ import annotations

import hashlib
import json
import struct
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from build_candidates import build
from finder import (
    AtomicLiveControl,
    BundleError,
    LiveCandidateSession,
    ProjectedTriangle,
    RASTER_BACKGROUND,
    RASTER_COLORS,
    ShapeFraming,
    deduplicate_candidates,
    load_bundle,
    load_live_fingerprint_manifest,
    prepare_candidates,
    project_triangles,
    rasterize_projected,
    rasterize_result,
    replay_candidate,
    save_winner,
    shared_shape_framing,
)
from import_capture import import_capture


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _fword(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def _packed_vertex(x: int, y: int, z: int) -> tuple[int, int]:
    return x | (y << 16), z


def _record(tx: float = 0.0) -> tuple[int, ...]:
    words = [0] * 32
    words[12] = _fword(1.0)
    words[16] = _fword(1.0)
    words[20] = _fword(1.0)
    words[21] = _fword(tx)
    return tuple(words)


def _write_bundle(root: Path, *, two_commands: bool = False) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    files: dict[str, bytes] = {
        "draw.bin": struct.pack("<IIIiI", 3, 1, 0, 0, 0),
        "indices.bin": struct.pack("<3H", 0, 1, 2),
        "b1-packed-vertices.bin": struct.pack(
            "<6I",
            *_packed_vertex(0, 0, 32768),
            *_packed_vertex(65535, 0, 32768),
            *_packed_vertex(32768, 65535, 32768),
        ),
        "b2-records.bin": struct.pack("<64I", *(_record(0.0) + _record(0.5))),
        "transform.bin": struct.pack(
            "<16f",
            1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0,
        ),
        "userdata-effective.bin": struct.pack("<5I", 0, 0, 0, 0, 0),
        "candidate-a.bin": struct.pack("<2I", 0, 1),
        "candidate-b.bin": struct.pack("<2I", 1, 0),
    }
    if two_commands:
        files["draw.bin"] += struct.pack("<IIIiI", 3, 1, 0, 0, 1)
    for name, data in files.items():
        (root / name).write_bytes(data)
    shared_names = {
        "draw": "draw.bin",
        "indices": "indices.bin",
        "b1": "b1-packed-vertices.bin",
        "b2": "b2-records.bin",
        "transform": "transform.bin",
        "userdata": "userdata-effective.bin",
    }
    document = {
        "schema": 1,
        "shader_hash": "0xd25db925",
        "index_type": "uint16",
        "indices_first_index": 0,
        "topology": "triangle_list",
        "primitive_restart": False,
        "files": {
            key: {"path": name, "sha256": _sha(root / name)}
            for key, name in shared_names.items()
        },
        "candidates": [
            {
                "name": "pre-image-alias-compute-output",
                "selectors": {"path": "candidate-a.bin", "sha256": _sha(root / "candidate-a.bin")},
                "config": {"kind": "raw_u32"},
                "provenance": {"capture_phase": "pre-image-alias-compute-output"},
            },
            {
                "name": "post-image-alias-bound-data",
                "selectors": {"path": "candidate-b.bin", "sha256": _sha(root / "candidate-b.bin")},
                "config": {"kind": "raw_u32"},
                "provenance": {"capture_phase": "post-image-alias-bound-data"},
            },
        ],
    }
    manifest = root / "bundle.json"
    manifest.write_text(json.dumps(document), encoding="utf-8")
    return manifest


class FinderTests(unittest.TestCase):
    def test_exact_position_replay_distinguishes_selectors(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            bundle = load_bundle(_write_bundle(Path(temporary)))
            left = replay_candidate(bundle, bundle.candidates[0]).instances[0].positions[0]
            right = replay_candidate(bundle, bundle.candidates[1]).instances[0].positions[0]
            assert left is not None and right is not None
            self.assertEqual(struct.pack("<f", left[0]), struct.pack("<f", -1.0))
            self.assertEqual(struct.pack("<f", right[0]), struct.pack("<f", -0.5))
            self.assertEqual(left[3], 1.0)

    def test_replays_every_active_indirect_command(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            bundle = load_bundle(_write_bundle(Path(temporary), two_commands=True))
            result = replay_candidate(bundle, bundle.candidates[0])
            self.assertEqual([instance.command for instance in result.instances], [0, 1])
            self.assertEqual([instance.selector for instance in result.instances], [0, 1])

    def test_shared_framing_is_derived_once_from_first_candidate_bbox(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            bundle = load_bundle(_write_bundle(Path(temporary)))
            first = replay_candidate(bundle, bundle.candidates[0])
            second = replay_candidate(bundle, bundle.candidates[1])
            framing = shared_shape_framing(first, padding=0.0)
            self.assertAlmostEqual(framing.center_x, 0.0, places=5)
            self.assertAlmostEqual(framing.center_y, 0.0, places=5)
            self.assertAlmostEqual(framing.span_x, 2.0, places=5)
            self.assertAlmostEqual(framing.span_y, 2.0, places=5)
            # Candidate 2 is translated, but it does not get a different frame.
            self.assertNotEqual(framing, shared_shape_framing(second, padding=0.0))

    def test_filled_raster_uses_nearest_depth_independent_of_draw_order(self) -> None:
        frame = ShapeFraming(0.0, 0.0, 2.0, 2.0)
        far = ProjectedTriangle(((-0.8, -0.8, 0.8), (0.8, -0.8, 0.8), (0.0, 0.8, 0.8)), 0)
        near = ProjectedTriangle(((-0.8, -0.8, 0.2), (0.8, -0.8, 0.2), (0.0, 0.8, 0.2)), 1)
        first = rasterize_projected((far, near), frame, 64, 64)
        second = rasterize_projected((near, far), frame, 64, 64)
        offset = (32 * 64 + 32) * 3
        expected = bytes(RASTER_COLORS[1])
        self.assertEqual(first.pixels[offset : offset + 3], expected)
        self.assertEqual(second.pixels[offset : offset + 3], expected)

    def test_filled_raster_of_replayed_geometry_is_nonblank(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            bundle = load_bundle(_write_bundle(Path(temporary)))
            result = replay_candidate(bundle, bundle.candidates[0])
            raster = rasterize_result(result, shared_shape_framing(result), 96, 96)
            background = bytes(RASTER_BACKGROUND)
            nonblank = sum(
                raster.pixels[offset : offset + 3] != background
                for offset in range(0, len(raster.pixels), 3)
            )
            self.assertGreater(nonblank, 100)

    def test_shared_framing_renders_full_offscreen_triangle_not_ndc_sliver(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            bundle = load_bundle(_write_bundle(Path(temporary)))
            result = replay_candidate(bundle, bundle.candidates[0])
            instance = replace(
                result.instances[0],
                positions=(
                    (1.05, -0.6, 0.2, 1.0),
                    (1.45, -0.6, 0.2, 1.0),
                    (1.25, 0.6, 0.2, 1.0),
                ),
            )
            result = replace(result, instances=(instance,))
            triangles = project_triangles(result)
            self.assertGreater(max(point[0] for point in triangles[0].points), 1.0)
            frame = shared_shape_framing(result)
            raster = rasterize_projected(triangles, frame, 120, 120)
            background = bytes(RASTER_BACKGROUND)
            occupied_x = {
                pixel % raster.width
                for pixel in range(raster.width * raster.height)
                if raster.pixels[pixel * 3 : pixel * 3 + 3] != background
            }
            self.assertGreater(max(occupied_x) - min(occupied_x), 20)

    def test_rejects_hash_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            manifest = _write_bundle(Path(temporary))
            (Path(temporary) / "transform.bin").write_bytes(b"x" * 64)
            with self.assertRaisesRegex(BundleError, "SHA-256 mismatch"):
                load_bundle(manifest)

    def test_rejects_incomplete_userdata(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = _write_bundle(root)
            path = root / "userdata-effective.bin"
            path.write_bytes(struct.pack("<4I", 0, 0, 0, 0))
            document = json.loads(manifest.read_text(encoding="utf-8"))
            document["files"]["userdata"]["sha256"] = _sha(path)
            manifest.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(BundleError, "exactly SGPR0..SGPR4"):
                load_bundle(manifest)

    def test_invalid_selector_is_rejected_not_drawn(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = _write_bundle(root)
            bad = root / "bad.bin"
            bad.write_bytes(struct.pack("<I", 999))
            document = json.loads(manifest.read_text(encoding="utf-8"))
            document["candidates"].append({
                "name": "bad",
                "selectors": {"path": "bad.bin", "sha256": _sha(bad)},
                "config": {"kind": "raw_u32"},
                "provenance": {"capture_phase": "test"},
            })
            manifest.write_text(json.dumps(document), encoding="utf-8")
            bundle = load_bundle(manifest)
            valid, rejected = prepare_candidates(bundle)
            self.assertEqual(len(valid), 2)
            self.assertIn("bad", rejected)

    def test_exact_fix_candidate_does_not_get_robust_zero_b2(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = _write_bundle(root)
            bad = root / "candidate-b.bin"
            bad.write_bytes(struct.pack("<2I", 0x0E000000, 0x3F800000))
            document = json.loads(manifest.read_text(encoding="utf-8"))
            document["candidates"][1]["selectors"]["sha256"] = _sha(bad)
            manifest.write_text(json.dumps(document), encoding="utf-8")
            bundle = load_bundle(manifest)
            valid, rejected = prepare_candidates(bundle)
            self.assertEqual([item.candidate.name for item in valid], ["pre-image-alias-compute-output"])
            self.assertIn("post-image-alias-bound-data", rejected)

    def test_only_current_corrupt_may_clip_w_zero_vertices(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = _write_bundle(root)
            current = root / "candidate-a.bin"
            current.write_bytes(struct.pack("<2I", 0x0E000000, 0x3F800000))
            transform = root / "transform.bin"
            transform.write_bytes(struct.pack("<16f", *([0.0] * 16)))
            document = json.loads(manifest.read_text(encoding="utf-8"))
            document["files"]["transform"]["sha256"] = _sha(transform)
            document["candidates"][0]["name"] = "current-corrupt-bound-data"
            document["candidates"][0]["config"]["missing_b2"] = "robust_zero_128"
            document["candidates"][0]["selectors"]["sha256"] = _sha(current)
            manifest.write_text(json.dumps(document), encoding="utf-8")
            bundle = load_bundle(manifest)
            valid, rejected = prepare_candidates(bundle)
            self.assertEqual([item.candidate.name for item in valid], ["current-corrupt-bound-data"])
            self.assertIn("post-image-alias-bound-data", rejected)

    def test_winner_is_self_contained_and_exact(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = load_bundle(_write_bundle(root / "input"))
            winner = save_winner(bundle, bundle.candidates[0], root / "winner.bundle")
            loaded = load_bundle(winner)
            self.assertEqual(len(loaded.candidates), 1)
            self.assertEqual(loaded.candidates[0].selectors, bundle.candidates[0].selectors)
            self.assertEqual(loaded.candidates[0].sha256, bundle.candidates[0].sha256)

    def test_live_control_atomically_starts_advances_and_removes_owned_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = load_bundle(_write_bundle(root / "bundle"))
            groups = deduplicate_candidates(bundle.candidates)
            path = root / "control" / "selectors.control"
            control = AtomicLiveControl(path)
            session = LiveCandidateSession(groups, control)
            session.start()
            self.assertEqual(control.candidate_path.read_bytes(), bundle.candidates[0].path.read_bytes())
            first_control = path.read_bytes()
            self.assertFalse(list(path.parent.glob(f".{path.name}.*.tmp")))
            self.assertFalse(
                list(path.parent.glob(f".{control.candidate_path.name}.*.tmp"))
            )
            self.assertTrue(session.advance())
            self.assertEqual(control.candidate_path.read_bytes(), bundle.candidates[1].path.read_bytes())
            second_control = path.read_bytes()
            self.assertNotEqual(first_control, second_control)
            self.assertNotEqual(len(first_control), len(second_control))
            self.assertFalse(session.advance())
            session.close()
            self.assertFalse(path.exists())
            self.assertFalse(control.candidate_path.exists())

    def test_live_control_uses_cpp_text_interface(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "selectors.control"
            control = AtomicLiveControl(path)
            payload = struct.pack("<2I", 7, 11)
            control.write(payload)
            lines = path.read_text(encoding="utf-8").splitlines()
            self.assertEqual(lines[:3], [
                "schema=1",
                "enabled=1",
                f"candidate={control.candidate_path}",
            ])
            self.assertTrue(lines[3].startswith("#"))
            self.assertEqual(control.candidate_path.read_bytes(), payload)
            control.remove_if_owned()

    def test_live_control_emits_only_cheap_manifest_fingerprint_fields(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = root / "manifest.tsv"
            manifest.write_text(
                "field\tvalue\n"
                "commands_base\t0x12340000\n"
                "count_address\t0x56780000\n"
                "commands_stride\t20\n"
                "max_count\t897\n"
                "selector_base\t0x2e3f60000\n"
                "selector_size\t0x800000\n"
                "transform_address\t0x2cab82f80\n"
                "draw_count\t38\n"
                "commands_hash\t0x182e877dd2053940\n",
                encoding="utf-8",
            )
            fingerprint = load_live_fingerprint_manifest(manifest)
            self.assertEqual(
                fingerprint,
                {
                    "commands_address": 0x12340000,
                    "count_address": 0x56780000,
                    "stride": 20,
                    "max_count": 897,
                    "selector_base": 0x2E3F60000,
                    "selector_size": 0x800000,
                    "transform_address": 0x2CAB82F80,
                },
            )
            path = root / "selectors.control"
            control = AtomicLiveControl(path, fingerprint=fingerprint)
            control.write(struct.pack("<I", 1))
            text = path.read_text(encoding="utf-8")
            for expected in (
                "commands_address=0x12340000",
                "count_address=0x56780000",
                "stride=20",
                "max_count=897",
                "selector_base=0x2e3f60000",
                "selector_size=0x800000",
                "transform_address=0x2cab82f80",
            ):
                self.assertIn(expected + "\n", text)
            self.assertNotIn("draw_count=", text)
            self.assertNotIn("commands_hash=", text)
            control.remove_if_owned()

    def test_live_fingerprint_manifest_requires_every_cheap_field(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            manifest = Path(temporary) / "manifest.tsv"
            manifest.write_text(
                "field\tvalue\n"
                "commands_base\t1\n"
                "count_address\t2\n"
                "commands_stride\t20\n"
                "max_count\t897\n"
                "selector_base\t3\n"
                "selector_size\t4\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(BundleError, "transform_address"):
                load_live_fingerprint_manifest(manifest)

    def test_live_control_does_not_remove_file_changed_by_another_writer(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "selectors.control"
            control = AtomicLiveControl(path)
            control.write(struct.pack("<I", 1))
            path.write_text("external", encoding="utf-8")
            control.remove_if_owned()
            self.assertEqual(path.read_text(encoding="utf-8"), "external")
            self.assertFalse(control.candidate_path.exists())

    def test_live_candidates_deduplicate_exact_bytes_and_keep_source_order(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = _write_bundle(root)
            duplicate = root / "candidate-b.bin"
            duplicate.write_bytes((root / "candidate-a.bin").read_bytes())
            document = json.loads(manifest.read_text(encoding="utf-8"))
            document["candidates"][1]["selectors"]["sha256"] = _sha(duplicate)
            manifest.write_text(json.dumps(document), encoding="utf-8")
            bundle = load_bundle(manifest)
            groups = deduplicate_candidates(bundle.candidates)
            self.assertEqual(len(groups), 1)
            self.assertEqual(groups[0].primary.name, "pre-image-alias-compute-output")
            self.assertEqual(
                [candidate.name for candidate in groups[0].equivalents],
                ["pre-image-alias-compute-output", "post-image-alias-bound-data"],
            )
            winner = save_winner(
                bundle,
                groups[0].primary,
                root / "winner-live.bundle",
                equivalents=groups[0].equivalents,
            )
            saved = json.loads((winner / "bundle.json").read_text(encoding="utf-8"))
            self.assertEqual(
                [item["name"] for item in saved["byte_identical_candidates"]],
                ["pre-image-alias-compute-output", "post-image-alias-bound-data"],
            )


class BuilderTests(unittest.TestCase):
    def test_only_format_authorized_candidates_and_exact_ordering(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = _write_bundle(root)
            document = json.loads(manifest.read_text(encoding="utf-8"))
            document["candidates"] = []
            manifest.write_text(json.dumps(document), encoding="utf-8")
            (root / "floats.bin").write_bytes(struct.pack("<2f", 1.0, 2.9))
            (root / "pre.bin").write_bytes(struct.pack("<2I", 0, 1))
            (root / "post.bin").write_bytes(struct.pack("<2I", 0x3F800000, 0x3F800000))
            (root / "word29.bin").write_bytes(struct.pack("<2I", 0x10, 0x10))
            (root / "counts.bin").write_bytes(struct.pack("<I", 0))
            plan = {
                "schema": 1,
                "sources": [
                    {
                        "name": "converted-float",
                        "path": "floats.bin",
                        "storage": {"kind": "f32"},
                        "provenance": {"capture_phase": "typed-view"},
                    },
                    {
                        "name": "canonical-1e7-reconstruction",
                        "storage": {"kind": "d25_1e7_replay"},
                        "active_b2_word29_path": "word29.bin",
                        "pre902_counts_path": "counts.bin",
                        "read_const_2": 2,
                        "read_const_12": 2,
                        "read_const_13": 897,
                        "gds_160": 2,
                        "provenance": {"capture_phase": "pre-1e7"},
                    },
                    {
                        "name": "pre-image-alias-compute-output",
                        "path": "pre.bin",
                        "storage": {"kind": "u32"},
                        "provenance": {"capture_phase": "pre-image-alias-compute-output"},
                    },
                    {
                        "name": "post-image-alias-bound-data",
                        "path": "post.bin",
                        "storage": {"kind": "u32"},
                        "provenance": {"capture_phase": "post-image-alias-bound-data"},
                    },
                ],
            }
            plan_path = root / "selector-sources.json"
            plan_path.write_text(json.dumps(plan), encoding="utf-8")
            output = build(manifest, plan_path)
            self.assertEqual(
                [candidate["name"] for candidate in output["candidates"]],
                [
                    "pre-image-alias-compute-output",
                    "post-image-alias-bound-data",
                    "canonical-1e7-reconstruction",
                    "converted-float",
                ],
            )
            reconstructed = output["candidates"][2]
            self.assertEqual(reconstructed["config"]["atomic_order"], "canonical_sequential_thread_id")
            self.assertEqual(
                struct.unpack("<2I", (root / reconstructed["selectors"]["path"]).read_bytes()),
                (0, 1),
            )

    def test_packed_builder_uses_only_declared_channels(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = _write_bundle(root)
            document = json.loads(manifest.read_text(encoding="utf-8"))
            document["candidates"] = []
            manifest.write_text(json.dumps(document), encoding="utf-8")
            (root / "packed.bin").write_bytes(struct.pack("<I", 0xA1B2C3D4))
            plan = {
                "schema": 1,
                "sources": [{
                    "name": "packed",
                    "path": "packed.bin",
                    "storage": {
                        "kind": "packed_u32",
                        "channels": [
                            {"name": "low8", "shift": 0, "bits": 8},
                            {"name": "high8", "shift": 24, "bits": 8},
                        ],
                    },
                    "provenance": {"capture_phase": "packed-view"},
                }],
            }
            plan_path = root / "selector-sources.json"
            plan_path.write_text(json.dumps(plan), encoding="utf-8")
            output = build(manifest, plan_path)
            values = [
                struct.unpack("<I", (root / item["selectors"]["path"]).read_bytes())[0]
                for item in output["candidates"]
            ]
            self.assertEqual(values, [0xD4, 0xA1])


class ImporterTests(unittest.TestCase):
    def test_imports_noncontiguous_indices_and_sparse_b2_with_current_first(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture = root / "capture"
            capture.mkdir()
            (capture / "manifest.tsv").write_text(
                "key\tvalue\n"
                "d25_vertex_hash\t0xd25db925\n"
                "commands_stride\t32\n"
                "draw_count\t2\n"
                "index_size\t2\n"
                "primitive_type\t4\n"
                "primitive_restart_enable\t0\n",
                encoding="utf-8",
            )
            (capture / "indirect-commands.bin").write_bytes(
                struct.pack("<IIIiI", 3, 1, 17, 0, 0)
                + b"\xAA" * 12
                + struct.pack("<IIIiI", 3, 1, 99, 0, 1)
                + b"\xBB" * 12
            )
            # Command 1 begins at byte 16: the gap is deliberately not an index range.
            (capture / "index-slices.bin").write_bytes(
                struct.pack("<3H", 0, 1, 2) + b"\xCC" * 10 + struct.pack("<3H", 0, 1, 2)
            )
            (capture / "index-slices.tsv").write_text(
                "command\tblob_offset\tguest_address\tfirst_index\tindex_count\t"
                "vertex_offset\tbytes\tmapped\tgpu_modified\thash\n"
                "0\t0\t0x1000\t17\t3\t0\t6\t1\t0\t0x1\n"
                "1\t16\t0x2000\t99\t3\t0\t6\t1\t0\t0x2\n",
                encoding="utf-8",
            )
            (capture / "b1-full.bin").write_bytes(struct.pack(
                "<6I",
                *_packed_vertex(0, 0, 32768),
                *_packed_vertex(65535, 0, 32768),
                *_packed_vertex(32768, 65535, 32768),
            ))
            (capture / "b2-records.bin").write_bytes(struct.pack("<32I", *_record(0.0)))
            (capture / "b2-record-ids.bin").write_bytes(struct.pack("<I", 0))
            (capture / "transform.bin").write_bytes(struct.pack(
                "<16f",
                1.0, 0.0, 0.0, 0.0,
                0.0, 1.0, 0.0, 0.0,
                0.0, 0.0, 1.0, 0.0,
                0.0, 0.0, 0.0, 1.0,
            ))
            (capture / "userdata-effective.bin").write_bytes(struct.pack("<5I", 0, 0, 0, 0, 0))
            (capture / "b0-selectors-post-alias.bin").write_bytes(
                struct.pack("<2I", 0x0E000000, 0x3F800000)
            )
            (capture / "producer-1e7-b0-post.bin").write_bytes(struct.pack("<2I", 0, 0))
            (capture / "b0-selectors-gpu-pre-alias.bin").write_bytes(struct.pack("<2I", 0, 0))

            manifest = import_capture(capture, root / "bundle")
            bundle = load_bundle(manifest)
            self.assertEqual(
                [candidate.name for candidate in bundle.candidates],
                [
                    "current-corrupt-bound-data",
                    "producer-1e7-b0-post",
                    "pre-image-alias-compute-output",
                ],
            )
            self.assertEqual(bundle.candidates[0].config["missing_b2"], "robust_zero_128")
            result = replay_candidate(bundle, bundle.candidates[0])
            self.assertEqual(
                [instance.selector for instance in result.instances],
                [0x0E000000, 0x3F800000],
            )
            self.assertEqual(len(result.instances), 2)
            producer = replay_candidate(bundle, bundle.candidates[1])
            self.assertEqual([instance.selector for instance in producer.instances], [0, 0])


if __name__ == "__main__":
    unittest.main()
