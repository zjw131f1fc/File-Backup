# Linux 本地备份系统架构说明

* **版本**：v0.3
* **架构**：Web + 调度器 + 功能子模块
* **用途**：指导 Codex 编码、多人协作和任务拆分
* **目标**：覆盖基础备份还原、特殊文件、元数据、六类筛选、自定义归档和 Web GUI

---

## 功能目标与分值

| 功能 | 分值 | 本项目中的明确含义 |
|---|---:|---|
| 基础备份与还原 | 40 | 递归扫描目录树，将选中的对象写入归档，并从归档恢复目录和文件 |
| 特殊文件支持 | 10 | 支持符号链接、硬链接、FIFO、字符设备和块设备；Unix Socket 不备份 |
| 元数据支持 | 10 | 保存并尽可能恢复属主、属组、权限、访问时间和修改时间 |
| 六类自定义筛选 | 18 | 路径、文件类型、名称、时间、尺寸、用户，每类 3 分 |
| 自定义打包与解包 | 10 | 自行定义并实现单文件归档格式，不依赖现成归档程序直接完成核心逻辑 |
| Web GUI | 10 | 浏览器前端；由本机后端直接扫描和访问本机文件系统 |
| **合计** | **98** | 作为项目难度分目标 |

---

## 1. 总体结构

```text
浏览器
   │
   ▼
Web 与任务管理
   │
   ▼
备份 / 还原调度器
   │
   ├── 扫描子模块（内部注入筛选子模块和归档写入子模块）
   ├── 归档写入子模块
   ├── 归档读取子模块
   └── 统一恢复子模块（内部包含文件恢复、特殊文件创建和元数据恢复）
```

系统是一个本机应用，不采用微服务。

第一版可以运行在同一个进程中。后续有需要时，可以将 Web 和任务执行拆成两个本机进程。

---

## 2. 调度器职责

调度器只负责组织任务，不负责底层文件处理，也不碰文件流。

主要职责：

* 接收 Web 提交的备份或还原请求；
* 按顺序调用子模块接口；
* 更新任务状态和进度；
* 处理取消请求；
* 判断任务成功、失败或部分成功；
* 失败时调用清理接口；
* 将最终结果返回给 Web。

调度器不负责：

* 遍历目录；
* 识别 inode；
* 解析符号链接；
* 读写自定义归档字节；
* 创建设备文件；
* 恢复权限、属主和时间；
* 打开源文件或传递文件流。

### 2.1 备份流程

```text
接收请求
→ 创建筛选器（从 FilterRules）
→ 创建归档写入器
→ 调用扫描器（扫描器内部：扫描 → 筛选 → 开源文件流 → 写入归档）
→ 提交归档 或 终止归档
→ 返回结果
```

### 2.2 还原流程

```text
接收请求
→ 打开并校验归档
→ 逐条枚举归档条目
→ 调用恢复器（恢复器内部：根据类型恢复文件系统对象，需要内容时从 Reader 取流）
→ 调用元数据恢复
→ 返回结果
```

调度器主要关心接口调用是否成功，不需要了解子模块内部如何实现。

---

## 3. 子模块划分

子模块是 Todo 任务，不固定分配给某个人。组员可以根据工作量自行领取。

### 3.1 扫描子模块

负责：

* 递归扫描源目录；
* 不跟随符号链接；
* 识别普通文件、目录和特殊文件；
* 识别硬链接；
* 跳过 Unix Socket；
* 对每个条目调用筛选子模块判断是否包含；
* 对包含的条目，普通文件开源文件流并写入归档；
* 对包含的非文件条目，写入元数据到归档；
* 每处理一个条目调用 progress_callback，若回调返回 false 则停止扫描并返回 CANCELLED。

参考接口：

```text
progress_callback(Progress) → bool  ← 返回 true 继续，false 取消

scan_and_backup(
    source_path,
    filter,          ← IFilter 引用，由调度器创建并注入
    archive_writer,  ← IArchiveWriter 引用，由调度器创建并注入
    progress_callback
) → Result
```

