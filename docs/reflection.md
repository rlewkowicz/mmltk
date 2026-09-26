# Reflected declarations and authoring

[Wiki index](README.md) · [Architecture](architecture.md#reflected-value-and-persistence-boundaries) · [Generated bindings](build.md#generated-bindings-and-dependency-maintenance) · [Declaration tooling](validation.md#raw-cpd-and-declaration-formatting)

Canonical C++ declarations own native vocabulary, defaults, constraints, field
identity, and structural projections. Ordinary systems own resources, execution,
and product policy; Rust/Iced owns presentation and interaction, as defined by
[the contract](../CONTRACT.md#model-presentation-model-and-views).

## Declare once, project structurally

Declare canonical types beside their systems in self-contained ordinary
headers with direct dependencies. Materialize reflection there; publish ordinary
types, functions, constants, or reusable template specializations so consumers
need neither re-reflection nor include-order assumptions.
[CMake registration](build.md#target-declarations-and-precompiled-headers) owns
source/header membership and generation inputs, without a parallel scanner or
field inventory.

[reflected_field_policy.h](../src/frameworks/reflection/reflected_field_policy.h)
provides `MMLTK_REFLECT_FIELDS(Type)`;
[reflection_metadata.h](../src/frameworks/reflection/reflection_metadata.h)
provides `MMLTK_REFLECT_ENUM(Type)`. Place the registration at namespace scope
after the complete type, in its namespace. Each defines a typed materialization
overload found through argument-dependent lookup; members, bases, annotations,
and enum values derive from C++26 reflection, without a second inventory. The
[field policy](../src/frameworks/reflection/field_policy.h) owns constraints and
presentation/catalog annotations. Serialization, generated
Rust, validation, settings traversal, and exhaustive dispatch consume those
canonical structures, rather than handwritten member-wise or string-keyed
mirrors. A genuinely different external format gets its own explicit conversion.

For example:

```cpp
#pragma once
#include <filesystem>
#include "src/frameworks/reflection/declaration_annotations.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"

namespace example {
struct ExportOptions final {
 MMLTK_MAX_PATH_BYTES std::filesystem::path output;
 MMLTK_MINIMUM(double, 0.0) MMLTK_MAXIMUM(double, 1.0)
 MMLTK_FINITE double threshold = 0.4;
 bool operator==(const ExportOptions&) const = default;
};
MMLTK_REFLECT_FIELDS(ExportOptions)
}  // namespace example
```

The struct, initializer, equality, and registration remain explicit C++.
Register its header with the owning CMake target and include its dependencies
directly. Declaration consumers use the owner's compile requirements;
[header isolation](build.md#target-declarations-and-precompiled-headers) checks
that the header does not rely on a PCH or a preceding include.

## Short annotation syntax

[declaration_annotations.h](../src/frameworks/reflection/declaration_annotations.h)
is a syntax-only header directly over `field_policy.h`. Each macro expands to
the same absolutely qualified annotation type and braced initialization that a
long declaration would use. Values occur once, and normal type checking and
narrowing rules remain in force.

These are the twelve public annotation authoring macros. Apply them immediately
before the canonical data member; their spelling introduces no runtime storage
or execution policy.

| Spelling | Canonical annotation | Use and admission |
| --- | --- | --- |
| `MMLTK_MAX_BYTES(n)` | `MaxBytes{n}` | Bound string/path bytes, byte sequences, each text-sequence element, or an admitted bounded dynamic leaf. |
| `MMLTK_MIN_BYTES(n)` | `MinBytes{n}` | Require a minimum string/path or byte-sequence length; it does not apply to a text sequence's elements. |
| `MMLTK_MAX_ITEMS(n)` | `MaxItems{n}` | Bound container elements or a bounded dynamic leaf's item budget. An `inplace_vector` limit cannot exceed its physical capacity. |
| `MMLTK_MINIMUM(T, value)` | `Minimum<T>{value}` | Set an inclusive lower bound on a non-boolean arithmetic member using the explicit annotation value type. |
| `MMLTK_MINIMUM_VALUE(value)` | `Minimum{value}` | The same lower bound with the annotation value type deduced from the expression. |
| `MMLTK_MAXIMUM(T, value)` | `Maximum<T>{value}` | Set an inclusive upper bound on a non-boolean arithmetic member. |
| `MMLTK_FINITE` | `Finite{}` | Reject NaN and infinity on floating members; required by reflected ingress completeness. |
| `MMLTK_RUNTIME_DESTINATION` | `RuntimeDestination{}` | Mark a concrete request output selected by a workflow. Settings vocabulary excludes it from generic mutable leaves; it supplies no path bound or directory reservation. |
| `MMLTK_MAX_PATH_BYTES` | `MaxBytes{kMaximumPathBytes}` | Use the canonical 4096-byte path limit instead of a local copy. |
| `MMLTK_MAX_NAME_BYTES` | `MaxBytes{kMaximumNameBytes}` | Use the canonical 256-byte name limit instead of a local copy. |
| `MMLTK_PRESENTATION(kind)` | `Presentation<kind>{}` | Attach a canonical `PresentationKind` hint such as `Preset` or `LearningRate`. Rust still owns visual copy and layout. |
| `MMLTK_CATALOG(Provider)` | `CatalogProvider<Provider>{}` | Attach the typed native catalog owner. Browser projection requires a valid provider, a reflected row type, and a supported static row shape. |

The path/name limits remain owned by `field_policy.h`.
Presentation and catalog macros accept template-argument tails; value tails
can contain commas. For a comma-containing `Minimum`/`Maximum` type, declare a
scoped type alias and pass that alias as the first argument. Do not introduce
another macro family or mirror the canonical limits locally.

Field-policy admission unwraps `std::optional`. It rejects duplicate numeric,
finite, byte/item, presentation, and catalog annotations, reversed bounds, and
nonzero byte/item limits on unsupported shapes. A zero byte/item limit means
no effective limit; it is not a zero-capacity budget. Completeness additionally
requires nonzero maximum bytes for strings, paths, and dynamic byte storage;
text sequences need a per-element byte bound and a declared or fixed item
capacity. Other dynamic sequences need an item bound, and bounded opaque dynamic
leaves need both budgets. `reflected_policies_are_valid`,
`reflected_policies_are_complete`, and `reflected_defaults_are_valid` are separate
checks consumed by the owning boundary. Registration alone does not promise
that every possible boundary admits a declaration.

`RuntimeDestination` is an ownership marker, deliberately outside
`kPolicyAnnotation`. Use it together with the ordinary path constraint when
appropriate, as in `TrainRequest::output_dir`. The
[settings vocabulary](../src/controller/contracts/settings_vocabulary.h) owns
its mutation rule; [workflow output](rfdetr-workflows.md#run-output-directories)
owns destination selection. Other annotation types such as fixed text,
endpoints, workflow scopes, and file dialogs retain their own explicit syntax
and owners; these twelve macros do not define an application schema language.

### Detection and schema identity

The field-policy materializer uses constrained `MinimumAnnotation`,
`MaximumAnnotation`, `MinBytesAnnotation`, `MaxBytesAnnotation`,
`MaxItemsAnnotation`, `FiniteAnnotation`, and `PresentationAnnotation` predicates.
Value predicates require the marker and `value` to exist, then preserve the
marker's constant-expression truth and the declared value type's implicit
convertibility to `long double` or `std::size_t`. They do not replace that test
with conversion from a const lvalue. Missing members fail the constraint;
malformed present markers still produce a compilation error. Presentation
detection checks the declared `kind` type's convertibility to `PresentationKind`.
Catalog detection retains the canonical `CatalogProvider` specialization.

Typed values, marker presence, and annotation visitation order feed the existing
[application fingerprint](../src/controller/browser/application_schema.h).
Authoring changes must preserve the materialized declarations and deployed encoding.
That encoding asks reflection for the identifier of its local `A` alias; the
current compiler reports `A`, rather than the underlying annotation's descriptive
name. Preserve this observed encoding when reusing aliases. The
[fingerprint fixture](../src/controller/browser/tests/application_browser_contract.test.cpp)
checks it together with typed values and order.

## Compact CLI declarations

[cli_declarations.h](../src/frameworks/reflection/cli_declarations.h) supplies
`CliScope<Request, Owner = Request, Prefix...>`. A scope binds the request type,
the current member owner, and a typed prefix using the existing
`MemberPathAccessor`. `Scope::within<&Owner::member>` descends into a member;
`Scope::path<&Owner::member, ...>` exposes the complete accessor to existing
factories and presence checks. It creates no command tree or runtime registry.

| Macro | Expansion and naming |
| --- | --- |
| `MMLTK_CLI_OPTION(Scope, member, help, ...)` | Calls the ordinary `option` factory with `--` plus the reflected terminal member name, replacing `_` with `-`. |
| `MMLTK_CLI_NAMED(Scope, member, name, help, ...)` | Calls the same factory with the explicit public spelling. |

After `help`, both pass through the existing optional `group`, `alias`,
`negated_name`, `required`, and `environment` arguments in that order. Defaults,
scalar types, constraints, enum admission, assignment, emission, and presence
remain owned by the canonical member and
[reflected_descriptors.h](../src/frameworks/reflection/reflected_descriptors.h).
Derived names use only the terminal field, not the enclosing scope. Name storage
is materialized at compile time with `std::define_static_string`.

Examples from the [root options](../src/entrypoints/cli/cli_options.h):

```cpp
using Compile = reflection::CliScope<data::CompilerConfig>;
inline constexpr std::array kCompileOptions{
 MMLTK_CLI_OPTION(Compile, resize_mode, "Image geometry: Stretch or Letterbox", "Dataset"),
 MMLTK_CLI_OPTION(Compile, source_dir, "Source dataset directory", "Dataset", "source_dir", {}, true),
 MMLTK_CLI_NAMED(Compile, target_width, "--width", "Target image width", "Dataset"),
};
```

This excerpt emits `--resize-mode` and preserves the exact `source_dir` alias
and required bit. `target_width` remains `--width`, through the named form.
Keep exceptions such as `weights_path` → `--weights`, `compilation_mode` →
`--compile-mode`, and desktop `cuda_device_index` → `--device-id` explicit.
Aliases and negations are exact declared spellings, not derived synonyms.

Nested training authoring uses ordinary aliases:

```cpp
using TrainInput = reflection::CliScope<TrainCliRequest>;
using Train = TrainInput::within<&TrainCliRequest::request>;
using Recipe = Train::within<&TrainRequest::recipe>;
// Inside the existing kTrainOptions array:
MMLTK_CLI_OPTION(Recipe, nesterov, "SGD Nesterov momentum", "Optimization", {}, "--no-nesterov")
```

Both boolean spellings refer to one descriptor and one presence bit.
`negative_flag` remains explicit for inverse polarity such as `--gdrcopy`
setting `h2d_dataloader` false. `custom_option` retains the bounded
comma-separated `--device-ids` and `--numa-nodes` codecs;
`option_with_item_policy` retains `--image` collection capacity plus its
canonical path-element policy. Their scope paths remove qualification
repetition without hiding the codec or policy choice.

Declare options in their intended help/emission order. Use
`unexposed<Request, Access>(reason)` for a policy-bearing field owned elsewhere,
then `audit_descriptors(options, exclusions)`. The audit rejects spelling or
member-identity collisions, invalid descriptors, invalid/unmatched exclusions,
and policy-bearing members without exactly one exposure decision. It traverses
reflected bases and eligible aggregates. Its policy trigger is a numeric bound,
finite marker, nonzero maximum bytes, or nonzero maximum items; it is not a rule
that every unannotated member must become a flag. Training also audits its
recipe-provider relation and single/list device selectors against the table.

The complete production declarations live in `cli_options.h`,
[rfdetr_cli_options.h](../src/entrypoints/cli/rfdetr_cli_options.h), and
[browser_runtime_options.h](../src/entrypoints/desktop/browser_runtime_options.h).
Their CMake owners register the headers; the CLI owner also registers isolation
checks for its new declaration headers. Ordinary option additions belong in
those declarations; special request finalization stays with the command owner.
For example, root compile fills an absent height from an
explicit width by canonical member presence, as described in
[dataset compilation](datasets.md#compile-and-inspect).

## Local repetition and aliases

Use existing scoped `using` declarations and type aliases when they express the
same owner or template application. The application binding generator reuses
its existing reflected object/optional/sequence/variant aliases. Settings use
the existing materialized-member visitor for complete same-name records,
including `ExploreViewState`; renamed keys and conversions remain explicit in
[settings persistence](architecture.md#reflected-value-and-persistence-boundaries).

Purposeful local macro families retain narrow owners and are undefined after
use:

| Owner | Local family and purpose |
| --- | --- |
| `rfdetr_cli_options.h` | `MMLTK_AUGMENTATION_OPTIONS` emits probability, minimum-strength, and maximum-strength descriptors for the six ordered geometry/resize/color/noise/blur/occlusion groups. |
| `reflected_field_policy.h` | `MMLTK_VALUE_ANNOTATION` defines the five value predicates with the same marker/convertibility contract. |
| `application_schema.h` | `MMLTK_SYSTEM_METHOD_SIGNATURE` defines zero/one-request member signatures for ordinary, const, noexcept, and const-noexcept methods. |
| [onnx_lowering.cpp](../src/backend/models/rfdetr/export/onnx_lowering.cpp) | `MMLTK_ONNX_LOWERING_TABLE` and `MMLTK_LOWER_*` emit the existing typed lowering callbacks for node, context, symbol, creation, and resize entries. |
| [ms_deform_im2col_cuda.cuh](../src/backend/ml/layers/detail/ms_deform_im2col_cuda.cuh) | `MMLTK_ML_LAYERS_*` shares fixed CUDA parameter, body, launch, and specialized dispatch syntax while ordinary functions/templates retain the algorithms. |

These implementation-local expansions are not extension points for a universal
DSL. Prefer an existing ordinary owner or a small constexpr function when it
already represents the repeated behavior.

## Safe mechanical formatting

[Declaration formatting](validation.md#raw-cpd-and-declaration-formatting)
provides separate check, preview, and fix modes. Its scanner can prove a
canonical policy spelling only when it is absolute, or uses an absolute global
alias whose target is the absolute canonical namespace:

```cpp
namespace policy = ::mmltk::frameworks::reflection;
// Both spellings can be rewritten safely:
[[= ::mmltk::frameworks::reflection::MaxBytes{64}]]
[[= ::policy::MaxBytes{64}]]
```

Relative lookup such as `mmltk::frameworks::reflection::MaxBytes` or
`policy::MaxBytes` stays manual, even if a local scan sees no competing symbol:
an included header can change lookup. Conditional dependencies, annotation
comments, conflicting macros, and unproven syntax likewise remain explicit
manual findings. Preview the report rather than treating lexical repetition
as proof of a shared algorithm. Raw CPD evidence and ordinary behavioral cleanup
are distinct tools with distinct filters.
