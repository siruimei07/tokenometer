# Tokenometer 0.1.0

Tokenometer 是一款原生 Windows 使用量监看器，面向本机 Codex 与 ChatGPT。它使用 WinUI 3、C++/WinRT、Windows App SDK、Direct3D 11 与 SQLite，在四个紧凑页面、系统托盘和可拖动浮动气泡中展示 token、缓存命中、会话、工具与历史趋势。

v0.1 的原则是：只展示来源能够证明的数据，不伪造费用、额度或账户切换能力。

## 数据能力与边界

| 来源 | v0.1 的能力 | 更新方式 | 不可用的数据 |
|---|---|---|---|
| Codex（Windows） | 输入、缓存输入、输出、模型、项目、会话、提示、工具调用与 Codex 本地记录中出现的额度窗口 | 本地 JSONL 增量扫描约每 2 秒一次，界面约每秒刷新 | 账单费用、云端跨设备同步、登录账户切换 |
| Codex（正在运行的 WSL 发行版） | 与 Windows Codex 相同的 JSONL 报告计数，并按发行版标记设备 | 自动发现，约每 5 分钟合并一次 | 停止的发行版、SQLite/WAL 工具、远端账户额度 |
| ChatGPT | 从官方 `conversations.json` 当前可见分支的用户/助手文本生成会话、提示、模型与每日估算 | 用户在“设置”中手动导入；不是实时数据 | 精确 token、缓存、额度、费用、设备与工具调用 |

“Codex 精确”表示忠实记录 Codex transcript 中上报的 token 计数，并不表示 OpenAI 账单真值。采集是本地文件轮询，不是供应商 API 推送；两轮 Windows 扫描之间通常约 2 秒。Codex 额度只来自最近一次 transcript 快照，可能缺失或陈旧。ChatGPT 数值以可见文本 UTF-8 大小进行启发式估算，始终与 Codex 精确计数分开标记。订阅价格不能从这些本地来源可靠推导，因此 v0.1 将费用显示为“不可用”。

Tokenometer 不读取或写入 `auth.json`、浏览器 Cookie、ChatGPT/Codex 登录状态或凭据，也不包含遥测或上传使用记录的网络客户端。界面中的账户名称只是分组与显示标签，不会切换任何实际登录账户。

## 界面

底部导航在四个页面之间切换；默认状态适配单屏，只有展开详细内容时才出现滚动区域。

- **总览**：今日/累计摘要、Codex 额度、设备状态、活动热图和短期趋势。
- **详情**：Codex 可按工具、设备、模型、会话、项目或账户分组，ChatGPT 估算可按工具、模型、会话或账户分组；选择会话后查看提示 token 拆分，工具输入/输出仅在点击工具调用时按需读取。
- **趋势**：按工具或模型查看 7/30/90/365 天柱状图和 K 线；ChatGPT 数据保持“估算”标记。
- **设置**：导入 ChatGPT 官方导出、调整主题/透明度/模糊、管理工具顺序，并编辑托盘与气泡的预设或自定义布局。

系统托盘工具提示显示实时摘要或剩余比例最低的可用 Codex 额度。右键菜单可打开仪表盘、显示/隐藏浮动气泡或退出；悬停托盘图标可预览气泡，气泡支持拖动与置顶。`启动时收起到托盘` 仅控制启动 Tokenometer 后是否隐藏窗口，不会注册 Windows 登录自启动。

浮动气泡的“模糊”默认关闭。启用后，气泡仅在可见期间通过 Windows Graphics Capture 在本机内存/GPU 中读取当前显示器画面并裁剪为玻璃背景；画面不落盘、不上传，隐藏气泡或关闭模糊会停止捕获。`--no-backdrop` 可强制禁用这条路径。

## 使用

1. 启动 `Tokenometer.exe`。Windows 上的 Codex 历史会被自动发现并建立增量索引。
2. 在“详情”中选择分组；选择会话查看提示拆分，点击工具调用按需读取输入/输出。
3. 若需要 ChatGPT 历史，在“设置”中选择官方数据导出内的 `conversations.json`（也支持编号分卷，单文件上限 256 MiB），可为导入指定本地账户标签。
4. 在“设置”中选择托盘/气泡布局、可见工具、外观以及关闭到托盘行为。

