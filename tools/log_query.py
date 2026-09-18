#!/usr/bin/env python3
"""Bounded, read-only queries over runtime logs; invoked through ./mmltk --logs."""

import argparse
import ast
from bisect import bisect_left
from collections import Counter, deque
from dataclasses import dataclass, field, replace
from datetime import datetime, timezone
from decimal import Decimal, InvalidOperation
import errno
import glob
import heapq
from itertools import islice
import json
import math
import os
from pathlib import Path
import re
import signal
import sys


MISSING = object()
MAX_QUERY_LENGTH = 8192
MAX_QUERY_TOKENS = 512
MAX_QUERY_DEPTH = 32
MAX_LINE_BYTES = 1024 * 1024
MAX_MESSAGE_LINES = 128
MAX_VULKAN_OBJECTS = 64
MAX_DISTINCT_KEYS = 10000
ROTATION_WINDOW_NS = 10000000
ROTATION_NAME = re.compile(r"(\d+)-(\d+)\Z")
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
LEXEME = re.compile(
    r"""\s*(?:("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')"""
    r"""|(!=|!~|>=|<=|[():=~<>])|([^\s():=~<>!"']+))"""
)
NUMBER = re.compile(r"-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?\Z")
FIELD_NAME = re.compile(r"@?[A-Za-z_][\w.-]*\Z")
KEY_VALUE = re.compile(
    r"""(?:^|\s)([A-Za-z_][\w.-]*)=("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|.*?)"""
    r"""(?=\s+[A-Za-z_][\w.-]*=|$)"""
)
NATIVE = re.compile(
    r"^(?P<timestamp>\d{4}-\d\d-\d\d[ T]\d\d:\d\d:\d\d(?:\.\d+)?"
    r"(?:Z|[+-]\d\d:\d\d)?) \[(?P<pid>\d+):(?P<tid>\d+)\] "
    r"\[(?P<logger>[^]]*)\] \[(?P<level>[^]]*)\] (?P<message>.*)$",
    re.DOTALL,
)
FIREFOX = re.compile(
    r"^(?:(?P<timestamp>\d{4}-\d\d-\d\d[ T]\d\d:\d\d:\d\d(?:\.\d+)?"
    r"(?: UTC|Z|[+-]\d\d:\d\d)?)\s+)?"
    r"\[(?P<process>.*?) (?P<pid>\d+): (?P<thread>[^]]+)\]: "
    r"(?P<level>[VDIWE])/(?P<module>\S+) (?P<message>.*)$",
    re.DOTALL,
)
RUST = re.compile(
    r"^\[(?:(?P<timestamp>\d{4}-\d\d-\d\dT\S+)\s+)?"
    r"(?P<level>TRACE|DEBUG|INFO|WARN|ERROR)\s+(?P<module>[^\s\]]+)\]\s?(?P<message>.*)$",
    re.DOTALL,
)
VULKAN_HEADER = re.compile(r"^VALIDATION\s+\[(?P<id>[A-Za-z0-9_-]+)(?:\s+\((?P<message_id>0x[0-9a-fA-F]+)\))?\]")
VULKAN_BODY = re.compile(
    r"^\s*Validation\s+(?P<level>Error|Warning|Info):\s*\[\s*(?P<id>[A-Za-z0-9_-]+)\s*\]",
    re.IGNORECASE,
)
VULKAN_OBJECT = re.compile(
    r"Object\s+\d+:\s*handle\s*=\s*(?P<handle>0x[0-9a-fA-F]+),\s*"
    r"type\s*=\s*VK_OBJECT_TYPE_(?P<type>[A-Z0-9_]+)"
)
VULKAN_RUST_OBJECT = re.compile(
    r"\(type:\s*(?P<type>[A-Z0-9_]+),\s*hndl:\s*(?P<handle>0x[0-9a-fA-F]+)"
    r"(?:,\s*name:\s*(?P<name>[^)\r\n]*))?\)"
)
VULKAN_REFERENCE = re.compile(
    r"\bVk(?P<type>[A-Z][A-Za-z0-9]*)[ \t]+(?P<handle>0x[0-9a-fA-F]+)"
    r"\[(?P<name>[^\]\r\n]*)\]"
)
# Typed resource joins retain the existing process/file scopes. Device and queue
# references remain message facts, never broad join keys.
VULKAN_RESOURCE_FIELDS = {
    "IMAGE": "vk_image", "DEVICE_MEMORY": "vk_memory", "COMMAND_BUFFER": "vk_command_buffer",
}
FAILURE_WORD = re.compile(
    r"(?<![a-z])(?:error|fatal|panic|failed|failure|exception|crash|timeout|timed out"
    r"|segmentation fault|sigsegv|sigabrt|sigbus|aborted"
    r"|invalid server CBOR record|invalid fixed text size|corrupted size vs\. prev_size"
    r"|corrupted double-linked list|double free|malloc\(\):|free\(\):)(?![a-z])",
    re.IGNORECASE,
)
LEVEL_NAMES = {
    "v": "trace", "t": "trace", "d": "debug", "i": "info", "w": "warning",
    "warn": "warning", "e": "error", "err": "error", "f": "fatal",
}
TEST_STATUS = re.compile(r"^\[\s*(RUN|FAILED|OK|SKIPPED)\s*\]\s+(.+)$")
CATCH_FAILURE = re.compile(r"^(.+?):(\d+): (failed|skipped|warning|fatal error): (.*)$")
CONTEXT_LABEL = re.compile(
    r"(?:^['\"]?|['\"] and ['\"]|\bwith \d+ messages?:\s*['\"])"
    r"(native diagnostics|native runtime log|Firefox diagnostics|acceptance diagnostics|Mozilla diagnostics|application log):\s*"
)
ANCHOR_EVENTS = (
    "browser.server.started", "child.spawned", "child.signaled", "child.exited",
    "shutdown.requested", "shutdown.firefox_terminal", "shutdown.complete",
)
ANCHOR_HINT = re.compile("|".join(re.escape(event) for event in ANCHOR_EVENTS))
HANDOFF_EVENT = re.compile(r"(?:^|[._])(?:intent|reply|response|message|failed|failure|error)(?:[._]|$)")
TYPED_ID = re.compile(r"\b([A-Z][A-Za-z0-9]*Id)\((\d+(?:,\s*\d+)*)\)")
STAGE_SUFFIX = re.compile(
    r"^(.*?)[._/-](start|started|begin|begun|requested|submitted|opened|acquired|"
    r"end|ended|stop|stopped|complete|completed|finish|finished|closed|released|"
    r"retired|destroyed|outcome|terminal|success|succeeded|failed|failure|error|"
    r"cancelled|canceled|aborted|rejected|missing|unavailable|blocked|stalled|pending|discarded)$",
    re.IGNORECASE,
)
START_STAGES = frozenset(("start", "started", "begin", "begun", "requested", "submitted", "opened", "acquired"))
INCOMPLETE_STAGES = frozenset(("missing", "unavailable", "blocked", "stalled", "pending", "discarded"))
FAILED_STAGES = frozenset(("failed", "failure", "error", "aborted", "rejected"))
AMBIENT_IDS = frozenset(("pid", "tid", "process_id", "thread_id", "device_id", "host_id",
                         "user_id", "test_id", "run_id", "session_id", "parent_process_id",
                         "native_process_id", "browser_process_id"))
REFINEMENT_FIELDS = ("sequence", "generation", "slot")
MAX_TRIAGE_STATES = 256
MAX_TRIAGE_EVENTS = 64
MAX_TRIAGE_TEXT = 2048
MAX_TRIAGE_PIXEL_SAMPLES = 16384
NATIVE_PIXEL_STAGES = {
    "presentation.pixel": "native",
    "presentation.pixel_after_release": "native_after_release",
    "presentation.pixel_source_copy_before_ready": "source_copy_before_ready",
    "presentation.pixel_source_copy_after_release": "source_copy_after_release",
}
DIRECT_PIXEL_STAGES = {
    "firefox.workspace.direct_pixel": "direct_image",
}
DIRECT_ALLOCATION_FIELDS = (
    "workspace_allocation", "vk_image", "vk_memory", "memory_type_index", "dedicated", "memory_size",
    "image_offset", "row_pitch", "capacity_width", "capacity_height",
)
MEMORY_PROVENANCE_EVENTS = frozenset((
    "cuda.workspace.memory_export", "gpu.workspace.memory_export",
    "firefox.workspace.image_created", "firefox.workspace.memory_import", "firefox.workspace.memory_bound",
    "presentation.workspace.descriptor_duplicated", "presentation.workspace.descriptor_sent",
    "presentation.workspace.descriptor_duplicate_failed", "presentation.workspace.descriptor_send_failed",
    "firefox.workspace.descriptor_received", "firefox.workspace.descriptor_claimed",
    "firefox.workspace.memory_allocation", "firefox.workspace.memory_export", "firefox.workspace.memory_export_failed",
    "firefox.workspace.descriptor_send", "firefox.workspace.descriptor_send_result",
    "presentation.workspace.descriptor_received", "cuda.workspace.memory_import_started",
    "cuda.workspace.memory_import", "cuda.workspace.memory_map", "cuda.workspace.memory_import_rejected",
    "cuda.workspace.memory_retirement_started", "cuda.workspace.memory_retirement",
))
VULKAN_PROVENANCE_EVENTS = frozenset((
    "firefox.workspace.image_created", "firefox.workspace.memory_import",
    "firefox.workspace.memory_bound", "firefox.workspace.direct_binding",
    "firefox.workspace.memory_allocation", "firefox.workspace.memory_export", "firefox.workspace.memory_export_failed",
))
MEMORY_PROVENANCE_FIELDS = (
    "native_process_id", "browser_process_id", "workspace_descriptor", "cuda_address",
    "retained_memory_descriptor", "import_descriptor", "cuda_external_memory", "cuda_mapped_base",
    "cuda_status", "vk_status", "device_incarnation", "initialization",
    "cuda_export_allocation", "cuda_mapped_allocation", "cuda_mapping_matches_export",
    "cuda_retain_status", "cuda_release_status", "cuda_property_status", "cuda_allocation_type",
    "cuda_location_type", "cuda_location_id", "cuda_requested_handle_types", "cuda_compression_type",
    "cuda_gpu_direct_rdma", "cuda_allocation_usage", "cuda_uuid_status", "device_uuid",
    "vk_device", "vk_physical_device", "vk_image", "vk_memory", "memory_type_index",
    "memory_property_flags", "memory_heap_index", "memory_size", "reserved_bytes",
    "row_pitch", "image_offset", "capacity_width", "capacity_height", "cuda_mapping_offset",
    "dedicated", "image_binding_offset", "image_tiling", "image_format", "image_usage", "image_flags",
    "image_depth", "image_type", "image_mip_levels", "image_array_layers", "image_samples",
    "image_sharing_mode", "image_initial_layout", "requirements_size", "requirements_alignment",
    "requirements_memory_type_bits", "requires_dedicated", "prefers_dedicated", "external_memory_features",
    "subresource_size", "array_pitch", "depth_pitch", "described_row_pitch", "described_image_offset",
    "export_handle_type", "import_handle_type", "external_handle_type",
    "fd_stat_status", "fd_stat_errno", "fd_dev", "fd_ino", "fd_rdev", "fd_mode", "fd_size",
    "fd_consumed", "fd_identity_scope", "fd_properties_query",
    "export_descriptor", "channel_descriptor", "descriptor_count", "memory_descriptor_index",
    "descriptor_transport", "record_bytes", "send_bytes", "send_errno", "fd_dup_errno", "peer_credentials_status",
    "peer_credentials_errno", "peer_credentials_known", "fd_getfd_result", "fd_getfd_errno",
    "fd_ofd_query", "fd_ofd_result", "fd_ofd_errno", "fd_ofd_status", "fd_observation", "vk_allocate_status",
)
DESCRIPTOR_LINEAGE_STAGES = {
    "cuda.workspace.memory_export": "cuda_export",
    "gpu.workspace.memory_export": "workspace_export",
    "presentation.workspace.descriptor_duplicated": "duplicated",
    "presentation.workspace.descriptor_sent": "sent",
    "presentation.workspace.descriptor_duplicate_failed": "duplicate_failed",
    "presentation.workspace.descriptor_send_failed": "send_failed",
    "firefox.workspace.descriptor_received": "received",
    "firefox.workspace.descriptor_claimed": "claimed",
    "firefox.workspace.memory_import": "import",
    "firefox.workspace.memory_allocation": "allocation",
    "firefox.workspace.memory_bound": "bound",
    "firefox.workspace.memory_export": "vulkan_export",
    "firefox.workspace.memory_export_failed": "export_failed",
    "firefox.workspace.descriptor_send": "send_attempt",
    "firefox.workspace.descriptor_send_result": "send_result",
    "presentation.workspace.descriptor_received": "native_received",
    "cuda.workspace.memory_import_started": "cuda_import_started",
    "cuda.workspace.memory_import": "cuda_import",
    "cuda.workspace.memory_map": "cuda_map",
    "cuda.workspace.memory_import_rejected": "import_rejected",
    "cuda.workspace.memory_retirement_started": "retirement_started",
    "cuda.workspace.memory_retirement": "retirement",
}
LIBRARY_PROVENANCE_EVENTS = frozenset((
    "cuda.workspace.loaded_library", "cuda.workspace.library_inventory",
    "firefox.workspace.loaded_library", "firefox.workspace.library_inventory",
))
MAX_AUTO_ANCHORS = 128
MAX_AUTO_RELATED = 48
MAX_AUTO_WEAK = 8
MAX_AUTO_PER_ANCHOR = 3
MAX_AUTO_PER_IDENTITY = 2
MAX_AUTO_VULKAN_RELATED = 4
AUTO_SOURCE_DOMAINS = frozenset(("explore", "annotation", "upscale", "live"))
AUTO_FIELDS = (
    "source_session", "source_instance", "source_revision", "frame_revision",
    "source_observation_revision", "snapshot_revision", "observation_revision",
    "presentation_revision", "allocation_generation", "transfer_sequence", "timeline_ready",
    "trace_id", "span_id", "parent_span_id", "request_id", "operation_id",
    "native_process_id", "browser_process_id", "workspace_allocation", "dataset_identity", "gallery_identity", "gallery_generation", "phase", "phase_id", "control",
)
# The integration reporter is a distinct external format: these event-specific
# f64 slots are translated once, never treated as generic numeric identities.
AUTO_INTEGRATION_FIELDS = {
    "integration.explore_reopen_wait": ("explore", {"a": "snapshot_revision", "b": "source_revision"}),
    "integration.explore_reopen_draw": ("explore", {"c": "source_revision", "d": "presentation_revision"}),
    "integration.explore_reopened": ("explore", {"a": "snapshot_revision", "b": "source_revision"}),
    "integration.explore_state": ("explore", {"a": "snapshot_revision"}),
    "integration.explore_frame": ("explore", {"a": "source_revision"}),
    "integration.annotation_ready": ("annotation", {"a": "source_revision"}),
    # c/d are distinct presentation and received-frame frontiers, not an
    # asserted matching publication of the native Upscale frame in b.
    "integration.upscale_settlement": ("upscale", {"a": "snapshot_revision", "b": "source_revision"}),
}
AUTO_NOISE = re.compile(r"(?:^|[._])(?:pixel|probe|slot|sample|tick|heartbeat|metric|poll)(?:[._]|$)")
AUTO_LIFECYCLE_STAGES = START_STAGES | FAILED_STAGES | INCOMPLETE_STAGES


def normalized_text(value):
    return " ".join(value.split()).casefold()


def record_text(record):
    pending = list(reversed(list(record.data.values())))
    strings = []
    while pending:
        value = pending.pop()
        if isinstance(value, str):
            strings.append(value)
        elif isinstance(value, dict):
            pending.extend(reversed(list(value.values())))
        elif isinstance(value, list):
            pending.extend(reversed(value))
    return normalized_text(" ".join(strings))


class QueryError(ValueError):
    """Invalid query, unreadable input, or an explicitly bounded resource limit."""


def scalar(text, quoted=False):
    if quoted:
        return ast.literal_eval(text)
    if text in ("true", "false", "null"):
        return {"true": True, "false": False, "null": None}[text]
    if NUMBER.fullmatch(text):
        try:
            value = float(text) if any(character in text for character in ".eE") else int(text)
        except ValueError as error:
            raise QueryError("numeric literal exceeds interpreter limits") from error
        if isinstance(value, float) and not math.isfinite(value):
            raise QueryError("numeric literals must be finite")
        return value
    return text


def encoded(value):
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"))


def exact(left, right):
    # bool is an int subclass; an identity of 1 must never equal true.
    if isinstance(left, bool) or isinstance(right, bool):
        return type(left) is type(right) and left == right
    return left == right


@dataclass(frozen=True)
class Expression:
    operation: str
    arguments: tuple

    def matches(self, record):
        if self.operation == "and":
            return all(child.matches(record) for child in self.arguments)
        if self.operation == "or":
            return any(child.matches(record) for child in self.arguments)
        if self.operation == "not":
            return not self.arguments[0].matches(record)
        if self.operation == "text":
            needle = self.arguments[0].casefold()
            return needle in record.raw.casefold() or any(
                needle in str(record.get(name)).casefold()
                for name in ("@test", "@tags", "@run", "@archive_id", "@family", "@signal")
                if record.get(name) is not MISSING
            )
        if self.operation == "all":
            return True
        if self.operation == "phrase":
            text = record_text(record)
            return any(phrase in text for phrase in self.arguments)
        if self.operation == "words":
            text = record_text(record)
            return all(word in text for word in self.arguments)
        value = record.get(self.arguments[0])
        if self.operation == "has":
            return value is not MISSING
        if value is MISSING:
            return False
        expected = self.arguments[1]
        if self.operation in ("=", "!="):
            equal = exact(value, expected)
            return equal if self.operation == "=" else not equal
        if self.operation == ":":
            return str(expected).casefold() in str(value).casefold()
        if self.operation in ("~", "!~"):
            found = expected.search(str(value)) is not None
            return found if self.operation == "~" else not found
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            return False
        if self.operation == ">":
            return value > expected
        if self.operation == ">=":
            return value >= expected
        if self.operation == "<":
            return value < expected
        return value <= expected


class QueryParser:
    """Recursive descent: OR < explicit/implicit AND < NOT < primary."""

    def __init__(self, source):
        if len(source) > MAX_QUERY_LENGTH:
            raise QueryError(f"query exceeds {MAX_QUERY_LENGTH} characters")
        self.tokens = []
        position = 0
        while source[position:].strip():
            match = LEXEME.match(source, position)
            if match is None:
                raise QueryError(f"unexpected character at column {position + 1}")
            quoted, operator, word = match.groups()
            self.tokens.append((quoted or operator or word, quoted is not None))
            position = match.end()
            if len(self.tokens) > MAX_QUERY_TOKENS:
                raise QueryError(f"query exceeds {MAX_QUERY_TOKENS} tokens")
        self.index = 0

    def peek(self, value):
        return (
            self.index < len(self.tokens)
            and not self.tokens[self.index][1]
            and self.tokens[self.index][0].lower() == value
        )

    def take(self):
        if self.index == len(self.tokens):
            raise QueryError("unexpected end of query")
        token = self.tokens[self.index]
        self.index += 1
        return token

    def require(self, value):
        if not self.peek(value):
            raise QueryError(f"expected {value!r} at token {self.index + 1}")
        self.index += 1

    def parse(self):
        if not self.tokens:
            return Expression("all", ())
        expression = self.parse_or(0)
        if self.index != len(self.tokens):
            raise QueryError(f"unexpected token {self.tokens[self.index][0]!r}")
        return expression

    def parse_or(self, depth):
        parts = [self.parse_and(depth)]
        while self.peek("or"):
            self.take()
            parts.append(self.parse_and(depth))
        return parts[0] if len(parts) == 1 else Expression("or", tuple(parts))

    def parse_and(self, depth):
        parts = [self.primary(depth)]
        while self.index < len(self.tokens) and not self.peek("or") and not self.peek(")"):
            if self.peek("and"):
                self.take()
            parts.append(self.primary(depth))
        return parts[0] if len(parts) == 1 else Expression("and", tuple(parts))

    def primary(self, depth):
        if depth > MAX_QUERY_DEPTH:
            raise QueryError(f"query nesting exceeds {MAX_QUERY_DEPTH}")
        if self.peek("not"):
            self.take()
            return Expression("not", (self.primary(depth + 1),))
        if self.peek("("):
            self.take()
            result = self.parse_or(depth + 1)
            self.require(")")
            return result
        if self.peek("has"):
            self.take()
            self.require("(")
            name, quoted = self.take()
            if quoted or not FIELD_NAME.fullmatch(name):
                raise QueryError("has() requires a field name")
            self.require(")")
            return Expression("has", (name,))
        name, quoted = self.take()
        if not quoted and name.lower() in ("and", "or", ")", "=", ":", "~", "!~", "!=", ">", ">=", "<", "<="):
            raise QueryError(f"expected a search term or field, received {name!r}")
        if self.index < len(self.tokens) and not self.tokens[self.index][1]:
            operator = self.tokens[self.index][0]
            if operator in ("=", "!=", ":", "~", "!~", ">", ">=", "<", "<="):
                if quoted or not FIELD_NAME.fullmatch(name):
                    raise QueryError("comparisons require an unquoted field name")
                self.take()
                value, is_quoted = self.take()
                if not is_quoted and value in ("(", ")", "=", ":", "~", "!~", "!=", ">", ">=", "<", "<="):
                    raise QueryError("comparison requires a value; quote literal punctuation")
                expected = scalar(value, is_quoted)
                if operator in (">", ">=", "<", "<="):
                    if isinstance(expected, bool) or not isinstance(expected, (int, float)):
                        raise QueryError(f"{operator} requires a numeric value")
                if operator in ("~", "!~"):
                    try:
                        expected = re.compile(str(expected))
                    except re.error as error:
                        raise QueryError(f"invalid regular expression: {error}") from error
                return Expression(operator, (name, expected))
        return Expression("all", ()) if name == "*" and not quoted else Expression("text", (scalar(name, True) if quoted else name,))


def lookup(data, name):
    if name in data:
        return data[name]
    value = data
    for part in name.split("."):
        if isinstance(value, dict) and part in value:
            value = value[part]
        elif isinstance(value, list):
            try:
                index = int(part)
            except ValueError:
                return MISSING
            if str(index) != part or not 0 <= index < len(value):
                return MISSING
            value = value[index]
        else:
            return MISSING
    return value


def value_from(data, *names):
    for name in names:
        value = lookup(data, name)
        if value is not MISSING and value is not None and value != "":
            return value
    return MISSING


def timestamp_ns(value):
    """Preserve nanoseconds and separate timezone-free wall time from UTC."""
    if not isinstance(value, str):
        return None
    text = value.replace(" UTC", "+00:00").replace("Z", "+00:00")
    fractional = re.search(r"\.(\d+)", text)
    fraction_ns = int((fractional[1] + "000000000")[:9]) if fractional else 0
    if fractional:
        text = text[:fractional.start()] + text[fractional.end():]
    try:
        moment = datetime.fromisoformat(text)
    except ValueError:
        return None
    clock = "wall" if moment.tzinfo is not None else "wall-local"
    delta = moment.replace(tzinfo=moment.tzinfo or timezone.utc) - datetime(1970, 1, 1, tzinfo=timezone.utc)
    return clock, (delta.days * 86400 + delta.seconds) * 1000000000 + fraction_ns


def record_time(data, source):
    for name, clock in (("steady_ns", "steady"), ("monotonic_ns", "steady"),
                        ("timestamp_ns", "wall"), ("unix_ns", "wall")):
        value = value_from(data, name, "fields." + name)
        if type(value) is int:
            return clock, value
    for name in ("timestamp", "time", "fields.timestamp"):
        parsed = timestamp_ns(lookup(data, name))
        if parsed is not None:
            return parsed
    elapsed = value_from(data, "elapsed_ms", "fields.elapsed_ms")
    if type(elapsed) is int:
        return "elapsed:" + source, elapsed * 1000000
    if type(elapsed) is float and math.isfinite(elapsed):
        return "elapsed:" + source, int(Decimal(str(elapsed)) * 1000000)
    return "none", None


