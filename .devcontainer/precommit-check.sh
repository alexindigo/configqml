#!/usr/bin/env bash
set -euo pipefail
# This container only formats/lints. Builds and tests belong in the project VM.
git ls-files -z --cached -- '*.cpp' '*.h' | xargs -0 -r clang-format --dry-run --Werror
shellcheck .githooks/pre-commit .devcontainer/precommit-check.sh
shfmt -i 4 -d .githooks/pre-commit .devcontainer/precommit-check.sh
git diff --cached --check
