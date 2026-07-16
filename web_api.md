# Web API 规格说明

## 1. 目标与边界

Web API 为本机备份系统提供浏览器访问入口。它只负责 HTTP 请求、参数校验、任务提交、任务查询和结果展示，不直接调用 Scanner、Filter、ArchiveWriter、ArchiveReader 或 Restorer。

调用链固定为：

```text
浏览器 → Web API → Scheduler / TaskManager → 子模块接口
```

Web API 与调度器在第一版运行于同一个进程。任务执行不能阻塞 HTTP 请求线程，创建任务后立即返回任务 ID，由后台执行器调用调度器运行任务。

Web 层需要一个 `TaskRuntime`（可以是 Web 服务内部组件，也可以放在 Scheduler 层）负责：

1. 创建任务记录并放入执行队列；
2. 在 worker 中把任务从 `PENDING` 改为 `RUNNING`；
3. 调用 `BackupScheduler::run()` 或 `RestoreScheduler::run()`；
4. 保存终态、发布进度事件并处理取消竞态。

第一版使用可配置的 worker pool。任务进入共享队列，最多同时运行 `worker_count` 个备份或还原任务，其余任务保持 `PENDING`。任务记录和事件缓冲暂存内存；服务重启后未完成任务视为丢失，旧任务查询返回 `404`。

并发运行约束：

- 每个任务创建独立的 Filter、Writer、Reader、Restorer 实例，不跨任务共享非线程安全实例；
- `TaskManager` 的状态、进度、取消和任务索引操作必须线程安全；
- 显式指定同一个 `archive_name` 的备份任务不能使用同一个输出目录，冲突时后提交任务返回 `409 OUTPUT_CONFLICT`；未指定名称时由运行时自动分配不重复的归档文件名；
- 归档写入期间不能有其他任务读取或写入同一个归档文件；
- 同一个源目录可以被多个只读扫描任务同时使用；
- worker 数量和队列长度应由服务配置限制，避免并发 I/O 使本机失去响应。

`GET /api/capabilities` 返回当前 `worker_count` 和是否支持并发任务。后续需要动态扩缩容或持久化时，再补充任务存储和资源调度设计。

现有 `TaskManager` 已提供单任务创建、查询、取消、进度更新和完成接口，但还没有任务列表、任务类型、时间戳或事件订阅能力。这些是 Web 运行时需要补充的上层能力，不应让 HTTP 层自行维护另一套状态。

## 2. 通用约定

- 基础路径：`/api`
- 请求和响应：`application/json; charset=utf-8`
- 路径使用 Linux 本地文件系统路径
- 任务 ID 使用调度器生成的字符串
- 所有任务都是异步任务，创建接口返回 HTTP `202 Accepted`
- Web API 不暴露底层子模块的内部接口和归档格式
- JSON 中的枚举值使用大写字符串，不使用 C++ 枚举整数值
- 生产环境使用同源前端；开发环境若前后端端口不同，只允许配置的前端 Origin 跨域

### 2.1 HTTP 状态码

| 状态码 | 使用场景 |
|---:|---|
| `200` | 查询成功、取消结果、健康检查 |
| `202` | 任务已创建并进入等待队列 |
| `400` | JSON 格式或字段类型错误 |
| `404` | 任务或路径不存在 |
| `409` | 任务当前状态不允许执行该操作 |
| `422` | 字段格式正确但业务校验失败 |
| `429` | worker 和等待队列均已达到上限 |
| `503` | 任务运行时正在关闭或暂不可用 |
| `500` | Web 服务内部错误 |

### 2.2 任务状态转换

```text
PENDING ──→ RUNNING ──→ SUCCESS
    │           │   ├──→ PARTIAL_SUCCESS
    │           │   ├──→ FAILED
    │           │   └──→ CANCELLED
    └──────────→ CANCELLED
```

终态不可再次执行或取消。取消与任务完成同时发生时，以先被运行时确认的终态为准；运行时必须避免 `complete_task()` 把已经确认的 `CANCELLED` 覆盖成 `SUCCESS`。

## 3. 核心接口

### 3.1 健康检查

