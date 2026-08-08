use serde::Serialize;

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AppError {
    pub code: String,
    pub message: String,
    pub retryable: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub source_id: Option<String>,
}

impl AppError {
    pub fn new(code: impl Into<String>, message: impl Into<String>, retryable: bool) -> Self {
        Self {
            code: code.into(),
            message: message.into(),
            retryable,
            source_id: None,
        }
    }

    pub fn with_source_id(mut self, source_id: impl Into<String>) -> Self {
        self.source_id = Some(source_id.into());
        self
    }

    pub fn storage_unavailable() -> Self {
        Self::new(
            "storage.unavailable",
            "本地数据暂时不可用，请稍后重试。",
            true,
        )
    }

    pub fn collector_unavailable() -> Self {
        Self::new(
            "collector.unavailable",
            "Codex 来源暂时不可读取；已保留上次成功数据。",
            true,
        )
    }
}

#[cfg(test)]
mod tests {
    use super::AppError;

    #[test]
    fn serialized_error_is_bounded_and_has_no_internal_cause() {
        let error = AppError::collector_unavailable().with_source_id("source-public");
        let json = serde_json::to_value(error).unwrap();
        assert_eq!(json["code"], "collector.unavailable");
        assert_eq!(json["sourceId"], "source-public");
        assert!(json.get("cause").is_none());
    }
}
