#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tokenometer
{
    struct TokenCounts
    {
        int64_t input{};
        int64_t cachedInput{};
        int64_t cacheWriteInput{};
        int64_t output{};
        int64_t reasoningOutput{};
        int64_t reportedTotal{};

        [[nodiscard]] int64_t UncachedInput() const noexcept
        {
            return input > cachedInput ? input - cachedInput : 0;
        }

        [[nodiscard]] int64_t DisplayTotal() const noexcept
        {
            return reportedTotal > 0 ? reportedTotal : input + output;
        }
    };

    struct SourceProgress
    {
        std::wstring path;
        std::wstring fileIdentity;
        int64_t size{};
        int64_t modifiedAt{};
        int64_t offset{};
        std::wstring sessionId;
        std::wstring project;
        std::wstring model;
        std::wstring turnId;
        int promptIndex{};
        bool trackingStarted{ true };
        bool forked{};
        int64_t sessionCreatedAt{};
        TokenCounts cumulative;
    };

    struct SessionRecord
    {
        std::wstring id;
        std::wstring sourcePath;
        std::wstring sourceKind{ L"codex" };
        std::wstring title;
        std::wstring project;
        std::wstring model;
        std::wstring deviceId;
        int64_t startedAt{};
        int64_t updatedAt{};
        int messageCount{};
        std::wstring accountId{ L"current" };
    };

    struct UsageEvent
    {
        std::wstring sourcePath;
        int64_t sourceOffset{};
        std::wstring sessionId;
        std::wstring sourceKind{ L"codex" };
        std::wstring tool{ L"Codex" };
        std::wstring model;
        std::wstring project;
        std::wstring deviceId;
        std::wstring turnId;
        int promptIndex{};
        int64_t timestamp{};
        std::wstring day;
        TokenCounts counts;
        std::wstring accountId{ L"current" };
    };

    struct ToolEvent
    {
        std::wstring sourcePath;
        int64_t sourceOffset{};
        std::wstring sessionId;
        std::wstring sourceKind{ L"codex" };
        std::wstring tool{ L"Codex" };
        std::wstring model;
        std::wstring project;
        std::wstring deviceId;
        std::wstring turnId;
        int promptIndex{};
        int64_t timestamp{};
        std::wstring day;
        std::wstring name;
        std::wstring callId;
        int64_t inputLength{};
        std::wstring accountId{ L"current" };
    };

    struct ToolOutputEvent
    {
        std::wstring sourcePath;
        std::wstring sessionId;
        std::wstring callId;
        int64_t outputOffset{};
        int64_t outputLength{};
    };

    struct PromptEvent
    {
        std::wstring sourcePath;
        int64_t sourceOffset{};
        std::wstring sessionId;
        std::wstring sourceKind{ L"codex" };
        std::wstring tool{ L"Codex" };
        std::wstring model;
        std::wstring project;
        std::wstring deviceId;
        std::wstring turnId;
        int promptIndex{};
        int64_t timestamp{};
        std::wstring day;
        std::wstring accountId{ L"current" };
    };

    struct RateLimitSnapshot
    {
        std::wstring provider{ L"codex" };
        std::wstring accountId{ L"current" };
        std::wstring limitId;
        std::wstring limitName;
        double primaryUsedPercent{ -1.0 };
        int primaryWindowMinutes{};
        int64_t primaryResetsAt{};
        double secondaryUsedPercent{ -1.0 };
        int secondaryWindowMinutes{};
        int64_t secondaryResetsAt{};
        std::wstring planType;
        int64_t capturedAt{};
    };

    struct UsageTotals
    {
        TokenCounts counts;
        int64_t messages{};
        int64_t toolCalls{};
        int64_t activeDays{};
        int64_t sessions{};
    };

    struct DailyUsage
    {
        std::wstring day;
        std::wstring sourceKind;
        std::wstring tool;
        std::wstring model;
        std::wstring project;
        std::wstring deviceId;
        TokenCounts counts;
        int64_t messages{};
        int64_t toolCalls{};
        std::wstring accountId;
    };

    struct HourlyUsage
    {
        int64_t hourStart{};
        std::wstring day;
        std::wstring sourceKind;
        std::wstring tool;
        std::wstring model;
        std::wstring project;
        std::wstring deviceId;
        TokenCounts counts;
        int64_t messages{};
        int64_t toolCalls{};
        std::wstring accountId;
    };

    struct BreakdownRow
    {
        std::wstring key;
        TokenCounts counts;
        int64_t sessions{};
        int64_t messages{};
        int64_t toolCalls{};
    };

    struct SessionSummary
    {
        std::wstring id;
        std::wstring title;
        std::wstring project;
        std::wstring model;
        std::wstring deviceId;
        int64_t startedAt{};
        int64_t updatedAt{};
        int64_t messages{};
        int64_t toolCalls{};
        TokenCounts counts;
        std::wstring accountId;
    };

    struct TurnSummary
    {
        std::wstring sessionId;
        std::wstring turnId;
        int promptIndex{};
        int64_t timestamp{};
        std::wstring model;
        std::wstring tools;
        TokenCounts counts;
    };

    struct ToolCallDetail
    {
        std::wstring sourcePath;
        std::wstring name;
        std::wstring callId;
        int64_t inputOffset{};
        int64_t inputLength{};
        int64_t outputOffset{};
        int64_t outputLength{};
    };
}
