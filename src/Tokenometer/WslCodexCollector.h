#pragma once

#include "CodexCollector.h"
#include "WslProcessRunner.h"

#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tokenometer
{
    struct WslCollectionResult
    {
        bool discoverySucceeded{};
        int distributionsFound{};
        int distributionsScanned{};
        int filesFound{};
        int errors{};
        CollectionResult usage;
        int64_t completedAt{};
    };

    class WslCodexCollector final
    {
    public:
        using Runner = std::function<WslProcessResult(
            std::vector<std::wstring> const&,
            WslProcessOptions const&,
            std::stop_token)>;

        explicit WslCodexCollector(Database& database, Runner runner = {});

        [[nodiscard]] WslCollectionResult CollectOnce(std::stop_token stopToken = {});
        [[nodiscard]] static std::optional<std::string> ReadSourceRange(
            std::wstring_view locator,
            int64_t offset,
            int64_t length,
            std::stop_token stopToken = {});
        [[nodiscard]] static bool SelfTest();

    private:
        struct OversizedScanState
        {
            int64_t recordOffset{};
            int64_t scanOffset{};
        };

        Database& m_database;
        CodexCollector m_parser;
        Runner m_runner;
        std::wstring m_localDeviceId;
        std::wstring m_hostName;
        std::unordered_map<std::wstring, size_t> m_activeBacklogCursors;
        std::unordered_map<std::wstring, size_t> m_archiveCursors;
        std::unordered_map<std::wstring, OversizedScanState> m_oversizedScanCursors;
    };
}
