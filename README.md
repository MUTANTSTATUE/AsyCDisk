[README.md](https://github.com/user-attachments/files/31025669/README.md)
# AsyCDisk - 高性能异步网络云盘系统 (Asynchronous Cloud Disk System)

[![Language](https://img.shields.io/badge/Language-C%2B%2B17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Framework](https://img.shields.io/badge/I%2FO%20Engine-libuv-brightgreen.svg)](https://libuv.org/)
[![GUI](https://img.shields.io/badge/GUI-Qt6%20%2F%20QML-green.svg)](https://www.qt.io/)
[![Database](https://img.shields.io/badge/Database-SQLite3-blue.svg)](https://www.sqlite.org/)
[![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**AsyCDisk** 是一款基于 C++17 开发的高性能、低延迟、事件驱动型异步网络云盘系统。系统包含三大核心模块：高性能服务端 (**AsyCDisk**)、命令行客户端 (**AsyCClient_CLI**) 以及跨平台图形界面客户端 (**AsyClient_GUI**)。

系统采用自定义高效二进制 TCP 传输协议 (`ACDK` 协议)，基于 `libuv` 异步 I/O 架构实现百万级并发连接与大文件高速传输，支持断点续传、在线视频流媒体播放与拖拽寻轨 (Seek)、多用户隔离以及细粒度传输限速控制。

---

## 目录

- [项目架构与核心组件](#项目架构与核心组件)
- [核心特性](#核心特性)
- [系统架构设计](#系统架构设计)
- [传输协议 (ACDK Protocol)](#传输协议-acdk-protocol)
- [模块分析](#模块分析)
  - [1. AsyCDisk 服务端](#1-asycdisk-服务端)
  - [2. AsyCClient_CLI 命令行客户端](#2-asycclient_cli-命令行客户端)
  - [3. AsyClient_GUI 图形客户端](#3-asyclient_gui-图形客户端)
- [编译与构建说明](#编译与构建说明)
  - [环境依赖](#环境依赖)
  - [编译 AsyCDisk 服务端与 CLI](#编译-asycdisk-服务端与-cli)
  - [编译 AsyClient_GUI](#编译-asyclient_gui)
- [配置说明](#配置说明)
- [运行与使用](#运行与使用)
- [性能测试与基准](#性能测试与基准)

---

## 项目架构与核心组件

代码库目录结构如下：

```
.
├── AsyCDisk/               # 高性能异步网络云盘服务端核心
│   ├── include/            # 服务端头文件 (EventLoop, Session, Database, Protocol 等)
│   ├── src/                # 服务端源代码
│   ├── config.json         # 服务端配置文件
│   ├── CMakeLists.txt      # 服务端 CMake 构建配置
│   ├── performance_tests.py# 高并发与吞吐量自动化性能测试脚本
│   └── AsyCClient_CLI/     # C++ 命令行客户端 (CLI)
│       ├── AsyCClient.h/.cpp # 客户端核心网络与传输引擎库
│       ├── main.cpp        # 命令行交互入口
│       └── CMakeLists.txt  # CLI 构建配置
└── AsyClient_GUI/          # 基于 Qt6/QML 的跨平台桌面客户端
    ├── ClientWrapper.h/.cpp# C++ Qt 桥接层及内置本地 HTTP 媒体代理服务器
    ├── *.qml               # QML 现代化用户界面 (云盘管理/传输中心/完成列表/播放器)
    └── CMakeLists.txt      # GUI 构建配置
```

---

## 核心特性

- **⚡ 高并发异步非阻塞 I/O 架构**：
  服务端基于 `libuv` 事件循环构建，全面集成 Linux `epoll` / `io_uring` 与动态线程池 (`UV_THREADPOOL_SIZE`)，轻松支撑高并发网络请求与大文件异步磁盘 I/O。
- **📦 自定义高效二进制协议 (ACDK Protocol)**：
  固定 25 字节对齐协议头 + JSON 控制元数据 + Raw Binary 数据块流，支持单 TCP 连接上的多路复用与并行流管理 (`stream_id`)。
- **⏯️ 断点续传与任务持久化恢复**：
  支持上传/下载任务暂停、继续与取消；客户端自动记录分块进度与 `.meta` 传输元数据，应用重启后支持跨会话状态自动恢复。
- **🎬 本地 HTTP 代理在线视频流媒体播放**：
  GUI 客户端内置嵌入式 `QTcpServer` HTTP 代理服务器，可将云盘文件转换为标准 HTTP Range 字节流，支持 QML `MediaPlayer` 或外部播放器 (VLC/mpv) 秒开在线播放与任意位置 Seek 拖拽。
- **🔒 安全用户隔离与身份验证**：
  基于 SQLite3 数据库实现用户注册/登录、密文存储 (OpenSSL 加密)、会话 Session 校验以及严格的 `user_id` 传输任务隔离。
- **📊 细粒度带宽控制与流量调度**：
  支持全局与单会话上传/下载速率限制；针对流媒体播放提供优先带宽调度 (`is_streaming`)。
- **🖥️ 现代化多端客户端**：
  - **CLI 客户端**：轻量级命令行交互界面，类似 Unix 风格的导航与文件管理功能。
  - **GUI 客户端**：基于 Qt 6 / Quick Controls 2 的深色极简 UI，支持拖拽文件、实时速率图表、云端文件搜索与视频预览弹窗。

---

## 系统架构设计

```text
+-----------------------------------------------------------------------+
|                            AsyClient_GUI                              |
|   +-------------------+    +--------------------+  +--------------+   |
|   | QML User Interface| <->| C++ ClientWrapper  | <| Qt Proxy     |   |
|   | (Cloud/Transfer)  |    | (Signals & Slots)  |  | HTTP Server  |   |
|   +-------------------+    +---------+----------+  +------+-------+   |
+--------------------------------------|--------------------|-----------+
                                       | (Lib Connection)   | (Range Req)
+--------------------------------------v--------------------v-----------+
|                          AsyCClient Core Engine                       |
|        - Send/Recv Loop  - Stream Dispatcher  - Range Chunking        |
+--------------------------------------+--------------------------------+
                                       |
                           ACDK Binary TCP Protocol
                                       |
+--------------------------------------v--------------------------------+
|                             AsyCDisk Server                           |
|   +--------------------+   +-------------------+   +--------------+   |
|   |  libuv EventLoop   | < | TCP Session Mgr   | < | Dynamic      |   |
|   |  (io_uring/epoll)  |   | (Header / Payload)|   | Thread Pool  |   |
|   +---------+----------+   +---------+---------+   +------+-------+   |
|             |                        |                    |           |
|             v                        v                    v           |
|     +---------------+        +---------------+    +---------------+   |
|     |  SQLite3 DB   |        | Async File I/O|    | OpenSSL Crypto|   |
|     +---------------+        +---------------+    +---------------+   |
+-----------------------------------------------------------------------+
```

---

## 传输协议 (ACDK Protocol)

系统采用自定义的网络传输协议 `ACDK`，协议头使用 `#pragma pack(push, 1)` 保证无对齐补位，固定长度为 **25 字节**：

| 字段 (Field) | 类型 (Type) | 字节数 (Size) | 说明 (Description) |
| :--- | :--- | :--- | :--- |
| `magic` | `uint32_t` | 4 字节 | 魔数 `0x4B444341` ("ACDK")，用于协议识别与快速校验 |
| `version` | `uint8_t` | 1 字节 | 协议版本号 (当前为 `1`) |
| `command` | `uint16_t` | 2 字节 | 命令 ID (如 1: Login, 10: UploadReq, 12: DownloadReq) |
| `status` | `uint16_t` | 2 字节 | 状态码 (请求为 `0`；响应为 `200/40x/50x`) |
| `stream_id`| `uint32_t` | 4 字节 | 流 ID，用于并发任务多路复用与并行数据传输 |
| `json_len` | `uint32_t` | 4 字节 | 紧随其后的 JSON 控制载荷字节长度 |
| `binary_len`| `uint64_t`| 8 字节 | 紧随其后的 Raw Binary 原始数据块字节长度 |

### 核心 Command 指令列表

- `0: Ping` - 心跳保活与延迟测量
- `1: Login` - 用户身份验证
- `2: ListDir` - 获取指定目录文件列表
- `3: MakeDir` - 创建新目录
- `4: Remove` - 删除文件或目录
- `5: Register` - 用户注册
- `6: Move` - 移动文件/重命名
- `7: ListAllDirs` - 获取完整目录树
- `8: Search` - 全局文件名搜索
- `10: UploadReq` / `11: UploadData` - 文件上传请求与分块数据推送
- `12: DownloadReq` / `13: DownloadData` - 文件/Range分块下载请求与数据响应

---

## 模块分析

### 1. AsyCDisk 服务端
- **技术栈**：C++17, `libuv` (1.46.0), `SQLite3`, `OpenSSL`, `spdlog`, `nlohmann_json`
- **设计要点**：
  - `TcpServer` 与 `Session` 模式：每个客户端连接建立独立 Session，异步读取 25 字节 Header 后自动拆包并路由处理。
  - `EventLoop` 与异步文件 I/O：文件读写全部提交至 `libuv` 异步文件句柄，避免网络主线程卡顿。
  - 线程池配置：通过 `config.json` 可配置 `UV_THREADPOOL_SIZE` 与是否启用 `UV_USE_IO_URING`。
  - 数据安全与持久化：用户鉴权信息存储于 SQLite3，上传文件落盘于隔离的数据存储目录。

### 2. AsyCClient_CLI 命令行客户端
- **技术栈**：C++17, POSIX Threads, `nlohmann_json`
- **功能特点**：
  - 封装 `AsyCClient` 核心动态/静态库（解耦传输逻辑与 UI）。
  - 支持类似 Linux 终端的交互模式（`asyc:/path>` 提示符），支持 `ls`, `cd`, `pwd`, `mkdir`, `put`, `get`, `rm`, `mv` 等常用命令。
  - 包含命令行实时进度条显示与动态传输速率计算。

### 3. AsyClient_GUI 图形客户端
- **技术栈**：C++17, Qt 6 / Qt 5 (Core, Gui, Qml, Quick, QuickControls2, Network, Multimedia)
- **功能特点**：
  - **QML 前端**：模块化 QML 设计，包含云端网盘列表 (`CloudPage`)、传输任务队列 (`TransferPage`)、已完成历史记录 (`CompletedPage`) 和内置播放器窗口 (`PreviewWindow`)。
  - **内置 HTTP Proxy 服务器**：`ClientWrapper` 中实现无缝 HTTP 代理服务器，拦截 HTML5/Qt MediaPlayer 的 Range 请求，映射为二进制协议中的 Download Req，实现毫秒级在线 Seek 播放。
  - **断点续传管理**：实时保存未完成任务至 `tasks.json` 与已完成记录至 `completed.json`，基于 `user_id` 实现多账号隔离防护。

---

## 编译与构建说明

### 环境依赖

- **操作系统**：Linux (Ubuntu 20.04+ / Debian / Arch / Fedora 等)
- **编译器**：GCC >= 8.0 或 Clang >= 7.0 (支持 C++17)
- **构建工具**：CMake >= 3.16, Make 或 Ninja
- **系统库依赖**：
  - `sqlite3` (`libsqlite3-dev`)
  - `openssl` (`libssl-dev`)
  - `pthread`
  - `Qt6` 或 `Qt5` 开发包 (用于 GUI 编译: `qt6-base-dev`, `qt6-declarative-dev`, `qt6-multimedia-dev` 等)

### 编译 AsyCDisk 服务端与 CLI

```bash
# 1. 进入服务端目录
cd AsyCDisk

# 2. 创建构建目录并编译
mkdir -p build && cd build
cmake ..
make -j$(nproc)

# 3. 编译 CLI 客户端
cd ../AsyCClient_CLI
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

### 编译 AsyClient_GUI

```bash
# 1. 进入 GUI 目录
cd AsyClient_GUI

# 2. 创建构建目录并编译
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

---

## 配置说明

服务端配置文件 `config.json` 位于 `AsyCDisk/config.json`，在程序启动时自动加载：

```json
{
  "server": {
    "port": 8080
  },
  "libuv": {
    "threadpool_size": 8,
    "use_io_uring": true
  },
  "storage": {
    "data_dir": "data",
    "db_path": "asycdisk.db"
  },
  "limits": {
    "upload_kbps": -1,
    "download_kbps": -1
  }
}
```

- `threadpool_size`: `libuv` 异步文件 I/O 线程池大小（建议设为 CPU 核心数的 2-4 倍）。
- `use_io_uring`: 是否启用 Linux `io_uring` 高能内核接口。
- `upload_kbps` / `download_kbps`: 限制全局传输速率 (`-1` 为不限速)。

---

## 运行与使用

### 1. 启动服务端

```bash
cd AsyCDisk/build
./AsyCDisk
```

服务端默认开启 `0.0.0.0:8080` 监听，并自动初始化 `asycdisk.db` 数据库与 `data/` 存储目录。

### 2. 运行命令行客户端 (CLI)

```bash
cd AsyCDisk/AsyCClient_CLI/build
./asyc_cli
```
交互命令示例：
```bash
asyc:/> login admin 123456
asyc:/> mkdir my_folder
asyc:/> cd my_folder
asyc:/my_folder> put /path/to/local/file.mp4
asyc:/my_folder> ls
asyc:/my_folder> get 1
```

### 3. 运行桌面图形客户端 (GUI)

```bash
cd AsyClient_GUI/build
./AsyClient_GUI
```

---

## 性能测试与基准

项目内置 Python 自动化性能与并发压测脚本 `performance_tests.py`：

```bash
cd AsyCDisk
python3 performance_tests.py
```

压测能力包括：
- **并发连接与 Ping 延迟测试** (支持 100 ~ 1000+ 并发连接基准测试)
- **多流上传/下载吞吐量测试** (测试单流与多并行流下的 MB/s 极值)
