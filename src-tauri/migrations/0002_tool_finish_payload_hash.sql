ALTER TABLE tool_calls ADD COLUMN closed_by_payload_hash TEXT
    CHECK (closed_by_payload_hash IS NULL OR length(closed_by_payload_hash) = 64);
