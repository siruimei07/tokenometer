CREATE TABLE app_settings (
    key TEXT PRIMARY KEY,
    value_json TEXT NOT NULL CHECK (json_valid(value_json)),
    value_version INTEGER NOT NULL CHECK (value_version > 0),
    updated_at_ms INTEGER NOT NULL CHECK (updated_at_ms >= 0)
);

CREATE TABLE devices (
    id INTEGER PRIMARY KEY,
    public_id TEXT NOT NULL UNIQUE,
    kind TEXT NOT NULL CHECK (kind = 'windows'),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    last_seen_at_ms INTEGER NOT NULL CHECK (last_seen_at_ms >= 0)
);

CREATE TABLE provider_accounts (
    provider_key TEXT NOT NULL,
    account_id TEXT NOT NULL,
    origin TEXT NOT NULL,
    health TEXT NOT NULL,
    plan TEXT,
    last_observed_at_ms INTEGER CHECK (last_observed_at_ms >= 0),
    PRIMARY KEY (provider_key, account_id)
);

CREATE TABLE source_roots (
    id INTEGER PRIMARY KEY,
    public_id TEXT NOT NULL UNIQUE,
    source_kind TEXT NOT NULL CHECK (source_kind = 'codex'),
    canonical_path TEXT NOT NULL UNIQUE,
    stable_directory_identity TEXT,
    identity_degraded INTEGER NOT NULL DEFAULT 0 CHECK (identity_degraded IN (0, 1)),
    health TEXT NOT NULL CHECK (health IN ('never', 'healthy', 'partial', 'stale', 'failed')),
    last_attempt_at_ms INTEGER CHECK (last_attempt_at_ms >= 0),
    last_success_at_ms INTEGER CHECK (last_success_at_ms >= 0),
    last_error_code TEXT,
    affected_file_count INTEGER NOT NULL DEFAULT 0 CHECK (affected_file_count >= 0),
    malformed_records INTEGER NOT NULL DEFAULT 0 CHECK (malformed_records >= 0),
    oversized_records INTEGER NOT NULL DEFAULT 0 CHECK (oversized_records >= 0),
    data_quality_errors INTEGER NOT NULL DEFAULT 0 CHECK (data_quality_errors >= 0),
    created_at_ms INTEGER NOT NULL CHECK (created_at_ms >= 0),
    updated_at_ms INTEGER NOT NULL CHECK (updated_at_ms >= 0)
);

CREATE TABLE source_files (
    id INTEGER PRIMARY KEY,
    source_root_id INTEGER NOT NULL REFERENCES source_roots(id) ON DELETE RESTRICT,
    logical_file_id TEXT NOT NULL,
    logical_session_id TEXT,
    canonical_path TEXT NOT NULL,
    location_kind TEXT NOT NULL CHECK (location_kind IN ('active', 'archive', 'index')),
    stable_file_identity TEXT,
    identity_degraded INTEGER NOT NULL DEFAULT 0 CHECK (identity_degraded IN (0, 1)),
    cursor_generation INTEGER NOT NULL DEFAULT 0 CHECK (cursor_generation >= 0),
    committed_offset INTEGER NOT NULL DEFAULT 0 CHECK (committed_offset >= 0),
    last_known_size INTEGER NOT NULL DEFAULT 0 CHECK (last_known_size >= 0),
    last_modified_at_ms INTEGER CHECK (last_modified_at_ms >= 0),
    parser_state_version INTEGER NOT NULL CHECK (parser_state_version > 0),
    parser_state_json TEXT NOT NULL CHECK (json_valid(parser_state_json)),
    parser_state_hash TEXT NOT NULL,
    health TEXT NOT NULL CHECK (health IN ('never', 'healthy', 'partial', 'stale', 'failed')),
    last_attempt_at_ms INTEGER CHECK (last_attempt_at_ms >= 0),
    last_success_at_ms INTEGER CHECK (last_success_at_ms >= 0),
    last_checked_at_ms INTEGER CHECK (last_checked_at_ms >= 0),
    last_error_code TEXT,
    malformed_records INTEGER NOT NULL DEFAULT 0 CHECK (malformed_records >= 0),
    oversized_records INTEGER NOT NULL DEFAULT 0 CHECK (oversized_records >= 0),
    data_quality_errors INTEGER NOT NULL DEFAULT 0 CHECK (data_quality_errors >= 0),
    UNIQUE (source_root_id, logical_file_id)
);

