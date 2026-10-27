#!/bin/sh
# Smoke test: run the freshly built shell on a tiny task and check the result.
set -e
bin="$(pwd)/taskshell_build"
dir=$(mktemp -d)
cd "$dir"
out=$(printf 'task create t\ntask add t "echo hi"\ntask run t\nexit\n' | "$bin")
echo "$out" | grep -q "Done: 1 ok, 0 failed, 0 skipped"
echo "smoke test passed"
