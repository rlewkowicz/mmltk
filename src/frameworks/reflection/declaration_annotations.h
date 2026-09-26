#pragma once
#include "src/frameworks/reflection/field_policy.h"
// Syntax only. Types, members, defaults and access remain ordinary declarations;
// field_policy.h owns policy meaning and declaration-local reflection owns its
// materialization. Each expansion preserves the exact annotation type and brace
// initialization, including narrowing diagnostics. Values occur exactly once.
// Variadic value/type tails preserve template commas and bound expressions. For
// a comma-containing Minimum/Maximum type, declare a scoped type alias first.
#define MMLTK_MAX_BYTES(...) [[= ::mmltk::frameworks::reflection::MaxBytes{__VA_ARGS__}]]
#define MMLTK_MIN_BYTES(...) [[= ::mmltk::frameworks::reflection::MinBytes{__VA_ARGS__}]]
#define MMLTK_MAX_ITEMS(...) [[= ::mmltk::frameworks::reflection::MaxItems{__VA_ARGS__}]]
#define MMLTK_MINIMUM(Type, ...) [[= ::mmltk::frameworks::reflection::Minimum<Type>{__VA_ARGS__}]]
#define MMLTK_MINIMUM_VALUE(...) [[= ::mmltk::frameworks::reflection::Minimum{__VA_ARGS__}]]
#define MMLTK_MAXIMUM(Type, ...) [[= ::mmltk::frameworks::reflection::Maximum<Type>{__VA_ARGS__}]]
#define MMLTK_FINITE [[= ::mmltk::frameworks::reflection::Finite{}]]
#define MMLTK_RUNTIME_DESTINATION [[= ::mmltk::frameworks::reflection::RuntimeDestination{}]]
#define MMLTK_MAX_PATH_BYTES [[= ::mmltk::frameworks::reflection::MaxBytes{::mmltk::frameworks::reflection::kMaximumPathBytes}]]
#define MMLTK_MAX_NAME_BYTES [[= ::mmltk::frameworks::reflection::MaxBytes{::mmltk::frameworks::reflection::kMaximumNameBytes}]]
#define MMLTK_PRESENTATION(...) [[= ::mmltk::frameworks::reflection::Presentation<__VA_ARGS__>{}]]
#define MMLTK_CATALOG(...) [[= ::mmltk::frameworks::reflection::CatalogProvider<__VA_ARGS__>{}]]
