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

[reflection_metadata.h](../src/frameworks/reflection/reflection_metadata.h)
provides `MMLTK_REFLECT_FIELDS(Type)` and `MMLTK_REFLECT_ENUM(Type)` registration.
They derive members and enum values through C++26 reflection. The
[field policy](../src/frameworks/reflection/field_policy.h) and its
[materialization](../src/frameworks/reflection/reflected_field_policy.h)
own constraints and presentation/catalog annotations. Serialization, generated
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

## Short annotation syntax

[declaration_annotations.h](../src/frameworks/reflection/declaration_annotations.h)
is a syntax-only header directly over `field_policy.h`. Each macro expands to
the same absolutely qualified annotation type and braced initialization that a
long declaration would use. Values occur once, and normal type checking and
narrowing rules remain in force.

| Spelling | Canonical annotation |
| --- | --- |
| `MMLTK_MAX_BYTES(n)`, `MMLTK_MIN_BYTES(n)` | `MaxBytes{n}`, `MinBytes{n}` |
| `MMLTK_MAX_ITEMS(n)` | `MaxItems{n}` |
| `MMLTK_MINIMUM(T, value)` | `Minimum<T>{value}` |
| `MMLTK_MINIMUM_VALUE(value)` | `Minimum{value}` with deduction |
| `MMLTK_MAXIMUM(T, value)` | `Maximum<T>{value}` |
| `MMLTK_FINITE` | `Finite{}` |
| `MMLTK_RUNTIME_DESTINATION` | `RuntimeDestination{}` |
| `MMLTK_MAX_PATH_BYTES`, `MMLTK_MAX_NAME_BYTES` | `MaxBytes` using the canonical path/name limits |
| `MMLTK_PRESENTATION(args...)` | `Presentation<args...>{}` |
| `MMLTK_CATALOG(args...)` | `CatalogProvider<args...>{}` |

The path/name limits remain owned by `field_policy.h` (4096 and 256 bytes).
Presentation and catalog macros accept template-argument tails; value tails
can contain commas. For a comma-containing `Minimum`/`Maximum` type, declare a
scoped type alias and pass that alias as the first argument. Do not introduce
another macro family or mirror the canonical limits locally.

Macros supply annotation syntax only: records, fields, constructors, defaults,
equality, and registrations stay explicit. Resource/execution state remains
ordinary; dynamic type tables or discovery require a product need.

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
