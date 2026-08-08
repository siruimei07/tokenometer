use std::sync::{
    Arc, RwLock,
    atomic::{AtomicBool, AtomicU64, Ordering},
};

use thiserror::Error;
use tokio::sync::mpsc;

use crate::{
    collector::{CodexIngestor, CollectorError, IngestReport, ScanReason},
    domain::{
        AgentProvider, BootstrapView, ProviderCapability, ProviderCapabilityView,
        RuntimeHealthView, RuntimeKind, RuntimeStatus,
    },
    platform::{now_ms, reporting_time_zone},
    storage::{Storage, StorageError},
};

#[derive(Debug, Error)]
pub enum StateError {
    #[error("application state lock is poisoned")]
    LockPoisoned,
    #[error("history revision overflowed")]
    RevisionOverflow,
    #[error("storage snapshot is unavailable")]
    Storage(#[from] StorageError),
    #[error("Codex ingestion failed")]
    Collector(#[from] CollectorError),
}

#[derive(Debug)]
pub struct TickPublication {
    pub report: IngestReport,
    pub history_revision: Option<u64>,
}

#[derive(Clone)]
pub struct AppState {
    storage: Arc<Storage>,
    ingestor: Arc<CodexIngestor>,
    history_revision: Arc<AtomicU64>,
    live_revision: Arc<AtomicU64>,
    snapshot_gate: Arc<RwLock<()>>,
    runtime_health: Arc<RwLock<Vec<RuntimeHealthView>>>,
    refresh_tx: mpsc::Sender<ScanReason>,
    quitting: Arc<AtomicBool>,
}

impl AppState {
    pub fn new(
        storage: Arc<Storage>,
        ingestor: Arc<CodexIngestor>,
        refresh_tx: mpsc::Sender<ScanReason>,
    ) -> Self {
        let updated_at = now_ms();
        Self {
            storage,
            ingestor,
            history_revision: Arc::new(AtomicU64::new(0)),
            live_revision: Arc::new(AtomicU64::new(0)),
            snapshot_gate: Arc::new(RwLock::new(())),
            runtime_health: Arc::new(RwLock::new(vec![
                RuntimeHealthView {
                    runtime: RuntimeKind::Usage,
                    status: RuntimeStatus::Starting,
                    updated_at,
                    message: None,
                },
                RuntimeHealthView {
                    runtime: RuntimeKind::Live,
                    status: RuntimeStatus::NotStarted,
                    updated_at,
                    message: None,
                },
                RuntimeHealthView {
                    runtime: RuntimeKind::Limits,
                    status: RuntimeStatus::NotStarted,
                    updated_at,
                    message: None,
                },
            ])),
            refresh_tx,
            quitting: Arc::new(AtomicBool::new(false)),
        }
    }

    pub fn bootstrap(&self) -> Result<BootstrapView, StateError> {
        let _snapshot = self
            .snapshot_gate
            .read()
            .map_err(|_| StateError::LockPoisoned)?;
        let storage = self.storage.bootstrap_data()?;
        let runtime_health = self
            .runtime_health
            .read()
            .map_err(|_| StateError::LockPoisoned)?
            .clone();
        Ok(BootstrapView {
            history_revision: self.history_revision.load(Ordering::Acquire),
            live_revision: self.live_revision.load(Ordering::Acquire),
            device_id: storage.device_id,
            reporting_time_zone: reporting_time_zone(),
            implemented_capabilities: vec![ProviderCapabilityView {
                provider: AgentProvider::Codex,
                capabilities: vec![
                    ProviderCapability::Usage,
                    ProviderCapability::Sessions,
                    ProviderCapability::Context,
                    ProviderCapability::Quota,
                ],
            }],
            runtime_health,
        })
    }

    pub fn run_tick(&self, reason: ScanReason) -> Result<TickPublication, StateError> {
        let _snapshot = self
            .snapshot_gate
            .write()
            .map_err(|_| StateError::LockPoisoned)?;
        let report = self.ingestor.run_tick(reason)?;
        self.set_usage_health(
            if matches!(
                report.health,
                crate::domain::SourceHealthStatus::Healthy
                    | crate::domain::SourceHealthStatus::Never
            ) {
                RuntimeStatus::Healthy
            } else {
                RuntimeStatus::Degraded
            },
            report.error_code.map(str::to_string),
        )?;
        let history_revision = if report.history_changed {
            Some(self.next_history_revision()?)
        } else {
            None
        };
        Ok(TickPublication {
            report,
            history_revision,
        })
    }

    pub fn record_tick_failure(&self, code: &'static str) -> Result<(), StateError> {
        let _snapshot = self
            .snapshot_gate
            .write()
            .map_err(|_| StateError::LockPoisoned)?;
        self.set_usage_health(RuntimeStatus::Degraded, Some(code.to_string()))
    }

    pub fn request_refresh(&self) -> bool {
        match self.refresh_tx.try_send(ScanReason::Manual) {
            Ok(()) | Err(mpsc::error::TrySendError::Full(_)) => true,
            Err(mpsc::error::TrySendError::Closed(_)) => false,
        }
    }

    pub fn history_revision(&self) -> u64 {
        self.history_revision.load(Ordering::Acquire)
    }

    pub fn begin_quit(&self) {
        self.quitting.store(true, Ordering::Release);
    }

    pub fn is_quitting(&self) -> bool {
        self.quitting.load(Ordering::Acquire)
    }

    fn next_history_revision(&self) -> Result<u64, StateError> {
        self.history_revision
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |revision| {
                revision.checked_add(1)
            })
            .map(|previous| previous + 1)
            .map_err(|_| StateError::RevisionOverflow)
    }

    fn set_usage_health(
        &self,
        status: RuntimeStatus,
        message: Option<String>,
    ) -> Result<(), StateError> {
        let mut health = self
            .runtime_health
            .write()
            .map_err(|_| StateError::LockPoisoned)?;
        if let Some(usage) = health
            .iter_mut()
            .find(|entry| entry.runtime == RuntimeKind::Usage)
        {
            usage.status = status;
            usage.updated_at = now_ms();
            usage.message = message;
        }
        Ok(())
    }
}
