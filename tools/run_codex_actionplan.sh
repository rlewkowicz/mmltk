#!/usr/bin/env bash

set -o pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
readonly RUNS=$1

readonly PROMPT="$(cat <<'EOF'
If the current actionplan.md is not complete, exit. If the current actionplan.md is complete, create a new one. We need to focus on folder organization. We have ./inlcude which has some includes, but then src has scattered includes as well. The cmake files are massive and monolithic. The application systems must keep ordinary domain classes and infrastructure boundaries modular instead of accumulating capabilities in one folder. Broadly, the number one goal is compilation time, and parallelism of compilation. The secondary goal is code organization and structure. src/rfdetr for example has cuda functionality and drawing that could be used for other non existent models. We don't want a spaghetti mess of code reaching across the code base. Broadly where can we have better interface demarcation, better organization, better modularity. Where are there sprawling duplicated classes, functions, templates across the code base that could be consolidated into into central frameworks to reduce boiler plate for future model modules. Where can we make model layers agnostic and reusable? ../ultralytics has examples of folder organization for multiple models and layers. It's python, but it can be used for organizational reference. ../obs-studio is a world class, compiled language implementation same with ../godot, and ../krita. With strong modularity and organization. Use these references and try to come up with features and phases that could benefit this repository. If you created a new actionplan.md, execute it. If the code base is well organized, free of massive "god files" (some might be ok), exit.
EOF
)"

render_agent_messages() {
    node --input-type=module -e '
        import readline from "node:readline";
        const input = readline.createInterface({ input: process.stdin });
        for await (const line of input) {
            try {
                const event = JSON.parse(line);
                if (event.type === "item.completed" && event.item?.type === "agent_message") {
                    process.stdout.write(`${event.item.text}\n`);
                }
            } catch {
                // The CLI may write non-JSON diagnostics; keep runner output transcript-free.
            }
        }
    '
}

run_codex() {
    codex --sandbox danger-full-access --ask-for-approval never exec --json --color never \
        --skip-git-repo-check --cd "${REPO_ROOT}" "${PROMPT}" 2>&1 | render_agent_messages
}

for ((run = 1; run <= RUNS; ++run)); do
    printf 'codex actionplan run %d/%d\n' "${run}" "${RUNS}" >&2
    run_codex "${run}"
    status=$?
    if (( status != 0 )); then
        printf 'codex actionplan run %d/%d failed (exit %d)\n' "${run}" "${RUNS}" "${status}" >&2
        exit "${status}"
    fi
    printf 'codex actionplan run %d of %d done\n' "${run}" "${RUNS}" >&2
done
