# ftpflow

以文件管理为主的 Windows 桌面 FTP / SSH 传输工作台。核心差异化：**在 SSH 侧把压缩与解压完全藏进传输管道**（上传前压、远端自动解压；下载反向同理），由五级智能判定引擎决定哪些文件值得压缩、哪些原样直传，用户全程无感。

技术方案依据：`v5.1 技术选型定案`（Qt Widgets / C++17 / 纯 Win32 核心 / LGPLv3 合规 / Windows 单平台）。

## 项目结构

```
ftpflow/
├── core/            core.lib —— 纯 C++17 + Win32，零 Qt 依赖
│   ├── include/ftpflow/
│   │   ├── vfs.hpp            统一目录条目 / 能力位
│   │   ├── vfs_local.hpp      本地文件后端（游标分页）
│   │   ├── transfer.hpp       传输任务 / 进度模型
│   │   ├── compress_decide.hpp 智能可压缩性判定（五级漏斗）
│   │   ├── protocol_status.hpp 协议引擎可用性
│   │   └── version.hpp
│   └── src/         判定 L1/L2/L3 已实现；L4/L5 与协议引擎随 M0 接入
├── cli/             ftpcli.exe —— 与桌面端共用 core.lib
├── app/             Qt Widgets 桌面壳（未装 Qt 时自动跳过）
├── tests/           零依赖断言式测试（70 项）+ bench_list 基准工具
├── tools/build_native.py   一键构建（vswhere → 直组装 MSVC/SDK 环境 → CMake+Ninja）
└── vcpkg.json       M0 协议依赖清单（baseline 已锁 8f363bd）
```

## 快速开始

前置：VS 2022 Build Tools（Desktop development with C++ 工作负载）。已在本机验证：MSVC 14.44 + VS 自带 CMake + Ninja。

```bat
python tools\build_native.py            Release + 测试（骨架模式，无需第三方库）
python tools\build_native.py --debug    Debug
python tools\build_native.py --tests    构建并跑单元测试
python tools\build_native.py --clean    清空重建
```

试一下 CLI：

```bat
build\release\cli\ftpcli.exe version
build\release\cli\ftpcli.exe decide README.md build\release\cli\ftpcli.exe
build\release\cli\ftpcli.exe info .
```

## M0 Spike 待办（两周期，对应方案第 12 章）

1. [x] vcpkg 清单锁定 baseline `8f363bd`（curl 默认 ssl=Windows schannel；libssh2 端口无 WinCNG 开关暂用 openssl 后端，exec 矩阵实测后再决定是否 overlay 切换；zstd）。`--full` 预设待依赖编译完成后打通。
2. [x] 判定 L4 试压（先落 Windows 原生 ntdll XPRESS_HUFF 零依赖路径，zstd -1 并列待依赖就绪）+ L5 带宽成本模型 `(1−r)S/B > margin·(S/C + rS/D)`。70 项断言全绿；33 MB 真实日志实测：20 MiB/s 链路 COMPRESS（省 77%、CPU 回报 1380%），10 GbE STORE（回报 22%）。1 万混合文件准确率统计待做。
3. [x] 10 万条目实测：`bench_list --create 100000`，首屏（含全量枚举+排序）45.8 ms cold / 40.7 ms warm，预算 300 ms。已知问题：每次 `list()` 重扫全目录，翻页全程 8.2 s → M1 做目录缓存 + ReadDirectoryChangesW 失效。
4. [ ] libssh2 exec 单流：远端 agent 投放 + zstd 边压边传 + 真实主机矩阵（含受限 sftp 账户）量覆盖率。
5. [ ] FTZ1 分块容器原型：块级 xxHash3 + SQLite 断点游标，1 GB 中断续传零误差。
6. [ ] Qt 6.12 在线安装器装 `MSVC 2022 x64_64` 组件，GUI 目标点亮。

## 约定

- 所有目标统一 `/MT` 静态 CRT（根 CMakeLists 强制，参照微信桌面端实践：用户零运行库依赖）。
- `core/` 永远不 include 任何 Qt 头 —— 桌面、CLI、未来的服务化形态共用同一份引擎。
- 代码与配置文件一律 ASCII 直引号；自然语言文档可用中文。
- 合规红线（详见方案第 9 章）：只链接 `qtbase`（LGPLv3 动态链接），随包提供 Qt 对应源码与替换说明，不使用任何 GPL-only 模块。