def physical_identity(data, text, high_name, low_name):
    value = value_from(data, text, "fields." + text)
    textual = value.lower() if isinstance(value, str) and re.fullmatch(r"[0-9a-fA-F]{32}", value) and int(value, 16) != 0 else MISSING
    high = value_from(data, high_name, "fields." + high_name)
    low = value_from(data, low_name, "fields." + low_name)
    valid_numeric = all(type(part) is int and 0 <= part < 2**64 for part in (high, low))
    numeric = f"{high:016x}{low:016x}" if valid_numeric and (high or low) else MISSING
    if ((value is not MISSING and textual is MISSING) or
            ((high is not MISSING or low is not MISSING) and numeric is MISSING) or
            (textual is not MISSING and numeric is not MISSING and textual != numeric)):
        return MISSING
    return textual if textual is not MISSING else numeric


def source_lifecycle(data):
    event = value_from(data, "fields.event", "fields.name", "event", "name")
    return isinstance(event, str) and (event.startswith("firefox.workspace.source.") or
        event.startswith("presentation.source.") and not event.endswith("read_submitted"))


def surface_identity(data):
    if source_lifecycle(data):
        return physical_identity(data, "arena", "workspace_arena_high", "workspace_arena_low")
    return physical_identity(data, "surface", "surface_high", "surface_low")


def workspace_source_identity(data):
    if source_lifecycle(data):
        return physical_identity(data, "surface", "workspace_source_high", "workspace_source_low")
    return physical_identity(data, "source", "workspace_source_high", "workspace_source_low")


def vulkan_handle(value, *, allow_null=False):
    if isinstance(value, str) and re.fullmatch(r"0x[0-9a-fA-F]{1,16}", value):
        number = int(value, 16)
        if number or allow_null:
            return hex(number)
    return None


def vulkan_resources(data):
    """One typed projection for diagnostic objects and existing workspace records."""
    result = {}
    remaining = MAX_VULKAN_OBJECTS
    for collection in ("objects", "referenced_objects"):
        objects = data.get(collection)
        if isinstance(objects, list):
            for item in objects[:remaining]:
                if (isinstance(item, dict) and isinstance(item.get("type"), str)
                        and (name := VULKAN_RESOURCE_FIELDS.get(item["type"]))):
                    if handle := vulkan_handle(item.get("handle")):
                        result[name, handle] = None
            remaining -= min(len(objects), remaining)
    for name in VULKAN_RESOURCE_FIELDS.values():
        if handle := vulkan_handle(value_from(data, name, "fields." + name)):
            result[name, handle] = None
    return tuple(result)


def vulkan_process(record):
    """Missing process facts remain missing; contradictory facts cannot join."""
    values = [record.get(name) for name in ("browser_process_id", "pid")]
    values = [value for value in values if value is not MISSING]
    if not values:
        return None
    if any(type(value) is not int or value <= 0 for value in values) or len(set(values)) != 1:
        return MISSING
    return values[0]


@dataclass
class Record:
    source: str
    line: int
    raw: str
    data: dict
    format: str
    parse_error: str = ""
    metadata: dict = field(default_factory=dict)
    clock: str = field(init=False)
    time_ns: int | None = field(init=False)

    def __post_init__(self):
        self.clock, self.time_ns = record_time(self.data, self.source)

    def get(self, name):
        if name == "@file":
            return self.source
        if name == "@line":
            return self.line
        if name == "@line_end":
            return self.metadata.get("line_end", self.line)
        if name == "@text":
            return self.raw
        if name == "@format":
            return self.format
        if name == "@clock":
            return self.clock
        if name == "@time_ns":
            return MISSING if self.time_ns is None else self.time_ns
        if name == "@parse_error":
            return self.parse_error or MISSING
        if name == "@surface":
            return surface_identity(self.data)
        if name == "@workspace_source":
            return workspace_source_identity(self.data)
        if name == "@event":
            return value_from(self.data, "fields.event", "fields.name", "event", "name")
        if name == "@owner":
            owner = value_from(self.data, "owner", "fields.owner", "component", "system", "module", "logger")
            event = self.get("@event")
            return event.split(".", 1)[0] if owner is MISSING and isinstance(event, str) else owner
        if name == "@level":
            value = value_from(self.data, "level", "severity", "fields.level")
            return LEVEL_NAMES.get(str(value).lower(), str(value).lower()) if value is not MISSING else MISSING
        if name == "@error":
            code = self.get("@exit_code")
            if code is not MISSING and code != 0:
                return True
            if self.get("@level") in ("error", "critical", "fatal", "panic"):
                return True
            # Pixel error=0, failed=false, and similar numeric metrics are not failures.
            values = (self.get("@event"), value_from(self.data, "message", "fields.message", "detail", "error"))
            return any(isinstance(value, str) and FAILURE_WORD.search(value) for value in values)
        if name == "@exit_code":
            if self.get("@event") in ("child.signaled", "child.exited"):
                return self.get("value")
            return self.get("exit_code")
        if name in ("@signal", "@signal_number"):
            code = self.get("@exit_code")
            if self.get("@event") != "child.signaled" or type(code) is not int:
                return MISSING
            number = code - 128
            if number not in signal.valid_signals():
                return MISSING
            try:
                return number if name == "@signal_number" else signal.Signals(number).name
            except ValueError:
                return f"SIG{number}"
        if name == "@terminal":
            return self.get("@event") in (
                "child.signaled", "child.exited", "shutdown.complete", "shutdown.firefox_terminal",
                "catch.test_failed", "catch.test_passed", "catch.test_skipped", "catch.summary",
                "process.terminal", "build.image", "build.failed",
            )
        if name.startswith("@"):
            return self.metadata.get(name[1:], MISSING)
        value = lookup(self.data, name)
        if value is MISSING and "." not in name:
            value = lookup(self.data, "fields." + name)
        if value is MISSING and name in VULKAN_RESOURCE_FIELDS.values():
            handles = [handle for field_name, handle in vulkan_resources(self.data) if field_name == name]
            if len(handles) == 1:
                return handles[0]
        return value


def reject_json_constant(token):
    raise ValueError(f"non-finite JSON number: {token}")


def finite_json_float(token):
    value = float(token)
    if not math.isfinite(value):
        raise ValueError("JSON number exceeds finite float range")
    return value


JSON_DECODER = json.JSONDecoder(parse_constant=reject_json_constant, parse_float=finite_json_float)


def embedded_objects(message):
    """Extract bounded complete objects, including a JSON-escaped INFO string."""
    position = 0
    attempts = 0
    while position < len(message) and attempts < 32:
        match = re.search(r'\{|"(?:\\.|[^"\\])*"', message[position:])
        if match is None:
            return
        start = position + match.start()
        attempts += 1
        try:
            value, end = JSON_DECODER.raw_decode(message, start)
        except (ValueError, RecursionError):
            position = start + 1
            continue
        position = end
        if isinstance(value, dict):
            yield start, value
        elif isinstance(value, str) and "{" in value:
            inner_start = value.find("{")
            try:
                inner, _ = JSON_DECODER.raw_decode(value, inner_start)
            except (ValueError, RecursionError):
                continue
            if isinstance(inner, dict):
                yield start, inner


def transcript_data(text):
    status = TEST_STATUS.match(text)
    if status:
        event = {"RUN": "started", "FAILED": "failed", "OK": "passed", "SKIPPED": "skipped"}[status[1]]
        return {"event": "catch.test_" + event, "test": status[2],
                "level": "error" if event == "failed" else "info", "message": text}
    failure = CATCH_FAILURE.match(text)
    if failure:
        return {
            "event": "catch.assertion_" + failure[3].replace(" ", "_"),
            "source_file": failure[1], "source_line": int(failure[2]),
            "level": "error" if failure[3] in ("failed", "fatal error") else "info",
            "message": failure[4],
        }
    if text.startswith("Filters:"):
        return {"event": "catch.filters", "filters": text.removeprefix("Filters:").strip(), "message": text}
    if text.startswith(("test cases:", "assertions:")):
        return {"event": "catch.summary", "message": text}
    terminal = re.search(r"(?:terminal status:\s*exit|exit(?:ed with)?(?:\s+status|\s+code))\s*[=:]?\s*(\d+)", text)
    if terminal:
        return {"event": "process.terminal", "exit_code": int(terminal[1]), "message": text}
    if text.startswith("mmltk:"):
        return {"event": "mmltk.message", "owner": "mmltk", "message": text.removeprefix("mmltk:").strip()}
    if re.search(r"(?:writing image|naming to|exporting (?:image|manifest)|Successfully (?:built|tagged))", text):
        return {"event": "build.image", "owner": "build", "message": text}
    if text.startswith(("FAILED:", "ERROR: failed to build", "ninja: build stopped")):
        return {"event": "build.failed", "owner": "build", "level": "error", "message": text}
    return None


def vulkan_diagnostic(message, data):
    """Keep the layer's assertion as text; extract only explicitly reported facts."""
    header = VULKAN_HEADER.match(message)
    if header is None and "\n" not in message and not VULKAN_BODY.match(message):
        return None
    body = next((match for line in message.splitlines() if (match := VULKAN_BODY.match(line))), None)
    if header is None and body is None:
        return None
    identifier = (header or body)["id"]
    data.update({"event": "vulkan.validation", "validation_id": identifier, "message": message})
    if body:
        data.setdefault("level", body["level"])
    message_id = re.search(r"\bMessageID\s*=\s*(0x[0-9a-fA-F]+)", message)
    if message_id or header and header["message_id"]:
        data["message_id"] = message_id[1] if message_id else header["message_id"]
    api = re.search(r"\b(vk[A-Z][A-Za-z0-9]+)\s*\(", message)
    if api:
        data["api"] = api[1]
    objects, references = {}, {}
    overflow = False
    for pattern, destination in ((VULKAN_OBJECT, objects), (VULKAN_RUST_OBJECT, objects),
                                 (VULKAN_REFERENCE, references)):
        # Callback debug names are labels, not additional resource assertions.
        text = VULKAN_RUST_OBJECT.sub("", message) if pattern is VULKAN_REFERENCE else message
        for match in pattern.finditer(text):
            handle = vulkan_handle(match["handle"], allow_null=True)
            if handle is None:
                continue
            kind = match["type"]
            if pattern is VULKAN_REFERENCE:
                # The validation formatter names the type explicitly. Normalize
                # CamelCase (including extension suffixes), never infer it from
                # a bare hexadecimal value or surrounding access terminology.
                kind = re.sub(r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])", "_", kind).upper()
            key = kind, handle
            if key not in destination:
                if len(objects) + len(references) == MAX_VULKAN_OBJECTS:
                    overflow = True
                    continue
                destination[key] = {"type": kind, "handle": handle}
            name = match.groupdict().get("name")
            if name is not None:
                destination[key]["name"] = name
    data["objects"] = list(objects.values())
    if references:
        data["referenced_objects"] = list(references.values())
    return "Vulkan object facts exceed bounded capacity; original text retained" if overflow else ""


def parse_record(source, line, raw, truncated=False):
    clean = ANSI.sub("", raw.rstrip("\r\n"))
    data = {}
    log_format = "text"
    message = clean
    context = CONTEXT_LABEL.search(clean)
    payload = clean[context.end():] if context else clean
    match = NATIVE.match(payload) or FIREFOX.match(payload) or RUST.match(payload)
    if match:
        log_format = "native" if "logger" in match.groupdict() else "firefox" if "pid" in match.groupdict() else "rust"
        data = {key: value for key, value in match.groupdict().items() if value is not None}
        for key in ("pid", "tid"):
            if key in data:
                data[key] = int(data[key])
        message = data["message"]
    error = "line exceeds byte limit; text is truncated" if truncated else ""
    if (vulkan_error := vulkan_diagnostic(message, data)) is not None:
        return Record(source, line, raw.rstrip("\r\n"), data, log_format, error or vulkan_error)
    transcript = transcript_data(clean)
    if transcript:
        data.update(transcript)
        log_format = "transcript"
    elif "{" in message:
        try:
            objects = None
            first = None
            malformed_jsonl = False
            if source.endswith(".jsonl") and message.lstrip().startswith("{"):
                try:
                    first = 0, JSON_DECODER.decode(message.strip())
                except ValueError:
                    malformed_jsonl = True
            if first is None:
                objects = embedded_objects(message)
                first = next(objects, None)
            if first:
                _, decoded = first
                if objects is not None and next(objects, None) is not None:
                    error = error or "multiple JSON payloads on one line; first retained"
                if malformed_jsonl:
                    error = error or "extra or malformed content in JSONL record"
                data.update(decoded)
                # Keep the original prefix in raw; a JSON payload is not its own message.
                if data.get("message") == message and "message" not in decoded:
                    data.pop("message", None)
                log_format = "json"
            elif message.lstrip().startswith("{") or source.endswith(".jsonl"):
                error = error or "invalid or truncated JSON object"
        except (ValueError, RecursionError):
            error = error or "invalid or truncated JSON object"
    elif source.endswith(".jsonl"):
        error = error or "expected a JSON object"
    if log_format not in ("json", "transcript"):
        data.setdefault("message", message)
        for key, value in KEY_VALUE.findall(message):
            try:
                data[key] = scalar(value, value.startswith(("'", '"')))
            except (ValueError, SyntaxError):
                data[key] = value
    details = value_from(data, "message", "fields.message")
    if isinstance(details, str) and len(details) <= 4096 and details.lstrip().startswith("{"):
        try:
            decoded_details = JSON_DECODER.decode(details)
            if isinstance(decoded_details, dict):
                # Keep the message verbatim and expose bounded object details to
                # ordinary nested-field filters and projections, without an event schema.
                data.setdefault("message_fields", decoded_details)
        except (ValueError, RecursionError):
            pass
    return Record(source, line, clean, data, log_format, error)


class VulkanMessages:
    """Assemble adjacent Rust callback lines before filtering, with bounded storage."""

    def __init__(self):
        self.pending = []
        self.bytes = 0
        self.module = None
        self.process = None
        self.identifier = None
        self.body_seen = False

    def take(self, error=""):
        first, last = self.pending[0], self.pending[-1]
        text = "\n".join(part[1].rstrip("\r\n") for part in self.pending)
        errors = [part[2] for part in self.pending if part[2]]
        self.pending = []
        self.bytes = 0
        self.body_seen = False
        return first[0], last[0], text, error or next(iter(errors), "")

    def push(self, line, text, error):
        clean = ANSI.sub("", text.rstrip("\r\n"))
        prefix = RUST.match(clean) or FIREFOX.match(clean) or NATIVE.match(clean)
        fields = prefix.groupdict() if prefix else {}
        message = fields.get("message", clean)
        header = VULKAN_HEADER.match(message)
        body = VULKAN_BODY.match(message)
        continuation = bool(
            self.pending and not header and (
                prefix and fields.get("module", fields.get("logger")) == self.module
                and fields.get("pid") == self.process and message.lstrip().startswith("objects:")
                or not prefix and (not clean or clean[:1].isspace() or body)
                and not clean.lstrip().startswith(("{", "["))
            )
        )
        # A callback has one body. A second body is an independent orphaned
        # diagnostic, not a continuation of a possibly interleaved callback.
        if body and self.pending and (self.body_seen or body["id"] != self.identifier):
            continuation = False
        size = len(text.encode("utf-8")) if header or body or continuation else 0
        if continuation and (len(self.pending) == MAX_MESSAGE_LINES or self.bytes + size > MAX_LINE_BYTES):
            yield self.take("Vulkan message exceeds byte/line limit; continuation retained separately")
            continuation = False
        if self.pending and not continuation:
            yield self.take()
        if header or body or continuation:
            if not self.pending:
                self.module = fields.get("module", fields.get("logger"))
                self.process = fields.get("pid")
                self.identifier = (header or body)["id"]
            self.pending.append((line, text, error))
            self.body_seen = self.body_seen or bool(body)
            self.bytes += size
        elif text.strip():
            yield line, line, text, error

    def finish(self):
        if self.pending:
            yield self.take()


class TranscriptContext:
    """Carry only transcript context; copied diagnostics retain their original clocks."""

    def __init__(self):
        self.test = ""
        self.tags = []
        self.context = ""
        self.active_run = None
        self.capture = 0
        self.last_timestamp = None

    def decorate(self, record, metadata, anchors):
        event = record.get("@event")
        if event == "catch.filters":
            self.tags = re.findall(r"\[([^]]+)\]", record.data["filters"])
        if event in ("catch.test_started", "catch.test_failed", "catch.test_passed", "catch.test_skipped"):
            self.test = record.data["test"]
        if event == "catch.test_started":
            self.active_run = None
            self.capture += 1
            self.last_timestamp = None
        label = CONTEXT_LABEL.search(record.raw)
        if label:
            self.context = label[1]
        if record.raw.startswith("workspace-wayland[") or event in (
            "catch.test_started", "catch.test_failed", "catch.test_passed", "catch.test_skipped", "process.terminal"
        ):
            self.context = ""
        if record.raw.startswith("workspace-wayland:") and "acceptance deadline armed" in record.raw:
            self.active_run = None
            self.capture += 1
            self.last_timestamp = None
            self.context = ""
        anchor = anchors.get((event, record.time_ns)) if record.clock == "steady" and isinstance(event, str) else None
        if anchor and (self.capture or metadata.get("family") != anchor["family"]):
            self.active_run = anchor
        record.metadata.update(metadata)
        if self.capture and not self.active_run:
            record.metadata["run"] = str(metadata.get("run", record.source)) + f"#capture-{self.capture}"
        if self.active_run:
            record.metadata.update({name: self.active_run[name] for name in ("run", "family")})
            record.metadata["run_link"] = "shared-event-and-steady-time"
        if self.test:
            record.metadata["test"] = self.test
        if self.tags:
            record.metadata["tags"] = self.tags
        if self.context and record.format != "transcript":
            record.metadata["context_copy"] = self.context
            if not record.parse_error and re.match(r'^[\w_]+":', record.raw):
                record.parse_error = "truncated diagnostic context fragment"
        if record.time_ns is not None and not record.metadata.get("context_copy"):
            self.last_timestamp = record.clock, record.time_ns
        if self.last_timestamp:
            record.metadata["near_clock"], record.metadata["near_time_ns"] = self.last_timestamp

    def parse_line(self, source, line, raw, metadata, anchors, truncated=False):
        primary = parse_record(source, line, raw, truncated)
        self.decorate(primary, metadata, anchors)
        yield primary
        if primary.format == "transcript" and primary.get("@event").startswith("catch.assertion_"):
            for part, (column, data) in enumerate(embedded_objects(primary.raw), 1):
                child = Record(source, line, primary.raw, data, "json", metadata=dict(primary.metadata))
                child.metadata.update({"part": part, "column": column + 1, "context_copy": "Catch INFO"})
                self.decorate(child, metadata, anchors)
                yield child


@dataclass(frozen=True)
class LogFile:
    path: Path
    source: str
    device: int
    inode: int
    size: int
    modified_ns: int
    metadata: dict = field(default_factory=dict, compare=False)
    linked_runs: tuple = field(default=(), compare=False)

    @classmethod
    def capture(cls, path, root):
        resolved = path.resolve(strict=True)
        if not resolved.is_relative_to(root):
            raise QueryError(f"log input escapes repository mount: {path}")
        if not resolved.is_file():
            raise QueryError(f"not a regular log file: {path}")
        info = resolved.stat()
        return cls(resolved, str(resolved.relative_to(root)), info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns)

    def records(self, unchanged=False, anchors=None, line_hint=None):
        context = TranscriptContext()
        messages = VulkanMessages()
        metadata = {"mtime_ns": self.modified_ns, "run": self.source, "artifact": Path(self.source).stem,
                    **self.metadata}
        anchors = anchors or {}

        def parse_message(first, last, text, error):
            if line_hint is not None and not line_hint(text) and not (
                ANCHOR_HINT.search(text) or TEST_STATUS.match(text)
                or text.startswith(("Filters:", "workspace-wayland:"))
            ):
                return
            for row in context.parse_line(self.source, first, text, metadata, anchors):
                row.parse_error = error or row.parse_error
                if last != first:
                    row.metadata["line_end"] = last
                yield row

        with self.path.open("rb") as stream:
            info = os.fstat(stream.fileno())
            if (info.st_dev, info.st_ino) != (self.device, self.inode) or info.st_size < self.size:
                raise QueryError(f"log was replaced or truncated during query: {self.source}")
            if unchanged and (info.st_size, info.st_mtime_ns) != (self.size, self.modified_ns):
                raise QueryError(f"log changed during correlation query; retry a completed capture: {self.source}")
            remaining = self.size
            line = 0
            while remaining:
                part = stream.readline(min(remaining, MAX_LINE_BYTES + 1))
                if not part:
                    raise QueryError(f"log was truncated during query: {self.source}")
                remaining -= len(part)
                line += 1
                truncated = len(part) > MAX_LINE_BYTES
                raw = part[:MAX_LINE_BYTES]
                while truncated and not part.endswith(b"\n") and remaining:
                    part = stream.readline(min(remaining, MAX_LINE_BYTES))
                    remaining -= len(part)
                    if not part:
                        raise QueryError(f"log was truncated during query: {self.source}")
                error = "line exceeds byte limit; text is truncated" if truncated else ""
                try:
                    text = raw.decode("utf-8")
                except UnicodeDecodeError:
                    text = raw.decode("utf-8", errors="replace")
                    error = error or "invalid UTF-8 bytes replaced"
                for message in messages.push(line, text, error):
                    yield from parse_message(*message)
            for message in messages.finish():
                yield from parse_message(*message)
            if unchanged:
                final = os.fstat(stream.fileno())
                if (final.st_size, final.st_mtime_ns) != (self.size, self.modified_ns):
                    raise QueryError(f"log changed during correlation query; retry a completed capture: {self.source}")


MOZILLA_ARTIFACT = re.compile(
    r"^(.*)-mozilla-(?:main\.[0-9]+\.log|child\.[0-9]+\.log(?:\.child-[0-9]+)+)\.moz_log(?:\.[0-3])?$"
)


APPLICATION_ARTIFACT = re.compile(r"^(.*)-application(?:\.[1-5])?\.log$")
FAMILY_SUFFIXES = (".jsonl", ".log", "-native.log", "-firefox.log", "-acceptance.jsonl", "-application.log")


def is_log_path(path, suffixes=(".jsonl", ".log", ".out", ".txt")):
    return path.suffix in suffixes or bool(MOZILLA_ARTIFACT.fullmatch(path.name))


def directory_logs(directory, suffixes=(".jsonl", ".log", ".out", ".txt")):
    """Stream names and reuse directory-entry stat data even in large histories."""
    with os.scandir(directory) as entries:
        for entry in entries:
            if (entry.name.endswith(suffixes) or MOZILLA_ARTIFACT.fullmatch(entry.name)) and entry.is_file():
                yield Path(entry.path)


def normalize_input(supplied, root, host_root=None):
    pattern = str(supplied)
    if host_root and (pattern == host_root or pattern.startswith(host_root + "/")):
        pattern = str(root) + pattern[len(host_root):]
    return pattern if os.path.isabs(pattern) else str(root / pattern)


def discover_files(inputs, root, recursive=False, host_root=None, captured=None, complete_histories=None, directories=None):
    selected = {}
    patterns = set()
    directories = {} if directories is None else directories
    candidates_seen = set()
    for supplied in inputs or ["build/validation"]:
        pattern = normalize_input(supplied, root, host_root)
        if pattern in patterns:
            continue
        patterns.add(pattern)
        paths = [Path(path) for path in sorted(glob.glob(pattern, recursive=recursive))]
        if not paths:
            raise QueryError(f"input path or glob matched no files: {supplied}")
        found = False
        for path in paths:
            resolved = path.resolve()
            if not resolved.is_relative_to(root):
                raise QueryError(f"log input escapes repository mount: {path}")
            if path.is_dir():
                if resolved in directories:
                    found |= directories[resolved]
                    continue
                directory_found = False
                candidates = (
                    (item for item in path.rglob("*") if is_log_path(item) and item.is_file())
                    if recursive else directory_logs(path)
                )
            else:
                directory_found = None
                candidates = (path,)
            for candidate in candidates:
                resolved_candidate = candidate.resolve()
                if resolved_candidate not in candidates_seen:
                    item = captured.get(resolved_candidate) if captured is not None else None
                    if item is None:
                        item = LogFile.capture(candidate, root)
                        if captured is not None:
                            captured[item.path] = item
                    selected[item.path] = item
                    candidates_seen.add(resolved_candidate)
                found = True
                if directory_found is not None:
                    directory_found = True
            if directory_found is not None:
                directories[resolved] = directory_found
                if complete_histories is not None and resolved.name.endswith(".history"):
                    complete_histories.add(resolved)
        if not found:
            raise QueryError(f"input contains no .jsonl, .log, .out, .txt, or Mozilla process logs: {supplied}")
    return sorted(selected.values(), key=lambda item: item.source)


