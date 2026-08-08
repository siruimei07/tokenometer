use std::{
    fs::File,
    io::{self, BufRead, BufReader, Seek, SeekFrom},
};

use thiserror::Error;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct JsonlLimits {
    pub max_batch_bytes: usize,
    pub max_line_bytes: usize,
    pub buffer_bytes: usize,
}

impl Default for JsonlLimits {
    fn default() -> Self {
        Self {
            max_batch_bytes: 4 * 1024 * 1024,
            max_line_bytes: 1024 * 1024,
            buffer_bytes: 64 * 1024,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct JsonlLine {
    pub start_offset: u64,
    pub end_offset: u64,
    pub bytes: Vec<u8>,
    pub terminated_by_newline: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct JsonlBatch {
    pub lines: Vec<JsonlLine>,
    pub committed_through: u64,
    pub read_through: u64,
    pub reached_eof: bool,
    pub incomplete_tail: bool,
    pub oversized_records: u64,
    pub discarding_oversized_line: bool,
}

#[derive(Debug, Error)]
pub enum JsonlError {
    #[error("the persisted cursor is beyond the current file size")]
    OffsetBeyondEof,
    #[error("JSONL limits must be non-zero")]
    InvalidLimits,
    #[error("JSONL cursor accounting overflowed")]
    CounterOverflow,
    #[error("the source file could not be read")]
    Io(#[source] io::Error),
}

impl From<io::Error> for JsonlError {
    fn from(value: io::Error) -> Self {
        Self::Io(value)
    }
}

pub fn read_batch(
    file: &mut File,
    start_offset: u64,
    mut discarding_oversized_line: bool,
    limits: JsonlLimits,
) -> Result<JsonlBatch, JsonlError> {
    if limits.max_batch_bytes == 0 || limits.max_line_bytes == 0 || limits.buffer_bytes == 0 {
        return Err(JsonlError::InvalidLimits);
    }

    let file_size = file.metadata()?.len();
    if start_offset > file_size {
        return Err(JsonlError::OffsetBeyondEof);
    }
    file.seek(SeekFrom::Start(start_offset))?;

    let mut reader = BufReader::with_capacity(limits.buffer_bytes, file);
    let mut lines = Vec::new();
    let mut current_line = Vec::new();
    let mut line_start = start_offset;
    let mut cursor = start_offset;
    let mut committed_through = start_offset;
    let mut consumed = 0usize;
    let mut oversized_records = 0u64;

    while consumed < limits.max_batch_bytes && cursor < file_size {
        let available = reader.fill_buf()?;
        if available.is_empty() {
            break;
        }
        let remaining = limits.max_batch_bytes - consumed;
        let take = available.len().min(remaining);
        let window = &available[..take];

        if discarding_oversized_line {
            if let Some(newline) = window.iter().position(|byte| *byte == b'\n') {
                let advance = newline + 1;
                reader.consume(advance);
                cursor = cursor
                    .checked_add(advance as u64)
                    .ok_or(JsonlError::OffsetBeyondEof)?;
                consumed += advance;
                committed_through = cursor;
                line_start = cursor;
                discarding_oversized_line = false;
            } else {
                reader.consume(take);
                cursor = cursor
                    .checked_add(take as u64)
                    .ok_or(JsonlError::OffsetBeyondEof)?;
                consumed += take;
                committed_through = cursor;
            }
            continue;
        }

        let newline = window.iter().position(|byte| *byte == b'\n');
        let content_len = newline.unwrap_or(window.len());
        let would_be_oversized = current_line
            .len()
            .checked_add(content_len)
            .is_none_or(|length| length > limits.max_line_bytes);

        if would_be_oversized {
            oversized_records = oversized_records
                .checked_add(1)
                .ok_or(JsonlError::CounterOverflow)?;
            current_line.clear();
            if let Some(newline) = newline {
                let advance = newline + 1;
                reader.consume(advance);
                cursor = cursor
                    .checked_add(advance as u64)
                    .ok_or(JsonlError::OffsetBeyondEof)?;
                consumed += advance;
                committed_through = cursor;
                line_start = cursor;
            } else {
                reader.consume(take);
                cursor = cursor
                    .checked_add(take as u64)
                    .ok_or(JsonlError::OffsetBeyondEof)?;
                consumed += take;
                committed_through = cursor;
                discarding_oversized_line = true;
            }
            continue;
        }

        current_line.extend_from_slice(&window[..content_len]);
        if let Some(newline) = newline {
            let advance = newline + 1;
            reader.consume(advance);
            cursor = cursor
                .checked_add(advance as u64)
                .ok_or(JsonlError::OffsetBeyondEof)?;
            consumed += advance;
            if current_line.last() == Some(&b'\r') {
                current_line.pop();
            }
            lines.push(JsonlLine {
                start_offset: line_start,
                end_offset: cursor,
                bytes: std::mem::take(&mut current_line),
                terminated_by_newline: true,
            });
            committed_through = cursor;
            line_start = cursor;
        } else {
            reader.consume(take);
            cursor = cursor
                .checked_add(take as u64)
                .ok_or(JsonlError::OffsetBeyondEof)?;
            consumed += take;
        }
    }

    let reached_eof = cursor == file_size;
    if reached_eof && discarding_oversized_line {
        committed_through = cursor;
    } else if reached_eof && !current_line.is_empty() {
        lines.push(JsonlLine {
            start_offset: line_start,
            end_offset: cursor,
            bytes: current_line,
            terminated_by_newline: false,
        });
    }

    let incomplete_tail = !reached_eof && !discarding_oversized_line && line_start < cursor;
    Ok(JsonlBatch {
        lines,
        committed_through,
        read_through: cursor,
        reached_eof,
        incomplete_tail,
        oversized_records,
        discarding_oversized_line,
    })
}

#[cfg(test)]
mod tests {
    use std::io::{Seek, SeekFrom, Write};

    use tempfile::NamedTempFile;

    use super::{JsonlError, JsonlLimits, read_batch};

    fn limits(batch: usize, line: usize) -> JsonlLimits {
        JsonlLimits {
            max_batch_bytes: batch,
            max_line_bytes: line,
            buffer_bytes: 4,
        }
    }

    #[test]
    fn reads_newline_and_crlf_records_with_byte_offsets() {
        let mut source = NamedTempFile::new().unwrap();
        source.write_all(b"{\"a\":1}\n{\"b\":2}\r\n").unwrap();
        source.as_file_mut().seek(SeekFrom::Start(0)).unwrap();

        let batch = read_batch(source.as_file_mut(), 0, false, limits(128, 64)).unwrap();
        assert_eq!(batch.lines.len(), 2);
        assert_eq!(batch.lines[0].bytes, br#"{"a":1}"#);
        assert_eq!(batch.lines[1].bytes, br#"{"b":2}"#);
        assert_eq!(
            batch.committed_through,
            source.as_file().metadata().unwrap().len()
        );
        assert!(batch.reached_eof);
    }

    #[test]
    fn returns_a_complete_json_candidate_at_eof_without_newline() {
        let mut source = NamedTempFile::new().unwrap();
        source.write_all(br#"{"ok":true}"#).unwrap();
        let size = source.as_file().metadata().unwrap().len();

        let batch = read_batch(source.as_file_mut(), 0, false, limits(128, 64)).unwrap();
        assert_eq!(batch.lines.len(), 1);
        assert!(!batch.lines[0].terminated_by_newline);
        assert_eq!(batch.lines[0].end_offset, size);
        assert_eq!(batch.committed_through, 0);
    }

    #[test]
    fn partial_tail_is_not_committed_or_returned_before_eof_is_reached() {
        let mut source = NamedTempFile::new().unwrap();
        source
            .write_all(b"{\"first\":true}\n{\"second\":true}")
            .unwrap();

        let batch = read_batch(source.as_file_mut(), 0, false, limits(20, 64)).unwrap();
        assert_eq!(batch.lines.len(), 1);
        assert_eq!(batch.committed_through, 15);
        assert!(batch.incomplete_tail);
    }

    #[test]
    fn oversized_records_are_discarded_across_bounded_batches() {
        let mut source = NamedTempFile::new().unwrap();
        source
            .write_all(b"01234567890123456789\n{\"ok\":1}\n")
            .unwrap();

        let first = read_batch(source.as_file_mut(), 0, false, limits(8, 5)).unwrap();
        assert_eq!(first.oversized_records, 1);
        assert!(first.discarding_oversized_line);
        assert_eq!(first.committed_through, 8);

        let second = read_batch(
            source.as_file_mut(),
            first.committed_through,
            first.discarding_oversized_line,
            limits(32, 16),
        )
        .unwrap();
        assert_eq!(second.oversized_records, 0);
        assert!(!second.discarding_oversized_line);
        assert_eq!(second.lines.len(), 1);
        assert_eq!(second.lines[0].bytes, br#"{"ok":1}"#);
    }

    #[test]
    fn oversized_record_discard_state_survives_eof_until_a_newline_arrives() {
        let mut source = NamedTempFile::new().unwrap();
        source.write_all(b"0123456789").unwrap();

        let first = read_batch(source.as_file_mut(), 0, false, limits(32, 5)).unwrap();
        assert_eq!(first.oversized_records, 1);
        assert!(first.discarding_oversized_line);
        assert_eq!(first.committed_through, 10);

        source.as_file_mut().seek(SeekFrom::End(0)).unwrap();
        source.as_file_mut().write_all(b"tail\n{}\n").unwrap();
        let second = read_batch(
            source.as_file_mut(),
            first.committed_through,
            first.discarding_oversized_line,
            limits(32, 5),
        )
        .unwrap();

        assert_eq!(second.oversized_records, 0);
        assert!(!second.discarding_oversized_line);
        assert_eq!(second.lines.len(), 1);
        assert_eq!(second.lines[0].bytes, b"{}");
    }

    #[test]
    fn an_offset_beyond_eof_requests_a_parser_reset() {
        let mut source = NamedTempFile::new().unwrap();
        source.write_all(b"{}").unwrap();

        assert!(matches!(
            read_batch(source.as_file_mut(), 3, false, limits(10, 10)),
            Err(JsonlError::OffsetBeyondEof)
        ));
    }
}