---

### 3.2 筛选子模块

负责：

* 路径筛选（包含/排除路径模式）；
* 类型筛选（只包含指定的文件类型）；
* 名称筛选（glob 模式匹配）；
* 时间筛选（ newer_than / older_than）；
* 尺寸筛选（min_size / max_size）；
* 用户筛选（UID 列表）。

筛选规则（FilterRules）在工厂函数时烘焙进实例，调用时只需传入条目。

参考接口：

```text
create_filter(filter_rules) → unique_ptr<IFilter>

should_include(entry) → bool
```

筛选子模块是独立可测试的，18 分功能直接对应此模块。

---

### 3.3 归档写入子模块

负责：

* 创建临时归档；
* 实现自定义归档格式；
* 写入文件和目录信息；
* 写入符号链接、硬链接和特殊文件信息；
* 流式写入普通文件内容；
* 成功时提交归档；
* 失败或取消时删除临时归档。

参考接口：

```text
create_archive(output_path) → unique_ptr<IArchiveWriter>

archive_writer.add_entry(entry_info, content_stream) → Result
archive_writer.add_entry(entry_info) → Result
archive_writer.commit() → Result
archive_writer.abort() → Result
```

---

### 3.4 归档读取子模块

负责：

* 打开归档；
* 检查格式标识和版本；
* 检查归档是否截断；
* 检查非法长度和危险路径；
* 枚举归档条目；
* 流式读取普通文件内容。

参考接口：

```text
open_archive(archive_path) → unique_ptr<IArchiveReader>

archive_reader.validate() → Result
archive_reader.has_next_entry() → bool
archive_reader.next_entry(entry_info) → Result
archive_reader.open_content(entry_info) → unique_ptr<istream>
```

---

### 3.5 统一恢复子模块

负责：

* 检查目标路径是否安全；
* 处理目标已存在的情况（skip / overwrite / rename）；
* 创建目录；
* 恢复普通文件（使用临时文件写入，完成后替换正式目标）；
* 创建符号链接；
* 创建硬链接；
* 创建 FIFO（mkfifo）；
* 创建字符设备（mknod）；
* 创建块设备（mknod）；
* 恢复权限（chmod）；
* 恢复 UID 和 GID（chown）；
* 恢复访问时间和修改时间（utimensat）；
* 处理权限不足等错误。

恢复器接收归档读取器引用，普通文件时内部调用 reader.open_content() 取流，调度器不碰文件流。

参考接口：

```text
create_restorer() → unique_ptr<IRestorer>

restorer.restore_entry(
    target_root,
    entry_info,
    archive_reader,  ← IArchiveReader 引用，恢复器内部取内容流
    conflict_policy
) → Result

restorer.restore_metadata(
    target_path,
    entry_info
) → Result
```

恢复器根据 entry_info.type 内部决定行为：

* REGULAR_FILE → 从 reader 取流，写临时文件后替换
* DIRECTORY → mkdir
* SYMBOLIC_LINK → symlink
* HARD_LINK → link
* FIFO → mkfifo
* CHARACTER_DEVICE / BLOCK_DEVICE → mknod

---

## 4. 最少公共接口

不建立复杂的全局数据模型。

调度器只需要使用少量公共类型。

### 4.1 备份请求

```text
BackupRequest
- source_path
- output_path (resolved archive path)
- output_directory (optional destination directory)
- archive_name (optional file name)
- filter_rules
```

### 4.2 还原请求

```text
RestoreRequest
- archive_path
- target_path
- conflict_policy
```

### 4.3 执行结果

```text
Result
- status
- message
- error_count
- warning_count
```

`status` 可以是：

```text
SUCCESS
FAILED
PARTIAL_SUCCESS
CANCELLED
```

### 4.4 任务进度

```text
Progress
- stage
- processed_entries
- processed_bytes
- current_path
```

### 4.5 筛选规则

```text
FilterRules
- include_paths / exclude_paths
- include_types
- include_names / exclude_names
- newer_than_sec / older_than_sec
- min_size / max_size
- include_uids
```

