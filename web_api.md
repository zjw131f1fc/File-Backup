# Web API 规格说明

## 1. 目标与边界

Web API 为本机备份系统提供浏览器访问入口。它只负责 HTTP 请求、参数校验、任务提交、任务查询和结果展示，不直接调用 Scanner、Filter、ArchiveWriter、ArchiveReader 或 Restorer。

调用链固定为：

```text
浏览器 → Web API → Scheduler / TaskManager → 子模块接口
```

Web API 与调度器在第一版运行于同一个进程。任务执行不能阻塞 HTTP 请求线程，创建任务后立即返回任务 ID，由后台执行器调用调度器运行任务。

## 2. 通用约定

- 基础路径：`/api`
- 请求和响应：`application/json; charset=utf-8`
- 路径使用 Linux 本地文件系统路径
- 任务 ID 使用调度器生成的字符串
- 所有任务都是异步任务，创建接口返回 HTTP `202 Accepted`
- Web API 不暴露底层子模块的内部接口和归档格式

### 2.1 HTTP 状态码

| 状态码 | 使用场景 |
|---:|---|
| `200` | 查询成功、取消结果、健康检查 |
| `202` | 任务已创建并进入等待队列 |
| `400` | JSON 格式或字段类型错误 |
| `404` | 任务或路径不存在 |
| `409` | 任务当前状态不允许执行该操作 |
| `422` | 字段格式正确但业务校验失败 |
| `500` | Web 服务内部错误 |

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

### 3.2 创建备份任务

```http
POST /api/backup
```

请求体：

```json
{
  "source_path": "/home/user/data",
  "output_path": "/backup/data.bak",
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

字段对应 `BackupRequest` 和 `FilterRules`。Web 层负责校验路径非空、筛选数组格式正确、尺寸范围合法，并将请求转换为公共类型后提交给 `TaskManager`。

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

### 3.3 创建还原任务

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

### 3.4 查询任务

```http
GET /api/tasks/{task_id}
```

响应：

```json
{
  "task_id": "task_1_123456",
  "type": "backup",
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

### 3.5 查询任务列表

```http
GET /api/tasks
GET /api/tasks?status=RUNNING
GET /api/tasks?type=backup&limit=20
```

第一版可返回当前进程内的任务列表。支持的筛选字段为 `status`、`type` 和 `limit`。列表中的每项使用任务摘要，不包含重复的完整错误详情。

响应：

```json
{
  "tasks": [
    {
      "task_id": "task_1_123456",
      "type": "backup",
      "status": "SUCCESS",
      "message": "backup completed successfully"
    }
  ]
}
```

### 3.6 取消任务

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

### 3.7 任务进度事件

```http
GET /api/tasks/{task_id}/events
Accept: text/event-stream
```

推荐使用 Server-Sent Events。事件类型：

```text
progress
status
result
error
```

示例：

```text
event: progress
data: {"stage":"scanning","processed_entries":42,"processed_bytes":4096,"current_path":"/home/user/data/a.txt"}

event: status
data: {"status":"SUCCESS"}
```

任务完成或失败后发送终止事件并关闭连接。前端也必须支持定时调用 `GET /api/tasks/{task_id}` 作为 SSE 不可用时的降级方案。

## 4. 本机路径浏览接口

前端需要让用户选择源目录、归档路径和还原目录。Web API 不提供文件内容读取接口，只提供目录和文件元信息。

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

该接口只用于 UI 选择路径，不替代 Scanner，也不跟随符号链接进入目录。目录不存在或无权限时返回 `404` 或 `422`。

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
| `TASK_NOT_FOUND` | 任务 ID 不存在 |
| `TASK_CONFLICT` | 当前任务状态不允许操作 |
| `TASK_FAILED` | 任务执行失败 |
| `INTERNAL_ERROR` | 服务内部错误 |

## 6. 安全与校验

- 默认只监听 `127.0.0.1`，不直接暴露到局域网。
- 所有路径由后端规范化和校验，不能仅依赖前端校验。
- 文件浏览接口必须限制在允许的本机路径范围内，不能通过 `..` 越界。
- 不提供任意文件内容下载、任意命令执行或底层归档流接口。
- 路径权限错误、归档校验错误和子模块错误转换为稳定的 API 错误响应。
- 不把 C++ 异常信息、堆栈或内部文件路径以外的敏感信息直接返回给浏览器。

## 7. 与 Scheduler 的映射

| Web API 操作 | Scheduler / TaskManager 操作 |
|---|---|
| 创建备份 | `create_backup_task()`，后台调用 `BackupScheduler::run()` |
| 创建还原 | `create_restore_task()`，后台调用 `RestoreScheduler::run()` |
| 查询任务 | `get_task()` |
| 取消任务 | `cancel_task()` |
| 进度推送 | 读取 `Task.progress` 或由任务事件队列转发 |
| 结果展示 | 读取 `Task.result` |

Web 层不应重新实现任务状态转换，也不应直接调用 5 个底层子模块。

## 8. 实现顺序

第一阶段先实现以下接口：

1. `GET /api/health`
2. `POST /api/backup`
3. `POST /api/restore`
4. `GET /api/tasks/{task_id}`
5. `POST /api/tasks/{task_id}/cancel`
6. `GET /api/filesystem/entries`

第二阶段增加：

1. `GET /api/tasks`
2. `GET /api/tasks/{task_id}/events`
3. `GET /api/filesystem/roots`
4. 前端错误、进度和任务历史页面

每新增一个接口，先补请求校验和响应契约测试，再实现路由和调度器调用。
