# Tokenometer Phase 1 开发记录与恢复手册

> 最后更新：2026-08-08（Asia/Shanghai）  
> 当前范围：补齐 Phase 0 必要前置，并完成 Phase 1「Codex 数据核心」；Phase 2 产品页面不在本阶段。

## 1. 当前阶段结论

仓库已从只有 `ARCHITECTURE.md` 的空仓进入可编译应用实现阶段。当前实现遵循以下边界：

- 桌面宿主为 Tauri 2，前端为 React + TypeScript + Vite。
- React 只通过 `src/lib/bridge.ts` 调用窄 IPC；不接触 SQLite、任意文件或 shell。
- Rust 独占 Codex 根目录发现、allowlist 校验、只读文件句柄、JSONL 增量解析、SQLite 事务、健康状态和 revision。
- Phase 1 只实现 Codex；没有为单一 provider 建 trait、factory、DI 容器或插件协议。
- 本阶段 UI 只是可访问的最小状态壳，不创建 Dashboard/Usage/Sessions 等 Phase 2 产品页，也不展示虚构数据。

## 2. 已落地的架构内容

### Phase 0 必要前置

- Tauri 2 / React / TypeScript / Vite 工程壳与锁定依赖。
- 应用标识 `app.tokenometer.desktop`、最小窗口尺寸、NSIS current-user 安装和 offline WebView2 配置。
- 严格 CSP、`main` 窗口最小 capability、自定义 `get_bootstrap` / `refresh_now` 权限。
- single-instance、托盘打开/刷新/退出、关闭到托盘、手动创建 `main` 窗口。
- `bridge.ts` 是前端唯一可 import Tauri `invoke/listen` 的位置；listener-before-bootstrap 顺序已实现。
- 固定 capability、measurement、availability、history/live revision 和 runtime health DTO。

### Phase 1 Codex 数据核心

- SQLite v1 migration、migration checksum、WAL、foreign keys、busy timeout、稳定 device/account/source rows。
- 单个 `Mutex<Connection>`；解析发生在事务外，batch facts + parser state + cursor 在短 `IMMEDIATE` 事务内原子提交。
- cursor CAS 同时匹配 `cursor_generation + committed_offset + parser_state_hash`，防止 ABA。
- Codex 根目录解析与 allowlist：只接受 `sessions/**/rollout-*.jsonl`、`archived_sessions/rollout-*.jsonl`、`session_index.jsonl`。
- 拒绝 reparse point；打开后用 Windows handle 最终路径再次验证 root/allowlist，并读取 volume serial + file index 作为稳定 File ID。
- 有界 JSONL 增量读取：partial tail 不推进、完整 EOF 无换行可提交、坏完整行隔离、超大行跨 tick 有界丢弃。
- 版本化 `ParserStateV1`，保存会话/turn/model/effort、累计 baseline/generation、未闭合工具和质量计数。
- `total_token_usage` 只产生 canonical `sessionCumulativeDelta`；`last_token_usage` 只产生 `turnReported`，canonical 查询绝不把两者相加。
- token cache/reasoning 保持子集语义；所有计数为 checked `i64`；非法 breakdown 标记并进入 data-quality health。
- cumulative/turn reconciliation 支持 consistent、mismatch、reset、missing；可选子计数缺 baseline 时保持 unavailable，不虚构从零 delta。
- source record key 使用版本化 SHA-256、length-prefixed fields；同 key 同 payload 为 replay，同 key 异 payload 为冲突，禁止覆盖。
- active/archive 先以 transcript `session_meta.payload.id` 归一逻辑身份，路径和文件名不作为 canonical 会话身份。
- sessions/turns/tool calls/quota 只保存结构事实；不持久化 prompt、assistant 正文、tool arguments/output。
- tool locator 同时保存 byte offset/length 和采集时 File ID，后续预览必须复验身份。
- quota 只采 transcript 中的 account-level Codex 窗口；窗口分钟数不硬编码，percent 用定点 micros，reset seconds checked 转为 UTC ms。
- source health、2 秒已知文件 poll、60 秒 discovery、单 lane 与容量 1 的手动刷新合并。
- DB 可读后才发布 history revision；无新数据的 poll/replay 不递增 revision。

