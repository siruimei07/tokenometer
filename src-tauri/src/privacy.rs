const REDACTED: &str = "[REDACTED]";

pub fn sanitize_bounded(input: &str, max_chars: usize) -> String {
    input
        .chars()
        .filter(|character| !character.is_control() && !is_bidi_control(*character))
        .take(max_chars)
        .collect()
}

pub fn strict_identifier(input: &str, max_chars: usize) -> Option<String> {
    if input.is_empty()
        || input.chars().count() > max_chars
        || input
            .chars()
            .any(|character| character.is_control() || is_bidi_control(character))
    {
        return None;
    }
    Some(input.to_string())
}

pub fn redact_preview(input: &[u8], max_bytes: usize) -> String {
    let bounded = &input[..input.len().min(max_bytes)];
    let sanitized: String = String::from_utf8_lossy(bounded)
        .chars()
        .filter(|character| {
            (!character.is_control() || matches!(character, '\n' | '\r' | '\t'))
                && !is_bidi_control(*character)
        })
        .take(max_bytes)
        .collect();
    sanitized
        .lines()
        .map(redact_line)
        .collect::<Vec<_>>()
        .join("\n")
}

fn redact_line(line: &str) -> String {
    let lower = line.to_ascii_lowercase();
    if let Some(index) = sensitive_assignment_index(&lower) {
        let delimiter = line[index..]
            .find([':', '='])
            .map(|relative| index + relative)
            .unwrap_or(index);
        return format!(
            "{}{} {}",
            &line[..delimiter],
            &line[delimiter..=delimiter],
            REDACTED
        );
    }

    let mut output = Vec::new();
    let mut redact_next = false;
    for token in command_tokens(line) {
        if redact_next {
            output.push(REDACTED.to_string());
            redact_next = false;
            continue;
        }
        let normalized = token
            .trim_matches(|character: char| matches!(character, '"' | '\'' | ',' | ';'))
            .to_ascii_lowercase();
        let sensitive_flag = matches!(
            normalized
                .split_once('=')
                .map_or(normalized.as_str(), |pair| pair.0),
            "--api-key" | "--apikey" | "--token" | "--access-token" | "--secret" | "--password"
        );
        if sensitive_flag && normalized.contains('=') {
            let flag = token.split_once('=').map_or(token.as_str(), |pair| pair.0);
            output.push(format!("{flag}={REDACTED}"));
        } else if sensitive_flag {
            output.push(token);
            redact_next = true;
        } else if looks_like_token(&normalized) {
            output.push(REDACTED.to_string());
        } else {
            output.push(token);
        }
    }
    output.join(" ")
}

fn command_tokens(line: &str) -> Vec<String> {
    let mut tokens = Vec::new();
    let mut current = String::new();
    let mut quote = None;
    let mut escaped = false;

    for character in line.chars() {
        if escaped {
            current.push(character);
            escaped = false;
            continue;
        }
        if quote.is_some() && character == '\\' {
            current.push(character);
            escaped = true;
            continue;
        }
        match quote {
            Some(delimiter) if character == delimiter => {
                current.push(character);
                quote = None;
            }
            Some(_) => current.push(character),
            None if matches!(character, '"' | '\'') => {
                current.push(character);
                quote = Some(character);
            }
            None if character.is_whitespace() => {
                if !current.is_empty() {
                    tokens.push(std::mem::take(&mut current));
                }
            }
            None => current.push(character),
        }
    }
    if !current.is_empty() {
        tokens.push(current);
    }
    tokens
}

fn sensitive_assignment_index(lower: &str) -> Option<usize> {
    const KEYS: &[&str] = &[
        "authorization",
        "proxy-authorization",
        "cookie",
        "set-cookie",
        "api_key",
        "api-key",
        "apikey",
        "access_token",
        "access-token",
        "client_secret",
        "client-secret",
        "password",
    ];
    KEYS.iter()
        .filter_map(|key| {
            lower.match_indices(key).find_map(|(index, _)| {
                let before_is_boundary = index == 0
                    || lower[..index]
                        .chars()
                        .next_back()
                        .is_some_and(|character| !character.is_ascii_alphanumeric());
                let suffix = &lower[index + key.len()..];
                (before_is_boundary && suffix.trim_start().starts_with([':', '='])).then_some(index)
            })
        })
        .min()
}

fn looks_like_token(value: &str) -> bool {
    (value.starts_with("sk-") && value.len() >= 20)
        || (value.starts_with("ghp_") && value.len() >= 20)
        || (value.starts_with("github_pat_") && value.len() >= 24)
        || (value.starts_with("xoxb-") && value.len() >= 20)
        || (value.starts_with("xoxp-") && value.len() >= 20)
}

fn is_bidi_control(character: char) -> bool {
    matches!(
        character,
        '\u{061c}'
            | '\u{200e}'
            | '\u{200f}'
            | '\u{202a}'..='\u{202e}'
            | '\u{2066}'..='\u{2069}'
    )
}

#[cfg(test)]
mod tests {
    use super::{REDACTED, redact_preview, sanitize_bounded, strict_identifier};

    #[test]
    fn strips_bidi_controls_and_bounds_output() {
        assert_eq!(sanitize_bounded("ab\u{202e}cdef", 4), "abcd");
        assert_eq!(sanitize_bounded("a\nb\tc", 8), "abc");
        assert!(strict_identifier("call\nspoof", 64).is_none());
    }

    #[test]
    fn redacts_headers_flags_assignments_and_known_token_shapes() {
        let preview = redact_preview(
            b"Authorization: Bearer synthetic-secret\napi_key=synthetic\ncmd --token value --secret=equals-value\ncookieJar Cookie: cookie-value\nsk-12345678901234567890",
            4096,
        );
        assert!(!preview.contains("synthetic-secret"));
        assert!(!preview.contains("api_key=synthetic"));
        assert!(!preview.contains("--token value"));
        assert!(!preview.contains("equals-value"));
        assert!(!preview.contains("cookie-value"));
        assert!(!preview.contains("sk-12345678901234567890"));
        assert!(preview.matches(REDACTED).count() >= 4);
    }

    #[test]
    fn redacts_quoted_cli_secret_values_as_one_value() {
        let preview = redact_preview(
            br#"cmd --token="secret with spaces" --secret='another secret' done"#,
            4096,
        );

        assert_eq!(preview, "cmd --token=[REDACTED] --secret=[REDACTED] done");
        assert!(!preview.contains("secret with spaces"));
        assert!(!preview.contains("another secret"));
    }
}
