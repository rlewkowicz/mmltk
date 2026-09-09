"""Required parser, bounded selection, and log format behavior for ./mmltk --logs."""

from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
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

    def test_shared_event_anchors_link_transcript_and_propagate_test_tags(self):
        self.family_fixture()
        self.write("build/validation/transcript.log",
            "Filters: [viewer_copy][hardware]\n"
            "[ RUN ] workspace_wayland_viewer_copy\n"
            'workspace-wayland[native]: {"event":"browser.server.started","steady_ns":100}\n'
            '{"event":"firefox.webgpu.blocking_map","buffer":"BufferId(21,1)","detail":"arc_extract"}\n'
            "native host terminal status: exit 139\n"
            "[ FAILED ] workspace_wayland_viewer_copy\n")
        status, rows, _ = self.exported("build/validation/transcript.log", "--family", "session",
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


if __name__ == "__main__":
    unittest.main()
