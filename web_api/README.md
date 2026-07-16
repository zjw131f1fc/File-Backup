# Web API

Build from the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target backup_web_server
```

Run the local server:

```bash
./build/web_api/backup_web_server --port 8080
```

The service binds to `127.0.0.1` by default. A separate frontend dev server can use
`--origin http://127.0.0.1:4173` when it runs on another port.
By default, the local path browser is unrestricted. Add one or more `--root PATH`
arguments to restrict it to explicit filesystem roots.

## Code map

- `src/web_api.cpp`: request routing, backup/restore validation, task cancellation, and SSE registration.
- `src/web_api_internal.cpp`: JSON conversion, path checks, query parsing, and status serialization. This is supporting code, not the main request flow.
- `src/web_api_server.cpp`: runtime startup, HTTP binding, and shutdown.
- `include/web_api/web_api.h`: public API and server interfaces.
