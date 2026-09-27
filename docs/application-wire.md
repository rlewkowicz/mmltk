# Generated Rust and application CBOR

[Wiki index](README.md) · [Declaration authoring](reflection.md) · [Generation and artifacts](build.md#generated-bindings-and-dependency-maintenance) · [GUI interaction](gui-interaction.md#typed-application-boundary)

This page owns the native-to-Rust schema projection, application record format,
and connection admission. Protocol **17** is a package boundary: the native
host, generated bindings, Iced bundle, and cross-language fixtures must agree.
The graphics ABI is an independent boundary; saved settings and annotation
documents retain their own named persistence formats.

## Canonical declarations and generator ownership

[ApplicationSystems](../src/controller/contracts/application_systems.h) is the
composition root for the application schema. Its named, nonowning system
pointers identify the native owners; their annotated methods, snapshot types,
and event variants define the exposed application vocabulary. The shell owns
the actual systems. Rust owns component state, layout, copy, styling,
navigation, and interaction behavior.

[application_schema.h](../src/controller/browser/application_schema.h) validates
and projects these declarations through C++26 reflection.
[application_materializer.h](../src/controller/browser/application_materializer.h)
uses that schema for native request decoding, direct endpoint dispatch,
snapshots, replies, events, and compact interactions. Neither file scans source
text or supplies a second member inventory. The reusable declaration syntax
and annotation rules belong in [reflection.md](reflection.md).

The compiled C++ executable
[application_binding_generator.cpp](../src/controller/browser/application_binding_generator.cpp)
emits Rust from the same materialized schema. Its responsibilities include:

- Native objects, enums and variants; named and transport codecs; constraints,
  defaults, settings/catalog relationships, and presentation metadata.
- Stable system, endpoint, event and field identities, typed request builders,
  compact interaction opcodes, and the schema fingerprint.
- Exhaustive snapshot/event/reply types, bootstrap completeness and projection
  traits through [the outer-routing emitter](../src/controller/browser/application_outer_routing_emitter.h).
- Visual source/geometry observations through
  [the visual emitter](../src/controller/browser/application_visual_projection_emitter.h),
  and typed training/evaluation scalar selectors. Rust selects these native
  facts for display without repeating their structural inventories.
- The separate data-only Firefox graphics projection through
  [the workspace ABI emitter](../src/controller/browser/application_workspace_abi_emitter.h).

Booleans, fixed-width integers and floating-point values retain typed Rust
equivalents. Native strings and paths project to `String`; static catalog text
can use `Cow<'static, str>`. Optionals become `Option`, fixed arrays retain their
length, and variable/fixed-capacity sequences use `Vec` with generated bounds.
Byte buffers and byte arrays use distinct byte-string wrappers. Structs and
variants get generated codecs, including inherited reflected members. Unsupported
shapes and projected Rust symbol collisions fail generation.

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
against declaration-only composition/schema dependencies and serialization.
Its custom command depends on canonical declarations and emitter inputs.
The generator writes temporary files, flushes them, then renames the graphics
projection, bindings, and package marker into place. Each replacement is atomic;
the three files are not one filesystem transaction. The executable reports
generation exceptions with `application binding generation failed` and exits
unsuccessfully.

[Frontend CMake](../src/frontend/iced/CMakeLists.txt) supplies the binding path
to Cargo as `MMLTK_APPLICATION_BINDINGS_PATH`;
[generated.rs](../src/frontend/iced/src/generated.rs) includes that artifact.
The native fixture generator emits server records. The Rust
[client fixture executable](../src/frontend/iced/src/bin/protocol_v17_client_fixture.rs)
consumes the generated boundary and emits client records. Both wrapper
generation commands select their shared `mmltk_protocol_v17_generation`
dependency target. The [build reference](build.md#generated-bindings-and-dependency-maintenance)
owns exact commands, graph/output locations, fixture filenames, packaging and
Firefox cache invalidation.

Generated Rust stays in build output. Change the native declaration, projection,
or build dependency, then regenerate through `./mmltk`. Generation produces
artifacts; it does not establish test or packaged-runtime acceptance.

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

The application shell configures the server's outbound capacity from the
32 MiB record ceiling. Snapshot declarations have their own per-system bounds;
generation statically checks the aggregate Bootstrap structural bound against
that ceiling. Bounds do not preallocate maximum-sized payload graphs.

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

Native [record tests](../src/controller/browser/tests/client_record.test.cpp),
[schema tests](../src/controller/browser/tests/application_browser_contract.test.cpp),
[host tests](../src/controller/browser/tests/application_browser_host.test.cpp),
serialization tests, and Rust protocol tests exercise malformed encodings,
budgets, projected constraints, compact interactions, complete Bootstrap,
delivery, and peer continuity. Native server and Rust client fixtures cross the
language boundary using generated artifacts. These are coverage locations,
not evidence that a particular checkout or package passed; the
[validation reference](validation.md) owns execution and acceptance.
