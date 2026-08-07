#pragma once

#include "Database.h"

#include <filesystem>
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
        int64_t bytesRead{};
        int64_t completedAt{};
    };

    class CodexCollector final
    {
    public:
        explicit CodexCollector(Database& database, std::filesystem::path codexRoot = DefaultCodexRoot());

        static std::filesystem::path DefaultCodexRoot();
        [[nodiscard]] static bool SelfTest();
        [[nodiscard]] CollectionResult CollectOnce(std::stop_token stopToken = {});

    private:
        void LoadSessionTitles();
        void CollectDirectory(
            std::filesystem::path const& directory,
            CollectionResult& result,
            std::stop_token stopToken);
        void CollectFile(
            std::filesystem::path const& path,
            CollectionResult& result,
            std::stop_token stopToken);

        Database& m_database;
        std::filesystem::path m_root;
        std::wstring m_deviceId;
        std::unordered_map<std::wstring, std::wstring> m_titles;
    };
}
