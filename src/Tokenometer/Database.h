#pragma once

#include "UsageModels.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>

struct sqlite3;

namespace tokenometer
{
    class Database final
    {
    public:
        explicit Database(std::filesystem::path path);
        ~Database();

        Database(Database const&) = delete;
        Database& operator=(Database const&) = delete;

        static std::filesystem::path DefaultDataDirectory();

        void Initialize();
        void Transaction(std::function<void()> const& work);
        [[nodiscard]] std::wstring GetOrCreateDeviceId(std::wstring_view displayName);

        [[nodiscard]] std::optional<SourceProgress> GetSourceProgress(
            std::wstring_view path,
            std::wstring_view fileIdentity = {});
        void SaveSourceProgress(SourceProgress const& progress);
        void ResetSourceFile(std::wstring_view path);

        void UpsertSession(SessionRecord const& session);
        bool InsertUsageEvent(UsageEvent const& event);
        bool InsertToolEvent(ToolEvent const& event);
        bool AttachToolOutput(ToolOutputEvent const& event);
        bool InsertPromptEvent(PromptEvent const& event);
        void UpsertRateLimit(RateLimitSnapshot const& snapshot);

        [[nodiscard]] UsageTotals GetTotals(int64_t since = 0);
        [[nodiscard]] std::vector<DailyUsage> GetDailyUsage(int days);
        [[nodiscard]] std::vector<HourlyUsage> GetHourlyUsage(int days);
        [[nodiscard]] std::vector<BreakdownRow> GetBreakdown(
            std::wstring_view dimension,
            int64_t since = 0,
            int limit = 20);
        [[nodiscard]] std::vector<SessionSummary> GetRecentSessions(int limit = 8);
        [[nodiscard]] std::vector<TurnSummary> GetSessionTurns(
            std::wstring_view sessionId,
            int limit = 100);
        [[nodiscard]] std::vector<ToolCallDetail> GetToolCalls(
            std::wstring_view sessionId,
            int promptIndex);
        [[nodiscard]] std::optional<RateLimitSnapshot> GetLatestRateLimit(
            std::wstring_view provider = L"codex",
            std::wstring_view accountId = L"current");

        void PruneDetails(int usageDays = 180, int toolDays = 180, int hourlyDays = 400);
        [[nodiscard]] bool PruneDetailsIfDue(
            int usageDays = 180,
            int toolDays = 180,
            int hourlyDays = 400);
        void Optimize();

        [[nodiscard]] static bool SelfTest();

    private:
        void Execute(char const* sql);
        [[noreturn]] void ThrowDatabaseError(char const* action) const;

        std::filesystem::path m_path;
        sqlite3* m_database{};
        std::recursive_mutex m_mutex;
    };
}
