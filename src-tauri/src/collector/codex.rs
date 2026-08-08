use std::collections::BTreeMap;

use chrono::DateTime;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};
use thiserror::Error;

use crate::{
    domain::{CountComparison, Reconciliation, TokenCounts},
    platform::safe_project_label,
    privacy::{sanitize_bounded, strict_identifier},
};

use super::jsonl::{JsonlBatch, JsonlLine};

pub const PARSER_STATE_VERSION: i64 = 1;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PendingToolState {
    pub call_key: String,
    pub source_record_key: String,
    pub tool_name: String,
    pub started_at_ms: Option<i64>,
    pub turn_key: Option<String>,
    pub input_offset: u64,
    pub input_length: u64,
    pub source_file_identity: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default, rename_all = "camelCase")]
pub struct ParserStateV1 {
    pub logical_session_id: Option<String>,
    pub session_started_at_ms: Option<i64>,
    pub last_activity_at_ms: Option<i64>,
    pub originator: Option<String>,
    pub cli_version: Option<String>,
    pub workspace_path: Option<String>,
    pub model: Option<String>,
    pub effort: Option<String>,
    pub current_turn_key: Option<String>,
    pub current_turn_started_at_ms: Option<i64>,
    pub prompt_index: u64,
    pub cumulative_generation: u64,
    pub cumulative_baseline: Option<TokenCounts>,
    pub pending_tools: BTreeMap<String, PendingToolState>,
    pub awaiting_assistant: bool,
    pub fork_like: bool,
    pub counting_boundary_seen: bool,
    pub discarding_oversized_line: bool,
    pub malformed_records: u64,
    pub oversized_records: u64,
    pub data_quality_errors: u64,
}

impl Default for ParserStateV1 {
    fn default() -> Self {
        Self {
            logical_session_id: None,
            session_started_at_ms: None,
            last_activity_at_ms: None,
            originator: None,
            cli_version: None,
            workspace_path: None,
            model: None,
            effort: None,
            current_turn_key: None,
            current_turn_started_at_ms: None,
            prompt_index: 0,
            cumulative_generation: 0,
            cumulative_baseline: None,
            pending_tools: BTreeMap::new(),
            awaiting_assistant: false,
            fork_like: false,
            counting_boundary_seen: true,
            discarding_oversized_line: false,
            malformed_records: 0,
            oversized_records: 0,
            data_quality_errors: 0,
        }
    }
}

impl ParserStateV1 {
    pub fn status(&self) -> TranscriptStatus {
        if !self.pending_tools.is_empty() {
            TranscriptStatus::Executing
        } else if self.awaiting_assistant {
            TranscriptStatus::Thinking
        } else {
            TranscriptStatus::Unknown
        }
    }

