#include "WslProcessRunner.h"

#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <limits>
#include <string_view>

namespace tokenometer
{
    namespace
    {
        using namespace std::chrono_literals;

        constexpr auto waitInterval = 100ms;
        constexpr size_t readBudgetPerPass = 256 * 1024;
        constexpr std::wstring_view remoteInnerScript =
            L"id=$1; shell=$2; limit=$3; script=$4; name=$5; shift 5; "
            L"pid_file=\"/tmp/tokenometer-$id.pid\"; "
            L"cancel_file=\"/tmp/tokenometer-$id.cancel\"; umask 077; "
            L"if [ -e \"$cancel_file\" ]; then rm -f -- \"$cancel_file\"; exit 143; fi; "
            L"(set -C; printf '%s\\n' \"$$\" >\"$pid_file\") 2>/dev/null || exit 126; "
            L"if [ -e \"$cancel_file\" ]; then "
            L"rm -f -- \"$pid_file\" \"$cancel_file\"; exit 143; fi; "
            L"exec timeout --signal=TERM --kill-after=2s \"$limit\" "
            L"\"$shell\" -c \"$script\" \"$name\" \"$@\"";
        constexpr std::wstring_view remoteWrapperScript =
            L"id=$1; inner=$2; shell=$3; limit=$4; script=$5; name=$6; shift 6; "
            L"case \"$id\" in ''|*[!A-Fa-f0-9]*) exit 126;; esac; "
            L"pid_file=\"/tmp/tokenometer-$id.pid\"; "
            L"cancel_file=\"/tmp/tokenometer-$id.cancel\"; child=; "
            L"cleanup() { rm -f -- \"$pid_file\" \"$cancel_file\"; }; "
            L"stop_child() { if [ -n \"$child\" ]; then "
            L"kill -TERM -- \"-$child\" 2>/dev/null || true; sleep 0.1; "
            L"kill -KILL -- \"-$child\" 2>/dev/null || true; fi; cleanup; exit 143; }; "
            L"trap stop_child HUP INT TERM; "
            L"setsid /bin/sh -c \"$inner\" tokenometer-inner \"$id\" \"$shell\" "
            L"\"$limit\" \"$script\" \"$name\" \"$@\" & child=$!; "
            L"wait \"$child\"; status=$?; cleanup; exit \"$status\"";
        constexpr std::wstring_view remoteCleanupScript =
            L"id=$1; case \"$id\" in ''|*[!A-Fa-f0-9]*) exit 126;; esac; "
            L"pid_file=\"/tmp/tokenometer-$id.pid\"; "
            L"cancel_file=\"/tmp/tokenometer-$id.cancel\"; umask 077; "
            L"(set -C; : >\"$cancel_file\") 2>/dev/null || true; i=0; "
            L"while [ ! -r \"$pid_file\" ] && [ \"$i\" -lt 40 ]; do "
            L"sleep 0.05; i=$((i + 1)); done; "
            L"[ -r \"$pid_file\" ] || exit 0; IFS= read -r pid <\"$pid_file\"; "
            L"case \"$pid\" in ''|*[!0-9]*) exit 126;; esac; "
            L"kill -TERM -- \"-$pid\" 2>/dev/null || true; i=0; "
            L"while kill -0 -- \"-$pid\" 2>/dev/null && [ \"$i\" -lt 20 ]; do "
            L"sleep 0.05; i=$((i + 1)); done; "
            L"kill -KILL -- \"-$pid\" 2>/dev/null || true; "
            L"i=0; while kill -0 -- \"-$pid\" 2>/dev/null && [ \"$i\" -lt 20 ]; do "
            L"sleep 0.05; i=$((i + 1)); done; "
            L"rm -f -- \"$pid_file\" \"$cancel_file\"; "
            L"! kill -0 -- \"-$pid\" 2>/dev/null";

        struct GuardedCommand
        {
            std::vector<std::wstring> arguments;
            std::wstring distribution;
            std::wstring runId;
        };

