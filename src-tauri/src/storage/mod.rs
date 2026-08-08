mod queries;

pub use queries::{BootstrapStorageData, CanonicalUsageTotals, SourceRootHealth, StorageTable};

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, MutexGuard};
use std::time::Duration;

use chrono::Utc;
use rusqlite::{Connection, OptionalExtension, TransactionBehavior, params};
use serde_json::Value;
use sha2::{Digest, Sha256};
use thiserror::Error;

use crate::{
    collector::codex::{
        MetricScope, PARSER_STATE_VERSION, ParsedBatch, QuotaSnapshotFact, SessionSnapshotFact,
        ToolFinishFact, ToolStartFact, TurnFact, UsageFact,
    },
    domain::SourceHealthStatus,
};

const BUSY_TIMEOUT: Duration = Duration::from_secs(5);
const MAX_ERROR_CODE_LEN: usize = 128;
const MIGRATION_TABLE_SQL: &str = r#"
CREATE TABLE IF NOT EXISTS schema_migrations (
    version INTEGER PRIMARY KEY CHECK (version > 0),
    name TEXT NOT NULL UNIQUE,
    checksum_sha256 TEXT NOT NULL,
    applied_at_ms INTEGER NOT NULL CHECK (applied_at_ms >= 0)
);
"#;

const MIGRATIONS: &[Migration] = &[Migration {
    version: 1,
    name: "0001_initial.sql",
    sql: include_str!("../../migrations/0001_initial.sql"),
}];

