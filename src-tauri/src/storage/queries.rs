use rusqlite::Connection;

use crate::domain::SourceHealthStatus;

use super::{Storage, StorageError, source_health_from_db};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SourceRootHealth {
    pub source_id: String,
    pub health: SourceHealthStatus,
    pub last_attempt_at_ms: Option<i64>,
    pub last_success_at_ms: Option<i64>,
    pub last_error_code: Option<String>,
    pub affected_file_count: u64,
    pub malformed_records: u64,
    pub oversized_records: u64,
    pub data_quality_errors: u64,
    pub updated_at_ms: i64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BootstrapStorageData {
    pub device_id: String,
    pub codex_health: SourceHealthStatus,
    pub last_error_code: Option<String>,
    pub source_root_count: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct CanonicalUsageTotals {
    pub event_count: u64,
    pub input_total: i64,
    pub cache_read_input: i64,
    pub cache_write_input: i64,
    pub output_total: i64,
    pub reasoning_output: i64,
    pub normalized_total: i64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StorageTable {
    SchemaMigrations,
    AppSettings,
    Devices,
    ProviderAccounts,
    SourceRoots,
    SourceFiles,
    Sessions,
    Turns,
    UsageEvents,
    ToolCalls,
    QuotaSnapshots,
    QuotaWindowValues,
}

impl StorageTable {
    fn sql_name(self) -> &'static str {
        match self {
            Self::SchemaMigrations => "schema_migrations",
            Self::AppSettings => "app_settings",
            Self::Devices => "devices",
            Self::ProviderAccounts => "provider_accounts",
            Self::SourceRoots => "source_roots",
            Self::SourceFiles => "source_files",
            Self::Sessions => "sessions",
            Self::Turns => "turns",
            Self::UsageEvents => "usage_events",
            Self::ToolCalls => "tool_calls",
            Self::QuotaSnapshots => "quota_snapshots",
            Self::QuotaWindowValues => "quota_window_values",
        }
    }
}

impl Storage {
    pub fn list_source_root_health(&self) -> Result<Vec<SourceRootHealth>, StorageError> {
        let connection = self.lock_connection()?;
        load_source_root_health(&connection)
    }

    pub fn bootstrap_data(&self) -> Result<BootstrapStorageData, StorageError> {
        let connection = self.lock_connection()?;
        let device_id: String = connection.query_row(
            "SELECT public_id FROM devices WHERE kind = 'windows' ORDER BY id LIMIT 1",
            [],
            |row| row.get(0),
        )?;
        let roots = load_source_root_health(&connection)?;
        let codex_health = aggregate_health(&roots);
        let last_error_code = roots
            .iter()
            .filter(|root| root.last_error_code.is_some())
            .max_by_key(|root| root.updated_at_ms)
            .and_then(|root| root.last_error_code.clone());
        let source_root_count =
            u64::try_from(roots.len()).map_err(|_| StorageError::ArithmeticOverflow)?;

        Ok(BootstrapStorageData {
            device_id,
            codex_health,
            last_error_code,
            source_root_count,
        })
    }

    pub fn canonical_usage_totals(&self) -> Result<CanonicalUsageTotals, StorageError> {
        let connection = self.lock_connection()?;
        let mut statement = connection.prepare(
            r#"
            SELECT
                input_total, cache_read_input, cache_write_input,
                output_total, reasoning_output
            FROM usage_events
            WHERE metric_scope = 'sessionCumulativeDelta'
            ORDER BY id
            "#,
        )?;
        let rows = statement.query_map([], |row| {
            Ok((
                row.get::<_, i64>(0)?,
                row.get::<_, Option<i64>>(1)?,
                row.get::<_, Option<i64>>(2)?,
                row.get::<_, i64>(3)?,
                row.get::<_, Option<i64>>(4)?,
            ))
        })?;
        let mut totals = CanonicalUsageTotals::default();
        for row in rows {
            let (input, cache_read, cache_write, output, reasoning) = row?;
            for (value, field) in [
                (input, "usage_events.input_total"),
                (cache_read.unwrap_or(0), "usage_events.cache_read_input"),
                (cache_write.unwrap_or(0), "usage_events.cache_write_input"),
                (output, "usage_events.output_total"),
                (reasoning.unwrap_or(0), "usage_events.reasoning_output"),
            ] {
                if value < 0 {
                    return Err(StorageError::InvalidDatabaseValue { field });
                }
            }
            totals.event_count = totals
                .event_count
                .checked_add(1)
                .ok_or(StorageError::ArithmeticOverflow)?;
            totals.input_total = totals
                .input_total
                .checked_add(input)
                .ok_or(StorageError::ArithmeticOverflow)?;
            totals.cache_read_input = totals
                .cache_read_input
                .checked_add(cache_read.unwrap_or(0))
                .ok_or(StorageError::ArithmeticOverflow)?;
            totals.cache_write_input = totals
                .cache_write_input
                .checked_add(cache_write.unwrap_or(0))
                .ok_or(StorageError::ArithmeticOverflow)?;
            totals.output_total = totals
                .output_total
                .checked_add(output)
                .ok_or(StorageError::ArithmeticOverflow)?;
            totals.reasoning_output = totals
                .reasoning_output
                .checked_add(reasoning.unwrap_or(0))
                .ok_or(StorageError::ArithmeticOverflow)?;
        }
        totals.normalized_total = totals
            .input_total
            .checked_add(totals.output_total)
            .ok_or(StorageError::ArithmeticOverflow)?;
        Ok(totals)
    }

    pub fn row_count(&self, table: StorageTable) -> Result<u64, StorageError> {
        let connection = self.lock_connection()?;
        let sql = format!("SELECT COUNT(*) FROM {}", table.sql_name());
        let count: i64 = connection.query_row(&sql, [], |row| row.get(0))?;
        non_negative(count, "row count")
    }
}

fn load_source_root_health(connection: &Connection) -> Result<Vec<SourceRootHealth>, StorageError> {
    let mut statement = connection.prepare(
        r#"
        SELECT
            public_id, health, last_attempt_at_ms, last_success_at_ms,
            last_error_code, affected_file_count, malformed_records,
            oversized_records, data_quality_errors, updated_at_ms
        FROM source_roots
        WHERE source_kind = 'codex'
        ORDER BY id
        "#,
    )?;
    let raw = statement
        .query_map([], |row| {
            Ok(RawSourceRootHealth {
                source_id: row.get(0)?,
                health: row.get(1)?,
                last_attempt_at_ms: row.get(2)?,
                last_success_at_ms: row.get(3)?,
                last_error_code: row.get(4)?,
                affected_file_count: row.get(5)?,
                malformed_records: row.get(6)?,
                oversized_records: row.get(7)?,
                data_quality_errors: row.get(8)?,
                updated_at_ms: row.get(9)?,
            })
        })?
        .collect::<Result<Vec<_>, _>>()?;
    raw.into_iter().map(TryInto::try_into).collect()
}

struct RawSourceRootHealth {
    source_id: String,
    health: String,
    last_attempt_at_ms: Option<i64>,
    last_success_at_ms: Option<i64>,
    last_error_code: Option<String>,
    affected_file_count: i64,
    malformed_records: i64,
    oversized_records: i64,
    data_quality_errors: i64,
    updated_at_ms: i64,
}

impl TryFrom<RawSourceRootHealth> for SourceRootHealth {
    type Error = StorageError;

    fn try_from(raw: RawSourceRootHealth) -> Result<Self, Self::Error> {
        Ok(Self {
            source_id: raw.source_id,
            health: source_health_from_db(&raw.health)?,
            last_attempt_at_ms: raw.last_attempt_at_ms,
            last_success_at_ms: raw.last_success_at_ms,
            last_error_code: raw.last_error_code,
            affected_file_count: non_negative(
                raw.affected_file_count,
                "source_roots.affected_file_count",
            )?,
            malformed_records: non_negative(
                raw.malformed_records,
                "source_roots.malformed_records",
            )?,
            oversized_records: non_negative(
                raw.oversized_records,
                "source_roots.oversized_records",
            )?,
            data_quality_errors: non_negative(
                raw.data_quality_errors,
                "source_roots.data_quality_errors",
            )?,
            updated_at_ms: raw.updated_at_ms,
        })
    }
}

fn aggregate_health(roots: &[SourceRootHealth]) -> SourceHealthStatus {
    if roots.is_empty()
        || roots
            .iter()
            .all(|root| root.health == SourceHealthStatus::Never)
    {
        return SourceHealthStatus::Never;
    }
    if roots
        .iter()
        .all(|root| root.health == SourceHealthStatus::Healthy)
    {
        return SourceHealthStatus::Healthy;
    }
    if roots
        .iter()
        .any(|root| root.health == SourceHealthStatus::Partial)
        || roots
            .iter()
            .any(|root| root.health == SourceHealthStatus::Healthy)
    {
        return SourceHealthStatus::Partial;
    }
    if roots
        .iter()
        .any(|root| root.health == SourceHealthStatus::Stale)
    {
        return SourceHealthStatus::Stale;
    }
    SourceHealthStatus::Failed
}

fn non_negative(value: i64, field: &'static str) -> Result<u64, StorageError> {
    u64::try_from(value).map_err(|_| StorageError::InvalidDatabaseValue { field })
}

#[cfg(test)]
mod tests {
    use std::path::Path;

    use rusqlite::params;
    use serde_json::json;

    use super::{StorageTable, *};
    use crate::storage::{
        CursorExpectation, CursorUpsertResult, SourceFileCursorWrite, SourceFileLocation,
        SourceRootHealthUpdate,
    };

    #[test]
    fn health_and_bootstrap_queries_never_return_a_canonical_path() {
        let storage = Storage::open_in_memory().unwrap();
        let root = storage
            .ensure_codex_source_root(Path::new("C:\\private\\.codex"), None, true, 10)
            .unwrap();
        storage
            .update_source_root_health(
                root.id,
                &SourceRootHealthUpdate {
                    health: SourceHealthStatus::Stale,
                    last_attempt_at_ms: Some(20),
                    last_success_at_ms: Some(10),
                    last_error_code: Some("SOURCE_UNREADABLE".to_string()),
                    affected_file_count: 1,
                    malformed_records: 2,
                    oversized_records: 3,
                    data_quality_errors: 4,
                    updated_at_ms: 20,
                },
            )
            .unwrap();

        let health = storage.list_source_root_health().unwrap();
        assert_eq!(health.len(), 1);
        assert_eq!(health[0].source_id, root.public_id);
        assert_eq!(health[0].health, SourceHealthStatus::Stale);
        assert_eq!(
            health[0].last_error_code.as_deref(),
            Some("SOURCE_UNREADABLE")
        );
        let bootstrap = storage.bootstrap_data().unwrap();
        assert_eq!(bootstrap.codex_health, SourceHealthStatus::Stale);
        assert_eq!(bootstrap.source_root_count, 1);
    }

    #[test]
    fn canonical_totals_exclude_turn_reported_facts() {
        let storage = Storage::open_in_memory().unwrap();
        let root = storage
            .ensure_codex_source_root(Path::new("C:\\synthetic\\.codex"), None, true, 10)
            .unwrap();
        let cursor_write = SourceFileCursorWrite {
            source_root_id: root.id,
            logical_file_id: "rollout-1".to_string(),
            logical_session_id: Some("session-1".to_string()),
            canonical_path: Path::new("C:\\synthetic\\rollout-1.jsonl").to_path_buf(),
            location_kind: SourceFileLocation::Active,
            stable_file_identity: None,
            identity_degraded: true,
            cursor_generation: 0,
            committed_offset: 1,
            last_known_size: 1,
            last_modified_at_ms: Some(10),
            parser_state_version: 1,
            parser_state: json!({}),
            health: SourceHealthStatus::Healthy,
            last_attempt_at_ms: Some(10),
            last_success_at_ms: Some(10),
            last_checked_at_ms: Some(10),
            last_error_code: None,
            malformed_records: 0,
            oversized_records: 0,
            data_quality_errors: 0,
            expected: CursorExpectation::Missing,
        };
        let CursorUpsertResult::Applied(cursor) =
            storage.upsert_source_file_cursor(&cursor_write).unwrap()
        else {
            panic!("cursor should be inserted");
        };
        let device = storage.device_identity().unwrap();
        {
            let connection = storage.lock_connection().unwrap();
            connection
                .execute(
                    r#"
                    INSERT INTO sessions(
                        source_root_id, device_id, provider_key, account_id, session_id
                    ) VALUES (?1, ?2, 'codex', 'current', 'session-1')
                    "#,
                    params![root.id, device.id],
                )
                .unwrap();
            let session_id = connection.last_insert_rowid();
            for (record_key, scope, input, output) in [
                ("record-canonical", "sessionCumulativeDelta", 10_i64, 5_i64),
                ("record-turn", "turnReported", 1000_i64, 500_i64),
            ] {
                connection
                    .execute(
                        r#"
                        INSERT INTO usage_events(
                            source_record_key, source_payload_hash, source_file_id,
                            session_id, device_id, provider_key, account_id,
                            metric_scope, cumulative_generation, observed_at_ms,
                            measurement_kind, reconciliation, input_total,
                            output_total, invalid_breakdown, source_offset
                        ) VALUES (
                            ?1, 'hash', ?2, ?3, ?4, 'codex', 'current',
                            ?5, 0, 10, 'exact', 'consistent', ?6, ?7, 0, 0
                        )
                        "#,
                        params![
                            record_key, cursor.id, session_id, device.id, scope, input, output
                        ],
                    )
                    .unwrap();
            }
        }

        let totals = storage.canonical_usage_totals().unwrap();
        assert_eq!(totals.event_count, 1);
        assert_eq!(totals.input_total, 10);
        assert_eq!(totals.output_total, 5);
        assert_eq!(totals.normalized_total, 15);
        assert_eq!(storage.row_count(StorageTable::UsageEvents).unwrap(), 2);
    }
}