def artifact_family(path):
    archived = path.parent.name.endswith(".history")
    original = path.parent.parent / path.parent.name.removesuffix(".history") if archived else path
    process = MOZILLA_ARTIFACT.fullmatch(original.name) or APPLICATION_ARTIFACT.fullmatch(original.name)
    stem = process[1] if process else re.sub(r"-(?:native|firefox|acceptance|application)$", "", original.stem)
    return original.parent / stem, original, archived


def normalize_family(family, root, host_root):
    supplied = Path(family)
    if not supplied.is_absolute() and supplied.parent == Path("."):
        supplied = Path("build/validation") / supplied
    supplied = Path(normalize_input(supplied, root, host_root))
    if is_log_path(supplied):
        supplied = artifact_family(supplied)[0]
    # Normalize the parent, preserving the selected artifact name for role matching.
    return supplied.parent.resolve() / supplied.name


def rotation_identity(identity):
    match = ROTATION_NAME.fullmatch(identity)
    return (match[1], int(match[2])) if match else None


class ArtifactCatalog:
    """Discover capture siblings and qualify inferred archive pairings explicitly."""

    def __init__(self, options, root):
        host_root = os.environ.get("MMLTK_LOG_HOST_ROOT")
        captured = {}
        complete_histories = set()
        directories = {}
        self.files = []
        if options.paths or not options.family:
            self.files = discover_files(options.paths, root, options.recursive, host_root,
                                        captured, complete_histories, directories)
        requested_families = {normalize_family(family, root, host_root) for family in options.family}
        families = set(requested_families)
        if options.history or options.run or options.triage:
            families.update(artifact_family(source.path)[0] for source in self.files)
        self.root = root
        self.inventory = {}
        self.candidate_count = 0
        self.discover_families(families, root)
        inputs = []
        for family in sorted(requested_families):
            siblings = self.family_paths(family)
            if not siblings and (options.history or options.run):
                siblings += [Path(str(path) + ".history") for path in self.family_paths(family, existing=False)
                             if Path(str(path) + ".history").is_dir()]
            if not siblings:
                raise QueryError(f"no artifact family found: {family}")
            inputs.extend(str(path) for path in siblings)
        if inputs:
            discover_files(inputs, root, options.recursive, host_root, captured, complete_histories, directories)
            self.files = sorted(captured.values(), key=lambda item: item.source)
        if options.history or options.run:
            archives = set()
            families = set()
            for source in self.files:
                family, original, archived = artifact_family(source.path)
                if archived or options.family:
                    if family in families:
                        continue
                    families.add(family)
                    siblings = self.family_paths(family, existing=False)
                else:
                    siblings = [original]
                for sibling in siblings:
                    directory = Path(str(sibling) + ".history")
                    if directory not in archives and directory.resolve() not in complete_histories and directory.is_dir():
                        archives.add(directory)
            if archives:
                discover_files([str(path) for path in sorted(archives)], root,
                               captured=captured, complete_histories=complete_histories, directories=directories)
                self.files = sorted(captured.values(), key=lambda item: item.source)
        original_paths = {source.path for source in self.files}
        if options.triage:
            self.discover_siblings(root, complete_histories)
        self.anchors = {}
        self.assign_runs(root)
        original_runs = {source.metadata["run"] for source in self.files if source.path in original_paths}
        # Nearby filenames are candidates until the ordinary rotation grouping
        # confirms they share a requested capture. Never pull in an adjacent run.
        self.files = [source for source in self.files
                      if source.path in original_paths or source.metadata["run"] in original_runs]
        self.selected_runs = set()
        if options.run:
            self.selected_runs = {
                source.metadata["run"] for source in self.files
                if options.run in (source.metadata["run"], source.metadata.get("archive_id"),
                                   source.metadata["run"].rsplit("@", 1)[-1])
            }
            if not self.selected_runs:
                raise QueryError(f"no artifact run matched {options.run!r}; use --list-runs --history")
            selected_families = {
                source.metadata["family"] for source in self.files if source.metadata["run"] in self.selected_runs
            }
            self.files = [
                source for source in self.files
                if source.metadata["run"] in self.selected_runs or source.metadata["family"] not in selected_families
            ]

    def discover_families(self, families, root):
        """Retain bounded process names so resolved symlinks need no parent rescan."""
        parents = set()
        for family in families:
            if not family.is_relative_to(root):
                raise QueryError(f"log input escapes repository mount: {family}")
            parents.add(family.parent)
        for parent in sorted(parents):
            if parent in self.inventory:
                continue
            candidates = self.inventory[parent] = {}
            if not parent.is_dir():
                continue
            with os.scandir(parent) as entries:
                for entry in entries:
                    original = entry.name.removesuffix(".history")
                    match = MOZILLA_ARTIFACT.fullmatch(original) or APPLICATION_ARTIFACT.fullmatch(original)
                    if not match or original == match[1] + "-application.log":
                        continue
                    names = candidates.setdefault(match[1], set())
                    if original not in names:
                        if self.candidate_count >= MAX_DISTINCT_KEYS:
                            raise QueryError("artifact candidate inventory exceeds file budget; narrow the input paths")
                        names.add(original)
                        self.candidate_count += 1

    def family_paths(self, family, existing=True):
        self.discover_families((family,), self.root)
        names = self.inventory[family.parent].get(family.name, ())
        if names and len(names) + len(FAMILY_SUFFIXES) > MAX_DISTINCT_KEYS:
            raise QueryError("artifact family exceeds file budget; narrow the input paths")
        paths = {family.parent / name for name in names} | {Path(str(family) + suffix) for suffix in FAMILY_SUFFIXES}
        return sorted(path for path in paths if not existing or path.is_file())

    def discover_siblings(self, root, complete_histories):
        """Scan each relevant history directory once; match only requested neighborhoods."""
        requests = {}
        selected = {source.path: source for source in self.files}
        original_count = len(selected)

        def add(path, why):
            resolved = path.resolve()
            if resolved not in selected:
                if len(selected) - original_count >= MAX_DISTINCT_KEYS:
                    raise QueryError("automatic artifact discovery exceeds file budget; narrow the input paths")
                captured = LogFile.capture(path, root)
                selected[resolved] = replace(captured, metadata={"discovery": why})

        for source in self.files:
            family, original, archived = artifact_family(source.path)
            if not is_log_path(original):
                continue
            request = requests.setdefault(family, {"current": False, "rotations": {}, "names": set()})
            if not archived:
                request["current"] = True
            elif rotation := rotation_identity(source.path.stem):
                pid, timestamp = rotation
                request["rotations"].setdefault(pid, set()).add(timestamp)
            else:
                request["names"].add(source.path.stem)
        for family, request in sorted(requests.items()):
            rotations = {pid: sorted(times) for pid, times in request["rotations"].items()}
            for original in self.family_paths(family, existing=False):
                if request["current"] and original.is_file():
                    add(original, "current artifact sibling")
                directory = Path(str(original) + ".history")
                if (rotations or request["names"]) and directory.is_dir():
                    resolved = directory.resolve()
                    if not resolved.is_relative_to(root):
                        raise QueryError(f"log input escapes repository mount: {directory}")
                    if resolved in complete_histories:
                        continue
                    for path in directory_logs(directory, (".jsonl" if original.suffix == ".jsonl" else ".log",)):
                        if path.stem in request["names"]:
                            add(path, "matching archive name")
                        elif rotation := rotation_identity(path.stem):
                            pid, timestamp = rotation
                            times = rotations.get(pid, ())
                            index = bisect_left(times, timestamp)
                            if any(abs(times[candidate] - timestamp) <= ROTATION_WINDOW_NS
                                   for candidate in (index - 1, index) if 0 <= candidate < len(times)):
                                add(path, "adjacent rotation (inferred)")
        self.files = sorted(selected.values(), key=lambda source: source.source)

    def assign_runs(self, root):
        groups = {}
        updated = []
        for source in self.files:
            family, original, archived = artifact_family(source.path)
            name = str(family.relative_to(root))
            identity = source.path.stem if archived else "current"
            metadata = {**source.metadata, "family": name, "run": name + "@" + identity,
                        "artifact": original.name, "run_link": "artifact-stem"}
            metadata["role"] = (
                "mozilla" if MOZILLA_ARTIFACT.fullmatch(original.name) else
                "acceptance" if original.name.endswith("-acceptance.jsonl") else
                "application" if APPLICATION_ARTIFACT.fullmatch(original.name) else
                "trace" if original.suffix == ".jsonl" else
                "native" if original.stem.endswith("-native") else
                "firefox" if original.stem.endswith("-firefox") else "transcript"
            )
            if archived:
                metadata["archive_id"] = identity
                if rotation := rotation_identity(identity):
                    pid, timestamp = rotation
                    metadata["rotation_ns"] = timestamp
                    groups.setdefault((name, pid), []).append((timestamp, len(updated)))
            updated.append(replace(source, metadata=metadata))
        for entries in groups.values():
            batch = []
            artifacts = set()
            for timestamp, index in sorted(entries):
                artifact = updated[index].metadata["artifact"]
                if batch and (timestamp - batch[0][0] > ROTATION_WINDOW_NS
                              or artifact in artifacts):
                    self.assign_batch(updated, batch)
                    batch = []
                    artifacts.clear()
                batch.append((timestamp, index))
                artifacts.add(artifact)
            self.assign_batch(updated, batch)
        self.files = updated

    @staticmethod
    def assign_batch(files, batch):
        canonical = files[batch[0][1]]
        for _, index in batch:
            source = files[index]
            metadata = {**source.metadata, "run": canonical.metadata["run"]}
            metadata["run_link"] = "rotation-neighbor-within-10ms" if len(batch) > 1 else "unpaired-history"
            files[index] = replace(source, metadata=metadata)

    def prepare_anchors(self):
        for source in self.files:
            if source.path.suffix != ".jsonl":
                continue
            for row in source.records(unchanged=True, line_hint=ANCHOR_HINT.search):
                event = row.get("@event")
                if row.clock != "steady" or event not in ANCHOR_EVENTS:
                    continue
                key = event, row.time_ns
                previous = self.anchors.get(key)
                if previous is not None and previous["run"] != source.metadata["run"]:
                    raise QueryError("ambiguous shared event timestamp across captures; narrow --family/--run")
                self.anchors[key] = source.metadata
                if len(self.anchors) > MAX_DISTINCT_KEYS:
                    raise QueryError("too many capture anchors; narrow --family/--run")
        # Transcripts need this pass even when their stem matches a native family.
        # Firefox/native siblings need no extra scan to infer test context.
        native_families = {item.metadata["family"] for item in self.files if item.path.suffix == ".jsonl"}
        tests_by_run = {}
        tags_by_run = {}
        runs_by_transcript = {}
        for source in self.files:
            if source.metadata["family"] in native_families and source.metadata["role"] != "transcript":
                continue
            for row in source.records(unchanged=True, anchors=self.anchors, line_hint=ANCHOR_HINT.search):
                test = row.metadata.get("test")
                if row.metadata.get("run_link") == "shared-event-and-steady-time":
                    run = row.metadata["run"]
                    runs_by_transcript.setdefault(source.source, set()).add(run)
                    if test:
                        tests_by_run.setdefault(run, set()).add(test)
                        tags_by_run.setdefault(run, set()).update(row.metadata.get("tags", []))
        self.files = [
            replace(source, linked_runs=tuple(sorted(runs_by_transcript.get(source.source, ()))), metadata={
                **source.metadata,
                **({"test": next(iter(tests_by_run[source.metadata["run"]]))}
                   if len(tests_by_run.get(source.metadata["run"], ())) == 1 else {}),
                **({"tags": sorted(tags_by_run[source.metadata["run"]])}
                   if source.metadata["run"] in tags_by_run else {}),
            })
            for source in self.files
        ]

    def list_runs(self, output, limit):
        grouped = {}
        for source in self.files:
            grouped.setdefault(source.metadata["run"], []).append(source)
        ordered = heapq.nlargest(limit, grouped.items(),
                                 key=lambda item: (max(source.modified_ns for source in item[1]), item[0]))
        for run, files in reversed(ordered):
            print(f"{run} [{files[0].metadata['run_link']}]", file=output)
            for source in sorted(files, key=lambda item: item.source):
                print(f"  {source.source} bytes={source.size} mtime_ns={source.modified_ns}", file=output)
        print(f"Showing {min(len(grouped), limit)}/{len(grouped)} artifact runs; rotation-neighbor links are inferred.", file=output)


class ErrorLookup:
    """Locate a pasted display message, then bound investigation to its first capture."""

    def __init__(self, text, options):
        if not text.strip():
            raise QueryError("--error requires non-empty display text")
        paragraphs = re.split(r"\n\s*\n", text.strip())
        self.phrases = tuple(dict.fromkeys((normalized_text(text), normalized_text(": ".join(paragraphs)))))
        self.words = tuple(dict.fromkeys(re.findall(r"\w+", normalized_text(text))))
        self.options = options
        self.focus = None
        self.matches = 0
        self.mode = ""
        self.expression = None
        self.ends = {}
        self.focus_estimate = None
        self.malformed = 0
        self.artifacts = []

    def hint(self, raw):
        lowered = raw.casefold()
        # Unicode escapes can hide literal text; let the JSON decoder decide.
        return "\\u" in lowered or all(word in lowered for word in self.words)

    @staticmethod
    def physical_key(record):
        return (
            record.metadata.get("role") == "transcript",
            bool(record.metadata.get("context_copy")),
            record.metadata.get("mtime_ns", 0), record.source, record.line,
            record.metadata.get("part", 0),
        )

    def locate(self, files, query, where, anchors):
        word_focus = None
        word_matches = 0
        phrase_key = word_key = None
        for source in files:
            for row in source.records(unchanged=True, anchors=anchors,
                                      line_hint=None if self.options.strict else self.hint):
                self.malformed += bool(row.parse_error)
                if not where.matches(row) or not query.matches(row):
                    continue
                text = record_text(row)
                if any(phrase in text for phrase in self.phrases):
                    self.matches += 1
                    key = self.physical_key(row)
                    if phrase_key is None or key < phrase_key:
                        self.focus, phrase_key = row, key
                elif self.focus is None and self.words and all(word in text for word in self.words):
                    word_matches += 1
                    key = self.physical_key(row)
                    if word_key is None or key < word_key:
                        word_focus, word_key = row, key
        if self.focus is not None:
            self.mode = "exact/display-normalized phrase"
            expression = Expression("phrase", self.phrases)
        elif word_focus is not None:
            self.mode = "all-words fallback"
            self.focus, self.matches = word_focus, word_matches
            expression = Expression("words", self.words)
        else:
            return
        self.expression = Expression("and", (query, expression))

    def relevant(self, source):
        return (
            source.metadata["run"] == self.focus.metadata["run"]
            or source.source == self.focus.source
            or self.focus.metadata["run"] in source.linked_runs
        )

    def calibrate(self, files, anchors):
        self.artifacts = [source.source for source in files if source.metadata["run"] == self.focus.metadata["run"]]
        for source in files:
            if source.metadata["role"] == "transcript" and source.source != self.focus.source:
                continue
            for row in source.records(unchanged=True, anchors=anchors):
                if row.metadata["run"] == self.focus.metadata["run"] and row.time_ns is not None and not row.metadata.get("context_copy"):
                    key = row.source, row.clock
                    self.ends[key] = max(self.ends.get(key, row.time_ns), row.time_ns)
        self.focus_estimate = self.estimate(self.focus, self.focus.clock, self.focus.time_ns)

    def estimate(self, row, clock, timestamp):
        end = self.ends.get((row.source, clock))
        if end is None or timestamp is None:
            return None
        return row.metadata["mtime_ns"] - (end - timestamp)

    def annotate(self, row, force=False):
        if row.metadata["run"] != self.focus.metadata["run"]:
            return False
        clock = row.clock
        timestamp = row.time_ns
        carried = timestamp is None
        if carried:
            clock = row.metadata.get("near_clock")
            timestamp = row.metadata.get("near_time_ns")
        if clock == self.focus.clock and timestamp is not None and self.focus.time_ns is not None:
            delta = timestamp - self.focus.time_ns
            link = "preceding-same-source-timestamp proximity" if carried else "same-clock proximity"
        else:
            estimated = self.estimate(row, clock, timestamp)
            if estimated is not None and self.focus_estimate is not None:
                delta = estimated - self.focus_estimate
                link = "file-mtime proximity"
            elif row.source == self.focus.source and abs(row.line - self.focus.line) <= self.options.context:
                delta = 0
                link = "line-neighborhood proximity"
            elif force:
                delta = 0
                link = "matching-message link; clock unaligned"
            else:
                return False
        if not force and abs(delta) > self.options.near_ms * 1000000:
            return False
        row.metadata.update({"proximity_ns": delta, "time_link": link})
        return True


def field_names(value, separator=","):
    names = tuple(item.strip() for item in value.split(separator))
    if not names or any(not FIELD_NAME.fullmatch(name) for name in names):
        raise QueryError(f"invalid field list: {value!r}")
    return names


def correlation_key(record, names):
    parts = []
    for name in names:
        value = record.get(name)
        if value is MISSING or value is None or isinstance(value, (bool, dict, list)):
            return None
        if value == 0 or value == "" or isinstance(value, str) and re.fullmatch(r"0+", value):
            return None
        parts.append(encoded(value))
    return tuple(parts)


def order_key(record, order):
    position = record.source, record.line, record.metadata.get("part", 0)
    if order == "file":
        return position
    if order == "capture":
        return record.metadata.get("mtime_ns", 0), *position
    if order == "proximity":
        return record.metadata.get("proximity_ns", 2**63), *position
    # Cross-clock order is a deterministic grouping, never a claim of causality.
    rank = {"steady": 0, "wall": 1, "wall-local": 2, "none": 4}.get(record.clock, 3)
    return rank, record.clock, record.time_ns or 0, *position


@dataclass
class Retained:
    key: tuple
    record: Record
    reason: str
    tail: bool
    priority: int = 0
    distance: int = 0

    def __lt__(self, other):
        if self.priority != other.priority:
            return self.priority > other.priority
        if self.distance != other.distance:
            return self.distance > other.distance
        return self.key < other.key if self.tail else self.key > other.key


class QueryResult:
    def __init__(self, options):
        self.options = options
        self.scanned = 0
        self.matches = 0
        self.related = 0
        self.proximity = 0
        self.context = 0
        self.errors = 0
        self.malformed = 0
        self.parse_examples = []
        self.files = Counter()
        self.groups = Counter()
        self.clocks = {}
        self.retained = []
        self.terminals = {}
        self.selected_runs = set()
        self.lookup = None
        self.native_tail = []
        self.automatic = None
        self.recent_query = ([], []) if auto_correlate_enabled(options) else None
        self.representatives = TriagePool(options.limit, retain_full=True) if options.representatives else None

    def inspect(self, record, permitted=True):
        self.scanned += 1
        if record.parse_error:
            self.malformed += 1
            if len(self.parse_examples) < 5:
                self.parse_examples.append(f"{record.source}:{record.line}: {record.parse_error}")
        if permitted and record.get("@terminal"):
            run = record.get("@run")
            code = record.get("@exit_code")
            key = (run, record.get("@event"), encoded(None if code is MISSING else code))
            previous = self.terminals.get(key)
            preference = (bool(record.metadata.get("context_copy")), record.source, record.line)
            if previous is None or preference < (
                bool(previous.metadata.get("context_copy")), previous.source, previous.line
            ):
                self.terminals[key] = record
            if len(self.terminals) > MAX_DISTINCT_KEYS:
                raise QueryError("terminal summary exceeds bounded capacity; narrow inputs")
        if permitted and self.lookup and record.metadata.get("role") in ("trace", "native"):
            event = record.get("@event")
            if isinstance(event, str) and HANDOFF_EVENT.search(event):
                item = Retained(order_key(record, "capture"), record, "run-tail proximity", True)
                if len(self.native_tail) < min(5, self.options.top):
                    heapq.heappush(self.native_tail, item)
                else:
                    heapq.heappushpop(self.native_tail, item)

    def include(self, record, reason):
        if (reason == "query" and self.recent_query is not None
                and not self.options.representatives
                and not record.metadata.get("context_copy") and not record.parse_error):
            recent = self.recent_query[int(auto_specific_query(record))]
            key = order_key(record, "capture")
            if len(recent) < 3 or key > recent[0].key:
                item = Retained(key, triage_snapshot(record), "query", True)
                if len(recent) < 3:
                    heapq.heappush(recent, item)
                else:
                    heapq.heapreplace(recent, item)
        if reason == "context":
            self.context += 1
            group = ("context", *physical_position(record))
        else:
            self.matches += reason == "query"
            self.related += reason in ("correlated", "run")
            self.proximity += reason == "proximity"
            self.selected_runs.add(record.get("@run"))
            self.errors += bool(record.get("@error"))
            self.files[record.source] += 1
            group = tuple(encoded(None if (value := record.get(name)) is MISSING else value) for name in self.options.group_by)
            if group not in self.groups and len(self.groups) == MAX_DISTINCT_KEYS:
                raise QueryError(f"group count exceeds {MAX_DISTINCT_KEYS}; narrow the query or --group-by")
            self.groups[group] += 1
            bounds = self.clocks.setdefault(record.clock, [record.time_ns, record.time_ns])
            if record.time_ns is not None:
                bounds[0] = min(bounds[0], record.time_ns)
                bounds[1] = max(bounds[1], record.time_ns)
        if self.representatives is not None:
            rank = (bool(record.metadata.get("context_copy")), order_key(record, self.options.order))
            self.representatives.add(record, rank, reason, group)
            return
        item = Retained(order_key(record, self.options.order), record, reason, self.options.tail)
        if self.lookup:
            event = record.get("@event")
            item.priority = (
                0 if reason == "query" else
                1 if record.get("@terminal") or isinstance(event, str) and HANDOFF_EVENT.search(event) else 2
            )
            if record.metadata.get("context_copy") or record.metadata.get("role") == "transcript":
                item.priority += 3
            item.distance = abs(record.metadata.get("proximity_ns", 2**63))
        if len(self.retained) < self.options.limit:
            heapq.heappush(self.retained, item)
        else:
            heapq.heappushpop(self.retained, item)

    def rows(self):
        rows = sorted(self.retained, key=lambda item: item.key)
        return self.automatic.interleave(rows) if self.automatic else rows


def auto_correlate_enabled(options):
    return (
        options.auto_correlate is not False
        and (options.auto_correlate is True or options.format == "timeline")
        and bool(options.query.strip() or options.errors)
        and not (options.correlate or options.related_run or options.error is not None or options.triage)
    )


def auto_specific_query(record):
    return not str(record.get("@event")).endswith(("progress", "phase_advanced"))


