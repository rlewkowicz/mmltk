# Generated Rust and application CBOR

[Wiki index](README.md) · [Declaration authoring](reflection.md) · [Generation and artifacts](build.md#generated-bindings-and-dependency-maintenance) · [GUI interaction](gui-interaction.md#typed-application-boundary)

Protocol **17** requires matching native host, generated bindings, Iced bundle
and cross-language fixtures. This page defines schema projection, records and
connection admission. Graphics ABI, saved settings and annotation documents
have independent formats.

## Canonical declarations and generator ownership

[ApplicationSystems](../src/controller/contracts/application_systems.h) names
nonowning system pointers. Their annotated methods, snapshots and event variants
define native vocabulary; the shell owns the systems, Rust owns presentation.
[Schema validation](../src/controller/browser/application_schema.h) and
[materialization](../src/controller/browser/application_materializer.h) derive
request decoding, direct dispatch, snapshots, replies, events and compact
interactions through C++26 reflection. [Reflection authoring](reflection.md)
defines declaration syntax; there is no source scanner or parallel inventory.

The compiled [binding generator](../src/controller/browser/application_binding_generator.cpp)
emits:

- Objects, enums, variants, named/transport codecs, bounds, defaults,
  settings/catalog relationships and presentation metadata.
- Stable system/endpoint/event/field IDs, request builders, interaction opcodes
  and the schema fingerprint.
- Exhaustive [outer routing/projection traits](../src/controller/browser/application_outer_routing_emitter.h)
  and Bootstrap completeness.
- [Visual observations](../src/controller/browser/application_visual_projection_emitter.h)
  and typed training/evaluation scalar selectors.
- The separate data-only [Firefox graphics ABI](../src/controller/browser/application_workspace_abi_emitter.h).

Booleans, fixed-width integers and floating-point values retain typed Rust
equivalents. Native strings and paths project to `String`; static catalog text
can use `Cow<'static, str>`. Optionals become `Option`, fixed arrays retain their
length, and variable/fixed-capacity sequences use `Vec` with generated bounds.
Byte buffers and byte arrays use distinct byte-string wrappers. Structs and
variants get generated codecs, including inherited reflected members. Unsupported
shapes and projected Rust symbol collisions fail generation.

The canonical [model selection key](../src/controller/contracts/model_selection_types.h)
contains workflow, source, input kind, preset, resolution, scratch mode and
class-layout path. [Member relations](../src/controller/contracts/model_selection.h)
cover every reflected key field exactly once. Train projects `from_scratch` from
its request; the other workflows supply a typed constant false. Native projection,
generated Rust selection, settings field identities and dialog derivation consume
that same relation. Constants have no invented editable settings field. Artifact
path and inspection device remain separate projected facts; a draft projection
does not authorize a prepared model. Iced compares the complete generated identity
when retaining preparation, continuation and pending actions.

Schema validation checks endpoint signatures, projectable types, field
constraints, defaults, catalog and settings relationships, visual projections,
and identity collisions. An endpoint is an annotated public ordinary instance
method, with zero or one by-value request and a by-value result. An interaction
requires one request and a void result; it cannot also be an intent. Snapshots
have declared bounded payloads. Each event belongs to its owner's reflected
variant and has exactly one supported delivery policy; latest-state events
require a state revision. Fixed-text rules include their declared capacity,
termination/tail, and character constraints.

Stable identities derive from canonical owner/member names; request field IDs
derive from the endpoint and field name, and settings IDs from their paths.
The two-word schema fingerprint covers more than the protocol integer: it
includes types, field order and constraints, defaults, endpoint and event
policies, compact opcode order, catalogs/settings relationships, and visual
projections. A declaration rename or reordered positional layout can therefore
change compatibility even while the package protocol remains 17.

## Generation and consumption

[Browser CMake](../src/controller/browser/CMakeLists.txt) compiles the generator
against declaration-only schema/composition and serialization dependencies.
Canonical declarations and emitters are custom-command inputs. The generator
writes temporary files, flushes, then atomically renames each graphics projection,
bindings file and package marker. The three replacements are not one transaction.
Exceptions report `application binding generation failed` and exit unsuccessfully.

[Frontend CMake](../src/frontend/iced/CMakeLists.txt) sets
`MMLTK_APPLICATION_BINDINGS_PATH`; [generated.rs](../src/frontend/iced/src/generated.rs)
includes it. Native fixtures emit server records; the generated-boundary
[Rust fixture](../src/frontend/iced/src/bin/protocol_v17_client_fixture.rs) emits
client records. Both wrapper generation commands use
`mmltk_protocol_v17_generation`. [Build generation](build.md#generated-bindings-and-dependency-maintenance)
owns commands, paths, fixtures, packaging and Firefox invalidation. Generated
Rust stays in build output: change canonical inputs and regenerate through
`./mmltk`. Generation alone does not establish runtime acceptance.

## Record shapes

[client_record.h](../src/controller/browser/client_record.h) owns the outer
vocabulary and budgets;
[client_record.cpp](../src/controller/browser/client_record.cpp) owns its codec.
Ordinary records use the named CBOR envelope
`{"kind": "RecordName", "payload": {...}}`. The writer emits `kind` before
`payload`, and the Rust server-record preflight requires that prefix. Payload
members use their canonical snake_case names. The notation here describes CBOR
values, not JSON bytes.

| Record | Direction | Named payload members and meaning |
| --- | --- | --- |
| `Intent` | Client → native | `protocol_version`, nonzero `correlation`, nonzero `endpoint_id`, `fields`; each field is `{field_id, value}` with a unique nonzero ID |
| `Bootstrap` | Native → client | `protocol_version`, two-word `schema_fingerprint`, nonzero `input_epoch`, and `snapshots` containing unique `{system_id, value}` records |
| `IntentReply` | Native → client | `protocol_version`, `correlation`, and exactly one of `result` or `error`; absent optional members are omitted |
| `SystemEvent` | Native → client | `protocol_version`, `system_id`, `event_id`, `delivery`, `state_revision`, and `value` |
| `InteractionRejected` | Native → client | `protocol_version`, `endpoint_id`, and `error` |
| `IntegrationControl` | Both, with direction checks | `protocol_version` and the canonical integration `receipt`; handled by the installed acceptance integration gate |

An error is `{category, detail}`. Categories are the declared
`InvalidIntent`, `Busy`, `Unavailable`, and `Failed` values. A successful void
intent returns an empty object as its result. A reply's pending correlation
identifies the generated result type; the reply does not carry another endpoint
inventory. Unknown request fields, duplicate identities, missing required
values, unknown record members, invalid constraints, or incompatible types are
rejected.

Inside snapshot/event `value` and reply `result` fields, reflected domain objects use
**positional arrays in canonical member order**, including a null slot for each
absent optional. Generated decoders enforce the exact number of members and
their constraints. Nested structs follow that projection; enums use their
canonical symbol as CBOR text, and variants use named `kind`/`payload` maps
with the canonical alternative name. Opaque relation storage retains
its defined named representation. Ordinary intent field values use the named
request projection. This distinction is implemented by
[reflected_cbor.h](../src/frameworks/serialization/reflected_cbor.h), rather than
by handwritten field lists. Transport positions do not change persisted files.

Interactions use a different top-level form: `[opcode, payload_bytes]`.
`opcode` is the zero-based interaction position derived from canonical endpoint
order; the byte string contains the generated compact request encoding.
Bootstrap already established protocol/schema identity, so this envelope does
not repeat either the protocol version or the stable endpoint ID. The native
codec resolves the opcode back to the endpoint and decodes a borrowed byte
view. Its owned fixture/diagnostic route uses that same ingress decoder.
A named `Interaction` envelope is deliberately rejected at ingress.

Within the compact request, reflected objects are exact-length member arrays,
optionals are null or their value, enums use their declared underlying integer,
and variants are `[alternative_index, value]`. Fixed arrays and bounded
`inplace_vector` sequences remain arrays; numeric/boolean scalars retain CBOR
scalar encodings. This restricted shape excludes strings, arbitrary vectors,
byte-sequence members and opaque relations. Schema generation rejects an
unsupported interaction request rather than providing a dynamic fallback.

Workspace mouse fields retain fractional coordinates, optional coordinates for
an empty workspace, modifiers, buttons, click counts, wheel units/deltas, and
source/peer/document identity. Replaceable interaction scheduling is local
queue policy, not an extra wire bit. The [GUI input reference](gui-interaction.md#shared-immediate-workspace-input)
owns ordering and retained component behavior.

## Dataset operation snapshots

[artifact.h](../src/controller/contracts/artifact.h) declares
`ArtifactCompileState` with `revision`, `generation`, `active`, `progress`, and
`terminal`. `ArtifactUiState` structurally extends it with `inspection`.
The generator emits both the borrowed projection and `apply_to` conversion;
handwritten Rust retains no second member inventory.

[DatasetSystem](../src/controller/subsystems/system/dataset_system.h) publishes
`DatasetProgress { snapshot: ArtifactCompileState }` as LatestState, using its
native revision for transport coalescing. `DatasetChanged` remains a critical
full snapshot. Bootstrap and operation replies also carry full state; progress
does not copy inspection products. Dataset revisions advance for accepted
compile, changed progress, cancellation request, and terminal settlement.

[DatasetCompileTrack](../src/backend/data/compiler/dataset_compile_progress.h)
includes its canonical `DatasetCompileUnit` (`Items`, `Bytes`, `Images`, or
`Rows`), projected into generated Rust with the counters and known-total flag.
The unit determines presentation; the track's position does not imply bytes or
images. Native admission still checks enum bounds, completed work against a
known total, and compatible active/complete state. The
[progress reference](benchmark-datasets.md#reading-compilation-progress) owns
row/component accounting, unknown totals, and display rules.

Both native and Rust pending queues replace an older latest-state record by
appending the newer record after intervening discrete events. Older/equal
queued revisions are discarded. The
[Dataset reducer](../src/frontend/iced/src/view_model/reduction.rs) additionally
checks operation generation, revision, and terminal rank: late progress cannot
revive a finished operation, undo Stop, or overwrite a new generation. Conflicting
facts at one revision or incompatible final outcomes are protocol errors.
A full snapshot at the same compile revision can still refresh inspection.
Reconnect admits current Bootstrap state through the ordinary schema boundary.

The [compiler observer](compile-execution.md#progress-delivery) coalesces ordinary
updates before snapshot copying and serialization. Native state revisions are
product identity; trace sequences and rendering receipts remain diagnostics.
These declaration changes affect the schema fingerprint while retaining protocol
17, so native host and generated client must be packaged together.

## CBOR encoding, limits and decoding

The native [CBOR implementation](../src/frameworks/serialization/cbor_wire.cpp)
and Rust [record codec](../src/frontend/iced/src/protocol/cbor.rs) support null,
booleans, signed/unsigned 64-bit integer domains, finite floating-point values,
UTF-8 text, byte strings, arrays, and maps with text keys. They use definite
lengths, minimally encoded integers/lengths, and the smallest exactly
representable half/single/double float. Nonnegative signed values encode as
unsigned CBOR integers. Declaration-order maps do not imply lexicographic map
sorting.

Decoders reject nonminimal forms, indefinite containers, tags, unsupported
simple values, nonfinite floats, invalid UTF-8, duplicate keys, overflow,
trailing bytes, and budget violations. Typed decoding additionally enforces
declared scalar ranges, collection limits, enums and object shapes. The native
reader accepts up to two input segments and can borrow payload spans; owners
must preserve that input lifetime. Encoding measures size before reserving
storage. Rust preflights record structure and budgets before constructing the
owned value graph, then performs typed projection.

| Boundary | Ceiling |
| --- | --- |
| Complete codec record | 32 MiB encoded bytes; at most 33,554,432 items |
| Individual output value | 8 MiB bytes and 8,388,608 items, further restricted by its declared schema |
| Individual intent field / compact request | 65,536 bytes; decoded input values at most 1,024 items |
| Nesting | Maximum depth 64, with the root at depth zero |
| Intent fields / Bootstrap snapshots | 64 fields / 32 snapshots |
| Error detail | 512 bytes |
| Native WebSocket inbound message | 8 MiB, nonempty binary messages only |

The shell sets outbound capacity from the 32 MiB ceiling. Per-system snapshot
bounds and aggregate Bootstrap bounds are checked at generation; they do not
preallocate maximum-sized payloads.

Generated failures retain type/member context and array indexes. Bootstrap and
event projection add system/event context. The low-level native codec retains
error code, offset and path where applicable; the outer native record API
returns its bounded `RecordCodecError` code. Native application error mapping
normalizes detail to printable ASCII, replaces other bytes with `?`, truncates
at its limit, and provides a fallback failure message.

## Connection, compatibility and failure behavior

[BrowserServer](../src/frameworks/transport/browser_server.cpp) listens on an
ephemeral loopback port. The local page receives the WebSocket URL
`ws://127.0.0.1:<port>/mmltk?session=<token>`; upgrade checks the session token
and exact expected local Origin. Invalid admission receives HTTP 403. One peer
is active; replacement closes its predecessor, and connection-generation
checks keep stale callbacks from acting on the replacement.

On open, [ApplicationBrowserHost](../src/controller/browser/application_browser_host.cpp)
publishes a critical Bootstrap with current snapshots, schema identity, and a
new nonzero input epoch. Visual input peers activate after that Bootstrap is
accepted for output. The Rust [connection owner](../src/frontend/iced/src/transport_connection.rs)
holds outgoing application work until Bootstrap establishes the epoch and
stamps mouse input from it. [Record decoding](../src/frontend/iced/src/protocol/records.rs)
checks protocol equality, exact fingerprint, known unique snapshots and record
constraints; generated completeness is checked before native state is installed.
There is no version negotiation or old-schema translation route.

Ordinary intent domain failures return a correlated `IntentReply.error`.
An interaction protocol failure rejects the message and closes the peer with
WebSocket code 1002. A valid interaction rejected by its owner publishes
`InteractionRejected`; essential workspace mouse rejection also loses continuity
and closes the peer. Unknown/malformed records fail the connection rather than
silently becoming commands. Rust decode or continuity failures retire the peer
and reconnect; new Bootstrap state authorizes the new connection.

Transient events may be dropped under output pressure. Critical output must
retain continuity; latest-state output may coalesce by its declared identity
and revision. Failure to encode or publish required output closes the peer so
the next connection obtains authoritative snapshots. Closing retires native
input peer state and the Rust connection's pending presentation/transport state.

Completed images travel on the separate FD graphics channel with immutable
`WorkspaceImageMetadata`: fingerprint, paired visual frame geometry, product
snapshot and optional source snapshot. Firefox carries those bytes opaquely.
The Iced [metadata decoder](../src/frontend/iced/src/presentation_surface/metadata.rs)
validates schema, image identity and product/source pairing before installation.
Its custody and retained-image behavior belong in the
[presentation reference](gui-interaction.md#typed-application-boundary).

## Boundary coverage

[Record](../src/controller/browser/tests/client_record.test.cpp),
[schema](../src/controller/browser/tests/application_browser_contract.test.cpp),
[host](../src/controller/browser/tests/application_browser_host.test.cpp),
serialization and Rust protocol tests cover encoding, budgets, constraints,
interactions, Bootstrap, delivery and continuity. Native/Rust fixtures cross the
generated boundary. [Validation](validation.md) owns execution and acceptance.
