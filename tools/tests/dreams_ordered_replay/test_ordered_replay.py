from __future__ import annotations

import dataclasses
import hashlib
import json
import math
import os
import struct
import tempfile
import unittest
from pathlib import Path

from aligned_capture import build_candidate_frames, load_aligned_capture
from ordered_replay import (
    ALIGNED_CHAIN_REQUIRED,
    CandidateSemantics,
    Pair,
    ReplayScenario,
    VS370_REQUIRED,
    candidates_for,
    inventory_capture_root,
    load_vs370_capture,
    parse_visibility_log,
    rank_d8_candidates,
    rank_candidates,
    replay,
    replay_d8,
    synthetic_scenario,
)
from viewer import aligned_winner_document, winner_document, write_winner


class OrderedReplayTests(unittest.TestCase):
    def test_d8_ten_pass_guest_semantics_are_the_only_accepted_candidate(self) -> None:
        results = rank_d8_candidates()
        accepted = [result for result in results if not result.rejected]
        self.assertEqual([result.candidate.name for result in accepted], ["ps4_exact"])
        self.assertEqual(accepted[0].score, 0)

    def test_d8_host_completion_order_keeps_counts_but_creates_holes(self) -> None:
        result = next(item for item in rank_d8_candidates() if item.candidate.name == "completion_order")
        self.assertTrue(all(item.counter_mismatch == 0 for item in result.passes))
        self.assertGreater(sum(item.holes for item in result.passes), 0)
        self.assertGreater(sum(item.slot_collisions for item in result.passes), 0)
        self.assertTrue(result.rejected)

    def test_7aa_guest_semantics_are_the_only_accepted_candidate(self) -> None:
        results = rank_candidates(synthetic_scenario("7aa"))
        accepted = [result for result in results if not result.metrics.rejected]
        self.assertEqual([result.candidate.name for result in accepted], ["ps4_exact"])
        self.assertEqual(accepted[0].metrics.score, 0)

    def test_016_guest_semantics_are_the_only_accepted_candidate(self) -> None:
        results = rank_candidates(synthetic_scenario("016"))
        accepted = [result for result in results if not result.metrics.rejected]
        self.assertEqual([result.candidate.name for result in accepted], ["ps4_exact"])

    def test_matching_counts_does_not_hide_order_destruction(self) -> None:
        scenario = synthetic_scenario("7aa")
        candidate = next(item for item in candidates_for("7aa") if item.name == "completion_order")
        result = replay(scenario, candidate)
        self.assertEqual(result.counter_a, scenario.expected_counter_a)
        self.assertEqual(result.counter_b, scenario.expected_counter_b)
        self.assertGreater(result.metrics.stable_order_breaks, 0)
        self.assertGreater(result.metrics.output_mismatch, 0)
        self.assertTrue(result.metrics.rejected)

    def test_wrong_counter_seed_is_rejected_for_slot_collisions(self) -> None:
        scenario = synthetic_scenario("7aa")
        candidate = next(item for item in candidates_for("7aa") if item.name == "zero_both_counters")
        result = replay(scenario, candidate)
        self.assertGreater(result.metrics.slot_collisions, 0)
        self.assertTrue(result.metrics.rejected)

    def test_too_small_output_exposes_holes(self) -> None:
        scenario = dataclasses.replace(synthetic_scenario("7aa"), output_capacity=8)
        result = replay(scenario, candidates_for("7aa")[0])
        self.assertGreater(result.metrics.holes, 0)
        self.assertGreater(result.metrics.invalid_refs, 0)
        self.assertTrue(result.metrics.rejected)

    def test_wrong_address_unit_is_rejected_for_invalid_references(self) -> None:
        scenario = synthetic_scenario("7aa")
        candidate = next(
            item for item in candidates_for("7aa") if item.name == "byte_offset_confusion"
        )
        result = replay(scenario, candidate)
        self.assertGreater(result.metrics.invalid_refs, 0)
        self.assertGreater(result.metrics.count_mismatch, 0)
        self.assertTrue(result.metrics.rejected)

    def test_unexpected_duplicate_reference_is_rejected(self) -> None:
        scenario = synthetic_scenario("016")
        records = list(scenario.direct_records)
        records[1] = Pair(records[0].lo, records[1].hi)
        scenario = dataclasses.replace(scenario, direct_records=tuple(records))
        result = replay(scenario, candidates_for("016")[0])
        self.assertGreater(result.metrics.duplicate_refs, 0)
        self.assertTrue(result.metrics.rejected)

    def test_spatial_nonfinite_value_is_a_hard_rejection(self) -> None:
        scenario = synthetic_scenario("016")
        first_reference = next(iter(scenario.expected_output.values())).low24_ref
        points = dict(scenario.spatial_by_ref)
        points[first_reference] = (math.nan, 0.0, 0.0)
        scenario = dataclasses.replace(scenario, spatial_by_ref=points)
        result = replay(scenario, candidates_for("016")[0])
        self.assertGreater(result.metrics.spatial_invalid, 0)
        self.assertTrue(result.metrics.rejected)

    def test_visibility_log_parser_checks_the_observed_shift(self) -> None:
        text = "\n".join(
            (
                "Dreams visibility list candidate #1[0] selector=0x5523 source_index=21 "
                "valid=true values=0xc3003b1a,0x10011 flags=1",
                "Dreams visibility list pre #1 sequence=1 shader=0x7aa925e9 active=175 "
                "gds_count[324]=175 gds_a[351]=0 gds_b[352]=175",
                "Dreams visibility list post #1 gds_a[351]=0->175 gds_b[352]=175->175 changed=175",
                "Dreams visibility list consume #1 instances=175 invalid_lookup_ids=0",
            )
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "capture.log"
            path.write_text(text, encoding="utf-8")
            dispatches = parse_visibility_log(path)
        self.assertEqual(len(dispatches), 1)
        dispatch = dispatches[0]
        self.assertTrue(dispatch.sampled_mapping_valid)
        self.assertEqual(dispatch.gds_pre, (0, 175))
        self.assertEqual(dispatch.gds_post, (175, 175))
        self.assertEqual(dispatch.instances, 175)
        self.assertEqual(dispatch.samples[0]["pair"], Pair(0xC3003B1A, 0x10011))


def _write_minimal_vs370(directory: Path) -> None:
    directory.mkdir(parents=True)
    (directory / "manifest.tsv").write_text(
        "key\tvalue\ninstances\t1\nindex_size\t2\ninvalid_vertices\t0\n",
        encoding="utf-8",
    )
    (directory / "draw.bin").write_bytes(struct.pack("<IIIII", 3, 1, 0, 0, 0))
    (directory / "indices.bin").write_bytes(struct.pack("<HHH", 0, 1, 2))
    (directory / "b3-list.bin").write_bytes(struct.pack("<II", 0xABCD5678, 0x00001234))
    (directory / "b0-object-keys.bin").write_bytes(struct.pack("<I", 0x1234))
    interface: list[float] = []
    for position in ((-1.0, -1.0, 0.0, 1.0), (1.0, -1.0, 0.0, 1.0), (0.0, 1.0, 0.0, 1.0)):
        interface.extend(position)
        interface.extend((0.0,) * 12)
    (directory / "vs-interface.bin").write_bytes(struct.pack(f"<{len(interface)}f", *interface))
    (directory / "vs-validity.bin").write_bytes(struct.pack("<III", 0x1FFFF, 0x1FFFF, 0x1FFFF))


def _geometry_for(reference: int) -> tuple[float, ...]:
    center = float(reference % 97) / 8.0
    interface: list[float] = []
    for position in (
        (center - 0.4, -0.4, 0.0, 1.0),
        (center + 0.4, -0.4, 0.0, 1.0),
        (center, 0.4, 0.0, 1.0),
    ):
        interface.extend(position)
        interface.extend((0.0,) * 12)
    return tuple(interface)


def _write_aligned_capture(directory: Path) -> None:
    directory.mkdir(parents=True)
    active = 70
    d8_input = tuple(((index * 29 + 7) % active) << 10 for index in range(active))
    d8_exact = replay_d8(d8_input, candidates_for("d8")[0])
    selectors = tuple(item.value for item in d8_exact.output if item is not None)
    lookup = tuple(
        Pair(0xC0000000 | (0x1000 + index), ((1 if index % 2 == 0 else 2) << 16) | index)
        for index in range(active)
    )
    scenario = ReplayScenario(
        name="aligned_fixture",
        shader="7aa",
        active_count=active,
        selectors=selectors,
        lookup=lookup,
        arrival_order=(1, 0),
        initial_a=0,
        initial_b=active,
        output_capacity=active * 2,
        valid_ref_ids=frozenset(pair.low24_ref for pair in lookup),
        stable_keys=tuple(range(active)),
    )
    exact = replay(scenario, candidates_for("7aa")[0])
    output_size = max(exact.counter_a, exact.counter_b)
    output = [Pair(0, 0)] * output_size
    for slot, pair in exact.output.items():
        output[slot] = pair
    draw_count = exact.counter_a

    binaries: dict[str, bytes] = {}
    gds_pre = [0] * 16384
    gds_pre[352] = active
    gds_post_d8 = list(gds_pre)
    for item in d8_exact.passes:
        gds_post_d8[341 + item.pass_index] = item.ones
    gds_post_list = list(gds_post_d8)
    gds_post_list[351] = exact.counter_a
    gds_post_list[352] = exact.counter_b
    binaries["gds-pre.bin"] = struct.pack("<16384I", *gds_pre)
    binaries["gds-post-d8.bin"] = struct.pack("<16384I", *gds_post_d8)
    binaries["gds-post-7aa.bin"] = struct.pack("<16384I", *gds_post_list)
    binaries["d8-input.bin"] = struct.pack(f"<{active}I", *d8_input)
    binaries["d8-output-selectors.bin"] = struct.pack(f"<{active}I", *selectors)
    binaries["7aa-lookup.bin"] = b"".join(struct.pack("<II", pair.lo, pair.hi) for pair in lookup)
    binaries["7aa-output.bin"] = b"".join(struct.pack("<II", pair.lo, pair.hi) for pair in output)
    binaries["draw-indirect.bin"] = struct.pack("<IIIII", 3, draw_count, 0, 0, 0)
    binaries["indices.bin"] = struct.pack("<HHH", 0, 1, 2)
    valid_refs = sorted(pair.low24_ref for pair in lookup)
    binaries["valid-reference-ids.bin"] = struct.pack(f"<{len(valid_refs)}I", *valid_refs)
    binaries["candidate-records.bin"] = binaries["7aa-lookup.bin"]
    geometries = {pair: _geometry_for(pair.low24_ref) for pair in lookup}
    candidate_values = tuple(value for pair in lookup for value in geometries[pair])
    binaries["candidate-interface.bin"] = struct.pack(
        f"<{len(candidate_values)}f", *candidate_values
    )
    baseline_values = tuple(
        value for slot in range(draw_count) for value in geometries[exact.output[slot]]
    )
    binaries["vs370-interface.bin"] = struct.pack(
        f"<{len(baseline_values)}f", *baseline_values
    )
    validity = (0x1FFFF,) * (draw_count * 3)
    binaries["vs370-validity.bin"] = struct.pack(f"<{len(validity)}I", *validity)
    for name, data in binaries.items():
        (directory / name).write_bytes(data)
    metadata = {
        "schema": 1,
        "shader": "7aa",
        "active_count": active,
        "group_size": 64,
        "arrival_order": [1, 0],
        "initial_a": 0,
        "initial_b": active,
        "expected_counter_a": exact.counter_a,
        "expected_counter_b": exact.counter_b,
        "d8_pass_count": 10,
        "vertices_per_instance": 3,
        "attributes_per_vertex": 4,
        "index_size": 2,
        "files": {
            name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            for name, data in binaries.items()
        },
    }
    (directory / "dispatch.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")


class CaptureTests(unittest.TestCase):
    def test_aligned_capture_builds_distinct_actual_geometry_frames(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "aligned"
            _write_aligned_capture(directory)
            capture = load_aligned_capture(directory)
            report = build_candidate_frames(capture)
        self.assertGreaterEqual(len(report.frames), 2)
        sequences = {frame.pairs for frame in report.frames}
        self.assertEqual(len(sequences), len(report.frames))
        self.assertTrue(all(frame.interfaces for frame in report.frames))
        self.assertTrue(
            all(
                math.isfinite(value)
                for frame in report.frames
                for interface in frame.interfaces
                for value in interface
            )
        )

    def test_aligned_loader_names_every_missing_upstream_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            occurrence = Path(temporary) / "occ00"
            _write_minimal_vs370(occurrence)
            with self.assertRaises(FileNotFoundError) as caught:
                load_aligned_capture(occurrence)
        message = str(caught.exception)
        self.assertIn("d8-input.bin", message)
        self.assertIn("7aa-lookup.bin", message)
        self.assertIn("candidate-records.bin", message)
        self.assertIn("candidate-interface.bin", message)

    def test_aligned_yes_result_records_semantics_and_all_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = root / "aligned"
            _write_aligned_capture(directory)
            capture = load_aligned_capture(directory)
            frame = build_candidate_frames(capture).frames[0]
            output = root / "winner.json"
            write_winner(output, aligned_winner_document(capture, frame))
            document = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(document["candidate_name"], frame.name)
        self.assertEqual(document["configuration"], frame.configuration)
        self.assertEqual(len(document["source_sha256"]), len(ALIGNED_CHAIN_REQUIRED))

    def test_vs370_fixture_scores_finite_reference_mappings(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            occurrence = Path(temporary) / "occ00"
            _write_minimal_vs370(occurrence)
            capture = load_vs370_capture(occurrence)
            candidates = capture.reference_candidates()
        self.assertEqual(capture.instances, 1)
        self.assertEqual(capture.vertices_per_instance, 3)
        self.assertEqual(capture.spatial_metrics()["invalid_vertices"], 0)
        self.assertEqual(candidates[0].invalid, 0)
        self.assertEqual(candidates[0].mapping.name, "second_word_32")

    def test_missing_geometry_buffer_is_named_exactly(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            occurrence = Path(temporary) / "occ00"
            occurrence.mkdir()
            with self.assertRaises(FileNotFoundError) as caught:
                load_vs370_capture(occurrence)
        message = str(caught.exception)
        for name in VS370_REQUIRED:
            self.assertIn(name, message)

    def test_inventory_refuses_to_claim_a_full_chain(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            occurrence = root / "captures" / "vs370-interface-test" / "occ00"
            _write_minimal_vs370(occurrence)
            report = inventory_capture_root(root)
        sufficiency = report["data_sufficiency"]
        self.assertTrue(sufficiency["post_vs_actual_geometry_viewer"])
        self.assertFalse(sufficiency["full_real_chain_candidate_selection"])
        self.assertEqual(sufficiency["missing_aligned_capture_files"], list(ALIGNED_CHAIN_REQUIRED))

    def test_yes_result_records_sources_and_scope(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            occurrence = root / "occ00"
            _write_minimal_vs370(occurrence)
            capture = load_vs370_capture(occurrence)
            candidate = capture.reference_candidates()[0]
            output = root / "winner.json"
            write_winner(output, winner_document(capture, candidate))
            document = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(document["candidate"]["name"], candidate.mapping.name)
        self.assertIn("not_proven", document)
        self.assertEqual(len(document["source_sha256"]), len(VS370_REQUIRED))

    @unittest.skipUnless(os.environ.get("DREAMS_CAPTURE_ROOT"), "set DREAMS_CAPTURE_ROOT for real-capture regression")
    def test_local_real_capture_inventory_and_vs_fixture(self) -> None:
        root = Path(os.environ["DREAMS_CAPTURE_ROOT"])
        report = inventory_capture_root(root)
        complete = [item for item in report["vs370_occurrences"] if item["complete"]]
        self.assertTrue(complete)
        capture = load_vs370_capture(Path(complete[-1]["path"]))
        self.assertGreater(capture.instances, 0)
        self.assertEqual(capture.spatial_metrics()["invalid_vertices"], 0)


if __name__ == "__main__":
    unittest.main()
