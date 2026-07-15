# 模块与接口规格说明

本文档详细描述每个子模块的职责和暴露接口的输入输出语义。后续实现子模块时必须满足这些契约。

---

## 1. IScanner — 扫描子模块

### 职责

递归走源目录树，识别每个文件系统对象（普通文件、目录、符号链接、硬链接、FIFO、设备文件），跳过 Unix Socket，不跟随符号链接。对每个条目调用 Filter 判断是否包含，包含的条目写入归档——普通文件开源文件流流式写入，非文件条目只写元数据。每处理一个条目调一次进度回调，回调返回 false 时停止扫描并返回 CANCELLED。

### 接口

#### create_scanner() → unique_ptr\<IScanner\>

创建扫描器实例。无参数，返回 Scanner 的智能指针。

#### scan_and_backup(source_path, IFilter&, IArchiveWriter&, ProgressCallback) → Result

执行完整的备份扫描流程。

| 参数 | 类型 | 说明 |
|---|---|---|
| source_path | const string& | 要扫描的源目录路径 |
| filter | IFilter& | 筛选器引用，由调用方创建并注入。Scanner 对每个条目调 filter.should_include 判断是否包含 |
| archive_writer | IArchiveWriter& | 归档写入器引用，包含的条目由 Scanner 内部写入：普通文件 Scanner 自己开源文件流传入 writer.add_entry，非文件条目调 writer.add_entry 只传元数据 |
| progress_callback | ProgressCallback | 进度回调函数。Scanner 每处理一个条目调用一次，传入当前进度；回调返回 true 表示继续，返回 false 表示取消，Scanner 收到 false 后停止扫描并返回 CANCELLED |

ProgressCallback 类型为 `bool(const Progress&)`。

返回 Result 表示整次扫描的最终结果：SUCCESS、FAILED、PARTIAL_SUCCESS 或 CANCELLED。

---

## 2. IFilter — 篮选子模块

### 职责

六类筛选——路径（包含/排除模式）、文件类型、名称（glob）、时间（newer/older）、尺寸（min/max）、用户（UID）。18 分功能直接对应此模块。FilterRules 在工厂函数时烘焙进实例，之后只需传条目就能判断。

### 接口

#### create_filter(FilterRules) → unique_ptr\<IFilter\>

创建筛选器实例。FilterRules 在此时烘焙进实例，之后不可更改。

FilterRules 包含六类筛选条件：

| 字段 | 类型 | 说明 |
|---|---|---|
| include_paths | vector\<string\> | 包含的路径模式（glob） |
| exclude_paths | vector\<string\> | 排除的路径模式（glob） |
| include_types | vector\<EntryType\> | 只包含的文件类型，空表示全部 |
| include_names | vector\<string\> | 包含的名称模式（glob） |
| exclude_names | vector\<string\> | 排除的名称模式（glob） |
| newer_than_sec | int64_t | 修改时间晚于此秒数的才包含，0 表示不限制 |
| older_than_sec | int64_t | 修改时间早于此秒数的才包含，0 表示不限制 |
| min_size | uint64_t | 文件大小下限（字节），0 表示不限制 |
| max_size | uint64_t | 文件大小上限（字节），0 表示不限制 |
| include_uids | vector\<uid_t\> | 只包含这些 UID 的文件，空表示全部 |

#### should_include(const EntryInfo&) → bool

判断一个条目是否应该被包含在备份中。

| 参数 | 类型 | 说明 |
|---|---|---|
| entry | const EntryInfo& | 条目信息，提供路径、类型、大小、修改时间、UID 等 |

EntryInfo 各字段与六类筛选的对应关系：

| 筛选类别 | 使用的 EntryInfo 字段 |
|---|---|
| 路径筛选 | path |
| 类型筛选 | type |
| 名称筛选 | path（提取文件名部分） |
| 时间筛选 | mtime_sec |
| 尺寸筛选 | size |
| 用户筛选 | uid |

返回 true 表示包含，false 表示排除。

---

## 3. IArchiveWriter — 归档写入子模块

### 职责

自定义归档格式的写入端。创建临时归档，流式写入普通文件内容，写入非文件条目的元数据（符号链接目标、硬链接指向、设备号等）。成功时 commit 提交归档，失败或取消时 abort 删除临时归档，不留伪成功文件。

### 接口

#### create_archive(output_path) → unique_ptr\<IArchiveWriter\>

创建归档写入器。

| 参数 | 类型 | 说明 |
|---|---|---|
| output_path | const string& | 归档文件的输出路径 |

内部创建临时归档，直到 commit 才变成正式文件。

#### add_entry(entry_info, istream&) → Result

写入一个有文件内容的条目，用于普通文件。

| 参数 | 类型 | 说明 |
|---|---|---|
| entry_info | const EntryInfo& | 条目元数据：路径、大小、权限等 |
| content | istream& | 文件内容输入流，归档写入器流式读取，不一次性加载到内存 |

返回 Result。失败时归档仍处于临时状态，可继续写入或 abort。

