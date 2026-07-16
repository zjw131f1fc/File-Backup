#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build"}
BUILD_TYPE=${BUILD_TYPE:-Debug}
BUILD_JOBS=${BUILD_JOBS:-$(nproc 2>/dev/null || printf '2')}
SKIP_TESTS=0

usage() {
    cat <<'EOF'
Usage: scripts/build.sh [--skip-tests]

Environment:
  BUILD_DIR    CMake build directory (default: ./build)
  BUILD_TYPE   CMake build type (default: Debug)
  BUILD_JOBS   Parallel build jobs (default: nproc)
EOF
}

for argument in "$@"; do
    case "$argument" in
        --skip-tests) SKIP_TESTS=1 ;;
        --help|-h) usage; exit 0 ;;
        *)
            printf 'unknown argument: %s\n' "$argument" >&2
            usage >&2
            exit 2
            ;;
    esac
done

printf 'Configuring %s (%s)\n' "$BUILD_DIR" "$BUILD_TYPE"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" --parallel "$BUILD_JOBS"

if [[ "$SKIP_TESTS" -eq 0 ]]; then
    ctest --test-dir "$BUILD_DIR" --output-on-failure
fi

printf 'Build complete: %s\n' "$BUILD_DIR"
