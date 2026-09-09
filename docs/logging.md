# Capturing and querying diagnostics

[Commands](commands.md) · [Validation](validation.md) · [Headless Wayland](headless-wayland.md)

## Capture one reproduction

```bash
MMLTK_GUI_TRACE_FILE=.mmltk-data/logs/gui-trace.jsonl \
MMLTK_FIREFOX_LOG_FILE=.mmltk-data/logs/firefox.log \
./mmltk --gui
```

`MMLTK_GUI_TRACE_FILE` opts into native JSONL command, transport, worker,
presentation, cleanup, and failure diagnostics. The native sink opens a new
capture and truncates that path; preserve earlier evidence before reusing it.
`MMLTK_FIREFOX_LOG_FILE` captures the separate browser log.
Without an enabled diagnostics target, the runtime avoids constructing its
diagnostic records.

Add `MMLTK_GUI_PIXEL_TRACE=1` for pixel probes; desktop startup enables them
only when the native trace path is also set. Lifecycle-only traces do not
prove pixel continuity. Native surface high/low values and the browser's
32-hex-digit surface identity describe the same surface.

Ordinary native logging also accepts `MMLTK_LOG_LEVEL`, `MMLTK_LOG_FILE`, and
`MMLTK_LOG_DIR`. GPU-specific trace variables are covered in
[GPU execution](gpu-execution.md). The Wayland acceptance harness captures and
rotates its native/Firefox/JSONL artifact family under `build/validation`.
The headless supervisor writes a separate unique directory for each invocation.

## Start with a bounded investigation

```bash
./mmltk --logs --family latest-wayland-test --triage
./mmltk --logs build/validation/latest-wayland-test-firefox.log --triage
./mmltk --logs --family latest-wayland-test --triage -q 'probe_failed OR "rendered probe"'
```

Triage ranks explicit failures, assertions, terminal events, damaged records,
and incomplete stages, then selects one run and follows bounded identity
chains. Each evidence row explains its inclusion. Use `--query` to narrow
anchors and `--where` to constrain every pass.

A single-file triage input discovers same-family current siblings, or only
the matching archive batch for an archived file. It does not mix an
archived-only input with the current run. Added sources carry `@discovery`
metadata; rotation-based relationships remain labeled as inferred. Ordinary
explicit path queries do not automatically add sibling files.

Paste a displayed error, including its title and blank line:

```bash
./mmltk --logs --error 'Presentation unavailable

Explore requires a measured non-empty gallery.'
```

`--error` searches whitespace-normalized full phrases first, accepting the
colon used between logged titles and bodies. If no phrase matches, it reports
an all-words fallback. It includes histories, prefers physical records over
copied Catch context, and focuses the earliest matching capture by file
modification time. It preserves original file/line and timestamp/clock data.
Combine it with `--triage` for automatic identity investigation around that
error.

## Query expressions

```bash
./mmltk --logs --errors --tail
./mmltk --logs -q 'onnx OR "CUDA error"' --context 2
./mmltk --logs -q '@event:shutdown AND NOT @event:started' --format timeline
./mmltk --logs -q 'duration_ns>=1000000' --fields @event,duration_ns,trace_id
./mmltk --logs --where '@file:"latest-wayland-test"' --group-by @event
```

Quote the entire expression for the shell:

```text
expr      := expr OR expr | expr [AND] expr | NOT expr | '(' expr ')'
primary   := text | '*' | has(field) | field operator value
operator  := = != : ~ !~ > >= < <=
```

`NOT` binds most tightly, then `AND`, then `OR`. Adjacent terms imply `AND`.
Bare/quoted text and `:` perform case-insensitive literal substring matches.
`=` and `!=` compare exact typed values; ordering compares numbers.
`~` and `!~` use Python regexes, case-sensitive unless you supply `(?i)`.
Quote values containing spaces or punctuation.

Missing fields do not satisfy comparisons, including `!=`; `NOT` can include
them. `has(field)` tests presence, including null and zero. `--errors` adds
`@error=true`; it is a broad failure-candidate search aid, not a diagnosis.
An `error=0` metric alone is not an error.

## Fields and correlation

JSON paths such as `fields.name` work directly. Unqualified names also look
inside `fields`. Useful metadata includes:

| Fields | Meaning |
| --- | --- |
| `@file`, `@line`, `@text`, `@format` | Physical provenance and input representation |
| `@clock`, `@time_ns`, `@mtime_ns` | Recorded clock/time and captured file modification time |
| `@event`, `@owner`, `@level`, `@error` | Normalized event and failure-candidate metadata |
| `@surface` | Joined native/browser surface identity |
| `@test`, `@tags`, `@context_copy`, `@part` | Test context, copied INFO, transcript segment |
| `@run`, `@archive_id`, `@family`, `@artifact` | Capture and artifact grouping |
| `@terminal`, `@exit_code`, `@signal`, `@signal_number` | Observed terminal status |
| `@parse_error` | Damaged/truncated input |
| `@proximity_ns`, `@time_link` | Pasted-error proximity and its time-link method |
| `@triage_reason`, `@triage_payload_truncated`, `@discovery` | Triage inclusion and limits |

`@event` resolves wrapped `fields.event`/`fields.name` before top-level names.
Bare searches also examine test/tag/run/signal metadata. Catch INFO copies keep
their original timestamps and are labeled as copies. A `child.signaled` value
of 139 decodes to SIGSEGV and 143 to SIGTERM; the signal does not establish
whether termination was expected.