    pub fn reset_for_replay(&self) -> Self {
        Self::default()
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TranscriptStatus {
    Executing,
    Thinking,
    Unknown,
}

impl TranscriptStatus {
    pub fn as_db_str(self) -> &'static str {
        match self {
            Self::Executing => "executing",
            Self::Thinking => "thinking",
            Self::Unknown => "unknown",
        }
    }

    pub fn reason(self) -> Option<&'static str> {
        match self {
            Self::Executing => Some("transcriptHeuristic:pendingToolCall"),
            Self::Thinking => Some("transcriptHeuristic:userAwaitingAssistant"),
            Self::Unknown => None,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct KeyContext {
    pub device_public_id: String,
    pub source_root_public_id: String,
    pub logical_file_id: String,
    pub source_file_identity: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MetricScope {
    SessionCumulativeDelta,
    TurnReported,
}

impl MetricScope {
    pub fn as_db_str(self) -> &'static str {
        match self {
            Self::SessionCumulativeDelta => "sessionCumulativeDelta",
            Self::TurnReported => "turnReported",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SessionSnapshotFact {
    pub session_id: String,
    pub originator: Option<String>,
    pub cli_version: Option<String>,
    pub workspace_path: Option<String>,
    pub safe_project_label: Option<String>,
    pub started_at_ms: Option<i64>,
    pub updated_at_ms: Option<i64>,
    pub model: Option<String>,
    pub effort: Option<String>,
    pub transcript_status: TranscriptStatus,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TurnFact {
    pub turn_key: String,
    pub prompt_index: u64,
    pub model: Option<String>,
    pub effort: Option<String>,
    pub workspace_path: Option<String>,
    pub started_at_ms: Option<i64>,
    pub updated_at_ms: Option<i64>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UsageFact {
    pub source_record_key: String,
    pub source_payload_hash: String,
    pub session_id: String,
    pub turn_key: Option<String>,
    pub metric_scope: MetricScope,
    pub cumulative_generation: u64,
    pub effective_at_ms: Option<i64>,
    pub observed_at_ms: i64,
    pub model: Option<String>,
    pub effort: Option<String>,
    pub reconciliation: Reconciliation,
    pub counts: TokenCounts,
    pub source_offset: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ToolStartFact {
    pub source_record_key: String,
    pub source_payload_hash: String,
    pub session_id: String,
    pub turn_key: Option<String>,
    pub call_key: String,
    pub tool_name: String,
    pub started_at_ms: Option<i64>,
    pub input_offset: u64,
    pub input_length: u64,
    pub source_file_identity: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ToolFinishFact {
    pub start_source_record_key: String,
    pub closed_by_record_key: String,
    pub ended_at_ms: Option<i64>,
    pub output_offset: u64,
    pub output_length: u64,
    pub output_file_identity: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct QuotaWindowFact {
    pub window_key: String,
    pub window_kind: String,
    pub label: String,
    pub window_minutes: Option<i64>,
    pub used_percent_micros: Option<i64>,
    pub resets_at_ms: Option<i64>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct QuotaSnapshotFact {
    pub source_record_key: String,
    pub source_payload_hash: String,
    pub session_id: String,
    pub provider_limit_id: Option<String>,
    pub captured_at_ms: Option<i64>,
    pub observed_at_ms: i64,
    pub plan: Option<String>,
    pub windows: Vec<QuotaWindowFact>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ParsedBatch {
    pub next_state: ParserStateV1,
    pub committed_offset: u64,
    pub session: Option<SessionSnapshotFact>,
    pub turns: Vec<TurnFact>,
    pub usage: Vec<UsageFact>,
    pub tool_starts: Vec<ToolStartFact>,
    pub tool_finishes: Vec<ToolFinishFact>,
    pub quotas: Vec<QuotaSnapshotFact>,
}

impl ParsedBatch {
    pub fn has_visible_facts(&self) -> bool {
        self.session.is_some()
            || !self.turns.is_empty()
            || !self.usage.is_empty()
            || !self.tool_starts.is_empty()
            || !self.tool_finishes.is_empty()
            || !self.quotas.is_empty()
    }
}

#[derive(Debug, Error)]
pub enum ParseError {
    #[error("the logical session identity changed during replay")]
    SessionIdentityConflict,
    #[error("a usage-bearing rollout has no session metadata")]
    MissingSessionMetadata,
    #[error("parser counters overflowed")]
    CounterOverflow,
}

enum PreparedLine {
    Blank(JsonlLine),
    Malformed(JsonlLine),
    Json {
        line: JsonlLine,
        value: Value,
        payload_hash: String,
    },
    Incomplete,
}

pub fn parse_batch(
    batch: JsonlBatch,
    mut state: ParserStateV1,
    context: &KeyContext,
    observed_at_ms: i64,
) -> Result<ParsedBatch, ParseError> {
    state.discarding_oversized_line = batch.discarding_oversized_line;
    state.oversized_records = state
        .oversized_records
        .checked_add(batch.oversized_records)
        .ok_or(ParseError::CounterOverflow)?;

    let prepared = prepare_lines(batch.lines);
    let discovered_session = prepared.iter().find_map(|entry| match entry {
        PreparedLine::Json { value, .. } if outer_type(value) == Some("session_meta") => value
            .get("payload")
            .and_then(|payload| payload.get("id"))
            .and_then(Value::as_str)
            .and_then(|id| strict_identifier(id, 256)),
        _ => None,
    });

    if let (Some(existing), Some(discovered)) = (
        state.logical_session_id.as_deref(),
        discovered_session.as_deref(),
    ) && existing != discovered
    {
        return Err(ParseError::SessionIdentityConflict);
    }
    if state.logical_session_id.is_none() {
        state.logical_session_id = discovered_session;
    }
    if state.logical_session_id.is_none() && prepared.iter().any(is_usage_bearing) {
        return Err(ParseError::MissingSessionMetadata);
    }

    let mut committed_offset = batch.committed_through;
    let mut turns = Vec::new();
    let mut usage = Vec::new();
    let mut tool_starts = Vec::new();
    let mut tool_finishes = Vec::new();
    let mut quotas = Vec::new();

    for entry in prepared {
        match entry {
            PreparedLine::Blank(line) => {
                committed_offset = committed_offset.max(line.end_offset);
            }
            PreparedLine::Malformed(line) => {
                state.malformed_records = state
                    .malformed_records
                    .checked_add(1)
                    .ok_or(ParseError::CounterOverflow)?;
                committed_offset = committed_offset.max(line.end_offset);
            }
            PreparedLine::Incomplete => break,
            PreparedLine::Json {
                line,
                value,
                payload_hash,
            } => {
                process_record(
                    &line,
                    &value,
                    &payload_hash,
                    context,
                    observed_at_ms,
                    &mut state,
                    &mut turns,
                    &mut usage,
                    &mut tool_starts,
                    &mut tool_finishes,
                    &mut quotas,
                )?;
                committed_offset = committed_offset.max(line.end_offset);
            }
        }
    }

    let session = state
        .logical_session_id
        .as_ref()
        .map(|session_id| SessionSnapshotFact {
            session_id: session_id.clone(),
            originator: state.originator.clone(),
            cli_version: state.cli_version.clone(),
            workspace_path: state.workspace_path.clone(),
            safe_project_label: state.workspace_path.as_deref().and_then(safe_project_label),
            started_at_ms: state.session_started_at_ms,
            updated_at_ms: state.last_activity_at_ms,
            model: state.model.clone(),
            effort: state.effort.clone(),
            transcript_status: state.status(),
        });

    Ok(ParsedBatch {
        next_state: state,
        committed_offset,
        session,
        turns,
        usage,
        tool_starts,
        tool_finishes,
        quotas,
    })
}

fn prepare_lines(lines: Vec<JsonlLine>) -> Vec<PreparedLine> {
    lines
        .into_iter()
        .map(|line| {
            if line.bytes.iter().all(u8::is_ascii_whitespace) {
                return PreparedLine::Blank(line);
            }
            match serde_json::from_slice::<Value>(&line.bytes) {
                Ok(value) if value.is_object() => PreparedLine::Json {
                    payload_hash: hash_bytes(&line.bytes),
                    line,
                    value,
                },
                Ok(_) | Err(_) if line.terminated_by_newline => PreparedLine::Malformed(line),
                Ok(_) | Err(_) => PreparedLine::Incomplete,
            }
        })
        .collect()
}

fn is_usage_bearing(entry: &PreparedLine) -> bool {
    let PreparedLine::Json { value, .. } = entry else {
        return false;
    };
    matches!(
        (outer_type(value), inner_type(value)),
        (
            Some("event_msg"),
            Some("token_count" | "user_message" | "agent_message")
        ) | (Some("response_item"), Some(_))
    )
}

#[allow(clippy::too_many_arguments)]
fn process_record(
    line: &JsonlLine,
    value: &Value,
    payload_hash: &str,
    context: &KeyContext,
    observed_at_ms: i64,
    state: &mut ParserStateV1,
    turns: &mut Vec<TurnFact>,
    usage: &mut Vec<UsageFact>,
    tool_starts: &mut Vec<ToolStartFact>,
    tool_finishes: &mut Vec<ToolFinishFact>,
    quotas: &mut Vec<QuotaSnapshotFact>,
) -> Result<(), ParseError> {
    let outer = outer_type(value).unwrap_or("unknown");
    let inner = inner_type(value).unwrap_or("unknown");
    let payload = value.get("payload").unwrap_or(&Value::Null);
    let timestamp = parse_record_timestamp(value);
    if value.get("timestamp").is_some() && timestamp.is_none() {
        increment_quality(state)?;
    }
    if let Some(timestamp) = timestamp {
        state.last_activity_at_ms = Some(
            state
                .last_activity_at_ms
                .map_or(timestamp, |previous| previous.max(timestamp)),
        );
    }

    match (outer, inner) {
        ("session_meta", _) => parse_session_meta(payload, timestamp, state)?,
        ("turn_context", _) => {
            update_context(payload, state);
            if let Some(turn) = current_turn_fact(state, timestamp) {
                turns.push(turn);
            }
        }
        ("event_msg", "task_started") => {
            state.counting_boundary_seen = true;
        }
        ("event_msg", "user_message") => {
            state.prompt_index = state
                .prompt_index
                .checked_add(1)
                .ok_or(ParseError::CounterOverflow)?;
            state.current_turn_key = Some(format!("prompt/{}", state.prompt_index));
            state.current_turn_started_at_ms = timestamp;
            state.awaiting_assistant = true;
            if let Some(turn) = current_turn_fact(state, timestamp) {
                turns.push(turn);
            }
        }
        ("event_msg", "agent_message") => state.awaiting_assistant = false,
        ("event_msg", "token_count") => {
            parse_token_count(
                line,
                value,
                payload,
                payload_hash,
                context,
                observed_at_ms,
                timestamp,
                state,
                usage,
                quotas,
            )?;
        }
        ("response_item", "function_call" | "custom_tool_call" | "tool_search_call") => {
            start_tool(
                line,
                value,
                payload,
                payload_hash,
                context,
                timestamp,
                state,
                tool_starts,
            );
        }
        (
            "response_item",
            "function_call_output" | "custom_tool_call_output" | "tool_search_call_output",
        )
        | ("event_msg", "exec_command_end" | "mcp_tool_call_end") => {
            finish_tool(
                line,
                value,
                payload,
                context,
                timestamp,
                state,
                tool_finishes,
            );
        }
        ("response_item", "message")
            if payload.get("role").and_then(Value::as_str) == Some("assistant") =>
        {
            state.awaiting_assistant = false;
        }
        _ => {}
    }
    Ok(())
}

fn parse_session_meta(
    payload: &Value,
    record_timestamp: Option<i64>,
    state: &mut ParserStateV1,
) -> Result<(), ParseError> {
    if let Some(id) = payload.get("id").and_then(Value::as_str) {
        let Some(id) = strict_identifier(id, 256) else {
            increment_quality(state)?;
            return Ok(());
        };
        if let Some(existing) = state.logical_session_id.as_deref() {
            if existing != id {
                return Err(ParseError::SessionIdentityConflict);
            }
        } else if !id.is_empty() {
            state.logical_session_id = Some(id);
        }
    }
    let payload_timestamp = payload
        .get("timestamp")
        .and_then(Value::as_str)
        .and_then(parse_rfc3339_ms);
    let started = payload_timestamp.or(record_timestamp);
    if let Some(started) = started {
        state.session_started_at_ms = Some(
            state
                .session_started_at_ms
                .map_or(started, |previous| previous.min(started)),
        );
    }
    state.originator = bounded_string(payload, &["originator"], 128).or(state.originator.take());
    state.cli_version = bounded_string(payload, &["cli_version"], 64).or(state.cli_version.take());
    state.workspace_path = bounded_string(payload, &["cwd"], 4096).or(state.workspace_path.take());

    let source_evidence = [
        payload.get("originator").and_then(Value::as_str),
        payload.get("source").and_then(Value::as_str),
    ]
    .into_iter()
    .flatten()
    .any(|value| {
        let lower = value.to_ascii_lowercase();
        lower.contains("subagent") || lower.contains("sub-agent") || lower.contains("fork")
    });
    if source_evidence {
        state.fork_like = true;
        state.counting_boundary_seen = false;
    }
    Ok(())
}

fn update_context(payload: &Value, state: &mut ParserStateV1) {
    if let Some(model) = bounded_string(payload, &["model"], 128) {
        state.model = Some(model);
    }
    if let Some(effort) = bounded_string(payload, &["effort"], 64) {
        state.effort = Some(effort);
    }
    if let Some(cwd) = bounded_string(payload, &["cwd"], 4096) {
        state.workspace_path = Some(cwd);
    }
}

fn current_turn_fact(state: &ParserStateV1, updated_at_ms: Option<i64>) -> Option<TurnFact> {
    Some(TurnFact {
        turn_key: state.current_turn_key.clone()?,
        prompt_index: state.prompt_index,
        model: state.model.clone(),
        effort: state.effort.clone(),
        workspace_path: state.workspace_path.clone(),
        started_at_ms: state.current_turn_started_at_ms,
        updated_at_ms,
    })
}

#[allow(clippy::too_many_arguments)]
fn parse_token_count(
    line: &JsonlLine,
    value: &Value,
    payload: &Value,
    payload_hash: &str,
    context: &KeyContext,
    observed_at_ms: i64,
    timestamp: Option<i64>,
    state: &mut ParserStateV1,
    usage: &mut Vec<UsageFact>,
    quotas: &mut Vec<QuotaSnapshotFact>,
) -> Result<(), ParseError> {
    let session_id = state
        .logical_session_id
        .clone()
        .ok_or(ParseError::MissingSessionMetadata)?;
    let info = payload.get("info").and_then(Value::as_object);
    let total_value = info.and_then(|info| info.get("total_token_usage"));
    let last_value = info.and_then(|info| info.get("last_token_usage"));
    let total = parse_optional_counts(total_value);
    let last = parse_optional_counts(last_value);
    let invalid_counts =
        total.as_ref().is_some_and(Result::is_err) || last.as_ref().is_some_and(Result::is_err);

    let line_key = line_key(value, line.start_offset, state, context);
    if invalid_counts {
        increment_quality(state)?;
    } else {
        let total = total.and_then(Result::ok);
        let last = last.and_then(Result::ok);
        if total
            .as_ref()
            .is_some_and(|counts| counts.invalid_breakdown)
            || last.as_ref().is_some_and(|counts| counts.invalid_breakdown)
        {
            increment_quality(state)?;
        }
        let mut delta = None;
        let mut reset = false;

        if let Some(current) = total.as_ref() {
            match current.delta_from(state.cumulative_baseline.as_ref()) {
                Ok(value) => delta = Some(value),
                Err(crate::domain::CountError::CounterDecreased) => {
                    state.cumulative_generation = state
                        .cumulative_generation
                        .checked_add(1)
                        .ok_or(ParseError::CounterOverflow)?;
                    delta = Some(current.clone());
                    reset = true;
                }
                Err(_) => increment_quality(state)?,
            }
            state.cumulative_baseline = Some(current.clone());
        }

        let reconciliation = if reset {
            Reconciliation::Reset
        } else {
            match (delta.as_ref(), last.as_ref()) {
                (Some(delta), Some(last)) => match delta.compare_components(last) {
                    CountComparison::Equal => Reconciliation::Consistent,
                    CountComparison::Different => Reconciliation::Mismatch,
                    CountComparison::Incomplete => Reconciliation::Missing,
                },
                _ => Reconciliation::Missing,
            }
        };

        let counting_enabled = !state.fork_like || state.counting_boundary_seen;
        if counting_enabled {
            if let Some(delta) = delta.filter(|counts| !counts.is_zero()) {
                usage.push(UsageFact {
                    source_record_key: fact_key(&line_key, "cumulative-delta"),
                    source_payload_hash: payload_hash.to_string(),
                    session_id: session_id.clone(),
                    turn_key: state.current_turn_key.clone(),
                    metric_scope: MetricScope::SessionCumulativeDelta,
                    cumulative_generation: state.cumulative_generation,
                    effective_at_ms: timestamp,
                    observed_at_ms,
                    model: state.model.clone(),
                    effort: state.effort.clone(),
                    reconciliation,
                    counts: delta,
                    source_offset: line.start_offset,
                });
            }
            if let Some(last) = last.filter(|counts| !counts.is_zero()) {
                usage.push(UsageFact {
                    source_record_key: fact_key(&line_key, "turn-reported"),
                    source_payload_hash: payload_hash.to_string(),
                    session_id: session_id.clone(),
                    turn_key: state.current_turn_key.clone(),
                    metric_scope: MetricScope::TurnReported,
                    cumulative_generation: state.cumulative_generation,
                    effective_at_ms: timestamp,
                    observed_at_ms,
                    model: state.model.clone(),
                    effort: state.effort.clone(),
                    reconciliation,
                    counts: last,
                    source_offset: line.start_offset,
                });
            }
        }
    }

    if (!state.fork_like || state.counting_boundary_seen)
        && let Some(quota) = parse_quota(
            payload,
            &line_key,
            payload_hash,
            state,
            observed_at_ms,
            timestamp,
        )
    {
        quotas.push(quota);
    }
    Ok(())
}

fn parse_optional_counts(
    value: Option<&Value>,
) -> Option<Result<TokenCounts, crate::domain::CountError>> {
    match value {
        None | Some(Value::Null) => None,
        Some(value) => Some(TokenCounts::from_codex(value)),
    }
}

fn parse_quota(
    payload: &Value,
    line_key: &str,
    payload_hash: &str,
    state: &ParserStateV1,
    observed_at_ms: i64,
    captured_at_ms: Option<i64>,
) -> Option<QuotaSnapshotFact> {
    let limits = payload.get("rate_limits")?.as_object()?;
    let limit_id = limits.get("limit_id").and_then(Value::as_str);
    if limit_id.is_some_and(|id| id != "codex") {
        return None;
    }
    let machine_limit_id = strict_identifier(limit_id.unwrap_or("codex"), 80)?;
    let mut windows = Vec::new();
    for (window_kind, value) in limits {
        if matches!(
            window_kind.as_str(),
            "limit_id" | "limit_name" | "plan_type"
        ) {
            continue;
        }
        let Some(window) = value.as_object() else {
            continue;
        };
        let window_minutes = window
            .get("window_minutes")
            .and_then(Value::as_i64)
            .filter(|value| *value > 0);
        let used_percent_micros = window.get("used_percent").and_then(parse_percent_micros);
        let resets_at_ms = window
            .get("resets_at")
            .and_then(Value::as_i64)
            .filter(|value| *value >= 0)
            .and_then(|seconds| seconds.checked_mul(1_000));
        if window_minutes.is_none() && used_percent_micros.is_none() && resets_at_ms.is_none() {
            continue;
        }
        let safe_kind = strict_identifier(window_kind, 80)?;
        windows.push(QuotaWindowFact {
            window_key: format!("codex/{machine_limit_id}/{safe_kind}"),
            window_kind: safe_kind.clone(),
            label: match safe_kind.as_str() {
                "primary" => "Primary".to_string(),
                "secondary" => "Secondary".to_string(),
                _ => safe_kind,
            },
            window_minutes,
            used_percent_micros,
            resets_at_ms,
        });
    }
    if windows.is_empty() {
        return None;
    }
    windows.sort_by(|left, right| left.window_key.cmp(&right.window_key));
    Some(QuotaSnapshotFact {
        source_record_key: fact_key(line_key, "quota-snapshot"),
        source_payload_hash: payload_hash.to_string(),
        session_id: state.logical_session_id.clone()?,
        provider_limit_id: Some(machine_limit_id),
        captured_at_ms,
        observed_at_ms,
        plan: limits
            .get("plan_type")
            .and_then(Value::as_str)
            .map(|value| sanitize_bounded(value, 80)),
        windows,
    })
}

fn parse_percent_micros(value: &Value) -> Option<i64> {
    let number = value.as_number()?.to_string();
    let (mantissa, exponent) = match number.split_once(['e', 'E']) {
        Some((mantissa, exponent)) => (mantissa, exponent.parse::<i32>().ok()?),
        None => (number.as_str(), 0i32),
    };
    if mantissa.starts_with('-') {
        return None;
    }
    let mantissa = mantissa.strip_prefix('+').unwrap_or(mantissa);
    let (whole, fraction) = mantissa.split_once('.').unwrap_or((mantissa, ""));
    if !whole.chars().all(|character| character.is_ascii_digit())
        || !fraction.chars().all(|character| character.is_ascii_digit())
    {
        return None;
    }
    let digits = format!("{whole}{fraction}");
    let raw = digits.parse::<i128>().ok()?;
    let decimal_places = i32::try_from(fraction.len()).ok()?;
    let power = 6i32.checked_add(exponent)?.checked_sub(decimal_places)?;
    let scaled = if power >= 0 {
        raw.checked_mul(10i128.checked_pow(power as u32)?)?
    } else {
        let divisor = 10i128.checked_pow(power.unsigned_abs())?;
        if raw % divisor != 0 {
            return None;
        }
        raw / divisor
    };
    i64::try_from(scaled)
        .ok()
        .filter(|value| (0..=100_000_000).contains(value))
}

#[allow(clippy::too_many_arguments)]
fn start_tool(
    line: &JsonlLine,
    value: &Value,
    payload: &Value,
    payload_hash: &str,
    context: &KeyContext,
    timestamp: Option<i64>,
    state: &mut ParserStateV1,
    tool_starts: &mut Vec<ToolStartFact>,
) {
    let Some(session_id) = state.logical_session_id.clone() else {
        return;
    };
    let call_key = payload
        .get("call_id")
        .and_then(Value::as_str)
        .and_then(|value| strict_identifier(value, 256))
        .unwrap_or_else(|| format!("offset/{}", line.start_offset));
    let tool_name = bounded_string(payload, &["name", "tool_name", "tool"], 128)
        .unwrap_or_else(|| "unknown".to_string());
    let line_key = line_key(value, line.start_offset, state, context);
    let source_record_key = fact_key(&line_key, "tool-start");
    let Some(input_length) = line.end_offset.checked_sub(line.start_offset) else {
        return;
    };
    let pending = PendingToolState {
        call_key: call_key.clone(),
        source_record_key: source_record_key.clone(),
        tool_name: tool_name.clone(),
        started_at_ms: timestamp,
        turn_key: state.current_turn_key.clone(),
        input_offset: line.start_offset,
        input_length,
        source_file_identity: context.source_file_identity.clone(),
    };
    state.pending_tools.insert(call_key.clone(), pending);
    state.awaiting_assistant = false;
    tool_starts.push(ToolStartFact {
        source_record_key,
        source_payload_hash: payload_hash.to_string(),
        session_id,
        turn_key: state.current_turn_key.clone(),
        call_key,
        tool_name,
        started_at_ms: timestamp,
        input_offset: line.start_offset,
        input_length,
        source_file_identity: context.source_file_identity.clone(),
    });
}

fn finish_tool(
    line: &JsonlLine,
    value: &Value,
    payload: &Value,
    context: &KeyContext,
    timestamp: Option<i64>,
    state: &mut ParserStateV1,
    tool_finishes: &mut Vec<ToolFinishFact>,
) {
    let Some(call_key) = payload
        .get("call_id")
        .and_then(Value::as_str)
        .and_then(|value| strict_identifier(value, 256))
    else {
        return;
    };
    let Some(pending) = state.pending_tools.remove(&call_key) else {
        return;
    };
    let line_key = line_key(value, line.start_offset, state, context);
    let Some(output_length) = line.end_offset.checked_sub(line.start_offset) else {
        return;
    };
    tool_finishes.push(ToolFinishFact {
        start_source_record_key: pending.source_record_key,
        closed_by_record_key: fact_key(&line_key, "tool-finish"),
        ended_at_ms: timestamp,
        output_offset: line.start_offset,
        output_length,
        output_file_identity: context.source_file_identity.clone(),
    });
}

fn outer_type(value: &Value) -> Option<&str> {
    value.get("type").and_then(Value::as_str)
}

fn inner_type(value: &Value) -> Option<&str> {
    value
        .get("payload")
        .and_then(|payload| payload.get("type"))
        .and_then(Value::as_str)
}

fn parse_record_timestamp(value: &Value) -> Option<i64> {
    value
        .get("timestamp")
        .and_then(Value::as_str)
        .and_then(parse_rfc3339_ms)
}

fn parse_rfc3339_ms(value: &str) -> Option<i64> {
    DateTime::parse_from_rfc3339(value)
        .ok()
        .map(|timestamp| timestamp.timestamp_millis())
        .filter(|timestamp| *timestamp >= 0)
}

fn bounded_string(value: &Value, keys: &[&str], max_chars: usize) -> Option<String> {
    keys.iter().find_map(|key| {
        let value = value.get(*key)?.as_str()?;
        let bounded = sanitize_bounded(value, max_chars);
        (!bounded.is_empty()).then_some(bounded)
    })
}

fn increment_quality(state: &mut ParserStateV1) -> Result<(), ParseError> {
    state.data_quality_errors = state
        .data_quality_errors
        .checked_add(1)
        .ok_or(ParseError::CounterOverflow)?;
    Ok(())
}

fn line_key(
    value: &Value,
    source_offset: u64,
    state: &ParserStateV1,
    context: &KeyContext,
) -> String {
    let outer = outer_type(value).unwrap_or("unknown");
    let inner = inner_type(value).unwrap_or("unknown");
    let payload = value.get("payload").unwrap_or(&Value::Null);
    let session = state.logical_session_id.as_deref().unwrap_or("unresolved");
    let timestamp = parse_record_timestamp(value)
        .map(|value| value.to_string())
        .unwrap_or_else(|| "unavailable".to_string());
    let base = [
        context.device_public_id.as_bytes(),
        context.source_root_public_id.as_bytes(),
        context.logical_file_id.as_bytes(),
        session.as_bytes(),
        outer.as_bytes(),
        inner.as_bytes(),
    ];

    if let Some(event_id) = value
        .get("id")
        .or_else(|| payload.get("event_id"))
        .and_then(Value::as_str)
        .and_then(|value| strict_identifier(value, 256))
    {
        return hash_fields(
            "tokenometer/codex-line/event-id/v1",
            base.into_iter().chain([event_id.as_bytes()]),
        );
    }
    if let Some(stable_id) = payload
        .get("call_id")
        .and_then(Value::as_str)
        .and_then(|value| strict_identifier(value, 256))
    {
        return hash_fields(
            "tokenometer/codex-line/stable-id/v1",
            base.into_iter()
                .chain([stable_id.as_bytes(), timestamp.as_bytes()]),
        );
    }
    let offset = source_offset.to_string();
    hash_fields(
        "tokenometer/codex-line/offset/v1",
        base.into_iter()
            .chain([timestamp.as_bytes(), offset.as_bytes()]),
    )
}

fn fact_key(line_key: &str, fact_kind: &str) -> String {
    hash_fields(
        "tokenometer/codex-fact/v1",
        [line_key.as_bytes(), fact_kind.as_bytes()],
    )
}

fn hash_bytes(value: &[u8]) -> String {
    let mut hasher = Sha256::new();
    hasher.update(value);
    hex(&hasher.finalize())
}

fn hash_fields<'a>(domain: &str, fields: impl IntoIterator<Item = &'a [u8]>) -> String {
    let mut hasher = Sha256::new();
    write_hash_field(&mut hasher, domain.as_bytes());
    for field in fields {
        write_hash_field(&mut hasher, field);
    }
    hex(&hasher.finalize())
}

fn write_hash_field(hasher: &mut Sha256, value: &[u8]) {
    hasher.update((value.len() as u64).to_be_bytes());
    hasher.update(value);
}

fn hex(value: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut output = String::with_capacity(value.len() * 2);
    for byte in value {
        output.push(HEX[(byte >> 4) as usize] as char);
        output.push(HEX[(byte & 0x0f) as usize] as char);
    }
    output
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::collector::jsonl::{JsonlBatch, JsonlLine};

    fn context() -> KeyContext {
        KeyContext {
            device_public_id: "device-synthetic".to_string(),
            source_root_public_id: "source-synthetic".to_string(),
            logical_file_id: "rollout/synthetic".to_string(),
            source_file_identity: "windows:synthetic:1".to_string(),
        }
    }

    fn batch(lines: &[&str]) -> JsonlBatch {
        let mut offset = 0u64;
        let lines = lines
            .iter()
            .map(|value| {
                let start = offset;
                offset += value.len() as u64 + 1;
                JsonlLine {
                    start_offset: start,
                    end_offset: offset,
                    bytes: value.as_bytes().to_vec(),
                    terminated_by_newline: true,
                }
            })
            .collect();
        JsonlBatch {
            lines,
            committed_through: offset,
            read_through: offset,
            reached_eof: true,
            incomplete_tail: false,
            oversized_records: 0,
            discarding_oversized_line: false,
        }
    }

    #[test]
    fn cumulative_and_turn_accounts_reconcile_without_double_counting() {
        let parsed = parse_batch(
            batch(&[
                r#"{"type":"session_meta","timestamp":"2026-08-08T00:00:00Z","payload":{"id":"session-a","cwd":"C:/synthetic/project"}}"#,
                r#"{"type":"turn_context","timestamp":"2026-08-08T00:00:01Z","payload":{"model":"gpt-synthetic","effort":"high"}}"#,
                r#"{"type":"event_msg","timestamp":"2026-08-08T00:00:02Z","payload":{"type":"user_message","message":"PRIVATE_PROMPT_SENTINEL"}}"#,
                r#"{"type":"event_msg","timestamp":"2026-08-08T00:00:03Z","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":100,"cached_input_tokens":80,"output_tokens":20,"reasoning_output_tokens":5,"total_tokens":120},"last_token_usage":{"input_tokens":100,"cached_input_tokens":80,"output_tokens":20,"reasoning_output_tokens":5,"total_tokens":120}}}}"#,
            ]),
            ParserStateV1::default(),
            &context(),
            1_786_147_200_000,
        )
        .unwrap();

        assert_eq!(parsed.usage.len(), 2);
        let canonical = parsed
            .usage
            .iter()
            .find(|fact| fact.metric_scope == MetricScope::SessionCumulativeDelta)
            .unwrap();
        assert_eq!(canonical.counts.normalized_total().unwrap(), 120);
        assert_eq!(canonical.reconciliation, Reconciliation::Consistent);
        assert_eq!(canonical.model.as_deref(), Some("gpt-synthetic"));
        assert_eq!(parsed.turns.len(), 1);
        let serialized = serde_json::to_string(&parsed.next_state).unwrap();
        assert!(!serialized.contains("PRIVATE_PROMPT_SENTINEL"));
    }

    #[test]
    fn mismatch_and_cumulative_reset_are_explicit_generations() {
        let first = parse_batch(
            batch(&[
                r#"{"type":"session_meta","payload":{"id":"session-a"}}"#,
                r#"{"type":"event_msg","timestamp":"2026-08-08T00:00:01Z","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":100,"output_tokens":20},"last_token_usage":{"input_tokens":90,"output_tokens":20}}}}"#,
            ]),
            ParserStateV1::default(),
            &context(),
            10,
        )
        .unwrap();
        assert!(
            first
                .usage
                .iter()
                .all(|fact| fact.reconciliation == Reconciliation::Mismatch)
        );

        let reset = parse_batch(
            batch(&[r#"{"type":"event_msg","timestamp":"2026-08-08T00:00:02Z","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":5,"output_tokens":2},"last_token_usage":{"input_tokens":5,"output_tokens":2}}}}"#]),
            first.next_state,
            &context(),
            11,
        )
        .unwrap();
        assert_eq!(reset.next_state.cumulative_generation, 1);
        assert!(
            reset
                .usage
                .iter()
                .all(|fact| fact.reconciliation == Reconciliation::Reset)
        );
    }

    #[test]
    fn malformed_newline_record_advances_but_invalid_eof_does_not() {
        let lines = vec![
            JsonlLine {
                start_offset: 0,
                end_offset: 5,
                bytes: b"nope".to_vec(),
                terminated_by_newline: true,
            },
            JsonlLine {
                start_offset: 5,
                end_offset: 9,
                bytes: b"{bad".to_vec(),
                terminated_by_newline: false,
            },
        ];
        let parsed = parse_batch(
            JsonlBatch {
                lines,
                committed_through: 5,
                read_through: 9,
                reached_eof: true,
                incomplete_tail: false,
                oversized_records: 0,
                discarding_oversized_line: false,
            },
            ParserStateV1::default(),
            &context(),
            0,
        )
        .unwrap();
        assert_eq!(parsed.committed_offset, 5);
        assert_eq!(parsed.next_state.malformed_records, 1);
    }

    #[test]
    fn pending_tool_state_survives_batches_without_persisting_arguments_or_output() {
        let started = parse_batch(
            batch(&[
                r#"{"type":"session_meta","payload":{"id":"session-a"}}"#,
                r#"{"type":"response_item","timestamp":"2026-08-08T00:00:01Z","payload":{"type":"function_call","name":"exec_command","call_id":"call-1","arguments":"SECRET_ARGUMENT_SENTINEL"}}"#,
            ]),
            ParserStateV1::default(),
            &context(),
            1,
        )
        .unwrap();
        assert_eq!(started.tool_starts.len(), 1);
        assert_eq!(started.next_state.status(), TranscriptStatus::Executing);
        let state_json = serde_json::to_string(&started.next_state).unwrap();
        assert!(!state_json.contains("SECRET_ARGUMENT_SENTINEL"));

        let finished = parse_batch(
            batch(&[r#"{"type":"response_item","timestamp":"2026-08-08T00:00:04Z","payload":{"type":"function_call_output","call_id":"call-1","output":"SECRET_OUTPUT_SENTINEL"}}"#]),
            started.next_state,
            &context(),
            2,
        )
        .unwrap();
        assert_eq!(finished.tool_finishes.len(), 1);
        assert_eq!(finished.next_state.status(), TranscriptStatus::Unknown);
        let state_json = serde_json::to_string(&finished.next_state).unwrap();
        assert!(!state_json.contains("SECRET_OUTPUT_SENTINEL"));
    }

    #[test]
    fn transcript_quota_uses_fixed_point_and_arbitrary_rows() {
        let parsed = parse_batch(
            batch(&[
                r#"{"type":"session_meta","payload":{"id":"session-a"}}"#,
                r#"{"type":"event_msg","timestamp":"2026-08-08T00:00:01Z","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":1,"output_tokens":1}},"rate_limits":{"limit_id":"codex","primary":{"used_percent":9.125,"window_minutes":300,"resets_at":1786147200},"secondary":{"used_percent":14,"window_minutes":10080,"resets_at":1786752000},"plan_type":"plus"}}}"#,
            ]),
            ParserStateV1::default(),
            &context(),
            1,
        )
        .unwrap();
        assert_eq!(parsed.quotas.len(), 1);
        assert_eq!(parsed.quotas[0].windows.len(), 2);
        assert_eq!(
            parsed.quotas[0].windows[0].used_percent_micros,
            Some(9_125_000)
        );
        assert!(parsed.quotas[0].windows[0].resets_at_ms.unwrap() > 1_000_000_000_000);
    }

    #[test]
    fn fork_history_is_baselined_before_task_started() {
        let parsed = parse_batch(
            batch(&[
                r#"{"type":"session_meta","payload":{"id":"child","originator":"codex-subagent"}}"#,
                r#"{"type":"event_msg","timestamp":"2026-08-08T00:00:01Z","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":100,"output_tokens":10}}}}"#,
                r#"{"type":"event_msg","timestamp":"2026-08-08T00:00:02Z","payload":{"type":"task_started"}}"#,
                r#"{"type":"event_msg","timestamp":"2026-08-08T00:00:03Z","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":120,"output_tokens":15},"last_token_usage":{"input_tokens":20,"output_tokens":5}}}}"#,
            ]),
            ParserStateV1::default(),
            &context(),
            1,
        )
        .unwrap();
        let canonical: Vec<_> = parsed
            .usage
            .iter()
            .filter(|fact| fact.metric_scope == MetricScope::SessionCumulativeDelta)
            .collect();
        assert_eq!(canonical.len(), 1);
        assert_eq!(canonical[0].counts.normalized_total().unwrap(), 25);
    }
}
