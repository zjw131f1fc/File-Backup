要求：采用测试驱动开发，测试先行，文档先行
做子模块时，在一个新的分支上做，后续合并到主分支
小步提交
子模块内聚

# Linux 本地备份系统

基于 doc.md v0.3 架构的 C++ 实现。

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

## 目录结构

```text
project/
├── common/              # 公共类型（Result, Progress, Request, EntryInfo 等）
├── scheduler/           # 调度器（备份调度、还原调度、任务管理）
├── modules/
│   ├── scanner/         # 扫描子模块（后续实现）
│   ├── filter/          # 篮选子模块（后续实现，18 分功能对应）
│   ├── archive_writer/  # 归档写入（后续实现）
│   ├── archive_reader/  # 归档读取（后续实现）
│   └── restore/         # 统一恢复（后续实现，含文件恢复+特殊文件+元数据）
├── third_party/         # gtest 等第三方依赖
├── doc.md               # 架构说明文档
└── interfaces.md        # 模块与接口规格说明
```

## 构建与测试

### 构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### 运行全部测试

```bash
cd build && ctest --output-on-failure
```

### 按模块运行测试

```bash
ctest -R scanner
ctest -R filter
ctest -R archive_writer
ctest -R archive_reader
ctest -R restore
ctest -R scheduler
ctest -R common
```

## 接口设计

### 核心原则

调度器不碰文件流，所有流式操作由子模块内部处理。

### 备份流程

```
1. runtime  = allocate the final archive path under the requested output directory
2. filter   = create_filter(request.filter_rules)
3. writer   = create_archive(request.output_path)
4. scanner.scan_and_backup(source_path, *filter, *writer, progress_cb)
5. result.ok() ? writer.commit() : writer.abort()
```

Scanner 内部：扫描 → 筛选 → 开源文件流 → 写入归档。调度器只调一次接口。

### 还原流程

```
1. reader = open_archive(request.archive_path)
2. reader.validate()
3. while reader.has_next_entry():
     reader.next_entry(info)
     restorer.restore_entry(target, info, *reader, conflict)
     restorer.restore_metadata(path, info)
```

Restorer 内部根据 EntryInfo.type 处理所有类型，普通文件时从 Reader 取流。调度器不做类型判断。

### 模块接口

| 模块 | 工厂函数 | 核心方法 |
|---|---|---|
| Scanner | `create_scanner()` | `scan_and_backup(src, filter, writer, progress_cb)` |
| Filter | `create_filter(FilterRules)` | `should_include(entry)` |
| ArchiveWriter | `create_archive(path)` | `add_entry`, `commit`, `abort` |
| ArchiveReader | `open_archive(path)` | `validate`, `has_next_entry`, `next_entry`, `open_content` |
| Restorer | `create_restorer()` | `restore_entry(target, info, reader, conflict)`, `restore_metadata(path, info)` |

### 模块开发

每个模块目录结构：

```text
modules/<name>/
├── include/modules/<name>/<name>.h   # 接口定义
├── src/<name>.cpp                     # 实现（当前为桩实现）
├── tests/
│   ├── CMakeLists.txt                 # 测试构建配置
│   └── <name>_test.cpp                # 测试代码
└── CMakeLists.txt                     # 模块构建配置
```

**模块开发工作流程：**

1. 领取模块任务
2. 替换 `src/` 下的桩实现为真实实现
3. 补充 `tests/` 下的测试用例
4. 确保 `ctest -R <模块名>` 通过

**接口规则：**

- 接口类以 `I` 开头（如 `IScanner`、`IRestorer`）
- 工厂函数以 `create_<name>()` 或 `open_<name>()` 返回 `std::unique_ptr<I...>`
- 所有错误通过 `Result` 返回，不使用 bool
- 大文件必须流式处理，调度器不碰文件流
- 失败时不留伪成功归档文件
- Restorer 统一处理所有条目类型，调度器不做类型路由

## 当前状态

- ✅ 公共类型定义完成
- ✅ 模块接口定义完成（5 个子模块）
- ✅ 桩实现完成（所有模块可编译链接）
- ✅ 调度器骨架完成（TaskManager、BackupScheduler、RestoreScheduler）
- ✅ 测试框架搭建完成（gtest + ctest，9/9 通过）
- ✅ 接口重新设计（调度器不碰文件流，Restorer 统一恢复）
- ⏳ 子模块真实实现（后续实现）
- ⏳ 接口契约测试
- ⏳ Web 后端 API
