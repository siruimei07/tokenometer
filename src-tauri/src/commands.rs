use tauri::State;

use crate::{
    domain::{AcceptedRevision, BootstrapView, RefreshScope},
    error::AppError,
    state::AppState,
};

#[tauri::command]
pub async fn get_bootstrap(state: State<'_, AppState>) -> Result<BootstrapView, AppError> {
    let state = state.inner().clone();
    tauri::async_runtime::spawn_blocking(move || state.bootstrap())
        .await
        .map_err(|_| AppError::storage_unavailable())?
        .map_err(|_| AppError::storage_unavailable())
}

#[tauri::command]
pub fn refresh_now(
    scope: RefreshScope,
    state: State<'_, AppState>,
) -> Result<AcceptedRevision, AppError> {
    match scope {
        RefreshScope::Codex => {
            let accepted = state.request_refresh();
            if !accepted {
                return Err(AppError::collector_unavailable());
            }
            Ok(AcceptedRevision {
                history_revision: state.history_revision(),
                accepted,
            })
        }
    }
}