### 4.6 条目信息

```text
EntryInfo
- path
- type
- size
- link_target
- hard_link_target / hard_link_inode
- permissions
- uid / gid
- atime_sec / atime_nsec
- mtime_sec / mtime_nsec
- device_major / device_minor
```

归档条目、inode、设备号、链接目标和权限等数据，由相关子模块自行定义内部结构。
公共 EntryInfo 仅用于子模块之间传递条目描述。

---

## 5. Web 部分

Web 负责：

* 创建备份任务；
* 创建还原任务；
* 查询任务状态；
* 查询任务进度；
* 取消任务；
* 显示任务结果和错误；
* 提供简单的本机路径输入和筛选条件输入。

Web 不直接扫描文件系统，也不直接读写归档。

建议提供以下接口：

```text
POST /api/backup
POST /api/restore
GET  /api/tasks/{task_id}
POST /api/tasks/{task_id}/cancel
```

任务可以使用以下状态：

```text
PENDING
RUNNING
SUCCESS
PARTIAL_SUCCESS
FAILED
CANCELLED
```

---

## 6. Todo List 建议

Todo 不按人数固定分组，只标注功能和工作量。

### 大任务

* 自定义归档格式设计与写入；
* 自定义归档读取与校验；
* 统一恢复模块（普通文件、目录、链接、特殊文件、元数据）；
* Web 和任务管理；
* 备份与还原调度器。

### 中等任务

* 目录递归扫描和文件类型识别；
* 六类筛选；
* 硬链接识别；
* 扫描→筛选→写入管道；
* 路径安全和冲突处理；
* 权限、属主和时间恢复；
* FIFO、字符设备和块设备恢复。

### 小任务

* 用户名解析为 UID；
* Unix Socket 识别和跳过；
* 进度回调；
* 错误信息整理；
* 功能测试；
* 演示截图和测试结果整理。

每个 Todo 应至少包含：

* 实现代码；
* 简单接口说明；
* 基本测试；
* 联调阶段的问题修复。

不能只按完成的 Todo 数量判断工作量，应同时参考任务的"大、中、小"标记。

---

## 7. Codex 开发规则

为了方便使用 Codex 开发，所有子模块应遵守以下规则：

1. 每个子模块放在独立目录中。
2. 每个子模块提供清晰的入口接口。
3. 不允许直接修改其他子模块内部代码。
4. 公共接口变更需要先同步。
5. 每个 Todo 尽量可以单独运行测试。
6. 子模块内部可以自由设计数据结构。
7. 调度器只依赖接口，不依赖内部实现。
8. 错误必须通过 `Result` 返回，不能只返回 `true` 或 `false`。
9. 大文件必须流式处理，不能一次性读入内存。
10. 失败时不能留下可被误认为成功的归档文件。
11. 调度器不碰文件流，所有流式操作由子模块内部处理。

---

## 8. 建议目录结构

```text
project/
├── web/
│   ├── routes/
│   ├── pages/
│   └── task_api/
├── scheduler/
│   ├── backup_scheduler/
│   ├── restore_scheduler/
│   └── task_manager/
├── modules/
│   ├── scanner/
│   ├── filter/
│   ├── archive_writer/
│   ├── archive_reader/
│   └── restorer/
├── common/
│   ├── request/
│   ├── result/
│   └── progress/
├── tests/
└── README.md
```

实际目录可以根据使用的编程语言调整。

---

## 9. 架构原则

本项目采用：

> Web 负责用户操作，调度器负责组织任务，子模块负责完成具体功能。
> 调度器不碰文件流，所有流式操作由子模块内部处理。

调度器只关心：

* 接口是否成功；
* 下一步调用什么；
* 是否停止或清理；
* 最终任务状态是什么。

子模块负责全部底层实现，包括文件流的打开、读取和传递。

这种方式能够减少调度器的复杂度，也方便组员从 Todo List 中独立领取任务，并使用 Codex 分别开发和测试。
