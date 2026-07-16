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
