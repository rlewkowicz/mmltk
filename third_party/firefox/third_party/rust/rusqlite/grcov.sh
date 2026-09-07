rg --files -g '*.profraw' | xargs rm
rm -rf target/debug/coverage/
export RUSTFLAGS="-Cinstrument-coverage"
cargo build
export LLVM_PROFILE_FILE="rusqlite-%p-%m.profraw"
cargo test
grcov . -s . --binary-path ./target/debug/ -t html --branch --ignore-not-existing -o ./target/debug/coverage/
rg --files -g '*.profraw' | xargs rm
open target/debug/coverage/index.html