```http
GET /api/health
```

响应：

```json
{
  "status": "ok",
  "service": "backup-web"
}
```

### 3.2 获取前端能力

```http
GET /api/capabilities
```

前端使用此接口生成筛选器和冲突策略选项，避免复制 C++ 枚举定义。

响应：

```json
{
  "entry_types": [
    "REGULAR_FILE",
    "DIRECTORY",
    "SYMBOLIC_LINK",
    "HARD_LINK",
    "FIFO",
    "CHARACTER_DEVICE",
    "BLOCK_DEVICE"
  ],
  "conflict_policies": ["SKIP", "OVERWRITE", "RENAME"],
  "filter_rules": [
    "include_paths",
    "exclude_paths",
    "include_types",
    "include_names",
    "exclude_names",
    "newer_than_sec",
    "older_than_sec",
    "min_size",
    "max_size",
    "include_uids"
  ],
  "progress_events": true,
  "concurrency": {
    "enabled": true,
    "worker_count": 2,
    "max_queued_tasks": 32
  }
}
```

### 3.3 创建备份任务

```http
POST /api/backup
```

请求体：

```json
{
  "source_path": "/home/user/data",
  "output_path": "/backup",
  "archive_name": "data.bak",
  "filter_rules": {
    "include_paths": [],
    "exclude_paths": [],
    "include_types": [],
    "include_names": [],
    "exclude_names": [],
    "newer_than_sec": 0,
    "older_than_sec": 0,
    "min_size": 0,
    "max_size": 0,
    "include_uids": []
  }
}
```

`output_path` 是归档输出目录，不是归档文件路径。`archive_name` 是可选的单个文件名；省略时默认使用 `backup.dat`，如果同名文件或任务已占用，则自动尝试 `backup-1.dat`、`backup-2.dat` 等名称。字段对应 `BackupRequest` 和 `FilterRules`。Web 层负责校验路径非空、筛选数组格式正确、尺寸范围合法，并将请求转换为公共类型后提交给 `TaskRuntime`。

备份请求还必须满足：

- `source_path` 必须是可读目录；
- `output_path` 必须是已存在且可写的目录；
- `output_path` 不能等于或位于 `source_path` 内部，避免扫描时把归档自身再次纳入备份；
- 显式指定的 `archive_name` 已存在时返回 `409 OUTPUT_EXISTS`，不覆盖；未指定名称时自动选择下一个可用名称；
- `newer_than_sec` 和 `older_than_sec` 是 Unix 时间戳秒，`0` 表示不限制；两者同时设置时必须满足 `newer_than_sec < older_than_sec`；
- `min_size` 和 `max_size` 单位为字节，`0` 表示对应方向不限制；两者都非零时必须满足 `min_size <= max_size`；
- `include_types` 只能使用 `capabilities` 返回的枚举字符串；
- `include_uids` 只能包含非负整数 UID。

成功响应：

```http
HTTP/1.1 202 Accepted
```

```json
{
  "task_id": "task_1_123456",
  "type": "backup",
  "status": "PENDING"
}
```

### 3.4 创建还原任务

```http
POST /api/restore
```

请求体：

```json
{
  "archive_path": "/backup/data.bak",
  "target_path": "/home/user/restore",
  "conflict_policy": "SKIP"
}
```

`conflict_policy` 允许值：

- `SKIP`
- `OVERWRITE`
- `RENAME`

字段对应 `RestoreRequest`。成功响应格式与备份任务相同，`type` 为 `restore`。

还原请求还必须满足：

- `archive_path` 必须是可读的普通文件；
- `target_path` 可以不存在，但其父目录必须存在且可写；
- `archive_path` 不能位于 `target_path` 内部，避免还原过程修改正在读取的归档；
- `conflict_policy` 必须是 `SKIP`、`OVERWRITE` 或 `RENAME`。

### 3.5 查询任务

```http
GET /api/tasks/{task_id}
```

响应：