        class UniqueHandle final
        {
        public:
            UniqueHandle() = default;
            explicit UniqueHandle(HANDLE value) noexcept : m_value(value) {}
            ~UniqueHandle() { Reset(); }

            UniqueHandle(UniqueHandle const&) = delete;
            UniqueHandle& operator=(UniqueHandle const&) = delete;

            UniqueHandle(UniqueHandle&& other) noexcept : m_value(other.Release()) {}
            UniqueHandle& operator=(UniqueHandle&& other) noexcept
            {
                if (this != &other)
                {
                    Reset(other.Release());
                }
                return *this;
            }

            [[nodiscard]] HANDLE Get() const noexcept { return m_value; }
            [[nodiscard]] explicit operator bool() const noexcept
            {
                return m_value && m_value != INVALID_HANDLE_VALUE;
            }

            [[nodiscard]] HANDLE Release() noexcept
            {
                HANDLE const value = m_value;
                m_value = nullptr;
                return value;
            }

            void Reset(HANDLE value = nullptr) noexcept
            {
                if (*this)
                {
                    CloseHandle(m_value);
                }
                m_value = value;
            }

        private:
            HANDLE m_value{};
        };

        std::wstring QuoteArgument(std::wstring_view argument)
        {
            if (!argument.empty() && argument.find_first_of(L" \t\r\n\v\"") == std::wstring_view::npos)
            {
                return std::wstring{argument};
            }

            std::wstring quoted;
            quoted.reserve(argument.size() + 2);
            quoted.push_back(L'"');

            size_t backslashes{};
            for (wchar_t const character : argument)
            {
                if (character == L'\\')
                {
                    ++backslashes;
                    continue;
                }

                if (character == L'"')
                {
                    quoted.append(backslashes * 2 + 1, L'\\');
                    quoted.push_back(L'"');
                }
                else
                {
                    quoted.append(backslashes, L'\\');
                    quoted.push_back(character);
                }
                backslashes = 0;
            }

            quoted.append(backslashes * 2, L'\\');
            quoted.push_back(L'"');
            return quoted;
        }

        std::wstring WslExecutablePath()
        {
            std::array<wchar_t, 32'768> buffer{};
            DWORD const length = GetSystemDirectoryW(
                buffer.data(),
                static_cast<UINT>(buffer.size()));
            if (length == 0 || length >= buffer.size())
            {
                return {};
            }

            std::wstring path{buffer.data(), length};
            while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
            {
                path.pop_back();
            }
            path += L"\\wsl.exe";
            return path;
        }

        std::wstring MakeCommandLine(
            std::wstring const& executable,
            std::vector<std::wstring> const& arguments)
        {
            std::wstring commandLine = QuoteArgument(executable);
            for (auto const& argument : arguments)
            {
                commandLine.push_back(L' ');
                commandLine += QuoteArgument(argument);
            }
            return commandLine;
        }

