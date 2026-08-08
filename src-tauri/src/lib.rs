pub mod collector;
pub mod commands;
pub mod domain;
pub mod error;
pub mod platform;
pub mod privacy;
pub mod state;
pub mod storage;

use std::{error::Error, io, sync::Arc, time::Duration};

use tauri::{
    AppHandle, Emitter, Manager, WebviewUrl, WebviewWindowBuilder,
    menu::{Menu, MenuItem},
    tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent},
};
use tokio::sync::mpsc;

use crate::{
    collector::{CodexIngestor, CollectorError, ScanReason},
    domain::{HistoryDomain, HistoryRevisionEvent},
    platform::{prepare_private_app_dir, validate_private_data_file},
    state::{AppState, StateError},
    storage::Storage,
};

const HISTORY_REVISION_EVENT: &str = "tokenometer://history-revision";

pub fn run() {
    let builder = tauri::Builder::default()
        // This plugin must be registered first so no second desktop instance can
        // initialize storage, polling, or a competing tray icon.
        .plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| {
            show_main_window(app);
        }))
        .invoke_handler(tauri::generate_handler![
            commands::get_bootstrap,
            commands::refresh_now
        ])
        .setup(initialize_desktop)
        .on_window_event(|window, event| {
            if window.label() != "main" {
                return;
            }
            if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                let state = window.state::<AppState>();
                if !state.is_quitting() {
                    api.prevent_close();
                    let _ = window.hide();
                }
            }
        });

    if builder.run(tauri::generate_context!()).is_err() {
        eprintln!("Tokenometer desktop runtime failed");
    }
}

fn initialize_desktop(app: &mut tauri::App) -> Result<(), Box<dyn Error>> {
    let data_dir = app.path().app_local_data_dir()?;
    let data_dir = prepare_private_app_dir(&data_dir)?;
    let database_path = data_dir.join("tokenometer.sqlite3");
    validate_private_data_file(&database_path)?;
    let storage = Arc::new(Storage::open_file(&database_path)?);
    validate_private_data_file(&database_path)?;
    let ingestor = Arc::new(CodexIngestor::new(storage.clone()));
    let (refresh_tx, refresh_rx) = mpsc::channel(1);
    let state = AppState::new(storage, ingestor, refresh_tx);
    app.manage(state.clone());

    create_tray(app)?;
    WebviewWindowBuilder::new(app, "main", WebviewUrl::App("index.html".into()))
        .title("Tokenometer")
        .inner_size(1280.0, 800.0)
        .min_inner_size(1024.0, 640.0)
        .on_navigation(|url| {
            url.scheme() == "tauri"
                || (url.host_str() == Some("tauri.localhost")
                    && matches!(url.scheme(), "http" | "https"))
                || (cfg!(debug_assertions)
                    && url
                        .host_str()
                        .is_some_and(|host| matches!(host, "localhost" | "127.0.0.1" | "[::1]")))
        })
        .on_new_window(|_, _| tauri::webview::NewWindowResponse::Deny)
        .build()?;

    spawn_usage_runtime(app.handle().clone(), state, refresh_rx);
    Ok(())
}

fn create_tray(app: &tauri::App) -> Result<(), Box<dyn Error>> {
    let open = MenuItem::with_id(app, "open", "打开 Tokenometer", true, None::<&str>)?;
    let refresh = MenuItem::with_id(app, "refresh", "刷新 Codex 数据", true, None::<&str>)?;
    let quit = MenuItem::with_id(app, "quit", "退出", true, None::<&str>)?;
    let open_id = open.id().clone();
    let refresh_id = refresh.id().clone();
    let quit_id = quit.id().clone();
    let menu = Menu::with_items(app, &[&open, &refresh, &quit])?;
    let icon = app
        .default_window_icon()
        .cloned()
        .ok_or_else(|| io::Error::other("the bundled application icon is missing"))?;

    TrayIconBuilder::with_id("main")
        .icon(icon)
        .tooltip("Tokenometer")
        .menu(&menu)
        .show_menu_on_left_click(false)
        .on_menu_event(move |app, event| {
            if event.id == open_id {
                show_main_window(app);
            } else if event.id == refresh_id {
                let state = app.state::<AppState>();
                let _ = state.request_refresh();
            } else if event.id == quit_id {
                let state = app.state::<AppState>().inner().clone();
                state.begin_quit();
                let app = app.clone();
                tauri::async_runtime::spawn(async move {
                    let _ =
                        tauri::async_runtime::spawn_blocking(move || state.wait_for_idle()).await;
                    app.exit(0);
                });
            }
        })
        .on_tray_icon_event(|tray, event| {
            if matches!(
                event,
                TrayIconEvent::Click {
                    button: MouseButton::Left,
                    button_state: MouseButtonState::Up,
                    ..
                } | TrayIconEvent::DoubleClick {
                    button: MouseButton::Left,
                    ..
                }
            ) {
                show_main_window(tray.app_handle());
            }
        })
        .build(app)?;
    Ok(())
}

fn show_main_window(app: &AppHandle) {
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.unminimize();
        let _ = window.show();
        let _ = window.set_focus();
    }
}

fn spawn_usage_runtime(
    app: AppHandle,
    state: AppState,
    mut refresh_rx: mpsc::Receiver<ScanReason>,
) {
    tauri::async_runtime::spawn(async move {
        publish_tick(&app, &state, ScanReason::Startup).await;
        let mut poll = tokio::time::interval(Duration::from_secs(2));
        let mut discovery = tokio::time::interval(Duration::from_secs(60));
        poll.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        discovery.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        poll.tick().await;
        discovery.tick().await;

        loop {
            if state.is_quitting() {
                break;
            }
            tokio::select! {
                _ = poll.tick() => publish_tick(&app, &state, ScanReason::Poll).await,
                _ = discovery.tick() => publish_tick(&app, &state, ScanReason::Discovery).await,
                request = refresh_rx.recv() => {
                    let Some(reason) = request else { break; };
                    publish_tick(&app, &state, reason).await;
                }
            }
        }
    });
}

async fn publish_tick(app: &AppHandle, state: &AppState, reason: ScanReason) {
    if state.is_quitting() {
        return;
    }
    let blocking_state = state.clone();
    match tauri::async_runtime::spawn_blocking(move || blocking_state.run_tick(reason)).await {
        Ok(Ok(publication)) => {
            if let Some(history_revision) = publication.history_revision {
                let _ = app.emit(
                    HISTORY_REVISION_EVENT,
                    HistoryRevisionEvent {
                        history_revision,
                        domains: vec![
                            HistoryDomain::Usage,
                            HistoryDomain::Sessions,
                            HistoryDomain::Sources,
                            HistoryDomain::Quotas,
                        ],
                    },
                );
            }
        }
        Ok(Err(StateError::Collector(CollectorError::Busy))) => {}
        Ok(Err(error)) => {
            let _ = state.record_tick_failure(safe_state_error_code(&error));
        }
        Err(_) => {
            let _ = state.record_tick_failure("runtime.workerFailed");
        }
    }
}

fn safe_state_error_code(error: &StateError) -> &'static str {
    match error {
        StateError::Storage(_) => "runtime.storageUnavailable",
        StateError::Collector(_) => "runtime.collectorUnavailable",
        StateError::LockPoisoned => "runtime.stateUnavailable",
        StateError::RevisionOverflow => "runtime.revisionOverflow",
    }
}
