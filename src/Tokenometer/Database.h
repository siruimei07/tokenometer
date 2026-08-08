#pragma once

#include "UsageModels.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
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
        [[nodiscard]] std::wstring GetOrCreateDeviceId(
            std::wstring_view displayName,
            std::wstring_view stateKey = L"local_device_id");
        [[nodiscard]] std::optional<std::wstring> GetAppState(
            std::wstring_view key);
        void SetAppState(std::wstring_view key, std::wstring_view value);
        [[nodiscard]] bool HasSessionSource(
            std::wstring_view sessionId,
            std::wstring_view sourceKind);

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
        [[nodiscard]] bool IsChatGPTExportCurrent(ChatGPTExportBatch const& batch);
        void ReplaceChatGPTExport(
            ChatGPTExportBatch const& batch,
            std::stop_token stopToken = {});

        [[nodiscard]] UsageTotals GetTotals(int64_t since = 0);
        [[nodiscard]] std::vector<DailyUsage> GetDailyUsage(int days);
        [[nodiscard]] std::vector<HourlyUsage> GetHourlyUsage(int days);
        [[nodiscard]] std::vector<BreakdownRow> GetBreakdown(
            std::wstring_view dimension,
            int64_t since = 0,
            int limit = 20);
        [[nodiscard]] std::vector<SessionSummary> GetRecentSessions(int limit = 8);
        [[nodiscard]] std::optional<SessionSummary> GetSession(SessionRef const& session);
        [[nodiscard]] std::vector<TurnSummary> GetSessionTurns(
            SessionRef const& session,
            int limit = 100);
        [[nodiscard]] std::vector<TurnSummary> GetSessionTurns(
            std::wstring_view sessionId,
            int limit = 100);
        [[nodiscard]] std::vector<ToolCallDetail> GetSessionToolCalls(
            SessionRef const& session,
            int limit = 100);
        [[nodiscard]] std::vector<ToolCallDetail> GetToolCalls(
            std::wstring_view sessionId,
            int promptIndex);
        [[nodiscard]] std::optional<RateLimitSnapshot> GetLatestRateLimit(
            std::wstring_view provider = L"codex",
            std::wstring_view accountId = L"current");
        [[nodiscard]] std::vector<ChatGPTSessionEstimate> GetChatGPTEstimatedSessions(
            std::wstring_view accountId = {},
            int limit = 100);
        [[nodiscard]] std::optional<ChatGPTSessionEstimate> GetChatGPTEstimatedSession(
            SessionRef const& session);
        [[nodiscard]] UsageTotals GetChatGPTEstimatedTotals(
            std::wstring_view accountId = {});
        [[nodiscard]] std::vector<BreakdownRow> GetChatGPTEstimatedBreakdown(
            std::wstring_view dimension,
            int64_t since = 0,
            int limit = 20);
        [[nodiscard]] std::vector<ChatGPTPromptEstimate> GetChatGPTEstimatedPrompts(
            SessionRef const& session,
            int limit = 500);
        [[nodiscard]] std::vector<ChatGPTPromptEstimate> GetChatGPTEstimatedPrompts(
            std::wstring_view accountId,
            std::wstring_view sessionId,
            int limit = 500);
        [[nodiscard]] std::vector<ChatGPTEstimatedDailyUsage> GetChatGPTEstimatedDailyUsage(
            int days,
            std::wstring_view accountId = {});
        [[nodiscard]] std::vector<ChatGPTEstimatedHourlyUsage> GetChatGPTEstimatedHourlyUsage(
            int days,
            std::wstring_view accountId = {});
        [[nodiscard]] std::vector<DeviceSummary> GetDeviceSummaries(int limit = 8);

        void PruneDetails(int usageDays = 180, int toolDays = 180, int hourlyDays = 400);
        [[nodiscard]] bool PruneDetailsIfDue(
            int usageDays = 180,
            int toolDays = 180,
            int hourlyDays = 400);
        void Optimize();

        [[nodiscard]] static bool SelfTest();

    private:
        void HardenStoragePermissions();
        void Execute(char const* sql);
        [[noreturn]] void ThrowDatabaseError(char const* action) const;

        std::filesystem::path m_path;
        sqlite3* m_database{};
        std::recursive_mutex m_mutex;
    };
}
