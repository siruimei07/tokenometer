# Tokenometer Phase 1 检查点与恢复手册

> 最后更新：2026-08-09（Asia/Shanghai）
>
> 交付结论：Phase 0 必要前置与 Phase 1「Codex 数据核心」已实现；Phase 2 产品页面尚未开始。
>
> 权威架构：[`../ARCHITECTURE.md`](../ARCHITECTURE.md)

## 1. 中断后先做什么

按顺序执行，完成条件是确认本地审计修正没有被覆盖、全部门禁仍为绿色：

1. 运行 `git status --short --untracked-files=all` 和 `git diff --check`。
2. 保留当前工作树中的 Phase 1 终审修正；不要 reset，也不要修改已经进入 Git 历史的 `0001_initial.sql`。
3. 确认分支与基准提交：分支 `main`，已推送基准 `013d0f8e1f306d91f56379f99c5847ae6e0743fe`，当时与 `origin/main` 一致。
4. 按第 7 节恢复 Node/Rust 工具链，再运行第 6 节全部门禁。
5. 若门禁失败，先修 Phase 1 correctness/security；全部恢复绿色后才进入 Phase 2。

本次终审修正有意保留在工作树中，尚未得到用户的提交授权。`git status` 是文件清单的权威来源；不要把基准提交误认为最终实现已经全部提交。

最终验收时的预期 dirty 范围如下；出现列表外文件时先确认归属：

```text
ARCHITECTURE.md
docs/PHASE_1_HANDOFF.md
src-tauri/gen/schemas/capabilities.json
src-tauri/migrations/0002_tool_finish_payload_hash.sql
src-tauri/src/collector/codex.rs
src-tauri/src/collector/mod.rs
src-tauri/src/lib.rs
src-tauri/src/platform.rs
src-tauri/src/privacy.rs
src-tauri/src/state.rs
src-tauri/src/storage/mod.rs
src-tauri/tauri.conf.json
```

## 2. 架构完成度

| 能力 | 状态 | 已落地边界 |
|---|---|---|
| Phase 0 可启动壳 | 已实现 | Tauri 2、React/TS/Vite、最小窗口、托盘、single-instance、严格 CSP/capability、NSIS current-user + offline WebView2、唯一前端 bridge |
| Phase 1 Codex 数据核心 | 已实现 | allowlist、稳定 File ID、JSONL 增量解析、SQLite migration/原子 batch、双账 reconciliation、幂等、source health、后台 tick、history revision |
| Phase 1 最小 UI | 已实现 | 只展示 bootstrap/runtime/source 状态和手动刷新；可访问性优先，不追求视觉设计，不创建未来空页面 |
| Phase 2 历史产品纵切 | 未实现 | Dashboard、Usage、Sessions、Trends、Sources、Settings、typed queries、图表、工具预览 |
| Live / Limits / 多 provider | 未实现 | live listener/DTO 只做契约预留；Live 与网络额度 runtime 未启动 |

### Phase 1 验收语义

- 同一合成 transcript 的首次导入、重复扫描、追加、重启续读、active → archive 保持 canonical 总量一致且不重复。
- 损坏行/损坏文件与来源删除不会删除已经提交的好数据；健康状态单独退化。
- canonical usage 只汇总 `sessionCumulativeDelta`；`turnReported` 只用于 turn/reconciliation。
- 无 read-model 可见变化的 tick 不递增 `historyRevision`。
- fixture 全部人工合成，不包含真实 transcript、用户名、项目路径或 token。

## 3. 已实现模块地图

