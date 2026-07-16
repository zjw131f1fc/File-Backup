#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build"}
API_PORT=${API_PORT:-8080}
FRONTEND_PORT=${FRONTEND_PORT:-4173}
BUILD_BEFORE_START=1
START_FRONTEND=1
EXTRA_ROOTS=()
BACKEND_PID=''
FRONTEND_PID=''

usage() {
    cat <<'EOF'
Usage: scripts/start.sh [options]

Options:
  --api-port PORT       Web API port (default: 8080)
  --frontend-port PORT  Frontend port (default: 4173)
  --root PATH           Additional filesystem root; may be repeated
  --skip-build          Start existing binaries without rebuilding
  --no-frontend         Start only the Web API
  --help                Show this help

Environment:
  BUILD_DIR             CMake build directory (default: ./build)
EOF
}

port_in_use() {
    ss -ltn "sport = :$1" | awk 'NR > 1 { found = 1 } END { exit !found }'
}

cleanup() {
    local exit_code=$?
    trap - EXIT INT TERM
    if [[ -n "$FRONTEND_PID" ]] && kill -0 "$FRONTEND_PID" 2>/dev/null; then
        kill "$FRONTEND_PID" 2>/dev/null || true
    fi
    if [[ -n "$BACKEND_PID" ]] && kill -0 "$BACKEND_PID" 2>/dev/null; then
        kill "$BACKEND_PID" 2>/dev/null || true
    fi
    wait 2>/dev/null || true
    exit "$exit_code"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --api-port)
            [[ $# -ge 2 ]] || { printf '%s\n' '--api-port requires a value' >&2; exit 2; }
            API_PORT=$2
            shift 2
            ;;
        --frontend-port)
            [[ $# -ge 2 ]] || { printf '%s\n' '--frontend-port requires a value' >&2; exit 2; }
            FRONTEND_PORT=$2
            shift 2
            ;;
        --root)
            [[ $# -ge 2 ]] || { printf '%s\n' '--root requires a path' >&2; exit 2; }
            EXTRA_ROOTS+=("$2")
            shift 2
            ;;
        --skip-build)
            BUILD_BEFORE_START=0
            shift
            ;;
        --no-frontend)
            START_FRONTEND=0
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            printf 'unknown argument: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ "$BUILD_BEFORE_START" -eq 1 ]]; then
    "$ROOT_DIR/scripts/build.sh"
fi

SERVER_BINARY="$BUILD_DIR/web_api/backup_web_server"
if [[ ! -x "$SERVER_BINARY" ]]; then
    printf 'Web API binary not found: %s\n' "$SERVER_BINARY" >&2
    printf 'Run scripts/build.sh first or omit --skip-build.\n' >&2
    exit 1
fi

if port_in_use "$API_PORT"; then
    printf 'API port %s is already in use\n' "$API_PORT" >&2
    exit 1
fi
if [[ "$START_FRONTEND" -eq 1 ]] && port_in_use "$FRONTEND_PORT"; then
    printf 'Frontend port %s is already in use\n' "$FRONTEND_PORT" >&2
    exit 1
fi

trap cleanup EXIT INT TERM

SERVER_ARGS=(
    --port "$API_PORT"
    --origin "http://127.0.0.1:$FRONTEND_PORT"
)
for root in "${EXTRA_ROOTS[@]}"; do
    SERVER_ARGS+=(--root "$root")
done

"$SERVER_BINARY" "${SERVER_ARGS[@]}" &
BACKEND_PID=$!

if [[ "$START_FRONTEND" -eq 1 ]]; then
    python3 -m http.server "$FRONTEND_PORT" --bind 127.0.0.1 --directory "$ROOT_DIR/frontend" &
    FRONTEND_PID=$!
fi

printf 'Frontend: http://127.0.0.1:%s/?api=http://127.0.0.1:%s\n' "$FRONTEND_PORT" "$API_PORT"
printf 'Press Ctrl-C to stop services.\n'
wait
