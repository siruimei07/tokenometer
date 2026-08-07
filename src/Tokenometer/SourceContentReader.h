#pragma once

#include "UsageModels.h"

#include <filesystem>
#include <stop_token>
#include <string>

namespace tokenometer
{
    struct ToolCallContent
    {
        std::wstring input;
        std::wstring output;
    };

    class SourceContentReader final
    {
    public:
        SourceContentReader();

        [[nodiscard]] ToolCallContent Read(
            ToolCallDetail const& locator,
            std::stop_token stopToken = {}) const;
        [[nodiscard]] static bool SelfTest();

    private:
        explicit SourceContentReader(std::filesystem::path codexRoot);

        std::filesystem::path m_codexRoot;
    };

    [[nodiscard]] bool TestSourceContentReader();
}
