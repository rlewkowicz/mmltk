"""Required parser, bounded selection, and log format behavior for ./mmltk --logs."""

from contextlib import contextmanager, redirect_stderr, redirect_stdout
from io import StringIO
from itertools import permutations
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import log_query as logs


def record(data, source="runtime.jsonl", line=1):
    return logs.parse_record(source, line, json.dumps(data))


class ParserTests(unittest.TestCase):
    def matches(self, expression, data):
        return logs.QueryParser(expression).parse().matches(record(data))

    def test_boolean_precedence_implicit_and_and_quoted_terms(self):
        data = {"event": "worker.failure", "owner": "explore", "message": "CUDA error: device lost"}
        self.assertTrue(self.matches('missing OR owner=explore "cuda ERROR" NOT retry', data))
        self.assertFalse(self.matches('(missing OR owner=other) AND "cuda error"', data))
        self.assertTrue(self.matches('NOT (owner=other OR event:ready)', data))
        self.assertTrue(self.matches("'error: device' AND *", data))
        self.assertFalse(self.matches('"*"', data))

    def test_exact_numeric_values_preserve_uint64_and_boolean_type(self):
        data = {"trace_id": 18446744073709551615, "value": 1, "flag": True, "ratio": 1.25}
        self.assertTrue(self.matches("trace_id=18446744073709551615 ratio>=1.2", data))
        self.assertFalse(self.matches("trace_id=18446744073709551614", data))
        self.assertFalse(self.matches('trace_id="18446744073709551615"', data))
        self.assertFalse(self.matches("value=true OR flag=1 OR flag>=0", data))
        self.assertTrue(self.matches("value!=true AND flag=true", data))

    def test_presence_null_and_missing_comparisons(self):
        data = {"nil": None, "zero": 0, "empty": ""}
        self.assertTrue(self.matches('has(nil) nil=null has(zero) empty=""', data))
        self.assertFalse(self.matches("absent!=0 OR absent!~anything", data))
        self.assertTrue(self.matches("NOT absent=0 AND NOT has(absent)", data))

    def test_substrings_regex_and_nested_fields(self):
        data = {"fields": {"name": "iced.draw.accepted", "trace_id": 88, "nested": {"value": 17}}}
        self.assertTrue(self.matches('fields.nested.value>16 trace_id=88 @event:ICED', data))
        self.assertTrue(self.matches('@event~"^iced[.]draw" AND @event!~"failure$"', data))
        self.assertTrue(self.matches('@event~"(?i)ACCEPTED"', data))
        self.assertFalse(self.matches('@event~"ACCEPTED"', data))

    def test_invalid_queries_report_errors(self):
        invalid = (
            "(", ")", "()", "NOT", "AND error", "error OR", "error AND",
            "event=", "event==value", "duration_ns>slow", "duration_ns>true",
            "event~'[invalid'", '"unclosed', "field='unclosed", "has()",
            'has("event")', '"event"=start', "duration_ns>1e9999", "error ! failure",
            "(" * 34 + "error" + ")" * 34, "a " * 513, "a" * 8193,
        )
        for expression in invalid:
            with self.subTest(expression=expression[:80]):
                with self.assertRaises((logs.QueryError, SyntaxError)):
                    logs.QueryParser(expression).parse()


class LogFormatTests(unittest.TestCase):
    def test_complete_jsonl_decodes_each_payload_once(self):
        data = {"event": "sample", "fields": {"nested": list(range(100))}}
        with patch.object(logs.JSON_DECODER, "raw_decode", wraps=logs.JSON_DECODER.raw_decode) as decoded:
            row = record(data)
        self.assertEqual(row.data, data)
        self.assertFalse(row.parse_error)
        self.assertEqual(decoded.call_count, 1)

    def test_runtime_and_wrapped_json_keep_original_vocabulary(self):
        row = record({
            "event": "browser.telemetry",
            "fields": {"name": "iced.draw.accepted", "trace_id": 52, "steady_ns": 123},
        })
        self.assertEqual(row.get("event"), "browser.telemetry")
        self.assertEqual(row.get("@event"), "iced.draw.accepted")
        self.assertEqual(row.get("trace_id"), 52)
        self.assertEqual(row.get("@owner"), "iced")
        self.assertEqual((row.clock, row.time_ns), ("steady", 123))

    def test_surface_identity_has_one_exact_projection(self):
        native = record({"surface_high": 0xFEDCBA9876543210, "surface_low": 0x1234567890ABCDEF})
        firefox = record({"surface": "FEDCBA98765432101234567890ABCDEF"})
        self.assertEqual(native.get("@surface"), firefox.get("@surface"))
        self.assertEqual(native.get("@surface"), "fedcba98765432101234567890abcdef")
        self.assertIs(record({"surface_high": -1, "surface_low": 5}).get("@surface"), logs.MISSING)
        self.assertIs(record({"surface": "12"}).get("@surface"), logs.MISSING)

    def test_workspace_source_is_separate_from_artifact_and_arena(self):
        native = record({"workspace_source_high": 11, "workspace_source_low": 12,
                         "surface_high": 21, "surface_low": 22})
        browser = record({"source": "000000000000000b000000000000000c",
                          "surface": "00000000000000150000000000000016"}, source="firefox.log")
        self.assertEqual(native.get("@workspace_source"), browser.get("@workspace_source"))
        self.assertEqual(native.get("@surface"), browser.get("@surface"))
        self.assertNotEqual(native.get("@surface"), native.get("@workspace_source"))
        self.assertEqual(browser.source, "firefox.log")
        self.assertIs(record({"source": "wrong"}).get("@workspace_source"), logs.MISSING)
        self.assertIs(record({"source": "000000000000000b000000000000000c",
                              "workspace_source_high": 11, "workspace_source_low": 13}).get("@workspace_source"), logs.MISSING)

    def test_transfer_correlation_uses_source_and_rejects_arena_counter_aliases(self):
        def identities(source, arena=20, transfer=3):
            return logs.auto_facts(record({"event": "presentation.release_wait.completed",
                "workspace_source_high": 1, "workspace_source_low": source,
                "surface_high": 1, "surface_low": arena, "transfer_sequence": transfer,
                "presentation_revision": 7}))[1]
        first = {identity for _, identity in identities(10) if "transfer_sequence" in identity.names}
        self.assertTrue(first)
        self.assertTrue(all("@workspace_source" in identity.names for identity in first))
        self.assertFalse(first.intersection(identity for _, identity in identities(11)))
        self.assertFalse(first.intersection(identity for _, identity in identities(10, transfer=4)))
        # Source identity survives an arena replacement; the separate arena fact
        # remains available to the automatic correlator's contradiction guard.
        self.assertEqual(first, {identity for _, identity in identities(10, arena=21)
                                 if "transfer_sequence" in identity.names})

    def test_native_header_and_key_value_messages(self):
        row = logs.parse_record("native.log", 4,
            "1970-01-01 00:00:01.123456789 [694:836] [mmltk-browser-host] [debug] "
            "event=execution.placement owner=compiled-read device=0 cpu=2 detail='two words' failed=false")
        self.assertEqual(row.format, "native")
        self.assertEqual((row.get("pid"), row.get("tid")), (694, 836))
        self.assertEqual(row.get("@event"), "execution.placement")
        self.assertEqual(row.get("@owner"), "compiled-read")
        self.assertEqual(row.get("detail"), "two words")
        self.assertEqual(row.get("device"), 0)
        self.assertEqual(row.get("failed"), False)
        self.assertEqual((row.clock, row.time_ns), ("wall-local", 1123456789))

    def test_firefox_moz_log_including_optional_timestamp(self):
        message = "[Child 780: IPC I/O Child]: E/ipc OnChannelErrorFromLink"
        row = logs.parse_record("firefox.log", 31, message)
        self.assertEqual(row.format, "firefox")
        self.assertEqual(row.get("pid"), 780)
        self.assertEqual(row.get("thread"), "IPC I/O Child")
        self.assertEqual(row.get("@owner"), "ipc")
        self.assertEqual(row.get("@level"), "error")
        self.assertTrue(row.get("@error"))
        self.assertEqual(row.clock, "none")
        timed = logs.parse_record("firefox.log", 32, "1970-01-01 00:00:01.000001 UTC " + message)
        self.assertEqual((timed.clock, timed.time_ns), ("wall", 1000001000))

    def test_embedded_json_and_unstructured_records_keep_provenance(self):
        row = logs.parse_record("firefox.log", 5,
            'console.error: {"event":"integration.annotation_pixel","error":0,"matched":true}')
        self.assertEqual(row.format, "json")
        self.assertFalse(row.get("@error"))
        self.assertEqual(row.source, "firefox.log")
        self.assertEqual(row.line, 5)
        self.assertTrue(row.raw.startswith("console.error:"))
        plain = logs.parse_record("acceptance.out", 6, "\x1b[31mFAILED: require ready\x1b[0m")
        self.assertTrue(plain.get("@error"))
        self.assertEqual(plain.raw, "FAILED: require ready")

    def test_json_and_plain_failure_candidates(self):
        self.assertFalse(record({"event": "integration.pixel", "error": 0}).get("@error"))
        self.assertTrue(record({"event": "worker.failed"}).get("@error"))
        self.assertTrue(record({"level": "critical", "message": "unavailable"}).get("@error"))
        self.assertTrue(record({"message": "operation timed out"}).get("@error"))
        self.assertTrue(logs.parse_record("test.log", 2, "Segmentation fault (core dumped)").get("@error"))

    def test_malformed_json_remains_searchable(self):
        for source, text in (("trace.jsonl", "bad data"), ("firefox.log", '{"event":"bad",'),
                             ("trace.jsonl", '{"value":NaN}'), ("trace.jsonl", "[]")):
            with self.subTest(source=source, text=text):
                row = logs.parse_record(source, 9, text)
                self.assertTrue(row.parse_error)
                self.assertEqual(row.format, "text")
                self.assertEqual(row.get("@text"), text)

    def test_clock_domains_and_nanosecond_precision(self):
        self.assertEqual(logs.timestamp_ns("1970-01-01T01:00:01.123456789+01:00"), ("wall", 1123456789))
        self.assertEqual(record({"timestamp_ns": 9007199254740993}).time_ns, 9007199254740993)
        elapsed = record({"elapsed_ms": 1.234567}, "firefox.log")
        self.assertEqual((elapsed.clock, elapsed.time_ns), ("elapsed:firefox.log", 1234567))
        self.assertEqual(record({"timestamp": "not a timestamp"}).clock, "none")
        self.assertEqual(record({"time_ns": 55}).clock, "none")

    def test_non_scalar_event_and_overflowing_json_numbers_do_not_crash(self):
        context = logs.TranscriptContext()
        rows = list(context.parse_line("log.jsonl", 1, '{"event":{"name":"future"},"steady_ns":5}', {}, {}))
        self.assertEqual(rows[0].get("event.name"), "future")
        self.assertTrue(logs.parse_record("log.jsonl", 2, '{"value":1e9999}').parse_error)
        self.assertTrue(logs.parse_record("log.jsonl", 3, '{"event":"one"} trailing').parse_error)
        row = logs.parse_record("log.log", 4, '{"event":"one"} {"event":"two"}')
        self.assertEqual(row.get("@event"), "one")
        self.assertIn("multiple JSON payloads", row.parse_error)

    def test_signal_semantics_preserve_encoded_child_exit_status(self):
        for code, number, name in ((139, 11, "SIGSEGV"), (143, 15, "SIGTERM")):
            row = record({"event": "child.signaled", "value": code})
            self.assertEqual(row.get("@exit_code"), code)
            self.assertEqual(row.get("@signal_number"), number)
            self.assertEqual(row.get("@signal"), name)
            self.assertTrue(row.get("@terminal"))
            self.assertTrue(logs.QueryParser(name).parse().matches(row))
        exited = record({"event": "child.exited", "value": 139})
        self.assertIs(exited.get("@signal"), logs.MISSING)
        self.assertFalse(record({"event": "child.exited", "value": 0}).get("@error"))

    def test_catch_failure_preserves_parent_and_extracts_quoted_json(self):
        context = logs.TranscriptContext()
        list(context.parse_line("test.log", 1, "[ RUN ] viewer_copy", {}, {}))
        payload = {"event": "firefox.webgpu.blocking_map", "buffer": "BufferId(21,1)",
                   "detail": "arc_extract", "strong_count": 2, "settled": False}
        line = "/workspace/test.cpp:17: failed: count == 1 with 1 message: " + json.dumps(json.dumps(payload))
        rows = list(context.parse_line("test.log", 2, line, {}, {}))
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0].get("@event"), "catch.assertion_failed")
        self.assertEqual(rows[0].get("source_line"), 17)
        self.assertEqual(rows[1].get("buffer"), "BufferId(21,1)")
        self.assertEqual(rows[1].metadata["part"], 1)
        self.assertEqual(rows[1].metadata["context_copy"], "Catch INFO")
        self.assertEqual(rows[1].line, 2)

    def test_wrapped_native_context_and_image_results(self):
        row = logs.parse_record("test.log", 8,
            "' and 'native runtime log: 1970-01-01 00:00:01 [1:2] [host] [trace] event=onnx.completed")
        self.assertEqual(row.get("@event"), "onnx.completed")
        self.assertEqual(row.clock, "wall-local")
        image = logs.parse_record("build.log", 20, "#37 naming to docker.io/library/mmltk done")
        self.assertEqual(image.get("@event"), "build.image")
        self.assertTrue(image.get("@terminal"))
        terminal = logs.parse_record("test.log", 21, "native host terminal status: exit 139")
        self.assertEqual(terminal.get("@exit_code"), 139)
        self.assertIs(terminal.get("@signal"), logs.MISSING)


