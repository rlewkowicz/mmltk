#!/usr/bin/env bash

RUSTFLAGS="${RUSTFLAGS} --cfg loom -C debug-assertions=on" \
    LOOM_MAX_PREEMPTIONS="${LOOM_MAX_PREEMPTIONS:-2}" \
    LOOM_CHECKPOINT_INTERVAL="${LOOM_CHECKPOINT_INTERVAL:-1}" \
    LOOM_LOG=1 \
    LOOM_LOCATION=1 \
    cargo test --release --lib "$@"
