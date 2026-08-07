#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace tokenometer
{
    struct WslProcessOptions
    {
        std::chrono::milliseconds timeout{std::chrono::seconds{30}};
        size_t maximumStandardOutputBytes{8 * 1024 * 1024};
        size_t maximumStandardErrorBytes{256 * 1024};
    };

    struct WslProcessResult
    {
        bool started{};
        bool timedOut{};
        bool cancelled{};
        bool standardOutputTruncated{};
        bool standardErrorTruncated{};
        bool remoteCleanupFailed{};
        uint32_t systemError{};
        std::optional<uint32_t> exitCode;
        std::vector<uint8_t> standardOutput;
        std::vector<uint8_t> standardError;
    };

    class WslProcessRunner final
    {
    public:
        static constexpr size_t MaximumPipeBytes = 20 * 1024 * 1024;

        [[nodiscard]] static WslProcessResult Run(
            std::vector<std::wstring> const& arguments,
            WslProcessOptions const& options = {},
            std::stop_token stopToken = {});

        [[nodiscard]] static bool SelfTest();
    };
}
