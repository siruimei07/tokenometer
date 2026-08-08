#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tokenometer
{
    class Database;

    enum class SurfaceTheme : uint8_t
    {
        System,
        Dark,
        Light,
    };

    enum class SurfaceLayoutPreset : uint8_t
    {
        LiveUsage,
        ProviderLimits,
        CostFocus,
        Custom,
    };

    enum class SurfaceTool : uint8_t
    {
        Codex,
        ChatGpt,
    };

    enum class SurfaceLayoutItemKind : uint8_t
    {
        ToolIcon,
        QuotaBar,
        Percentage,
        ResetTime,
        Cost,
        CustomText,
    };

    enum class SurfaceQuotaWindow : uint8_t
    {
        Nearest,
        FiveHour,
        Weekly,
    };

    enum class SurfaceFontStyle : uint8_t
    {
        System,
        Mono,
        Emphasis,
    };

    enum class OverviewModule : uint8_t
    {
        TokenSummary,
        TokenActivity,
        CodexLimits,
        ActivityHeatmap,
        RecentSessions,
    };

    struct SurfaceLayoutItem
    {
        SurfaceLayoutItemKind kind{ SurfaceLayoutItemKind::ToolIcon };
        SurfaceTool tool{ SurfaceTool::Codex };
        std::wstring accountLabel{ L"当前帐户" };
        SurfaceQuotaWindow quotaWindow{ SurfaceQuotaWindow::Nearest };
        SurfaceFontStyle font{ SurfaceFontStyle::System };
        std::wstring customText;

        bool operator==(SurfaceLayoutItem const&) const = default;
    };

    struct SurfaceToolPreference
    {
        SurfaceTool tool{ SurfaceTool::Codex };
        bool visible{ true };
        bool pinned{};

        bool operator==(SurfaceToolPreference const&) const = default;
    };

    struct OverviewModulePreference
    {
        OverviewModule module{ OverviewModule::TokenSummary };
        bool visible{ true };

        bool operator==(OverviewModulePreference const&) const = default;
    };

    struct SurfacePreferences
    {
        static constexpr size_t MaxLayoutItems = 6;
        static constexpr size_t MaxToolEntries = 16;

        bool launchToTray{};
        bool closeToTray{ true };
        SurfaceTheme theme{ SurfaceTheme::System };
        int glassOpacityPercent{ 75 };
        bool blurEnabled{ false };
        bool transparentWindow{};
        bool providerColors{ true };
        bool bubbleAlwaysOnTop{ true };
        bool hoverPreview{ true };
        bool hasBubblePosition{};
        int bubbleX{};
        int bubbleY{};
        SurfaceLayoutPreset layoutPreset{ SurfaceLayoutPreset::LiveUsage };
        std::vector<SurfaceLayoutItem> customLayout;
        std::vector<SurfaceToolPreference> tools{
            { SurfaceTool::Codex, true, true },
            { SurfaceTool::ChatGpt, true, false },
        };
        std::vector<OverviewModulePreference> overviewModules{
            { OverviewModule::TokenSummary, true },
            { OverviewModule::TokenActivity, true },
            { OverviewModule::CodexLimits, true },
            { OverviewModule::ActivityHeatmap, true },
            { OverviewModule::RecentSessions, true },
        };

        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] std::wstring Serialize() const;
        [[nodiscard]] static std::optional<SurfacePreferences> Deserialize(
            std::wstring_view value) noexcept;
        [[nodiscard]] static SurfacePreferences Load(Database& database);
        void Save(Database& database) const;

        [[nodiscard]] static bool SelfTest();

        bool operator==(SurfacePreferences const&) const = default;
    };
}