## 3. 关键文件入口

- `ARCHITECTURE.md`：权威规范和阶段边界。
- `src-tauri/migrations/0001_initial.sql`：当前空库 schema；尚未发布前可修正，发布后禁止修改。
- `src-tauri/src/domain.rs`：token/DTO/revision 领域类型。
- `src-tauri/src/platform.rs`：Windows 路径、File ID、ACL、时区和 allowlist 信任边界。
- `src-tauri/src/collector/jsonl.rs`：纯字节增量 reader。
- `src-tauri/src/collector/codex.rs`：Codex parser/reconciliation/source keys。
- `src-tauri/src/collector/mod.rs`：发现、poll、句柄复验、reset/replay 与 atomic commit 编排。
- `src-tauri/src/storage/mod.rs`：migration、cursor CAS、batch transaction 与幂等写入。
- `src-tauri/src/storage/queries.rs`：bootstrap/source health/canonical checked totals。
- `src-tauri/src/state.rs`：AppState、snapshot/revision gate、runtime health、刷新合并。
- `src-tauri/src/commands.rs`：窄 IPC。
- `src-tauri/src/lib.rs`：Tauri 启动顺序、single-instance、托盘、窗口和后台 tick。
- `src/lib/contracts.ts` / `src/lib/bridge.ts`：前端契约与唯一 Tauri bridge。
- `src/app/AppShell.tsx`：本阶段最小 UI。

## 4. 必须保持的不变量

1. 不读取 `auth.json`、浏览器 cookie/credential store 或 allowlist 外 Codex 文件。
2. 不修改源 transcript。
3. 不把 prompt/reply/tool arguments/output、绝对路径或原始 OS error 放进普通 DTO/错误/日志。
4. canonical usage 只汇总 `sessionCumulativeDelta`；`turnReported` 只能用于 turn 展示和 reconciliation。
5. cache/reasoning 是 input/output 子集，不能再次加进总量。
6. facts、cursor、parser state 必须同事务；失败时全部回滚。
7. replay 不能增加事实或总量；同身份不同 payload 必须报安全冲突并保留 last-good。
8. active → archive、重启、shrink、File ID 变化都不能让历史总量下降或重复。
9. 无可见变化的 2 秒检查不能发布 history revision。
10. React 不得绕过 `bridge.ts` 直接 import Tauri API。

## 5. 当前验证状态

- 存储模块已用隔离验证 crate 运行：11/11 tests passed，`clippy -D warnings` 通过。
- 前端依赖已成功解析并生成 `package-lock.json`；npm audit 为 0 vulnerabilities。
- 主 Tauri crate、前端 typecheck/lint/test/build 和最终 bundle 验证仍在本次工作中继续；完成后应更新本节为精确命令与结果。

## 6. 中断后的恢复顺序

1. 阅读本文件，再阅读 `ARCHITECTURE.md` 的 Phase 1、Codex ingestion、SQLite、IPC/security 和 test matrix。
2. 运行 `git status --short`，确认不要覆盖用户的无关修改。
3. 运行 `npm ci`，随后 `npm run check` 和 `npm run build`。
4. 运行 `cargo fmt --manifest-path src-tauri/Cargo.toml -- --check`。
5. 运行 `cargo test --manifest-path src-tauri/Cargo.toml`。
6. 运行 `cargo clippy --manifest-path src-tauri/Cargo.toml --all-targets -- -D warnings`。
7. 运行 `npm run tauri build -- --no-bundle`，再验证 NSIS bundle（若 offline WebView2 获取受网络限制，应如实记录）。
8. 优先修复 correctness/security 测试；不要为了通过测试降低 allowlist、CSP、File ID 或幂等约束。

## 7. 后续阶段边界

Phase 2 才实现 Dashboard、Usage、Sessions、Trends、Sources、Settings、typed query、图表和工具预览 UI。进入 Phase 2 前，应先确保本文件第 5 节的 Phase 1 验收全部为绿色，并新增 Rust serialization fixture 与 TypeScript 共享契约测试，避免跨语言 DTO 漂移。
