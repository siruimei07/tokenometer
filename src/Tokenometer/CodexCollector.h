#pragma once

#include "Database.h"

#include <filesystem>
#include <iosfwd>
#include <stop_token>
#include <string>
#include <unordered_map>

namespace tokenometer
{
    struct CollectionResult
    {
        int filesVisited{};
        int filesChanged{};
        int usageEvents{};
        int promptEvents{};
        int toolEvents{};
        int malformedRecords{};
        int oversizedRecords{};
        int ioErrors{};
        int64_t bytesRead{};
        int64_t completedAt{};

        [[nodiscard]] bool HasPartialErrors() const noexcept
        {
            return ioErrors > 0 || malformedRecords > 0 || oversizedRecords > 0;
        }
    };

    struct ExternalCodexTranscript
    {
        std::wstring sourcePath;
        std::wstring fileIdentity;
        std::wstring sessionId;
        std::wstring deviceId;
        std::wstring sourceKind{ L"codex" };
        std::wstring accountId{ L"current" };
        int64_t size{};
        int64_t modifiedAt{};
        bool trailingNewline{};
        int64_t contentOffset{};
        std::string content;
        bool collectRateLimits{ true };
    };

    class CodexCollector final
    {
    public:
        explicit CodexCollector(Database& database, std::filesystem::path codexRoot = DefaultCodexRoot());

        static std::filesystem::path DefaultCodexRoot();
        [[nodiscard]] static bool SelfTest();
        [[nodiscard]] CollectionResult CollectOnce(std::stop_token stopToken = {});
        [[nodiscard]] CollectionResult CollectExternal(
            ExternalCodexTranscript const& transcript,
            std::stop_token stopToken = {});

    private:
        void LoadSessionTitles(CollectionResult& result);
        void CollectDirectory(
            std::filesystem::path const& directory,
            CollectionResult& result,
            std::stop_token stopToken);
        void CollectFile(
            std::filesystem::path const& path,
            CollectionResult& result,
            std::stop_token stopToken);
        [[nodiscard]] bool PrepareTranscript(
            ExternalCodexTranscript const& transcript,
            bool requireExpectedOffset,
            SourceProgress& current,
            SessionRecord& session);
        void CollectStream(
            ExternalCodexTranscript const& transcript,
            SourceProgress& current,
            SessionRecord& session,
            std::istream& stream,
            int64_t streamBaseOffset,
            int64_t availableEnd,
            CollectionResult& result,
            std::stop_token stopToken);

        Database& m_database;
        std::filesystem::path m_root;
        std::wstring m_deviceId;
        std::unordered_map<std::wstring, std::wstring> m_titles;
    };
}