CREATE UNIQUE INDEX source_files_stable_identity
    ON source_files(source_root_id, stable_file_identity)
    WHERE stable_file_identity IS NOT NULL AND identity_degraded = 0;
CREATE INDEX source_files_health ON source_files(source_root_id, health);

CREATE TABLE sessions (
    id INTEGER PRIMARY KEY,
    source_root_id INTEGER NOT NULL REFERENCES source_roots(id) ON DELETE RESTRICT,
    device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE RESTRICT,
    provider_key TEXT NOT NULL CHECK (provider_key = 'codex'),
    account_id TEXT NOT NULL DEFAULT 'current',
    session_id TEXT NOT NULL,
    originator TEXT,
    cli_version TEXT,
    workspace_path TEXT,
    safe_project_label TEXT,
    started_at_ms INTEGER CHECK (started_at_ms >= 0),
    updated_at_ms INTEGER CHECK (updated_at_ms >= 0),
    latest_model TEXT,
    latest_effort TEXT,
    transcript_status TEXT NOT NULL DEFAULT 'unknown'
        CHECK (transcript_status IN ('executing', 'thinking', 'unknown')),
    status_confidence TEXT NOT NULL DEFAULT 'low'
        CHECK (status_confidence IN ('high', 'medium', 'low')),
    status_reason TEXT,
    measurement_kind TEXT NOT NULL DEFAULT 'exact' CHECK (measurement_kind = 'exact'),
    UNIQUE (source_root_id, session_id),
    FOREIGN KEY (provider_key, account_id)
        REFERENCES provider_accounts(provider_key, account_id) ON DELETE RESTRICT
);

CREATE INDEX sessions_updated ON sessions(updated_at_ms DESC);

CREATE TABLE turns (
    id INTEGER PRIMARY KEY,
    session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE RESTRICT,
    turn_key TEXT NOT NULL,
    prompt_index INTEGER NOT NULL CHECK (prompt_index >= 0),
    model TEXT,
    effort TEXT,
    workspace_path TEXT,
    started_at_ms INTEGER CHECK (started_at_ms >= 0),
    updated_at_ms INTEGER CHECK (updated_at_ms >= 0),
    latest_input_total INTEGER CHECK (latest_input_total >= 0),
    latest_cache_read_input INTEGER CHECK (latest_cache_read_input >= 0),
    latest_cache_write_input INTEGER CHECK (latest_cache_write_input >= 0),
    latest_output_total INTEGER CHECK (latest_output_total >= 0),
    latest_reasoning_output INTEGER CHECK (latest_reasoning_output >= 0),
    latest_reported_total INTEGER CHECK (latest_reported_total >= 0),
    invalid_breakdown INTEGER NOT NULL DEFAULT 0 CHECK (invalid_breakdown IN (0, 1)),
    UNIQUE (session_id, turn_key),
    UNIQUE (session_id, prompt_index)
);

CREATE TABLE usage_events (
    id INTEGER PRIMARY KEY,
    source_record_key TEXT NOT NULL UNIQUE,
    source_payload_hash TEXT NOT NULL,
    source_file_id INTEGER NOT NULL REFERENCES source_files(id) ON DELETE RESTRICT,
    session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE RESTRICT,
    turn_id INTEGER REFERENCES turns(id) ON DELETE RESTRICT,
    device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE RESTRICT,
    provider_key TEXT NOT NULL CHECK (provider_key = 'codex'),
    account_id TEXT NOT NULL DEFAULT 'current',
    metric_scope TEXT NOT NULL
        CHECK (metric_scope IN ('sessionCumulativeDelta', 'turnReported')),
    cumulative_generation INTEGER NOT NULL CHECK (cumulative_generation >= 0),
    effective_at_ms INTEGER CHECK (effective_at_ms >= 0),
    observed_at_ms INTEGER NOT NULL CHECK (observed_at_ms >= 0),
    model TEXT,
    effort TEXT,
    measurement_kind TEXT NOT NULL CHECK (measurement_kind = 'exact'),
    reconciliation TEXT NOT NULL
        CHECK (reconciliation IN ('consistent', 'mismatch', 'reset', 'missing')),
    input_total INTEGER NOT NULL CHECK (input_total >= 0),
    cache_read_input INTEGER CHECK (cache_read_input >= 0),
    cache_write_input INTEGER CHECK (cache_write_input >= 0),
    output_total INTEGER NOT NULL CHECK (output_total >= 0),
    reasoning_output INTEGER CHECK (reasoning_output >= 0),
    reported_total INTEGER CHECK (reported_total >= 0),
    invalid_breakdown INTEGER NOT NULL CHECK (invalid_breakdown IN (0, 1)),
    source_offset INTEGER NOT NULL CHECK (source_offset >= 0),
    FOREIGN KEY (provider_key, account_id)
        REFERENCES provider_accounts(provider_key, account_id) ON DELETE RESTRICT
);