WSL 只选择枚举时正在运行的发行版；Tokenometer 不主动选择已停止的发行版，但发行版若恰好在枚举与后续读取之间停止，`wsl.exe` 缺少原子的“仅当仍在运行时执行”选项，Windows 可能再次启动它。基于 SQLite/WAL 的工具不能安全地通过 Windows 的 WSL 文件共享实时读取；v0.1 尚未实现 WSL 内无头代理，这项能力标记为 experimental。设计约束与后续代理模式参见 [WSL SQLite setup](https://github.com/Javis603/token-monitor/blob/main/docs/wsl-sqlite-setup.md)。

## 构建

要求：

- Windows x64（最低目标 Windows 10 2004 / build 19041；Windows 11 可获得完整视觉效果）；
- Visual Studio 2026 或对应 Build Tools，安装 MSVC v145 C++ 工具；
- Windows 11 SDK `10.0.26100.0`；
- NuGet 依赖可在首次还原时访问：Windows App SDK `2.3.1` 与 C++/WinRT `3.0.260715.1`。

```powershell
# Debug
.\build.cmd -Configuration Debug

# Release
.\build.cmd -Configuration Release
```

输出目录：

```text
src\Tokenometer\bin\x64\Debug\
src\Tokenometer\bin\x64\Release\
```

运行存储、解析、趋势、ChatGPT 导入、WSL 与设置持久化自测：

```powershell
$test = Start-Process `
  -FilePath '.\src\Tokenometer\bin\x64\Debug\Tokenometer.exe' `
  -ArgumentList '--self-test-storage' `
  -Wait -PassThru
$test.ExitCode  # 0 表示通过
```

开发/诊断参数：

```text
--bubble          直接打开浮动气泡
--page-details    从详情页启动
--page-trends     从趋势页启动
--page-settings   从设置页启动
--no-collection   不启动 Codex/WSL 采集器
--no-backdrop     禁用动态玻璃背景
--self-test-storage
                  运行无界面自测并以退出码报告结果
```

应用采用单实例采集锁；已有实例运行时再次启动会直接退出。

## 分发

当前版本是免安装、未签名的 Windows App SDK 自包含 x64 构建。它仍使用动态 MSVC C++ 运行库；目标电脑需要安装与 v145 兼容的 x64 Microsoft Visual C++ Redistributable，或者分发包按 Microsoft 许可要求附带对应的 app-local VC Runtime。使用固定的发布脚本构建、自测并打包：

```powershell
.\package.ps1
```

脚本输出 `dist\Tokenometer-0.1.0-win-x64.zip` 与 `dist\SHA256SUMS.txt`。不要只复制 `Tokenometer.exe`：Windows App SDK 运行库、语言资源、PRI 文件以及 `Glass.hlsl` 都是运行所需内容。脚本排除 PDB 和本地数据文件，并依据实际 NuGet 解析图把包内自带的所有 `LICENSE`/`NOTICE` 文件保存在归档的 `LICENSES\<package>\<version>\` 下；只有许可 URL 的纯构建依赖会保留原始 NuGet manifest，`DEPENDENCIES.txt` 记录对应版本。

v0.1.0 未进行代码签名，Windows 下载保护可能在首次运行公开归档时显示未知发布者警告。它是开发者预览；只应从项目发布页下载、核对 SHA-256，并且不要以管理员身份运行。漏洞报告与更新完整性说明见 [SECURITY.md](SECURITY.md)。

## 本地数据与卸载

数据库、索引游标和界面设置存放在：

```text
%LOCALAPPDATA%\Tokenometer\tokenometer.db
```

SQLite 运行时可能在同一目录创建 `-wal` 与 `-shm` 辅助文件。Tokenometer 没有后台服务，也不进行跨设备云同步。彻底卸载时先从托盘退出程序，再删除应用目录以及 `%LOCALAPPDATA%\Tokenometer`；这不会删除 Codex 原始 transcript 或用户的 ChatGPT 导出文件。

Tokenometer 启动时会把数据目录以及 SQLite 的数据库、WAL、SHM 权限收紧为当前用户、SYSTEM 与本机 Administrators，并拒绝将数据目录或数据库放在 reparse point 上。数据库仍是未额外加密的普通 SQLite；同一用户下的恶意程序、管理员或离线磁盘读取仍可能访问，因此需要防范静态泄露时应启用 BitLocker 或 Windows 设备加密。工具输入/输出只在点击后按需读取并做常见凭据脱敏，但脱敏属于尽力而为，屏幕共享或截图时仍应把展开内容视为敏感信息。

完整的数据来源、保留期和隐私说明见 [docs/DATA_STORAGE.md](docs/DATA_STORAGE.md)。

## 许可证

[MIT](LICENSE)。第三方说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