- `src-tauri/src/platform.rs`：Codex root、allowlist、reparse/最终 handle 校验、Windows File ID、ACL 与时区。
- `src-tauri/src/collector/jsonl.rs`：纯字节、有界的增量 JSONL reader。
- `src-tauri/src/collector/codex.rs`：版本化 parser、session/turn/tool/quota、双账 reconciliation 与稳定 record key。
- `src-tauri/src/collector/mod.rs`：discovery/poll、reset/replay、单文件隔离与 batch 编排。
- `src-tauri/src/storage/mod.rs`：migration、单 `Mutex<Connection>`、cursor CAS 与原子幂等写入。
- `src-tauri/src/storage/queries.rs`：bootstrap/source health/canonical checked totals。
- `src-tauri/src/state.rs`：AppState、刷新合并、revision gate 与 runtime health。
- `src-tauri/src/commands.rs`：窄 IPC；当前只有 `get_bootstrap`、`refresh_now`。
- `src-tauri/src/lib.rs`：启动、窗口/托盘、single-instance、2 秒/60 秒后台 tick 和 history event。
- `src/lib/contracts.ts`：前端 DTO；与 Rust 共用 `src-tauri/tests/fixtures/contracts/bootstrap.json` 做序列化回归。
- `src/lib/bridge.ts`：前端唯一可 import Tauri `invoke/listen` 的模块。
- `src/app/AppShell.tsx`：Phase 1 最小状态壳。

不要按这张地图预建 Phase 2 空壳；第二个真实调用者出现时再拆公共抽象。

## 4. 固定版本、接口与运行参数

| 项目 | 当前值 |
|---|---|
| SQLite schema | `0001` 已随 `013d0f8` 合并并冻结；当前工作树新增 `0002_tool_finish_payload_hash.sql`，后续继续递增 migration |
| parser state | `ParserStateV1`，版本 `1` |
| record key | `tokenometer/codex-line/*/v1` 与 `tokenometer/codex-fact/v1` |
| JSONL 限制 | 每 batch 4 MiB、单行 1 MiB、reader buffer 64 KiB |
| SQLite | WAL、foreign keys、busy timeout 5 秒、单 connection mutex |
| 调度 | poll 2 秒、discovery 60 秒、手动刷新 channel 容量 1；同一 root 单 lane |
| IPC | `get_bootstrap`、`refresh_now` |
| history event | `tokenometer://history-revision` |
| live event | `tokenometer://live-revision` 仅前端契约预留，Phase 1 不发布 |
| 首发平台 | Windows 10 build 19041+、x64、WebView2 |

Phase 2 的 TanStack Query、ECharts、lucide-react 尚未安装，这是有意延期，不是缺失依赖。

## 5. 必须保持的不变量

1. 只读 `%CODEX_HOME%`，否则 `%USERPROFILE%\.codex`；只允许 `sessions/**/rollout-*.jsonl`、`archived_sessions/rollout-*.jsonl`、`session_index.jsonl`。
2. 不读取 `auth.json`、Cookie/credential store、allowlist 外文件，也不修改 transcript。
3. prompt/reply/tool arguments/output、凭据、绝对路径和原始 OS error 不进入普通 DTO、错误或日志。
4. cache/reasoning 是 input/output 子集；计数使用 checked `i64`，不可重复相加或用 saturating 掩盖错误。
5. facts、cursor、parser state 在同一短事务提交；失败全部回滚。
6. replay 是 no-op；同 record identity、同 payload 是重复，同 identity、异 payload 是冲突并保留 last-good。
7. active/archive、shrink、File ID 变化、parser state 版本/哈希失配从 0 幂等重放，既有历史不回退。
8. tool locator 必须绑定采集时 File ID，预览前重新验证；数据库不保存 tool 正文。
9. React 只能通过 `src/lib/bridge.ts` 访问 Tauri。
10. `0001_initial.sql` 已冻结；新增 migration，不回改已合并文件。

## 6. 验证证据与剩余人工项

### 已通过的自动化门禁