#### add_entry(entry_info) → Result

写入一个无文件内容的条目，用于目录、符号链接、硬链接、FIFO、字符设备、块设备等。

| 参数 | 类型 | 说明 |
|---|---|---|
| entry_info | const EntryInfo& | 条目元数据。符号链接用 link_target 字段，硬链接用 hard_link_target 字段，设备文件用 device_major/device_minor 字段 |

返回 Result。

#### commit() → Result

提交归档，将临时归档变为正式文件。调用后归档写入器不可再用。成功返回 SUCCESS，失败返回 FAILED。失败时调用方应确认临时文件已被清理。

#### abort() → Result

终止归档，删除临时归档文件。用于扫描失败或任务取消时，确保不留可被误认为成功的归档文件。调用后归档写入器不可再用。

---

## 4. IArchiveReader — 归档读取子模块

### 职责

自定义归档格式的读取端。打开归档后先 validate 检查格式标识、版本、截断、非法长度和危险路径。通过后逐条枚举条目信息，普通文件可以 open_content 获取流式内容读取器。

### 接口

#### open_archive(archive_path) → unique_ptr\<IArchiveReader\>

打开归档文件。

| 参数 | 类型 | 说明 |
|---|---|---|
| archive_path | const string& | 归档文件路径 |

返回读取器实例，后续通过它校验和枚举条目。

#### validate() → Result

校验归档。检查内容：

- 格式标识是否正确
- 版本是否兼容
- 归档是否截断
- 是否存在非法长度
- 是否存在危险路径（绝对路径或路径穿越如 `../`）

校验通过返回 SUCCESS。任何一项不通过返回 FAILED 并在 message 中附带具体错误信息。

#### has_next_entry() → bool

查询是否还有下一个条目可以读取。遍历开始前和每次 next_entry 后调用。返回 true 表示还有，false 表示已读完。

#### next_entry(EntryInfo&) → Result

读取下一个条目的元数据，填充到传入的 EntryInfo 引用中。

| 参数 | 类型 | 说明 |
|---|---|---|
| entry_info | EntryInfo& | 输出参数，由归档读取器根据条目类型填充 |

填充字段：path、type、size、permissions、uid、gid、atime_sec/nsec、mtime_sec/nsec、link_target（符号链接）、hard_link_target（硬链接）、device_major/minor（设备文件）等。

返回 SUCCESS 表示读取成功，FAILED 表示读取失败。

#### open_content(EntryInfo&) → unique_ptr\<istream\>

打开一个普通文件条目的内容流。

| 参数 | 类型 | 说明 |
|---|---|---|
| entry_info | const EntryInfo& | 要读取内容的条目，必须为 REGULAR_FILE 类型 |

返回内容流的智能指针，调用方流式读取文件内容。非普通文件条目调用此方法返回 nullptr。

---

## 5. IRestorer — 统一恢复子模块

### 职责

恢复所有类型的文件系统对象到目标目录。内部根据 EntryInfo.type 分发行为：

| EntryType | 行为 |
|---|---|
| REGULAR_FILE | 从 Reader 取流，写临时文件到目标路径旁，完成后 rename 替换正式目标，确保不留半写文件 |
| DIRECTORY | mkdir 创建目录 |
| SYMBOLIC_LINK | symlink 创建符号链接，目标取自 entry_info.link_target |
| HARD_LINK | link 创建硬链接，指向 entry_info.hard_link_target 对应的首次路径 |
| FIFO | mkfifo 创建命名管道 |
| CHARACTER_DEVICE | mknod 创建字符设备，主次号取自 entry_info.device_major/device_minor |
| BLOCK_DEVICE | mknod 创建块设备，主次号取自 entry_info.device_major/device_minor |

处理目标已存在的冲突（SKIP / OVERWRITE / RENAME）。

单独提供 restore_metadata 恢复权限、属主和时间戳，因为目录时间戳需要在子文件创建后才能正确设置。

### 接口

#### create_restorer() → unique_ptr\<IRestorer\>

创建恢复器实例。无参数，返回 Restorer 的智能指针。

#### restore_entry(target_root, EntryInfo&, IArchiveReader&, ConflictPolicy) → Result

恢复一个条目到目标目录。

| 参数 | 类型 | 说明 |
|---|---|---|
| target_root | const string& | 还原的根目录 |
| entry_info | const EntryInfo& | 要恢复的条目描述 |
| reader | IArchiveReader& | 归档读取器引用。当条目是普通文件时，Restorer 内部调 reader.open_content 取流读出内容写入目标；非文件条目不使用流但接口统一传入 |
| conflict_policy | ConflictPolicy | 目标已存在时的处理策略 |

ConflictPolicy 取值：

| 值 | 说明 |
|---|---|
| SKIP | 跳过已存在的目标文件 |
| OVERWRITE | 覆盖已存在的目标文件 |
| RENAME | 重命名新文件以避免冲突 |

返回 Result，SUCCESS 表示恢复成功，FAILED 表示恢复失败。

