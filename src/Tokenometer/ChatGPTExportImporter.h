#pragma once

#include "UsageModels.h"

#include <filesystem>
#include <stop_token>
#include <string_view>

namespace tokenometer
{
    class Database;

    class ChatGPTExportImporter final
    {
    public:
        explicit ChatGPTExportImporter(Database& database) noexcept;

        [[nodiscard]] ChatGPTImportResult Import(
            std::filesystem::path const& selectedFile,
            std::wstring_view accountId,
            std::stop_token stopToken = {});

        [[nodiscard]] static bool SelfTest();

    private:
        Database& m_database;
    };
}
