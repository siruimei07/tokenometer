use serde::{Deserialize, Serialize};
use serde_json::Value;
use thiserror::Error;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum AgentProvider {
    Codex,
    ClaudeCode,
    OpenCode,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum ProviderCapability {
    Usage,
    Sessions,
    LiveProcess,
    Context,
    Quota,
    Git,
    Ports,
    Mcp,
    Subagents,
    ProviderMemory,
    NetworkQuota,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum MeasurementKind {
    Exact,
    Estimated,
    Heuristic,
    Unavailable,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "state", rename_all = "camelCase")]
pub enum Availability<T> {
    Available {
        value: T,
        #[serde(rename = "measuredAt")]
        measured_at: i64,
    },
    Stale {
        #[serde(rename = "lastGood")]
        last_good: T,
        #[serde(rename = "measuredAt")]
        measured_at: i64,
        #[serde(rename = "staleSince")]
        stale_since: i64,
        #[serde(skip_serializing_if = "Option::is_none")]
        reason: Option<String>,
    },
    Unsupported {
        #[serde(skip_serializing_if = "Option::is_none")]
        reason: Option<String>,
    },
    NotConfigured {
        #[serde(skip_serializing_if = "Option::is_none")]
        reason: Option<String>,
    },
    NotObserved {
        #[serde(skip_serializing_if = "Option::is_none")]
        reason: Option<String>,
    },
    PermissionDenied {
        #[serde(skip_serializing_if = "Option::is_none")]
        reason: Option<String>,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum SourceHealthStatus {
    Never,
    Healthy,
    Partial,
    Stale,
    Failed,
}

impl SourceHealthStatus {
    pub fn as_db_str(self) -> &'static str {
        match self {
            Self::Never => "never",
            Self::Healthy => "healthy",
            Self::Partial => "partial",
            Self::Stale => "stale",
            Self::Failed => "failed",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum Reconciliation {
    Consistent,
    Mismatch,
    Reset,
    Missing,
}

impl Reconciliation {
    pub fn as_db_str(self) -> &'static str {
        match self {
            Self::Consistent => "consistent",
            Self::Mismatch => "mismatch",
            Self::Reset => "reset",
            Self::Missing => "missing",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CountComparison {
    Equal,
    Different,
    Incomplete,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TokenCounts {
    pub input_total: i64,
    pub cache_read_input: Option<i64>,
    pub cache_write_input: Option<i64>,
    pub output_total: i64,
    pub reasoning_output: Option<i64>,
    pub reported_total: Option<i64>,
    pub invalid_breakdown: bool,
}

#[derive(Debug, Error, Clone, PartialEq, Eq)]
pub enum CountError {
    #[error("required token field is missing")]
    MissingRequired,
    #[error("token field must be a signed 64-bit integer")]
    InvalidInteger,
    #[error("token count cannot be negative")]
    Negative,
    #[error("token count arithmetic overflowed")]
    Overflow,
    #[error("conflicting cache token aliases were reported")]
    ConflictingAliases,
    #[error("cumulative token counters decreased")]
    CounterDecreased,
}

impl TokenCounts {
    pub fn from_codex(value: &Value) -> Result<Self, CountError> {
        let input_total = required_non_negative(value, "input_tokens")?;
        let output_total = required_non_negative(value, "output_tokens")?;
        let cache_read_input = optional_non_negative_alias(
            value,
            &["cached_input_tokens", "cache_read_input_tokens"],
        )?;
        let cache_write_input = optional_non_negative_alias(value, &["cache_write_input_tokens"])?;
        let reasoning_output = optional_non_negative_alias(value, &["reasoning_output_tokens"])?;
        let reported_total = optional_non_negative_alias(value, &["total_tokens"])?;

        input_total
            .checked_add(output_total)
            .ok_or(CountError::Overflow)?;

        let cache_sum = cache_read_input
            .unwrap_or(0)
            .checked_add(cache_write_input.unwrap_or(0))
            .ok_or(CountError::Overflow)?;
        let invalid_breakdown = cache_sum > input_total
            || reasoning_output
                .map(|reasoning| reasoning > output_total)
                .unwrap_or(false);

        Ok(Self {
            input_total,
            cache_read_input,
            cache_write_input,
            output_total,
            reasoning_output,
            reported_total,
            invalid_breakdown,
        })
    }

    pub fn normalized_total(&self) -> Result<i64, CountError> {
        self.input_total
            .checked_add(self.output_total)
            .ok_or(CountError::Overflow)
    }

    pub fn is_zero(&self) -> bool {
        self.input_total == 0 && self.output_total == 0
    }

    pub fn decreased_from(&self, previous: &Self) -> bool {
        self.input_total < previous.input_total
            || self.output_total < previous.output_total
            || optional_decreased(self.cache_read_input, previous.cache_read_input)
            || optional_decreased(self.cache_write_input, previous.cache_write_input)
            || optional_decreased(self.reasoning_output, previous.reasoning_output)
            || optional_decreased(self.reported_total, previous.reported_total)
    }

    pub fn delta_from(&self, previous: Option<&Self>) -> Result<Self, CountError> {
        let Some(previous) = previous else {
            return Ok(self.clone());
        };
        if self.decreased_from(previous) {
            return Err(CountError::CounterDecreased);
        }

        let input_total = self
            .input_total
            .checked_sub(previous.input_total)
            .ok_or(CountError::Overflow)?;
        let output_total = self
            .output_total
            .checked_sub(previous.output_total)
            .ok_or(CountError::Overflow)?;
        let cache_read_input = optional_delta(self.cache_read_input, previous.cache_read_input)?;
        let cache_write_input = optional_delta(self.cache_write_input, previous.cache_write_input)?;
        let reasoning_output = optional_delta(self.reasoning_output, previous.reasoning_output)?;
        let reported_total = optional_delta(self.reported_total, previous.reported_total)?;

        input_total
            .checked_add(output_total)
            .ok_or(CountError::Overflow)?;
        let cache_sum = cache_read_input
            .unwrap_or(0)
            .checked_add(cache_write_input.unwrap_or(0))
            .ok_or(CountError::Overflow)?;
        let invalid_breakdown = cache_sum > input_total
            || reasoning_output
                .map(|reasoning| reasoning > output_total)
                .unwrap_or(false);

        Ok(Self {
            input_total,
            cache_read_input,
            cache_write_input,
            output_total,
            reasoning_output,
            reported_total,
            invalid_breakdown,
        })
    }

    pub fn compare_components(&self, other: &Self) -> CountComparison {
        if self.input_total != other.input_total || self.output_total != other.output_total {
            return CountComparison::Different;
        }

        let optional = [
            (self.cache_read_input, other.cache_read_input),
            (self.cache_write_input, other.cache_write_input),
            (self.reasoning_output, other.reasoning_output),
            (self.reported_total, other.reported_total),
        ];
        if optional
            .iter()
            .any(|(left, right)| left.is_some() && right.is_some() && left != right)
        {
            CountComparison::Different
        } else if optional
            .iter()
            .any(|(left, right)| left.is_some() != right.is_some())
        {
            CountComparison::Incomplete
        } else {
            CountComparison::Equal
        }
    }
}

fn required_non_negative(value: &Value, key: &str) -> Result<i64, CountError> {
    let raw = value.get(key).ok_or(CountError::MissingRequired)?;
    parse_non_negative(raw)
}

fn optional_non_negative_alias(value: &Value, keys: &[&str]) -> Result<Option<i64>, CountError> {
    let mut result = None;
    for key in keys {
        let Some(raw) = value.get(*key) else {
            continue;
        };
        if raw.is_null() {
            continue;
        }
        let parsed = parse_non_negative(raw)?;
        if result.is_some_and(|existing| existing != parsed) {
            return Err(CountError::ConflictingAliases);
        }
        result = Some(parsed);
    }
    Ok(result)
}

fn parse_non_negative(value: &Value) -> Result<i64, CountError> {
    let parsed = value.as_i64().ok_or(CountError::InvalidInteger)?;
    if parsed < 0 {
        return Err(CountError::Negative);
    }
    Ok(parsed)
}

fn optional_decreased(current: Option<i64>, previous: Option<i64>) -> bool {
    matches!((current, previous), (Some(current), Some(previous)) if current < previous)
}

fn optional_delta(current: Option<i64>, previous: Option<i64>) -> Result<Option<i64>, CountError> {
    match (current, previous) {
        (None, _) => Ok(None),
        // Once a cumulative baseline exists, a newly appearing optional subset has
        // no trustworthy prior value. Keep it unavailable instead of inventing a
        // delta from zero.
        (Some(_), None) => Ok(None),
        (Some(current), Some(previous)) => current
            .checked_sub(previous)
            .filter(|delta| *delta >= 0)
            .map(Some)
            .ok_or(CountError::CounterDecreased),
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum RuntimeKind {
    Usage,
    Live,
    Limits,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum RuntimeStatus {
    NotStarted,
    Starting,
    Healthy,
    Degraded,
    Stopped,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RuntimeHealthView {
    pub runtime: RuntimeKind,
    pub status: RuntimeStatus,
    pub updated_at: i64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub message: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ProviderCapabilityView {
    pub provider: AgentProvider,
    pub capabilities: Vec<ProviderCapability>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ReportingTimeZoneView {
    pub id: String,
    pub display_name: String,
    pub source: ReportingTimeZoneSource,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum ReportingTimeZoneSource {
    WindowsSystem,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct BootstrapView {
    pub history_revision: u64,
    pub live_revision: u64,
    pub device_id: String,
    pub reporting_time_zone: ReportingTimeZoneView,
    pub implemented_capabilities: Vec<ProviderCapabilityView>,
    pub runtime_health: Vec<RuntimeHealthView>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum RefreshScope {
    Codex,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AcceptedRevision {
    pub history_revision: u64,
    pub accepted: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum HistoryDomain {
    Dashboard,
    Usage,
    Sessions,
    Trends,
    Sources,
    Quotas,
    Accounts,
    Cost,
    Devices,
    Settings,
    Exports,
    Sync,
    Alerts,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum LiveDomain {
    Host,
    LiveSessions,
    Processes,
    Ports,
    Mcp,
    Alerts,
    Compact,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct HistoryRevisionEvent {
    pub history_revision: u64,
    pub domains: Vec<HistoryDomain>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LiveRevisionEvent {
    pub live_revision: u64,
    pub domains: Vec<LiveDomain>,
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::{CountComparison, CountError, TokenCounts};

    #[test]
    fn codex_counts_preserve_subsets_without_double_counting() {
        let counts = TokenCounts::from_codex(&json!({
            "input_tokens": 100,
            "cached_input_tokens": 80,
            "output_tokens": 20,
            "reasoning_output_tokens": 5,
            "total_tokens": 120
        }))
        .unwrap();

        assert_eq!(counts.normalized_total().unwrap(), 120);
        assert_eq!(counts.cache_read_input, Some(80));
        assert_eq!(counts.reasoning_output, Some(5));
        assert!(!counts.invalid_breakdown);
    }

    #[test]
    fn invalid_subsets_are_flagged_without_hiding_source_totals() {
        let counts = TokenCounts::from_codex(&json!({
            "input_tokens": 10,
            "cached_input_tokens": 11,
            "output_tokens": 3,
            "reasoning_output_tokens": 4
        }))
        .unwrap();

        assert_eq!(counts.normalized_total().unwrap(), 13);
        assert!(counts.invalid_breakdown);
    }

    #[test]
    fn negative_and_conflicting_aliases_are_rejected() {
        assert_eq!(
            TokenCounts::from_codex(&json!({"input_tokens": -1, "output_tokens": 0})).unwrap_err(),
            CountError::Negative
        );
        assert_eq!(
            TokenCounts::from_codex(&json!({
                "input_tokens": 2,
                "output_tokens": 1,
                "cached_input_tokens": 1,
                "cache_read_input_tokens": 2
            }))
            .unwrap_err(),
            CountError::ConflictingAliases
        );
    }

    #[test]
    fn cumulative_delta_is_checked_and_component_aware() {
        let first = TokenCounts::from_codex(&json!({
            "input_tokens": 100,
            "cached_input_tokens": 30,
            "output_tokens": 20,
            "reasoning_output_tokens": 4
        }))
        .unwrap();
        let second = TokenCounts::from_codex(&json!({
            "input_tokens": 150,
            "cached_input_tokens": 40,
            "output_tokens": 35,
            "reasoning_output_tokens": 9
        }))
        .unwrap();
        let delta = second.delta_from(Some(&first)).unwrap();

        assert_eq!(delta.input_total, 50);
        assert_eq!(delta.cache_read_input, Some(10));
        assert_eq!(delta.output_total, 15);
        assert_eq!(delta.reasoning_output, Some(5));
        assert_eq!(delta.normalized_total().unwrap(), 65);
        assert_eq!(delta.compare_components(&delta), CountComparison::Equal);
        assert_eq!(
            first.delta_from(Some(&second)).unwrap_err(),
            CountError::CounterDecreased
        );
    }

    #[test]
    fn newly_observed_optional_subset_does_not_invent_a_delta_from_zero() {
        let first = TokenCounts::from_codex(&json!({
            "input_tokens": 100,
            "output_tokens": 20
        }))
        .unwrap();
        let second = TokenCounts::from_codex(&json!({
            "input_tokens": 150,
            "cached_input_tokens": 40,
            "output_tokens": 35,
            "reasoning_output_tokens": 9
        }))
        .unwrap();

        let delta = second.delta_from(Some(&first)).unwrap();
        assert_eq!(delta.input_total, 50);
        assert_eq!(delta.output_total, 15);
        assert_eq!(delta.cache_read_input, None);
        assert_eq!(delta.reasoning_output, None);
    }
}