- `npm run check`：TypeScript typecheck、ESLint、Vitest；2 个测试文件、6 个 case 通过。
- `npm run build`：生产前端构建通过，无远程资产。
- `cargo fmt --all -- --check`：通过。
- `cargo test --locked --all-targets`：46 个 Rust 单元测试与 1 个 Rust/TypeScript contract fixture 测试通过；包含 `0001 → 0002` 数据保留升级回归。
- `cargo clippy --locked --all-targets --all-features -- -D warnings`：通过。
- `tauri build --no-bundle`：release executable 构建通过。
- `tauri build --bundles nsis`：2026-08-09 在最终工作树重新执行，NSIS + offline WebView2 installer 构建通过。
- 隔离生产运行 smoke：空合成 `CODEX_HOME`/`LOCALAPPDATA` 下首实例保持运行、第二实例退出、清理前目标进程数为 1；未访问真实 transcript。

构建产物位于仓库外的临时 target，可删除重建，不能作为源代码检查点：

- `$env:CARGO_TARGET_DIR\release\tokenometer.exe`
- `$env:CARGO_TARGET_DIR\release\bundle\nsis\Tokenometer_0.1.0_x64-setup.exe`

### 仍需人工桌面验收

- `tauri dev` 实际操作：关闭到托盘、托盘打开/刷新/退出、1024×640 最小尺寸。
- 第二实例除了“只有一个进程”外，确认现有窗口被显示并获得焦点。
- release WebView 确认远程导航、新窗口和远程请求均被拒绝。
- 托盘退出已停止新 tick 并等待当前 snapshot/短事务临界区；仍需人工确认退出交互没有可感知卡顿。
- 托盘 tooltip 当前为静态 `Tokenometer`；“今日 Token”摘要属于 Phase 2 projection，尚未实现。

## 7. 工具链恢复与命令

当前普通 `PATH` 没有全局 `node/npm/cargo`。优先让 Codex Desktop 加载 bundled workspace dependencies；若仍处于本机当前环境，可用下列已验证入口。

### Node / npm

```powershell
$deps = Join-Path $env:USERPROFILE '.cache\codex-runtimes\codex-primary-runtime\dependencies'
$env:PATH = "$deps\node\bin;$env:PATH"
$npm = "$deps\bin\fallback\pnpm.cmd"
& $npm dlx npm@11.6.2 --version
& $npm dlx npm@11.6.2 ci
& $npm dlx npm@11.6.2 run check
& $npm dlx npm@11.6.2 run build
```

### Rust

```powershell
$workspace = (Resolve-Path '.').Path
$scratch = Join-Path (Split-Path -Parent $workspace) 'Temp\tokenometer-temp'
$rust = Join-Path $scratch 'rust-toolchain'
$env:CARGO_HOME = "$rust\cargo"
$env:RUSTUP_HOME = "$rust\rustup"
$env:CARGO_TARGET_DIR = Join-Path $scratch 'target-main'
$env:PATH = "$env:CARGO_HOME\bin;$env:PATH"
rustc --version
cargo --version
cargo fmt --manifest-path src-tauri/Cargo.toml --all -- --check
cargo test --manifest-path src-tauri/Cargo.toml --locked --all-targets
cargo clippy --manifest-path src-tauri/Cargo.toml --locked --all-targets --all-features -- -D warnings
& $npm dlx npm@11.6.2 run tauri -- build --no-bundle
& $npm dlx npm@11.6.2 run tauri -- build --bundles nsis
git diff --check
git status --short --untracked-files=all
```

本次验证使用 `rustc/cargo 1.97.1`。临时工具链路径不是仓库契约；路径失效时重新加载 Desktop bundled dependencies，不要把绝对路径写进项目配置。

## 8. 下一步

最早未完成阶段是 Phase 2。开始前先完成第 6 节人工桌面验收，并确认本文件列出的自动化门禁在当前工作树全部绿色。Phase 2 应按一个可运行纵切实现 typed queries 与 Dashboard/Usage/Sessions/Trends/Sources/Settings；保持 availability、measurement、revision 和隐私语义，不把 UI 查询下推成任意 SQL/path IPC。