```bash
./mmltk --logs -q 'trace_id=42' --correlate trace_id --format timeline --limit 80
./mmltk --logs -q '@event=firefox.workspace.ready' --correlate @surface
./mmltk --logs -q 'source_session=42' --correlate source_session+source_instance
```

Ordinary correlation requires a query or error seed. Repeat `--correlate FIELD`
for alternative identities; join field names with `+` for a composite
identity. It joins original matches, excludes empty/zero identities, and
does not expand transitively. Family/history correlations are scoped to each
run. `--where` filters correlated and context rows too.

## Select captures and histories

```bash
./mmltk --logs --family latest-wayland-test --history --list-runs
./mmltk --logs --family latest-wayland-test --run current --errors
./mmltk --logs --family latest-wayland-test --errors --related-run --tail
./mmltk --logs build/validation/viewer-copy-ownership-trace.log \
  --family latest-wayland-test --history -q 'buffer="BufferId(21,1)"' --context 2
```

Use a returned archive ID with `--run` to select a historical capture;
`--run` implies `--history`. `--family STEM` selects the `.jsonl`, `.log`,
`-native.log`, and `-firefox.log` siblings under `build/validation`, or under an
explicit repository path. Families are repeatable. `--history` adds rotated
`.history` siblings.

Native, Firefox, and trace rotations sample their IDs independently. Adjacent
same-PID rotations within 10 ms are grouped and labeled inferred. Identical
event/steady-time anchors join transcript segments to native captures and
propagate known tests/tags; missing anchors leave them separate. IDs and
monotonic timestamps may repeat between runs, so select one reproduction.
`--related-run` includes other records from a matched run.

Inputs are repository-relative/absolute paths or quoted globs inside the
repository. Explicit files may have any suffix. Directories select
`.jsonl`, `.log`, `.out`, and `.txt` files; `--recursive` includes nested
histories. Repeated files are deduplicated. The default is `build/validation`
without recursive archived captures.

## Triage conventions and limits

| Option | Default | Range |
| --- | --- | --- |
| `--triage-anchors` | 4 | 1–12 |
| `--triage-identities` | 12 | 1–64 |
| `--triage-hops` | 2 | 0–3 |
| `--triage-gap-ms` | 1000 | 1–600000 |
| `--near-ms` | 1000 | 0–600000 |
| `--limit` | 20 | 1–10000 |
| `--top` | 5 in triage, 10 otherwise | 1–100 |

Triage follows strong run-qualified surface IDs, typed handles, nonempty
`*_id`/`*_identity` fields, and source-session composites. Ambient process,
thread, device, user, and session IDs are excluded unless explicitly selected
with `--correlate`. Related identities co-occurring on evidence can seed the
next hop. Sequence/generation/slot values only join as nearby multi-field
composites; they do not start further identity expansion.

Generic event suffixes identify possible unclosed starts, duplicate endings,
failed textual outcomes, incomplete stages, and handoffs. Lifecycle balances
use the most specific operation/resource identity and exact event families.
Hand-off hypotheses require shared identities without conflicting operation
IDs. Copied INFO does not inflate balances; numeric outcome enums are not
guessed. These are investigative leads: disabled diagnostics, truncated
captures, or a completion in another source may explain missing counterparts.

Gap findings require comparable timestamps in the same physical source.
Triage also reports bounded terminal evidence independently of the row budget;
without terminals, it shows source ends without inferring successful exit,
timeout, or failure. Internal identity/event/payload capacities are bounded,
and reports state when those limits truncate evidence.

`--triage` rejects `--related-run`, `--tail`, and `--list-runs`. In JSONL mode,
only bounded evidence rows go to stdout; triage summaries go to stderr.

## Time, output, and exit status

Ordinary time ordering groups steady, UTC wall, timezone-free wall, per-file
elapsed, and untimed records separately. It does not guess clock offsets.
`--order file` preserves physical ordering; `--order capture` orders files by
captured modification time, then by line. Modification time describes an
artifact, not causality. `--tail` selects the last records of the chosen order
and prints them ascending.

Pasted-error lookup uses `--order proximity` by default. Same-clock distances
use recorded times. Cross-clock estimates align each source's last timestamp
with its file modification time; untimed rows may borrow a preceding timestamp
for proximity only. `@time_link` labels these estimates. Proximity ordering
requires `--error` and does not establish causal order.

`--format summary`, `timeline`, and `jsonl` control output.
`--fields` projects comma-separated fields while retaining provenance;
`--group-by` controls summary groups. `--context` includes up to 100 nearby
nonblank records on either side in the same file. Counts cover all selected
records even when `--limit` bounds the displayed sample. Group/correlation
cardinality is capped at 10,000.

Malformed or oversized records remain searchable with warnings.
`--strict` makes malformed/truncated input an error. Status 0 indicates
successful output, 1 indicates no matches, and 2 reports invalid queries,
unreadable/changing inputs, or strict parse failures.

The tool uses the existing build image, no GPU or network, and a read-only
repository mount. It reads files only up to captured sizes; multi-pass
correlation requires unchanged inputs. There is no follow mode or persistent
index. Implementation and current help live in
[tools/log_query.py](../tools/log_query.py). Run
`./mmltk --test log-query-tool` during the permitted testing stage for its
fixture suite.