```json
{
  "task_id": "task_1_123456",
  "type": "backup",
  "created_at": "2026-07-16T10:00:00Z",
  "started_at": "2026-07-16T10:00:01Z",
  "status": "RUNNING",
  "progress": {
    "stage": "scanning",
    "processed_entries": 42,
    "processed_bytes": 4096,
    "current_path": "/home/user/data/a.txt"
  },
  "result": null
}
```

`type`、时间戳和请求摘要属于 Web 任务运行时元数据。现有公共 `Task` 结构没有这些字段，不能由 Web 层临时推断；应由 `TaskRuntime` 或任务存储统一保存。

任务状态必须与 `TaskStatus` 保持一致：

```text
PENDING
RUNNING
SUCCESS
PARTIAL_SUCCESS
FAILED
CANCELLED
```

任务完成后，`result` 返回：

```json
{
  "status": "SUCCESS",
  "message": "backup completed successfully",
  "error_count": 0,
  "warning_count": 0
}
```

任务不存在时返回 `404`，不直接暴露底层异常堆栈。

### 3.6 查询任务列表

```http
GET /api/tasks
GET /api/tasks?status=RUNNING
GET /api/tasks?type=backup&limit=20
```

第一版可返回当前进程内的任务列表，但必须由任务运行时维护任务索引，不能从现有 `TaskManager::get_task()` 逐个猜测。支持的筛选字段为 `status`、`type` 和 `limit`，`limit` 范围为 `1..100`，默认 `20`。列表中的每项使用任务摘要，不包含重复的完整错误详情。

响应：

```json
{
  "tasks": [
    {
      "task_id": "task_1_123456",
      "type": "backup",
      "status": "SUCCESS",
      "message": "backup completed successfully",
      "progress": {
        "stage": "completed",
        "processed_entries": 42,
        "processed_bytes": 4096,
        "current_path": "/home/user/data"
      }
    }
  ]
}
```

### 3.7 取消任务

```http
POST /api/tasks/{task_id}/cancel
```

成功响应：

```json
{
  "task_id": "task_1_123456",
  "status": "CANCELLED"
}
```

只有 `PENDING` 或 `RUNNING` 状态可以取消。任务已经完成、失败或取消时返回 `409`，并返回当前状态。

取消请求只改变任务状态。备份调度器通过进度回调传播取消信号，恢复调度器在每个条目处理前检查取消状态。

### 3.8 任务进度事件

```http
GET /api/tasks/{task_id}/events
Accept: text/event-stream
```

推荐使用 Server-Sent Events。连接建立时先发送当前快照，之后发送增量事件。每个事件必须包含递增的 `id`，支持浏览器使用 `Last-Event-ID` 断线续传。事件至少保留到任务进入终态。

事件类型：

```text
progress
status
result
error
```

示例：

```text
event: progress
id: 42
data: {"stage":"scanning","processed_entries":42,"processed_bytes":4096,"current_path":"/home/user/data/a.txt"}

event: status
id: 43
data: {"status":"SUCCESS"}
```

任务完成或失败后发送终止事件并关闭连接。前端也必须支持定时调用 `GET /api/tasks/{task_id}` 作为 SSE 不可用时的降级方案。

SSE 需要事件缓冲或发布订阅组件。仅保存 `Task.progress` 无法实现可靠的事件推送和断线续传。

## 4. 本机路径浏览接口

前端需要让用户选择源目录、归档路径和还原目录。Web API 不提供文件内容读取接口，只提供目录和文件元信息。可浏览根目录由服务端配置决定，不能让请求者通过传入 `/` 绕过根目录限制。

### 4.1 获取可浏览根目录

```http
GET /api/filesystem/roots
```

响应：

```json
{
  "roots": [
    {"path":"/home/user","name":"user","type":"directory"},
    {"path":"/tmp","name":"tmp","type":"directory"}
  ]
}
```

### 4.2 浏览目录

```http
GET /api/filesystem/entries?path=/home/user
```

响应：

```json
{
  "path": "/home/user",
  "entries": [
    {"name":"data","path":"/home/user/data","type":"directory"},
    {"name":"archive.bak","path":"/home/user/archive.bak","type":"regular_file","size":1024}
  ]
}
```

