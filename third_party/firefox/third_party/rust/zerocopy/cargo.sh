# Copyright 2024 The Fuchsia Authors
# Licensed under a BSD-style license <LICENSE-BSD>, Apache License, Version 2.0
# <LICENSE-APACHE or https://www.apache.org/licenses/LICENSE-2.0>, or the MIT
# license <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your option.

set -eo pipefail

env -u RUSTFLAGS cargo +stable build --manifest-path tools/Cargo.toml -p cargo-zerocopy -q
./tools/target/debug/cargo-zerocopy $@
