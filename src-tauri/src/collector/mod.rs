pub mod codex;
pub mod jsonl;

use std::{
    collections::BTreeMap,
    io::{BufRead, BufReader, Read},
    path::{Path, PathBuf},
    sync::{Arc, Mutex, MutexGuard, TryLockError},
};

use serde_json::Value;
use thiserror::Error;

use crate::{
    domain::SourceHealthStatus,
    platform::{
        CodexFileKind, DiscoveredCodexFile, PlatformError, discover_codex_files, file_identity,
        metadata_modified_ms, now_ms, open_allowed_file, resolve_codex_root,
    },
    privacy::strict_identifier,
    storage::{
        CursorExpectation, SourceFileCursor, SourceFileCursorWrite, SourceFileLocation,
        SourceRootHealthUpdate, SourceRootIdentity, Storage, StorageError,
    },
};

use self::{
    codex::{KeyContext, PARSER_STATE_VERSION, ParseError, ParserStateV1, parse_batch},
    jsonl::{JsonlError, JsonlLimits, read_batch},
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ScanReason {
    Startup,
    Poll,
    Discovery,
    Manual,
}

impl ScanReason {
    fn discovers(self) -> bool {
        !matches!(self, Self::Poll)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct IngestReport {
    pub source_id: Option<String>,
    pub health: SourceHealthStatus,
    pub files_seen: usize,
    pub files_processed: usize,
    pub failed_files: usize,
    pub history_changed: bool,
    pub error_code: Option<&'static str>,
}

impl IngestReport {
    fn unavailable(history_changed: bool) -> Self {
        Self {
            source_id: None,
            health: SourceHealthStatus::Stale,
            files_seen: 0,
            files_processed: 0,
            failed_files: 0,
            history_changed,
            error_code: Some("codex.rootUnavailable"),
        }
    }
}

#[derive(Debug, Error)]
pub enum CollectorError {
    #[error("a Codex collection tick is already running")]
    Busy,
    #[error("collector state lock is poisoned")]
    LockPoisoned,
    #[error("the Codex source trust boundary rejected a path")]
    Platform(#[from] PlatformError),
    #[error("the Codex source could not be read")]
    Jsonl(#[from] JsonlError),
    #[error("the Codex transcript could not be reconciled")]
    Parse(#[from] ParseError),
    #[error("the Codex read model could not be committed")]
    Storage(#[from] StorageError),
    #[error("collector accounting overflowed")]
    ArithmeticOverflow,
    #[error("a source file changed identity while it was being read")]
    FileChangedDuringRead,
}

#[derive(Debug, Clone)]
struct KnownSource {
    root: PathBuf,
    identity: SourceRootIdentity,
    files: Vec<DiscoveredCodexFile>,
}

pub struct CodexIngestor {
    storage: Arc<Storage>,
    limits: JsonlLimits,
    lane: Mutex<()>,
    known: Mutex<Option<KnownSource>>,
}

impl CodexIngestor {
    pub fn new(storage: Arc<Storage>) -> Self {
        Self::with_limits(storage, JsonlLimits::default())
    }

    pub fn with_limits(storage: Arc<Storage>, limits: JsonlLimits) -> Self {
        Self {
            storage,
            limits,
            lane: Mutex::new(()),
            known: Mutex::new(None),
        }
    }

    pub fn run_tick(&self, reason: ScanReason) -> Result<IngestReport, CollectorError> {
        let _lane = self.enter_lane(reason)?;
        let now = now_ms();
        let root = match resolve_codex_root() {
            Ok(root) => root,
            Err(PlatformError::RootUnavailable) => {
                let changed = self.mark_known_root_stale(now)?;
                return Ok(IngestReport::unavailable(changed));
            }
            Err(error) => return Err(error.into()),
        };
        let (source, discovery_changed) = self.load_known_source(&root, reason.discovers(), now)?;
        let device = self.storage.device_identity()?;
        let previous_health = self
            .storage
            .list_source_root_health()?
            .into_iter()
            .find(|health| health.source_id == source.identity.public_id);

        let mut history_changed = discovery_changed;
        let mut files_processed = 0usize;
        let mut failed_files = 0usize;
        for discovered in &source.files {
            match self.scan_file(&source, discovered, &device.public_id, now) {
                Ok(outcome) => {
                    files_processed = files_processed
                        .checked_add(usize::from(outcome.processed))
                        .ok_or(CollectorError::ArithmeticOverflow)?;
                    history_changed |= outcome.history_changed;
                }
                Err(error) => {
                    failed_files = failed_files
                        .checked_add(1)
                        .ok_or(CollectorError::ArithmeticOverflow)?;
                    let changed =
                        self.mark_file_failure(&source, discovered, now, safe_error_code(&error))?;
                    history_changed |= changed;
                }
            }
        }

        let aggregate = self.aggregate_source_health(&source)?;
        let root_update = SourceRootHealthUpdate {
            health: aggregate.health,
            last_attempt_at_ms: aggregate.last_attempt_at_ms,
            last_success_at_ms: aggregate.last_success_at_ms,
            last_error_code: aggregate.last_error_code.clone(),
            affected_file_count: aggregate.affected_file_count,
            malformed_records: aggregate.malformed_records,
            oversized_records: aggregate.oversized_records,
            data_quality_errors: aggregate.data_quality_errors,
            updated_at_ms: now,
        };
        let root_visible_changed = previous_health.as_ref().is_none_or(|previous| {
            previous.health != root_update.health
                || previous.last_attempt_at_ms != root_update.last_attempt_at_ms
                || previous.last_success_at_ms != root_update.last_success_at_ms
                || previous.last_error_code != root_update.last_error_code
                || previous.affected_file_count != root_update.affected_file_count
                || previous.malformed_records != root_update.malformed_records
                || previous.oversized_records != root_update.oversized_records
                || previous.data_quality_errors != root_update.data_quality_errors
        });
        if root_visible_changed {
            self.storage
                .update_source_root_health(source.identity.id, &root_update)?;
            history_changed = true;
        }

        Ok(IngestReport {
            source_id: Some(source.identity.public_id),
            health: aggregate.health,
            files_seen: source.files.len(),
            files_processed,
            failed_files,
            history_changed,
            error_code: aggregate.error_code,
        })
    }

    fn enter_lane(&self, reason: ScanReason) -> Result<MutexGuard<'_, ()>, CollectorError> {
        if matches!(reason, ScanReason::Poll) {
            match self.lane.try_lock() {
                Ok(guard) => Ok(guard),
                Err(TryLockError::WouldBlock) => Err(CollectorError::Busy),
                Err(TryLockError::Poisoned(_)) => Err(CollectorError::LockPoisoned),
            }
        } else {
            self.lane.lock().map_err(|_| CollectorError::LockPoisoned)
        }
    }

    fn load_known_source(
        &self,
        root: &Path,
        discover: bool,
        now: i64,
    ) -> Result<(KnownSource, bool), CollectorError> {
        let mut known = self
            .known
            .lock()
            .map_err(|_| CollectorError::LockPoisoned)?;
        let needs_discovery = discover
            || known
                .as_ref()
                .is_none_or(|source| source.root != root || source.files.is_empty());
        if !needs_discovery {
            return known
                .clone()
                .map(|source| (source, false))
                .ok_or(CollectorError::LockPoisoned);
        }

        let identity = self
            .storage
            .ensure_codex_source_root(root, None, false, now)?;
        let mut files = discover_codex_files(root)?;
        resolve_logical_session_ids(root, &mut files);
        let mut discovery_changed = false;
        if let Some(previous) = known.as_ref().filter(|previous| previous.root == root) {
            let current: BTreeMap<&str, ()> = files
                .iter()
                .map(|file| (file.logical_file_id.as_str(), ()))
                .collect();
            for removed in previous
                .files
                .iter()
                .filter(|file| !current.contains_key(file.logical_file_id.as_str()))
            {
                discovery_changed |= self.mark_removed_file(&identity, removed, now)?;
            }
        }
        let source = KnownSource {
            root: root.to_path_buf(),
            identity,
            files,
        };
        *known = Some(source.clone());
        Ok((source, discovery_changed))
    }

    fn mark_known_root_stale(&self, now: i64) -> Result<bool, CollectorError> {
        let known = self
            .known
            .lock()
            .map_err(|_| CollectorError::LockPoisoned)?;
        let Some(source) = known.as_ref() else {
            return Ok(false);
        };
        let previous = self
            .storage
            .list_source_root_health()?
            .into_iter()
            .find(|health| health.source_id == source.identity.public_id);
        if previous.as_ref().is_some_and(|health| {
            health.health == SourceHealthStatus::Stale
                && health.last_error_code.as_deref() == Some("codex.rootUnavailable")
        }) {
            return Ok(false);
        }
        let update = SourceRootHealthUpdate {
            health: SourceHealthStatus::Stale,
            last_attempt_at_ms: Some(now),
            last_success_at_ms: previous
                .as_ref()
                .and_then(|health| health.last_success_at_ms),
            last_error_code: Some("codex.rootUnavailable".to_string()),
            affected_file_count: previous
                .as_ref()
                .map_or(0, |health| health.affected_file_count),
            malformed_records: previous
                .as_ref()
                .map_or(0, |health| health.malformed_records),
            oversized_records: previous
                .as_ref()
                .map_or(0, |health| health.oversized_records),
            data_quality_errors: previous
                .as_ref()
                .map_or(0, |health| health.data_quality_errors),
            updated_at_ms: now,
        };
        self.storage
            .update_source_root_health(source.identity.id, &update)?;
        Ok(true)
    }

    fn scan_file(
        &self,
        source: &KnownSource,
        discovered: &DiscoveredCodexFile,
        device_public_id: &str,
        now: i64,
    ) -> Result<FileOutcome, CollectorError> {
        let opened = open_allowed_file(&source.root, &discovered.path)?;
        let canonical = opened.canonical_path;
        let mut file = opened.file;
        let before = file.metadata()?;
        let identity = opened.identity;
        let existing = self
            .storage
            .load_source_file_cursor(source.identity.id, &discovered.logical_file_id)?;
        let identity_changed = existing.as_ref().is_some_and(|cursor| {
            cursor
                .stable_file_identity
                .as_ref()
                .is_some_and(|previous| previous != &identity.value)
        });
        let version_changed = existing
            .as_ref()
            .is_some_and(|cursor| i64::from(cursor.parser_state_version) != PARSER_STATE_VERSION);
        let shrunk = existing
            .as_ref()
            .is_some_and(|cursor| before.len() < cursor.committed_offset);
        let state_hash_invalid = existing
            .as_ref()
            .is_some_and(|cursor| !cursor.parser_state_hash_valid);
        let reset = identity_changed || version_changed || shrunk || state_hash_invalid;
        let state = if reset {
            ParserStateV1::default()
        } else {
            existing
                .as_ref()
                .and_then(|cursor| serde_json::from_value(cursor.parser_state.clone()).ok())
                .unwrap_or_default()
        };
        let state_invalid = existing.is_some()
            && !reset
            && serde_json::from_value::<ParserStateV1>(
                existing
                    .as_ref()
                    .map_or(Value::Null, |cursor| cursor.parser_state.clone()),
            )
            .is_err();
        let reset = reset || state_invalid;
        let state = if state_invalid {
            ParserStateV1::default()
        } else {
            state
        };
        let start_offset = if reset {
            0
        } else {
            existing
                .as_ref()
                .map_or(0, |cursor| cursor.committed_offset)
        };
        let location = location(discovered.kind);
        let modified = metadata_modified_ms(&before);

        if !reset
            && existing.as_ref().is_some_and(|cursor| {
                cursor.committed_offset == before.len()
                    && cursor.last_known_size == before.len()
                    && cursor.last_modified_at_ms == modified
                    && cursor.location_kind == location
                    && cursor.stable_file_identity.as_deref() == Some(identity.value.as_str())
            })
        {
            return Ok(FileOutcome::default());
        }

        let batch = read_batch(
            &mut file,
            start_offset,
            state.discarding_oversized_line,
            self.limits,
        )?;
        let after = file.metadata()?;
        let after_identity = file_identity(&file, &canonical)?;
        if identity.value != after_identity.value || after.len() < batch.read_through {
            return Err(CollectorError::FileChangedDuringRead);
        }

        let key_context = KeyContext {
            device_public_id: device_public_id.to_string(),
            source_root_public_id: source.identity.public_id.clone(),
            logical_file_id: discovered.logical_file_id.clone(),
            source_file_identity: identity.value.clone(),
        };
        let parsed = parse_batch(batch, state, &key_context, now)?;
        let counters_are_clean = parsed.next_state.malformed_records == 0
            && parsed.next_state.oversized_records == 0
            && parsed.next_state.data_quality_errors == 0;
        let health = if counters_are_clean && !identity.degraded {
            SourceHealthStatus::Healthy
        } else {
            SourceHealthStatus::Partial
        };
        let cursor_generation = match existing.as_ref() {
            Some(cursor) => cursor
                .cursor_generation
                .checked_add(1)
                .ok_or(CollectorError::ArithmeticOverflow)?,
            None => 1,
        };
        let expectation = existing
            .as_ref()
            .map_or(CursorExpectation::Missing, |cursor| {
                CursorExpectation::Match {
                    cursor_generation: cursor.cursor_generation,
                    committed_offset: cursor.committed_offset,
                    parser_state_hash: cursor.parser_state_hash.clone(),
                }
            });
        let write = SourceFileCursorWrite {
            source_root_id: source.identity.id,
            logical_file_id: discovered.logical_file_id.clone(),
            logical_session_id: parsed.next_state.logical_session_id.clone(),
            canonical_path: canonical,
            location_kind: location,
            stable_file_identity: Some(identity.value),
            identity_degraded: identity.degraded,
            cursor_generation,
            committed_offset: parsed.committed_offset,
            last_known_size: after.len(),
            last_modified_at_ms: metadata_modified_ms(&after),
            parser_state_version: u32::try_from(PARSER_STATE_VERSION)
                .map_err(|_| CollectorError::ArithmeticOverflow)?,
            parser_state: serde_json::to_value(&parsed.next_state).map_err(StorageError::from)?,
            health,
            last_attempt_at_ms: Some(now),
            last_success_at_ms: Some(now),
            last_checked_at_ms: Some(now),
            last_error_code: None,
            malformed_records: parsed.next_state.malformed_records,
            oversized_records: parsed.next_state.oversized_records,
            data_quality_errors: parsed.next_state.data_quality_errors,
            expected: expectation,
        };
        let made_progress = existing.as_ref().is_none_or(|cursor| {
            reset
                || cursor.committed_offset != parsed.committed_offset
                || cursor.health != health
                || cursor.location_kind != location
        });
        let outcome = self.storage.commit_codex_batch(&write, &parsed)?;
        Ok(FileOutcome {
            processed: made_progress,
            history_changed: outcome.read_model_changed,
        })
    }

    fn mark_file_failure(
        &self,
        source: &KnownSource,
        discovered: &DiscoveredCodexFile,
        now: i64,
        error_code: &'static str,
    ) -> Result<bool, CollectorError> {
        let existing = self
            .storage
            .load_source_file_cursor(source.identity.id, &discovered.logical_file_id)?;
        if existing.as_ref().is_some_and(|cursor| {
            cursor.health == SourceHealthStatus::Failed
                && cursor.last_error_code.as_deref() == Some(error_code)
        }) {
            return Ok(false);
        }
        let write = failure_cursor(source, discovered, existing.as_ref(), now, error_code)?;
        match self.storage.upsert_source_file_cursor(&write)? {
            crate::storage::CursorUpsertResult::Applied(_) => Ok(true),
            crate::storage::CursorUpsertResult::Conflict(_) => Ok(false),
        }
    }

    fn mark_removed_file(
        &self,
        identity: &SourceRootIdentity,
        discovered: &DiscoveredCodexFile,
        now: i64,
    ) -> Result<bool, CollectorError> {
        let Some(cursor) = self
            .storage
            .load_source_file_cursor(identity.id, &discovered.logical_file_id)?
        else {
            return Ok(false);
        };
        if cursor.health == SourceHealthStatus::Stale
            && cursor.last_error_code.as_deref() == Some("codex.sourceRemoved")
        {
            return Ok(false);
        }
        let generation = cursor
            .cursor_generation
            .checked_add(1)
            .ok_or(CollectorError::ArithmeticOverflow)?;
        let mut write = cursor_write_from_existing(&cursor, discovered.path.clone(), generation);
        write.health = SourceHealthStatus::Stale;
        write.last_attempt_at_ms = Some(now);
        write.last_checked_at_ms = Some(now);
        write.last_error_code = Some("codex.sourceRemoved".to_string());
        match self.storage.upsert_source_file_cursor(&write)? {
            crate::storage::CursorUpsertResult::Applied(_) => Ok(true),
            crate::storage::CursorUpsertResult::Conflict(_) => Ok(false),
        }
    }

    fn aggregate_source_health(
        &self,
        source: &KnownSource,
    ) -> Result<AggregateHealth, CollectorError> {
        let mut aggregate = AggregateHealth::default();
        for cursor in self.storage.list_source_file_cursors(source.identity.id)? {
            aggregate.observe(&cursor)?;
        }
        aggregate.finish();
        Ok(aggregate)
    }
}

#[derive(Debug, Default)]
struct FileOutcome {
    processed: bool,
    history_changed: bool,
}

#[derive(Debug)]
struct AggregateHealth {
    health: SourceHealthStatus,
    last_attempt_at_ms: Option<i64>,
    last_success_at_ms: Option<i64>,
    last_error_code: Option<String>,
    affected_file_count: u64,
    malformed_records: u64,
    oversized_records: u64,
    data_quality_errors: u64,
    seen: u64,
    healthy: u64,
    failed: u64,
    stale: u64,
    error_code: Option<&'static str>,
}

impl Default for AggregateHealth {
    fn default() -> Self {
        Self {
            health: SourceHealthStatus::Never,
            last_attempt_at_ms: None,
            last_success_at_ms: None,
            last_error_code: None,
            affected_file_count: 0,
            malformed_records: 0,
            oversized_records: 0,
            data_quality_errors: 0,
            seen: 0,
            healthy: 0,
            failed: 0,
            stale: 0,
            error_code: None,
        }
    }
}

impl AggregateHealth {
    fn observe(&mut self, cursor: &SourceFileCursor) -> Result<(), CollectorError> {
        self.seen = self
            .seen
            .checked_add(1)
            .ok_or(CollectorError::ArithmeticOverflow)?;
        self.last_attempt_at_ms = max_optional(self.last_attempt_at_ms, cursor.last_attempt_at_ms);
        self.last_success_at_ms = max_optional(self.last_success_at_ms, cursor.last_success_at_ms);
        self.malformed_records = self
            .malformed_records
            .checked_add(cursor.malformed_records)
            .ok_or(CollectorError::ArithmeticOverflow)?;
        self.oversized_records = self
            .oversized_records
            .checked_add(cursor.oversized_records)
            .ok_or(CollectorError::ArithmeticOverflow)?;
        self.data_quality_errors = self
            .data_quality_errors
            .checked_add(cursor.data_quality_errors)
            .ok_or(CollectorError::ArithmeticOverflow)?;
        if cursor.health == SourceHealthStatus::Healthy {
            self.healthy = self
                .healthy
                .checked_add(1)
                .ok_or(CollectorError::ArithmeticOverflow)?;
        } else {
            self.affected_file_count = self
                .affected_file_count
                .checked_add(1)
                .ok_or(CollectorError::ArithmeticOverflow)?;
            if let Some(code) = cursor.last_error_code.as_ref() {
                self.last_error_code = Some(code.clone());
            }
            if cursor.health == SourceHealthStatus::Failed {
                self.failed = self
                    .failed
                    .checked_add(1)
                    .ok_or(CollectorError::ArithmeticOverflow)?;
            } else if cursor.health == SourceHealthStatus::Stale {
                self.stale = self
                    .stale
                    .checked_add(1)
                    .ok_or(CollectorError::ArithmeticOverflow)?;
            }
        }
        Ok(())
    }

    fn finish(&mut self) {
        self.health = if self.seen == 0 {
            SourceHealthStatus::Never
        } else if self.affected_file_count == 0 {
            SourceHealthStatus::Healthy
        } else if self.stale == self.seen {
            SourceHealthStatus::Stale
        } else if self.failed == self.seen {
            SourceHealthStatus::Failed
        } else {
            SourceHealthStatus::Partial
        };
        self.error_code = match self.health {
            SourceHealthStatus::Failed => Some("codex.allFilesFailed"),
            SourceHealthStatus::Stale => Some("codex.sourcesStale"),
            SourceHealthStatus::Partial => Some("codex.someFilesPartial"),
            _ => None,
        };
    }
}

fn failure_cursor(
    source: &KnownSource,
    discovered: &DiscoveredCodexFile,
    existing: Option<&SourceFileCursor>,
    now: i64,
    error_code: &'static str,
) -> Result<SourceFileCursorWrite, CollectorError> {
    if let Some(cursor) = existing {
        let generation = cursor
            .cursor_generation
            .checked_add(1)
            .ok_or(CollectorError::ArithmeticOverflow)?;
        let mut write = cursor_write_from_existing(cursor, discovered.path.clone(), generation);
        write.location_kind = location(discovered.kind);
        write.health = SourceHealthStatus::Failed;
        write.last_attempt_at_ms = Some(now);
        write.last_checked_at_ms = Some(now);
        write.last_error_code = Some(error_code.to_string());
        return Ok(write);
    }
    Ok(SourceFileCursorWrite {
        source_root_id: source.identity.id,
        logical_file_id: discovered.logical_file_id.clone(),
        logical_session_id: None,
        canonical_path: discovered.path.clone(),
        location_kind: location(discovered.kind),
        stable_file_identity: None,
        identity_degraded: true,
        cursor_generation: 1,
        committed_offset: 0,
        last_known_size: 0,
        last_modified_at_ms: None,
        parser_state_version: u32::try_from(PARSER_STATE_VERSION)
            .map_err(|_| CollectorError::ArithmeticOverflow)?,
        parser_state: serde_json::to_value(ParserStateV1::default()).map_err(StorageError::from)?,
        health: SourceHealthStatus::Failed,
        last_attempt_at_ms: Some(now),
        last_success_at_ms: None,
        last_checked_at_ms: Some(now),
        last_error_code: Some(error_code.to_string()),
        malformed_records: 0,
        oversized_records: 0,
        data_quality_errors: 0,
        expected: CursorExpectation::Missing,
    })
}

fn cursor_write_from_existing(
    cursor: &SourceFileCursor,
    path: PathBuf,
    generation: u64,
) -> SourceFileCursorWrite {
    SourceFileCursorWrite {
        source_root_id: cursor.source_root_id,
        logical_file_id: cursor.logical_file_id.clone(),
        logical_session_id: cursor.logical_session_id.clone(),
        canonical_path: path,
        location_kind: cursor.location_kind,
        stable_file_identity: cursor.stable_file_identity.clone(),
        identity_degraded: cursor.identity_degraded,
        cursor_generation: generation,
        committed_offset: cursor.committed_offset,
        last_known_size: cursor.last_known_size,
        last_modified_at_ms: cursor.last_modified_at_ms,
        parser_state_version: cursor.parser_state_version,
        parser_state: cursor.parser_state.clone(),
        health: cursor.health,
        last_attempt_at_ms: cursor.last_attempt_at_ms,
        last_success_at_ms: cursor.last_success_at_ms,
        last_checked_at_ms: cursor.last_checked_at_ms,
        last_error_code: cursor.last_error_code.clone(),
        malformed_records: cursor.malformed_records,
        oversized_records: cursor.oversized_records,
        data_quality_errors: cursor.data_quality_errors,
        expected: CursorExpectation::Match {
            cursor_generation: cursor.cursor_generation,
            committed_offset: cursor.committed_offset,
            parser_state_hash: cursor.parser_state_hash.clone(),
        },
    }
}

fn location(kind: CodexFileKind) -> SourceFileLocation {
    match kind {
        CodexFileKind::Active => SourceFileLocation::Active,
        CodexFileKind::Archive => SourceFileLocation::Archive,
        CodexFileKind::Index => SourceFileLocation::Index,
    }
}

fn resolve_logical_session_ids(root: &Path, files: &mut Vec<DiscoveredCodexFile>) {
    for discovered in files
        .iter_mut()
        .filter(|file| file.kind != CodexFileKind::Index)
    {
        if let Some(session_id) = probe_session_id(root, &discovered.path) {
            discovered.logical_file_id = format!("rollout/{session_id}");
        }
    }
    files.sort_by(|left, right| {
        left.logical_file_id
            .cmp(&right.logical_file_id)
            .then_with(|| file_priority(left.kind).cmp(&file_priority(right.kind)))
            .then_with(|| left.path.cmp(&right.path))
    });
    files.dedup_by(|left, right| left.logical_file_id == right.logical_file_id);
}

fn probe_session_id(root: &Path, path: &Path) -> Option<String> {
    const PROBE_BYTES: u64 = 1024 * 1024;
    let file = open_allowed_file(root, path).ok()?.file;
    let mut reader = BufReader::with_capacity(64 * 1024, file.take(PROBE_BYTES));
    let mut line = Vec::new();
    loop {
        let read = reader.read_until(b'\n', &mut line).ok()?;
        if read == 0 {
            break;
        }
        if line.len() > usize::try_from(PROBE_BYTES).ok()? {
            return None;
        }
        if let Ok(value) = serde_json::from_slice::<Value>(&line)
            && value.get("type").and_then(Value::as_str) == Some("session_meta")
        {
            return value
                .get("payload")
                .and_then(|payload| payload.get("id"))
                .and_then(Value::as_str)
                .and_then(|id| strict_identifier(id, 256));
        }
        line.clear();
    }
    None
}

fn file_priority(kind: CodexFileKind) -> u8 {
    match kind {
        CodexFileKind::Active => 0,
        CodexFileKind::Archive => 1,
        CodexFileKind::Index => 2,
    }
}

fn safe_error_code(error: &CollectorError) -> &'static str {
    match error {
        CollectorError::Platform(PlatformError::ReparsePoint) => "codex.reparseRejected",
        CollectorError::Platform(PlatformError::PathOutsideRoot) => "codex.pathOutsideRoot",
        CollectorError::Platform(PlatformError::PathNotAllowed) => "codex.pathNotAllowed",
        CollectorError::Platform(_) => "codex.fileUnavailable",
        CollectorError::Jsonl(JsonlError::OffsetBeyondEof) => "codex.fileShrank",
        CollectorError::Jsonl(_) => "codex.readFailed",
        CollectorError::Parse(ParseError::SessionIdentityConflict) => "codex.sessionConflict",
        CollectorError::Parse(ParseError::MissingSessionMetadata) => "codex.metadataMissing",
        CollectorError::Parse(_) => "codex.parseFailed",
        CollectorError::Storage(StorageError::RecordIdentityConflict { .. }) => {
            "codex.recordIdentityConflict"
        }
        CollectorError::Storage(_) => "codex.commitFailed",
        CollectorError::FileChangedDuringRead => "codex.fileChangedDuringRead",
        CollectorError::Busy => "codex.tickBusy",
        CollectorError::LockPoisoned | CollectorError::ArithmeticOverflow => {
            "codex.internalFailure"
        }
    }
}

fn max_optional(left: Option<i64>, right: Option<i64>) -> Option<i64> {
    match (left, right) {
        (Some(left), Some(right)) => Some(left.max(right)),
        (Some(value), None) | (None, Some(value)) => Some(value),
        (None, None) => None,
    }
}

impl From<std::io::Error> for CollectorError {
    fn from(value: std::io::Error) -> Self {
        Self::Platform(PlatformError::from(value))
    }
}

#[cfg(test)]
mod tests {
    use std::{
        env, fs,
        io::Write,
        sync::{Arc, Mutex},
    };

    use tempfile::tempdir;

    use super::{CodexIngestor, ScanReason};
    use crate::domain::SourceHealthStatus;
    use crate::platform::resolve_codex_root;
    use crate::storage::{SourceRootHealthUpdate, Storage, StorageTable};

    static CODEX_HOME_ENV_LOCK: Mutex<()> = Mutex::new(());

    #[test]
    fn first_repeat_append_and_archive_keep_one_canonical_total() {
        let _env_guard = CODEX_HOME_ENV_LOCK.lock().unwrap();
        let root = tempdir().unwrap();
        let active = root.path().join("sessions/2026/08/08");
        let archive = root.path().join("archived_sessions");
        fs::create_dir_all(&active).unwrap();
        fs::create_dir_all(&archive).unwrap();
        let file = active.join("rollout-synthetic.jsonl");
        fs::write(
            &file,
            concat!(
                "{\"type\":\"session_meta\",\"timestamp\":\"2026-08-08T00:00:00Z\",\"payload\":{\"id\":\"session-synthetic\",\"cwd\":\"C:/synthetic/project\"}}\n",
                "{\"type\":\"event_msg\",\"timestamp\":\"2026-08-08T00:00:01Z\",\"payload\":{\"type\":\"user_message\",\"message\":\"PRIVATE_PROMPT_SENTINEL\"}}\n",
                "{\"type\":\"response_item\",\"timestamp\":\"2026-08-08T00:00:02Z\",\"payload\":{\"type\":\"function_call\",\"name\":\"exec_command\",\"call_id\":\"call-synthetic\",\"arguments\":\"SECRET_ARGUMENT_SENTINEL\"}}\n",
                "{\"type\":\"response_item\",\"timestamp\":\"2026-08-08T00:00:03Z\",\"payload\":{\"type\":\"function_call_output\",\"call_id\":\"call-synthetic\",\"output\":\"SECRET_OUTPUT_SENTINEL\"}}\n",
                "{\"type\":\"event_msg\",\"timestamp\":\"2026-08-08T00:00:04Z\",\"payload\":{\"type\":\"token_count\",\"info\":{\"total_token_usage\":{\"input_tokens\":10,\"cached_input_tokens\":4,\"output_tokens\":5,\"total_tokens\":15},\"last_token_usage\":{\"input_tokens\":10,\"cached_input_tokens\":4,\"output_tokens\":5,\"total_tokens\":15}}}}\n"
            ),
        )
        .unwrap();
        let previous = env::var_os("CODEX_HOME");
        unsafe { env::set_var("CODEX_HOME", root.path()) };

        let database_directory = tempdir().unwrap();
        let database_path = database_directory.path().join("tokenometer.sqlite3");
        let storage = Arc::new(Storage::open_file(&database_path).unwrap());
        let ingestor = CodexIngestor::new(storage.clone());
        let first = ingestor.run_tick(ScanReason::Startup).unwrap();
        assert!(first.history_changed);
        assert_eq!(
            storage.canonical_usage_totals().unwrap().normalized_total,
            15
        );
        let repeated = ingestor.run_tick(ScanReason::Manual).unwrap();
        assert!(!repeated.history_changed);
        assert_eq!(
            storage.canonical_usage_totals().unwrap().normalized_total,
            15
        );

        // Simulate interruption after a file cursor commit but before the root aggregate was
        // persisted. A known-file fast-path tick must repair the root without reprocessing data.
        let canonical_root = resolve_codex_root().unwrap();
        let identity = storage
            .ensure_codex_source_root(&canonical_root, None, false, 1)
            .unwrap();
        let healthy_root = storage
            .list_source_root_health()
            .unwrap()
            .into_iter()
            .find(|health| health.source_id == identity.public_id)
            .unwrap();
        assert!(
            storage
                .update_source_root_health(
                    identity.id,
                    &SourceRootHealthUpdate {
                        health: SourceHealthStatus::Stale,
                        last_attempt_at_ms: healthy_root.last_attempt_at_ms,
                        last_success_at_ms: healthy_root.last_success_at_ms,
                        last_error_code: Some("synthetic.interruptedAggregate".to_string()),
                        affected_file_count: 1,
                        malformed_records: healthy_root.malformed_records,
                        oversized_records: healthy_root.oversized_records,
                        data_quality_errors: healthy_root.data_quality_errors,
                        updated_at_ms: healthy_root.updated_at_ms + 1,
                    },
                )
                .unwrap()
        );
        assert_eq!(
            storage
                .list_source_root_health()
                .unwrap()
                .into_iter()
                .find(|health| health.source_id == identity.public_id)
                .unwrap()
                .health,
            SourceHealthStatus::Stale
        );
        let repaired = ingestor.run_tick(ScanReason::Poll).unwrap();
        assert_eq!(repaired.files_processed, 0);
        assert_eq!(repaired.health, SourceHealthStatus::Healthy);
        assert!(repaired.history_changed);
        let repaired_root = storage
            .list_source_root_health()
            .unwrap()
            .into_iter()
            .find(|health| health.source_id == identity.public_id)
            .unwrap();
        assert_eq!(repaired_root.health, SourceHealthStatus::Healthy);
        assert_eq!(repaired_root.affected_file_count, 0);
        assert_eq!(repaired_root.last_error_code, None);
        let consistent = ingestor.run_tick(ScanReason::Poll).unwrap();
        assert_eq!(consistent.files_processed, 0);
        assert!(!consistent.history_changed);

        fs::OpenOptions::new()
            .append(true)
            .open(&file)
            .unwrap()
            .write_all(b"{\"type\":\"event_msg\",\"timestamp\":\"2026-08-08T00:00:05Z\",\"payload\":{\"type\":\"token_count\",\"info\":{\"total_token_usage\":{\"input_tokens\":14,\"cached_input_tokens\":5,\"output_tokens\":7,\"total_tokens\":21},\"last_token_usage\":{\"input_tokens\":4,\"cached_input_tokens\":1,\"output_tokens\":2,\"total_tokens\":6}}}}\n")
            .unwrap();
        ingestor.run_tick(ScanReason::Poll).unwrap();
        assert_eq!(
            storage.canonical_usage_totals().unwrap().normalized_total,
            21
        );

        drop(ingestor);
        drop(storage);
        let storage = Arc::new(Storage::open_file(&database_path).unwrap());
        let ingestor = CodexIngestor::new(storage.clone());
        let restarted = ingestor.run_tick(ScanReason::Startup).unwrap();
        assert!(!restarted.history_changed);
        assert_eq!(
            storage.canonical_usage_totals().unwrap().normalized_total,
            21
        );

        let archived = archive.join("rollout-renamed-synthetic.jsonl");
        fs::rename(&file, &archived).unwrap();
        ingestor.run_tick(ScanReason::Discovery).unwrap();
        assert_eq!(
            storage.canonical_usage_totals().unwrap().normalized_total,
            21
        );
        assert_eq!(storage.row_count(StorageTable::UsageEvents).unwrap(), 4);

        let broken = active.join("rollout-broken.jsonl");
        fs::write(
            &broken,
            concat!(
                "{not-json}\n",
                "{\"type\":\"event_msg\",\"payload\":{\"type\":\"token_count\",\"info\":{\"total_token_usage\":{\"input_tokens\":999,\"output_tokens\":999}}}}\n"
            ),
        )
        .unwrap();
        let isolated = ingestor.run_tick(ScanReason::Discovery).unwrap();
        assert_eq!(isolated.failed_files, 1);
        assert_eq!(isolated.health, SourceHealthStatus::Partial);
        assert_eq!(
            storage.canonical_usage_totals().unwrap().normalized_total,
            21
        );

        fs::remove_file(&archived).unwrap();
        fs::remove_file(&broken).unwrap();
        let removed = ingestor.run_tick(ScanReason::Discovery).unwrap();
        assert!(removed.history_changed);
        assert_eq!(removed.health, SourceHealthStatus::Stale);
        assert_eq!(
            storage.canonical_usage_totals().unwrap().normalized_total,
            21
        );

        drop(ingestor);
        drop(storage);
        let mut persisted = fs::read(&database_path).unwrap();
        let wal_path = database_path.with_extension("sqlite3-wal");
        if wal_path.exists() {
            persisted.extend(fs::read(wal_path).unwrap());
        }
        for sentinel in [
            b"PRIVATE_PROMPT_SENTINEL".as_slice(),
            b"SECRET_ARGUMENT_SENTINEL".as_slice(),
            b"SECRET_OUTPUT_SENTINEL".as_slice(),
        ] {
            assert!(
                !persisted
                    .windows(sentinel.len())
                    .any(|window| window == sentinel)
            );
        }

        if let Some(previous) = previous {
            unsafe { env::set_var("CODEX_HOME", previous) };
        } else {
            unsafe { env::remove_var("CODEX_HOME") };
        }
    }

    #[test]
    fn parser_state_hash_mismatch_replays_from_zero_and_heals_cursor() {
        let _env_guard = CODEX_HOME_ENV_LOCK.lock().unwrap();
        let root = tempdir().unwrap();
        let active = root.path().join("sessions/2026/08/08");
        fs::create_dir_all(&active).unwrap();
        fs::write(
            active.join("rollout-hash-replay.jsonl"),
            concat!(
                "{\"type\":\"session_meta\",\"timestamp\":\"2026-08-08T00:00:00Z\",\"payload\":{\"id\":\"session-hash-replay\"}}\n",
                "{\"type\":\"event_msg\",\"timestamp\":\"2026-08-08T00:00:01Z\",\"payload\":{\"type\":\"token_count\",\"info\":{\"total_token_usage\":{\"input_tokens\":10,\"output_tokens\":5},\"last_token_usage\":{\"input_tokens\":10,\"output_tokens\":5}}}}\n"
            ),
        )
        .unwrap();
        let previous = env::var_os("CODEX_HOME");
        unsafe { env::set_var("CODEX_HOME", root.path()) };

        let storage = Arc::new(Storage::open_in_memory().unwrap());
        let ingestor = CodexIngestor::new(storage.clone());
        ingestor.run_tick(ScanReason::Startup).unwrap();
        let original = storage.list_source_file_cursors(1).unwrap().pop().unwrap();
        let mut tampered_state = original.parser_state.clone();
        tampered_state["cumulativeGeneration"] = serde_json::Value::from(99);
        let tampered_json = serde_json::to_string(&tampered_state).unwrap();
        storage
            .lock_connection()
            .unwrap()
            .execute(
                "UPDATE source_files SET parser_state_json = ?1 WHERE id = ?2",
                rusqlite::params![tampered_json, original.id],
            )
            .unwrap();

        let replayed = ingestor.run_tick(ScanReason::Manual).unwrap();
        let healed = storage.list_source_file_cursors(1).unwrap().pop().unwrap();

        assert_eq!(replayed.files_processed, 1);
        assert_eq!(healed.cursor_generation, original.cursor_generation + 1);
        assert_eq!(healed.parser_state, original.parser_state);
        assert_eq!(storage.row_count(StorageTable::UsageEvents).unwrap(), 2);
        assert_eq!(
            storage.canonical_usage_totals().unwrap().normalized_total,
            15
        );

        if let Some(previous) = previous {
            unsafe { env::set_var("CODEX_HOME", previous) };
        } else {
            unsafe { env::remove_var("CODEX_HOME") };
        }
    }
}