def execute(files, query, where, options, anchors=None, lookup=None):
    anchors = anchors or {}
    run_scope = ("@run",) if options.family or options.history or options.run else ()
    relationships = tuple(run_scope + field_names(value, "+") for value in options.correlate)
    seeds = [set() for _ in relationships]
    seed_runs = set()
    two_passes = bool(relationships or options.related_run)
    if two_passes:
        for source in files:
            for record in source.records(unchanged=True, anchors=anchors):
                if where.matches(record) and query.matches(record):
                    if options.related_run:
                        seed_runs.add(record.get("@run"))
                    for names, values in zip(relationships, seeds):
                        key = correlation_key(record, names)
                        if key is not None:
                            values.add(key)
                            if len(values) > MAX_DISTINCT_KEYS:
                                raise QueryError(f"correlation seeds exceed {MAX_DISTINCT_KEYS}; narrow --query or --where")
    result = QueryResult(options)
    result.lookup = lookup
    automatic = auto_correlate_enabled(options)
    for source in files:
        before = deque(maxlen=options.context)
        remaining_context = 0
        last_included = (0, 0)
        for record in source.records(unchanged=two_passes or bool(anchors), anchors=anchors):
            permitted = where.matches(record)
            result.inspect(record, permitted)
            reason = ""
            if permitted:
                if query.matches(record):
                    if lookup is None or record.metadata["run"] == lookup.focus.metadata["run"]:
                        reason = "query"
                        if lookup:
                            lookup.annotate(record, force=True)
                elif any(correlation_key(record, names) in values for names, values in zip(relationships, seeds)):
                    reason = "correlated"
                elif lookup and lookup.annotate(record):
                    reason = "proximity"
                elif record.get("@run") in seed_runs:
                    reason = "run"
            if reason:
                for previous in before:
                    position = previous.line, previous.metadata.get("part", 0)
                    if position > last_included and where.matches(previous):
                        result.include(previous, "context")
                        last_included = position
                # Every buffered row has now been considered for preceding context.
                # Clearing also avoids revisiting O(context) old rows at every match.
                before.clear()
                result.include(record, reason)
                last_included = record.line, record.metadata.get("part", 0)
                remaining_context = options.context
            elif remaining_context:
                if permitted:
                    result.include(record, "context")
                    last_included = record.line, record.metadata.get("part", 0)
                remaining_context -= 1
            before.append(record)
    # Representative selection precedes enrichment, so repeated diagnostics
    # do not consume the space reserved for their direct provenance.
    if result.representatives is not None:
        for _, record, reason in result.representatives.rows():
            if reason != "context":
                group = tuple(encoded(None if (value := record.get(name)) is MISSING else value)
                              for name in options.group_by)
                record.metadata["representative_count"] = result.groups[group]
            result.retained.append(Retained(order_key(record, options.order), record, reason, False))
    if automatic:
        result.automatic = AutoCorrelation(result)
        result.automatic.collect(files, query, where, anchors)
    return result


def physical_position(record):
    return record.source, record.line, record.metadata.get("part", 0)


def triage_snapshot(record):
    """Bound retained payloads too, not just row counts. Matching uses the full row."""
    budget = [96]
    shortened = [False]

    def trim(value, depth=0):
        if isinstance(value, str):
            if len(value) > MAX_TRIAGE_TEXT:
                shortened[0] = True
                return value[:MAX_TRIAGE_TEXT] + "…"
            return value
        if isinstance(value, (dict, list)):
            if depth >= 4 or budget[0] <= 0:
                shortened[0] = True
                return "<triage payload omitted>"
            if isinstance(value, dict):
                # A large earlier array/context must not consume the budget
                # before the same record's event, reason and exact identities.
                # Inspect at most the remaining budget plus one dictionary key.
                items = list(islice(value.items(), budget[0] + 1))
                items.sort(key=lambda item: isinstance(item[1], (dict, list)))
            else:
                items = enumerate(value)
            result = {}
            for name, child in items:
                if budget[0] <= 0:
                    shortened[0] = True
                    break
                budget[0] -= 1
                result[trim(name) if isinstance(name, str) else name] = trim(child, depth + 1)
            return result if isinstance(value, dict) else list(result.values())
        return value

    raw = trim(record.raw)
    data = trim(record.data)
    result = replace(record, raw=raw, data=data, metadata=dict(record.metadata))
    # Retention must not change a clock, even when a verbose payload was shortened.
    result.clock, result.time_ns = record.clock, record.time_ns
    if shortened[0]:
        result.metadata["triage_payload_truncated"] = True
    return result


@dataclass(frozen=True)
class Identity:
    run: str
    names: tuple
    values: tuple

    def label(self):
        return compact("+".join(self.names), 128) + "=" + "+".join(compact(json.loads(value), 80) for value in self.values)


def strong_identities(record, explicit=()):
    """Conservative namespaces; counters and ambient device/process IDs do not fan out."""
    result = []
    run = str(record.metadata.get("run", record.source))

    def add(names, values=None):
        key = values if values is not None else correlation_key(record, names)
        if key is not None and all(
            not isinstance(value := json.loads(part), str) or bool(value.strip()) for part in key
        ):
            identity = Identity(run, names, key)
            if identity not in result:
                result.append(identity)

    add(("@surface",))
    add(("@workspace_source",))
    add(("@workspace_source", "transfer_sequence"))
    for names in explicit:
        add(names)
    # Source instance numbers are only meaningful within a source session.
    add(("source_session", "source_instance"))
    fields = record.data.get("fields")
    values = {**record.data, **(fields if isinstance(fields, dict) else {})}
    for name, value in values.items():
        if len(result) >= 16:
            break
        if len(name) > 128 or not FIELD_NAME.fullmatch(name):
            continue
        if name in AMBIENT_IDS or name in ("surface_high", "surface_low"):
            continue
        if record.get("@event") == "vulkan.validation" and name in ("validation_id", "message_id"):
            # A validation rule/message hash classifies reports; it does not
            # identify an operation, process, or physical Vulkan object.
            continue
        if isinstance(value, str) and len(value) > 128:
            continue
        if isinstance(value, str) and TYPED_ID.fullmatch(value):
            match = TYPED_ID.fullmatch(value)
            if match[1] == "DeviceId":
                continue
            add((match[1],), (encoded(match[1] + "(" + match[2].replace(" ", "") + ")"),))
        elif name.endswith(("_id", "_identity")):
            canonical = name.removeprefix("parent_")
            key = correlation_key(record, (name,))
            if key is not None:
                add((canonical,), key)
        elif name.endswith("_surface") and isinstance(value, str) and re.fullmatch(r"[0-9a-fA-F]{32}", value):
            if int(value, 16):
                add(("@surface",), (encoded(value.lower()),))
    # Handles also occur in unstructured Firefox messages and Catch context.
    for match in TYPED_ID.finditer(record.raw[:MAX_TRIAGE_TEXT]):
        if len(result) >= 16:
            break
        if match[1] != "DeviceId":
            add((match[1],), (encoded(match[1] + "(" + match[2].replace(" ", "") + ")"),))
    return tuple(result)


def event_stage(record):
    event = record.get("@event")
    match = STAGE_SUFFIX.fullmatch(event) if isinstance(event, str) else None
    return (match[1], match[2].lower()) if match else ("", "")


def triage_terminal(record):
    return record.get("@terminal") or record.get("terminal") is True or event_stage(record)[1] == "terminal"


def anchor_rank(record, identities=()):
    family, stage = event_stage(record)
    event = record.get("@event")
    exit_code = record.get("@exit_code")
    failed_outcome = any(
        isinstance(value := record.get(name), str) and value.lower() in FAILED_STAGES
        for name in ("span_outcome", "outcome", "status", "result")
    )
    if failed_outcome or record.get("failed") is True:
        score, why = 110, "failed outcome/span"
    elif event == "catch.assertion_failed" or isinstance(event, str) and "assertion" in event and record.get("@error"):
        score, why = 105, "assertion failure"
    elif stage in FAILED_STAGES or record.get("@level") in ("error", "critical", "fatal", "panic"):
        score, why = 100, "explicit failure"
    elif (type(exit_code) is int and exit_code != 0) or any(
        isinstance(value := record.get(name), str) and FAILURE_WORD.search(value)
        for name in ("message", "detail", "error")
    ):
        score, why = 90, "error/terminal candidate"
    elif record.get("@error"):
        score, why = 50, "failure-related event (not an explicit failure)"
    elif stage in INCOMPLETE_STAGES:
        score, why = (85 if stage in ("missing", "unavailable", "stalled") else 45), "incomplete state"
    elif record.parse_error:
        score, why = 35, "parse failure (may be capture damage)"
    elif triage_terminal(record):
        score, why = 20, "terminal evidence (not necessarily failure)"
    else:
        return 0, ""
    if identities:
        score += 10
    if record.metadata.get("context_copy"):
        score -= 25
    return score, why


@dataclass(frozen=True)
class PoolRank:
    rank: tuple
    insertion: int
    key: object

    def __lt__(self, other):
        # max(dict, key=rank) previously evicted the earliest inserted equal rank.
        return self.rank > other.rank if self.rank != other.rank else self.insertion < other.insertion


class TriagePool:
    """Small, diverse deterministic reservoir; cardinality never follows the input."""

    def __init__(self, limit, retain_full=False):
        self.limit = limit
        self.retain_full = retain_full
        self.entries = {}
        self.discarded = 0
        self._ranks = {}
        self._heap = []
        self._insertion = 0

    def add(self, record, rank, why, key=None):
        key = physical_position(record) if key is None else key
        previous = self.entries.get(key)
        if previous is not None and previous[0] <= rank:
            return
        if previous is None and len(self.entries) == self.limit:
            while self._ranks.get(self._heap[0].key) is not self._heap[0]:
                heapq.heappop(self._heap)
            worst = self._heap[0]
            if worst.rank <= rank:
                self.discarded += 1
                return
            heapq.heappop(self._heap)
            del self.entries[worst.key]
            del self._ranks[worst.key]
            self.discarded += 1
        insertion = self._ranks[key].insertion if previous is not None else self._insertion
        self._insertion += previous is None
        ranked = PoolRank(rank, insertion, key)
        self._ranks[key] = ranked
        heapq.heappush(self._heap, ranked)
        # Stale ranks carry no record payload; compact to keep churn bounded too.
        # Each rebuild pays for at least limit preceding rank improvements.
        if len(self._heap) > 2 * self.limit:
            self._heap = list(self._ranks.values())
            heapq.heapify(self._heap)
        self.entries[key] = rank, record if self.retain_full else triage_snapshot(record), why

    def rows(self):
        return sorted(self.entries.values(), key=lambda item: item[0])


def auto_value(value):
    if type(value) is int and value > 0:
        return encoded(value)
    if (isinstance(value, str) and len(value) <= 128 and value.strip()
            and not re.fullmatch(r"0+(?:[.:-]0+)*", value.strip())):
        return encoded(value)
    return None


def auto_facts(record):
    event = record.get("@event")
    facts = {name: value for name in AUTO_FIELDS if (value := auto_value(record.get(name))) is not None}
    surface = auto_value(record.get("@surface"))
    if surface:
        facts["@surface"] = surface
    if source := auto_value(record.get("@workspace_source")):
        facts["@workspace_source"] = source
    owner = record.get("@owner")
    domain = owner if isinstance(owner, str) and owner in AUTO_SOURCE_DOMAINS else ""
    if event == "iced.gallery.source":
        domain = "explore"
    if isinstance(event, str) and event in AUTO_INTEGRATION_FIELDS:
        domain, fields = AUTO_INTEGRATION_FIELDS[event]
        for external, native in fields.items():
            value = record.get(external)
            # The external f64 reporter writes String(value). Only these known
            # slots accept its integer spelling; generic string IDs stay typed.
            if isinstance(value, str) and re.fullmatch(r"[1-9][0-9]{0,15}", value):
                value = int(value)
            # Ambiguous large values are not identities.
            if type(value) in (int, float) and 0 < value <= 2**53 and int(value) == value:
                facts.setdefault(native, encoded(int(value)))
    if event in ("integration.phase_progress", "integration.phase_advanced"):
        if phase := auto_value(record.get("detail")):
            facts.setdefault("phase", phase)
    if domain:
        facts["domain"] = domain
    if revision := facts.get("source_revision", facts.get("frame_revision")):
        facts["frame"] = revision
    if revision := facts.get("source_observation_revision", facts.get("snapshot_revision")):
        facts["snapshot"] = revision
    run = str(record.metadata.get("run", record.source))
    identities = []

    def add(priority, names, values=None):
        values = tuple(facts.get(name) for name in names) if values is None else values
        if all(value is not None for value in values):
            identities.append((priority, Identity(run, names, values)))

    if isinstance(event, str) and (event == "vulkan.validation" or event in VULKAN_PROVENANCE_EVENTS):
        process = vulkan_process(record)
        if process is not MISSING:
            if process is not None:
                facts["browser_process_id"] = encoded(process)
            if device := vulkan_handle(record.get("vk_device")):
                facts["vk_device"] = encoded(device)
            for name, handle in vulkan_resources(record.data):
                if process is not None:
                    add(0, ("browser_process_id", name), (encoded(process), encoded(handle)))
                # Rust stderr has no PID. These file-scoped keys are only
                # candidates; collect rejects conflicting observed owners.
                add(1, ("@file", name), (encoded(record.source), encoded(handle)))
    if event in MEMORY_PROVENANCE_EVENTS:
        add(0, ("native_process_id", "workspace_allocation"))
        add(0, ("browser_process_id", "@workspace_source", "workspace_allocation"))
    for field in ("frame", "snapshot"):
        add(0, ("source_session", "source_instance", field))
        if domain and field in facts:
            add(1, (domain + "." + field,), (facts[field],))
    for field in ("presentation_revision", "allocation_generation"):
        add(0, ("@surface", field))
    for field in ("transfer_sequence", "timeline_ready", "workspace_allocation"):
        add(0, ("@workspace_source", field))
    for field in ("span_id", "request_id", "operation_id", "presentation_revision"):
        add(2, (field,))
    if "parent_span_id" in facts:
        add(2, ("span_id",), (facts["parent_span_id"],))
    for field in ("trace_id", "@surface", "@workspace_source", "dataset_identity", "gallery_identity"):
        add(3, (field,))
    add(1, ("dataset_identity", "gallery_generation"))
    if domain and "observation_revision" in facts:
        add(1, (domain + ".observation_revision",), (facts["observation_revision"],))
    for field in ("phase", "phase_id"):
        add(4, (field,))
    control = record.get("control")
    if isinstance(control, str) and re.search(r"[./]", control):
        add(5, ("control",))
    return facts, tuple(dict.fromkeys(identities))


def auto_event_rank(record):
    event = record.get("@event")
    if not isinstance(event, str) or AUTO_NOISE.search(event):
        return None
    if event in MEMORY_PROVENANCE_EVENTS or event in VULKAN_PROVENANCE_EVENTS:
        return 2
    if event in AUTO_INTEGRATION_FIELDS or event == "iced.gallery.source":
        return 1
    parts = event.split(".")
    if "snapshot" in parts or parts[-1] in ("published", "publication"):
        return 0
    if parts[-1] in ("state", "source", "observation", "edge"):
        return 1
    stage = event_stage(record)[1]
    if parts[0] == "application" or stage in AUTO_LIFECYCLE_STAGES or parts[-1] in (
        "ready", "started", "completed", "complete", "failed", "failure",
        "created", "imported", "retired", "released", "exited", "signaled", "terminal",
    ):
        return 2
    if event == "integration.phase_advanced":
        return 3
    return None


class AutoAnchorGroup:
    """At most two nearest anchors per identity; no per-record anchor sweep."""

    def __init__(self):
        self.first = None
        self.times = {}
        self.lines = {}

    def add(self, index, row):
        if self.first is None:
            self.first = index
        if row.time_ns is not None:
            self.times.setdefault(row.clock, []).append((row.time_ns, index))
        self.lines.setdefault(row.source, []).append((row.line, index))

    def finish(self):
        for entries in (*self.times.values(), *self.lines.values()):
            entries.sort()

    def nearest(self, row):
        entries = self.times.get(row.clock) if row.time_ns is not None else None
        point = row.time_ns
        if not entries:
            entries, point = self.lines.get(row.source), row.line
        if not entries:
            return (self.first,)
        index = bisect_left(entries, (point, -1))
        return tuple(entries[candidate][1] for candidate in (index - 1, index)
                     if 0 <= candidate < len(entries))


class AutoCorrelation:
    """One direct, bounded enrichment pass over retained exact-query anchors."""

    def __init__(self, result):
        self.options = result.options
        self.limit = min(MAX_AUTO_RELATED, max(0, self.options.limit - len(result.retained)))
        matches = [item for item in result.retained if item.reason == "query"
                   and not item.record.metadata.get("context_copy") and not item.record.parse_error]
        specific = [item for item in matches if auto_specific_query(item.record)]
        recency = lambda item: order_key(item.record, "capture")
        preferred = heapq.nlargest(MAX_AUTO_ANCHORS, specific, key=recency)
        positions = {physical_position(item.record) for item in preferred}
        remaining = MAX_AUTO_ANCHORS - len(preferred)
        if remaining:
            preferred.extend(heapq.nlargest(
                remaining, (item for item in matches if physical_position(item.record) not in positions), key=recency,
            ))
        recent = result.recent_query
        self.recent = sorted(recent[1] or recent[0], key=lambda item: item.key)
        self.highlight_recent = not self.options.representatives and (result.matches > 20 or result.matches > len(matches))
        self.omitted_anchors = max(0, len(matches) - len(preferred))
        self.seeds = sorted(preferred, key=lambda item: item.key)
        self.facts = []
        self.groups = {}
        self.pools = [TriagePool(MAX_AUTO_VULKAN_RELATED if item.record.get("@event") == "vulkan.validation"
                                 else MAX_AUTO_PER_ANCHOR) for item in self.seeds]
        self.related = {}
        self.count = 0
        self.bridge_sources = {}
        self.object_owners = {}
        self.ambiguous = set()
        self.existing = {physical_position(item.record) for item in result.retained}
        if not self.limit:
            return
        for index, item in enumerate(self.seeds):
            facts, identities = auto_facts(item.record)
            self.facts.append(facts)
            for priority, identity in identities:
                self.observe_bridge(priority, identity, facts)
                if identity not in self.groups:
                    self.groups[identity] = AutoAnchorGroup()
                self.groups[identity].add(index, item.record)
        for group in self.groups.values():
            group.finish()

    def observe_bridge(self, priority, identity, facts):
        if self.object_identity(identity):
            previous = self.object_owners.setdefault(identity, {})
            for name in ("browser_process_id", "vk_device", "@workspace_source", "workspace_allocation"):
                if name in facts and previous.setdefault(name, facts[name]) != facts[name]:
                    # Reused handles or multiple processes/devices are ambiguous
                    # without a recorded lifetime on the diagnostic itself.
                    self.ambiguous.add(identity)
        if priority == 1 and len(identity.names) == 1 and "domain" in facts:
            source_identity = facts.get("source_session"), facts.get("source_instance")
            if all(source_identity):
                previous = self.bridge_sources.setdefault(identity, source_identity)
                if previous != source_identity:
                    self.ambiguous.add(identity)

    @staticmethod
    def object_identity(identity):
        return (len(identity.names) == 2 and identity.names[0] in ("@file", "browser_process_id")
                and identity.names[1] in VULKAN_RESOURCE_FIELDS.values())

    def link(self, row, facts, priority, identity, index):
        seed = self.seeds[index].record
        original = self.facts[index]
        # A coarse surface/trace/dataset match must not override contradictory
        # recorded source, frame, publication, allocation, or dataset identities.
        for name in ("domain", "@surface", "@workspace_source", "workspace_allocation", "transfer_sequence", "source_session", "source_instance", "frame",
                     "presentation_revision", "allocation_generation", "dataset_identity", "gallery_identity",
                     "browser_process_id", "vk_device"):
            if name in facts and name in original and facts[name] != original[name]:
                return None
        comparable = row.clock == seed.clock and row.time_ns is not None and seed.time_ns is not None
        distance = abs(row.time_ns - seed.time_ns) if comparable else 2**63
        weak = priority >= 4
        if weak:
            if comparable:
                if distance > min(250, self.options.near_ms) * 1000000:
                    return None
                why = identity.label() + " + time proximity"
            elif row.source == seed.source and abs(row.line - seed.line) <= 8:
                distance = abs(row.line - seed.line)
                why = identity.label() + " + line proximity"
            else:
                return None
        else:
            why = identity.label()
            if self.object_identity(identity):
                if row.source == seed.source and not comparable:
                    distance = abs(row.line - seed.line)
                if identity.names[0] == "@file":
                    why = ("same-file handle candidate; process unrecorded: "
                           + identity.names[1] + "=" + json.loads(identity.values[1]))
                else:
                    why += " (exact typed handle and recorded process)"
        return (weak, priority, distance, index), why

    def collect(self, files, query, where, anchors):
        if not self.limit or not self.groups:
            return
        runs = {seed.record.metadata["run"] for seed in self.seeds}
        sources = {seed.record.source for seed in self.seeds}
        for source in files:
            if source.source not in sources and source.metadata["run"] not in runs and not runs.intersection(source.linked_runs):
                continue
            for row in source.records(unchanged=True, anchors=anchors):
                if (row.metadata["run"] not in runs or row.metadata.get("context_copy") or row.parse_error
                        or physical_position(row) in self.existing or not where.matches(row) or query.matches(row)):
                    continue
                event_rank = auto_event_rank(row)
                if event_rank is None:
                    continue
                facts, identities = auto_facts(row)
                best = None
                for priority, identity in identities:
                    group = self.groups.get(identity)
                    if group is None:
                        continue
                    self.observe_bridge(priority, identity, facts)
                    for index in group.nearest(row):
                        linked = self.link(row, facts, priority, identity, index)
                        if linked is not None and (best is None or linked[0] < best[0]):
                            best = *linked, identity, index
                if best is None:
                    continue
                rank, why, identity, index = best
                self.pools[index].add(row, (rank[0], event_rank, *rank[1:], *physical_position(row)),
                                      (identity, why), str(row.get("@event")))
        candidates = sorted((rank, index, row, identity, why)
                            for index, pool in enumerate(self.pools)
                            for rank, row, (identity, why) in pool.rows())
        identities = Counter()
        weak_count = 0
        for rank, index, row, identity, why in candidates:
            if self.count == self.limit:
                break
            identity_limit = MAX_AUTO_VULKAN_RELATED if self.object_identity(identity) else MAX_AUTO_PER_IDENTITY
            if identity in self.ambiguous or identities[identity] == identity_limit:
                continue
            if rank[0] and weak_count == MAX_AUTO_WEAK:
                continue
            weak_count += bool(rank[0])
            identities[identity] += 1
            seed = self.seeds[index].record
            row.metadata.update({"auto_reason": compact(why, 150),
                                 "auto_anchor": {"file": seed.source, "line": seed.line,
                                                 "part": seed.metadata.get("part", 0)}})
            if row.metadata.pop("triage_payload_truncated", False):
                row.metadata["auto_payload_truncated"] = True
            after = (row.clock == seed.clock and row.time_ns is not None and seed.time_ns is not None
                     and row.time_ns > seed.time_ns)
            self.related.setdefault(physical_position(seed), ([], []))[int(after)].append(
                Retained(order_key(row, self.options.order), row, "auto", self.options.tail),
            )
            self.count += 1

    def interleave(self, rows):
        result = []
        for item in rows:
            before, after = self.related.get(physical_position(item.record), ((), ()))
            result.extend(sorted(before, key=lambda row: row.key))
            result.append(item)
            result.extend(sorted(after, key=lambda row: row.key))
        return result

    def highlight(self, output):
        if not self.recent or not self.highlight_recent:
            return
        print("Recent query matches (timeline follows):", file=output)
        for item in self.recent:
            row = item.record
            facts = [f"{name}={compact(value, 70)}" for name in
                     ("reason", "observed_event", "control", "a", "b", "c", "d",
                      "source_revision", "presentation_revision", "detail", "message")
                     if (value := row.get(name)) is not MISSING and value not in ("", None)]
            event = row.get("@event")
            print("  " + compact("(text)" if event is MISSING else event, 80) + " " + compact(" ".join(facts), 240) +
                  f" @ {compact(row.source, 100)}:{row.line}", file=output)


@dataclass
class TriageFinding:
    kind: str
    score: int
    message: str
    record: Record
    identity: Identity | None = None
    other: Record | None = None
    observations: int = 1
    last_position: tuple | None = None