class TriageSelectionTests(unittest.TestCase):
    def setUp(self):
        options = logs.argument_parser().parse_args(["--triage", "--triage-identities", "64"])
        self.triage = logs.Triage(options, logs.Expression("all", ()), {})
        self.triage.run = "capture"

    def row(self, data, line=1, source="input.jsonl"):
        row = record(data, source, line)
        row.metadata.update({"run": "capture", "mtime_ns": 1})
        return row

    def test_reservoir_equal_ranks_preserve_insertion_priority_after_updates(self):
        pool = logs.TriagePool(2)
        row = self.row({"event": "sample"})
        for key, rank in (("a", 3), ("b", 3), ("a", 1), ("b", 1), ("c", 0), ("late", 1)):
            pool.add(row, (rank,), key, key)
        self.assertEqual(list(pool.entries), ["b", "c"])
        self.assertEqual(pool.discarded, 2)
        for rank in range(-1, -1001, -1):
            pool.add(row, (rank,), "improved", "b")
            self.assertLessEqual(len(pool._heap), 2 * pool.limit)
        self.assertEqual([why for _, _, why in pool.rows()], ["improved", "c"])
        self.assertEqual(pool.discarded, 2)

    def test_saturated_reservoir_has_logarithmic_comparison_budget(self):
        class CountedRank(int):
            comparisons = 0

            def __eq__(self, other):
                type(self).comparisons += 1
                return super().__eq__(other)

        capacity, total = 256, 4096
        pool = logs.TriagePool(capacity)
        row = self.row({"event": "sample"})
        for index in range(total):
            pool.add(row, (CountedRank(total - index),), "sample", index)
        comparisons = CountedRank.comparisons
        self.assertLess(comparisons, total * 4 * capacity.bit_length())
        self.assertEqual([int(rank[0]) for rank, _, _ in pool.rows()], list(range(1, capacity + 1)))
        self.assertEqual(pool.discarded, total - capacity)
        self.assertLessEqual(len(pool._heap), 2 * capacity)

    def test_counter_lookup_reads_each_field_once_at_identity_capacity(self):
        for index in range(64):
            origin = self.row({"generation": index + 1, "slot": 0}, source=f"{index}.jsonl")
            strong = logs.Identity("capture", ("request_id",), (str(index + 1),))
            self.triage.add_refinement(origin, strong)
        row = self.row({"generation": 64, "slot": 0}, source="63.jsonl")
        with patch.object(row, "get", wraps=row.get) as fields:
            neighbor = self.triage.counter_neighbor(row)
        self.assertEqual(neighbor[0].values, ("64", "0"))
        self.assertEqual(neighbor[1], strong)
        self.assertEqual(neighbor[2], "line")
        self.assertLessEqual(fields.call_count, 3)

    def test_counter_lookup_preserves_first_matching_schema_and_origin(self):
        strong = logs.Identity("capture", ("request_id",), ("1",))
        schemas = ({"generation": 4, "slot": 0}, {"sequence": 7, "generation": 4},
                   {"sequence": 7, "generation": 4, "slot": 0}, {"sequence": 7, "slot": 0})
        for index, data in enumerate(schemas):
            self.triage.add_refinement(self.row(data, line=index + 1), strong)
        row = self.row(schemas[2], line=20)
        self.assertEqual(self.triage.counter_neighbor(row)[0].names, ("generation", "slot"))
        self.assertIsNone(self.triage.counter_neighbor(self.row(schemas[2], line=1000)))
        self.assertIsNone(self.triage.counter_neighbor(self.row({"generation": "4", "slot": False})))

    def test_automatic_anchor_search_is_logarithmic_and_returns_two_neighbors(self):
        class CountedTimes(list):
            reads = 0

            def __getitem__(self, index):
                self.reads += 1
                return super().__getitem__(index)

        group = logs.AutoAnchorGroup()
        for index in range(logs.MAX_AUTO_ANCHORS):
            group.add(index, self.row({"steady_ns": index}, line=index + 1))
        group.finish()
        group.times["steady"] = times = CountedTimes(group.times["steady"])
        self.assertEqual(group.nearest(self.row({"steady_ns": 64})), (63, 64))
        self.assertLessEqual(times.reads, logs.MAX_AUTO_ANCHORS.bit_length() + 2)
        self.assertEqual(group.nearest(self.row({}, source="another.log")), (0,))

    def pixel_row(self, boundary, transfer=1, rgba=10, line=1):
        data = {
            "surface": "00000000000000010000000000000002", "presentation_revision": 1,
            "source": "00000000000000030000000000000004",
            "sample_index": 0, "sample_x": 4, "sample_y": 5, "sample_rgba": rgba,
            "transfer_sequence": transfer, "boundary": boundary,
            "event": "firefox.workspace.frame_forwarded" if boundary == "forwarded" else
                     "presentation.pixel" if boundary == "native" else
                     "iced.surface.pixel" if boundary == "sample" else "firefox.workspace.pixel",
        }
        if boundary in ("import", "mailbox"):
            del data["source"]
        return self.row(data, line)

    def test_pixel_edges_keep_the_same_evidence_for_every_arrival_order(self):
        for order in permutations(("native", "forwarded", "import", "mailbox", "sample")):
            with self.subTest(order=order):
                self.triage.pixel_samples.clear()
                self.triage.pixel_sources.clear()
                self.triage.pixel_pending.clear()
                self.triage.pixel_pending_count = 0
                self.triage.pixel_stage_count = 0
                self.triage.pixel_divergences.clear()
                self.triage.findings.clear()
                for line, boundary in enumerate(order, 1):
                    row = self.pixel_row(boundary, rgba=10 if boundary == "native" else 11, line=line)
                    self.triage.observe_pixel_chain(row, emit=True)
                self.triage.finalize_pixel_chain(emit=True)
                finding, = self.triage.findings.values()
                self.assertEqual(finding.kind, "pixel-chain-divergence")
                self.assertIn("native -> import", finding.message)
                self.assertEqual(finding.record.get("boundary"), "import")
                self.assertEqual(finding.other.get("boundary"), "native")
                self.assertEqual(self.triage.pixel_pending_count, 0)

    def test_direct_pixels_join_only_the_acquired_source_in_every_arrival_order(self):
        for order in permutations(("native", "forwarded", "sample")):
            for rgba in (10, 11):
                with self.subTest(order=order, rgba=rgba):
                    triage = logs.Triage(self.triage.options, self.triage.where, {})
                    for boundary in order:
                        record = self.pixel_row(boundary, rgba=rgba if boundary == "sample" else 10)
                        record.data["direct_sampling"] = True
                        triage.observe_pixel_chain(record, emit=True)
                    triage.finalize_pixel_chain(emit=True)
                    if rgba == 10:
                        self.assertFalse(triage.findings)
                    else:
                        finding, = triage.findings.values()
                        self.assertEqual(finding.kind, "pixel-chain-divergence")
                        self.assertIn("native -> sample", finding.message)

    def test_direct_pixels_reject_copy_receipts_before_or_after_acquisition(self):
        for order in permutations(("forwarded", "import", "sample")):
            with self.subTest(order=order):
                triage = logs.Triage(self.triage.options, self.triage.where, {})
                for boundary in order:
                    record = self.pixel_row(boundary)
                    record.data["direct_sampling"] = True
                    triage.observe_pixel_chain(record, emit=True)
                finding, = triage.findings.values()
                self.assertEqual(finding.kind, "pixel-mode-conflict")

    def test_absent_pixel_bridges_finalize_each_exact_transfer_once(self):
        for boundary, transfer, line in (("import", 7, 9), ("mailbox", 7, 3), ("import", 8, 4)):
            self.triage.observe_pixel_chain(self.pixel_row(boundary, transfer, line=line), emit=True)
        self.assertEqual(self.triage.pixel_pending_count, 3)
        self.assertFalse(self.triage.findings)
        self.triage.finalize_pixel_chain(emit=True)
        findings = sorted(self.triage.findings.values(), key=lambda item: item.record.line)
        self.assertEqual([item.kind for item in findings], ["pixel-transfer-missing"] * 2)
        self.assertEqual([item.record.line for item in findings], [3, 4])
        for finding, transfer in zip(findings, (7, 8)):
            self.assertIn("run=capture", finding.message)
            self.assertIn("surface=00000000000000010000000000000002", finding.message)
            self.assertIn(f"presentation_revision=1 transfer_sequence={transfer}", finding.message)
            self.assertIs(finding.record.get("@workspace_source"), logs.MISSING)
        self.assertIn("2 pixel receipt(s)", findings[0].message)
        self.assertFalse(self.triage.pixel_pending)
        self.assertEqual(self.triage.pixel_pending_count, 0)
        self.triage.finalize_pixel_chain(emit=True)
        self.assertEqual(len(self.triage.findings), 2)

    def test_exact_pixel_bridge_succeeds_before_or_after_the_pixel(self):
        for order in (("import", "forwarded"), ("forwarded", "import")):
            with self.subTest(order=order):
                triage = logs.Triage(self.triage.options, self.triage.where, {})
                triage.observe_pixel_chain(self.pixel_row("native"), emit=True)
                for boundary in order:
                    triage.observe_pixel_chain(self.pixel_row(boundary), emit=True)
                triage.finalize_pixel_chain(emit=True)
                self.assertFalse(triage.findings)
                self.assertFalse(triage.pixel_pending)
                self.assertEqual(triage.pixel_pending_count, 0)
                chain, = triage.pixel_samples.values()
                self.assertEqual({stage[0] for stage in chain.stages}, {"native", "import"})

    def test_pending_pixel_capacity_counts_replay_and_finalization_exactly(self):
        with patch.object(logs, "MAX_TRIAGE_PIXEL_SAMPLES", 3):
            for line, transfer in enumerate((1, 1, 2, 3), 1):
                self.triage.observe_pixel_chain(self.pixel_row("import", transfer, line=line), emit=True)
            self.assertEqual(self.triage.pixel_pending_count, 3)
            self.assertEqual(sum(map(len, self.triage.pixel_pending.values())), 3)
            self.assertTrue(any("Unbridged pixel sample capacity reached" in note for note in self.triage.notes))
            self.triage.observe_pixel_chain(self.pixel_row("forwarded", 1, line=5), emit=True)
            self.assertEqual(self.triage.pixel_pending_count, 1)
            self.triage.observe_pixel_chain(self.pixel_row("import", 3, line=6), emit=True)
            self.assertEqual(self.triage.pixel_pending_count, 2)
            self.triage.finalize_pixel_chain(emit=True)
            self.assertEqual(len(self.triage.findings), 2)
            self.assertFalse(self.triage.pixel_pending)
            self.assertEqual(self.triage.pixel_pending_count, 0)

    def test_saturated_bridge_index_reports_incomplete_for_later_pixels(self):
        for emit in (False, True):
            with self.subTest(emit=emit), patch.object(logs, "MAX_TRIAGE_PIXEL_SAMPLES", 2):
                triage = logs.Triage(self.triage.options, self.triage.where, {})
                for transfer in (1, 2, 3):
                    triage.observe_pixel_chain(self.pixel_row("forwarded", transfer), emit=emit)
                self.assertEqual(len(triage.pixel_sources), 2)
                self.assertTrue(triage.pixel_sources_truncated)
                for line, boundary in enumerate(("import", "mailbox"), 1):
                    triage.observe_pixel_chain(self.pixel_row(boundary, 3, line=line), emit=emit)
                self.assertEqual(triage.pixel_pending_count, 2)
                triage.finalize_pixel_chain(emit=emit)
                self.assertFalse(triage.pixel_pending)
                self.assertEqual(triage.pixel_pending_count, 0)
                self.assertFalse(triage.pixel_samples)
                self.assertTrue(any("Pixel transfer bridge capacity reached" in note for note in triage.notes))
                if emit:
                    finding, = triage.findings.values()
                    self.assertEqual(finding.kind, "pixel-transfer-incomplete")
                    self.assertIn("bridge index was truncated", finding.message)
                    self.assertIn("transfer_sequence=3", finding.message)
                    self.assertIn("2 pixel receipt(s)", finding.message)
                else:
                    _, _, reason = triage.seed_pool.rows()[0]
                    self.assertIn("pixel-transfer-incomplete", reason)
                before = len(triage.findings), len(triage.seed_pool.entries)
                triage.finalize_pixel_chain(emit=emit)
                self.assertEqual((len(triage.findings), len(triage.seed_pool.entries)), before)
                self.assertEqual(triage.pixel_pending_count, 0)

    def test_saturated_bridge_index_resolves_pending_pixels_directly(self):
        for rgba in (10, 11):
            with self.subTest(rgba=rgba), patch.object(logs, "MAX_TRIAGE_PIXEL_SAMPLES", 2):
                triage = logs.Triage(self.triage.options, self.triage.where, {})
                for transfer in (1, 2):
                    triage.observe_pixel_chain(self.pixel_row("forwarded", transfer), emit=True)
                triage.observe_pixel_chain(self.pixel_row("native", 3), emit=True)
                for boundary in ("import", "mailbox"):
                    triage.observe_pixel_chain(self.pixel_row(boundary, 3, rgba=rgba), emit=True)
                self.assertEqual(triage.pixel_pending_count, 2)
                triage.observe_pixel_chain(self.pixel_row("forwarded", 3), emit=True)
                self.assertEqual(len(triage.pixel_sources), 2)
                self.assertTrue(triage.pixel_sources_truncated)
                self.assertFalse(triage.pixel_pending)
                self.assertEqual(triage.pixel_pending_count, 0)
                triage.finalize_pixel_chain(emit=True)
                chain, = triage.pixel_samples.values()
                self.assertEqual({stage[0] for stage in chain.stages}, {"native", "import", "mailbox"})
                self.assertEqual({item.kind for item in triage.findings.values()},
                                 {"pixel-chain-divergence"} if rgba != 10 else set())
                self.assertTrue(any("Pixel transfer bridge capacity reached" in note for note in triage.notes))

    def test_saturated_bridge_direct_replay_preserves_corroborating_conflict(self):
        with patch.object(logs, "MAX_TRIAGE_PIXEL_SAMPLES", 1):
            self.triage.observe_pixel_chain(self.pixel_row("forwarded"), emit=True)
            pixel = self.pixel_row("import", 2)
            pixel.data["generation"] = 4
            self.triage.observe_pixel_chain(pixel, emit=True)
            bridge = self.pixel_row("forwarded", 2)
            bridge.data["generation"] = 5
            self.triage.observe_pixel_chain(bridge, emit=True)
            self.triage.finalize_pixel_chain(emit=True)
            finding, = self.triage.findings.values()
            self.assertEqual(finding.kind, "pixel-transfer-conflict")
            self.assertFalse(self.triage.pixel_samples)
            self.assertEqual(self.triage.pixel_pending_count, 0)

    def test_full_but_untruncated_bridge_index_preserves_proven_missing(self):
        with patch.object(logs, "MAX_TRIAGE_PIXEL_SAMPLES", 2):
            for transfer in (1, 2):
                self.triage.observe_pixel_chain(self.pixel_row("forwarded", transfer), emit=True)
            self.triage.observe_pixel_chain(self.pixel_row("import", 3), emit=True)
            self.triage.finalize_pixel_chain(emit=True)
            self.assertFalse(self.triage.pixel_sources_truncated)
            finding, = self.triage.findings.values()
            self.assertEqual(finding.kind, "pixel-transfer-missing")
            self.assertEqual(self.triage.pixel_pending_count, 0)

    def test_missing_pixel_finalization_respects_finding_and_anchor_budgets(self):
        for emit in (False, True):
            triage = logs.Triage(self.triage.options, self.triage.where, {})
            with patch.object(logs, "MAX_TRIAGE_PIXEL_SAMPLES", 70):
                for transfer in range(1, 72):
                    triage.observe_pixel_chain(self.pixel_row("import", transfer, line=transfer), emit=emit)
                self.assertEqual(triage.pixel_pending_count, 70)
                triage.finalize_pixel_chain(emit=emit)
            self.assertFalse(triage.pixel_pending)
            self.assertEqual(triage.pixel_pending_count, 0)
            if emit:
                self.assertEqual(len(triage.findings), 64)
                self.assertTrue(any("Finding inventory capped" in note for note in triage.notes))
            else:
                self.assertFalse(triage.findings)
                self.assertEqual(len(triage.seed_pool.entries), triage.seed_pool.limit)
                self.assertGreater(triage.seed_pool.discarded, 0)

    def test_source_less_pixel_requires_an_exact_forwarded_transfer(self):
        self.triage.observe_pixel_chain(self.pixel_row("native"), emit=True)
        self.triage.observe_pixel_chain(self.pixel_row("import", rgba=11), emit=True)
        self.triage.observe_pixel_chain(self.pixel_row("forwarded", transfer=2), emit=True)
        self.assertFalse(self.triage.findings)
        self.triage.observe_pixel_chain(self.pixel_row("forwarded"), emit=True)
        self.assertEqual(next(iter(self.triage.findings.values())).kind, "pixel-chain-divergence")

    def test_forwarded_source_conflict_is_rejected(self):
        self.triage.observe_pixel_chain(self.pixel_row("forwarded"), emit=True)
        conflicting = self.pixel_row("forwarded", line=2)
        conflicting.data["source"] = "00000000000000050000000000000006"
        self.triage.observe_pixel_chain(conflicting, emit=True)
        self.triage.finalize_pixel_chain(emit=True)
        self.assertEqual(next(iter(self.triage.findings.values())).kind, "pixel-transfer-conflict")

    def test_pixel_owner_mutation_wins_before_cross_stage_comparison(self):
        self.triage.observe_pixel_chain(self.pixel_row("native"), emit=True)
        self.triage.observe_pixel_chain(self.pixel_row("native", rgba=11, line=2), emit=True)
        self.triage.observe_pixel_chain(self.pixel_row("import", rgba=12, line=3), emit=True)
        finding, = self.triage.findings.values()
        self.assertEqual(finding.kind, "pixel-owner-mutation")
        self.assertEqual((finding.other.line, finding.record.line), (1, 2))

    def test_pixel_transfer_comparisons_are_linear_in_samples(self):
        class CountedStages(dict):
            operations = 0

            def get(self, key, default=None):
                self.operations += 1
                return super().get(key, default)

            def __iter__(self):
                for key in super().__iter__():
                    self.operations += 1
                    yield key

        first = self.pixel_row("sample")
        self.triage.observe_pixel_chain(first, emit=True)
        chain, = self.triage.pixel_samples.values()
        chain.stages = stages = CountedStages(chain.stages)
        transfers = 256
        for transfer in range(1, transfers + 1):
            for boundary in ("native", "forwarded", "import", "mailbox"):
                self.triage.observe_pixel_chain(self.pixel_row(boundary, transfer), emit=True)
        self.assertLessEqual(stages.operations, transfers * 3 * 7)
        self.assertEqual(chain.mailbox_count, transfers)
        self.assertFalse(self.triage.findings)


class FileQueryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def write(self, name, contents):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(contents, list):
            contents = "".join(json.dumps(item) + "\n" for item in contents)
        path.write_text(contents, encoding="utf-8")
        return path

    def run_query(self, *arguments):
        output, diagnostics = StringIO(), StringIO()
        status = logs.main(list(arguments), root=self.root, output=output, diagnostics=diagnostics)
        return status, output.getvalue(), diagnostics.getvalue()

    def exported(self, *arguments):
        status, output, diagnostics = self.run_query(*arguments, "--format", "jsonl")
        return status, [json.loads(line) for line in output.splitlines()], diagnostics

    def automatic_fixture(self, progress=0):
        self.write("build/validation/auto.jsonl", [
            {"event": "explore.frame.published", "owner": "explore", "source_session": 1,
             "source_instance": 1, "source_revision": 7, "steady_ns": 100},
            {"event": "presentation.frame.edge", "owner": "presentation", "presentation_revision": 11,
             "source_session": 1, "source_instance": 1, "source_revision": 7, "steady_ns": 110},
            {"event": "presentation.pixel", "presentation_revision": 11, "steady_ns": 111},
        ])
        self.write("build/validation/auto-firefox.log", [
            *({"event": "integration.phase_progress", "control": "work", "detail": f"AwaitPhase{index}",
               "elapsed_ms": index, "a": 0, "b": 0, "c": 0, "d": 0} for index in range(progress)),
            {"event": "integration.explore_state", "control": "explore.gallery", "a": 19, "elapsed_ms": 1000},
            {"event": "integration.explore_reopen_wait", "control": "explore.open", "detail": "physical-gallery-draw",
             "a": 19, "b": 7, "c": 3, "d": 4, "elapsed_ms": 1001},
            {"event": "integration.explore_reopen_draw", "control": "explore.gallery", "detail": "physical-gallery-draw",
             "a": 12, "b": 6, "c": 7, "d": 11, "elapsed_ms": 1002},
        ])
        return ("--family", "auto", "--run", "current", "-q",
                'event=integration.phase_progress OR event=integration.explore_reopen_wait '
                'OR event=integration.explore_reopen_draw OR event=integration.explore_reopened')

    def test_automatic_timeline_interleaves_exact_frame_snapshot_and_publication(self):
        arguments = self.automatic_fixture()
        status, output, diagnostics = self.run_query(*arguments, "--format", "timeline",
                                                      "--fields", "event,control,a,b,c,d,detail")
        self.assertEqual(status, 0, diagnostics)
        lines = output.splitlines()
        self.assertEqual(sum("[query]" in line for line in lines), 2)
        self.assertEqual(sum("[auto:" in line for line in lines), 3)
        self.assertLess(output.index("explore.frame.published"), output.index("integration.explore_reopen_wait"))
        self.assertLess(output.index("presentation.frame.edge"), output.index("integration.explore_reopen_draw"))
        self.assertIn("explore.frame=7", output)
        self.assertIn("explore.snapshot=19", output)
        self.assertIn("presentation_revision=11", output)
        self.assertNotIn("presentation.pixel", output)
        self.assertIn("cross-clock times remain unaligned", diagnostics)

    def test_automatic_machine_formats_require_opt_in_and_preserve_query_rows(self):
        arguments = self.automatic_fixture()
        baseline = self.exported(*arguments)
        self.assertEqual(self.exported(*arguments, "--no-auto-correlate"), baseline)
        self.assertEqual(len(baseline[1]), 2)
        status, rows, diagnostics = self.exported(*arguments, "--auto-correlate")
        self.assertEqual(status, 0, diagnostics)
        self.assertEqual([row for row in rows if row["_log"]["match"] == "query"], baseline[1])
        automatic = [row for row in rows if row["_log"]["match"] == "auto"]
        self.assertEqual(len(automatic), 3)
        self.assertTrue(all(row["_log"]["auto_reason"] and row["_log"]["auto_anchor"] for row in automatic))
        _, projected, _ = self.exported(*arguments, "--auto-correlate", "--fields", "event")
        self.assertTrue(all(row.get("@auto_reason") and row.get("@auto_anchor")
                            for row in projected if row["@match"] == "auto"))
        _, output, _ = self.run_query(*arguments)
        self.assertNotIn("[auto:", output)
        _, output, _ = self.run_query(*arguments, "--format", "timeline", "--no-auto-correlate")
        self.assertNotIn("[auto:", output)

    def test_automatic_recent_highlight_keeps_final_reopen_visible_after_phase_volume(self):
        arguments = self.automatic_fixture(progress=201)
        status, output, diagnostics = self.run_query(*arguments, "--format", "timeline", "--limit", "350",
                                                      "--fields", "event,control,a,b,c,d,detail")
        self.assertEqual(status, 0, diagnostics)
        self.assertTrue(output.startswith("Recent query matches"))
        first_timeline = output.index("@file=")
        self.assertIn("integration.explore_reopen_wait", output[:first_timeline])
        self.assertIn("integration.explore_reopen_draw", output[:first_timeline])
        self.assertNotIn("integration.explore_reopened", output)
        self.assertEqual(output.count("[query]"), 203)
        self.assertIn("explore.frame.published", output[first_timeline:])
        self.assertIn("presentation.frame.edge", output[first_timeline:])
        self.assertIn("75 query anchors outside", diagnostics)
        status, rows, _ = self.exported(*arguments, "--auto-correlate", "--limit", "350")
        self.assertEqual(status, 0)
        selected = [row for row in rows if row["_log"]["match"] == "query"]
        self.assertEqual(len(selected), 203)
        self.assertEqual([row["data"]["elapsed_ms"] for row in selected], [*range(201), 1001, 1002])
        _, limited, _ = self.run_query(*arguments, "--format", "timeline", "--limit", "20", "--fields", "event")
        self.assertEqual(limited.count("[query]"), 20)
        summary = limited.split("@file=", 1)[0]
        self.assertIn("integration.explore_reopen_wait", summary)
        self.assertIn("integration.explore_reopen_draw", summary)
        self.write("plain.log", "".join(f"ordinary message {index}\n" for index in range(30)))
        status, plain, diagnostics = self.run_query("plain.log", "-q", "ordinary", "--format", "timeline", "--limit", "2")
        self.assertEqual(status, 0, diagnostics)
        self.assertIn("ordinary message 29", plain.split("[query]", 1)[0])
        self.assertEqual(plain.count("[query]"), 2)

    def test_automatic_scopes_reject_zero_generic_counter_conflicts_and_transitive_links(self):
        surface = "00000000000000010000000000000002"
        self.write("input.jsonl", [
            {"event": "seed", "request_id": 1, "source_session": 1, "source_instance": 1,
             "source_revision": 7, "surface": surface, "presentation_revision": 11, "a": 8, "b": 9},
            {"event": "frame.published", "request_id": 1, "trace_id": 55},
            {"event": "application.transitive", "trace_id": 55},
            {"event": "application.other_source", "request_id": 1, "source_session": 2, "source_instance": 1},
            {"event": "application.other_instance", "request_id": 1, "source_session": 1, "source_instance": 2},
            {"event": "application.other_frame", "request_id": 1, "source_revision": 8},
            {"event": "application.other_publication", "surface": surface, "presentation_revision": 12},
            {"event": "application.numeric_collision", "a": 8, "b": 9, "value": 7, "generation": 1},
            {"event": "application.empty", "request_id": 0, "span_id": False, "trace_id": "000", "operation_id": " "},
            {"event": "application.empty_strings", "request_id": " 0 ", "trace_id": "0000-0000"},
            {"event": "application.wrong_type", "request_id": "1"},
            {"event": "frame.pixel", "request_id": 1},
        ])
        _, rows, _ = self.exported("input.jsonl", "-q", "event=seed", "--auto-correlate")
        self.assertEqual({row["data"]["event"] for row in rows}, {"seed", "frame.published"})
        self.assertEqual(self.run_query("input.jsonl", "--auto-correlate")[0], 2)

    def test_automatic_ambiguous_projected_source_revisions_do_not_join(self):
        arguments = self.automatic_fixture()
        self.write("build/validation/auto-native.log", [
            {"event": "explore.frame.published", "owner": "explore", "source_session": 1,
             "source_instance": 2, "source_revision": 7, "steady_ns": 101},
        ])
        _, rows, _ = self.exported(*arguments, "--auto-correlate")
        self.assertNotIn("explore.frame.published", {row["data"]["event"] for row in rows})
        self.assertIn("integration.explore_state", {row["data"]["event"] for row in rows})
        self.write("build/validation/auto-native.log", [
            {"event": "native.seed", "owner": "explore", "source_session": 1,
             "source_instance": 2, "source_revision": 7, "steady_ns": 101},
        ])
        with_seed = (*arguments[:-1], arguments[-1] + " OR event=native.seed")
        _, rows, _ = self.exported(*with_seed, "--auto-correlate")
        self.assertNotIn("explore.frame.published", {row["data"]["event"] for row in rows})

    def test_automatic_control_proximity_is_labeled_and_bounded(self):
        self.write("input.jsonl", [
            {"event": "application.snapshot", "control": "explore.open", "steady_ns": 100000000},
            {"event": "seed", "control": "explore.open", "steady_ns": 200000000},
            {"event": "application.too_late", "control": "explore.open", "steady_ns": 1000000000},
            {"event": "application.other_control", "control": "annotation.open", "steady_ns": 200000001},
            {"event": "application.generic_control", "control": "work", "steady_ns": 200000001},
        ])
        _, rows, _ = self.exported("input.jsonl", "-q", "event=seed", "--auto-correlate")
        self.assertEqual({row["data"]["event"] for row in rows}, {"seed", "application.snapshot"})
        automatic, = [row for row in rows if row["_log"]["match"] == "auto"]
        self.assertIn("time proximity", automatic["_log"]["auto_reason"])
        _, rows, _ = self.exported("input.jsonl", "-q", "event=seed", "--auto-correlate", "--near-ms", "50")
        self.assertEqual(len(rows), 1)

    def test_automatic_bounds_diversity_global_capacity_and_existing_match_limit(self):
        self.write("input.jsonl", [
            row for index in range(80) for row in (
                {"event": "seed", "request_id": index + 1, "steady_ns": index * 100},
                *({"event": event, "request_id": index + 1, "steady_ns": index * 100 + offset}
                  for offset, event in enumerate(("frame.published", "application.snapshot", "application.ready",
                                                   "application.completed", "frame.published"), 1)),
            )
        ])
        arguments = ("input.jsonl", "-q", "event=seed", "--auto-correlate", "--limit", "150")
        status, rows, diagnostics = self.exported(*arguments)
        self.assertEqual(status, 0, diagnostics)
        self.assertEqual(self.exported(*arguments)[1], rows)
        automatic = [row for row in rows if row["_log"]["match"] == "auto"]
        self.assertEqual(len(automatic), logs.MAX_AUTO_RELATED)
        self.assertEqual(sum(row["_log"]["match"] == "query" for row in rows), 80)
        by_anchor = logs.Counter(logs.encoded(row["_log"]["auto_anchor"]) for row in automatic)
        by_identity = logs.Counter(row["_log"]["auto_reason"] for row in automatic)
        self.assertLessEqual(max(by_anchor.values()), logs.MAX_AUTO_PER_ANCHOR)
        self.assertLessEqual(max(by_identity.values()), logs.MAX_AUTO_PER_IDENTITY)
        _, limited, _ = self.exported("input.jsonl", "-q", "event=seed", "--auto-correlate", "--limit", "2")
        self.assertEqual(len(limited), 2)
        self.assertTrue(all(row["_log"]["match"] == "query" for row in limited))
        self.write("weak.jsonl", [
            row for index in range(20) for row in (
                {"event": "seed", "phase_id": index + 1, "steady_ns": index * 100},
                {"event": "application.snapshot", "phase_id": index + 1, "steady_ns": index * 100 + 1},
            )
        ])
        _, weak, _ = self.exported("weak.jsonl", "-q", "event=seed", "--auto-correlate", "--limit", "100")
        self.assertEqual(sum(row["_log"]["match"] == "auto" for row in weak), logs.MAX_AUTO_WEAK)

    def test_automatic_respects_where_runs_and_explicit_correlation(self):
        arguments = self.automatic_fixture()
        self.write("build/validation/auto.jsonl.history/57-1000.jsonl", [
            {"event": "explore.frame.published", "owner": "explore", "source_revision": 7},
        ])
        _, rows, _ = self.exported(*arguments, "--auto-correlate", "--where", "NOT event=presentation.frame.edge")
        self.assertEqual({row["_log"]["run"] for row in rows}, {"build/validation/auto@current"})
        self.assertNotIn("presentation.frame.edge", {row["data"]["event"] for row in rows})
        self.write("explicit.jsonl", [{"event": "seed", "request_id": 1}, {"event": "arbitrary", "request_id": 1}])
        explicit = ("explicit.jsonl", "-q", "event=seed", "--correlate", "request_id", "--format", "timeline")
        self.assertEqual(self.run_query(*explicit), self.run_query(*explicit, "--no-auto-correlate"))
        self.assertIn("[correlated]", self.run_query(*explicit)[1])

    def test_automatic_payloads_and_scan_count_stay_bounded(self):
        self.write("input.jsonl", [
            {"event": "seed", "request_id": 1},
            {"event": "application.snapshot", "request_id": 1, "detail": "x" * 20000},
        ])
        original = logs.LogFile.records
        with patch.object(logs.LogFile, "records", autospec=True, side_effect=original) as scans:
            status, rows, diagnostics = self.exported("input.jsonl", "-q", "event=seed", "--auto-correlate")
        self.assertEqual(status, 0, diagnostics)
        self.assertEqual(scans.call_count, 2)
        automatic, = [row for row in rows if row["_log"]["match"] == "auto"]
        self.assertTrue(automatic["_log"]["auto_payload_truncated"])
        self.assertNotIn("triage_payload_truncated", automatic["_log"])
        self.assertLessEqual(len(automatic["text"]), logs.MAX_TRIAGE_TEXT + 1)
        self.assertLessEqual(len(automatic["data"]["detail"]), logs.MAX_TRIAGE_TEXT + 1)

    def test_discovery_deduplicates_paths_and_excludes_histories_by_default(self):
        self.write("capture/trace.jsonl", [{"event": "one"}])
        self.write("capture/native.log", "plain\n")
        self.write("capture/trace.jsonl.history/previous.jsonl", [{"event": "old"}])
        self.write("capture/not-a-log.bin", "binary")
        files = logs.discover_files(["capture", "capture/trace.jsonl"], self.root)
        self.assertEqual([item.source for item in files], ["capture/native.log", "capture/trace.jsonl"])
        recursive = logs.discover_files(["capture"], self.root, recursive=True)
        self.assertEqual(len(recursive), 3)
        mapped = logs.discover_files(["/host/repo/capture/*.log"], self.root, host_root="/host/repo")
        self.assertEqual([item.source for item in mapped], ["capture/native.log"])

    def test_discovery_captures_each_repeated_path_only_once(self):
        self.write("capture/trace.jsonl", [{"event": "one"}])
        self.write("capture/native.log", "plain\n")
        with patch.object(logs.LogFile, "capture", wraps=logs.LogFile.capture) as captured:
            files = logs.discover_files(["capture", "capture", "capture/*", "capture/trace.jsonl"], self.root)
        self.assertEqual(len(files), 2)
        self.assertEqual(captured.call_count, 2)

    def test_directory_discovery_streams_entries_and_releases_handles(self):
        for index in range(3):
            self.write(f"capture/{index}.log", "")
        produced = []
        closed = []
        original = logs.os.scandir

        @contextmanager
        def scanned(directory):
            try:
                with original(directory) as entries:
                    def counted():
                        for entry in entries:
                            produced.append(entry.name)
                            yield entry
                    yield counted()
            finally:
                closed.append(True)

        with patch.object(logs.os, "scandir", scanned):
            candidates = logs.directory_logs(self.root / "capture")
            self.assertEqual(next(candidates).suffix, ".log")
            self.assertEqual(len(produced), 1)
            candidates.close()
        self.assertEqual(closed, [True])

    def test_empty_missing_and_outside_inputs_fail_clearly(self):
        (self.root / "empty").mkdir()
        for path in ("missing", "empty", "/etc/passwd"):
            status, _, error = self.run_query(path)
            self.assertEqual(status, 2)
            self.assertTrue(error.startswith("mmltk logs:"))

    def test_file_boundaries_blank_lines_and_oversized_records(self):
        path = self.write("input.log", "\n" + "a" * 70 + "\n\nnext\n")
        source = logs.LogFile.capture(path, self.root)
        with patch.object(logs, "MAX_LINE_BYTES", 32):
            rows = list(source.records())
        self.assertEqual([item.line for item in rows], [2, 4])
        self.assertEqual(len(rows[0].raw), 32)
        self.assertTrue(rows[0].parse_error)
        self.assertEqual(rows[1].raw, "next")
        self.assertFalse(rows[1].parse_error)
        invalid = self.root / "invalid.log"
        invalid.write_bytes(b"invalid \xff bytes\n")
        decoded = list(logs.LogFile.capture(invalid, self.root).records())
        self.assertEqual(decoded[0].parse_error, "invalid UTF-8 bytes replaced")

    def test_captured_byte_size_and_correlation_stability(self):
        path = self.write("input.log", "first\n")
        source = logs.LogFile.capture(path, self.root)
        with path.open("a", encoding="utf-8") as output:
            output.write("second\n")
        self.assertEqual([item.raw for item in source.records()], ["first"])
        with self.assertRaisesRegex(logs.QueryError, "changed during correlation"):
            list(source.records(unchanged=True))
        path.write_text("", encoding="utf-8")
        with self.assertRaisesRegex(logs.QueryError, "truncated"):
            list(source.records())

    def test_retention_is_bounded_deterministic_and_counts_all_matches(self):
        self.write("b.jsonl", [{"event": "tick", "steady_ns": time} for time in (5, 1, 3)])
        self.write("a.jsonl", [{"event": "tick", "steady_ns": time} for time in (4, 2, 3)])
        status, rows, diagnostics = self.exported("*.jsonl", "--limit", "3")
        self.assertEqual(status, 0)
        self.assertEqual([item["data"]["steady_ns"] for item in rows], [1, 2, 3])
        self.assertEqual(rows[2]["_log"]["file"], "a.jsonl")
        self.assertIn("6 matched", diagnostics)
        self.assertIn("showing 3/6", diagnostics)
        _, tail, _ = self.exported("*.jsonl", "--tail", "--limit", "2")
        self.assertEqual([item["data"]["steady_ns"] for item in tail], [4, 5])
        _, ordered, _ = self.exported("*.jsonl", "--order", "file", "--limit", "2")
        self.assertEqual([item["data"]["steady_ns"] for item in ordered], [4, 2])

    def test_clock_order_never_implies_cross_domain_alignment(self):
        self.write("input.jsonl", [
            {"event": "untimed"}, {"timestamp_ns": 1}, {"steady_ns": 999},
            {"timestamp": "1970-01-01 00:00:01"}, {"elapsed_ms": 0.5},
        ])
        _, rows, diagnostics = self.exported("input.jsonl")
        self.assertEqual([item["_log"]["clock"] for item in rows],
                         ["steady", "wall", "wall-local", "elapsed:input.jsonl", "none"])
        self.assertIn("do not establish cross-domain order", diagnostics)

    def test_correlation_is_direct_and_never_joins_empty_identities(self):
        self.write("native.jsonl", [
            {"event": "seed", "trace_id": 1, "span_id": 10},
            {"event": "zero-seed", "trace_id": 0},
        ])
        self.write("firefox.log", [
            {"event": "direct", "trace_id": 1, "span_id": 20},
            {"event": "transitive-only", "trace_id": 2, "span_id": 20},
            {"event": "zero", "trace_id": 0},
            {"event": "missing"},
            {"event": "different-type", "trace_id": "1"},
        ])
        _, rows, _ = self.exported(".", "-q", 'event:seed', "--correlate", "trace_id", "--correlate", "span_id")
        self.assertEqual({item["data"]["event"] for item in rows}, {"seed", "zero-seed", "direct"})
        self.assertEqual(sum(item["_log"]["match"] == "correlated" for item in rows), 1)

    def test_composite_identity_keeps_sessions_separate(self):
        self.write("input.jsonl", [
            {"event": "seed", "source_session": 7, "source_instance": 1},
            {"event": "same", "source_session": 7, "source_instance": 1},
            {"event": "other-session", "source_session": 8, "source_instance": 1},
            {"event": "other-instance", "source_session": 7, "source_instance": 2},
        ])
        _, rows, _ = self.exported("input.jsonl", "-q", "event=seed",
                                   "--correlate", "source_session+source_instance")
        self.assertEqual({item["data"]["event"] for item in rows}, {"seed", "same"})

    def test_surface_correlates_native_and_firefox_and_where_scopes_all_rows(self):
        self.write("native.jsonl", [{"event": "native.ready", "surface_high": 1, "surface_low": 2}])
        self.write("firefox.log", [
            {"event": "firefox.ready", "surface": "00000000000000010000000000000002"},
            {"event": "skip.me", "surface": "00000000000000010000000000000002"},
        ])
        _, rows, _ = self.exported(".", "-q", "event=native.ready", "--correlate", "@surface",
                                   "--where", "NOT event=skip.me")
        self.assertEqual({item["data"]["event"] for item in rows}, {"native.ready", "firefox.ready"})

    def test_context_is_bounded_deduplicated_and_obeys_where(self):
        self.write("input.log", "before\nfailed first\nbetween\nfailed second\nafter\nexcluded\n")
        _, rows, _ = self.exported("input.log", "-q", "failed", "--context", "2", "--where", "NOT excluded")
        self.assertEqual([item["_log"]["line"] for item in rows], [1, 2, 3, 4, 5])
        self.assertEqual([item["_log"]["match"] for item in rows],
                         ["context", "query", "context", "query", "context"])

    def test_dense_context_considers_buffered_rows_only_once(self):
        class CountedContext(logs.deque):
            visited = 0

            def __iter__(self):
                type(self).visited += len(self)
                return super().__iter__()

        count = 512
        self.write("input.log", "match\n" * count)
        with patch.object(logs, "deque", CountedContext):
            status, rows, diagnostics = self.exported("input.log", "-q", "match", "--context", "100", "--limit", "1")
        self.assertEqual(status, 0, diagnostics)
        self.assertEqual(len(rows), 1)
        self.assertIn(f"{count} matched", diagnostics)
        self.assertIn("0 context", diagnostics)
        self.assertLessEqual(CountedContext.visited, count)

    def test_projection_preserves_provenance_and_uint64(self):
        self.write("input.jsonl", [{"event": "one", "trace_id": 18446744073709551615}])
        _, rows, _ = self.exported("input.jsonl", "--fields", "event,trace_id,absent")
        self.assertEqual(rows, [{
            "@file": "input.jsonl", "@line": 1, "@match": "query",
            "event": "one", "trace_id": 18446744073709551615, "absent": None,
        }])

    def test_error_filter_strict_mode_and_exit_statuses(self):
        self.write("input.jsonl", [
            {"event": "pixel", "error": 0}, {"event": "worker.failed", "trace_id": 5},
        ])
        status, rows, _ = self.exported("input.jsonl", "--errors")
        self.assertEqual(status, 0)
        self.assertEqual([item["data"]["event"] for item in rows], ["worker.failed"])
        self.assertEqual(self.run_query("input.jsonl", "-q", "absent=5")[0], 1)
        self.assertEqual(self.run_query("input.jsonl", "-q", "event=")[0], 2)
        self.assertEqual(self.run_query("input.jsonl", "--correlate", "trace_id")[0], 2)
        self.write("broken.jsonl", '{"event":"partial')
        status, _, error = self.run_query("broken.jsonl", "--strict")
        self.assertEqual(status, 2)
        self.assertIn("1 malformed/truncated", error)

    def test_summary_groups_are_deterministic_and_cardinality_is_bounded(self):
        self.write("input.jsonl", [{"event": value} for value in ("b", "a", "b", "c")])
        status, output, _ = self.run_query("input.jsonl", "--group-by", "event", "--top", "2")
        self.assertEqual(status, 0)
        self.assertIn("      2  b\n        1  a\n", output)
        with patch.object(logs, "MAX_DISTINCT_KEYS", 2):
            status, _, error = self.run_query("input.jsonl")
        self.assertEqual(status, 2)
        self.assertIn("group count exceeds 2", error)
        with patch.object(logs, "MAX_DISTINCT_KEYS", 2):
            status, _, error = self.run_query("input.jsonl", "-q", "*", "--correlate", "event")
        self.assertEqual(status, 2)
        self.assertIn("correlation seeds exceed 2", error)

    def test_help_documents_grammar_and_wrapper_shell_syntax_is_valid(self):
        output = StringIO()
        with redirect_stdout(output), redirect_stderr(StringIO()), self.assertRaises(SystemExit) as result:
            logs.main(["--help"], root=self.root)
        self.assertEqual(result.exception.code, 0)
        self.assertIn("Grammar", output.getvalue())
        self.assertIn("--correlate", output.getvalue())
        wrapper = Path(logs.__file__).resolve().parent.parent / "mmltk"
        subprocess.run(["bash", "-n", str(wrapper)], check=True, capture_output=True, text=True)

    def family_fixture(self):
        self.write("build/validation/session.jsonl", [
            {"event": "browser.server.started", "steady_ns": 100},
            {"event": "child.signaled", "steady_ns": 200, "value": 139},
        ])
        self.write("build/validation/session-native.log", "native plain\n")
        self.write("build/validation/session-firefox.log", [
            {"event": "firefox.webgpu.blocking_map", "detail": "arc_extract",
             "buffer": "BufferId(21,1)", "strong_count": 2, "settled": False},
        ])
        self.write("build/validation/session.jsonl.history/57-1000000000.jsonl", [
            {"event": "browser.server.started", "steady_ns": 10},
            {"event": "child.signaled", "steady_ns": 20, "value": 143},
        ])
        self.write("build/validation/session-native.log.history/57-1000030000.log", "older native\n")
        self.write("build/validation/session-firefox.log.history/57-1000060000.log", [
            {"event": "firefox.webgpu.blocking_map", "detail": "settled", "buffer": "BufferId(21,1)"},
        ])

    def test_family_and_history_pairing_retains_inference_and_archive_identity(self):
        self.family_fixture()
        status, rows, _ = self.exported("--family", "session", "--run", "57-1000030000")
        self.assertEqual(status, 0)
        self.assertEqual({row["_log"]["run"] for row in rows}, {"build/validation/session@57-1000000000"})
        self.assertEqual({row["_log"]["run_link"] for row in rows}, {"rotation-neighbor-within-10ms"})
        self.assertEqual(len({row["_log"]["file"] for row in rows}), 3)
        self.assertTrue(all("archive_id" in row["_log"] for row in rows))
        status, listing, _ = self.run_query("--family", "session", "--history", "--list-runs")
        self.assertEqual(status, 0)
        self.assertIn("session@57-1000000000", listing)
        self.assertIn("session@current", listing)
        self.assertIn("links are inferred", listing)

    def test_family_discovers_separate_acceptance_and_rotating_mozilla_evidence(self):
        self.family_fixture()
        self.write("build/validation/session-acceptance.jsonl", [{"event": "acceptance.readiness.completed"}])
        self.write("build/validation/session-application.log", "application text\n")
        for role, pid, child in (("main", 12, ""), ("child", 13, ".child-4")):
            for index in range(4):
                self.write(f"build/validation/session-mozilla-{role}.{pid}.log{child}.moz_log.{index}",
                           f"Mozilla module {role} ring {index}\n")
        self.write("build/validation/session-acceptance.jsonl.history/57-1000070000.jsonl",
                   [{"event": "acceptance.shutdown.stalled"}])
        self.write("build/validation/session-application.log.history/57-1000075000.log", "old application\n")
        # The process owning this archive no longer has any current artifact.
        self.write("build/validation/session-mozilla-child.9.log.child-3.moz_log.0.history/57-1000080000.log", "old Mozilla\n")
        status, rows, _ = self.exported("--family", "session")
        self.assertEqual(status, 0)
        self.assertEqual(len({row["_log"]["file"] for row in rows}), 13)
        self.assertEqual({row["_log"]["role"] for row in rows},
                         {"trace", "native", "firefox", "acceptance", "application", "mozilla"})
        status, rows, _ = self.exported("--family", "session", "--run", "57-1000080000")
        self.assertEqual(status, 0)
        self.assertEqual(len({row["_log"]["file"] for row in rows}), 6)
        self.assertEqual({row["_log"]["run"] for row in rows}, {"build/validation/session@57-1000000000"})
        status, rows, _ = self.exported("build/validation")
        self.assertEqual(status, 0)
        self.assertEqual(sum(row["_log"]["role"] == "mozilla" for row in rows), 8)

    def test_family_inventory_groups_selectors_without_capturing_unrelated_entries(self):
        expected = set()
        histories = set()
        for family in ("first", "second", "third"):
            for suffix in (".jsonl", "-application.1.log", "-mozilla-child.13.log.child-4.child-5.moz_log.2"):
                current = self.write(f"build/validation/{family}{suffix}", "")
                expected.add(current)
                extension = ".jsonl" if suffix == ".jsonl" else ".log"
                archive = self.write(f"{current.relative_to(self.root)}.history/57-1000000000{extension}", "")
                expected.add(archive)
                histories.add(archive.parent)
        for index in range(80):
            self.write(f"build/validation/unrelated-{index}.bin", "")
            self.write(f"build/validation/other-mozilla-main.{index}.log.moz_log.0", "")
        options = logs.argument_parser().parse_args([
            "--family", "first", "--family", "second", "--family", "third",
            "--family", "first", "--family", "build/validation/./second.jsonl", "--history", "--triage",
        ])
        # Recognized names remain candidates for a later symlink target, but
        # unrelated content is never captured and nonartifact names are discarded.
        with patch.object(logs, "MAX_DISTINCT_KEYS", 128), \
                patch.object(logs.os, "scandir", wraps=logs.os.scandir) as scanned, \
                patch.object(logs.LogFile, "capture", wraps=logs.LogFile.capture) as captured:
            catalog = logs.ArtifactCatalog(options, self.root)
        self.assertEqual({source.path for source in catalog.files}, expected)
        counts = logs.Counter(Path(call.args[0]).resolve() for call in scanned.call_args_list)
        self.assertEqual(counts, {self.root / "build/validation": 1, **{path: 1 for path in histories}})
        self.assertEqual(captured.call_count, len(expected))
        self.assertEqual(catalog.candidate_count, 86)
        self.assertEqual(sum(len(names) for candidates in catalog.inventory.values() for names in candidates.values()), 86)
        with patch.object(logs, "MAX_DISTINCT_KEYS", 85):
            with self.assertRaisesRegex(logs.QueryError, "candidate inventory exceeds file budget"):
                logs.ArtifactCatalog(options, self.root)

    def test_family_symlink_reuses_earlier_target_candidates_and_checks_containment(self):
        expected = set()
        process = "-mozilla-child.12.log.child-3.moz_log.0"
        for suffix in (".jsonl", "-application.1.log", process):
            current = self.write("build/validation/a-target" + suffix, "")
            extension = ".jsonl" if suffix == ".jsonl" else ".log"
            archive = self.write(f"{current.relative_to(self.root)}.history/57-1000000000{extension}", "")
            expected.update((current, archive))
        parent = self.root / "build/validation"
        alias = parent / ("z-alias" + process)
        alias.symlink_to(parent / ("a-target" + process))
        options = logs.argument_parser().parse_args(["--family", "z-alias", "--history", "--triage"])
        counts = logs.Counter()
        closed = []
        names = {}
        original = logs.os.scandir

        @contextmanager
        def ordered(directory):
            directory = Path(directory).resolve()
            counts[directory] += 1
            try:
                with original(directory) as entries:
                    ordered_entries = sorted(entries, key=lambda entry: entry.name)
                    names[directory] = [entry.name for entry in ordered_entries]
                    yield iter(ordered_entries)
            finally:
                closed.append(directory)

        with patch.object(logs.os, "scandir", ordered), \
                patch.object(logs.LogFile, "capture", wraps=logs.LogFile.capture) as captured:
            catalog = logs.ArtifactCatalog(options, self.root)
        self.assertEqual({source.path for source in catalog.files}, expected)
        self.assertEqual({source.metadata["family"] for source in catalog.files}, {"build/validation/a-target"})
        self.assertEqual(captured.call_count, len(expected))
        self.assertEqual(counts, {parent: 1, **{path.parent: 1 for path in expected if path.parent != parent}})
        self.assertCountEqual(closed, counts.keys())
        self.assertLess(names[parent].index("a-target" + process), names[parent].index(alias.name))
        with tempfile.TemporaryDirectory() as outside:
            external = Path(outside) / ("escaped" + process)
            external.write_text("outside\n")
            alias.unlink()
            alias.symlink_to(external)
            with self.assertRaisesRegex(logs.QueryError, "escapes repository"):
                logs.ArtifactCatalog(options, self.root)

    def test_child_mozilla_archived_only_family_and_direct_role(self):
        archive = "build/validation/retired-mozilla-child.34.log.child-7.moz_log.2.history/57-1000000000.log"
        self.write(archive, "archived child module\n")
        self.write("build/validation/retired.jsonl.history/57-1000000000.jsonl", [{"event": "shutdown.complete"}])
        for arguments in (("--family", "retired", "--history"),
                          ("--family", "retired", "--run", "57-1000000000")):
            status, rows, error = self.exported(*arguments)
            self.assertEqual(status, 0, error)
            self.assertEqual({row["_log"]["role"] for row in rows}, {"trace", "mozilla"})
            self.assertEqual({row["_log"]["family"] for row in rows}, {"build/validation/retired"})
            self.assertEqual({row["_log"]["run"] for row in rows}, {"build/validation/retired@57-1000000000"})
        status, rows, _ = self.exported(archive)
        self.assertEqual(status, 0)
        self.assertEqual(rows[0]["_log"]["role"], "mozilla")
        for name in ("retired-mozilla-main.3.log.moz_log", "retired-mozilla-child.4.log.child-8.moz_log.3",
                     "retired-mozilla-child.5.log.child-8.child-9.moz_log.0"):
            self.assertEqual(logs.MOZILLA_ARTIFACT.fullmatch(name)[1], "retired")
        for name in ("retired-mozilla-child.4.log.moz_log.0", "retired-mozilla-main.3.log.child-8.moz_log.0",
                     "retired-mozilla-child.4.log.child-x.moz_log.0", "retired-mozilla-child.4.log.child-8.moz_log.4",
                     "unrelated.moz_log.0"):
            self.assertIsNone(logs.MOZILLA_ARTIFACT.fullmatch(name))

    def test_mozilla_family_retains_containment_and_discovery_budget(self):
        self.write("build/validation/session.jsonl", [{"event": "native"}])
        self.write("build/validation/session-mozilla-main.1.log.moz_log.0", "Mozilla\n")
        with patch.object(logs, "MAX_DISTINCT_KEYS", 6):
            status, _, error = self.run_query("--family", "session")
        self.assertEqual(status, 2)
        self.assertIn("file budget", error)
        with tempfile.TemporaryDirectory() as outside:
            target = Path(outside) / "escaped.log"
            target.write_text("outside\n")
            (self.root / "build/validation/session-mozilla-child.2.log.child-6.moz_log.0").symlink_to(target)
            status, _, error = self.run_query("--family", "session")
            self.assertEqual(status, 2)
            self.assertIn("escapes repository", error)

    def test_history_catalog_enumerates_each_archive_directory_once(self):
        for suffix in (".jsonl", "-native.log", "-firefox.log"):
            current = self.write("build/validation/capture" + suffix, "")
            for index in range(12):
                self.write(f"{current.relative_to(self.root)}.history/57-{index * 20000000}{current.suffix}", "")
        counts = logs.Counter()
        original = logs.directory_logs

        def counted(directory, *arguments):
            counts[directory] += 1
            return original(directory, *arguments)

        options = logs.argument_parser().parse_args([
            "build/validation/capture.jsonl.history/*.jsonl", "--history", "--triage",
        ])
        with patch.object(logs, "directory_logs", counted), \
                patch.object(logs.os, "scandir", wraps=logs.os.scandir) as scanned, \
                patch.object(logs.LogFile, "capture", wraps=logs.LogFile.capture) as captured:
            catalog = logs.ArtifactCatalog(options, self.root)
        self.assertEqual(len(catalog.files), 36)
        self.assertEqual(len(counts), 3)
        self.assertEqual(set(counts.values()), {1})
        physical = logs.Counter(Path(call.args[0]).resolve() for call in scanned.call_args_list)
        # The explicit wildcard enumerates its directory separately from full
        # history expansion; the family parent and other histories each scan once.
        self.assertEqual(physical, {
            self.root / "build/validation": 1,
            self.root / "build/validation/capture.jsonl.history": 2,
            self.root / "build/validation/capture-native.log.history": 1,
            self.root / "build/validation/capture-firefox.log.history": 1,
        })
        self.assertEqual(captured.call_count, 36)
        for direct in ("build/validation/capture.jsonl.history",
                       "/host/repo/build/validation/capture.jsonl.history"):
            options = logs.argument_parser().parse_args([direct, "--history", "--triage"])
            with patch.dict(logs.os.environ, {"MMLTK_LOG_HOST_ROOT": "/host/repo"}), \
                    patch.object(logs.os, "scandir", wraps=logs.os.scandir) as scanned, \
                    patch.object(logs.LogFile, "capture", wraps=logs.LogFile.capture) as captured:
                direct_catalog = logs.ArtifactCatalog(options, self.root)
            self.assertEqual({source.path for source in direct_catalog.files},
                             {source.path for source in catalog.files})
            physical = logs.Counter(Path(call.args[0]).resolve() for call in scanned.call_args_list)
            self.assertEqual(set(physical.values()), {1})
            self.assertEqual(len(physical), 4)
            self.assertEqual(captured.call_count, 36)

    def test_shared_event_anchors_link_transcript_and_propagate_test_tags(self):
        self.family_fixture()
        for name in ("transcript.log", "session-acceptance.log"):
            with self.subTest(name=name):
                path = "build/validation/" + name
                self.write(path,
                    "Filters: [viewer_copy][hardware]\n"
                    "[ RUN ] workspace_wayland_viewer_copy\n"
                    'workspace-wayland[native]: {"event":"browser.server.started","steady_ns":100}\n'
                    '{"event":"firefox.webgpu.blocking_map","buffer":"BufferId(21,1)","detail":"arc_extract"}\n'
                    "native host terminal status: exit 139\n"
                    "[ FAILED ] workspace_wayland_viewer_copy\n")
                status, rows, _ = self.exported(path, "--family", "session",
                                               "-q", '@tags:viewer_copy', "--limit", "30")
                self.assertEqual(status, 0)
                related = [row for row in rows if row["_log"]["file"].endswith("session.jsonl")]
                self.assertEqual(len(related), 2)
                self.assertTrue(all(row["_log"]["test"] == "workspace_wayland_viewer_copy" for row in related))
                terminal = next(row for row in rows if row["data"].get("event") == "process.terminal")
                self.assertEqual(terminal["_log"]["run"], "build/validation/session@current")
                self.assertEqual(terminal["_log"]["run_link"], "shared-event-and-steady-time")

    def test_related_run_includes_terminal_and_never_crosses_reused_buffer_ids(self):
        self.family_fixture()
        status, rows, _ = self.exported("--family", "session", "--history", "-q",
                                       'detail=arc_extract', "--correlate", "buffer",
                                       "--related-run", "--limit", "30")
        self.assertEqual(status, 0)
        self.assertEqual({row["_log"]["run"] for row in rows}, {"build/validation/session@current"})
        self.assertIn("child.signaled", {row["data"].get("event") for row in rows})
        status, output, _ = self.run_query("--family", "session", "-q", 'buffer="BufferId(21,1)"', "--limit", "1")
        self.assertEqual(status, 0)
        self.assertIn("Terminals in matched runs", output)
        self.assertIn("@signal=SIGSEGV", output)
        self.assertIn("@exit_code=139", output)

    def test_transcript_incident_keeps_buffer_ids_failure_and_catch_context(self):
        self.write("incident.log",
            "Filters: [viewer_copy]\n"
            "[ RUN ] workspace_wayland_viewer_copy\n"
            '{"event":"firefox.webgpu.blocking_map","detail":"arc_extract","buffer":"BufferId(21,1)","strong_count":2,"settled":false}\n'
            '/workspace/test.cpp:4181: failed: cleanup_count >= 2 with 2 messages: '
            "'termination: sigint' and 'native diagnostics: eration\":0,\"trace_id\":721\n"
            '{"event":"child.signaled","steady_ns":131858036752003,"value":139}\n'
            "' and 'Firefox diagnostics: {\"event\":\"firefox.webgpu.blocking_map\",\"buffer\":\"BufferId(21,1)\",\"detail\":\"callback\"}'\n"
            "[ FAILED ] workspace_wayland_viewer_copy\n")
        status, output, diagnostics = self.run_query("incident.log", "-q", "viewer_copy", "--limit", "20")
        self.assertEqual(status, 0)
        self.assertIn("strong_count", self.run_query("incident.log", "-q", 'buffer="BufferId(21,1)"', "--format", "jsonl")[1])
        self.assertIn("catch.assertion_failed", output)
        self.assertIn("@signal=SIGSEGV", output)
        self.assertIn("context-copy", output)
        self.assertNotIn("Traceback", diagnostics)

    def pasted_error_fixture(self):
        message = "Presentation unavailable: Explore requires a measured non-empty gallery."
        for suffix in (".jsonl", "-native.log", "-firefox.log"):
            self.write("build/validation/presentation" + suffix, "")
        native = self.write("build/validation/presentation.jsonl.history/57-2000000000.jsonl", [
            {"event": "browser.server.started", "steady_ns": 1000000000},
            {"event": "browser.intent.accepted", "steady_ns": 1001000000, "sequence": 107},
            {"event": "browser.reply.sent", "steady_ns": 1002000000, "sequence": 107},
            {"event": "child.signaled", "steady_ns": 1021000000, "value": 143},
        ])
        runtime = self.write("build/validation/presentation-native.log.history/57-2000030000.log",
            "1970-01-01 00:00:01.010 [1:2] [host] [debug] event=explore.completed\n")
        firefox = self.write("build/validation/presentation-firefox.log.history/57-2000060000.log", [
            {"event": "integration.explore_message", "detail": "Gallery(Measured { width: 896 })", "elapsed_ms": 19320},
            {"event": "integration.explore_message", "detail": "Detail(CloseRequested)", "elapsed_ms": 19435},
            {"event": "integration.explore_message", "detail": "Dataset(OpenRequested)", "elapsed_ms": 19478},
            {"event": "integration.failed", "detail": message, "elapsed_ms": 19479},
            {"event": "integration.surface_sync", "elapsed_ms": 19500},
        ])
        for path in (native, runtime, firefox):
            os.utime(path, ns=(1700000019500000000, 1700000019500000000))
        later = self.write("build/validation/presentation-firefox.log.history/57-3000060000.log", [
            {"event": "integration.failed", "detail": message, "elapsed_ms": 20557},
        ])
        os.utime(later, ns=(1700000099500000000, 1700000099500000000))
        self.write("build/validation/acceptance.log",
            "Filters: [viewer_copy]\n[ RUN ] workspace_wayland_viewer_copy\n"
            'workspace-wayland[native]: {"event":"browser.server.started","steady_ns":1000000000}\n'
            "workspace-wayland[firefox]: " + json.dumps({"event": "integration.failed", "detail": message, "elapsed_ms": 19479}) + "\n"
            "[ FAILED ] workspace_wayland_viewer_copy\n")
        return "Presentation unavailable\n\nExplore requires a measured non-empty gallery."

    def test_pasted_multiline_error_finds_earliest_physical_record_and_nearby_family(self):
        pasted = self.pasted_error_fixture()
        status, output, diagnostics = self.run_query("--error", pasted)
        self.assertEqual(status, 0, diagnostics)
        self.assertIn("exact/display-normalized phrase", output)
        self.assertIn("3 occurrences", output)
        first = next(line for line in output.splitlines() if line.startswith("First physical match:"))
        self.assertIn("57-2000060000.log:4", first)
        self.assertIn("19479.000ms", first)
        for expected in ("Gallery(Measured", "Detail(CloseRequested)", "Dataset(OpenRequested)",
                         "integration.failed", "browser.intent.accepted", "browser.reply.sent", "SIGTERM"):
            self.assertIn(expected, output)
        self.assertIn("file-mtime proximity", output)
        self.assertIn("same-clock proximity", output)
        self.assertIn("proximity is not causality", diagnostics)
        self.assertNotIn("57-3000060000.log", output)

    def test_pasted_error_uses_phrase_before_unordered_fallback(self):
        self.write("build/validation/example.log",
            "gallery empty because unavailable presentation\n"
            "presentation unavailable empty gallery\n")
        status, output, _ = self.run_query("--error", "presentation unavailable empty gallery")
        self.assertEqual(status, 0)
        self.assertIn("exact/display-normalized phrase; 1 occurrences", output)
        self.assertIn("example.log:2", output.split("First physical match:", 1)[1].splitlines()[0])
        status, output, _ = self.run_query("--error", "unavailable empty presentation gallery")
        self.assertEqual(status, 0)
        self.assertIn("all-words fallback; 2 occurrences", output)

    def test_pasted_error_selection_scans_once_with_exact_fallback_and_unicode(self):
        self.write("input.jsonl",
                   '{"message":"blue red"}\n{"message":"red blue"}\n'
                   '{"message":"\\u0072ed \\u0062lue"}\n{"broken":\n')
        files = logs.discover_files(["input.jsonl"], self.root)
        query = logs.Expression("all", ())
        original = logs.LogFile.records
        for strict in (False, True):
            options = logs.argument_parser().parse_args(["--strict"] if strict else [])
            for text, mode, count, line in (
                ("red blue", "exact/display-normalized phrase", 2, 2),
                ("red, blue", "all-words fallback", 3, 1),
                ("unseen message", "", 0, None),
                ("!!!", "", 0, None),
            ):
                with self.subTest(strict=strict, text=text):
                    lookup = logs.ErrorLookup(text, options)
                    with patch.object(logs.LogFile, "records", autospec=True, side_effect=original) as scans, \
                            patch.object(logs, "record_text", wraps=logs.record_text) as normalized:
                        lookup.locate(files, query, query, {})
                    self.assertEqual(scans.call_count, 1)
                    self.assertLessEqual(normalized.call_count, 4)
                    self.assertEqual((lookup.mode, lookup.matches), (mode, count))
                    self.assertEqual(lookup.focus.line if lookup.focus else None, line)
                    self.assertEqual(lookup.malformed, int(strict or text == "!!!"))

    def test_error_neighborhood_respects_radius_and_machine_output_bounds(self):
        pasted = self.pasted_error_fixture()
        status, rows, _ = self.exported("--error", pasted, "--near-ms", "50", "--limit", "5")
        self.assertEqual(status, 0)
        self.assertLessEqual(len(rows), 5)
        self.assertTrue(any(row["data"].get("event") == "integration.failed" for row in rows))
        self.assertFalse(any("Gallery(Measured" in row["data"].get("detail", "") for row in rows))
        self.assertTrue(all("proximity_ns" in row["_log"] for row in rows))

    def test_error_lookup_reports_absence_and_rejects_empty_text(self):
        self.write("build/validation/example.log", "ordinary success\n")
        self.assertEqual(self.run_query("--error", "not present anywhere")[0], 1)
        self.assertEqual(self.run_query("--error", " \n ")[0], 2)
        self.write("build/validation/broken.jsonl", '{"event":')
        self.assertEqual(self.run_query("--error", "not present anywhere", "--strict")[0], 2)

    def test_triage_identity_namespaces_exclude_ambient_empty_and_bare_counters(self):
        row = record({
            "surface_high": 0, "surface_low": 0, "trace_id": 0, "span_id": True,
            "device_id": "DeviceId(0,1)", "pid": 20, "thread_id": 3,
            "source_session": 2, "source_instance": 4, "sequence": 18, "generation": 1,
            "texture_id": "TextureId(2, 1)", "buffer": "BufferId(21,1)", "request_id": "abc",
            "parent_span_id": 52, "user_id": "user", "empty_id": "",
        })
        identities = logs.strong_identities(row)
        labels = {item.label() for item in identities}
        self.assertEqual(labels, {
            "source_session+source_instance=2+4", "TextureId=TextureId(2,1)",
            "BufferId=BufferId(21,1)", "request_id=abc", "span_id=52",
        })
        explicit = logs.strong_identities(row, (("device_id",),))
        self.assertTrue(any(item.names == ("device_id",) for item in explicit))
        string = record({"request_id": "1"})
        number = record({"request_id": 1})
        self.assertNotEqual(logs.strong_identities(string), logs.strong_identities(number))

    def triage_fixture(self):
        surface = "00000000000000010000000000000002"
        self.write("build/validation/triage.jsonl", [
            {"event": "browser.server.started", "steady_ns": 100},
            {"event": "task.requested", "surface_high": 1, "surface_low": 2, "steady_ns": 200,
             "trace_id": 55, "generation": 9, "slot": 0},
            {"event": "task.started", "trace_id": 55, "span_id": 56, "steady_ns": 220},
            {"event": "scheduler.dispatch", "generation": 9, "slot": 0, "sequence": 7, "steady_ns": 230},
            {"event": "worker.completed", "parent_span_id": 56, "steady_ns": 240},
            {"event": "task.completed", "trace_id": 55, "span_id": 56, "steady_ns": 250},
            {"event": "upload.started", "surface_high": 1, "surface_low": 2, "steady_ns": 260},
            {"event": "child.signaled", "value": 139, "steady_ns": 300},
        ])
        self.write("build/validation/triage-firefox.log", [
            {"event": "ui.measure", "elapsed_ms": 90},
            {"event": "renderer.claim_requested", "surface": surface},
            {"event": "renderer.claim_outcome", "surface": surface, "outcome": "claimed"},
            {"event": "renderer.probe_failed", "surface": surface, "boundary": "Allocation"},
            {"event": "renderer.draw_missing", "surface": surface, "generation": 9, "slot": 0},
            {"event": "renderer.ready", "surface": surface},
            {"event": "renderer.retired", "surface": surface},
        ])
        self.write("build/validation/triage-acceptance.log",
            "Filters: [pixel_probe]\n[ RUN ] workspace_pixel_probe\n"
            'workspace-wayland[native]: {"event":"browser.server.started","steady_ns":100}\n'
            "/workspace/test.cpp:44: failed: rendered probe with 1 message: 'native failure'\n"
            "[ FAILED ] workspace_pixel_probe\n")
        return surface

    def test_triage_automatically_correlates_probe_surface_catch_native_and_counter_leaf(self):
        surface = self.triage_fixture()
        args = ("build/validation/triage-acceptance.log", "--family", "triage", "--triage", "--limit", "30")
        status, output, diagnostics = self.run_query(*args)
        self.assertEqual(status, 0, diagnostics)
        for text in ("renderer.probe_failed", "catch.assertion_failed", "SIGSEGV", surface,
                     "upload", "missing-counterpart", "trace_id=55", "span_id=56",
                     "counter refinement", "scheduler.dispatch", "Earliest divergence"):
            self.assertIn(text, output)
        self.assertIn("not a root-cause conclusion", output)
        self.assertIn("proximity does not establish causality", diagnostics)
        status, rows, _ = self.exported(*args, "--limit", "8")
        self.assertEqual(status, 0)
        self.assertLessEqual(len(rows), 8)
        self.assertTrue(all(row["_log"].get("triage_reason") for row in rows))
        self.assertTrue(any(row["data"].get("event") == "renderer.probe_failed" for row in rows))
        self.assertTrue(any(row["data"].get("event") == "catch.assertion_failed" for row in rows))
        self.assertEqual({row["_log"]["run"] for row in rows}, {"build/validation/triage@current"})

    def test_triage_single_file_discovers_current_siblings_without_expanding_explicit_queries(self):
        self.write("build/validation/capture-firefox.log", [
            {"event": "renderer.probe_failed", "request_id": 7},
        ])
        self.write("build/validation/capture.jsonl", [
            {"event": "task.started", "request_id": 7},
            {"event": "child.signaled", "value": 139},
        ])
        self.write("build/validation/capture-native.log", [
            {"event": "acceptance.failed", "request_id": 7},
        ])
        path = "build/validation/capture-firefox.log"
        status, rows, diagnostics = self.exported(path, "--triage")
        self.assertEqual(status, 0, diagnostics)
        events = {row["data"].get("event"): row for row in rows}
        self.assertTrue({"renderer.probe_failed", "task.started", "child.signaled", "acceptance.failed"} <= events.keys())
        self.assertEqual(events["task.started"]["_log"]["discovery"], "current artifact sibling")
        self.assertNotIn("discovery", events["renderer.probe_failed"]["_log"])
        self.assertIn("[auto: current artifact sibling]", diagnostics)
        _, explicit, _ = self.exported(path)
        self.assertEqual([row["data"]["event"] for row in explicit], ["renderer.probe_failed"])
        with patch.object(logs, "MAX_DISTINCT_KEYS", 1):
            status, _, diagnostics = self.run_query(path, "--triage")
            self.assertEqual(status, 2)
            self.assertIn("automatic artifact discovery exceeds file budget", diagnostics)

    def test_triage_single_archive_discovers_only_its_rotation_batch_without_current_files(self):
        for base, suffix, timestamp, event in (
            ("capture", ".jsonl", 1000000, "task.started"),
            ("capture-native", ".log", 1000100, "acceptance.failed"),
            ("capture-firefox", ".log", 1000200, "renderer.probe_failed"),
            ("capture", ".jsonl", 1000300, "unrelated.failed"),
            ("capture-native", ".log", 1000400, "unrelated.failed"),
            ("capture-firefox", ".log", 1000500, "unrelated.failed"),
        ):
            self.write(f"build/validation/{base}{suffix}.history/57-{timestamp}{suffix}", [
                {"event": event, "request_id": 7, **({"failed": True} if event == "unrelated.failed" else {})},
            ])
        path = "build/validation/capture-firefox.log.history/57-1000200.log"
        status, rows, diagnostics = self.exported(path, "--triage")
        self.assertEqual(status, 0, diagnostics)
        self.assertEqual({row["_log"]["run"] for row in rows}, {"build/validation/capture@57-1000000"})
        self.assertEqual({row["data"]["event"] for row in rows},
                         {"task.started", "acceptance.failed", "renderer.probe_failed"})
        self.assertIn("[auto: adjacent rotation (inferred)]", diagnostics)
        self.assertIn("[rotation-neighbor-within-10ms]", diagnostics)
        self.assertNotIn("1000300", diagnostics)
        self.write("build/validation/capture.jsonl", [{"event": "current.failed", "failed": True}])
        self.assertEqual(self.exported(path, "--triage")[1], rows)

    def test_triage_named_archives_match_exact_names_and_reject_external_history(self):
        path = "build/validation/capture-firefox.log.history/investigation.log"
        self.write(path, [{"event": "renderer.failed", "request_id": 7}])
        self.write("build/validation/capture.jsonl.history/investigation.jsonl", [
            {"event": "task.started", "request_id": 7},
        ])
        self.write("build/validation/capture.jsonl.history/other.jsonl", [
            {"event": "unrelated.failed", "failed": True},
        ])
        status, rows, diagnostics = self.exported(path, "--triage")
        self.assertEqual(status, 0, diagnostics)
        self.assertEqual({row["data"]["event"] for row in rows}, {"renderer.failed", "task.started"})
        self.assertIn("[auto: matching archive name]", diagnostics)
        with tempfile.TemporaryDirectory() as external:
            (self.root / "build/validation/capture-native.log.history").symlink_to(external, target_is_directory=True)
            status, _, diagnostics = self.run_query(path, "--triage")
            self.assertEqual(status, 2)
            self.assertIn("escapes repository mount", diagnostics)

    def test_triage_ranks_explicit_failure_above_error_related_events_without_changing_errors_filter(self):
        self.write("input.jsonl", [
            {"event": "ui.error_modal", "trace_id": 7},
            {"event": "worker.failed"},
        ])
        status, output, diagnostics = self.run_query("input.jsonl", "--triage", "--triage-anchors", "1")
        self.assertEqual(status, 0, diagnostics)
        anchors = output.split("Anchors (ranked evidence, not causes):", 1)[1].split("Highlights:", 1)[0]
        self.assertIn("worker.failed", anchors)
        self.assertNotIn("ui.error_modal", anchors)
        _, rows, _ = self.exported("input.jsonl", "--errors")
        self.assertEqual({row["data"]["event"] for row in rows}, {"ui.error_modal", "worker.failed"})
        _, output, _ = self.run_query("input.jsonl", "--triage", "-q", "event=ui.error_modal",
                                      "--where", "event=ui.error_modal")
        self.assertIn("failure-related event (not an explicit failure)", output)
        self.assertNotIn("anchor-failure", output)

    def test_triage_pasted_error_retains_earliest_physical_and_timestamp_neighbors(self):
        pasted = self.pasted_error_fixture()
        status, output, diagnostics = self.run_query("--error", pasted, "--triage")
        self.assertEqual(status, 0, diagnostics)
        self.assertIn("exact/display-normalized phrase", output)
        first = next(line for line in output.splitlines() if line.startswith("First physical match:"))
        self.assertIn("57-2000060000.log:4", first)
        self.assertIn("19479.000ms", first)
        for text in ("Gallery(Measured", "Detail(CloseRequested)", "Dataset(OpenRequested)",
                     "browser.intent.accepted", "browser.reply.sent", "SIGTERM"):
            self.assertIn(text, output)
        self.assertIn("No non-empty strong identity", diagnostics)
        self.assertNotIn("57-3000060000.log", output)
        self.assertIn("file-mtime proximity", output)

    def test_triage_pasted_error_context_survives_noisy_historical_identity_chain(self):
        pasted = self.pasted_error_fixture()
        path = self.root / "build/validation/presentation-firefox.log.history/57-2000060000.log"
        original = path.read_text(encoding="utf-8")
        old = [
            {"event": "render.failed", "dataset_identity": 7, "surface": f"{index + 1:032x}",
             "elapsed_ms": 500 + index}
            for index in range(100)
        ]
        path.write_text("".join(json.dumps(row) + "\n" for row in old) + original +
                        '{"event":"render.presented","dataset_identity":7,"elapsed_ms":19480}\n',
                        encoding="utf-8")
        os.utime(path, ns=(1700000019500000000, 1700000019500000000))
        status, output, diagnostics = self.run_query("--error", pasted, "--triage", "--limit", "12")
        self.assertEqual(status, 0, diagnostics)
        for event in ("Gallery(Measured", "Detail(CloseRequested)", "Dataset(OpenRequested)", "integration.failed"):
            self.assertIn(event, output)
        self.assertIn("anchor-failure", output)

    def test_triage_recognizes_generic_terminal_suffix_without_guessing_numeric_outcome(self):
        self.write("input.jsonl", [
            {"event": "new.subsystem.terminal", "sequence": 2, "outcome": 4},
        ])
        status, output, _ = self.run_query("input.jsonl", "--triage")
        self.assertEqual(status, 0)
        self.assertIn("terminal evidence (not necessarily failure)", output)
        self.assertIn("outcome=4", output)
        self.assertNotIn("anchor-failure", output)
        self.assertIn("not a process/test exit", output)

    def test_triage_terminal_summary_survives_failure_volume_and_preserves_unknown_fields(self):
        self.write("input.jsonl", [
            {"event": "new.stage.terminal", "sequence": 2, "packed_bits": 4096, "outcome": 0},
            *({"event": f"failure.{index}.failed", "request_id": index + 1} for index in range(30)),
        ])
        status, output, _ = self.run_query("input.jsonl", "--triage", "--limit", "1")
        self.assertEqual(status, 0)
        self.assertIn("new.stage.terminal", output)
        self.assertIn("packed_bits=4096", output)
        self.assertIn("stage terminals need not mean run completion", output)
        self.write("status.jsonl", [{"event": "opaque", "terminal": True, "status": "failed"}])
        self.assertIn("anchor-failure", self.run_query("status.jsonl", "--triage")[1])

    def test_triage_untrusted_field_names_cannot_escape_size_or_terminal_output_bounds(self):
        hostile = {"event": "operation.terminal", "evil\u001b[31m_id": "data", "x" * 10000: "payload"}
        self.write("input.jsonl", [hostile])
        status, output, _ = self.run_query("input.jsonl", "--triage")
        self.assertEqual(status, 0)
        self.assertNotIn("\u001b", output)
        self.assertFalse(logs.strong_identities(record(hostile)))
        _, rows, _ = self.exported("input.jsonl", "--triage")
        self.assertTrue(rows[0]["_log"]["triage_payload_truncated"])
        self.assertTrue(all(len(key) <= logs.MAX_TRIAGE_TEXT + 1 for key in rows[0]["data"]))

    def test_triage_handles_unmatched_start_duplicate_end_and_failed_span(self):
        self.write("input.jsonl", [
            {"event": "work.started", "span_id": 1, "steady_ns": 10},
            {"event": "work.completed", "span_id": 1, "steady_ns": 20, "span_outcome": "failed"},
            {"event": "work.completed", "span_id": 1, "steady_ns": 30},
            {"event": "read.started", "span_id": 1, "steady_ns": 40},
        ])
        status, output, _ = self.run_query("input.jsonl", "--triage", "--top", "20")
        self.assertEqual(status, 0)
        self.assertIn("failed-span", output)
        self.assertIn("duplicate-terminal", output)
        self.assertIn("missing-counterpart", output)
        self.assertIn("read: 1 start(s)", output)
        self.assertNotIn("work: 1 start(s)", output)

    def test_triage_request_submit_start_progress_is_not_three_unfinished_operations(self):
        self.write("input.jsonl", [
            {"event": "job.requested", "request_id": 1},
            {"event": "job.submitted", "request_id": 1},
            {"event": "job.started", "request_id": 1},
            {"event": "job.completed", "request_id": 1},
        ])
        status, output, _ = self.run_query("input.jsonl", "--triage", "-q", "request_id=1")
        self.assertEqual(status, 0)
        self.assertNotIn("missing-counterpart", output)
        self.assertNotIn("duplicate-terminal", output)
        status, output, _ = self.run_query("input.jsonl", "--triage")
        self.assertEqual(status, 1)
        self.assertNotIn("unclosed start", output)

    def test_triage_secondary_handle_and_identity_handoff_do_not_fake_missing_completion(self):
        self.write("input.jsonl", [
            {"event": "claim.requested", "surface_high": 1, "surface_low": 2,
             "texture_id": "TextureId(3,1)"},
            {"event": "claim.outcome", "surface_high": 1, "surface_low": 2},
            {"event": "job.requested", "request_id": 1, "span_id": 2},
            {"event": "job.completed", "request_id": 1},
            {"event": "probe.failed", "surface_high": 1, "surface_low": 2, "request_id": 1},
        ])
        status, output, diagnostics = self.run_query("input.jsonl", "--triage", "--top", "20")
        self.assertEqual(status, 0, diagnostics)
        self.assertNotIn("missing-counterpart", output)
        self.assertIn("handoff/instrumentation mismatch possible", output)
        self.assertIn("source-order input.jsonl:", output)

    def test_triage_unrelated_completion_is_not_an_identity_handoff(self):
        cases = (
            [
                {"event": "job.started", "request_id": 1},
                {"event": "job.completed", "request_id": 2},
            ],
            [
                {"event": "job.started", "span_id": 1, "trace_id": 7},
                {"event": "job.completed", "span_id": 2, "trace_id": 7},
            ],
            [
                {"event": "job.started", "request_id": 1},
                {"event": "job.started", "request_id": 1},
                {"event": "job.completed", "request_id": 1},
            ],
        )
        for rows in cases:
            with self.subTest(rows=rows):
                self.write("input.jsonl", rows)
                status, output, diagnostics = self.run_query("input.jsonl", "--triage", "-q", "*", "--top", "20")
                self.assertEqual(status, 0, diagnostics)
                self.assertIn("missing-counterpart", output)
                self.assertIn("job: 1 start(s)", output)
                self.assertNotIn("identity-handoff", output)

    def test_triage_prefers_explicit_divergence_over_earlier_unclosed_start(self):
        self.write("input.jsonl", [
            {"event": "background.started", "request_id": 1},
            {"event": "probe.missing", "request_id": 1},
            {"event": "probe.failed", "request_id": 1},
        ])
        status, output, _ = self.run_query("input.jsonl", "--triage")
        self.assertEqual(status, 0)
        candidate = output.split("Earliest divergence candidates", 1)[1].splitlines()[1]
        self.assertIn("input.jsonl:2: incomplete-state", candidate)

    @staticmethod
    def pixel_bridge_records():
        common = {"surface": "00000000000000010000000000000002",
                  "presentation_revision": 7, "transfer_sequence": 9}
        pixel = {"event": "firefox.workspace.pixel", "boundary": "import", **common,
                 "sample_index": 0, "sample_x": 4, "sample_y": 5, "sample_rgba": 10}
        bridge = {"event": "firefox.workspace.frame_forwarded", **common,
                  "source": "00000000000000030000000000000004"}
        return pixel, bridge

    def test_triage_missing_pixel_bridge_is_a_visible_anchor_and_finding(self):
        pixel, _ = self.pixel_bridge_records()
        self.write("input.jsonl", [pixel])
        for query in ((), ("-q", "@event=firefox.workspace.pixel")):
            status, output, diagnostics = self.run_query("input.jsonl", "--triage", *query)
            self.assertEqual(status, 0, diagnostics)
            self.assertIn("pixel-transfer-missing", output)
            self.assertIn("presentation_revision=7 transfer_sequence=9", output)
            self.assertIn("Highlights:", output)
        status, rows, diagnostics = self.exported("input.jsonl", "--triage")
        self.assertEqual(status, 0, diagnostics)
        pending, = rows
        self.assertEqual(pending["data"], pixel)
        self.assertIn("pixel-transfer-missing", pending["_log"]["match"])
        self.assertNotIn("source", pending["data"])

    def test_triage_explicit_pixel_query_does_not_exclude_its_later_bridge(self):
        pixel, bridge = self.pixel_bridge_records()
        for records in ([pixel, bridge], [bridge, pixel]):
            with self.subTest(records=records):
                self.write("input.jsonl", records)
                status, output, diagnostics = self.run_query(
                    "input.jsonl", "--triage", "-q", "@event=firefox.workspace.pixel")
                self.assertEqual(status, 0, diagnostics)
                self.assertNotIn("pixel-transfer-missing", output)
                self.assertNotIn("pixel-transfer-conflict", output)
                status, output, diagnostics = self.run_query("input.jsonl", "--triage")
                self.assertEqual(status, 1, diagnostics)
                self.assertNotIn("pixel-transfer-missing", output)

    def test_triage_pixel_query_keeps_conflicting_bridge_evidence(self):
        pixel, bridge = self.pixel_bridge_records()
        conflict = {**bridge, "source": "00000000000000050000000000000006"}
        self.write("input.jsonl", [pixel, bridge, conflict])
        status, output, diagnostics = self.run_query(
            "input.jsonl", "--triage", "-q", "@event=firefox.workspace.pixel")
        self.assertEqual(status, 0, diagnostics)
        self.assertIn("pixel-transfer-conflict", output)
        self.assertNotIn("pixel-transfer-missing", output)

    def test_triage_highlights_first_deterministic_pixel_chain_divergence(self):
        surface = "00000000000000010000000000000002"
        common = {
            "presentation_revision": 7, "transfer_sequence": 3,
            "source": "00000000000000030000000000000004",
            "sample_index": 4, "sample_x": 8, "sample_y": 9,
        }
        self.write("input.jsonl", [
            {"event": "firefox.workspace.frame_forwarded", "surface": surface, **common},
            {"event": "presentation.pixel", "surface_high": 1, "surface_low": 2,
             **common, "sample_rgba": 10},
            {"event": "firefox.workspace.pixel", "surface": surface, "boundary": "import",
             **common, "sample_rgba": 11},
            {"event": "firefox.workspace.pixel", "surface": surface, "boundary": "mailbox",
             **common, "sample_rgba": 11},
            {"event": "iced.surface.pixel", "surface": surface,
             **common, "sample_rgba": 11},
        ])
        status, output, diagnostics = self.run_query("input.jsonl", "--triage", "--top", "20")
        self.assertEqual(status, 0, diagnostics)
        self.assertIn("pixel-chain-divergence", output)
        self.assertIn("native -> import sample differs", output)
        self.assertIn("presentation_revision=7 sample_index=4", output)

        self.write("input.jsonl", [
            {"event": "firefox.workspace.frame_forwarded", "surface": surface, **common},
            {"event": "presentation.pixel", "surface_high": 1, "surface_low": 2,
             **common, "sample_rgba": 10},
            {"event": "firefox.workspace.pixel", "surface": surface, "boundary": "import",
             **common, "sample_rgba": 10},
            {"event": "firefox.workspace.pixel", "surface": surface, "boundary": "mailbox",
             **common, "sample_rgba": 10},
            {"event": "iced.surface.pixel", "surface": surface,
             **common, "sample_rgba": 10},
        ])
        status, output, diagnostics = self.run_query("input.jsonl", "--triage")
        self.assertEqual(status, 1, diagnostics)
        self.assertNotIn("pixel-chain-divergence", output)

    def test_triage_groups_repeated_anomalies_without_losing_counts_or_context(self):
        self.write("input.jsonl", [
            *({"event": "draw.missing", "request_id": 1} for _ in range(100)),
            {"event": "probe.failed", "request_id": 1},
        ])
        status, output, _ = self.run_query("input.jsonl", "--triage")
        self.assertEqual(status, 0)
        highlights = output.split("Highlights:", 1)[1].split("Earliest divergence", 1)[0]
        self.assertEqual(highlights.count("incomplete-state"), 1)
        self.assertIn("100 observations", highlights)

    def test_triage_first_catch_info_context_recognizes_singular_plural_and_inline_json(self):
        for count in ("1 message", "2 messages"):
            with self.subTest(count=count):
                self.write("context.log",
                    "[ RUN ] copying_test\n"
                    f"/workspace/test.cpp:4: failed: false with {count}: 'native diagnostics: "
                    '{"event":"job.completed","span_id":7}\n'
                    '{"event":"job.completed","span_id":7}\n')
                status, output, _ = self.run_query("context.log", "--triage")
                self.assertEqual(status, 0)
                self.assertIn("copies=2", output)
                self.assertNotIn("duplicate-terminal", output)

    def test_triage_zero_hops_still_includes_bounded_counter_refinement(self):
        self.write("input.jsonl", [
            {"event": "probe.failed", "request_id": 1, "generation": 4, "slot": 0, "steady_ns": 10},
            *({"event": "noise"} for _ in range(5)),
            {"event": "counter.evidence", "generation": 4, "slot": 0, "steady_ns": 20},
            {"event": "wrong.generation", "generation": 5, "slot": 0, "steady_ns": 21},
            {"event": "wrong.type", "generation": "4", "slot": 0, "steady_ns": 22},
            {"event": "bare.sequence", "sequence": 4, "steady_ns": 23},
        ])
        _, rows, _ = self.exported("input.jsonl", "--triage", "--triage-hops", "0", "--near-ms", "0")
        by_event = {row["data"].get("event"): row for row in rows}
        self.assertIn("counter refinement", by_event["counter.evidence"]["_log"]["match"])
        for event in ("wrong.generation", "wrong.type", "bare.sequence"):
            self.assertNotIn(event, by_event)

    def test_triage_incomplete_end_is_an_anchor_without_claiming_failure(self):
        self.write("input.jsonl", [{"event": "upload.started", "request_id": 7}])
        status, output, diagnostics = self.run_query("input.jsonl", "--triage")
        self.assertEqual(status, 0)
        self.assertIn("unclosed start at capture end", output)
        self.assertIn("missing-counterpart", output)
        self.assertIn("not a proven product requirement", output)
        self.assertIn("partial captures", diagnostics)
        self.write("input.jsonl", [{"event": "ordinary.event"}])
        status, output, _ = self.run_query("input.jsonl", "--triage")
        self.assertEqual(status, 1)
        self.assertIn("no deterministic lifecycle anomaly", output)

    def test_triage_gaps_use_only_same_source_actual_clock_and_detect_regression(self):
        self.write("input.jsonl", [
            {"event": "one", "request_id": 4, "steady_ns": 1},
            {"event": "two", "request_id": 4, "steady_ns": 2000000001},
            {"event": "three", "request_id": 4, "elapsed_ms": 10000},
            {"event": "four", "request_id": 4, "elapsed_ms": 9999},
            {"event": "job.failed", "request_id": 4},
        ])
        status, output, _ = self.run_query("input.jsonl", "--triage", "--top", "20")
        self.assertEqual(status, 0)
        self.assertIn("2000.000ms", output)
        self.assertNotIn("8000.000ms", output)
        self.assertIn("clock-regression", output)
        self.assertNotIn("gap:", self.run_query("input.jsonl", "--triage", "--triage-gap-ms", "3000")[1])
        self.write("input.jsonl", [
            {"event": "before", "request_id": 4, "elapsed_ms": 0},
            {"event": "job.failed", "request_id": 4, "steady_ns": 100000000000},
        ])
        self.assertNotIn("gap [", self.run_query("input.jsonl", "--triage")[1])

    def test_triage_hops_are_capped_and_empty_ids_do_not_expand(self):
        self.write("input.jsonl", [
            {"event": "seed.failed", "request_id": 1, "trace_id": 0},
            *({"event": "padding"} for _ in range(10)),
            {"event": "hop.one", "request_id": 1, "span_id": 2},
            *({"event": "padding"} for _ in range(10)),
            {"event": "hop.two", "span_id": 2, "trace_id": 3},
            {"event": "hop.three", "trace_id": 3, "request_id": 4},
            {"event": "zero", "trace_id": 0},
        ])
        args = ("input.jsonl", "--triage", "-q", "event=seed.failed", "--triage-hops", "0", "--format", "jsonl")
        _, rows, _ = self.exported(*args)
        self.assertIn("hop.one", {row["data"].get("event") for row in rows})
        self.assertNotIn("hop.two", {row["data"].get("event") for row in rows})
        _, rows, _ = self.exported("input.jsonl", "--triage", "-q", "event=seed.failed", "--triage-hops", "1")
        events = {row["data"].get("event") for row in rows}
        self.assertIn("hop.two", events)
        self.assertNotIn("hop.three", events)
        self.assertNotIn("zero", events)

    def test_triage_bounds_cardinality_payload_and_default_output_without_related_run(self):
        self.write("input.jsonl", [
            {"event": "seed.failed", "request_id": 1, "detail": "x" * 10000},
            *({"event": f"stage.{index}", "request_id": 1, "span_id": index + 10} for index in range(1000)),
            *({"event": "unrelated.noise", "request_id": 9000} for _ in range(1000)),
        ])
        args = ("input.jsonl", "--triage", "--triage-identities", "3", "--limit", "7")
        status, rows, diagnostics = self.exported(*args)
        self.assertEqual(status, 0, diagnostics)
        self.assertEqual(len(rows), 7)
        self.assertTrue(any(row["_log"].get("triage_payload_truncated") for row in rows))
        self.assertTrue(all(len(row["text"]) <= logs.MAX_TRIAGE_TEXT + 1 for row in rows))
        self.assertNotIn("unrelated.noise", {row["data"].get("event") for row in rows})
        self.assertIn("Identity budget reached", diagnostics)
        self.assertIn("capacity reached", diagnostics)
        self.assertEqual(self.exported(*args)[1], rows)
        for incompatible in ("--related-run", "--tail", "--list-runs"):
            self.assertEqual(self.run_query("input.jsonl", "--triage", incompatible)[0], 2)

    def test_triage_run_reuse_where_and_context_copies_are_not_false_duplicates(self):
        self.family_fixture()
        _, rows, _ = self.exported("--family", "session", "--history", "--run", "current",
                                    "--triage", "-q", 'buffer="BufferId(21,1)"')
        self.assertEqual({row["_log"]["run"] for row in rows}, {"build/validation/session@current"})
        self.write("copy.log",
            "[ RUN ] copied_test\n"
            '/workspace/test.cpp:4: failed: false with 1 message: \'native diagnostics:\n'
            '{"event":"job.completed","span_id":7}\n'
            '{"event":"job.completed","span_id":7}\n')
        status, output, diagnostics = self.run_query("copy.log", "--triage")
        self.assertEqual(status, 0, diagnostics)
        self.assertNotIn("duplicate-terminal", output)
        self.assertIn("copies=2", output)
        self.write("input.jsonl", [
            {"event": "work.started", "request_id": 1},
            {"event": "work.failed", "request_id": 1},
            {"event": "excluded", "request_id": 1},
        ])
        _, rows, _ = self.exported("input.jsonl", "--triage", "--where", "NOT event=excluded")
        self.assertNotIn("excluded", {row["data"].get("event") for row in rows})

    def test_triage_parse_damage_is_bounded_evidence_and_strict_remains_strict(self):
        self.write("broken.jsonl", '{"event":\n{"event":"worker.completed","span_outcome":2,"span_id":4}\n')
        status, output, diagnostics = self.run_query("broken.jsonl", "--triage")
        self.assertEqual(status, 0)
        self.assertIn("parse failure", output)
        self.assertIn("parse damage is not itself a product failure", diagnostics)
        self.assertNotIn("failed-span", output)
        self.assertEqual(self.run_query("broken.jsonl", "--triage", "--strict")[0], 2)
        self.assertEqual(self.run_query("broken.jsonl", "--triage", "--strict", "--where", "span_id=4")[0], 2)


if __name__ == "__main__":
    unittest.main()