CREATE INDEX usage_events_canonical_time
    ON usage_events(effective_at_ms)
    WHERE metric_scope = 'sessionCumulativeDelta';
CREATE INDEX usage_events_session_time ON usage_events(session_id, effective_at_ms);
CREATE INDEX usage_events_model_time ON usage_events(model, effective_at_ms);
CREATE INDEX usage_events_source_file ON usage_events(source_file_id);

CREATE TABLE tool_calls (
    id INTEGER PRIMARY KEY,
    source_record_key TEXT NOT NULL UNIQUE,
    source_payload_hash TEXT NOT NULL,
    source_file_id INTEGER NOT NULL REFERENCES source_files(id) ON DELETE RESTRICT,
    session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE RESTRICT,
    turn_id INTEGER REFERENCES turns(id) ON DELETE RESTRICT,
    call_key TEXT NOT NULL,
    tool_name TEXT NOT NULL,
    started_at_ms INTEGER CHECK (started_at_ms >= 0),
    ended_at_ms INTEGER CHECK (ended_at_ms >= 0),
    status TEXT NOT NULL CHECK (status IN ('open', 'closed')),
    closed_by_record_key TEXT UNIQUE,
    input_file_identity TEXT NOT NULL,
    input_offset INTEGER NOT NULL CHECK (input_offset >= 0),
    input_length INTEGER NOT NULL CHECK (input_length >= 0),
    output_file_identity TEXT,
    output_offset INTEGER CHECK (output_offset >= 0),
    output_length INTEGER CHECK (output_length >= 0),
    UNIQUE (session_id, call_key)
);

CREATE INDEX tool_calls_session_time ON tool_calls(session_id, started_at_ms);

CREATE TABLE quota_snapshots (
    id INTEGER PRIMARY KEY,
    source_record_key TEXT NOT NULL UNIQUE,
    source_payload_hash TEXT NOT NULL,
    source_file_id INTEGER NOT NULL REFERENCES source_files(id) ON DELETE RESTRICT,
    session_id INTEGER REFERENCES sessions(id) ON DELETE RESTRICT,
    provider_key TEXT NOT NULL CHECK (provider_key = 'codex'),
    account_id TEXT NOT NULL DEFAULT 'current',
    source_key TEXT NOT NULL CHECK (source_key = 'transcript'),
    provider_limit_id TEXT,
    captured_at_ms INTEGER CHECK (captured_at_ms >= 0),
    observed_at_ms INTEGER NOT NULL CHECK (observed_at_ms >= 0),
    measurement_kind TEXT NOT NULL CHECK (measurement_kind = 'exact'),
    status TEXT NOT NULL,
    plan TEXT,
    FOREIGN KEY (provider_key, account_id)
        REFERENCES provider_accounts(provider_key, account_id) ON DELETE RESTRICT
);

CREATE INDEX quota_snapshots_account_time
    ON quota_snapshots(provider_key, account_id, captured_at_ms DESC);

CREATE TABLE quota_window_values (
    snapshot_id INTEGER NOT NULL REFERENCES quota_snapshots(id) ON DELETE RESTRICT,
    window_key TEXT NOT NULL,
    window_kind TEXT,
    label TEXT,
    window_minutes INTEGER CHECK (window_minutes > 0),
    used_percent_micros INTEGER CHECK (
        used_percent_micros >= 0 AND used_percent_micros <= 100000000
    ),
    resets_at_ms INTEGER CHECK (resets_at_ms >= 0),
    unit TEXT,
    used_amount TEXT,
    limit_amount TEXT,
    remaining_amount TEXT,
    balance TEXT,
    PRIMARY KEY (snapshot_id, window_key)
);