@dataclass
class StageBalance:
    first: Record | None = None
    last_end: Record | None = None
    pending: int = 0
    last_start: str = ""

    def begin(self, record, stage):
        if not self.pending:
            self.first = triage_snapshot(record)
        # requested -> submitted -> started is progress, not three operations.
        if not self.pending or self.last_start == stage:
            self.pending += 1
        self.last_start = stage


@dataclass
class PixelChainSample:
    stages: dict = field(default_factory=dict)
    mailbox_count: int = 0
    mailbox: tuple | None = None
    direct_transfer: tuple | None = None


def lifecycle_identity(identities):
    """Prefer the operation/resource owner, not a handle mentioned beside it."""
    priority = {"span_id": 0, "request_id": 1, "@surface": 2, "trace_id": 4}
    return min(identities, key=lambda item: (
        priority.get(item.names[0], 5 if item.names[0].endswith("Id") else 3), item.names
    )) if identities else None


class IdentityChain:
    """Streaming lifecycle ledger for one selected identity, not a whole-run index."""

    def __init__(self, identity, origin, why, hop):
        self.identity, self.origin, self.why, self.hop = identity, origin, why, hop
        self.count = 0
        self.copies = 0
        self.events = Counter()
        self.owners = Counter()
        self.representatives = TriagePool(16)
        self.states = {}
        self.last_by_source = {}
        self.bounds = {}
        self.gaps = {}
        self.limited = False

    def observe(self, record, triage, balance=True):
        self.count += 1
        event = record.get("@event")
        event = str(event) if event is not MISSING else "(text)"
        owner = record.get("@owner")
        owner = str(owner) if owner is not MISSING else "(unknown)"
        for counts, key in ((self.events, event), (self.owners, owner)):
            if key in counts or len(counts) < MAX_TRIAGE_EVENTS:
                counts[key] += 1
            else:
                self.limited = True
        score, _ = anchor_rank(record)
        self.representatives.add(record, (-score, *physical_position(record)), "identity chain",
                                 (record.source, event))
        if record.metadata.get("context_copy"):
            self.copies += 1
            return
        previous = self.last_by_source.get(record.source)
        row = triage_snapshot(record)
        if previous:
            if previous.get("@owner") is not MISSING and record.get("@owner") is not MISSING \
                    and previous.get("@owner") != record.get("@owner"):
                triage.finding("owner-handoff", 25, "same identity appears under another owner; handoff candidate, not proof",
                               row, self.identity, previous)
            if previous.clock == record.clock and record.time_ns is not None and previous.time_ns is not None:
                delta = record.time_ns - previous.time_ns
                if delta < 0:
                    triage.finding("clock-regression", 65, "timestamp decreases in physical source order; no stall inferred",
                                   row, self.identity, previous)
                elif delta >= triage.options.triage_gap_ms * 1000000:
                    current = self.gaps.get(record.source)
                    if current is None or delta > current[0]:
                        self.gaps[record.source] = delta, previous, row
        if len(self.last_by_source) < 32 or record.source in self.last_by_source:
            self.last_by_source[record.source] = row
            self.bounds.setdefault(record.source, row)
        else:
            self.limited = True
        if not balance:
            return
        family, stage = event_stage(record)
        if not family or stage in INCOMPLETE_STAGES:
            if stage in INCOMPLETE_STAGES:
                triage.finding("incomplete-state", 85, f"explicit {stage} stage; later recovery is possible",
                               row, self.identity)
            return
        # Pair only within the same physical source and exact event family.
        key = record.source, family
        if key not in self.states:
            if len(self.states) == MAX_TRIAGE_EVENTS:
                self.limited = True
                return
            self.states[key] = StageBalance()
        state = self.states[key]
        if stage in START_STAGES:
            state.begin(row, stage)
        else:
            if state.pending:
                state.pending -= 1
                if not state.pending:
                    state.first = None
            elif state.last_end and state.last_end.get("@event") == event:
                triage.finding("duplicate-terminal", 55,
                               f"repeated {event} without an intervening {family} start in this source; may be idempotent",
                               row, self.identity, state.last_end)
            elif not state.last_end:
                triage.finding("unmatched-end", 20,
                               f"{family} end has no captured same-source start (partial capture or another source possible)",
                               row, self.identity)
            state.last_end = row
            if stage in FAILED_STAGES and self.identity.names == ("span_id",):
                triage.finding("failed-span", 100, f"span has explicit {stage} stage", row, self.identity)

    def finish(self, triage):
        for (source, family), state in self.states.items():
            if state.pending:
                counterparts = []
                for shared in strong_identities(state.first, triage.explicit):
                    other = triage.observed_ends.get((source, family, shared))
                    if other is None or other.line < state.first.line:
                        continue
                    end_identities = strong_identities(other, triage.explicit)
                    if lifecycle_identity(end_identities) == self.identity:
                        continue  # Already reflected in this identity's balance.
                    if any(identity.names == self.identity.names and identity != self.identity for identity in end_identities):
                        continue  # A different span/request/resource is not a handoff.
                    counterparts.append(other)
                if counterparts:
                    other = min(counterparts, key=physical_position)
                    triage.finding("identity-handoff", 45,
                                   f"{family}: an end shares a recorded identity but has another primary identity; "
                                   "handoff/instrumentation mismatch possible", state.first, self.identity, other)
                else:
                    triage.finding("missing-counterpart", 70,
                                   f"{family}: {state.pending} start(s) without a captured same-source end/completion; "
                                   "suffix convention only, not a proven product requirement",
                                   state.first, self.identity)
        for delta, before, after in self.gaps.values():
            triage.finding("gap", 50,
                           f"{delta / 1000000:.3f}ms without this identity in the same source/clock; "
                           "a gap is not proof of a stall", after, self.identity, before)
        if self.limited:
            triage.notes.add("Lifecycle event/source capacity reached; absence findings describe only tracked families.")