#[derive(Debug, Error)]
pub enum StorageError {
    #[error("SQLite operation failed")]
    Sqlite(#[from] rusqlite::Error),
    #[error("stored JSON is invalid")]
    Json(#[from] serde_json::Error),
    #[error("storage connection lock is poisoned")]
    LockPoisoned,
    #[error("the system clock is before the Unix epoch")]
    ClockBeforeUnixEpoch,
    #[error("path cannot be represented without loss")]
    NonUnicodePath,
    #[error("invalid storage input: {0}")]
    InvalidInput(&'static str),
    #[error("migration versions must be positive and strictly increasing")]
    InvalidMigrationSequence,
    #[error("database contains unknown migration version {version}")]
    UnknownMigration { version: i64 },
    #[error("migration {version} checksum does not match the application")]
    MigrationChecksumMismatch { version: i64 },
    #[error("migration {version} failed")]
    MigrationFailed {
        version: i64,
        #[source]
        source: rusqlite::Error,
    },
    #[error("database contains invalid {field} value")]
    InvalidDatabaseValue { field: &'static str },
    #[error("storage arithmetic overflowed")]
    ArithmeticOverflow,
    #[error("source cursor changed before the batch could commit")]
    CursorConflict { current_offset: Option<u64> },
    #[error("{entity} refers to a missing related record")]
    MissingRelatedRecord { entity: &'static str },
    #[error("stable record identity was reused with different content in {entity}")]
    RecordIdentityConflict {
        entity: &'static str,
        source_record_key: String,
    },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DeviceIdentity {
    pub id: i64,
    pub public_id: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SourceRootIdentity {
    pub id: i64,
    pub public_id: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SourceFileLocation {
    Active,
    Archive,
    Index,
}

impl SourceFileLocation {
    fn as_db_str(self) -> &'static str {
        match self {
            Self::Active => "active",
            Self::Archive => "archive",
            Self::Index => "index",
        }
    }

    fn from_db(value: &str) -> Result<Self, StorageError> {
        match value {
            "active" => Ok(Self::Active),
            "archive" => Ok(Self::Archive),
            "index" => Ok(Self::Index),
            _ => Err(StorageError::InvalidDatabaseValue {
                field: "source_files.location_kind",
            }),
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct SourceFileCursor {
    pub id: i64,
    pub source_root_id: i64,
    pub logical_file_id: String,
    pub logical_session_id: Option<String>,
    pub location_kind: SourceFileLocation,
    pub stable_file_identity: Option<String>,
    pub identity_degraded: bool,
    pub cursor_generation: u64,
    pub committed_offset: u64,
    pub last_known_size: u64,
    pub last_modified_at_ms: Option<i64>,
    pub parser_state_version: u32,
    pub parser_state: Value,
    pub parser_state_hash: String,
    pub health: SourceHealthStatus,
    pub last_attempt_at_ms: Option<i64>,
    pub last_success_at_ms: Option<i64>,
    pub last_checked_at_ms: Option<i64>,
    pub last_error_code: Option<String>,
    pub malformed_records: u64,
    pub oversized_records: u64,
    pub data_quality_errors: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CursorExpectation {
    Missing,
    Match {
        cursor_generation: u64,
        committed_offset: u64,
        parser_state_hash: String,
    },
}

#[derive(Debug, Clone, PartialEq)]
pub struct SourceFileCursorWrite {
    pub source_root_id: i64,
    pub logical_file_id: String,
    pub logical_session_id: Option<String>,
    pub canonical_path: PathBuf,
    pub location_kind: SourceFileLocation,
    pub stable_file_identity: Option<String>,
    pub identity_degraded: bool,
    pub cursor_generation: u64,
    pub committed_offset: u64,
    pub last_known_size: u64,
    pub last_modified_at_ms: Option<i64>,
    pub parser_state_version: u32,
    pub parser_state: Value,
    pub health: SourceHealthStatus,
    pub last_attempt_at_ms: Option<i64>,
    pub last_success_at_ms: Option<i64>,
    pub last_checked_at_ms: Option<i64>,
    pub last_error_code: Option<String>,
    pub malformed_records: u64,
    pub oversized_records: u64,
    pub data_quality_errors: u64,
    pub expected: CursorExpectation,
}

#[derive(Debug, Clone, PartialEq)]
pub enum CursorUpsertResult {
    Applied(SourceFileCursor),
    Conflict(Option<SourceFileCursor>),
}

#[derive(Debug, Clone, PartialEq)]
pub struct CommitOutcome {
    pub read_model_changed: bool,
    pub cursor: SourceFileCursor,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SourceRootHealthUpdate {
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

pub struct Storage {
    connection: Mutex<Connection>,
}

impl Storage {
    pub fn open_file(path: impl AsRef<Path>) -> Result<Self, StorageError> {
        let mut connection = Connection::open(path)?;
        configure_connection(&connection, true)?;
        initialize(&mut connection)?;
        Ok(Self {
            connection: Mutex::new(connection),
        })
    }

    pub fn open_in_memory() -> Result<Self, StorageError> {
        let mut connection = Connection::open_in_memory()?;
        configure_connection(&connection, false)?;
        initialize(&mut connection)?;
        Ok(Self {
            connection: Mutex::new(connection),
        })
    }

    pub fn run_migrations(&self) -> Result<(), StorageError> {
        let mut connection = self.lock_connection()?;
        apply_migrations(&mut connection, MIGRATIONS)
    }

    pub fn device_identity(&self) -> Result<DeviceIdentity, StorageError> {
        let connection = self.lock_connection()?;
        load_device_identity(&connection)?.ok_or(StorageError::InvalidDatabaseValue {
            field: "devices.windows",
        })
    }

    pub fn ensure_codex_source_root(
        &self,
        canonical_path: &Path,
        stable_directory_identity: Option<&str>,
        identity_degraded: bool,
        now_ms: i64,
    ) -> Result<SourceRootIdentity, StorageError> {
        validate_timestamp(Some(now_ms))?;
        let canonical_path = path_text(canonical_path)?;
        if canonical_path.is_empty() {
            return Err(StorageError::InvalidInput("source root path is empty"));
        }

        let mut connection = self.lock_connection()?;
        let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
        transaction.execute(
            r#"
            INSERT INTO source_roots (
                public_id, source_kind, canonical_path, stable_directory_identity,
                identity_degraded, health, created_at_ms, updated_at_ms
            )
            VALUES (
                'source-' || lower(hex(randomblob(16))), 'codex', ?1, ?2,
                ?3, 'never', ?4, ?4
            )
            ON CONFLICT(canonical_path) DO UPDATE SET
                stable_directory_identity = COALESCE(
                    excluded.stable_directory_identity,
                    source_roots.stable_directory_identity
                ),
                identity_degraded = excluded.identity_degraded,
                updated_at_ms = MAX(source_roots.updated_at_ms, excluded.updated_at_ms)
            "#,
            params![
                canonical_path,
                stable_directory_identity,
                bool_to_i64(identity_degraded),
                now_ms
            ],
        )?;
        let identity = transaction.query_row(
            "SELECT id, public_id FROM source_roots WHERE canonical_path = ?1",
            [canonical_path],
            |row| {
                Ok(SourceRootIdentity {
                    id: row.get(0)?,
                    public_id: row.get(1)?,
                })
            },
        )?;
        transaction.commit()?;
        Ok(identity)
    }

    pub fn update_source_root_health(
        &self,
        source_root_id: i64,
        update: &SourceRootHealthUpdate,
    ) -> Result<bool, StorageError> {
        if source_root_id <= 0 {
            return Err(StorageError::InvalidInput(
                "source root id must be positive",
            ));
        }
        validate_timestamp(update.last_attempt_at_ms)?;
        validate_timestamp(update.last_success_at_ms)?;
        validate_timestamp(Some(update.updated_at_ms))?;
        validate_error_code(update.last_error_code.as_deref())?;

        let affected_file_count = to_i64(update.affected_file_count, "affected file count")?;
        let malformed_records = to_i64(update.malformed_records, "malformed record count")?;
        let oversized_records = to_i64(update.oversized_records, "oversized record count")?;
        let data_quality_errors = to_i64(update.data_quality_errors, "data quality error count")?;
        let connection = self.lock_connection()?;
        let changed = connection.execute(
            r#"
            UPDATE source_roots
            SET health = ?2,
                last_attempt_at_ms = ?3,
                last_success_at_ms = ?4,
                last_error_code = ?5,
                affected_file_count = ?6,
                malformed_records = ?7,
                oversized_records = ?8,
                data_quality_errors = ?9,
                updated_at_ms = ?10
            WHERE id = ?1
            "#,
            params![
                source_root_id,
                update.health.as_db_str(),
                update.last_attempt_at_ms,
                update.last_success_at_ms,
                update.last_error_code,
                affected_file_count,
                malformed_records,
                oversized_records,
                data_quality_errors,
                update.updated_at_ms,
            ],
        )?;
        Ok(changed == 1)
    }

    pub fn load_source_file_cursor(
        &self,
        source_root_id: i64,
        logical_file_id: &str,
    ) -> Result<Option<SourceFileCursor>, StorageError> {
        validate_cursor_key(source_root_id, logical_file_id)?;
        let connection = self.lock_connection()?;
        load_cursor(&connection, source_root_id, logical_file_id)
    }

    pub fn upsert_source_file_cursor(
        &self,
        write: &SourceFileCursorWrite,
    ) -> Result<CursorUpsertResult, StorageError> {
        validate_cursor_write(write)?;
        let canonical_path = path_text(&write.canonical_path)?;
        let parser_state_json = serde_json::to_string(&write.parser_state)?;
        let parser_state_hash = sha256_hex(parser_state_json.as_bytes());
        let cursor_generation = to_i64(write.cursor_generation, "cursor generation")?;
        let committed_offset = to_i64(write.committed_offset, "committed offset")?;
        let last_known_size = to_i64(write.last_known_size, "last known size")?;
        let malformed_records = to_i64(write.malformed_records, "malformed record count")?;
        let oversized_records = to_i64(write.oversized_records, "oversized record count")?;
        let data_quality_errors = to_i64(write.data_quality_errors, "data quality error count")?;

        let mut connection = self.lock_connection()?;
        let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
        let applied = match &write.expected {
            CursorExpectation::Missing => {
                if let Some(current) =
                    load_cursor(&transaction, write.source_root_id, &write.logical_file_id)?
                {
                    transaction.commit()?;
                    return Ok(CursorUpsertResult::Conflict(Some(current)));
                }
                transaction.execute(
                    INSERT_CURSOR_SQL,
                    params![
                        write.source_root_id,
                        write.logical_file_id,
                        write.logical_session_id,
                        canonical_path,
                        write.location_kind.as_db_str(),
                        write.stable_file_identity,
                        bool_to_i64(write.identity_degraded),
                        cursor_generation,
                        committed_offset,
                        last_known_size,
                        write.last_modified_at_ms,
                        i64::from(write.parser_state_version),
                        parser_state_json,
                        parser_state_hash,
                        write.health.as_db_str(),
                        write.last_attempt_at_ms,
                        write.last_success_at_ms,
                        write.last_checked_at_ms,
                        write.last_error_code,
                        malformed_records,
                        oversized_records,
                        data_quality_errors,
                    ],
                )? == 1
            }
            CursorExpectation::Match {
                cursor_generation: expected_generation,
                committed_offset: expected_offset,
                parser_state_hash: expected_hash,
            } => {
                let expected_generation =
                    to_i64(*expected_generation, "expected cursor generation")?;
                let expected_offset = to_i64(*expected_offset, "expected committed offset")?;
                transaction.execute(
                    UPDATE_CURSOR_SQL,
                    params![
                        write.source_root_id,
                        write.logical_file_id,
                        write.logical_session_id,
                        canonical_path,
                        write.location_kind.as_db_str(),
                        write.stable_file_identity,
                        bool_to_i64(write.identity_degraded),
                        cursor_generation,
                        committed_offset,
                        last_known_size,
                        write.last_modified_at_ms,
                        i64::from(write.parser_state_version),
                        parser_state_json,
                        parser_state_hash,
                        write.health.as_db_str(),
                        write.last_attempt_at_ms,
                        write.last_success_at_ms,
                        write.last_checked_at_ms,
                        write.last_error_code,
                        malformed_records,
                        oversized_records,
                        data_quality_errors,
                        expected_generation,
                        expected_offset,
                        expected_hash,
                    ],
                )? == 1
            }
        };

        if !applied {
            let current = load_cursor(&transaction, write.source_root_id, &write.logical_file_id)?;
            transaction.commit()?;
            return Ok(CursorUpsertResult::Conflict(current));
        }

        let cursor = load_cursor(&transaction, write.source_root_id, &write.logical_file_id)?
            .ok_or(StorageError::InvalidDatabaseValue {
                field: "source_files.upsert_result",
            })?;
        transaction.commit()?;
        Ok(CursorUpsertResult::Applied(cursor))
    }

    pub fn commit_codex_batch(
        &self,
        source_file: &SourceFileCursorWrite,
        batch: &ParsedBatch,
    ) -> Result<CommitOutcome, StorageError> {
        let mut final_cursor = source_file.clone();
        if let (Some(expected), Some(actual)) = (
            source_file.logical_session_id.as_deref(),
            batch.next_state.logical_session_id.as_deref(),
        ) && expected != actual
        {
            return Err(StorageError::InvalidInput(
                "cursor and parsed session identities differ",
            ));
        }
        final_cursor.logical_session_id = batch.next_state.logical_session_id.clone();
        final_cursor.committed_offset = batch.committed_offset;
        final_cursor.parser_state_version = u32::try_from(PARSER_STATE_VERSION)
            .map_err(|_| StorageError::InvalidInput("parser state version is invalid"))?;
        final_cursor.parser_state = serde_json::to_value(&batch.next_state)?;
        final_cursor.malformed_records = batch.next_state.malformed_records;
        final_cursor.oversized_records = batch.next_state.oversized_records;
        final_cursor.data_quality_errors = batch.next_state.data_quality_errors;
        if final_cursor.committed_offset > final_cursor.last_known_size {
            return Err(StorageError::InvalidInput(
                "committed offset exceeds the known file size",
            ));
        }
        let prepared_cursor = prepare_cursor_write(&final_cursor)?;

        let mut connection = self.lock_connection()?;
        let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
        let (source_file_id, expected_generation, expected_offset, expected_hash) =
            prepare_source_file_for_commit(&transaction, &final_cursor)?;
        let device_id: i64 = transaction.query_row(
            "SELECT id FROM devices WHERE kind = 'windows' ORDER BY id LIMIT 1",
            [],
            |row| row.get(0),
        )?;

        let mut read_model_changed = false;
        let mut sessions = HashMap::<String, i64>::new();
        if let Some(session) = batch.session.as_ref() {
            let (session_id, changed) = upsert_session(
                &transaction,
                final_cursor.source_root_id,
                device_id,
                session,
            )?;
            sessions.insert(session.session_id.clone(), session_id);
            read_model_changed |= changed;
        }

        let default_session_key = batch.next_state.logical_session_id.as_deref();
        if !batch.turns.is_empty() && default_session_key.is_none() {
            return Err(StorageError::MissingRelatedRecord {
                entity: "turns.session",
            });
        }
        let mut turns = HashMap::<(String, String), i64>::new();
        for turn in &batch.turns {
            let session_key = default_session_key.ok_or(StorageError::MissingRelatedRecord {
                entity: "turns.session",
            })?;
            let session_id = resolve_session_id(
                &transaction,
                final_cursor.source_root_id,
                session_key,
                &mut sessions,
            )?;
            let (turn_id, changed) = upsert_turn(&transaction, session_id, turn)?;
            turns.insert((session_key.to_string(), turn.turn_key.clone()), turn_id);
            read_model_changed |= changed;
        }

        for fact in &batch.usage {
            read_model_changed |= insert_usage_fact(
                &transaction,
                source_file_id,
                final_cursor.source_root_id,
                device_id,
                fact,
                &mut sessions,
                &mut turns,
            )?;
        }
        for fact in &batch.tool_starts {
            read_model_changed |= insert_tool_start(
                &transaction,
                source_file_id,
                final_cursor.source_root_id,
                fact,
                &mut sessions,
                &mut turns,
            )?;
        }
        for fact in &batch.tool_finishes {
            read_model_changed |= finish_tool_call(&transaction, fact)?;
        }
        for fact in &batch.quotas {
            read_model_changed |= insert_quota_snapshot(
                &transaction,
                source_file_id,
                final_cursor.source_root_id,
                fact,
                &mut sessions,
            )?;
        }

        let cursor_changed = update_cursor_row(
            &transaction,
            &final_cursor,
            &prepared_cursor,
            expected_generation,
            expected_offset,
            &expected_hash,
        )?;
        if !cursor_changed {
            let current = load_cursor(
                &transaction,
                final_cursor.source_root_id,
                &final_cursor.logical_file_id,
            )?;
            return Err(StorageError::CursorConflict {
                current_offset: current.map(|cursor| cursor.committed_offset),
            });
        }
        let cursor = load_cursor(
            &transaction,
            final_cursor.source_root_id,
            &final_cursor.logical_file_id,
        )?
        .ok_or(StorageError::InvalidDatabaseValue {
            field: "source_files.commit_result",
        })?;
        transaction.commit()?;
        Ok(CommitOutcome {
            read_model_changed,
            cursor,
        })
    }

    pub(super) fn lock_connection(&self) -> Result<MutexGuard<'_, Connection>, StorageError> {
        self.connection
            .lock()
            .map_err(|_| StorageError::LockPoisoned)
    }
}

#[derive(Clone, Copy)]
struct Migration {
    version: i64,
    name: &'static str,
    sql: &'static str,
}

fn configure_connection(connection: &Connection, file_database: bool) -> Result<(), StorageError> {
    connection.busy_timeout(BUSY_TIMEOUT)?;
    connection.pragma_update(None, "foreign_keys", "ON")?;
    if file_database {
        connection.pragma_update(None, "journal_mode", "WAL")?;
    }
    let foreign_keys: i64 = connection.query_row("PRAGMA foreign_keys", [], |row| row.get(0))?;
    if foreign_keys != 1 {
        return Err(StorageError::InvalidDatabaseValue {
            field: "PRAGMA foreign_keys",
        });
    }
    Ok(())
}

fn initialize(connection: &mut Connection) -> Result<(), StorageError> {
    apply_migrations(connection, MIGRATIONS)?;
    ensure_core_rows(connection, now_ms()?)
}

fn apply_migrations(
    connection: &mut Connection,
    migrations: &[Migration],
) -> Result<(), StorageError> {
    validate_migration_sequence(migrations)?;
    connection.execute_batch(MIGRATION_TABLE_SQL)?;

    let applied = {
        let mut statement = connection
            .prepare("SELECT version, checksum_sha256 FROM schema_migrations ORDER BY version")?;
        statement
            .query_map([], |row| {
                Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?))
            })?
            .collect::<Result<Vec<_>, _>>()?
    };

    for (version, stored_checksum) in applied {
        let Some(migration) = migrations
            .iter()
            .find(|migration| migration.version == version)
        else {
            return Err(StorageError::UnknownMigration { version });
        };
        if stored_checksum != migration_checksum(migration) {
            return Err(StorageError::MigrationChecksumMismatch { version });
        }
    }

    for migration in migrations {
        let already_applied: bool = connection.query_row(
            "SELECT EXISTS(SELECT 1 FROM schema_migrations WHERE version = ?1)",
            [migration.version],
            |row| row.get(0),
        )?;
        if already_applied {
            continue;
        }

        let checksum = migration_checksum(migration);
        let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
        transaction.execute_batch(migration.sql).map_err(|source| {
            StorageError::MigrationFailed {
                version: migration.version,
                source,
            }
        })?;
        transaction
            .execute(
                "INSERT INTO schema_migrations(version, name, checksum_sha256, applied_at_ms) VALUES (?1, ?2, ?3, ?4)",
                params![migration.version, migration.name, checksum, now_ms()?],
            )
            .map_err(|source| StorageError::MigrationFailed {
                version: migration.version,
                source,
            })?;
        transaction.commit()?;
    }
    Ok(())
}

fn validate_migration_sequence(migrations: &[Migration]) -> Result<(), StorageError> {
    if migrations
        .iter()
        .any(|migration| migration.version <= 0 || migration.name.is_empty())
        || migrations
            .windows(2)
            .any(|pair| pair[0].version >= pair[1].version)
    {
        return Err(StorageError::InvalidMigrationSequence);
    }
    Ok(())
}

fn migration_checksum(migration: &Migration) -> String {
    sha256_hex(migration.sql.as_bytes())
}

fn ensure_core_rows(connection: &mut Connection, now_ms: i64) -> Result<(), StorageError> {
    let transaction = connection.transaction_with_behavior(TransactionBehavior::Immediate)?;
    if load_device_identity(&transaction)?.is_none() {
        transaction.execute(
            r#"
            INSERT INTO devices(public_id, kind, created_at_ms, last_seen_at_ms)
            VALUES ('device-' || lower(hex(randomblob(16))), 'windows', ?1, ?1)
            "#,
            [now_ms],
        )?;
    } else {
        transaction.execute(
            "UPDATE devices SET last_seen_at_ms = MAX(last_seen_at_ms, ?1) WHERE kind = 'windows'",
            [now_ms],
        )?;
    }
    let account_exists: bool = transaction.query_row(
        r#"
        SELECT EXISTS(
            SELECT 1 FROM provider_accounts
            WHERE provider_key = 'codex' AND account_id = 'current'
        )
        "#,
        [],
        |row| row.get(0),
    )?;
    if !account_exists {
        transaction.execute(
            r#"
            INSERT INTO provider_accounts(provider_key, account_id, origin, health)
            VALUES ('codex', 'current', 'localTranscript', 'never')
            "#,
            [],
        )?;
    }
    transaction.commit()?;
    Ok(())
}

fn load_device_identity(connection: &Connection) -> Result<Option<DeviceIdentity>, StorageError> {
    connection
        .query_row(
            "SELECT id, public_id FROM devices WHERE kind = 'windows' ORDER BY id LIMIT 1",
            [],
            |row| {
                Ok(DeviceIdentity {
                    id: row.get(0)?,
                    public_id: row.get(1)?,
                })
            },
        )
        .optional()
        .map_err(StorageError::from)
}

const CURSOR_COLUMNS: &str = r#"
id, source_root_id, logical_file_id, logical_session_id, location_kind,
stable_file_identity, identity_degraded, cursor_generation, committed_offset,
last_known_size, last_modified_at_ms, parser_state_version, parser_state_json,
parser_state_hash, health, last_attempt_at_ms, last_success_at_ms,
last_checked_at_ms, last_error_code, malformed_records, oversized_records,
data_quality_errors
"#;

fn load_cursor(
    connection: &Connection,
    source_root_id: i64,
    logical_file_id: &str,
) -> Result<Option<SourceFileCursor>, StorageError> {
    let sql = format!(
        "SELECT {CURSOR_COLUMNS} FROM source_files WHERE source_root_id = ?1 AND logical_file_id = ?2"
    );
    let raw = connection
        .query_row(&sql, params![source_root_id, logical_file_id], |row| {
            Ok(RawCursor {
                id: row.get(0)?,
                source_root_id: row.get(1)?,
                logical_file_id: row.get(2)?,
                logical_session_id: row.get(3)?,
                location_kind: row.get(4)?,
                stable_file_identity: row.get(5)?,
                identity_degraded: row.get(6)?,
                cursor_generation: row.get(7)?,
                committed_offset: row.get(8)?,
                last_known_size: row.get(9)?,
                last_modified_at_ms: row.get(10)?,
                parser_state_version: row.get(11)?,
                parser_state_json: row.get(12)?,
                parser_state_hash: row.get(13)?,
                health: row.get(14)?,
                last_attempt_at_ms: row.get(15)?,
                last_success_at_ms: row.get(16)?,
                last_checked_at_ms: row.get(17)?,
                last_error_code: row.get(18)?,
                malformed_records: row.get(19)?,
                oversized_records: row.get(20)?,
                data_quality_errors: row.get(21)?,
            })
        })
        .optional()?;
    raw.map(TryInto::try_into).transpose()
}

struct RawCursor {
    id: i64,
    source_root_id: i64,
    logical_file_id: String,
    logical_session_id: Option<String>,
    location_kind: String,
    stable_file_identity: Option<String>,
    identity_degraded: i64,
    cursor_generation: i64,
    committed_offset: i64,
    last_known_size: i64,
    last_modified_at_ms: Option<i64>,
    parser_state_version: i64,
    parser_state_json: String,
    parser_state_hash: String,
    health: String,
    last_attempt_at_ms: Option<i64>,
    last_success_at_ms: Option<i64>,
    last_checked_at_ms: Option<i64>,
    last_error_code: Option<String>,
    malformed_records: i64,
    oversized_records: i64,
    data_quality_errors: i64,
}

impl TryFrom<RawCursor> for SourceFileCursor {
    type Error = StorageError;

    fn try_from(raw: RawCursor) -> Result<Self, Self::Error> {
        Ok(Self {
            id: raw.id,
            source_root_id: raw.source_root_id,
            logical_file_id: raw.logical_file_id,
            logical_session_id: raw.logical_session_id,
            location_kind: SourceFileLocation::from_db(&raw.location_kind)?,
            stable_file_identity: raw.stable_file_identity,
            identity_degraded: db_bool(raw.identity_degraded, "source_files.identity_degraded")?,
            cursor_generation: from_i64(raw.cursor_generation, "source_files.cursor_generation")?,
            committed_offset: from_i64(raw.committed_offset, "source_files.committed_offset")?,
            last_known_size: from_i64(raw.last_known_size, "source_files.last_known_size")?,
            last_modified_at_ms: raw.last_modified_at_ms,
            parser_state_version: u32::try_from(raw.parser_state_version).map_err(|_| {
                StorageError::InvalidDatabaseValue {
                    field: "source_files.parser_state_version",
                }
            })?,
            parser_state: serde_json::from_str(&raw.parser_state_json)?,
            parser_state_hash: raw.parser_state_hash,
            health: source_health_from_db(&raw.health)?,
            last_attempt_at_ms: raw.last_attempt_at_ms,
            last_success_at_ms: raw.last_success_at_ms,
            last_checked_at_ms: raw.last_checked_at_ms,
            last_error_code: raw.last_error_code,
            malformed_records: from_i64(raw.malformed_records, "source_files.malformed_records")?,
            oversized_records: from_i64(raw.oversized_records, "source_files.oversized_records")?,
            data_quality_errors: from_i64(
                raw.data_quality_errors,
                "source_files.data_quality_errors",
            )?,
        })
    }
}

const INSERT_CURSOR_SQL: &str = r#"
INSERT INTO source_files (
    source_root_id, logical_file_id, logical_session_id, canonical_path,
    location_kind, stable_file_identity, identity_degraded, cursor_generation,
    committed_offset, last_known_size, last_modified_at_ms,
    parser_state_version, parser_state_json, parser_state_hash, health,
    last_attempt_at_ms, last_success_at_ms, last_checked_at_ms, last_error_code,
    malformed_records, oversized_records, data_quality_errors
)
VALUES (
    ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15,
    ?16, ?17, ?18, ?19, ?20, ?21, ?22
)
"#;

const UPDATE_CURSOR_SQL: &str = r#"
UPDATE source_files SET
    logical_session_id = ?3,
    canonical_path = ?4,
    location_kind = ?5,
    stable_file_identity = ?6,
    identity_degraded = ?7,
    cursor_generation = ?8,
    committed_offset = ?9,
    last_known_size = ?10,
    last_modified_at_ms = ?11,
    parser_state_version = ?12,
    parser_state_json = ?13,
    parser_state_hash = ?14,
    health = ?15,
    last_attempt_at_ms = ?16,
    last_success_at_ms = ?17,
    last_checked_at_ms = ?18,
    last_error_code = ?19,
    malformed_records = ?20,
    oversized_records = ?21,
    data_quality_errors = ?22
WHERE source_root_id = ?1
  AND logical_file_id = ?2
  AND cursor_generation = ?23
  AND committed_offset = ?24
  AND parser_state_hash = ?25
"#;

struct PreparedCursorWrite {
    canonical_path: String,
    cursor_generation: i64,
    committed_offset: i64,
    last_known_size: i64,
    parser_state_json: String,
    parser_state_hash: String,
    malformed_records: i64,
    oversized_records: i64,
    data_quality_errors: i64,
}

fn prepare_cursor_write(
    write: &SourceFileCursorWrite,
) -> Result<PreparedCursorWrite, StorageError> {
    validate_cursor_write(write)?;
    let parser_state_json = serde_json::to_string(&write.parser_state)?;
    Ok(PreparedCursorWrite {
        canonical_path: path_text(&write.canonical_path)?.to_string(),
        cursor_generation: to_i64(write.cursor_generation, "cursor generation")?,
        committed_offset: to_i64(write.committed_offset, "committed offset")?,
        last_known_size: to_i64(write.last_known_size, "last known size")?,
        parser_state_hash: sha256_hex(parser_state_json.as_bytes()),
        parser_state_json,
        malformed_records: to_i64(write.malformed_records, "malformed record count")?,
        oversized_records: to_i64(write.oversized_records, "oversized record count")?,
        data_quality_errors: to_i64(write.data_quality_errors, "data quality error count")?,
    })
}

fn insert_cursor_row(
    connection: &Connection,
    write: &SourceFileCursorWrite,
    prepared: &PreparedCursorWrite,
) -> Result<bool, StorageError> {
    Ok(connection.execute(
        INSERT_CURSOR_SQL,
        params![
            write.source_root_id,
            write.logical_file_id,
            write.logical_session_id,
            prepared.canonical_path,
            write.location_kind.as_db_str(),
            write.stable_file_identity,
            bool_to_i64(write.identity_degraded),
            prepared.cursor_generation,
            prepared.committed_offset,
            prepared.last_known_size,
            write.last_modified_at_ms,
            i64::from(write.parser_state_version),
            prepared.parser_state_json,
            prepared.parser_state_hash,
            write.health.as_db_str(),
            write.last_attempt_at_ms,
            write.last_success_at_ms,
            write.last_checked_at_ms,
            write.last_error_code,
            prepared.malformed_records,
            prepared.oversized_records,
            prepared.data_quality_errors,
        ],
    )? == 1)
}

fn update_cursor_row(
    connection: &Connection,
    write: &SourceFileCursorWrite,
    prepared: &PreparedCursorWrite,
    expected_generation: i64,
    expected_offset: i64,
    expected_hash: &str,
) -> Result<bool, StorageError> {
    Ok(connection.execute(
        UPDATE_CURSOR_SQL,
        params![
            write.source_root_id,
            write.logical_file_id,
            write.logical_session_id,
            prepared.canonical_path,
            write.location_kind.as_db_str(),
            write.stable_file_identity,
            bool_to_i64(write.identity_degraded),
            prepared.cursor_generation,
            prepared.committed_offset,
            prepared.last_known_size,
            write.last_modified_at_ms,
            i64::from(write.parser_state_version),
            prepared.parser_state_json,
            prepared.parser_state_hash,
            write.health.as_db_str(),
            write.last_attempt_at_ms,
            write.last_success_at_ms,
            write.last_checked_at_ms,
            write.last_error_code,
            prepared.malformed_records,
            prepared.oversized_records,
            prepared.data_quality_errors,
            expected_generation,
            expected_offset,
            expected_hash,
        ],
    )? == 1)
}

fn prepare_source_file_for_commit(
    connection: &Connection,
    final_cursor: &SourceFileCursorWrite,
) -> Result<(i64, i64, i64, String), StorageError> {
    let current = load_cursor(
        connection,
        final_cursor.source_root_id,
        &final_cursor.logical_file_id,
    )?;
    match &final_cursor.expected {
        CursorExpectation::Missing => {
            if let Some(current) = current {
                return Err(StorageError::CursorConflict {
                    current_offset: Some(current.committed_offset),
                });
            }
            let mut placeholder = final_cursor.clone();
            placeholder.committed_offset = 0;
            placeholder.parser_state = serde_json::json!({});
            placeholder.malformed_records = 0;
            placeholder.oversized_records = 0;
            placeholder.data_quality_errors = 0;
            let prepared = prepare_cursor_write(&placeholder)?;
            if !insert_cursor_row(connection, &placeholder, &prepared)? {
                return Err(StorageError::InvalidDatabaseValue {
                    field: "source_files.placeholder_insert",
                });
            }
            let id = connection.query_row(
                "SELECT id FROM source_files WHERE source_root_id = ?1 AND logical_file_id = ?2",
                params![final_cursor.source_root_id, final_cursor.logical_file_id],
                |row| row.get(0),
            )?;
            Ok((
                id,
                prepared.cursor_generation,
                prepared.committed_offset,
                prepared.parser_state_hash,
            ))
        }
        CursorExpectation::Match {
            cursor_generation,
            committed_offset,
            parser_state_hash,
        } => {
            let Some(current) = current else {
                return Err(StorageError::CursorConflict {
                    current_offset: None,
                });
            };
            if current.cursor_generation != *cursor_generation
                || current.committed_offset != *committed_offset
                || current.parser_state_hash != *parser_state_hash
            {
                return Err(StorageError::CursorConflict {
                    current_offset: Some(current.committed_offset),
                });
            }
            Ok((
                current.id,
                to_i64(*cursor_generation, "expected cursor generation")?,
                to_i64(*committed_offset, "expected committed offset")?,
                parser_state_hash.clone(),
            ))
        }
    }
}

fn validate_cursor_write(write: &SourceFileCursorWrite) -> Result<(), StorageError> {
    validate_cursor_key(write.source_root_id, &write.logical_file_id)?;
    if write.canonical_path.as_os_str().is_empty() {
        return Err(StorageError::InvalidInput("source file path is empty"));
    }
    if write.parser_state_version == 0 {
        return Err(StorageError::InvalidInput(
            "parser state version must be positive",
        ));
    }
    validate_timestamp(write.last_modified_at_ms)?;
    validate_timestamp(write.last_attempt_at_ms)?;
    validate_timestamp(write.last_success_at_ms)?;
    validate_timestamp(write.last_checked_at_ms)?;
    validate_error_code(write.last_error_code.as_deref())?;
    if let CursorExpectation::Match {
        parser_state_hash, ..
    } = &write.expected
        && (parser_state_hash.len() != 64
            || !parser_state_hash
                .bytes()
                .all(|byte| byte.is_ascii_hexdigit()))
    {
        return Err(StorageError::InvalidInput(
            "expected parser state hash is invalid",
        ));
    }
    Ok(())
}

fn validate_cursor_key(source_root_id: i64, logical_file_id: &str) -> Result<(), StorageError> {
    if source_root_id <= 0 {
        return Err(StorageError::InvalidInput(
            "source root id must be positive",
        ));
    }
    if logical_file_id.is_empty() {
        return Err(StorageError::InvalidInput("logical file id is empty"));
    }
    Ok(())
}

fn validate_timestamp(timestamp: Option<i64>) -> Result<(), StorageError> {
    if timestamp.is_some_and(|value| value < 0) {
        return Err(StorageError::InvalidInput("timestamp cannot be negative"));
    }
    Ok(())
}

fn validate_error_code(error_code: Option<&str>) -> Result<(), StorageError> {
    let Some(error_code) = error_code else {
        return Ok(());
    };
    if error_code.len() > MAX_ERROR_CODE_LEN
        || error_code.is_empty()
        || !error_code
            .chars()
            .all(|ch| ch.is_ascii_alphanumeric() || matches!(ch, '_' | '-' | '.' | ':'))
    {
        return Err(StorageError::InvalidInput("error code is not safe"));
    }
    Ok(())
}

fn now_ms() -> Result<i64, StorageError> {
    let now = Utc::now().timestamp_millis();
    if now < 0 {
        Err(StorageError::ClockBeforeUnixEpoch)
    } else {
        Ok(now)
    }
}

fn path_text(path: &Path) -> Result<&str, StorageError> {
    path.to_str().ok_or(StorageError::NonUnicodePath)
}

fn sha256_hex(bytes: &[u8]) -> String {
    let digest = Sha256::digest(bytes);
    let mut output = String::with_capacity(digest.len() * 2);
    for byte in digest {
        use std::fmt::Write as _;
        let _ = write!(output, "{byte:02x}");
    }
    output
}

fn to_i64(value: u64, field: &'static str) -> Result<i64, StorageError> {
    i64::try_from(value).map_err(|_| StorageError::InvalidInput(field))
}

fn from_i64(value: i64, field: &'static str) -> Result<u64, StorageError> {
    u64::try_from(value).map_err(|_| StorageError::InvalidDatabaseValue { field })
}

fn bool_to_i64(value: bool) -> i64 {
    if value { 1 } else { 0 }
}

fn db_bool(value: i64, field: &'static str) -> Result<bool, StorageError> {
    match value {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err(StorageError::InvalidDatabaseValue { field }),
    }
}

pub(super) fn source_health_from_db(value: &str) -> Result<SourceHealthStatus, StorageError> {
    match value {
        "never" => Ok(SourceHealthStatus::Never),
        "healthy" => Ok(SourceHealthStatus::Healthy),
        "partial" => Ok(SourceHealthStatus::Partial),
        "stale" => Ok(SourceHealthStatus::Stale),
        "failed" => Ok(SourceHealthStatus::Failed),
        _ => Err(StorageError::InvalidDatabaseValue {
            field: "source health",
        }),
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct StoredSession {
    id: i64,
    originator: Option<String>,
    cli_version: Option<String>,
    workspace_path: Option<String>,
    safe_project_label: Option<String>,
    started_at_ms: Option<i64>,
    updated_at_ms: Option<i64>,
    latest_model: Option<String>,
    latest_effort: Option<String>,
    transcript_status: String,
    status_reason: Option<String>,
}

fn upsert_session(
    connection: &Connection,
    source_root_id: i64,
    device_id: i64,
    session: &SessionSnapshotFact,
) -> Result<(i64, bool), StorageError> {
    if source_root_id <= 0 || device_id <= 0 || session.session_id.is_empty() {
        return Err(StorageError::InvalidInput("session identity is invalid"));
    }
    validate_timestamp(session.started_at_ms)?;
    validate_timestamp(session.updated_at_ms)?;
    let before = load_session(connection, source_root_id, &session.session_id)?;
    connection.execute(
        r#"
        INSERT INTO sessions(
            source_root_id, device_id, provider_key, account_id, session_id,
            originator, cli_version, workspace_path, safe_project_label,
            started_at_ms, updated_at_ms, latest_model, latest_effort,
            transcript_status, status_confidence, status_reason, measurement_kind
        ) VALUES (
            ?1, ?2, 'codex', 'current', ?3, ?4, ?5, ?6, ?7,
            ?8, ?9, ?10, ?11, ?12, 'low', ?13, 'exact'
        )
        ON CONFLICT(source_root_id, session_id) DO UPDATE SET
            started_at_ms = CASE
                WHEN sessions.started_at_ms IS NULL THEN excluded.started_at_ms
                WHEN excluded.started_at_ms IS NULL THEN sessions.started_at_ms
                ELSE MIN(sessions.started_at_ms, excluded.started_at_ms)
            END,
            updated_at_ms = CASE
                WHEN sessions.updated_at_ms IS NULL THEN excluded.updated_at_ms
                WHEN excluded.updated_at_ms IS NULL THEN sessions.updated_at_ms
                ELSE MAX(sessions.updated_at_ms, excluded.updated_at_ms)
            END,
            originator = CASE
                WHEN sessions.originator IS NULL THEN excluded.originator
                WHEN excluded.updated_at_ms IS NOT NULL
                  AND (sessions.updated_at_ms IS NULL OR excluded.updated_at_ms > sessions.updated_at_ms)
                THEN COALESCE(excluded.originator, sessions.originator)
                ELSE sessions.originator
            END,
            cli_version = CASE
                WHEN sessions.cli_version IS NULL THEN excluded.cli_version
                WHEN excluded.updated_at_ms IS NOT NULL
                  AND (sessions.updated_at_ms IS NULL OR excluded.updated_at_ms > sessions.updated_at_ms)
                THEN COALESCE(excluded.cli_version, sessions.cli_version)
                ELSE sessions.cli_version
            END,
            workspace_path = CASE
                WHEN sessions.workspace_path IS NULL THEN excluded.workspace_path
                WHEN excluded.updated_at_ms IS NOT NULL
                  AND (sessions.updated_at_ms IS NULL OR excluded.updated_at_ms > sessions.updated_at_ms)
                THEN COALESCE(excluded.workspace_path, sessions.workspace_path)
                ELSE sessions.workspace_path
            END,
            safe_project_label = CASE
                WHEN sessions.safe_project_label IS NULL THEN excluded.safe_project_label
                WHEN excluded.updated_at_ms IS NOT NULL
                  AND (sessions.updated_at_ms IS NULL OR excluded.updated_at_ms > sessions.updated_at_ms)
                THEN COALESCE(excluded.safe_project_label, sessions.safe_project_label)
                ELSE sessions.safe_project_label
            END,
            latest_model = CASE
                WHEN sessions.latest_model IS NULL THEN excluded.latest_model
                WHEN excluded.updated_at_ms IS NOT NULL
                  AND (sessions.updated_at_ms IS NULL OR excluded.updated_at_ms > sessions.updated_at_ms)
                THEN COALESCE(excluded.latest_model, sessions.latest_model)
                ELSE sessions.latest_model
            END,
            latest_effort = CASE
                WHEN sessions.latest_effort IS NULL THEN excluded.latest_effort
                WHEN excluded.updated_at_ms IS NOT NULL
                  AND (sessions.updated_at_ms IS NULL OR excluded.updated_at_ms > sessions.updated_at_ms)
                THEN COALESCE(excluded.latest_effort, sessions.latest_effort)
                ELSE sessions.latest_effort
            END,
            transcript_status = CASE
                WHEN excluded.updated_at_ms IS NOT NULL
                  AND (sessions.updated_at_ms IS NULL OR excluded.updated_at_ms > sessions.updated_at_ms)
                THEN excluded.transcript_status
                ELSE sessions.transcript_status
            END,
            status_confidence = CASE
                WHEN excluded.updated_at_ms IS NOT NULL
                  AND (sessions.updated_at_ms IS NULL OR excluded.updated_at_ms > sessions.updated_at_ms)
                THEN excluded.status_confidence
                ELSE sessions.status_confidence
            END,
            status_reason = CASE
                WHEN excluded.updated_at_ms IS NOT NULL
                  AND (sessions.updated_at_ms IS NULL OR excluded.updated_at_ms > sessions.updated_at_ms)
                THEN excluded.status_reason
                ELSE sessions.status_reason
            END
        "#,
        params![
            source_root_id,
            device_id,
            session.session_id,
            session.originator,
            session.cli_version,
            session.workspace_path,
            session.safe_project_label,
            session.started_at_ms,
            session.updated_at_ms,
            session.model,
            session.effort,
            session.transcript_status.as_db_str(),
            session.transcript_status.reason(),
        ],
    )?;
    let after = load_session(connection, source_root_id, &session.session_id)?.ok_or(
        StorageError::InvalidDatabaseValue {
            field: "sessions.upsert_result",
        },
    )?;
    let changed = before.as_ref() != Some(&after);
    Ok((after.id, changed))
}

fn load_session(
    connection: &Connection,
    source_root_id: i64,
    session_key: &str,
) -> Result<Option<StoredSession>, StorageError> {
    connection
        .query_row(
            r#"
            SELECT id, originator, cli_version, workspace_path, safe_project_label,
                   started_at_ms, updated_at_ms, latest_model, latest_effort,
                   transcript_status, status_reason
            FROM sessions
            WHERE source_root_id = ?1 AND session_id = ?2
            "#,
            params![source_root_id, session_key],
            |row| {
                Ok(StoredSession {
                    id: row.get(0)?,
                    originator: row.get(1)?,
                    cli_version: row.get(2)?,
                    workspace_path: row.get(3)?,
                    safe_project_label: row.get(4)?,
                    started_at_ms: row.get(5)?,
                    updated_at_ms: row.get(6)?,
                    latest_model: row.get(7)?,
                    latest_effort: row.get(8)?,
                    transcript_status: row.get(9)?,
                    status_reason: row.get(10)?,
                })
            },
        )
        .optional()
        .map_err(StorageError::from)
}

fn resolve_session_id(
    connection: &Connection,
    source_root_id: i64,
    session_key: &str,
    cache: &mut HashMap<String, i64>,
) -> Result<i64, StorageError> {
    if let Some(id) = cache.get(session_key) {
        return Ok(*id);
    }
    let id = load_session(connection, source_root_id, session_key)?
        .map(|session| session.id)
        .ok_or(StorageError::MissingRelatedRecord {
            entity: "fact.session",
        })?;
    cache.insert(session_key.to_string(), id);
    Ok(id)
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct StoredTurn {
    id: i64,
    turn_key: String,
    prompt_index: i64,
    model: Option<String>,
    effort: Option<String>,
    workspace_path: Option<String>,
    started_at_ms: Option<i64>,
    updated_at_ms: Option<i64>,
    latest_input_total: Option<i64>,
    latest_cache_read_input: Option<i64>,
    latest_cache_write_input: Option<i64>,
    latest_output_total: Option<i64>,
    latest_reasoning_output: Option<i64>,
    latest_reported_total: Option<i64>,
    invalid_breakdown: i64,
}

fn upsert_turn(
    connection: &Connection,
    session_id: i64,
    turn: &TurnFact,
) -> Result<(i64, bool), StorageError> {
    if session_id <= 0 || turn.turn_key.is_empty() {
        return Err(StorageError::InvalidInput("turn identity is invalid"));
    }
    validate_timestamp(turn.started_at_ms)?;
    validate_timestamp(turn.updated_at_ms)?;
    let prompt_index = to_i64(turn.prompt_index, "turn prompt index")?;
    let before = load_turn(connection, session_id, &turn.turn_key)?;
    if before
        .as_ref()
        .is_some_and(|stored| stored.prompt_index != prompt_index)
    {
        return Err(StorageError::RecordIdentityConflict {
            entity: "turns.turn_key",
            source_record_key: turn.turn_key.clone(),
        });
    }
    let prompt_owner: Option<String> = connection
        .query_row(
            "SELECT turn_key FROM turns WHERE session_id = ?1 AND prompt_index = ?2",
            params![session_id, prompt_index],
            |row| row.get(0),
        )
        .optional()?;
    if prompt_owner
        .as_deref()
        .is_some_and(|owner| owner != turn.turn_key)
    {
        return Err(StorageError::RecordIdentityConflict {
            entity: "turns.prompt_index",
            source_record_key: turn.turn_key.clone(),
        });
    }
    connection.execute(
        r#"
        INSERT INTO turns(
            session_id, turn_key, prompt_index, model, effort, workspace_path,
            started_at_ms, updated_at_ms
        ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
        ON CONFLICT(session_id, turn_key) DO UPDATE SET
            started_at_ms = CASE
                WHEN turns.started_at_ms IS NULL THEN excluded.started_at_ms
                WHEN excluded.started_at_ms IS NULL THEN turns.started_at_ms
                ELSE MIN(turns.started_at_ms, excluded.started_at_ms)
            END,
            updated_at_ms = CASE
                WHEN turns.updated_at_ms IS NULL THEN excluded.updated_at_ms
                WHEN excluded.updated_at_ms IS NULL THEN turns.updated_at_ms
                ELSE MAX(turns.updated_at_ms, excluded.updated_at_ms)
            END,
            model = CASE
                WHEN turns.model IS NULL THEN excluded.model
                WHEN excluded.updated_at_ms IS NOT NULL
                  AND (turns.updated_at_ms IS NULL OR excluded.updated_at_ms > turns.updated_at_ms)
                THEN COALESCE(excluded.model, turns.model)
                ELSE turns.model
            END,
            effort = CASE
                WHEN turns.effort IS NULL THEN excluded.effort
                WHEN excluded.updated_at_ms IS NOT NULL
                  AND (turns.updated_at_ms IS NULL OR excluded.updated_at_ms > turns.updated_at_ms)
                THEN COALESCE(excluded.effort, turns.effort)
                ELSE turns.effort
            END,
            workspace_path = CASE
                WHEN turns.workspace_path IS NULL THEN excluded.workspace_path
                WHEN excluded.updated_at_ms IS NOT NULL
                  AND (turns.updated_at_ms IS NULL OR excluded.updated_at_ms > turns.updated_at_ms)
                THEN COALESCE(excluded.workspace_path, turns.workspace_path)
                ELSE turns.workspace_path
            END
        "#,
        params![
            session_id,
            turn.turn_key,
            prompt_index,
            turn.model,
            turn.effort,
            turn.workspace_path,
            turn.started_at_ms,
            turn.updated_at_ms,
        ],
    )?;
    let after = load_turn(connection, session_id, &turn.turn_key)?.ok_or(
        StorageError::InvalidDatabaseValue {
            field: "turns.upsert_result",
        },
    )?;
    let changed = before.as_ref() != Some(&after);
    Ok((after.id, changed))
}

fn load_turn(
    connection: &Connection,
    session_id: i64,
    turn_key: &str,
) -> Result<Option<StoredTurn>, StorageError> {
    connection
        .query_row(
            r#"
            SELECT id, turn_key, prompt_index, model, effort, workspace_path,
                   started_at_ms, updated_at_ms, latest_input_total,
                   latest_cache_read_input, latest_cache_write_input,
                   latest_output_total, latest_reasoning_output,
                   latest_reported_total, invalid_breakdown
            FROM turns WHERE session_id = ?1 AND turn_key = ?2
            "#,
            params![session_id, turn_key],
            |row| {
                Ok(StoredTurn {
                    id: row.get(0)?,
                    turn_key: row.get(1)?,
                    prompt_index: row.get(2)?,
                    model: row.get(3)?,
                    effort: row.get(4)?,
                    workspace_path: row.get(5)?,
                    started_at_ms: row.get(6)?,
                    updated_at_ms: row.get(7)?,
                    latest_input_total: row.get(8)?,
                    latest_cache_read_input: row.get(9)?,
                    latest_cache_write_input: row.get(10)?,
                    latest_output_total: row.get(11)?,
                    latest_reasoning_output: row.get(12)?,
                    latest_reported_total: row.get(13)?,
                    invalid_breakdown: row.get(14)?,
                })
            },
        )
        .optional()
        .map_err(StorageError::from)
}

fn resolve_turn_id(
    connection: &Connection,
    session_key: &str,
    session_id: i64,
    turn_key: Option<&str>,
    cache: &mut HashMap<(String, String), i64>,
) -> Result<Option<i64>, StorageError> {
    let Some(turn_key) = turn_key else {
        return Ok(None);
    };
    let cache_key = (session_key.to_string(), turn_key.to_string());
    if let Some(id) = cache.get(&cache_key) {
        return Ok(Some(*id));
    }
    let id = load_turn(connection, session_id, turn_key)?
        .map(|turn| turn.id)
        .ok_or(StorageError::MissingRelatedRecord {
            entity: "fact.turn",
        })?;
    cache.insert(cache_key, id);
    Ok(Some(id))
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum RecordDisposition {
    New,
    Replay,
}

fn payload_record_disposition(
    connection: &Connection,
    select_sql: &'static str,
    entity: &'static str,
    source_record_key: &str,
    source_payload_hash: &str,
) -> Result<RecordDisposition, StorageError> {
    validate_sha256(source_record_key, "source record key")?;
    validate_sha256(source_payload_hash, "source payload hash")?;
    let stored_hash: Option<String> = connection
        .query_row(select_sql, [source_record_key], |row| row.get(0))
        .optional()?;
    match stored_hash {
        None => Ok(RecordDisposition::New),
        Some(stored_hash) if stored_hash == source_payload_hash => Ok(RecordDisposition::Replay),
        Some(_) => Err(StorageError::RecordIdentityConflict {
            entity,
            source_record_key: source_record_key.to_string(),
        }),
    }
}

#[allow(clippy::too_many_arguments)]
fn insert_usage_fact(
    connection: &Connection,
    source_file_id: i64,
    source_root_id: i64,
    device_id: i64,
    fact: &UsageFact,
    sessions: &mut HashMap<String, i64>,
    turns: &mut HashMap<(String, String), i64>,
) -> Result<bool, StorageError> {
    match payload_record_disposition(
        connection,
        "SELECT source_payload_hash FROM usage_events WHERE source_record_key = ?1",
        "usage_events",
        &fact.source_record_key,
        &fact.source_payload_hash,
    )? {
        RecordDisposition::Replay => return Ok(false),
        RecordDisposition::New => {}
    }
    validate_timestamp(fact.effective_at_ms)?;
    validate_timestamp(Some(fact.observed_at_ms))?;
    validate_token_counts(&fact.counts)?;
    let session_id = resolve_session_id(connection, source_root_id, &fact.session_id, sessions)?;
    let turn_id = resolve_turn_id(
        connection,
        &fact.session_id,
        session_id,
        fact.turn_key.as_deref(),
        turns,
    )?;
    let cumulative_generation = to_i64(fact.cumulative_generation, "cumulative generation")?;
    let source_offset = to_i64(fact.source_offset, "usage source offset")?;
    connection.execute(
        r#"
        INSERT INTO usage_events(
            source_record_key, source_payload_hash, source_file_id, session_id,
            turn_id, device_id, provider_key, account_id, metric_scope,
            cumulative_generation, effective_at_ms, observed_at_ms, model,
            effort, measurement_kind, reconciliation, input_total,
            cache_read_input, cache_write_input, output_total, reasoning_output,
            reported_total, invalid_breakdown, source_offset
        ) VALUES (
            ?1, ?2, ?3, ?4, ?5, ?6, 'codex', 'current', ?7, ?8, ?9, ?10,
            ?11, ?12, 'exact', ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21
        )
        "#,
        params![
            fact.source_record_key,
            fact.source_payload_hash,
            source_file_id,
            session_id,
            turn_id,
            device_id,
            fact.metric_scope.as_db_str(),
            cumulative_generation,
            fact.effective_at_ms,
            fact.observed_at_ms,
            fact.model,
            fact.effort,
            fact.reconciliation.as_db_str(),
            fact.counts.input_total,
            fact.counts.cache_read_input,
            fact.counts.cache_write_input,
            fact.counts.output_total,
            fact.counts.reasoning_output,
            fact.counts.reported_total,
            bool_to_i64(fact.counts.invalid_breakdown),
            source_offset,
        ],
    )?;

    if fact.metric_scope == MetricScope::TurnReported
        && let Some(turn_id) = turn_id
    {
        connection.execute(
            r#"
            UPDATE turns SET
                latest_input_total = ?2,
                latest_cache_read_input = ?3,
                latest_cache_write_input = ?4,
                latest_output_total = ?5,
                latest_reasoning_output = ?6,
                latest_reported_total = ?7,
                invalid_breakdown = ?8,
                updated_at_ms = CASE
                    WHEN updated_at_ms IS NULL THEN ?9
                    WHEN ?9 IS NULL THEN updated_at_ms
                    ELSE MAX(updated_at_ms, ?9)
                END
            WHERE id = ?1
              AND (
                  latest_input_total IS NULL
                  OR (?9 IS NOT NULL AND (updated_at_ms IS NULL OR ?9 > updated_at_ms))
              )
            "#,
            params![
                turn_id,
                fact.counts.input_total,
                fact.counts.cache_read_input,
                fact.counts.cache_write_input,
                fact.counts.output_total,
                fact.counts.reasoning_output,
                fact.counts.reported_total,
                bool_to_i64(fact.counts.invalid_breakdown),
                fact.effective_at_ms,
            ],
        )?;
    }
    Ok(true)
}

#[allow(clippy::too_many_arguments)]
fn insert_tool_start(
    connection: &Connection,
    source_file_id: i64,
    source_root_id: i64,
    fact: &ToolStartFact,
    sessions: &mut HashMap<String, i64>,
    turns: &mut HashMap<(String, String), i64>,
) -> Result<bool, StorageError> {
    match payload_record_disposition(
        connection,
        "SELECT source_payload_hash FROM tool_calls WHERE source_record_key = ?1",
        "tool_calls",
        &fact.source_record_key,
        &fact.source_payload_hash,
    )? {
        RecordDisposition::Replay => return Ok(false),
        RecordDisposition::New => {}
    }
    validate_timestamp(fact.started_at_ms)?;
    if fact.call_key.is_empty() || fact.tool_name.is_empty() || fact.source_file_identity.is_empty()
    {
        return Err(StorageError::InvalidInput("tool identity is invalid"));
    }
    let session_id = resolve_session_id(connection, source_root_id, &fact.session_id, sessions)?;
    let turn_id = resolve_turn_id(
        connection,
        &fact.session_id,
        session_id,
        fact.turn_key.as_deref(),
        turns,
    )?;
    let existing_call: Option<String> = connection
        .query_row(
            "SELECT source_record_key FROM tool_calls WHERE session_id = ?1 AND call_key = ?2",
            params![session_id, fact.call_key],
            |row| row.get(0),
        )
        .optional()?;
    if existing_call.is_some() {
        return Err(StorageError::RecordIdentityConflict {
            entity: "tool_calls.call_key",
            source_record_key: fact.source_record_key.clone(),
        });
    }
    connection.execute(
        r#"
        INSERT INTO tool_calls(
            source_record_key, source_payload_hash, source_file_id, session_id,
            turn_id, call_key, tool_name, started_at_ms, status,
            input_file_identity, input_offset, input_length
        ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, 'open', ?9, ?10, ?11)
        "#,
        params![
            fact.source_record_key,
            fact.source_payload_hash,
            source_file_id,
            session_id,
            turn_id,
            fact.call_key,
            fact.tool_name,
            fact.started_at_ms,
            fact.source_file_identity,
            to_i64(fact.input_offset, "tool input offset")?,
            to_i64(fact.input_length, "tool input length")?,
        ],
    )?;
    Ok(true)
}

fn finish_tool_call(connection: &Connection, fact: &ToolFinishFact) -> Result<bool, StorageError> {
    validate_sha256(&fact.start_source_record_key, "tool start record key")?;
    validate_sha256(&fact.closed_by_record_key, "tool finish record key")?;
    validate_timestamp(fact.ended_at_ms)?;
    if fact.output_file_identity.is_empty() {
        return Err(StorageError::InvalidInput(
            "tool output file identity is invalid",
        ));
    }
    let output_offset = to_i64(fact.output_offset, "tool output offset")?;
    let output_length = to_i64(fact.output_length, "tool output length")?;
    let stored = connection
        .query_row(
            r#"
            SELECT id, status, closed_by_record_key, ended_at_ms,
                   output_file_identity, output_offset, output_length
            FROM tool_calls WHERE source_record_key = ?1
            "#,
            [&fact.start_source_record_key],
            |row| {
                Ok((
                    row.get::<_, i64>(0)?,
                    row.get::<_, String>(1)?,
                    row.get::<_, Option<String>>(2)?,
                    row.get::<_, Option<i64>>(3)?,
                    row.get::<_, Option<String>>(4)?,
                    row.get::<_, Option<i64>>(5)?,
                    row.get::<_, Option<i64>>(6)?,
                ))
            },
        )
        .optional()?
        .ok_or(StorageError::MissingRelatedRecord {
            entity: "tool_finish.start",
        })?;
    if stored.1 == "closed" {
        if stored.2.as_deref() == Some(fact.closed_by_record_key.as_str())
            && stored.3 == fact.ended_at_ms
            && stored.4.as_deref() == Some(fact.output_file_identity.as_str())
            && stored.5 == Some(output_offset)
            && stored.6 == Some(output_length)
        {
            return Ok(false);
        }
        return Err(StorageError::RecordIdentityConflict {
            entity: "tool_calls.finish",
            source_record_key: fact.closed_by_record_key.clone(),
        });
    }
    let existing_finish: Option<i64> = connection
        .query_row(
            "SELECT id FROM tool_calls WHERE closed_by_record_key = ?1",
            [&fact.closed_by_record_key],
            |row| row.get(0),
        )
        .optional()?;
    if existing_finish.is_some() {
        return Err(StorageError::RecordIdentityConflict {
            entity: "tool_calls.closed_by_record_key",
            source_record_key: fact.closed_by_record_key.clone(),
        });
    }
    let changed = connection.execute(
        r#"
        UPDATE tool_calls SET
            ended_at_ms = ?2,
            status = 'closed',
            closed_by_record_key = ?3,
            output_file_identity = ?4,
            output_offset = ?5,
            output_length = ?6
        WHERE id = ?1 AND status = 'open'
        "#,
        params![
            stored.0,
            fact.ended_at_ms,
            fact.closed_by_record_key,
            fact.output_file_identity,
            output_offset,
            output_length,
        ],
    )?;
    if changed != 1 {
        return Err(StorageError::RecordIdentityConflict {
            entity: "tool_calls.finish",
            source_record_key: fact.closed_by_record_key.clone(),
        });
    }
    Ok(true)
}

fn insert_quota_snapshot(
    connection: &Connection,
    source_file_id: i64,
    source_root_id: i64,
    fact: &QuotaSnapshotFact,
    sessions: &mut HashMap<String, i64>,
) -> Result<bool, StorageError> {
    match payload_record_disposition(
        connection,
        "SELECT source_payload_hash FROM quota_snapshots WHERE source_record_key = ?1",
        "quota_snapshots",
        &fact.source_record_key,
        &fact.source_payload_hash,
    )? {
        RecordDisposition::Replay => return Ok(false),
        RecordDisposition::New => {}
    }
    validate_timestamp(fact.captured_at_ms)?;
    validate_timestamp(Some(fact.observed_at_ms))?;
    let session_id = resolve_session_id(connection, source_root_id, &fact.session_id, sessions)?;
    connection.execute(
        r#"
        INSERT INTO quota_snapshots(
            source_record_key, source_payload_hash, source_file_id, session_id,
            provider_key, account_id, source_key, provider_limit_id,
            captured_at_ms, observed_at_ms, measurement_kind, status, plan
        ) VALUES (
            ?1, ?2, ?3, ?4, 'codex', 'current', 'transcript', ?5,
            ?6, ?7, 'exact', 'available', ?8
        )
        "#,
        params![
            fact.source_record_key,
            fact.source_payload_hash,
            source_file_id,
            session_id,
            fact.provider_limit_id,
            fact.captured_at_ms,
            fact.observed_at_ms,
            fact.plan,
        ],
    )?;
    let snapshot_id = connection.last_insert_rowid();
    let mut window_keys = std::collections::HashSet::new();
    for window in &fact.windows {
        if window.window_key.is_empty()
            || window.window_kind.is_empty()
            || !window_keys.insert(window.window_key.as_str())
        {
            return Err(StorageError::InvalidInput(
                "quota window identity is invalid or duplicated",
            ));
        }
        if window.window_minutes.is_some_and(|value| value <= 0)
            || window
                .used_percent_micros
                .is_some_and(|value| !(0..=100_000_000).contains(&value))
        {
            return Err(StorageError::InvalidInput("quota window value is invalid"));
        }
        validate_timestamp(window.resets_at_ms)?;
        connection.execute(
            r#"
            INSERT INTO quota_window_values(
                snapshot_id, window_key, window_kind, label, window_minutes,
                used_percent_micros, resets_at_ms
            ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
            "#,
            params![
                snapshot_id,
                window.window_key,
                window.window_kind,
                window.label,
                window.window_minutes,
                window.used_percent_micros,
                window.resets_at_ms,
            ],
        )?;
    }
    Ok(true)
}

fn validate_token_counts(counts: &crate::domain::TokenCounts) -> Result<(), StorageError> {
    if counts.input_total < 0
        || counts.output_total < 0
        || [
            counts.cache_read_input,
            counts.cache_write_input,
            counts.reasoning_output,
            counts.reported_total,
        ]
        .into_iter()
        .flatten()
        .any(|value| value < 0)
    {
        return Err(StorageError::InvalidInput("token count cannot be negative"));
    }
    counts
        .input_total
        .checked_add(counts.output_total)
        .ok_or(StorageError::ArithmeticOverflow)?;
    Ok(())
}

fn validate_sha256(value: &str, field: &'static str) -> Result<(), StorageError> {
    if value.len() != 64 || !value.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return Err(StorageError::InvalidInput(field));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use std::path::Path;

    use rusqlite::Connection;
    use serde_json::json;
    use tempfile::tempdir;

    use super::{
        CursorExpectation, CursorUpsertResult, Migration, SourceFileCursorWrite,
        SourceFileLocation, SourceHealthStatus, Storage, StorageError, StorageTable,
        apply_migrations, configure_connection,
    };
    use crate::{
        collector::codex::{
            MetricScope, ParsedBatch, ParserStateV1, QuotaSnapshotFact, QuotaWindowFact,
            SessionSnapshotFact, ToolFinishFact, ToolStartFact, TranscriptStatus, TurnFact,
            UsageFact,
        },
        domain::{Reconciliation, TokenCounts},
    };

    #[test]
    fn empty_database_migrates_and_seeds_stable_core_rows() {
        let storage = Storage::open_in_memory().unwrap();

        assert_eq!(
            storage.row_count(StorageTable::SchemaMigrations).unwrap(),
            1
        );
        assert_eq!(storage.row_count(StorageTable::Devices).unwrap(), 1);
        assert_eq!(
            storage.row_count(StorageTable::ProviderAccounts).unwrap(),
            1
        );
        assert!(
            storage
                .device_identity()
                .unwrap()
                .public_id
                .starts_with("device-")
        );
    }

    #[test]
    fn repeated_migration_is_a_noop() {
        let storage = Storage::open_in_memory().unwrap();
        storage.run_migrations().unwrap();
        storage.run_migrations().unwrap();

        assert_eq!(
            storage.row_count(StorageTable::SchemaMigrations).unwrap(),
            1
        );
    }

    #[test]
    fn failed_migration_rolls_back_schema_and_version_row() {
        let mut connection = Connection::open_in_memory().unwrap();
        configure_connection(&connection, false).unwrap();
        let migrations = [Migration {
            version: 1,
            name: "0001_broken.sql",
            sql: "CREATE TABLE should_rollback(id INTEGER); THIS IS NOT SQL;",
        }];

        let error = apply_migrations(&mut connection, &migrations).unwrap_err();
        assert!(matches!(
            error,
            StorageError::MigrationFailed { version: 1, .. }
        ));
        let table_exists: bool = connection
            .query_row(
                "SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE type='table' AND name='should_rollback')",
                [],
                |row| row.get(0),
            )
            .unwrap();
        let migration_rows: i64 = connection
            .query_row("SELECT COUNT(*) FROM schema_migrations", [], |row| {
                row.get(0)
            })
            .unwrap();
        assert!(!table_exists);
        assert_eq!(migration_rows, 0);
    }

    #[test]
    fn file_database_keeps_one_random_device_identity_across_reopen() {
        let directory = tempdir().unwrap();
        let path = directory.path().join("tokenometer.sqlite3");
        let first_id = {
            let storage = Storage::open_file(&path).unwrap();
            storage.device_identity().unwrap().public_id
        };
        let storage = Storage::open_file(&path).unwrap();

        assert_eq!(storage.device_identity().unwrap().public_id, first_id);
        assert_eq!(storage.row_count(StorageTable::Devices).unwrap(), 1);
    }

    #[test]
    fn cursor_upsert_is_optimistic_and_round_trips_json() {
        let storage = Storage::open_in_memory().unwrap();
        let root = storage
            .ensure_codex_source_root(Path::new("C:\\synthetic\\.codex"), Some("dir-1"), false, 10)
            .unwrap();
        let initial = SourceFileCursorWrite {
            source_root_id: root.id,
            logical_file_id: "rollout-1".to_string(),
            logical_session_id: Some("session-1".to_string()),
            canonical_path: Path::new("C:\\synthetic\\.codex\\sessions\\rollout-1.jsonl")
                .to_path_buf(),
            location_kind: SourceFileLocation::Active,
            stable_file_identity: Some("file-1".to_string()),
            identity_degraded: false,
            cursor_generation: 0,
            committed_offset: 10,
            last_known_size: 10,
            last_modified_at_ms: Some(20),
            parser_state_version: 1,
            parser_state: json!({"model":"gpt-5","pendingTools":[]}),
            health: SourceHealthStatus::Healthy,
            last_attempt_at_ms: Some(20),
            last_success_at_ms: Some(20),
            last_checked_at_ms: Some(20),
            last_error_code: None,
            malformed_records: 0,
            oversized_records: 0,
            data_quality_errors: 0,
            expected: CursorExpectation::Missing,
        };
        let CursorUpsertResult::Applied(saved) =
            storage.upsert_source_file_cursor(&initial).unwrap()
        else {
            panic!("initial cursor should be inserted");
        };
        assert_eq!(saved.parser_state, initial.parser_state);
        assert_eq!(saved.parser_state_hash.len(), 64);

        let mut next = initial.clone();
        next.cursor_generation = 1;
        next.committed_offset = 25;
        next.last_known_size = 25;
        next.parser_state = json!({"model":"gpt-5","pendingTools":["call-1"]});
        next.expected = CursorExpectation::Match {
            cursor_generation: saved.cursor_generation,
            committed_offset: saved.committed_offset,
            parser_state_hash: saved.parser_state_hash.clone(),
        };
        let CursorUpsertResult::Applied(updated) =
            storage.upsert_source_file_cursor(&next).unwrap()
        else {
            panic!("matching cursor should update");
        };
        assert_eq!(updated.committed_offset, 25);

        let mut stale = next;
        stale.committed_offset = 30;
        stale.expected = CursorExpectation::Match {
            cursor_generation: saved.cursor_generation,
            committed_offset: updated.committed_offset,
            parser_state_hash: updated.parser_state_hash.clone(),
        };
        let CursorUpsertResult::Conflict(Some(current)) =
            storage.upsert_source_file_cursor(&stale).unwrap()
        else {
            panic!("stale cursor should conflict");
        };
        assert_eq!(current.committed_offset, 25);
    }

    fn digest(byte: u8) -> String {
        format!("{byte:02x}").repeat(32)
    }

    fn synthetic_parsed_batch() -> ParsedBatch {
        let state = ParserStateV1 {
            logical_session_id: Some("session-commit".to_string()),
            session_started_at_ms: Some(10),
            last_activity_at_ms: Some(30),
            model: Some("gpt-new".to_string()),
            effort: Some("high".to_string()),
            workspace_path: Some("C:/private/project".to_string()),
            current_turn_key: Some("prompt/1".to_string()),
            current_turn_started_at_ms: Some(20),
            prompt_index: 1,
            ..ParserStateV1::default()
        };
        let counts = TokenCounts {
            input_total: 10,
            cache_read_input: Some(8),
            cache_write_input: None,
            output_total: 5,
            reasoning_output: Some(2),
            reported_total: Some(15),
            invalid_breakdown: false,
        };
        ParsedBatch {
            next_state: state,
            committed_offset: 100,
            session: Some(SessionSnapshotFact {
                session_id: "session-commit".to_string(),
                originator: Some("synthetic".to_string()),
                cli_version: Some("1.0".to_string()),
                workspace_path: Some("C:/private/project".to_string()),
                safe_project_label: Some("project".to_string()),
                started_at_ms: Some(10),
                updated_at_ms: Some(30),
                model: Some("gpt-new".to_string()),
                effort: Some("high".to_string()),
                transcript_status: TranscriptStatus::Unknown,
            }),
            turns: vec![TurnFact {
                turn_key: "prompt/1".to_string(),
                prompt_index: 1,
                model: Some("gpt-new".to_string()),
                effort: Some("high".to_string()),
                workspace_path: Some("C:/private/project".to_string()),
                started_at_ms: Some(20),
                updated_at_ms: Some(20),
            }],
            usage: vec![
                UsageFact {
                    source_record_key: digest(1),
                    source_payload_hash: digest(11),
                    session_id: "session-commit".to_string(),
                    turn_key: Some("prompt/1".to_string()),
                    metric_scope: MetricScope::SessionCumulativeDelta,
                    cumulative_generation: 0,
                    effective_at_ms: Some(30),
                    observed_at_ms: 31,
                    model: Some("gpt-new".to_string()),
                    effort: Some("high".to_string()),
                    reconciliation: Reconciliation::Consistent,
                    counts: counts.clone(),
                    source_offset: 50,
                },
                UsageFact {
                    source_record_key: digest(2),
                    source_payload_hash: digest(12),
                    session_id: "session-commit".to_string(),
                    turn_key: Some("prompt/1".to_string()),
                    metric_scope: MetricScope::TurnReported,
                    cumulative_generation: 0,
                    effective_at_ms: Some(30),
                    observed_at_ms: 31,
                    model: Some("gpt-new".to_string()),
                    effort: Some("high".to_string()),
                    reconciliation: Reconciliation::Consistent,
                    counts,
                    source_offset: 50,
                },
            ],
            tool_starts: vec![ToolStartFact {
                source_record_key: digest(3),
                source_payload_hash: digest(13),
                session_id: "session-commit".to_string(),
                turn_key: Some("prompt/1".to_string()),
                call_key: "call-1".to_string(),
                tool_name: "exec_command".to_string(),
                started_at_ms: Some(21),
                source_file_identity: "volume-1:file-1".to_string(),
                input_offset: 21,
                input_length: 5,
            }],
            tool_finishes: vec![ToolFinishFact {
                start_source_record_key: digest(3),
                closed_by_record_key: digest(4),
                ended_at_ms: Some(25),
                output_file_identity: "volume-1:file-1".to_string(),
                output_offset: 26,
                output_length: 4,
            }],
            quotas: vec![QuotaSnapshotFact {
                source_record_key: digest(5),
                source_payload_hash: digest(15),
                session_id: "session-commit".to_string(),
                provider_limit_id: Some("codex".to_string()),
                captured_at_ms: Some(30),
                observed_at_ms: 31,
                plan: Some("plus".to_string()),
                windows: vec![QuotaWindowFact {
                    window_key: "codex/codex/primary".to_string(),
                    window_kind: "primary".to_string(),
                    label: "Primary".to_string(),
                    window_minutes: Some(300),
                    used_percent_micros: Some(9_000_000),
                    resets_at_ms: Some(1_000),
                }],
            }],
        }
    }

    fn commit_cursor(source_root_id: i64, expected: CursorExpectation) -> SourceFileCursorWrite {
        SourceFileCursorWrite {
            source_root_id,
            logical_file_id: "rollout-commit".to_string(),
            logical_session_id: None,
            canonical_path: Path::new("C:\\synthetic\\rollout-commit.jsonl").to_path_buf(),
            location_kind: SourceFileLocation::Active,
            stable_file_identity: Some("file-commit".to_string()),
            identity_degraded: false,
            cursor_generation: 0,
            committed_offset: 0,
            last_known_size: 200,
            last_modified_at_ms: Some(40),
            parser_state_version: 1,
            parser_state: json!({}),
            health: SourceHealthStatus::Healthy,
            last_attempt_at_ms: Some(40),
            last_success_at_ms: Some(40),
            last_checked_at_ms: Some(40),
            last_error_code: None,
            malformed_records: 0,
            oversized_records: 0,
            data_quality_errors: 0,
            expected,
        }
    }

    #[test]
    fn codex_batch_commit_is_idempotent_and_turn_facts_are_not_canonical() {
        let storage = Storage::open_in_memory().unwrap();
        let root = storage
            .ensure_codex_source_root(Path::new("C:\\synthetic\\.codex"), None, true, 1)
            .unwrap();
        let batch = synthetic_parsed_batch();
        let first = storage
            .commit_codex_batch(&commit_cursor(root.id, CursorExpectation::Missing), &batch)
            .unwrap();
        assert!(first.read_model_changed);
        assert_eq!(storage.row_count(StorageTable::UsageEvents).unwrap(), 2);
        assert_eq!(storage.row_count(StorageTable::ToolCalls).unwrap(), 1);
        assert_eq!(storage.row_count(StorageTable::QuotaSnapshots).unwrap(), 1);
        let totals = storage.canonical_usage_totals().unwrap();
        assert_eq!(totals.event_count, 1);
        assert_eq!(totals.normalized_total, 15);
        {
            let connection = storage.lock_connection().unwrap();
            let locator = connection
                .query_row(
                    r#"
                    SELECT input_file_identity, input_offset, input_length,
                           output_file_identity, output_offset, output_length
                    FROM tool_calls WHERE source_record_key = ?1
                    "#,
                    [digest(3)],
                    |row| {
                        Ok((
                            row.get::<_, String>(0)?,
                            row.get::<_, i64>(1)?,
                            row.get::<_, i64>(2)?,
                            row.get::<_, Option<String>>(3)?,
                            row.get::<_, Option<i64>>(4)?,
                            row.get::<_, Option<i64>>(5)?,
                        ))
                    },
                )
                .unwrap();
            assert_eq!(
                locator,
                (
                    "volume-1:file-1".to_string(),
                    21,
                    5,
                    Some("volume-1:file-1".to_string()),
                    Some(26),
                    Some(4),
                )
            );

            let mut columns = connection.prepare("PRAGMA table_info(tool_calls)").unwrap();
            let columns = columns
                .query_map([], |row| row.get::<_, String>(1))
                .unwrap()
                .collect::<Result<Vec<_>, _>>()
                .unwrap();
            assert!(!columns.iter().any(|column| {
                matches!(
                    column.as_str(),
                    "arguments"
                        | "arguments_json"
                        | "input_text"
                        | "input_json"
                        | "output"
                        | "output_text"
                        | "output_json"
                        | "raw_input"
                        | "raw_output"
                )
            }));
        }

        let replay = storage
            .commit_codex_batch(
                &commit_cursor(
                    root.id,
                    CursorExpectation::Match {
                        cursor_generation: first.cursor.cursor_generation,
                        committed_offset: first.cursor.committed_offset,
                        parser_state_hash: first.cursor.parser_state_hash.clone(),
                    },
                ),
                &batch,
            )
            .unwrap();
        assert!(!replay.read_model_changed);
        assert_eq!(storage.row_count(StorageTable::UsageEvents).unwrap(), 2);
        assert_eq!(storage.row_count(StorageTable::Turns).unwrap(), 1);
    }

    #[test]
    fn codex_batch_failure_rolls_back_cursor_and_every_fact() {
        let storage = Storage::open_in_memory().unwrap();
        let root = storage
            .ensure_codex_source_root(Path::new("C:\\synthetic\\.codex"), None, true, 1)
            .unwrap();
        let mut batch = synthetic_parsed_batch();
        let duplicate_window = batch.quotas[0].windows[0].clone();
        batch.quotas[0].windows.push(duplicate_window);

        assert!(
            storage
                .commit_codex_batch(&commit_cursor(root.id, CursorExpectation::Missing), &batch,)
                .is_err()
        );
        assert_eq!(storage.row_count(StorageTable::SourceFiles).unwrap(), 0);
        assert_eq!(storage.row_count(StorageTable::Sessions).unwrap(), 0);
        assert_eq!(storage.row_count(StorageTable::Turns).unwrap(), 0);
        assert_eq!(storage.row_count(StorageTable::UsageEvents).unwrap(), 0);
        assert_eq!(storage.row_count(StorageTable::ToolCalls).unwrap(), 0);
        assert_eq!(storage.row_count(StorageTable::QuotaSnapshots).unwrap(), 0);
    }

    #[test]
    fn payload_identity_conflict_rolls_back_newer_session_and_cursor() {
        let storage = Storage::open_in_memory().unwrap();
        let root = storage
            .ensure_codex_source_root(Path::new("C:\\synthetic\\.codex"), None, true, 1)
            .unwrap();
        let first_batch = synthetic_parsed_batch();
        let first = storage
            .commit_codex_batch(
                &commit_cursor(root.id, CursorExpectation::Missing),
                &first_batch,
            )
            .unwrap();
        let mut conflicting = first_batch;
        conflicting.committed_offset = 120;
        conflicting.next_state.last_activity_at_ms = Some(50);
        conflicting.next_state.model = Some("must-rollback".to_string());
        conflicting.session.as_mut().unwrap().updated_at_ms = Some(50);
        conflicting.session.as_mut().unwrap().model = Some("must-rollback".to_string());
        conflicting.usage[0].source_payload_hash = digest(99);
        let error = storage
            .commit_codex_batch(
                &commit_cursor(
                    root.id,
                    CursorExpectation::Match {
                        cursor_generation: first.cursor.cursor_generation,
                        committed_offset: first.cursor.committed_offset,
                        parser_state_hash: first.cursor.parser_state_hash.clone(),
                    },
                ),
                &conflicting,
            )
            .unwrap_err();
        assert!(matches!(error, StorageError::RecordIdentityConflict { .. }));
        let cursor = storage
            .load_source_file_cursor(root.id, "rollout-commit")
            .unwrap()
            .unwrap();
        assert_eq!(cursor.committed_offset, first.cursor.committed_offset);
        let latest_model: String = storage
            .lock_connection()
            .unwrap()
            .query_row(
                "SELECT latest_model FROM sessions WHERE session_id = 'session-commit'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(latest_model, "gpt-new");
    }

    #[test]
    fn older_session_replay_cannot_overwrite_newer_fields() {
        let storage = Storage::open_in_memory().unwrap();
        let root = storage
            .ensure_codex_source_root(Path::new("C:\\synthetic\\.codex"), None, true, 1)
            .unwrap();
        let mut newer = synthetic_parsed_batch();
        newer.session.as_mut().unwrap().updated_at_ms = Some(100);
        newer.session.as_mut().unwrap().model = Some("gpt-newest".to_string());
        newer.next_state.last_activity_at_ms = Some(100);
        newer.next_state.model = Some("gpt-newest".to_string());
        storage
            .commit_codex_batch(&commit_cursor(root.id, CursorExpectation::Missing), &newer)
            .unwrap();

        let mut older = synthetic_parsed_batch();
        older.committed_offset = 50;
        older.session.as_mut().unwrap().updated_at_ms = Some(50);
        older.session.as_mut().unwrap().model = Some("gpt-old".to_string());
        older.next_state.last_activity_at_ms = Some(50);
        older.next_state.model = Some("gpt-old".to_string());
        older.turns.clear();
        older.usage.clear();
        older.tool_starts.clear();
        older.tool_finishes.clear();
        older.quotas.clear();
        let mut second_cursor = commit_cursor(root.id, CursorExpectation::Missing);
        second_cursor.logical_file_id = "rollout-older".to_string();
        second_cursor.canonical_path =
            Path::new("C:\\synthetic\\rollout-older.jsonl").to_path_buf();
        second_cursor.stable_file_identity = Some("file-older".to_string());
        let outcome = storage.commit_codex_batch(&second_cursor, &older).unwrap();
        assert!(!outcome.read_model_changed);
        let (latest_model, updated_at): (String, i64) = storage
            .lock_connection()
            .unwrap()
            .query_row(
                "SELECT latest_model, updated_at_ms FROM sessions WHERE session_id = 'session-commit'",
                [],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .unwrap();
        assert_eq!(latest_model, "gpt-newest");
        assert_eq!(updated_at, 100);
    }
}
