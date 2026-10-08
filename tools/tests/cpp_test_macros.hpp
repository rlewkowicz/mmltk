#pragma once

#define CHECK(...) ::mmltk::tools::testing::require((__VA_ARGS__), #__VA_ARGS__, __LINE__)