该接口只用于 UI 选择路径，不替代 Scanner，也不跟随符号链接进入目录。`path` 必须位于服务端允许的根目录下，并在规范化后再次检查，防止 `..` 越界。目录不存在或无权限时返回 `404` 或 `422`。

## 5. 错误响应

所有错误使用统一格式：

```json
{
  "error": {
    "code": "INVALID_PATH",
    "message": "source_path must be an existing directory",
    "details": {}
  }
}
```

建议错误码：

| 错误码 | 含义 |
|---|---|
| `INVALID_JSON` | 请求体不是合法 JSON |
| `INVALID_REQUEST` | 必填字段缺失或类型错误 |
| `INVALID_PATH` | 路径不存在、不是目录或不可访问 |
| `OUTPUT_EXISTS` | 备份目标归档已存在 |
| `OUTPUT_CONFLICT` | 并发任务正在使用相同输出归档 |
| `QUEUE_FULL` | worker 和等待队列均已达到上限 |
| `PATH_NOT_ALLOWED` | 启用目录限制时，路径不在服务端允许的根目录内 |
| `INVALID_FILTER` | 筛选条件之间存在矛盾 |
| `TASK_NOT_FOUND` | 任务 ID 不存在 |
| `TASK_CONFLICT` | 当前任务状态不允许操作 |
| `TASK_FAILED` | 任务执行失败 |
| `INTERNAL_ERROR` | 服务内部错误 |

## 6. 安全与校验

- 默认只监听 `127.0.0.1`，不直接暴露到局域网。
- 所有路径由后端规范化和校验，不能仅依赖前端校验。
- 本地开发默认不限制文件浏览范围；部署时可通过一个或多个 `--root PATH` 启用允许目录限制，不能通过 `..` 越界。
- `source_path`、`output_path`、`archive_path` 和 `target_path` 的关系校验必须使用规范化后的路径，不能只比较字符串前缀。
- 备份默认不覆盖已有归档，避免重复点击造成数据丢失。
- 不提供任意文件内容下载、任意命令执行或底层归档流接口。
- 路径权限错误、归档校验错误和子模块错误转换为稳定的 API 错误响应。
- 不把 C++ 异常信息、堆栈或内部文件路径以外的敏感信息直接返回给浏览器。

## 7. 与 Scheduler 的映射

| Web API 操作 | Scheduler / TaskManager 操作 |
|---|---|
| 创建备份 | `create_backup_task()`，运行时入队并调用 `BackupScheduler::run()` |
| 创建还原 | `create_restore_task()`，运行时入队并调用 `RestoreScheduler::run()` |
| 查询任务 | `get_task()` |
| 取消任务 | `cancel_task()` |
| 任务启动 | 运行时把任务置为 `RUNNING`，现有 `TaskManager` 需要补充等价能力 |
| 任务列表 | 运行时任务索引或任务存储，现有 `TaskManager` 尚未提供 |
| 进度推送 | 进度更新写入事件缓冲，再由 SSE 转发 |
| 结果展示 | 读取 `Task.result` |

Web 层不应重新实现任务状态转换，也不应直接调用 5 个底层子模块。

## 8. 实现顺序

第一阶段先实现以下接口：

1. `GET /api/health`
2. `GET /api/capabilities`
3. `POST /api/backup`
4. `POST /api/restore`
5. `GET /api/tasks/{task_id}`
6. `POST /api/tasks/{task_id}/cancel`
7. `GET /api/filesystem/entries`

第二阶段增加：

1. `GET /api/tasks`
2. `GET /api/tasks/{task_id}/events`
3. `GET /api/filesystem/roots`
4. 前端错误、进度和任务历史页面

每新增一个接口，先补请求校验和响应契约测试，再实现路由和调度器调用。

## 9. 暂不纳入第一版的接口

- 归档下载和归档内容浏览：本项目是本机备份工具，前端只需要提交路径和查看任务结果；
- 直接调用某个底层子模块的接口：会破坏 Scheduler 的职责边界；
- 删除历史任务：当前 `TaskManager` 是进程内存储，先明确持久化策略再增加；
- 远程主机、账号认证和多用户权限：当前范围是本机单用户应用。