        std::optional<std::wstring> DecodeListOutput(std::vector<uint8_t> const& bytes)
        {
            bool utf16 = bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE;
            if (!utf16)
            {
                for (size_t index = 1; index < bytes.size(); index += 2)
                {
                    if (bytes[index] == 0)
                    {
                        utf16 = true;
                        break;
                    }
                }
            }
            if (utf16)
            {
                if ((bytes.size() & 1u) != 0) return std::nullopt;
                size_t offset = bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE
                    ? 2
                    : 0;
                std::wstring value;
                value.reserve((bytes.size() - offset) / 2);
                for (; offset < bytes.size(); offset += 2)
                {
                    value.push_back(static_cast<wchar_t>(
                        static_cast<uint16_t>(bytes[offset]) |
                        (static_cast<uint16_t>(bytes[offset + 1]) << 8)));
                }
                return value;
            }

            if (bytes.empty()) return std::wstring{};
            if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
            {
                return std::nullopt;
            }
            int const length = MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                reinterpret_cast<char const*>(bytes.data()),
                static_cast<int>(bytes.size()),
                nullptr,
                0);
            if (length <= 0) return std::nullopt;
            std::wstring value(static_cast<size_t>(length), L'\0');
            return MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                reinterpret_cast<char const*>(bytes.data()),
                static_cast<int>(bytes.size()),
                value.data(),
                length) == length
                ? std::optional{std::move(value)}
                : std::nullopt;
        }

        std::optional<bool> DistributionIsRunning(std::wstring_view distribution)
        {
            WslProcessOptions options;
            options.timeout = 3s;
            options.maximumStandardOutputBytes = 64 * 1024;
            options.maximumStandardErrorBytes = 64 * 1024;
            auto const listed = WslProcessRunner::Run(
                {L"--list", L"--running", L"--quiet"}, options, {});
            if (!listed.started || listed.timedOut || listed.cancelled ||
                listed.standardOutputTruncated || listed.remoteCleanupFailed ||
                !listed.exitCode || *listed.exitCode != 0)
            {
                return std::nullopt;
            }
            auto text = DecodeListOutput(listed.standardOutput);
            if (!text) return std::nullopt;

            size_t start{};
            for (size_t index = 0; index <= text->size(); ++index)
            {
                if (index != text->size() && (*text)[index] != L'\r' &&
                    (*text)[index] != L'\n' && (*text)[index] != L'\0')
                {
                    continue;
                }
                size_t first = start;
                size_t last = index;
                while (first < last && std::iswspace((*text)[first])) ++first;
                while (last > first && std::iswspace((*text)[last - 1])) --last;
                if (std::wstring_view{*text}.substr(first, last - first) == distribution)
                {
                    return true;
                }
                start = index + 1;
            }
            return false;
        }

        std::optional<std::wstring> NewRunId()
        {
            std::array<uint8_t, 16> bytes{};
            if (!BCRYPT_SUCCESS(BCryptGenRandom(
                    nullptr,
                    bytes.data(),
                    static_cast<ULONG>(bytes.size()),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
            {
                return std::nullopt;
            }
            constexpr wchar_t hex[] = L"0123456789abcdef";
            std::wstring result;
            result.reserve(bytes.size() * 2);
            for (uint8_t const byte : bytes)
            {
                result.push_back(hex[byte >> 4]);
                result.push_back(hex[byte & 0x0F]);
            }
            return result;
        }

        std::optional<GuardedCommand> GuardRemoteCommand(
            std::vector<std::wstring> const& arguments,
            std::chrono::milliseconds timeout)
        {
            GuardedCommand guarded{arguments};
            if (arguments.size() < 7 || arguments[0] != L"--distribution" ||
                arguments[2] != L"--exec" || arguments[4] != L"-c" ||
                arguments[6] != L"tokenometer" ||
                (arguments[3] != L"/bin/sh" && arguments[3] != L"/bin/bash"))
            {
                return guarded;
            }

            auto runId = NewRunId();
            if (!runId) return std::nullopt;
            int64_t milliseconds = timeout == std::chrono::milliseconds::max()
                ? 3'600'000
                : std::clamp<int64_t>(timeout.count(), 1, 86'400'000);
            int64_t const seconds = (milliseconds + 999) / 1'000;

            guarded.distribution = arguments[1];
            guarded.runId = std::move(*runId);
            guarded.arguments = {
                L"--distribution", guarded.distribution, L"--exec", L"/bin/sh", L"-c",
                std::wstring{remoteWrapperScript}, L"tokenometer-guard", guarded.runId,
                std::wstring{remoteInnerScript}, arguments[3],
                std::to_wstring(seconds) + L"s", arguments[5], arguments[6]
            };
            guarded.arguments.insert(
                guarded.arguments.end(),
                arguments.begin() + 7,
                arguments.end());
            return guarded;
        }

        bool CleanupRemoteCommand(GuardedCommand const& guarded)
        {
            if (guarded.runId.empty()) return true;
            auto const running = DistributionIsRunning(guarded.distribution);
            if (!running) return false;
            if (!*running) return true;
            WslProcessOptions options;
            options.timeout = 5s;
            options.maximumStandardOutputBytes = 0;
            options.maximumStandardErrorBytes = 64 * 1024;
            auto const cleanup = WslProcessRunner::Run({
                L"--distribution", guarded.distribution, L"--exec", L"/bin/sh", L"-c",
                std::wstring{remoteCleanupScript}, L"tokenometer-cleanup", guarded.runId
            }, options, {});
            return cleanup.started && !cleanup.timedOut && !cleanup.cancelled &&
                !cleanup.remoteCleanupFailed && cleanup.exitCode && *cleanup.exitCode == 0;
        }

        bool CreateOutputPipe(UniqueHandle& readHandle, UniqueHandle& writeHandle)
        {
            SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
            HANDLE read{};
            HANDLE write{};
            if (!CreatePipe(&read, &write, &attributes, 0))
            {
                return false;
            }

            readHandle.Reset(read);
            writeHandle.Reset(write);
            return SetHandleInformation(readHandle.Get(), HANDLE_FLAG_INHERIT, 0) != FALSE;
        }

        void AppendBounded(
            std::vector<uint8_t>& destination,
            uint8_t const* source,
            size_t size,
            size_t limit,
            bool& truncated)
        {
            size_t const available = destination.size() < limit ? limit - destination.size() : 0;
            size_t const copied = std::min(size, available);
            destination.insert(destination.end(), source, source + copied);
            truncated = truncated || copied != size;
        }

        bool DrainPipe(
            HANDLE pipe,
            std::vector<uint8_t>& destination,
            size_t limit,
            bool& truncated,
            uint32_t& systemError)
        {
            std::array<uint8_t, 16 * 1024> buffer{};
            size_t drained{};
            bool readAny{};

            while (drained < readBudgetPerPass)
            {
                DWORD available{};
                if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
                {
                    DWORD const error = GetLastError();
                    if (error != ERROR_BROKEN_PIPE && systemError == ERROR_SUCCESS)
                    {
                        systemError = error;
                    }
                    break;
                }
                if (available == 0)
                {
                    break;
                }

                DWORD const requested = static_cast<DWORD>(std::min<size_t>(
                    {buffer.size(), available, readBudgetPerPass - drained}));
                DWORD bytesRead{};
                if (!ReadFile(pipe, buffer.data(), requested, &bytesRead, nullptr) || bytesRead == 0)
                {
                    DWORD const error = GetLastError();
                    if (error != ERROR_BROKEN_PIPE && systemError == ERROR_SUCCESS)
                    {
                        systemError = error;
                    }
                    break;
                }

                AppendBounded(destination, buffer.data(), bytesRead, limit, truncated);
                drained += bytesRead;
                readAny = true;
            }
            return readAny;
        }

        bool TerminateTree(
            HANDLE job,
            bool assignedToJob,
            HANDLE process,
            uint32_t exitCode,
            uint32_t& failure) noexcept
        {
            bool terminationRequested{};
            if (assignedToJob)
            {
                terminationRequested = TerminateJobObject(job, exitCode) != FALSE;
            }
            if (!terminationRequested)
            {
                terminationRequested = TerminateProcess(process, exitCode) != FALSE;
            }
            if (!terminationRequested)
            {
                failure = GetLastError();
                return false;
            }

            DWORD const waitResult = WaitForSingleObject(process, 2'000);
            if (waitResult != WAIT_OBJECT_0)
            {
                failure = waitResult == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
                return false;
            }
            return true;
        }

        bool QuoteRoundTrips(std::wstring const& argument)
        {
            std::wstring const commandLine = L"probe " + QuoteArgument(argument);
            int count{};
            LPWSTR* values = CommandLineToArgvW(commandLine.c_str(), &count);
            bool const matches = values && count == 2 && argument == values[1];
            if (values)
            {
                LocalFree(values);
            }
            return matches;
        }
    }

    WslProcessResult WslProcessRunner::Run(
        std::vector<std::wstring> const& arguments,
        WslProcessOptions const& options,
        std::stop_token stopToken)
    {
        WslProcessResult result;
        if (stopToken.stop_requested())
        {
            result.cancelled = true;
            result.systemError = ERROR_CANCELLED;
            return result;
        }

        auto guarded = GuardRemoteCommand(arguments, options.timeout);
        if (!guarded)
        {
            result.systemError = ERROR_NOT_ENOUGH_MEMORY;
            return result;
        }

        std::wstring const executable = WslExecutablePath();
        if (executable.empty())
        {
            result.systemError = ERROR_PATH_NOT_FOUND;
            return result;
        }

        UniqueHandle standardOutputRead;
        UniqueHandle standardOutputWrite;
        UniqueHandle standardErrorRead;
        UniqueHandle standardErrorWrite;
        if (!CreateOutputPipe(standardOutputRead, standardOutputWrite) ||
            !CreateOutputPipe(standardErrorRead, standardErrorWrite))
        {
            result.systemError = GetLastError();
            return result;
        }

        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        UniqueHandle standardInput{CreateFileW(
            L"NUL",
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &attributes,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr)};
        if (!standardInput)
        {
            result.systemError = GetLastError();
            return result;
        }

        STARTUPINFOEXW startupInfo{};
        startupInfo.StartupInfo.cb = sizeof(startupInfo);
        startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startupInfo.StartupInfo.hStdInput = standardInput.Get();
        startupInfo.StartupInfo.hStdOutput = standardOutputWrite.Get();
        startupInfo.StartupInfo.hStdError = standardErrorWrite.Get();

        SIZE_T attributeBytes{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        if (attributeBytes == 0)
        {
            result.systemError = GetLastError();
            return result;
        }
        std::vector<uint8_t> attributeStorage(attributeBytes);
        startupInfo.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
            attributeStorage.data());
        if (!InitializeProcThreadAttributeList(
                startupInfo.lpAttributeList,
                1,
                0,
                &attributeBytes))
        {
            result.systemError = GetLastError();
            return result;
        }
        struct AttributeListCleanup final
        {
            PPROC_THREAD_ATTRIBUTE_LIST value{};
            ~AttributeListCleanup()
            {
                if (value) DeleteProcThreadAttributeList(value);
            }
        } attributeCleanup{startupInfo.lpAttributeList};
        std::array<HANDLE, 3> const inheritedHandles{
            standardInput.Get(),
            standardOutputWrite.Get(),
            standardErrorWrite.Get()
        };
        if (!UpdateProcThreadAttribute(
                startupInfo.lpAttributeList,
                0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                const_cast<HANDLE*>(inheritedHandles.data()),
                sizeof(inheritedHandles),
                nullptr,
                nullptr))
        {
            result.systemError = GetLastError();
            return result;
        }

        std::wstring commandLine = MakeCommandLine(executable, guarded->arguments);
        PROCESS_INFORMATION processInformation{};
        if (!CreateProcessW(
            executable.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                EXTENDED_STARTUPINFO_PRESENT,
            nullptr,
            nullptr,
            &startupInfo.StartupInfo,
            &processInformation))
        {
            result.systemError = GetLastError();
            return result;
        }

        UniqueHandle process{processInformation.hProcess};
        UniqueHandle thread{processInformation.hThread};
        result.started = true;
        standardOutputWrite.Reset();
        standardErrorWrite.Reset();
        standardInput.Reset();

        UniqueHandle job{CreateJobObjectW(nullptr, nullptr)};
        bool assignedToJob{};
        if (job)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (SetInformationJobObject(
                    job.Get(),
                    JobObjectExtendedLimitInformation,
                    &limits,
                    sizeof(limits)))
            {
                assignedToJob = AssignProcessToJobObject(job.Get(), process.Get()) != FALSE;
            }
        }

        bool processExited{};
        bool remoteCleanupNeeded{};
        if (ResumeThread(thread.Get()) == std::numeric_limits<DWORD>::max())
        {
            result.systemError = GetLastError();
            uint32_t terminationError{};
            processExited = TerminateTree(
                job.Get(), assignedToJob, process.Get(), result.systemError, terminationError);
            if (!processExited && terminationError != ERROR_SUCCESS)
            {
                result.systemError = terminationError;
            }
        }
        else
        {
            auto const startedAt = std::chrono::steady_clock::now();
            bool finished{};
            while (!finished)
            {
                size_t const outputLimit = std::min(options.maximumStandardOutputBytes, MaximumPipeBytes);
                size_t const errorLimit = std::min(options.maximumStandardErrorBytes, MaximumPipeBytes);
                DrainPipe(
                    standardOutputRead.Get(),
                    result.standardOutput,
                    outputLimit,
                    result.standardOutputTruncated,
                    result.systemError);
                DrainPipe(
                    standardErrorRead.Get(),
                    result.standardError,
                    errorLimit,
                    result.standardErrorTruncated,
                    result.systemError);

                if (stopToken.stop_requested())
                {
                    remoteCleanupNeeded = !guarded->runId.empty();
                    result.cancelled = true;
                    if (result.systemError == ERROR_SUCCESS)
                    {
                        result.systemError = ERROR_CANCELLED;
                    }
                    uint32_t terminationError{};
                    processExited = TerminateTree(
                        job.Get(), assignedToJob, process.Get(), ERROR_CANCELLED, terminationError);
                    if (!processExited && terminationError != ERROR_SUCCESS)
                    {
                        result.systemError = terminationError;
                    }
                    break;
                }

                DWORD waitMilliseconds = static_cast<DWORD>(waitInterval.count());
                if (options.timeout != std::chrono::milliseconds::max())
                {
                    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - startedAt);
                    if (elapsed >= options.timeout)
                    {
                        remoteCleanupNeeded = !guarded->runId.empty();
                        result.timedOut = true;
                        if (result.systemError == ERROR_SUCCESS)
                        {
                            result.systemError = ERROR_TIMEOUT;
                        }
                        uint32_t terminationError{};
                        processExited = TerminateTree(
                            job.Get(), assignedToJob, process.Get(), ERROR_TIMEOUT, terminationError);
                        if (!processExited && terminationError != ERROR_SUCCESS)
                        {
                            result.systemError = terminationError;
                        }
                        break;
                    }

                    auto const remaining = options.timeout - elapsed;
                    waitMilliseconds = static_cast<DWORD>(std::clamp<int64_t>(
                        remaining.count(),
                        1,
                        waitInterval.count()));
                }

                DWORD const waitResult = WaitForSingleObject(process.Get(), waitMilliseconds);
                if (waitResult == WAIT_OBJECT_0)
                {
                    finished = true;
                    processExited = true;
                }
                else if (waitResult == WAIT_FAILED)
                {
                    remoteCleanupNeeded = !guarded->runId.empty();
                    if (result.systemError == ERROR_SUCCESS)
                    {
                        result.systemError = GetLastError();
                    }
                    uint32_t terminationError{};
                    processExited = TerminateTree(
                        job.Get(), assignedToJob, process.Get(), result.systemError, terminationError);
                    if (!processExited && terminationError != ERROR_SUCCESS)
                    {
                        result.systemError = terminationError;
                    }
                    break;
                }
            }
        }

        if (remoteCleanupNeeded && !CleanupRemoteCommand(*guarded))
        {
            result.remoteCleanupFailed = true;
        }
        if (remoteCleanupNeeded && !processExited)
        {
            processExited = WaitForSingleObject(process.Get(), 2'000) == WAIT_OBJECT_0;
        }

        size_t const outputLimit = std::min(options.maximumStandardOutputBytes, MaximumPipeBytes);
        size_t const errorLimit = std::min(options.maximumStandardErrorBytes, MaximumPipeBytes);
        for (int pass = 0; pass != 8; ++pass)
        {
            bool const readOutput = DrainPipe(
                standardOutputRead.Get(),
                result.standardOutput,
                outputLimit,
                result.standardOutputTruncated,
                result.systemError);
            bool const readError = DrainPipe(
                standardErrorRead.Get(),
                result.standardError,
                errorLimit,
                result.standardErrorTruncated,
                result.systemError);
            if (!readOutput && !readError)
            {
                break;
            }
        }

        if (!processExited)
        {
            processExited = WaitForSingleObject(process.Get(), 0) == WAIT_OBJECT_0;
        }
        DWORD exitCode{};
        BOOL const hasExitCode = processExited
            ? GetExitCodeProcess(process.Get(), &exitCode)
            : FALSE;
        if (hasExitCode)
        {
            result.exitCode = exitCode;
        }
        else if (processExited && result.systemError == ERROR_SUCCESS)
        {
            result.systemError = GetLastError();
        }
        return result;
    }

    bool WslProcessRunner::SelfTest()
    {
        std::array<std::wstring, 7> const arguments{
            L"",
            L"plain",
            L"two words",
            L"embedded\"quote",
            L"C:\\path with space\\",
            L"slashes\\\\\\\"quote",
            L"tab\tvalue"
        };
        if (!std::all_of(arguments.begin(), arguments.end(), QuoteRoundTrips))
        {
            return false;
        }

        std::vector<uint8_t> captured;
        bool truncated{};
        std::array<uint8_t, 6> const sample{1, 2, 3, 4, 5, 6};
        AppendBounded(captured, sample.data(), sample.size(), 3, truncated);
        if (captured != std::vector<uint8_t>({1, 2, 3}) || !truncated)
        {
            return false;
        }

        std::wstring const distribution = L"开发 环境 \"$(noop)\"";
        std::wstring const script = L"printf '%s' \"$1\"";
        std::wstring const path = L"/home/test/with space/$(still-data)";
        auto guarded = GuardRemoteCommand({
            L"--distribution", distribution, L"--exec", L"/bin/sh", L"-c", script,
            L"tokenometer", path
        }, 1'500ms);
        if (!guarded || guarded->runId.size() != 32 || guarded->arguments.size() != 14 ||
            guarded->arguments[1] != distribution || guarded->arguments[5] != remoteWrapperScript ||
            guarded->arguments[7] != guarded->runId ||
            guarded->arguments[8] != remoteInnerScript ||
            guarded->arguments[9] != L"/bin/sh" || guarded->arguments[10] != L"2s" ||
            guarded->arguments[11] != script || guarded->arguments[12] != L"tokenometer" ||
            guarded->arguments[13] != path ||
            guarded->arguments[5].find(distribution) != std::wstring::npos ||
            guarded->arguments[5].find(path) != std::wstring::npos ||
            guarded->arguments[8].find(distribution) != std::wstring::npos ||
            guarded->arguments[8].find(path) != std::wstring::npos)
        {
            return false;
        }
        auto cleanup = GuardRemoteCommand({
            L"--distribution", distribution, L"--exec", L"/bin/sh", L"-c",
            std::wstring{remoteCleanupScript}, L"tokenometer-cleanup", guarded->runId
        }, 3s);
        if (!cleanup || !cleanup->runId.empty() || cleanup->arguments[6] != L"tokenometer-cleanup")
        {
            return false;
        }

        WslProcessOptions const defaults;
        return defaults.timeout == 30s &&
            defaults.maximumStandardOutputBytes <= MaximumPipeBytes &&
            defaults.maximumStandardErrorBytes <= MaximumPipeBytes &&
            waitInterval == 100ms;
    }
}
