use tokenometer_lib::domain::{
    AgentProvider, BootstrapView, ProviderCapability, ProviderCapabilityView,
    ReportingTimeZoneSource, ReportingTimeZoneView, RuntimeHealthView, RuntimeKind, RuntimeStatus,
};

#[test]
fn rust_bootstrap_wire_shape_matches_the_shared_frontend_fixture() {
    let actual = BootstrapView {
        history_revision: 7,
        live_revision: 0,
        device_id: "device-synthetic-contract".to_string(),
        reporting_time_zone: ReportingTimeZoneView {
            id: "Synthetic Standard Time".to_string(),
            display_name: "Synthetic Time".to_string(),
            source: ReportingTimeZoneSource::WindowsSystem,
        },
        implemented_capabilities: vec![ProviderCapabilityView {
            provider: AgentProvider::Codex,
            capabilities: vec![
                ProviderCapability::Usage,
                ProviderCapability::Sessions,
                ProviderCapability::Context,
                ProviderCapability::Quota,
            ],
        }],
        runtime_health: vec![
            RuntimeHealthView {
                runtime: RuntimeKind::Usage,
                status: RuntimeStatus::Healthy,
                updated_at: 1_786_147_200_000,
                message: None,
            },
            RuntimeHealthView {
                runtime: RuntimeKind::Live,
                status: RuntimeStatus::NotStarted,
                updated_at: 1_786_147_200_000,
                message: None,
            },
            RuntimeHealthView {
                runtime: RuntimeKind::Limits,
                status: RuntimeStatus::NotStarted,
                updated_at: 1_786_147_200_000,
                message: None,
            },
        ],
    };
    let expected: serde_json::Value =
        serde_json::from_str(include_str!("fixtures/contracts/bootstrap.json")).unwrap();

    assert_eq!(serde_json::to_value(actual).unwrap(), expected);
}