#### restore_metadata(target_path, EntryInfo&) → Result

恢复一个条目的元数据。

| 参数 | 类型 | 说明 |
|---|---|---|
| target_path | const string& | 目标文件系统上的完整路径 |
| entry_info | const EntryInfo& | 元数据来源 |

恢复内容：

- chmod 设置权限（取自 entry_info.permissions）
- chown 设置属主和属组（取自 entry_info.uid / gid）
- utimensat 设置访问时间和修改时间（取自 entry_info.atime_sec/nsec / mtime_sec/nsec）

权限不足时尽可能恢复，记录 warning 但不视为失败。返回 Result。

---

## 6. TaskManager — 任务管理

### 职责

管理任务生命周期——创建、查询、取消、更新进度、标记完成。Web 层通过它提交请求和获取状态。

### 接口

#### create_backup_task(BackupRequest) → string

创建一个备份任务。

| 参数 | 类型 | 说明 |
|---|---|---|
| request | const BackupRequest& | 包含 source_path、output_path、filter_rules |

返回任务 ID 字符串，任务初始状态为 PENDING。

#### create_restore_task(RestoreRequest) → string

创建一个还原任务。

| 参数 | 类型 | 说明 |
|---|---|---|
| request | const RestoreRequest& | 包含 archive_path、target_path、conflict_policy |

返回任务 ID 字符串，任务初始状态为 PENDING。

#### get_task(task_id) → Task

查询任务信息。

| 参数 | 类型 | 说明 |
|---|---|---|
| task_id | const string& | 任务 ID |

返回 Task 结构体，包含 task_id、status、progress、result。task_id 不存在时返回 Task（status = FAILED，result.message = "task not found: ..."）。

TaskStatus 取值：

| 值 | 说明 |
|---|---|
| PENDING | 任务已创建，等待执行 |
| RUNNING | 任务正在执行 |
| SUCCESS | 任务成功完成 |
| PARTIAL_SUCCESS | 任务部分成功（有错误但不致命） |
| FAILED | 任务失败 |
| CANCELLED | 任务被用户取消 |

#### cancel_task(task_id) → bool

取消任务。

| 参数 | 类型 | 说明 |
|---|---|---|
| task_id | const string& | 任务 ID |

仅在任务状态为 PENDING 或 RUNNING 时生效，将状态设为 CANCELLED。已完成或已取消的任务返回 false。

备份侧的取消传播：调度器的进度回调检测到 CANCELLED 后返回 false，Scanner 停止扫描。

还原侧的取消传播：调度器每次循环前检查任务状态，发现取消后直接退出。

#### update_progress(task_id, Progress)

更新任务进度。

| 参数 | 类型 | 说明 |
|---|---|---|
| task_id | const string& | 任务 ID |
| progress | const Progress& | 进度信息 |

Progress 字段：

| 字段 | 类型 | 说明 |
|---|---|---|
| stage | string | 当前阶段名（如 "scanning"、"restoring"、"committing_archive"） |
| processed_entries | uint64_t | 已处理条目数 |
| processed_bytes | uint64_t | 已处理字节数 |
| current_path | string | 正在处理的路径 |

任务 ID 不存在时静默忽略。

#### complete_task(task_id, Result)

标记任务完成。根据 Result.status 设置任务的最终状态：

| Result.status | Task 最终状态 |
|---|---|
| SUCCESS | SUCCESS |
| PARTIAL_SUCCESS | PARTIAL_SUCCESS |
| CANCELLED | CANCELLED |
| FAILED | FAILED |

同时保存 Result 的 message、error_count、warning_count 到任务记录。任务 ID 不存在时静默忽略。

---

## 7. 调度器编排流程

### 备份流程

```
1. filter   = create_filter(request.filter_rules)     // 创建筛选器，规则烘焙进去
2. writer   = create_archive(request.output_path)     // 创建归档写入器（临时归档）
3. result   = scanner.scan_and_backup(                // 一次调用完成扫描+筛选+写入
                request.source_path,
                *filter,
                *writer,
                progress_cb)                           // 回调里检查取消信号
4. result.ok() ? writer.commit() : writer.abort()     // 成功提交，失败或取消终止
```

进度回调 `progress_cb` 实现：更新任务进度，同时检查 `task.status != CANCELLED`，若被取消则返回 false 让 Scanner 停止。

### 还原流程

```
1. reader   = open_archive(request.archive_path)      // 打开归档
2. reader.validate()                                   // 校验格式、版本、截断、危险路径
3. while reader.has_next_entry():                      // 逐条枚举
     if task cancelled → return CANCELLED              // 每次迭代前检查取消
     reader.next_entry(info)                           // 读取条目元数据
     restorer.restore_entry(                           // 恢复条目（Restorer 内部取流）
         target_path, info, *reader, conflict)
     restorer.restore_metadata(path, info)             // 恢复元数据
4. 返回结果（SUCCESS 或 PARTIAL_SUCCESS）
```

调度器全程不碰文件流，不做条目类型判断。