class Triage:
    """Rank anchors, corral exact chains with bounded passes, and explain evidence."""

    def __init__(self, options, where, anchors, lookup=None):
        self.options, self.where, self.anchors, self.lookup = options, where, anchors, lookup
        self.explicit = tuple(field_names(value, "+") for value in options.correlate)
        self.seed_pool = TriagePool(max(32, options.triage_anchors * 4))
        self.context_pool = TriagePool(32)
        self.failure_pool = TriagePool(options.triage_anchors)
        self.terminal_pool = TriagePool(4)
        self.source_tails = TriagePool(4)
        self.evidence = TriagePool(options.limit)
        self.selected = []
        self.identities = {}
        self.refinements = {}
        self.refinement_groups = {}
        self.findings = {}
        self.notes = set()
        self.run = ""
        self.scanned = self.scoped = self.related = self.malformed = 0
        self.parse_examples = []
        self.artifacts = []
        self.artifact_discovery = {}
        self.pending = {}
        self.observed_ends = {}
        self.pixel_samples = {}
        self.pixel_sources = {}
        self.pixel_sources_truncated = False
        self.pixel_pending = {}
        self.pixel_pending_count = 0
        self.pixel_stage_count = 0
        self.pixel_divergences = set()

    def records(self, files, apply_where=True):
        for source in files:
            for row in source.records(unchanged=True, anchors=self.anchors):
                if (not apply_where or self.where.matches(row)) and (not self.run or row.metadata["run"] == self.run):
                    yield row

    def finding(self, kind, score, message, record, identity=None, other=None, *, discriminator=None):
        if identity is not None:
            score -= min(15, self.identities[identity].hop * 5)
        if self.lookup and self.nearby(record) is None:
            score -= 25
        position = physical_position(record)
        anchor = self.findings.get(("anchor-failure", position))
        if kind == "explicit-failure" and anchor is not None:
            if anchor.identity is None:
                anchor.identity = identity
            return
        repeated_kind = kind in ("incomplete-state", "explicit-failure", "unmatched-end", "duplicate-terminal",
                                 "owner-handoff", "clock-regression")
        key = (kind, record.source, str(record.get("@event")), identity) if repeated_kind else (kind, position)
        if discriminator is not None:
            key = (*key, discriminator)
        rank = lambda item: (-item.score, *physical_position(item.record), item.kind)
        new_rank = (-score, *position, kind)
        count = 1
        if key in self.findings:
            previous = self.findings[key]
            count = previous.observations + (previous.last_position != position)
            if new_rank >= rank(previous):
                previous.observations, previous.last_position = count, position
                return
        elif len(self.findings) >= 64:
            self.notes.add("Finding inventory capped at 64; lower-ranked observations omitted.")
            worst = max(self.findings, key=lambda item: rank(self.findings[item]))
            if new_rank >= rank(self.findings[worst]):
                return
            del self.findings[worst]
        self.findings[key] = TriageFinding(kind, score, message, triage_snapshot(record), identity,
                                          triage_snapshot(other) if other else None,
                                          observations=count, last_position=position)

    @staticmethod
    def pixel_sample(record, source=None):
        event = record.get("@event")
        boundary = record.get("boundary")
        if event in NATIVE_PIXEL_STAGES:
            stage = (NATIVE_PIXEL_STAGES[event], (record.get("@workspace_source"), record.get("transfer_sequence")))
        elif event in DIRECT_PIXEL_STAGES:
            stage = (DIRECT_PIXEL_STAGES[event], (source, record.get("transfer_sequence")))
        elif event == "firefox.workspace.pixel" and boundary in ("import", "mailbox"):
            stage = (boundary, (source, record.get("transfer_sequence")))
        elif event == "iced.surface.pixel":
            stage = ("sample", None)
        else:
            return None
        surface = record.get("@surface")
        revision = record.get("presentation_revision")
        index = record.get("sample_index")
        sample = tuple(record.get(name) for name in ("sample_x", "sample_y", "sample_rgba"))
        if (not isinstance(surface, str) or type(revision) is not int or revision <= 0 or
                type(index) is not int or not 0 <= index < 25 or
                any(type(value) is not int for value in sample) or
                (stage[0] != "sample" and (not isinstance(stage[1][0], str) or type(stage[1][1]) is not int or stage[1][1] <= 0))):
            return None
        run = str(record.metadata.get("run", record.source))
        return (run, surface, revision, index), stage, sample

    def pixel_divergence(self, kind, message, record, other, publication, emit, optional_edge=None):
        # Optional diagnostic boundaries cannot suppress the existing physical
        # mode comparisons, including when their records arrive later.
        identity = publication if optional_edge is None else (*publication, optional_edge)
        if identity in self.pixel_divergences:
            return
        self.pixel_divergences.add(identity)
        if emit:
            self.finding(kind, 115, message, record, other=other, discriminator=optional_edge)
        else:
            self.seed_pool.add(
                record, (-120, False, -record.metadata["mtime_ns"], *physical_position(record)),
                "deterministic pixel-chain divergence", ("pixel-chain", *identity),
            )

    def observe_pixel_chain(self, record, emit, bridge=None):
        event = record.get("@event")
        if event == "firefox.workspace.direct_memory_pixel":
            # This retired diagnostic read a buffer alias after the image's
            # queue-family acquire. Vulkan makes that alias's contents undefined;
            # completion and markers establish execution, not defined bytes.
            self.notes.add("Historical direct_memory_pixel used an alias invalidated by the image ownership acquire; excluded from pixel comparisons.")
            return
        source = None
        direct_stage = DIRECT_PIXEL_STAGES.get(event)
        direct_probe = direct_stage is not None
        native_stage = NATIVE_PIXEL_STAGES.get(event)
        source_probe = native_stage is not None and native_stage != "native"
        optional_stage = direct_stage if direct_probe else native_stage if source_probe else None
        optional_kind = "direct" if direct_probe else "source-copy" if source_probe else None
        if (event in ("firefox.workspace.direct_memory_alias", "firefox.workspace.direct_memory_status") and
                (record.get("memory_alias_supported") is False or record.get("completion") not in (MISSING, "Ok(true)"))):
            self.notes.add("Optional direct-memory probe reports unsupported or unsettled evidence; inspect its structured reason.")
        if direct_probe and (record.get("completion") != "Ok(true)" or
                             record.get("readback_marker") != 0x4d4d4c54 or
                             record.get("expected_marker") != 0x4d4d4c54):
            self.notes.add(f"Optional {direct_stage} probe lacks successful completion or its readback marker; bytes were not compared.")
            return
        if source_probe and not isinstance(record.get("@workspace_source"), str):
            self.notes.add("Optional native probe lacks an exact source identity; bytes were not compared.")
            return
        if direct_probe or source_probe or event in ("firefox.workspace.frame_forwarded", "firefox.workspace.pixel"):
            run = str(record.metadata.get("run", record.source))
            key = (run, record.get("@surface"), record.get("presentation_revision"), record.get("transfer_sequence"))
            if (not isinstance(key[1], str) or type(key[2]) is not int or key[2] <= 0 or
                    type(key[3]) is not int or key[3] <= 0):
                return
            if event == "firefox.workspace.frame_forwarded":
                source = record.get("@workspace_source")
                if not isinstance(source, str):
                    return
                previous = self.pixel_sources.get(key)
                if previous is not None and previous[0] != source:
                    self.pixel_divergence("pixel-transfer-conflict", "forwarded physical transfer names conflicting native sources",
                                          record, previous[1], key[:3], emit)
                    return
                if previous is None:
                    previous = source, triage_snapshot(record)
                    if len(self.pixel_sources) >= MAX_TRIAGE_PIXEL_SAMPLES:
                        self.pixel_sources_truncated = True
                        self.notes.add("Pixel transfer bridge capacity reached; narrow the capture to compare additional transfers.")
                    else:
                        self.pixel_sources[key] = previous
                for pending in self.pixel_pending.pop(key, ()):
                    self.pixel_pending_count -= 1
                    self.observe_pixel_chain(pending, emit, bridge=previous)
                if record.get("direct_sampling") is True:
                    transfer = source, key[3]
                    for index in range(25):
                        sample_key = (*key[:3], index)
                        if sample_key not in self.pixel_samples:
                            if len(self.pixel_samples) >= MAX_TRIAGE_PIXEL_SAMPLES:
                                self.notes.add("Pixel-chain sample capacity reached; additional direct publications were not compared.")
                                break
                            self.pixel_samples[sample_key] = PixelChainSample()
                        chain = self.pixel_samples[sample_key]
                        if chain.direct_transfer is not None and chain.direct_transfer != transfer:
                            self.pixel_divergence("pixel-transfer-conflict", "direct publication acquired conflicting physical sources",
                                                  record, previous[1], key[:3], emit)
                            break
                        chain.direct_transfer = transfer
                        if any(stage[0] in ("import", "mailbox") for stage in chain.stages):
                            self.pixel_divergence("pixel-mode-conflict", "direct sampling contains a forbidden sample-arena copy receipt",
                                                  record, None, key[:3], emit)
                        for name in ("sample", *DIRECT_PIXEL_STAGES.values(), *NATIVE_PIXEL_STAGES.values()):
                            stage = (name, None if name == "sample" else transfer)
                            sample = chain.stages.get(stage)
                            if sample is not None:
                                self.observe_pixel_chain(sample[1], emit, bridge=previous)
                return
            if bridge is None:
                bridge = self.pixel_sources.get(key)
            if bridge is None:
                if self.pixel_pending_count < MAX_TRIAGE_PIXEL_SAMPLES:
                    self.pixel_pending.setdefault(key, []).append(triage_snapshot(record))
                    self.pixel_pending_count += 1
                else:
                    self.notes.add("Unbridged pixel sample capacity reached; no native transfer inferred from proximity.")
                return
            source, forwarded = bridge
            declared = record.get("@workspace_source")
            corroborating = ("workspace_allocation", "timeline_ready") if source_probe else (
                "generation", "slot", "frame_id", "session_epoch", "layer_generation", "content_width", "content_height")
            if direct_probe:
                corroborating += ("layer", "content_session", "content_sequence", "timeline_ready", "timeline_release") + DIRECT_ALLOCATION_FIELDS
            conflict = "@workspace_source" if declared is not MISSING and declared != source else None
            for field in corroborating:
                stated, acquired = record.get(field), forwarded.get(field)
                if stated is not MISSING and acquired is not MISSING and (
                        stated != acquired or source_probe and type(stated) is not type(acquired)):
                    conflict = field
                    break
            if conflict:
                self.pixel_divergence(f"pixel-{optional_kind}-transfer-conflict" if optional_kind else "pixel-transfer-conflict",
                                      f"pixel sample conflicts with its exact forwarded transfer ({conflict})",
                                      record, forwarded, key[:3], emit,
                                      optional_edge=f"{optional_stage}-bridge" if optional_stage else None)
                return
            if direct_probe and forwarded.get("direct_sampling") is not True:
                self.notes.add(f"Optional {direct_stage} probe has no exact direct-mode acquisition; bytes were not compared.")
                return
        parsed = self.pixel_sample(record, source)
        if parsed is None:
            return
        key, stage, sample = parsed
        publication = key[:3]
        if key not in self.pixel_samples:
            if len(self.pixel_samples) == MAX_TRIAGE_PIXEL_SAMPLES:
                self.notes.add("Pixel-chain sample capacity reached; additional publications were not compared.")
                return
            self.pixel_samples[key] = PixelChainSample()
        chain = self.pixel_samples[key]
        stages = chain.stages
        previous = stages.get(stage)
        if previous is None:
            if self.pixel_stage_count >= MAX_TRIAGE_PIXEL_SAMPLES * 4:
                self.notes.add("Pixel-chain stage capacity reached; additional physical transfers were not compared.")
                return
            self.pixel_stage_count += 1
        if previous is not None and previous[0] != sample:
            self.pixel_divergence(
                f"pixel-{optional_kind}-owner-mutation" if optional_kind else "pixel-owner-mutation",
                f"{stage[0]} sample changed within surface={key[1]} presentation_revision={key[2]} "
                f"sample_index={key[3]}: {previous[0]} != {sample}",
                record, previous[1], publication, emit,
                optional_edge=f"{optional_stage}-mutation" if optional_stage else None,
            )
        else:
            if previous is None and stage[0] == "mailbox":
                chain.mailbox_count += 1
                chain.mailbox = stage
            stages[stage] = sample, triage_snapshot(record)

        def compare(left, right, optional=None):
            before, after = stages.get(left), stages.get(right)
            if before is None or after is None:
                return
            if optional == "source-copy":
                if any(not isinstance(item[1].get("@workspace_source"), str) for item in (before, after)):
                    self.notes.add("Optional source-copy comparison lacks an exact source identity; bytes were not compared.")
                    return
                conflict = "sample_x/sample_y" if before[0][:2] != after[0][:2] else None
                for field in ("@workspace_source", "transfer_sequence", "workspace_allocation", "timeline_ready"):
                    before_fact, after_fact = before[1].get(field), after[1].get(field)
                    if before_fact is not MISSING and after_fact is not MISSING and (
                            type(before_fact) is not type(after_fact) or before_fact != after_fact):
                        conflict = field
                        break
                if conflict:
                    self.pixel_divergence(
                        "pixel-source-copy-transfer-conflict",
                        f"{left[0]} -> {right[0]} identities or coordinates conflict ({conflict}) for surface={key[1]} "
                        f"presentation_revision={key[2]} sample_index={key[3]}; bytes were not compared",
                        after[1], before[1], publication, emit, optional_edge=(left[0], right[0]),
                    )
                    return
            if before[0] == after[0]:
                return
            self.pixel_divergence(
                f"pixel-{optional}-boundary-divergence" if optional else "pixel-chain-divergence",
                f"{left[0]} -> {right[0]} sample differs for surface={key[1]} "
                f"presentation_revision={key[2]} sample_index={key[3]}: {before[0]} != {after[0]}",
                after[1], before[1], publication, emit,
                optional_edge=(left[0], right[0]) if optional else None,
            )

        # Only edges adjacent to this stage can have changed. Rechecking every
        # transfer makes repeated transfers of one publication quadratic.
        name, transfer = stage
        for copied, kernel in (("source_copy_before_ready", "native"), ("source_copy_after_release", "native_after_release")):
            if name in (copied, kernel):
                compare((kernel, transfer), (copied, transfer), optional="source-copy")
            if chain.direct_transfer is not None:
                if name in (copied, "direct_image"):
                    compare((copied, chain.direct_transfer), ("direct_image", chain.direct_transfer), optional="source-copy")
                if name in (copied, "sample"):
                    compare((copied, chain.direct_transfer), ("sample", None), optional="source-copy")
        if name in ("source_copy_before_ready", "source_copy_after_release"):
            compare(("source_copy_before_ready", transfer), ("source_copy_after_release", transfer), optional="source-copy")
        if chain.direct_transfer is not None:
            if name in ("import", "mailbox"):
                self.pixel_divergence("pixel-mode-conflict", "direct sampling contains a forbidden sample-arena copy receipt",
                                      record, None, publication, emit)
            elif name in ("native", "sample"):
                compare(("native", chain.direct_transfer), ("sample", None))
            if name in ("native", "direct_image"):
                compare(("native", chain.direct_transfer), ("direct_image", chain.direct_transfer), optional="direct")
            if name in ("direct_image", "sample"):
                compare(("direct_image", chain.direct_transfer), ("sample", None), optional="direct")
            return
        if name in ("native", "import"):
            compare(("native", transfer), ("import", transfer))
        if name in ("import", "mailbox"):
            compare(("import", transfer), ("mailbox", transfer))
        if chain.mailbox_count == 1 and name in ("mailbox", "sample"):
            compare(chain.mailbox, ("sample", None))

    def finalize_pixel_chain(self, emit):
        """Resolve absence only after every applicable bridge could be read."""
        kind = "pixel-transfer-incomplete" if self.pixel_sources_truncated else "pixel-transfer-missing"
        reason = ("exact frame_forwarded correlation incomplete because bridge index was truncated"
                  if self.pixel_sources_truncated else "no exact frame_forwarded bridge in scanned scope")
        for key, pending in self.pixel_pending.items():
            pending = [row for row in pending if row.get("@event") not in DIRECT_PIXEL_STAGES and
                       NATIVE_PIXEL_STAGES.get(row.get("@event")) in (None, "native")]
            if not pending:
                self.notes.add("Optional pixel probe has no exact frame_forwarded bridge; bytes were not compared.")
                continue
            run, surface, revision, transfer = key
            representative = min(pending, key=physical_position)
            message = (f"{reason}: surface={surface} "
                       f"presentation_revision={revision} transfer_sequence={transfer} run={run}; "
                       f"{len(pending)} pixel receipt(s) have no proven native source")
            if emit:
                self.finding(kind, 115, message, representative)
            else:
                self.seed_pool.add(
                    representative, (-120, False, -representative.metadata["mtime_ns"],
                                     *physical_position(representative)),
                    kind + ": " + message, (kind, *key),
                )
        self.pixel_pending.clear()
        self.pixel_pending_count = 0

    def choose_anchors(self, files, query):
        explicit = bool(self.options.query.strip() or self.options.errors or self.lookup)
        peer_scope = peer_opened = last_peer_close = None
        short_peers = 0
        stage_scope = None
        workspace_waits = {}

        def retain_pending():
            for state in self.pending.values():
                row = state.first
                span = row.get("span_id")
                exact_span = type(span) is int and span > 0 and row.get("span_outcome") == 0
                why = ("unclosed instrumented span at capture end (candidate, not proof)" if exact_span else
                       "unclosed start at capture end (candidate, not proof)")
                self.seed_pool.add(row, (-80 if exact_span else -30, False, -row.metadata["mtime_ns"], *physical_position(row)),
                                   why, (row.metadata["run"], str(row.get("@event")), "unclosed"))
            for row in workspace_waits.values():
                why = "workspace wait without a captured frame edge: " + str(row.get("message"))
                self.seed_pool.add(row, (-85, False, -row.metadata["mtime_ns"], *physical_position(row)),
                                   why, (row.metadata["run"], row.source, "workspace-wait"))
            self.pending.clear()
            workspace_waits.clear()

        for row in self.records(files, apply_where=False):
            self.scanned += 1
            scope = row.source, row.metadata["run"]
            if scope != stage_scope:
                retain_pending()
                stage_scope = scope
            if row.parse_error:
                self.malformed += 1
                if len(self.parse_examples) < 5:
                    self.parse_examples.append(f"{row.source}:{row.line}: {row.parse_error}")
            if not self.where.matches(row) or not query.matches(row):
                continue
            # Records arrive in physical file order. Keep one source-local
            # lifetime, not a growing peer registry or a cross-clock join.
            scope = row.source, row.metadata["run"], row.clock
            if scope != peer_scope:
                peer_scope, peer_opened, last_peer_close, short_peers = scope, None, None, 0
            event = row.get("@event")
            if not row.metadata.get("context_copy"):
                product = correlation_key(row, ("source_session", "source_instance", "source_revision"))
                if product is not None:
                    if event == "presentation.frame.edge":
                        workspace_waits.pop(product, None)
                    elif event == "presentation.workspace.wait" and row.get("message") == "producer_finalization_pending":
                        if product in workspace_waits or len(workspace_waits) < MAX_TRIAGE_STATES:
                            workspace_waits[product] = triage_snapshot(row)
            if not row.metadata.get("context_copy") and event in (
                "browser.server.peer_opened", "browser.server.peer_closed", "browser.server.peer_replaced"
            ):
                peer = row.get("value")
                if event == "browser.server.peer_opened" and type(peer) is int and peer > 0 and row.time_ns is not None:
                    peer_opened = peer, row.time_ns
                elif event == "browser.server.peer_closed" and peer_opened is not None and row.time_ns is not None:
                    same_peer = type(peer) is int and peer == peer_opened[0]
                    short = same_peer and 0 <= row.time_ns - peer_opened[1] <= 100_000_000
                    consecutive = last_peer_close is not None and 0 <= row.time_ns - last_peer_close <= 1_000_000_000
                    short_peers = min(short_peers + 1, 4) if short and consecutive else int(short)
                    last_peer_close, peer_opened = row.time_ns, None
                    if short_peers == 3:
                        why = "rapid reconnects: three peers closed within 100ms each, at intervals under 1s; cause not recorded by these events"
                        self.seed_pool.add(row, (-95, False, -row.metadata["mtime_ns"], *physical_position(row)), why,
                                           (row.metadata["run"], row.source, "rapid-reconnects"))
                else:
                    peer_opened, last_peer_close, short_peers = None, None, 0
            self.observe_pixel_chain(row, emit=False)
            identities = strong_identities(row, self.explicit)
            score, why = anchor_rank(row, identities)
            if not score and explicit:
                score, why = 40, "query-selected anchor"
            if score:
                self.seed_pool.add(row, (-score, bool(row.metadata.get("context_copy")),
                                        -row.metadata["mtime_ns"], *physical_position(row)), why,
                                   (row.metadata["run"], str(row.get("@event")), why))
            # A bounded open-stage ledger supplies a useful anchor even without errors.
            family, stage = event_stage(row)
            if identities and family and not row.metadata.get("context_copy"):
                key = row.source, lifecycle_identity(identities), family
                if stage in START_STAGES:
                    if key in self.pending or len(self.pending) < MAX_TRIAGE_STATES:
                        state = self.pending.setdefault(key, StageBalance())
                        state.begin(row, stage)
                    else:
                        self.notes.add("Open-stage anchor inventory capped; narrow --where to inspect other identities.")
                elif stage and stage not in INCOMPLETE_STAGES and key in self.pending:
                    self.pending[key].pending -= 1
                    if not self.pending[key].pending:
                        del self.pending[key]
        # Explicit predicates may select pixels while excluding their bridge.
        # Their missing joins are judged by the full analysis pass instead.
        if not explicit:
            self.finalize_pixel_chain(emit=False)
        retain_pending()
        candidates = self.seed_pool.rows()
        self.pixel_samples.clear()
        self.pixel_sources.clear()
        self.pixel_sources_truncated = False
        self.pixel_pending.clear()
        self.pixel_pending_count = 0
        self.pixel_stage_count = 0
        self.pixel_divergences.clear()
        if not candidates:
            return
        self.run = (self.lookup.focus if self.lookup else candidates[0][1]).metadata["run"]
        if self.lookup:
            focus = triage_snapshot(self.lookup.focus)
            self.selected.append(((-1000, *physical_position(focus)), focus, "first physical pasted-error match"))
            return
        for entry in candidates:
            if len(self.selected) >= self.options.triage_anchors:
                break
            if entry[1].metadata["run"] == self.run and all(
                physical_position(item[1]) != physical_position(entry[1]) for item in self.selected
            ):
                self.selected.append(entry)
                if len(self.selected) >= self.options.triage_anchors:
                    break

    def nearby(self, row):
        if self.lookup and self.lookup.annotate(row):
            return abs(row.metadata["proximity_ns"]), row.metadata["time_link"]
        best = None
        for _, anchor, _ in self.selected:
            if row.source == anchor.source:
                delta = abs(row.line - anchor.line)
                if delta <= max(3, self.options.context):
                    candidate = delta, "same-source line context (not identity)"
                    best = min(best, candidate) if best else candidate
            clock, time = row.clock, row.time_ns
            anchor_clock, anchor_time = anchor.clock, anchor.time_ns
            carried = time is None or anchor_time is None
            if time is None:
                clock, time = row.metadata.get("near_clock"), row.metadata.get("near_time_ns")
            if anchor_time is None:
                anchor_clock = anchor.metadata.get("near_clock")
                anchor_time = anchor.metadata.get("near_time_ns")
            if clock == anchor_clock and time is not None and anchor_time is not None:
                delta = abs(time - anchor_time)
                if delta <= self.options.near_ms * 1000000:
                    link = "preceding-source timestamp proximity" if carried else "same-clock proximity"
                    candidate = 10 + delta, link + " (not identity)"
                    best = min(best, candidate) if best else candidate
        return best

    def add_identity(self, identity, origin, why, hop):
        if identity in self.identities:
            return
        if len(self.identities) == self.options.triage_identities:
            self.notes.add("Identity budget reached; additional identities were not expanded.")
            return
        self.identities[identity] = IdentityChain(identity, triage_snapshot(origin), why, hop)

    def gather_context(self, files):
        for _, row, why in self.selected:
            if why.startswith("rapid reconnects:"):
                self.finding("rapid-reconnects", 108, why, row)
            if why.startswith("workspace wait without"):
                self.finding("workspace-wait", 108, why, row)
            if anchor_rank(row)[0] >= 85 and (row.get("@error") or why == "failed outcome/span"):
                self.finding("anchor-failure", 115, why, row)
            for identity in strong_identities(row, self.explicit):
                self.add_identity(identity, row, "anchor: " + why, 0)
        for row in self.records(files):
            self.scoped += 1
            score, why = anchor_rank(row)
            self.source_tails.add(row, (row.source, -row.line, -row.metadata.get("part", 0)),
                                  "captured source end (not a terminal unless logged)", row.source)
            if triage_terminal(row):
                self.terminal_pool.add(row, (bool(row.metadata.get("context_copy")), *physical_position(row)),
                                       "process/test terminal evidence" if row.get("@terminal") else
                                       "generic terminal stage (not a process/test exit; numeric outcomes uninterpreted)",
                                       (str(row.get("@event")), str(row.get("@exit_code"))))
            if score >= 90 or triage_terminal(row):
                self.failure_pool.add(row, (-score, bool(row.metadata.get("context_copy")), *physical_position(row)),
                                      "same-run terminal/failure evidence (not identity)",
                                      (str(row.get("@event")), why))
            neighborhood = self.nearby(row)
            if neighborhood is None:
                continue
            distance, link = neighborhood
            # Prefer eventful physical neighbors; diversify repeated hot-loop messages.
            event = row.get("@event")
            detail = value_from(row.data, "detail", "message", "fields.detail", "fields.message")
            signature = row.source, str(event), str(detail)[:100]
            conversational = isinstance(event, str) and HANDOFF_EVENT.search(event)
            rank = (bool(row.metadata.get("context_copy")),
                    self.lookup is not None and row.source != self.lookup.focus.source, event is MISSING,
                    not bool(conversational), distance, *physical_position(row))
            self.context_pool.add(row, rank, link, signature)
        for _, row, why in self.context_pool.rows():
            for identity in strong_identities(row, self.explicit):
                self.add_identity(identity, row, "neighbor seed: " + why, 0)

    def add_refinement(self, row, identity):
        names = []
        for name in REFINEMENT_FIELDS:
            value = row.get(name)
            if type(value) is int and value >= 0 and (name != "sequence" or value > 0):
                names.append(name)
        if len(names) < 2:
            return
        key = Identity(self.run, tuple(names), tuple(encoded(row.get(name)) for name in names))
        if key not in self.refinements:
            if len(self.refinements) == self.options.triage_identities:
                self.notes.add("Counter refinement budget reached; additional composites omitted.")
                return
            self.refinements[key] = []
            self.refinement_groups.setdefault(key.names, {})[key.values] = (
                len(self.refinements), key, self.refinements[key],
            )
        origins = self.refinements[key]
        if len(origins) < 4 and all(previous.source != row.source for previous, _ in origins):
            origins.append((triage_snapshot(row), identity))

    def expand(self, files):
        if not self.identities:
            return
        # Even zero identity hops still discovers nearby counter refinements.
        for hop in range(max(1, self.options.triage_hops)):
            known = set(self.identities)
            additions = TriagePool(self.options.triage_identities)
            for row in self.records(files):
                identities = strong_identities(row, self.explicit)
                matched = [identity for identity in identities if identity in known]
                if not matched:
                    continue
                self.add_refinement(row, matched[0])
                if hop >= self.options.triage_hops:
                    continue
                unknown = [identity for identity in identities if identity not in known]
                if not unknown:
                    continue
                neighborhood = self.nearby(row)
                distance = neighborhood[0] if neighborhood else 2**63
                rank = (self.identities[matched[0]].hop, bool(row.metadata.get("context_copy")), distance,
                        *physical_position(row))
                for identity in unknown:
                    additions.add(row, (*rank, identity.label()), identity, key=identity)
            for _, row, identity in additions.rows():
                self.add_identity(identity, row, "co-occurs on an exact identity record (handoff candidate)", hop + 1)
            if len(self.identities) == len(known):
                break

    def counter_neighbor(self, row):
        # Composites are terminal expansion leaves: never seed another identity hop.
        if not self.refinement_groups:
            return None
        values = {name: encoded(value) if (value := row.get(name)) is not MISSING else None
                  for name in REFINEMENT_FIELDS}
        matches = []
        # There are only four possible two-or-three-field counter schemas.
        for names, group in self.refinement_groups.items():
            candidate = group.get(tuple(values[name] for name in names))
            if candidate is not None:
                matches.append(candidate)
        for _, identity, origins in sorted(matches):
            for origin, strong in origins:
                line_near = row.source == origin.source and abs(row.line - origin.line) <= 100
                time_near = (row.clock != "none" and row.clock == origin.clock and row.time_ns is not None
                             and origin.time_ns is not None
                             and abs(row.time_ns - origin.time_ns) <= self.options.near_ms * 1000000)
                if line_near or time_near:
                    return identity, strong, "line" if line_near else "same-clock"
        return None

    def retain(self, row, priority, why):
        self.evidence.add(row, (priority, *order_key(row, self.options.order)), why)

    def analyze(self, files):
        known = set(self.identities)
        counter_evidence = TriagePool(4)
        for row in self.records(files):
            self.observe_pixel_chain(row, emit=True)
            identities = strong_identities(row, self.explicit)
            matched = [identity for identity in identities if identity in known]
            if matched:
                self.related += 1
                family, stage = event_stage(row)
                if family and stage not in START_STAGES and stage not in INCOMPLETE_STAGES \
                        and not row.metadata.get("context_copy"):
                    snapshot = triage_snapshot(row)
                    for identity in matched:
                        key = row.source, family, identity
                        if key in self.observed_ends or len(self.observed_ends) < MAX_TRIAGE_STATES:
                            self.observed_ends[key] = snapshot
                        else:
                            self.notes.add("Related end-stage inventory capped; counterpart absence may be incomplete.")
                primary = lifecycle_identity(identities)
                for identity in matched:
                    self.identities[identity].observe(row, self, balance=identity == primary)
                score, why = anchor_rank(row)
                if score >= 85:
                    kind = "failed-span" if why == "failed outcome/span" and any(
                        item.names == ("span_id",) for item in matched
                    ) else "explicit-failure" if row.get("@error") or why == "failed outcome/span" else "incomplete-state"
                    self.finding(kind, score, why, row,
                                 next((item for item in matched if item.names == ("span_id",)), matched[0])
                                 if kind == "failed-span" else matched[0])
                self.retain(row, 4, "exact identity: " + matched[0].label())
            else:
                refinement = self.counter_neighbor(row)
                if refinement:
                    identity, strong, link = refinement
                    why = f"counter refinement {identity.label()} + {link} proximity to {strong.label()} (not identity)"
                    counter_evidence.add(row, physical_position(row), why, (row.source, str(row.get("@event"))))
        self.finalize_pixel_chain(emit=True)
        for chain in self.identities.values():
            chain.finish(self)
            if chain.hop:
                self.finding("identity-handoff", 25, chain.why, chain.origin, chain.identity)
        # Prefer lifecycle endpoints and distinct stages to repeated hot-loop rows.
        # Anchors and anomaly evidence win ties.
        for index, chain in enumerate(self.identities.values()):
            for row in (*chain.bounds.values(), *chain.last_by_source.values()):
                self.retain(row, 3 + index / max(1, len(self.identities)), "identity lifecycle endpoint: " + chain.identity.label())
            for _, row, _ in chain.representatives.rows():
                self.retain(row, 4 + index / max(1, len(self.identities)), "identity stage: " + chain.identity.label())
        for finding in sorted(self.findings.values(), key=lambda item: (-item.score, *physical_position(item.record))):
            if finding.score >= 50:
                self.retain(finding.record, 1 + (110 - finding.score) / 100, "finding: " + finding.kind)
                if finding.other:
                    self.retain(finding.other, 2.8, "finding context: " + finding.kind)
        context_slots = 0
        anchor_positions = {physical_position(entry[1]) for entry in self.selected}
        for _, row, why in self.context_pool.rows():
            # Reserve useful context even when old failures on a shared dataset
            # identity would otherwise consume the evidence budget.
            redundant = physical_position(row) in anchor_positions or triage_terminal(row)
            context_budget = min(12, max(1, 2 * self.options.limit // 3)) if self.lookup \
                else min(6, max(1, self.options.limit // 3))
            priority = 0.8 if not redundant and context_slots < context_budget else 2.7
            context_slots += not redundant
            self.retain(row, priority, why)
        for _, row, why in counter_evidence.rows():
            self.retain(row, 2.6, why)
        for _, row, why in self.failure_pool.rows():
            self.retain(row, 1.8, why)
        for _, row, why in self.terminal_pool.rows():
            self.retain(row, 0.7, why)
        for _, row, why in self.selected:
            self.retain(row, 0, "anchor: " + why)

    def run_query(self, files, query):
        self.choose_anchors(files, query)
        if not self.selected:
            return self
        files = [source for source in files if source.metadata["run"] == self.run or self.run in source.linked_runs
                 or any(entry[1].source == source.source for entry in self.selected)]
        self.artifacts = [source.source for source in files]
        self.artifact_discovery = {source.source: source.metadata.get("discovery", "") for source in files}
        self.gather_context(files)
        self.expand(files)
        self.analyze(files)
        if not self.identities:
            self.notes.add("No non-empty strong identity near the anchors; evidence is context/proximity only.")
        if self.seed_pool.discarded or self.context_pool.discarded:
            self.notes.add("Anchor/context samples are bounded and diversified; repeated or lower-ranked rows omitted.")
        return self


def render_triage(result, options, output, diagnostics):
    report = output if options.format == "summary" else diagnostics
    print(f"Triage: {result.scanned} records scanned; {len(result.selected)} anchors; "
          f"{len(result.identities)} identities; {result.related}/{result.scoped} run records in exact chains; "
          f"showing {len(result.evidence.entries)} evidence rows (limit {options.limit}).", file=report)
    if result.selected:
        link = result.selected[0][1].metadata.get("run_link", "")
        print("Run: " + compact(result.run, 200) + (f" [{compact(link)}]" if link else ""), file=report)
        if result.lookup:
            print(f"Error lookup: {result.lookup.mode}; {result.lookup.matches} occurrences.", file=report)
            print("First physical match: " + render_record(
                Retained((), result.lookup.focus, "first physical pasted error", False), options), file=report)
        print("Artifacts: " + " | ".join(
            compact(path, 180) + (" [auto: " + result.artifact_discovery[path] + "]" if result.artifact_discovery[path] else "")
            for path in result.artifacts[:options.top]), file=report)
        print("Anchors (ranked evidence, not causes):", file=report)
        for _, row, why in result.selected:
            print("  " + render_record(Retained((), row, why, False), options), file=report)
    findings = sorted(result.findings.values(), key=lambda item: (-item.score, *physical_position(item.record), item.kind))
    if findings:
        print("Highlights:", file=report)
        for finding in findings[:options.top]:
            identity = " [" + finding.identity.label() + "]" if finding.identity else ""
            repeated = f" ({finding.observations} observations)" if finding.observations > 1 else ""
            print(f"  {finding.kind}{identity}: {compact(finding.message, 230)} "
                  f"@ {compact(finding.record.source, 160)}:{finding.record.line}{repeated}", file=report)
        earliest = {}
        for finding in findings:
            if finding.score < 65 or finding.kind in ("clock-regression",):
                continue
            previous = earliest.get(finding.record.source)
            # Explicit observed divergence outranks absence inferred from suffixes.
            rank = lambda item: (
                result.lookup is not None and result.nearby(item.record) is None,
                item.kind == "missing-counterpart", item.record.line,
            )
            if previous is None or rank(finding) < rank(previous):
                earliest[finding.record.source] = finding
        if earliest:
            print("Earliest divergence candidates per source (physical order; partial-capture hypotheses):", file=report)
            for source, finding in sorted(earliest.items())[:min(3, options.top)]:
                print(f"  {compact(source, 160)}:{finding.record.line}: {finding.kind}; not a root-cause conclusion", file=report)
    else:
        print("Highlights: no deterministic lifecycle anomaly established in the captured evidence.", file=report)
    if result.identities:
        print("Identity lifecycles (same-source order; independent sources are not causally ordered):", file=report)
        for chain in list(result.identities.values())[:options.top]:
            owners = ", ".join(name for name, _ in chain.owners.most_common(6))
            events = ", ".join(f"{name}×{count}" for name, count in chain.events.most_common(8))
            print(f"  {chain.identity.label()} hop={chain.hop}, rows={chain.count}, copies={chain.copies}; "
                  f"seed={compact(chain.why, 120)}", file=report)
            print("    owners: " + compact(owners, 180) + "; events: " + compact(events, 500), file=report)
            ordered = {}
            for _, row, _ in chain.representatives.rows():
                ordered.setdefault(row.source, []).append(row)
            for source, rows in sorted(ordered.items())[:2]:
                sequence = sorted(rows, key=physical_position)[:7]
                stages = " -> ".join(f"{row.line}:{row.get('@event')}" for row in sequence)
                print("    source-order " + compact(source, 130) + ": " + compact(stages, 520) +
                      " (distinct-stage sample)", file=report)
        if len(result.identities) > options.top:
            print(f"  {len(result.identities) - options.top} additional identities analyzed (--top controls display).", file=report)
    if result.selected:
        print("Terminal evidence in this run (stage terminals need not mean run completion; independent row budget):", file=report)
        terminal_rows = result.terminal_pool.rows()
        if not terminal_rows:
            print("  No terminal record captured; source ends alone do not establish success, failure, or timeout.", file=report)
        for _, row, why in (terminal_rows or result.source_tails.rows())[:min(4, options.top)]:
            print("  " + render_record(Retained((), row, why, False), options), file=report)
    if result.refinements:
        print("Nearby counter composites (weaker than identity, not transitive): " +
              " | ".join(identity.label() for identity in list(result.refinements)[:min(5, options.top)]), file=report)
    if options.format == "summary" and result.evidence.entries:
        print("Evidence (each row states why it was included):", file=output)
    for _, row, why in sorted(result.evidence.entries.values(), key=lambda item: order_key(item[1], options.order)):
        row.metadata["triage_reason"] = why
        print(render_record(Retained((), row, why, False), options), file=output)
    for note in sorted(result.notes):
        print("Triage limit: " + note, file=diagnostics)
    if result.malformed:
        print(f"Warning: {result.malformed} malformed/truncated records; parse damage is not itself a product failure.", file=diagnostics)
        for example in result.parse_examples:
            print("  " + compact(example, 250), file=diagnostics)
    print("Triage is heuristic, not a diagnosis: missing stages may be disabled logs, partial captures, "
          "or another source. Only recorded same-source clocks support gaps; proximity does not establish causality. "
          "Copied INFO is excluded from lifecycle balances. Payloads retained by triage are size-bounded.", file=diagnostics)


def compact(value, width=180):
    text = value if isinstance(value, str) else encoded(value)
    text = text.replace("\t", "\\t").replace("\r", "\\r").replace("\n", "\\n")
    text = "".join(character if character.isprintable() else f"\\x{ord(character):02x}" for character in text)
    return text if width is None or len(text) <= width else text[:width - 1] + "…"


def projected(record, names):
    output = {"@file": record.source, "@line": record.line}
    if "line_end" in record.metadata:
        output["@line_end"] = record.metadata["line_end"]
    if "representative_count" in record.metadata:
        output["@representative_count"] = record.metadata["representative_count"]
    if record.metadata.get("part"):
        output["@part"] = record.metadata["part"]
    output.update({name: None if (value := record.get(name)) is MISSING else value for name in names})
    return output


def render_record(item, options):
    record = item.record
    if options.format == "jsonl":
        if options.fields:
            output = projected(record, options.fields)
            output["@match"] = item.reason
            if item.reason == "auto":
                output.update({"@auto_reason": record.metadata["auto_reason"],
                               "@auto_anchor": record.metadata["auto_anchor"]})
        else:
            output = {
                "_log": {
                    "file": record.source, "line": record.line, "format": record.format,
                    "clock": record.clock, "time_ns": record.time_ns, "match": item.reason,
                    "parse_error": record.parse_error or None,
                    **record.metadata,
                },
                "data": record.data, "text": record.raw,
            }
        return encoded(output)
    reason = "auto: " + record.metadata["auto_reason"] if item.reason == "auto" else item.reason
    detail_width = 2048 if record.get("@error") is True else 180
    if options.fields:
        cells = projected(record, options.fields)
        return " ".join(f"{name}={compact(value, None if record.get('@event') == 'vulkan.validation' else detail_width)}"
                        for name, value in cells.items()) + f" [{reason}]"
    clock = "elapsed" if record.clock.startswith("elapsed:") else record.clock
    timestamp = (
        f"{record.time_ns / 1000000:.3f}ms" if clock == "elapsed" else
        str(record.time_ns) if record.time_ns is not None else "-"
    )
    event = record.get("@event")
    owner = record.get("@owner")
    level = record.get("@level")
    label = " ".join(str(value) for value in (owner, level, event) if value is not MISSING)
    detail = value_from(record.data, "message", "fields.message", "detail")
    if detail is MISSING and event is MISSING:
        detail = record.raw
    facts = []
    if "representative_count" in record.metadata:
        facts.append(f"occurrences={record.metadata['representative_count']}")
    if event == "vulkan.validation":
        for name in ("validation_id", "message_id", "api", "objects", "referenced_objects"):
            if (value := record.get(name)) is not MISSING:
                facts.append(f"{name}={compact(value, None)}")
    for name in ("trace_id", "span_id", "@surface", "@workspace_source", "source_session", "source_instance",
                 "source_revision", "frame_revision", "presentation_revision", "allocation_generation",
                 "workspace_allocation", "workspace_plane"):
        if correlation_key(record, (name,)) is not None:
            facts.append(f"{name}={compact(record.get(name), 70)}")
    for name in ("participant", "reason", "observed_event", "observed_file", "observed_line", "audit_context.field"):
        value = record.get(name)
        if value is not MISSING:
            facts.append(f"{name}={compact(value, 150)}")
    if event in NATIVE_PIXEL_STAGES or event in DIRECT_PIXEL_STAGES or event in (
            "firefox.workspace.direct_memory_alias", "firefox.workspace.direct_memory_status", "firefox.workspace.direct_memory_pixel"):
        for name in ("transfer_sequence", "timeline_ready", "memory_alias_supported", "sample_index",
                     "sample_x", "sample_y", "sample_byte_offset", "sample_rgba", "completion", "readback_marker"):
            value = record.get(name)
            if value is not MISSING:
                facts.append(f"{name}={compact(value, 70)}")
        if event == "firefox.workspace.direct_memory_pixel":
            facts.append("pixel_evidence=undefined_alias_after_image_acquire")
    if event in MEMORY_PROVENANCE_EVENTS or event == "firefox.workspace.direct_binding":
        for name in MEMORY_PROVENANCE_FIELDS:
            value = record.get(name)
            if value is not MISSING:
                facts.append(f"{name}={compact(value, 100)}")
        if event == "firefox.workspace.direct_binding":
            for name in ("phase", "storage_image", "chosen_layout", "transfer_sequence", "texture_id"):
                if (value := record.get(name)) is not MISSING:
                    facts.append(f"{name}={compact(value, 100)}")
    if event in LIBRARY_PROVENANCE_EVENTS:
        for name in ("native_process_id", "browser_process_id", "library_path", "library_provenance",
                     "inventory_status", "inventory_errno", "library_count", "maps_bytes", "maps_byte_limit",
                     "inventory_bounded", "path_encoding_unavailable"):
            if (value := record.get(name)) is not MISSING:
                facts.append(f"{name}={compact(value, None if name == 'library_path' else 100)}")
    for name in ("@signal", "@exit_code", "@test", "buffer"):
        if name == "@test" and options.error is not None:
            continue
        value = record.get(name)
        if value is not MISSING:
            facts.append(f"{name}={compact(value, 90)}")
    if isinstance(event, str) and HANDOFF_EVENT.search(event):
        for name in ("sequence", "value"):
            value = record.get(name)
            if value is not MISSING:
                facts.append(f"{name}={compact(value, 70)}")
    if event in ("presentation.workspace.wait", "presentation.workspace.service"):
        for name in ("requested_product_owner", "requested_product_revision", "observed_product_owner", "observed_product_revision",
                     "admitted_allocation", "admitted_access", "admitted_generation", "admitted_display_held",
                     "admitted_write_reserved", "admitted_completion_pending", "source_timeline_imported",
                     "workspace_admitted", "workspace_write_available"):
            if (value := record.get(name)) is not MISSING:
                facts.append(f"{name}={compact(value, 70)}")
    if options.triage:
        for name in ("generation", "slot", "boundary", "status", "code", "outcome", "span_outcome"):
            value = record.get(name)
            if value is not MISSING:
                facts.append(f"{name}={compact(value, 70)}")
        if triage_terminal(record):
            # Unknown terminal schemas stay visible; numeric enum meanings are not guessed.
            fields = record.data.get("fields")
            values = fields if isinstance(fields, dict) else record.data
            emitted = 0
            for name, value in values.items():
                if name in ("event", "name", "owner", "level", "timestamp", "steady_ns", "elapsed_ms",
                            "status", "code", "outcome", "span_outcome") \
                        or value in (None, "", 0, False) or not isinstance(value, (str, int, float, bool)):
                    continue
                facts.append(f"{compact(name, 70)}={compact(value, 70)}")
                emitted += 1
                if emitted == 6:
                    break
    for name in ("strong_count", "settled"):
        value = record.get(name)
        if value is not MISSING:
            facts.append(f"{name}={compact(value, 70)}")
    if record.metadata.get("context_copy"):
        facts.append("context-copy")
    if "proximity_ns" in record.metadata:
        facts.append(f"delta_ms={record.metadata['proximity_ns'] / 1000000:+.3f} ({record.metadata['time_link']})")
    suffix = (" " + compact(detail, detail_width)) if detail is not MISSING and detail != "" and event != "vulkan.validation" else ""
    if facts:
        suffix += " " + " ".join(facts)
    location = str(record.line)
    if "line_end" in record.metadata:
        location += "-" + str(record.metadata["line_end"])
    rendered = f"{compact(clock, 70)}:{timestamp} {compact(record.source, 150)}:{location} [{reason}] {compact(label)}{suffix}"
    if event == "vulkan.validation":
        rendered += "\n" + "\n".join("    " + compact(line, None) for line in record.raw.splitlines())
    return rendered


def render_descriptor_lineage(result, options, output):
    """Summarize only retained physical records; never turn FD numbers into OFD identities."""
    groups, exports, cuda, imports, sources = {}, {}, {}, {}, {}

    def index_row(index, key, row):
        # Two examples suffice to reject an ambiguous/reused export association.
        bucket = index.setdefault(key, [])
        if len(bucket) < 2:
            bucket.append(row)

    for item in result.retained:
        row = item.record
        event = row.get("@event")
        if not isinstance(event, str) or event not in DESCRIPTOR_LINEAGE_STAGES or row.metadata.get("context_copy"):
            continue
        run = str(row.metadata.get("run", row.source))
        if event.startswith("cuda.workspace.") and event != "cuda.workspace.memory_export":
            key = correlation_key(row, ("native_process_id", "workspace_allocation"))
            if key:
                imports.setdefault((run, key), []).append(row)
        elif event == "gpu.workspace.memory_export":
            key = correlation_key(row, ("native_process_id", "workspace_allocation"))
            if key:
                index_row(exports, (run, key), row)
        elif event == "cuda.workspace.memory_export":
            key = correlation_key(row, ("native_process_id", "workspace_descriptor", "workspace_plane"))
            if key:
                index_row(cuda, (run, key), row)
        elif isinstance(source := row.get("@workspace_source"), str):
            groups.setdefault((run, source), []).append(row)
            key = correlation_key(row, ("native_process_id", "workspace_allocation"))
            if key:
                sources.setdefault((run, key), set()).add(source)
    print("Descriptor lineage: retained physical rows only; missing/ambiguous stages are not proof of a failed handoff.", file=output)
    print("  Equal FD integers or fstat metadata never prove payload identity. Cross-process links below are "
          "SCM_RIGHTS record associations, not independent OFD/payload comparisons.", file=output)
    if not groups:
        if imports:
            print("  Missing native process/allocation source association; no CUDA import selected.", file=output)
        print("  No exact source chain retained; query workspace_allocation and include the native/browser artifact family.", file=output)
        return
    historical_required = ("cuda_export", "workspace_export", "duplicated", "sent", "received", "claimed", "import", "allocation", "bound")
    for (run, source), rows in sorted(groups.items())[:options.top]:
        issues, unavailable = [], []

        def values(selected, name):
            found = set()
            for row in selected:
                value = row.get(name)
                if value is MISSING or value is None:
                    continue
                if name in ("native_process_id", "browser_process_id") and (type(value) is not int or value <= 0):
                    continue
                if name == "dedicated" and type(value) in (bool, int):
                    value = bool(value)
                found.add(encoded(value))
                if len(found) == 3:
                    break
            return found

        def shown(row, name):
            value = row.get(name)
            return "not_recorded" if value is MISSING else compact(value, 100)

        def compare(selected, names):
            for name in names:
                observed = values(selected, name)
                if len(observed) > 1:
                    issues.append(f"{name} differs: {compact(','.join(sorted(observed)), 200)}")

        compare(rows, ("workspace_allocation", "@surface", "native_process_id", "browser_process_id"))
        modern = any(row.get("@event") in (
            "firefox.workspace.memory_export", "firefox.workspace.memory_export_failed",
            "firefox.workspace.descriptor_send", "firefox.workspace.descriptor_send_result",
            "presentation.workspace.descriptor_received") or row.get("vk_status") is not MISSING for row in rows)
        required = ("allocation", "bound", "vulkan_export", "send_attempt", "send_result", "native_received",
                    "cuda_import_started", "cuda_import", "cuda_map", "retirement_started", "retirement") if modern else historical_required
        native = values(rows, "native_process_id")
        allocations = values(rows, "workspace_allocation")
        if len(native) == 1 and len(allocations) == 1:
            owner_key = (next(iter(native)), next(iter(allocations)))
            if modern:
                source_count = len(sources.get((run, owner_key), ()))
                if source_count == 1:
                    rows = [*rows, *imports.get((run, owner_key), [])]
                else:
                    association = "missing" if source_count == 0 else "ambiguous"
                    unavailable.append(f"{association} native process/allocation source association; no CUDA import selected")
            matches = [] if modern else exports.get((run, owner_key), [])
            if len(matches) == 1:
                exported = matches[0]
                rows = [*rows, exported]
                key = correlation_key(exported, ("native_process_id", "workspace_descriptor", "workspace_plane"))
                mapped = cuda.get((run, key), []) if key else []
                if len(mapped) == 1:
                    rows.append(mapped[0])
                elif mapped:
                    unavailable.append("ambiguous CUDA export association; process/FD/address may have been reused")
            elif matches:
                unavailable.append("ambiguous workspace export association; no export selected")
        elif modern:
            association = "ambiguous" if len(native) > 1 or len(allocations) > 1 else "missing"
            unavailable.append(f"{association} native process/allocation source association; no CUDA import selected")
        stages = {}
        for row in rows:
            stages.setdefault(DESCRIPTOR_LINEAGE_STAGES[row.get("@event")], []).append(row)
        if modern:
            required += tuple(stage for stage in ("import_rejected", "export_failed")
                              if stage in stages)
        for stage, entries in stages.items():
            if len(entries) > 1 and stage not in ("send_attempt", "send_result"):
                unavailable.append(f"{stage} has {len(entries)} retained records; no unique stage selected")
        single = {stage: entries[0] for stage, entries in stages.items() if len(entries) == 1}
        if modern:
            results = stages.get("send_result", [])
            compare([*stages.get("send_attempt", []), *results],
                    ("workspace_descriptor", "native_process_id", "browser_process_id", "@workspace_source",
                     "@surface", "workspace_allocation", "channel_descriptor", "descriptor_count",
                     "memory_descriptor_index", "record_bytes", "descriptor_transport", "device_uuid",
                     "device_incarnation", "memory_size", "image_offset", "row_pitch", "capacity_width",
                     "capacity_height", "dedicated", "initialization"))
            successful = [row for row in results if type(row.get("send_bytes")) is int
                          and exact(row.get("send_bytes"), row.get("record_bytes")) and exact(row.get("send_errno"), 0)]
            if len(successful) == 1:
                single["send_result"] = successful[0]
            elif len(successful) > 1:
                unavailable.append("ambiguous successful SCM_RIGHTS sends")
                single.pop("send_result", None)
            else:
                single.pop("send_result", None)
                unavailable.append("SCM_RIGHTS send lacks an exact successful record-length result")
            for row in results:
                if not (type(row.get("send_bytes")) is int and exact(row.get("send_bytes"), row.get("record_bytes"))
                        and exact(row.get("send_errno"), 0)) and row.get("send_errno") not in (errno.EINTR, errno.EAGAIN):
                    issues.append("SCM_RIGHTS send failed or returned a short record")
        for stage, row in single.items():
            if stage.endswith("_failed") or stage == "import_rejected":
                issues.append(f"{stage} reported by its owner")
            if row.get("cuda_mapping_matches_export") is False:
                issues.append("CUDA mapping differs from the exported allocation handle")
            for status, error in (("fd_getfd_result", "fd_getfd_errno"), ("fd_stat_status", "fd_stat_errno")):
                if type(row.get(status)) is int and row.get(status) < 0:
                    target = issues if exact(row.get(error), errno.EBADF) else unavailable
                    target.append(f"{stage} {status}={shown(row, status)} errno={shown(row, error)}")
            if row.get("peer_credentials_known") is False:
                unavailable.append(f"{stage} peer credentials unavailable (errno={shown(row, 'peer_credentials_errno')})")
        duplicated = single.get("duplicated")
        if duplicated:
            status = duplicated.get("fd_ofd_status")
            if status == "same":
                if duplicated.get("fd_ofd_query") != "F_DUPFD_QUERY" \
                        or not exact(duplicated.get("fd_ofd_result"), 1) or not exact(duplicated.get("fd_ofd_errno"), 0):
                    issues.append("inconsistent F_DUPFD_QUERY same-OFD result")
            elif status in ("different", "invalid_descriptor"):
                issues.append(f"native duplicate OFD comparison: {status}")
            else:
                unavailable.append(f"native duplicate OFD comparison: {shown(duplicated, 'fd_ofd_status')}"
                                   f" (errno={shown(duplicated, 'fd_ofd_errno')})")
            exported = single.get("workspace_export")
            if exported and exported.get("workspace_descriptor") is not MISSING and duplicated.get("export_descriptor") is not MISSING \
                    and not exact(exported.get("workspace_descriptor"), duplicated.get("export_descriptor")):
                issues.append("workspace export FD differs from the native duplication argument")
        compare([single[stage] for stage in ("duplicated", "sent") if stage in single], ("workspace_descriptor",))
        compare([single[stage] for stage in ("received", "claimed", "import", "allocation") if stage in single],
                ("workspace_descriptor",))
        compare([single[stage] for stage in ("import", "allocation", "bound") if stage in single], ("vk_device", "vk_image"))
        compare([single[stage] for stage in ("allocation", "bound") if stage in single], ("vk_memory",))
        compare([single[stage] for stage in ("sent", "received", "claimed") if stage in single],
                ("descriptor_count", "memory_descriptor_index", "record_bytes"))
        sent = single.get("sent")
        if sent and (not exact(sent.get("send_bytes"), sent.get("record_bytes")) or not exact(sent.get("send_errno"), 0)):
            issues.append("SCM_RIGHTS send lacks an exact successful record-length result")
        allocated = single.get("allocation")
        if not modern and allocated and (not exact(allocated.get("vk_allocate_status"), 0) or allocated.get("fd_consumed") is not True):
            issues.append(f"vkAllocateMemory status={shown(allocated, 'vk_allocate_status')} "
                          f"fd_consumed={shown(allocated, 'fd_consumed')}")
        if modern:
            compare([single[stage] for stage in ("allocation", "bound", "vulkan_export") if stage in single],
                    ("vk_device", "vk_image", "vk_memory"))
            compare(rows, ("device_uuid", "device_incarnation", "memory_size", "image_offset", "row_pitch",
                           "capacity_width", "capacity_height", "dedicated"))
            compare([single[stage] for stage in ("vulkan_export", "send_result") if stage in single],
                    ("workspace_descriptor",))
            compare([single[stage] for stage in ("send_result", "native_received") if stage in single],
                    ("descriptor_count", "memory_descriptor_index", "record_bytes"))
            compare([single[stage] for stage in ("cuda_import_started", "cuda_import", "cuda_map", "retirement_started", "retirement")
                     if stage in single], ("retained_memory_descriptor",))
            received, started = single.get("native_received"), single.get("cuda_import_started")
            if received and started and not exact(received.get("workspace_descriptor"), started.get("retained_memory_descriptor")):
                issues.append("native received FD differs from retained backing FD")
            if started and exact(started.get("retained_memory_descriptor"), started.get("import_descriptor")):
                issues.append("CUDA consuming import descriptor is not an independent duplicate")
            compare([single[stage] for stage in ("cuda_import", "cuda_map") if stage in single], ("cuda_external_memory",))
            for stage, row in single.items():
                for status in ("cuda_status", "vk_status"):
                    if type(row.get(status)) is int and row.get(status) != 0:
                        issues.append(f"{stage} {status}={shown(row, status)}")
            for stage, names in (("allocation", ("vk_status",)), ("bound", ("vk_status",)),
                                 ("send_result", ("initialization",)),
                                 ("cuda_import_started", ("retained_memory_descriptor", "import_descriptor")),
                                 ("cuda_import", ("cuda_status", "cuda_external_memory")),
                                 ("cuda_map", ("cuda_status", "cuda_mapped_base")),
                                 ("retirement", ("cuda_status", "cuda_mapped_base", "cuda_external_memory"))):
                if stage in single:
                    for name in names:
                        if single[stage].get(name) is MISSING:
                            unavailable.append(f"{stage} {name} not recorded")
            for stage, handle in (("cuda_import", "cuda_external_memory"), ("cuda_map", "cuda_mapped_base")):
                row = single.get(stage)
                if row and exact(row.get("cuda_status"), 0) and exact(row.get(handle), 0):
                    issues.append(f"{stage} reports success with null {handle}")
            retired = single.get("retirement")
            if retired and exact(retired.get("cuda_status"), 0):
                for handle in ("cuda_mapped_base", "cuda_external_memory"):
                    value = retired.get(handle)
                    if value is not MISSING and not exact(value, 0):
                        issues.append(f"retirement reports success with unreleased {handle}={shown(retired, handle)}")
            imported = single.get("cuda_import")
            if imported and exact(imported.get("cuda_status"), 0) and imported.get("fd_consumed") is not True:
                unavailable.append("successful CUDA import lacks consumed-duplicate observation")
        allocation = compact(",".join(sorted(allocations)), 100) or "unknown"
        browser = ",".join(sorted(values(rows, "browser_process_id"))) or "unknown"
        print(f"  source={source} allocation={allocation} native_pid={','.join(sorted(native)) or 'unknown'} "
              f"browser_pid={browser} run={run}", file=output)
        chain = []
        for stage in required:
            row = single.get(stage)
            if stage == "send_attempt" and stage in stages:
                chain.append(f"send_attempt(count={len(stages[stage])})")
                continue
            if modern and stage == "send_result" and not row and stage in stages:
                chain.append("send_result(no_unique_success)")
                continue
            if row is None:
                chain.append(stage + "(missing)" if stage not in stages else stage + "(ambiguous)")
                continue
            descriptor = row.get("workspace_descriptor")
            label = stage + (f"(fd={shown(row, 'workspace_descriptor')})" if descriptor is not MISSING else "")
            if stage == "duplicated":
                label += f"[export_fd={shown(row, 'export_descriptor')},ofd={shown(row, 'fd_ofd_status')}]"
            if stage == "allocation" and not modern:
                label += f"[status={shown(row, 'vk_allocate_status')},consumed={shown(row, 'fd_consumed')}]"
            if stage in ("cuda_import_started", "cuda_import", "cuda_map"):
                label += f"[retained_fd={shown(row, 'retained_memory_descriptor')},import_fd={shown(row, 'import_descriptor')},status={shown(row, 'cuda_status')}]"
            chain.append(label)
        print("    direction=" + ("Vulkan_export_to_CUDA_import" if modern else "historical_CUDA_export_to_Vulkan_import"), file=output)
        print("    " + " -> ".join(chain), file=output)
        print("    conflicts: " + ("; ".join(dict.fromkeys(issues)) or "none observed in retained evidence"), file=output)
        if unavailable:
            print("    unavailable: " + "; ".join(dict.fromkeys(unavailable)), file=output)
    if len(groups) > options.top:
        print(f"  {len(groups) - options.top} additional source chains omitted by --top.", file=output)


def render(result, options, output, diagnostics):
    automatic = result.automatic.count if result.automatic else 0
    rows = result.rows()
    total = result.matches + result.related + result.proximity + result.context + automatic
    status = (
        f"Scanned {result.scanned} records; {result.matches} matched, {result.related} correlated, "
        f"{result.proximity} proximity, {result.context} context; {result.errors} failure candidates; "
        f"{str(automatic) + ' automatic; ' if automatic else ''}showing {len(rows)}/{total}."
    )
    if options.representatives:
        status += " One representative per --group-by value; occurrences count all selected records."
    if result.lookup:
        lookup = result.lookup
        report = output if options.format == "summary" else diagnostics
        print(f"Error lookup: {lookup.mode}; {lookup.matches} occurrences; focused on first physical capture by file mtime.", file=report)
        print("First physical match: " + render_record(Retained((), lookup.focus, "query", False), options), file=report)
        print(f"Run: {lookup.focus.metadata['run']} [{lookup.focus.metadata['run_link']}]", file=report)
        if lookup.focus.metadata.get("test"):
            print(f"Test: {lookup.focus.metadata['test']}", file=report)
        if lookup.artifacts:
            print("Artifacts:", file=report)
            for source in lookup.artifacts[:options.top]:
                print("  " + source, file=report)
    if options.format == "summary":
        print(status, file=output)
        if result.groups:
            print("Groups (" + ", ".join(options.group_by) + "):", file=output)
            for group, count in heapq.nsmallest(options.top, result.groups.items(),
                                                key=lambda item: (-item[1], item[0])):
                print(f"  {count:>7}  " + " | ".join(compact(json.loads(value), 110) for value in group), file=output)
        for clock, (start, end) in sorted(result.clocks.items()):
            detail = f"{start}..{end} ns" if start is not None else "no comparable timestamp"
            print(f"Clock {compact(clock)}: {detail}", file=output)
        if result.retained:
            print("Records:", file=output)
    else:
        print(status, file=diagnostics)
    if result.automatic:
        if options.format == "timeline":
            result.automatic.highlight(output)
        if automatic or result.automatic.omitted_anchors:
            print(f"Auto correlation: {automatic} related rows; at most {MAX_AUTO_PER_ANCHOR}/anchor, "
                  f"{MAX_AUTO_PER_IDENTITY}/identity, {MAX_AUTO_RELATED} total, within --limit. "
                  f"Vulkan object provenance allows {MAX_AUTO_VULKAN_RELATED}/anchor and identity. "
                  "Rows are grouped beside exact query anchors; cross-clock times remain unaligned. "
                  "Use --no-auto-correlate for the original query timeline.", file=diagnostics)
        if result.automatic.omitted_anchors:
            print(f"Auto correlation: {result.automatic.omitted_anchors} query anchors outside the "
                  f"{MAX_AUTO_ANCHORS}-anchor budget; recent specific matches take priority.", file=diagnostics)
        object_conflicts = sum(result.automatic.object_identity(identity) for identity in result.automatic.ambiguous)
        if object_conflicts:
            print(f"Auto correlation: {object_conflicts} Vulkan handle keys have conflicting recorded "
                  "process/device/source/allocation facts; their candidate joins were omitted.", file=diagnostics)
    for item in rows:
        print(render_record(item, options), file=output)
    if options.descriptor_lineage:
        render_descriptor_lineage(result, options, output if options.format == "summary" else diagnostics)
    if options.format == "summary":
        terminals = heapq.nlargest(
            options.top, (row for key, row in result.terminals.items() if key[0] in result.selected_runs),
            key=lambda item: order_key(item, "capture"),
        )
        if terminals:
            print("Terminals in matched runs (independent of --limit):", file=output)
            for row in reversed(terminals):
                print("  " + render_record(Retained((), row, "terminal", False), options), file=output)
        elif result.lookup:
            print("Terminal evidence: no terminal record was captured in this matched run.", file=output)
        if result.native_tail:
            print("Latest native intent/reply evidence in this run (source order; cross-clock proximity only):", file=output)
            for item in sorted(result.native_tail, key=lambda item: item.key):
                print("  " + render_record(item, options), file=output)
    if result.malformed:
        print(f"Warning: {result.malformed} malformed/truncated records (retained as searchable text):", file=diagnostics)
        for example in result.parse_examples:
            print("  " + compact(example, 300), file=diagnostics)
    if options.order == "proximity":
        print("Cross-clock neighborhoods use file mtime aligned to each source's last timestamp; proximity is not causality.", file=diagnostics)
    elif len(result.clocks) > 1:
        print("Clock domains are grouped separately; their timestamps do not establish cross-domain order.", file=diagnostics)


HELP = """\
Examples:
  ./mmltk --logs --family latest-wayland-test --triage
  ./mmltk --logs build/validation/latest-wayland-test-firefox.log --triage
  ./mmltk --logs build/validation/final-wayland-acceptance.log \\
      --family latest-wayland-test --triage -q 'probe_failed OR "rendered probe"'
  ./mmltk --logs --errors
  ./mmltk --logs --family latest-wayland-test -q '@event=vulkan.validation' \\
      --group-by validation_id --representatives --format timeline --limit 40 --top 20
  ./mmltk --logs --family latest-wayland-test -q 'validation_id:SYNC-HAZARD' \\
      --group-by validation_id --representatives --format jsonl --auto-correlate
  ./mmltk --logs -q 'onnx OR "CUDA error"'
  ./mmltk --logs -q '@event:shutdown AND NOT @event:started' --format timeline
  ./mmltk --logs -q 'trace_id=42' --correlate trace_id --tail --limit 80
  ./mmltk --logs --family latest-wayland-test -q 'workspace_allocation=34' \\
      --where '@event:memory_export OR @event:memory_import OR @event:memory_bound OR @event:image_created' \\
      --correlate native_process_id+workspace_descriptor+workspace_plane \\
      --correlate browser_process_id+vk_device+vk_image --format timeline --limit 16
  ./mmltk --logs --family latest-wayland-test -q 'workspace_allocation=34' \\
      --descriptor-lineage --format timeline --limit 32
  ./mmltk --logs --family latest-wayland-test \\
      -q '@event:loaded_library OR @event:library_inventory' --format timeline --limit 70
  ./mmltk --logs -q '@event=firefox.workspace.ready' --correlate @surface
  ./mmltk --logs -q 'duration_ns>=1000000' --fields @event,duration_ns,trace_id
  ./mmltk --logs --where '@file:"latest-wayland-test"' --group-by @event
  ./mmltk --logs --family latest-wayland-test --errors --related-run --tail
  ./mmltk --logs --family latest-wayland-test --history --list-runs
  ./mmltk --logs --family latest-wayland-test --run 57-131007497132582 -q SIGSEGV
  ./mmltk --logs build/validation/viewer-copy-ownership-trace.log \\
      --family latest-wayland-test --history -q 'buffer="BufferId(21,1)"' --context 2
  ./mmltk --logs --error 'Presentation unavailable

  Explore requires a measured non-empty gallery.'

Grammar (quote the entire expression for the shell):
  expr      := expr OR expr | expr [AND] expr | NOT expr | '(' expr ')'
  primary   := text | '*' | has(field) | field operator value
  operator  := = != : ~ !~ > >= < <=
AND binds tighter than OR; NOT binds tightest. Adjacent terms imply AND.
Bare/quoted text and ':' are case-insensitive literal substring searches.
'='/'!=' compare exact typed values; ordering compares numbers; '~'/'!~' use
Python regex (case-sensitive; use (?i) for insensitive). Missing fields do
not satisfy comparisons, including '!='; NOT includes them. has() tests
presence including null/zero. Quote strings containing spaces or punctuation.

Fields:
  JSON paths (fields.name), event/trace_id/etc. (unqualified names also look
  inside fields), plus @file, @line, @line_end, @text, @format, @clock, @time_ns,
  @event, @owner, @level, @error, @surface, @parse_error, @test, @tags,
  @run, @archive_id, @family, @artifact, @mtime_ns, @context_copy, @part,
  @terminal, @exit_code, @signal, @signal_number, @proximity_ns, @time_link,
  @triage_reason, @triage_payload_truncated, @discovery.
  @event resolves wrapped fields.event/name before event/name.
  @surface joins native uint64 surface_high/low and Firefox 32-hex surfaces.
  @error marks failure candidates from levels/event/message text; it is a
  search aid, not a diagnosis. Numeric error=0 metrics do not mark failures.
  child.signaled value=139 decodes to SIGSEGV (11); 143 to SIGTERM (15).
  Signals describe the observed termination, not whether shutdown was intended.
  Bare terms search test/tag/run/signal metadata as well as original text.
  Catch INFO copies retain their original timestamps and are labeled context-copy.
  Memory provenance follows Firefox Vulkan allocation/binding/export, SCM_RIGHTS
  send attempts and exact send results, native receipt, CUDA import and mapping,
  and retirement. --descriptor-lineage restricts a query to these physical facts
  and retained historical CUDA-export captures. Query workspace_allocation in one
  capture and use --family to include native and Firefox artifacts. Correlations
  use native_process_id+workspace_allocation, browser_process_id+source+workspace_allocation,
  and process-local image or historical export identities. FD numbers never join
  different processes; fstat metadata does not establish backing payload identity.
  The bounded report uses retained rows only (--limit/--where still apply), with
  at most --top source chains. Missing and ambiguous observations remain explicit.
  A descriptor_send is only an attempt, including EINTR/EAGAIN retries. Only an
  exact record-length descriptor_send_result with send_errno=0 proves send success.
  Native received storage remains in retained_memory_descriptor. import_descriptor
  is its consuming duplicate, observed before CUDA; successful import does not
  query that consumed FD. Historical F_DUPFD_QUERY observations retain their
  original same/different/unsupported/denied meaning without asserting identity
  across processes. Diagnostics never retain extra descriptors or GPU resources.
  loaded_library/library_inventory capture existing /proc/self/maps once at the
  native first_memory_import or Firefox first_memory_export. They load no DSO.
  Inventories bound reads to 1 MiB and 32 paths shorter than 1024 bytes, reporting
  incomplete/unavailable facts. Query these separately by process ID so they do
  not consume the allocation chain's retained-row budget. Disabled diagnostics
  collect nothing. Raw rows retain their original file/line provenance.

Vulkan layer diagnostics:
  Adjacent Rust VALIDATION headers, indented message bodies, and matching
  logger objects lines form one searchable record. The original physical lines
  and complete message remain available in text/message; @line..@line_end
  identifies their span. Assembly is bounded to 128 lines and 1 MiB; overflow is
  reported as a parse error and remaining lines stay searchable separately.
  @event=vulkan.validation exposes validation_id (VUID or SYNC-HAZARD), message_id,
  api, level, and typed objects with normalized hexadecimal handles and reported
  names. A sole IMAGE or DEVICE_MEMORY is also queryable as vk_image/vk_memory.
  Human timelines print the complete diagnostic, including its stated conditions.
  It remains a layer allegation; a handle match does not establish its cause.
  --group-by validation_id --representatives selects one first record per group,
  preferring physical records over context copies, with @representative_count.
  All selected occurrences still contribute to counts. --order controls selection
  and ordering; --limit bounds retained groups/context plus automatic evidence.
  Add api or message to --group-by when different messages use the same VUID.
  --representatives cannot combine with --tail, --error, or --triage.
  Automatic correlation joins the representative's exact typed image/memory
  handles to image_created, memory_import, memory_bound, and direct_binding.
  It retains at most four diverse provenance events per Vulkan anchor/handle,
  within the normal 48-row and --limit budgets, using the existing streaming pass.
  Explicit equal process IDs may join files in the same run; conflicting process,
  device, source, or allocation facts reject the join. PID-free Rust stderr can
  only produce labeled same-file handle candidates, never a cross-process join
  or an asserted process identity. Observed handle reuse/multiple owners suppress
  ambiguous candidates. Missing provenance is not proof that an object is unrelated.
  Narrow a capture/allocation lifetime because handle numbers may be reused.
  Correlated rows do not seed further expansion. JSONL requires --auto-correlate;
  --no-auto-correlate disables the automatic joins in human timelines.

Inputs and ordering:
  Paths/globs are repository-relative or absolute within the repository.
  Directories select .jsonl/.log/.out/.txt and Mozilla process logs; --recursive includes histories.
  Explicit files can have any suffix. Repeated paths are deduplicated.
  The default is build/validation, without recursive archived captures.
  Time ordering groups steady, UTC wall, timezone-free wall, per-file elapsed,
  and untimed records separately; ties use file/line. No clock offset is guessed.
  --order capture sorts files by captured mtime then preserves their line order;
  mtime is artifact recency, not an event timestamp or proof of causality.
  Use one capture at a time: IDs and monotonic times can repeat across runs.
  Files are read up to their captured byte size; correlation requires unchanged
  files over two passes. There is no follow mode, persistent index, or network.

Artifact families:
  --family STEM selects STEM.jsonl / STEM.log / STEM-native.log / STEM-firefox.log
  plus STEM-acceptance.jsonl, STEM-application.log, and
  STEM-mozilla-main.PID.log.moz_log[.0..3] and
  STEM-mozilla-child.PID.log.child-LOGICAL_ID.moz_log[.0..3]. Descendant
  launchers append another .child-LOGICAL_ID before .moz_log. Mozilla modules
  retain four bounded rotating files per process, separately from structured Firefox evidence.
  Families live under build/validation (or supply a repository path). --history adds rotated
  siblings; --run selects an exact archive ID or 'current'. Adjacent rotations
  with the same PID within 10ms are grouped and explicitly labeled inferred.
  Shared (event, steady_ns) anchors link transcript captures to native runs and
  propagate known test/tag metadata. Missing anchors leave transcripts separate.
  --related-run includes other records from a matched run; named correlations
  are automatically scoped to the run in family/history mode. Summary output
  includes terminal events from matched runs independently of the sample limit.

Automatic timeline correlation:
  Human --format timeline queries with --query/--errors add a small set of
  direct frame/publication/snapshot/lifecycle matches beside exact query rows.
  --no-auto-correlate restores the original timeline. JSONL and summary formats
  require explicit --auto-correlate; --correlate, --related-run, --error, and
  --triage keep their existing selection behavior without this extra pass.
  Original matches and ordering are preserved; automatic rows use spare --limit
  capacity only. Broad/truncated timelines start with three recent specific query
  matches, including matches beyond the retained sample. Recency uses captured
  file mtime and physical source order; it never aligns independent clocks.
  Up to 128 retained query anchors prefer recent specific events over progress
  chatter. Limits are 3 diverse events per anchor, 2 rows per joining identity,
  and 48 related rows total. Added rows never become new correlation seeds.
  Source/session/revision, surface/publication/allocation/transfer, trace/span,
  dataset/gallery, and scoped observation identities must be nonempty and agree
  where both records state them. Bare counters, pixel/probe/slot loops, context
  copies, malformed rows, and ambiguous source revisions do not auto-expand.
  Known integration report slots have event-specific projections, such as
  explore_reopen_wait.b -> Explore source revision and explore_reopen_draw.c/d
  -> source/presentation revision. Generic a/b/c/d numbers never join.
  Phase/control matches additionally need at most 250ms (--near-ms can narrow
  it), or eight same-source lines with unaligned clocks, and are labeled weaker
  proximity, capped at eight rows globally. Cross-clock grouping by an exact
  identity never aligns clocks.
  Every added row shows [auto: reason]. Explicit JSONL opt-in uses match=auto
  and auto_reason/auto_anchor metadata (also included with --fields). Retained
  automatic payloads use the triage size bounds; auto_payload_truncated flags
  any shortening. --where constrains all added rows. One extra streaming pass
  uses bounded per-identity nearest-anchor indexes, without an all-run index.

Pasted errors:
  --error TEXT searches exact whitespace-normalized phrases first, accepting a
  colon when a displayed title and body were separated by a blank line. Only
  when no phrase matches does it try all words, reporting that fallback.
  It includes histories, prefers physical logs over copied transcript context,
  and focuses the earliest matching capture by file mtime, then file/line.
  The original event timestamp and clock are always retained.
  Nearby records default to +/-1000ms (--near-ms). Same-clock differences are
  direct; cross-clock estimates align file mtime to each source's last timestamp.
  Untimed rows may borrow a preceding timestamp for proximity only. Every such
  estimate is labeled; file timestamps are not proof of causal order.
  Output prioritizes the error and nearby intent/reply/message/terminal records.
  --limit bounds the timeline sample; terminal summaries have at most --top rows
  and native run-tail evidence has at most min(5, --top) rows.

Automatic triage:
  Graphics pixel joins follow the acquired mode: direct_sampling=true compares
  native source -> Iced sample, without import/mailbox copy stages. Copy mode
  compares native -> import -> mailbox -> sample. An exact frame_forwarded
  source+transfer bridge selects the physical source; arena identity alone
  never identifies source storage. Direct GPU read_settled receipts prove
  released source custody; copy_completed proves the Copy-mode transfer.
  Offered publications need no read settlement until actually acquired.
  Optional firefox.workspace.direct_pixel adds native -> direct_image -> sample
  comparisons only for that exact direct acquisition after completion=Ok(true)
  and matching 0x4d4d4c54 readback markers. Missing or invalid optional probes
  never replace native -> sample or require another evidence stage.
  Optional presentation.pixel_source_copy_before_ready and
  presentation.pixel_source_copy_after_release report direct CUDA source-to-host
  samples separately from the native kernel probes (presentation.pixel and
  presentation.pixel_after_release). Exact source+transfer+publication and
  coordinates join these observations to their kernel, each other, and the
  acquired direct_image/Iced samples. Contradictory source/allocation facts
  prevent byte comparison; missing optional observations never imply failure.
  Raw buffer-alias observation is unavailable across the image's external
  ownership acquire; direct_memory_alias/status report the exact reason.
  Historical direct_memory_pixel records read undefined alias contents and
  remain inspectable, but never participate in pixel comparisons.
  --triage selects a run using ranked failure/assertion/error/terminal/parse
  anchors, or an unmatched start when no explicit failure is available. --query
  narrows anchors; --where constrains every pass. --error keeps its first-physical
  phrase lookup and proximity behavior. Nothing in triage includes the whole run.
  A single file discovers same-family current siblings, or only the matching
  archive batch (even without current files). Auto additions carry @discovery;
  rotation links are inferred. Ordinary path queries do not auto-expand inputs.
  Explicit failures outrank error-related event names; --errors remains broad.
  Defaults: 4 anchors, 12 identities, 2 expansion hops, 20 evidence rows,
  5 summary entries per section. Tune
  --triage-anchors, --triage-identities, --triage-hops, --limit, and --top.
  At most 32 diversified neighboring records seed identities, using 3 lines (or
  --context if larger) and --near-ms. Every row states its inclusion reason.
  Exact identities are run-qualified surface/typed handles, nonempty *_id or
  *_identity fields, and source_session+source_instance. parent_*_id joins its
  corresponding *_id namespace. Device/process/thread/user/session IDs and
  bare counters are excluded as ambient; --correlate explicitly opts fields in.
  Identities co-occurring on a matched row can seed the next hop. Counter
  sequence/generation/slot composites need two fields plus same-clock proximity
  or 100 same-source lines; zero slots are valid. They never seed further hops.
  Lifecycle balances use each row's most-specific operation/resource identity
  (span, request, surface, then other IDs; not secondary handles), exact event
  prefixes and generic start/end suffixes
  (requested/submitted/started -> completed/failed/outcome, opened -> closed,
  acquired -> released, etc.). These conventions are hypotheses, not product
  requirements. Missing/blocked/stalled stages, failed textual span outcomes,
  duplicate ends, unclosed starts, identity handoffs, and the largest same-source
  clock gaps (--triage-gap-ms, default 1000) are highlighted with evidence.
  Handoff candidates require a shared identity and no conflicting operation ID;
  unrelated endings or already-balanced endpoints cannot satisfy missing stages.
  Sources are never causally ordered from proximity. Copied INFO does not
  participate in lifecycle balances. Numeric outcome enums are not guessed.
  Up to 4 generic *.terminal/terminal=true or known process/test terminal
  records are reported independently of --limit. Without terminals, captured
  source ends are shown without claiming success/failure/timeout.
  Memory is bounded: 256 open anchor families, 64 findings, 64 event/family
  values per identity, 32 sources per identity, 10000 auto-added artifact files,
  and size-bounded retained
  payloads (96 fields, depth 4, 2048 characters per string). Capacity limits are
  reported. Triage uses bounded streaming passes, no all-run index or network.
  --related-run/--tail/--list-runs cannot combine with --triage. JSONL writes
  only evidence rows to stdout, summaries to stderr; @triage_reason records why.
"""


def bounded_integer(minimum, maximum):
    def parse(value):
        number = int(value)
        if not minimum <= number <= maximum:
            raise argparse.ArgumentTypeError(f"expected {minimum}..{maximum}")
        return number
    return parse


def argument_parser():
    parser = argparse.ArgumentParser(
        prog="./mmltk --logs", description=__doc__,
        epilog=HELP, formatter_class=argparse.RawDescriptionHelpFormatter,
        allow_abbrev=False,
    )
    parser.add_argument("paths", nargs="*", help="files, directories, or quoted globs (default: build/validation)")
    parser.add_argument("-q", "--query", default="", help="select records using the expression grammar")
    parser.add_argument("--where", default="", help="filter all records, including correlated/context rows")
    parser.add_argument("--errors", action="store_true", help="AND @error=true with --query")
    parser.add_argument("--error", metavar="TEXT", help="paste displayed error text; locate first physical capture and nearby events")
    parser.add_argument("--triage", action="store_true", help="automatically rank failures, expand bounded identity chains, and highlight lifecycle evidence")
    parser.add_argument("--triage-anchors", type=bounded_integer(1, 12), default=4, metavar="N",
                        help="maximum ranked triage anchors (default: 4)")
    parser.add_argument("--triage-identities", type=bounded_integer(1, 64), default=12, metavar="N",
                        help="maximum automatic identities across all expansion hops (default: 12)")
    parser.add_argument("--triage-hops", type=bounded_integer(0, 3), default=2, metavar="N",
                        help="identity co-occurrence expansion hops after nearby seeds (default: 2)")
    parser.add_argument("--triage-gap-ms", type=bounded_integer(1, 600000), default=1000, metavar="MS",
                        help="highlight comparable same-source clock gaps at least this long (default: 1000)")
    parser.add_argument("--near-ms", type=bounded_integer(0, 600000), default=1000,
                        help="pasted-error neighborhood radius in milliseconds (default: 1000)")
    parser.add_argument("--correlate", action="append", default=[], metavar="FIELD[+FIELD...]",
                        help="include records sharing a seed identity; repeat for OR, + for composite identities")
    parser.add_argument("--descriptor-lineage", action="store_true",
                        help="follow Vulkan export/CUDA import (or historical captures), distinguishing send attempts, success and retained/consumed FDs")
    automatic = parser.add_mutually_exclusive_group()
    automatic.add_argument("--auto-correlate", action="store_true", dest="auto_correlate",
                           help="opt in to bounded automatic related evidence, including for JSONL")
    automatic.add_argument("--no-auto-correlate", action="store_false", dest="auto_correlate",
                           help="disable automatic evidence and recent-match highlights in human timelines")
    parser.set_defaults(auto_correlate=None)
    parser.add_argument("--context", type=bounded_integer(0, 100), default=0,
                        help="include up to N nonblank records before/after each match in the same file")
    parser.add_argument("--format", choices=("summary", "timeline", "jsonl"), default="summary")
    parser.add_argument("--fields", help="comma-separated output field projection; provenance is always retained")
    parser.add_argument("--group-by", default="@owner,@event", help="comma-separated summary grouping fields")
    parser.add_argument("--representatives", action="store_true",
                        help="retain one first record per --group-by value, with full occurrence counts")
    parser.add_argument("--top", type=bounded_integer(1, 100),
                        help="maximum summary entries per section (default: 5 for triage, 10 otherwise)")
    parser.add_argument("--limit", type=bounded_integer(1, 10000), default=20,
                        help="maximum timeline sample records; counts cover all matches (default: 20)")
    parser.add_argument("--order", choices=("time", "file", "capture", "proximity"))
    parser.add_argument("--tail", action="store_true", help="retain the last N records, displayed in ascending order")
    parser.add_argument("--recursive", action="store_true", help="descend directories, including archived runs")
    parser.add_argument("--family", action="append", default=[], metavar="STEM",
                        help="discover native/acceptance/application/Firefox/Mozilla artifact siblings; repeatable")
    parser.add_argument("--history", action="store_true", help="include selected files' rotated .history siblings")
    parser.add_argument("--run", help="select an archive ID, full run identity, or current; implies --history")
    parser.add_argument("--list-runs", action="store_true", help="list discovered run IDs and files without parsing records")
    parser.add_argument("--related-run", action="store_true", help="include the rest of each run containing a query match")
    parser.add_argument("--strict", action="store_true", help="exit 2 if any input record is malformed/truncated")
    return parser


def main(argv=None, *, root=None, output=None, diagnostics=None):
    parser = argument_parser()
    options = parser.parse_args(argv)
    output = output or sys.stdout
    diagnostics = diagnostics or sys.stderr
    try:
        query = QueryParser(options.query).parse()
        where = QueryParser(options.where).parse()
        if options.errors:
            query = Expression("and", (query, Expression("=", ("@error", True))))
        if options.descriptor_lineage:
            if not options.query.strip() or options.triage or options.related_run or options.error is not None \
                    or options.list_runs or options.representatives:
                raise QueryError("--descriptor-lineage requires --query and cannot combine with "
                                 "--triage, --related-run, --error, --list-runs, or --representatives")
            where = Expression("and", (where, Expression("or", tuple(
                Expression("=", ("@event", event)) for event in sorted(MEMORY_PROVENANCE_EVENTS)))))
            for names in ("native_process_id+workspace_allocation", "browser_process_id+source+workspace_allocation",
                          "native_process_id+workspace_descriptor+workspace_plane", "browser_process_id+vk_device+vk_image"):
                if names not in options.correlate:
                    options.correlate.append(names)
        if options.triage and (options.related_run or options.tail or options.list_runs):
            raise QueryError("--triage cannot combine with --related-run, --tail, or --list-runs; its evidence is bounded automatically")
        if options.representatives and (options.triage or options.tail or options.error is not None):
            raise QueryError("--representatives cannot combine with --triage, --tail, or --error")
        if options.correlate and not (options.query.strip() or options.errors or options.triage):
            raise QueryError("--correlate requires an explicit --query or --errors seed")
        if options.auto_correlate is True and not auto_correlate_enabled(options):
            raise QueryError("--auto-correlate requires --query or --errors and cannot combine with "
                             "--correlate, --related-run, --error, or --triage")
        options.fields = field_names(options.fields) if options.fields else ()
        options.group_by = field_names(options.group_by)
        options.top = options.top or (5 if options.triage else 10)
        options.order = options.order or ("proximity" if options.error is not None else "time")
        if options.order == "proximity" and options.error is None:
            raise QueryError("--order proximity requires --error")
        if options.error is not None:
            options.history = True
        for relationship in options.correlate:
            field_names(relationship, "+")
        repository = (root or Path(__file__).resolve().parent.parent).resolve()
        catalog = ArtifactCatalog(options, repository)
        if options.list_runs:
            catalog.list_runs(output, options.limit)
            return 0
        if options.family or options.history or options.run or options.related_run or options.triage:
            catalog.prepare_anchors()
        if catalog.selected_runs:
            scope = Expression("or", tuple(Expression("=", ("@run", run)) for run in sorted(catalog.selected_runs)))
            where = Expression("and", (where, scope))
        lookup = None
        files = catalog.files
        if options.error is not None:
            lookup = ErrorLookup(options.error, options)
            lookup.locate(files, query, where, catalog.anchors)
            if lookup.focus is None:
                print(f"No exact/display-normalized phrase or all-words matches in {len(files)} files.", file=output)
                if options.strict and lookup.malformed:
                    print(f"Strict lookup scan observed {lookup.malformed} malformed/truncated records.", file=diagnostics)
                    return 2
                return 1
            files = [source for source in files if lookup.relevant(source)]
            lookup.calibrate(files, catalog.anchors)
            lookup.annotate(lookup.focus, force=True)
            query = lookup.expression
            where = Expression("and", (where, Expression("=", ("@run", lookup.focus.metadata["run"]))))
        if options.triage:
            triage = Triage(options, where, catalog.anchors, lookup).run_query(files, query)
            render_triage(triage, options, output, diagnostics)
            if options.strict and (triage.malformed or lookup and lookup.malformed):
                return 2
            return 0 if triage.selected else 1
        result = execute(files, query, where, options, catalog.anchors, lookup)
        render(result, options, output, diagnostics)
        if options.strict and lookup and lookup.malformed:
            if lookup.malformed != result.malformed:
                print(f"Strict lookup scan observed {lookup.malformed} malformed/truncated records across all inputs.", file=diagnostics)
            return 2
        if options.strict and result.malformed:
            return 2
        return 0 if result.matches + result.related else 1
    except BrokenPipeError:
        raise
    except (QueryError, OSError, SyntaxError, InvalidOperation) as error:
        print(f"mmltk logs: {error}", file=diagnostics)
        return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:
        # Avoid a second exception when Python flushes a pipe closed by its reader.
        sys.stdout = open(os.devnull, "w", encoding="utf-8")
        sys.exit(0)
