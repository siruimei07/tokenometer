#include "CodexCollector.h"

#include <windows.h>
#include <shlobj.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace tokenometer
{
    namespace
    {
        namespace json = winrt::Windows::Data::Json;

        constexpr size_t maximumRecordBytes = 16 * 1024 * 1024;

        struct FileState
        {
            std::wstring identity;
            int64_t size{};
            int64_t modifiedAt{};
            bool trailingNewline{};
        };

        int64_t UnixNow()
        {
            return std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        int64_t FileTimeToUnix(FILETIME const& value)
        {
            ULARGE_INTEGER ticks{};
            ticks.LowPart = value.dwLowDateTime;
            ticks.HighPart = value.dwHighDateTime;
            constexpr uint64_t windowsToUnixSeconds = 11'644'473'600ULL;
            uint64_t const seconds = ticks.QuadPart / 10'000'000ULL;
            return seconds > windowsToUnixSeconds
                ? static_cast<int64_t>(seconds - windowsToUnixSeconds)
                : 0;
        }

        std::optional<FileState> ReadFileState(std::filesystem::path const& path)
        {
            HANDLE const file = CreateFileW(
                path.c_str(),
                FILE_READ_ATTRIBUTES | GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                return std::nullopt;
            }

            BY_HANDLE_FILE_INFORMATION information{};
            if (!GetFileInformationByHandle(file, &information))
            {
                CloseHandle(file);
                return std::nullopt;
            }

            ULARGE_INTEGER size{};
            size.LowPart = information.nFileSizeLow;
            size.HighPart = information.nFileSizeHigh;
            std::array<wchar_t, 64> identity{};
            swprintf_s(
                identity.data(),
                identity.size(),
                L"%08X-%08X%08X",
                information.dwVolumeSerialNumber,
                information.nFileIndexHigh,
                information.nFileIndexLow);

            bool trailingNewline = size.QuadPart == 0;
            if (size.QuadPart > 0)
            {
                LARGE_INTEGER position{};
                position.QuadPart = -1;
                if (SetFilePointerEx(file, position, nullptr, FILE_END))
                {
                    char last{};
                    DWORD read{};
                    trailingNewline = ReadFile(file, &last, 1, &read, nullptr) && read == 1 && last == '\n';
                }
            }
            CloseHandle(file);
            return FileState{
                identity.data(),
                static_cast<int64_t>(size.QuadPart),
                FileTimeToUnix(information.ftLastWriteTime),
                trailingNewline
            };
        }

        std::wstring DeviceName()
        {
            DWORD length{};
            GetComputerNameExW(ComputerNamePhysicalDnsHostname, nullptr, &length);
            std::wstring name(static_cast<size_t>(length), L'\0');
            if (length && GetComputerNameExW(ComputerNamePhysicalDnsHostname, name.data(), &length))
            {
                name.resize(length);
                return name;
            }
            return L"Windows device";
        }

        bool IsInteresting(std::string_view line)
        {
            constexpr std::array types{
                std::string_view{ "\"session_meta\"" },
                std::string_view{ "\"turn_context\"" },
                std::string_view{ "\"task_started\"" },
                std::string_view{ "\"token_count\"" },
                std::string_view{ "\"user_message\"" },
                std::string_view{ "\"function_call\"" },
                std::string_view{ "\"custom_tool_call\"" },
                std::string_view{ "\"function_call_output\"" },
                std::string_view{ "\"custom_tool_call_output\"" }
            };
            return std::ranges::any_of(types, [&](auto type) { return line.find(type) != line.npos; });
        }

        std::optional<json::JsonObject> Object(json::JsonObject const& object, wchar_t const* name)
        {
            if (!object.HasKey(name))
            {
                return std::nullopt;
            }
            auto const value = object.Lookup(name);
            if (!value || value.ValueType() != json::JsonValueType::Object)
            {
                return std::nullopt;
            }
            return value.GetObject();
        }

        std::wstring String(json::JsonObject const& object, wchar_t const* name)
        {
            if (!object.HasKey(name))
            {
                return {};
            }
            auto const value = object.Lookup(name);
            return value && value.ValueType() == json::JsonValueType::String
                ? std::wstring(value.GetString())
                : std::wstring{};
        }

        bool HasValue(json::JsonObject const& object, wchar_t const* name)
        {
            if (!object.HasKey(name))
            {
                return false;
            }
            auto const value = object.Lookup(name);
            return value && value.ValueType() != json::JsonValueType::Null;
        }

        int64_t Integer(json::JsonObject const& object, wchar_t const* name)
        {
            if (!object.HasKey(name))
            {
                return 0;
            }
            auto const value = object.Lookup(name);
            return value && value.ValueType() == json::JsonValueType::Number
                ? static_cast<int64_t>(value.GetNumber())
                : 0;
        }

        double Number(json::JsonObject const& object, wchar_t const* name, double fallback = -1.0)
        {
            if (!object.HasKey(name))
            {
                return fallback;
            }
            auto const value = object.Lookup(name);
            return value && value.ValueType() == json::JsonValueType::Number
                ? value.GetNumber()
                : fallback;
        }

        std::optional<json::JsonObject> Parse(std::string_view line)
        {
            try
            {
                return json::JsonObject::Parse(winrt::to_hstring(line));
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        int64_t ParseTimestamp(std::wstring_view value)
        {
            if (value.size() < 19)
            {
                return 0;
            }
            SYSTEMTIME time{};
            if (swscanf_s(
                    std::wstring(value.substr(0, 19)).c_str(),
                    L"%hu-%hu-%huT%hu:%hu:%hu",
                    &time.wYear,
                    &time.wMonth,
                    &time.wDay,
                    &time.wHour,
                    &time.wMinute,
                    &time.wSecond) != 6)
            {
                return 0;
            }
            FILETIME fileTime{};
            return SystemTimeToFileTime(&time, &fileTime) ? FileTimeToUnix(fileTime) : 0;
        }

        std::wstring LocalDay(int64_t timestamp)
        {
            if (timestamp <= 0)
            {
                return L"unknown";
            }
            ULARGE_INTEGER ticks{};
            ticks.QuadPart = (static_cast<uint64_t>(timestamp) + 11'644'473'600ULL) * 10'000'000ULL;
            FILETIME utc{ ticks.LowPart, ticks.HighPart };
            SYSTEMTIME utcTime{};
            SYSTEMTIME time{};
            DYNAMIC_TIME_ZONE_INFORMATION timeZone{};
            if (!FileTimeToSystemTime(&utc, &utcTime) ||
                GetDynamicTimeZoneInformation(&timeZone) == TIME_ZONE_ID_INVALID ||
                !SystemTimeToTzSpecificLocalTimeEx(&timeZone, &utcTime, &time))
            {
                return L"unknown";
            }
            std::array<wchar_t, 16> value{};
            swprintf_s(value.data(), value.size(), L"%04u-%02u-%02u", time.wYear, time.wMonth, time.wDay);
            return value.data();
        }

        TokenCounts ReadCounts(json::JsonObject const& object)
        {
            return {
                Integer(object, L"input_tokens"),
                Integer(object, L"cached_input_tokens"),
                Integer(object, L"cache_write_input_tokens"),
                Integer(object, L"output_tokens"),
                Integer(object, L"reasoning_output_tokens"),
                Integer(object, L"total_tokens")
            };
        }

        bool Decreased(TokenCounts const& current, TokenCounts const& previous)
        {
            return current.input < previous.input ||
                   current.cachedInput < previous.cachedInput ||
                   current.cacheWriteInput < previous.cacheWriteInput ||
                   current.output < previous.output ||
                   current.reasoningOutput < previous.reasoningOutput ||
                   current.reportedTotal < previous.reportedTotal;
        }

        bool Equal(TokenCounts const& left, TokenCounts const& right)
        {
            return left.input == right.input &&
                   left.cachedInput == right.cachedInput &&
                   left.cacheWriteInput == right.cacheWriteInput &&
                   left.output == right.output &&
                   left.reasoningOutput == right.reasoningOutput &&
                   left.reportedTotal == right.reportedTotal;
        }

        TokenCounts Difference(TokenCounts const& current, TokenCounts const& previous)
        {
            auto delta = [](int64_t value, int64_t prior)
            {
                return value > prior ? value - prior : 0;
            };
            return {
                delta(current.input, previous.input),
                delta(current.cachedInput, previous.cachedInput),
                delta(current.cacheWriteInput, previous.cacheWriteInput),
                delta(current.output, previous.output),
                delta(current.reasoningOutput, previous.reasoningOutput),
                delta(current.reportedTotal, previous.reportedTotal)
            };
        }

        bool HasUsage(TokenCounts const& counts)
        {
            return counts.input || counts.cachedInput || counts.cacheWriteInput ||
                   counts.output || counts.reasoningOutput || counts.reportedTotal;
        }

        std::wstring SessionIdFromPath(std::filesystem::path const& path)
        {
            auto const stem = path.stem().wstring();
            return stem.size() >= 36 ? stem.substr(stem.size() - 36) : stem;
        }
    }

    CodexCollector::CodexCollector(Database& database, std::filesystem::path codexRoot) :
        m_database(database),
        m_root(std::move(codexRoot)),
        m_deviceId(database.GetOrCreateDeviceId(DeviceName()))
    {
    }

    std::filesystem::path CodexCollector::DefaultCodexRoot()
    {
        DWORD const environmentLength = GetEnvironmentVariableW(L"CODEX_HOME", nullptr, 0);
        if (environmentLength > 1)
        {
            std::wstring configured(static_cast<size_t>(environmentLength), L'\0');
            DWORD const written = GetEnvironmentVariableW(
                L"CODEX_HOME",
                configured.data(),
                environmentLength);
            if (written > 0 && written < environmentLength)
            {
                configured.resize(written);
                std::filesystem::path const path(configured);
                if (path.is_absolute())
                {
                    return path;
                }
            }
        }
        PWSTR rawPath{};
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &rawPath)))
        {
            return {};
        }
        std::filesystem::path path(rawPath);
        CoTaskMemFree(rawPath);
        return path / L".codex";
    }

    CollectionResult CodexCollector::CollectOnce(std::stop_token stopToken)
    {
        CollectionResult result;
        LoadSessionTitles();
        CollectDirectory(m_root / L"sessions", result, stopToken);
        CollectDirectory(m_root / L"archived_sessions", result, stopToken);
        result.completedAt = UnixNow();
        return result;
    }

    bool CodexCollector::SelfTest()
    {
        auto const root = std::filesystem::temp_directory_path() /
            (L"TokenometerCollectorTest-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code cleanupError;
        std::filesystem::remove_all(root, cleanupError);
        try
        {
            std::filesystem::create_directories(root / L"sessions" / L"2026" / L"01" / L"01");
            std::ofstream index(root / L"session_index.jsonl", std::ios::binary);
            index << R"({"id":"11111111-1111-1111-1111-111111111111","thread_name":"Collector fixture","updated_at":"2026-01-01T00:00:10Z"})" << '\n';
            index.close();

            auto const fixture = root / L"sessions" / L"2026" / L"01" / L"01" /
                L"rollout-2026-01-01T00-00-00-11111111-1111-1111-1111-111111111111.jsonl";
            std::ofstream stream(fixture, std::ios::binary);
            stream << R"({"timestamp":"2026-01-01T00:00:00.000Z","type":"session_meta","payload":{"id":"11111111-1111-1111-1111-111111111111","cwd":"D:\\fixture","parent_thread_id":"parent"}})" << '\n';
            stream << R"({"timestamp":"2025-12-31T23:55:00.000Z","type":"turn_context","payload":{"turn_id":"old","model":"old-model","cwd":"D:\\parent"}})" << '\n';
            stream << R"({"timestamp":"2025-12-31T23:55:01.000Z","type":"event_msg","payload":{"type":"user_message","message":"replayed"}})" << '\n';
            stream << R"({"timestamp":"2026-01-01T00:00:00.100Z","type":"event_msg","payload":{"type":"task_started","turn_id":"old","started_at":1767225300.0}})" << '\n';
            stream << R"({"timestamp":"2025-12-31T23:55:02.000Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":500,"cached_input_tokens":100,"cache_write_input_tokens":0,"output_tokens":50,"reasoning_output_tokens":10,"total_tokens":550},"last_token_usage":{"input_tokens":500,"cached_input_tokens":100,"cache_write_input_tokens":0,"output_tokens":50,"reasoning_output_tokens":10,"total_tokens":550}}}})" << '\n';
            stream << R"({"timestamp":"2026-01-01T00:00:00.200Z","type":"event_msg","payload":{"type":"task_started","turn_id":"old-without-started-at"}})" << '\n';
            stream << R"({"timestamp":"2026-01-01T00:00:00.300Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":600,"cached_input_tokens":120,"cache_write_input_tokens":0,"output_tokens":60,"reasoning_output_tokens":12,"total_tokens":660},"last_token_usage":{"input_tokens":100,"cached_input_tokens":20,"cache_write_input_tokens":0,"output_tokens":10,"reasoning_output_tokens":2,"total_tokens":110}}}})" << '\n';
            stream << R"({"timestamp":"2026-01-01T00:00:00.500Z","type":"event_msg","payload":{"type":"task_started","turn_id":"turn-1","started_at":1767225600.5}})" << '\n';
            stream << R"({"timestamp":"2026-01-01T00:00:01.000Z","type":"turn_context","payload":{"turn_id":"turn-1","model":"gpt-test","cwd":"D:\\fixture"}})" << '\n';
            stream << R"({"timestamp":"2026-01-01T00:00:02.000Z","type":"event_msg","payload":{"type":"user_message","message":"private body"}})" << '\n';
            stream << R"({"timestamp":"2026-01-01T00:00:03.000Z","type":"response_item","payload":{"type":"function_call","name":"shell_command","call_id":"call-1","arguments":"private input"}})" << '\n';
            stream << R"({"timestamp":"2026-01-01T00:00:03.500Z","type":"response_item","payload":{"type":"function_call_output","call_id":"call-1","output":"private output"}})" << '\n';
            stream << R"({"timestamp":"2026-01-01T00:00:04.000Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":100,"cached_input_tokens":40,"cache_write_input_tokens":0,"output_tokens":20,"reasoning_output_tokens":5,"total_tokens":120},"last_token_usage":{"input_tokens":100,"cached_input_tokens":40,"cache_write_input_tokens":0,"output_tokens":20,"reasoning_output_tokens":5,"total_tokens":120}},"rate_limits":{"limit_id":"fixture","limit_name":"Fixture","primary":{"used_percent":12.5,"window_minutes":300,"resets_at":1767229200},"secondary":null}}})" << '\n';
            stream.close();

            Database database(L":memory:");
            database.Initialize();
            CodexCollector collector(database, root);
            auto const first = collector.CollectOnce();
            auto const second = collector.CollectOnce();
            std::filesystem::create_directories(root / L"archived_sessions");
            std::filesystem::rename(fixture, root / L"archived_sessions" / fixture.filename());
            auto const afterArchiveMove = collector.CollectOnce();
            auto const totals = database.GetTotals();
            auto const sessions = database.GetRecentSessions();
            auto const tools = database.GetToolCalls(L"11111111-1111-1111-1111-111111111111", 1);
            auto const archived = root / L"archived_sessions" / fixture.filename();
            bool valid = first.usageEvents == 1 && first.promptEvents == 1 && first.toolEvents == 1 &&
                         second.usageEvents == 0 && totals.counts.reportedTotal == 120 &&
                         afterArchiveMove.usageEvents == 0 &&
                         totals.counts.cachedInput == 40 && totals.messages == 1 && totals.toolCalls == 1 &&
                         sessions.size() == 1 && sessions.front().model == L"gpt-test" &&
                         tools.size() == 1 && tools.front().sourcePath == archived.wstring() &&
                         tools.front().inputLength > 0 && tools.front().outputLength > 0;

            std::ofstream partial(archived, std::ios::binary | std::ios::app);
            partial << R"({"timestamp":"2026-01-01T00:00:05.000Z","type":"event_msg","payload":{"type":"token_count")";
            partial.close();
            auto const incomplete = collector.CollectOnce();
            std::ofstream complete(archived, std::ios::binary | std::ios::app);
            complete << R"(,"info":{"total_token_usage":{"input_tokens":150,"cached_input_tokens":50,"cache_write_input_tokens":0,"output_tokens":30,"reasoning_output_tokens":7,"total_tokens":180},"last_token_usage":{"input_tokens":50,"cached_input_tokens":10,"cache_write_input_tokens":0,"output_tokens":10,"reasoning_output_tokens":2,"total_tokens":60}}}})" << '\n';
            complete.close();
            auto const completed = collector.CollectOnce();
            valid = valid && incomplete.usageEvents == 0 && completed.usageEvents == 1 &&
                    database.GetTotals().counts.reportedTotal == 180;

            std::ofstream finalRecord(archived, std::ios::binary | std::ios::app);
            finalRecord << R"({"timestamp":"2026-01-01T00:00:06.000Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":170,"cached_input_tokens":55,"cache_write_input_tokens":0,"output_tokens":40,"reasoning_output_tokens":9,"total_tokens":210},"last_token_usage":{"input_tokens":20,"cached_input_tokens":5,"cache_write_input_tokens":0,"output_tokens":10,"reasoning_output_tokens":2,"total_tokens":30}}}})";
            finalRecord.close();
            auto const withoutNewline = collector.CollectOnce();
            valid = valid && withoutNewline.usageEvents == 1 &&
                    database.GetTotals().counts.reportedTotal == 210;
            std::filesystem::remove_all(root, cleanupError);
            return valid;
        }
        catch (...)
        {
            std::filesystem::remove_all(root, cleanupError);
            return false;
        }
    }

    void CodexCollector::LoadSessionTitles()
    {
        m_titles.clear();
        std::ifstream stream(m_root / L"session_index.jsonl", std::ios::binary);
        std::string line;
        while (std::getline(stream, line))
        {
            if (line.size() > maximumRecordBytes)
            {
                continue;
            }
            auto const object = Parse(line);
            if (!object)
            {
                continue;
            }
            auto const id = String(*object, L"id");
            auto const title = String(*object, L"thread_name");
            if (!id.empty() && !title.empty())
            {
                m_titles.insert_or_assign(id, title);
            }
        }
    }

    void CodexCollector::CollectDirectory(
        std::filesystem::path const& directory,
        CollectionResult& result,
        std::stop_token stopToken)
    {
        std::error_code error;
        if (!std::filesystem::exists(directory, error))
        {
            return;
        }
        std::vector<std::filesystem::path> files;
        for (std::filesystem::recursive_directory_iterator iterator(
                 directory,
                 std::filesystem::directory_options::skip_permission_denied,
                 error), end;
             iterator != end;
             iterator.increment(error))
        {
            if (error)
            {
                error.clear();
                continue;
            }
            auto const name = iterator->path().filename().wstring();
            if (iterator->is_regular_file(error) && iterator->path().extension() == L".jsonl" &&
                name.starts_with(L"rollout-"))
            {
                files.push_back(iterator->path());
            }
        }
        std::sort(files.begin(), files.end());
        for (auto const& file : files)
        {
            if (stopToken.stop_requested()) break;
            CollectFile(file, result, stopToken);
        }
    }

    void CodexCollector::CollectFile(
        std::filesystem::path const& path,
        CollectionResult& result,
        std::stop_token stopToken)
    {
        ++result.filesVisited;
        auto const state = ReadFileState(path);
        if (!state)
        {
            return;
        }

        std::wstring const sourcePath = path.lexically_normal().wstring();
        std::wstring const fileSessionId = SessionIdFromPath(path);
        std::wstring const fileIdentity = state->identity + L":" + fileSessionId;
        auto progress = m_database.GetSourceProgress(sourcePath, fileIdentity);
        if (progress &&
            (progress->fileIdentity != fileIdentity || state->size < progress->offset))
        {
            auto const stalePath = progress->path;
            m_database.Transaction([&] { m_database.ResetSourceFile(stalePath); });
            progress.reset();
        }
        if (progress && progress->path != sourcePath)
        {
            progress->path = sourcePath;
            progress->size = state->size;
            progress->modifiedAt = state->modifiedAt;
            SessionRecord relocated{
                fileSessionId, sourcePath, L"codex", {}, progress->project, progress->model,
                m_deviceId, 0, 0, progress->promptIndex
            };
            if (auto const title = m_titles.find(fileSessionId); title != m_titles.end())
            {
                relocated.title = title->second;
            }
            m_database.Transaction([&]
            {
                m_database.SaveSourceProgress(*progress);
                m_database.UpsertSession(relocated);
            });
        }
        if (progress && progress->offset >= state->size &&
            progress->size == state->size && progress->modifiedAt == state->modifiedAt)
        {
            return;
        }

        SourceProgress current = progress.value_or(SourceProgress{});
        current.path = sourcePath;
        current.fileIdentity = fileIdentity;
        if (!progress)
        {
            current.trackingStarted = false;
        }
        bool sessionMetaLocked = current.offset > 0;
        if (current.sessionId.empty())
        {
            current.sessionId = SessionIdFromPath(path);
        }
        current.sessionId = fileSessionId;

        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            return;
        }
        stream.seekg(current.offset);
        if (!stream)
        {
            return;
        }

        SessionRecord session;
        session.id = current.sessionId;
        session.sourcePath = sourcePath;
        session.project = current.project;
        session.model = current.model;
        session.deviceId = m_deviceId;
        session.messageCount = current.promptIndex;
        if (auto const title = m_titles.find(session.id); title != m_titles.end())
        {
            session.title = title->second;
        }

        std::vector<UsageEvent> usageBatch;
        std::vector<PromptEvent> promptBatch;
        std::vector<ToolEvent> toolBatch;
        std::vector<ToolOutputEvent> toolOutputBatch;
        std::optional<RateLimitSnapshot> limitBatch;
        int64_t batchBytes{};

        auto flush = [&]
        {
            current.size = state->size;
            current.modifiedAt = state->modifiedAt;
            int addedUsage{};
            int addedPrompts{};
            int addedTools{};
            m_database.Transaction([&]
            {
                m_database.UpsertSession(session);
                for (auto const& event : promptBatch)
                {
                    if (m_database.InsertPromptEvent(event)) ++addedPrompts;
                }
                for (auto const& event : usageBatch)
                {
                    if (m_database.InsertUsageEvent(event)) ++addedUsage;
                }
                for (auto const& event : toolBatch)
                {
                    if (m_database.InsertToolEvent(event)) ++addedTools;
                }
                for (auto const& event : toolOutputBatch)
                {
                    m_database.AttachToolOutput(event);
                }
                if (limitBatch) m_database.UpsertRateLimit(*limitBatch);
                m_database.SaveSourceProgress(current);
            });
            result.usageEvents += addedUsage;
            result.promptEvents += addedPrompts;
            result.toolEvents += addedTools;
            usageBatch.clear();
            promptBatch.clear();
            toolBatch.clear();
            toolOutputBatch.clear();
            limitBatch.reset();
            batchBytes = 0;
        };

        auto flushIfNeeded = [&]
        {
            if (batchBytes >= 4 * 1024 * 1024 ||
                usageBatch.size() + promptBatch.size() + toolBatch.size() + toolOutputBatch.size() >= 500)
            {
                flush();
            }
        };

        std::string line;
        std::vector<char> recordBuffer(maximumRecordBytes + 2);
        while (true)
        {
            if (stopToken.stop_requested()) break;
                auto const before = stream.tellg();
                if (before < 0)
                {
                    break;
                }
                int64_t const sourceOffset = static_cast<int64_t>(before);
                stream.getline(recordBuffer.data(), static_cast<std::streamsize>(recordBuffer.size()));
                std::streamsize const extracted = stream.gcount();
                if (stream.bad() || (extracted == 0 && stream.eof()))
                {
                    break;
                }
                if (stream.fail() && !stream.eof())
                {
                    stream.clear();
                    stream.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                    auto const after = stream.tellg();
                    int64_t const nextOffset = after >= 0
                        ? static_cast<int64_t>(after)
                        : state->size;
                    if (nextOffset > state->size)
                    {
                        current.offset = sourceOffset;
                        break;
                    }
                    current.offset = nextOffset;
                    result.bytesRead += nextOffset - sourceOffset;
                    batchBytes += nextOffset - sourceOffset;
                    ++result.oversizedRecords;
                    flushIfNeeded();
                    continue;
                }

                bool const finalWithoutNewline = stream.eof() && !state->trailingNewline;
                size_t const lineBytes = static_cast<size_t>(extracted) -
                    (finalWithoutNewline ? 0u : 1u);
                line.assign(recordBuffer.data(), lineBytes);
                int64_t const recordBytes = static_cast<int64_t>(lineBytes) +
                    (finalWithoutNewline ? 0 : 1);
                int64_t const nextOffset = sourceOffset + recordBytes;
                if (nextOffset > state->size)
                {
                    current.offset = sourceOffset;
                    break;
                }
                if (finalWithoutNewline &&
                    (line.size() > maximumRecordBytes || !Parse(line).has_value()))
                {
                    current.offset = sourceOffset;
                    break;
                }
                current.offset = std::min(nextOffset, state->size);
                result.bytesRead += recordBytes;
                batchBytes += recordBytes;
                if (!IsInteresting(line))
                {
                    flushIfNeeded();
                    continue;
                }
                auto const root = Parse(line);
                if (!root)
                {
                    ++result.malformedRecords;
                    flushIfNeeded();
                    continue;
                }
                auto const payload = Object(*root, L"payload");
                if (!payload)
                {
                    flushIfNeeded();
                    continue;
                }
                auto const outerType = String(*root, L"type");
                auto const payloadType = String(*payload, L"type");
                auto const timestampText = String(*root, L"timestamp");
                int64_t const timestamp = ParseTimestamp(timestampText);
                if (outerType == L"session_meta")
                {
                    auto const id = String(*payload, L"id");
                    if (id == fileSessionId && !sessionMetaLocked)
                    {
                        sessionMetaLocked = true;
                        current.sessionId = fileSessionId;
                        session.id = fileSessionId;
                        if (auto const title = m_titles.find(id); title != m_titles.end())
                        {
                            session.title = title->second;
                        }
                        if (current.sessionCreatedAt == 0 && timestamp)
                        {
                            current.sessionCreatedAt = timestamp;
                            session.startedAt = timestamp;
                        }
                        bool const forked = HasValue(*payload, L"forked_from_id") ||
                                            HasValue(*payload, L"parent_thread_id") ||
                                            HasValue(*payload, L"agent_path");
                        current.forked = forked;
                        current.trackingStarted = !forked;
                        auto const project = String(*payload, L"cwd");
                        if (!project.empty())
                        {
                            current.project = project;
                            session.project = project;
                        }
                    }
                }
                else if (outerType == L"event_msg" && payloadType == L"task_started")
                {
                    double const reportedStart = Number(*payload, L"started_at");
                    if (!current.forked ||
                        (reportedStart >= 0 && current.sessionCreatedAt > 0 &&
                         static_cast<int64_t>(std::floor(reportedStart)) >= current.sessionCreatedAt - 5))
                    {
                        current.trackingStarted = true;
                        auto const turn = String(*payload, L"turn_id");
                        if (!turn.empty()) current.turnId = turn;
                    }
                }
                else if (!current.trackingStarted)
                {
                    flushIfNeeded();
                    continue;
                }
                else if (outerType == L"turn_context")
                {
                    auto const model = String(*payload, L"model");
                    auto const project = String(*payload, L"cwd");
                    auto const turn = String(*payload, L"turn_id");
                    if (!model.empty()) current.model = session.model = model;
                    if (!project.empty()) current.project = session.project = project;
                    if (!turn.empty()) current.turnId = turn;
                }
                else if (outerType == L"event_msg" && payloadType == L"user_message")
                {
                    ++current.promptIndex;
                    session.messageCount = current.promptIndex;
                    promptBatch.push_back({
                        sourcePath, sourceOffset, current.sessionId, L"codex", L"Codex",
                        current.model, current.project, m_deviceId, current.turnId,
                        current.promptIndex, timestamp, LocalDay(timestamp)
                    });
                }
                else if (outerType == L"event_msg" && payloadType == L"token_count")
                {
                    if (auto const info = Object(*payload, L"info"))
                    {
                        if (auto const total = Object(*info, L"total_token_usage"))
                        {
                            TokenCounts const reported = ReadCounts(*total);
                            if (!Equal(reported, current.cumulative))
                            {
                                TokenCounts delta;
                                if (auto const last = Object(*info, L"last_token_usage"))
                                {
                                    delta = ReadCounts(*last);
                                    if (!HasUsage(delta) && !Decreased(reported, current.cumulative))
                                    {
                                        delta = Difference(reported, current.cumulative);
                                    }
                                }
                                else if (!Decreased(reported, current.cumulative))
                                {
                                    delta = Difference(reported, current.cumulative);
                                }
                                current.cumulative = reported;
                                if (HasUsage(delta))
                                {
                                    usageBatch.push_back({
                                        sourcePath, sourceOffset, current.sessionId, L"codex", L"Codex",
                                        current.model, current.project, m_deviceId, current.turnId,
                                        current.promptIndex, timestamp, LocalDay(timestamp), delta
                                    });
                                }
                            }
                        }
                    }

                    if (auto const limits = Object(*payload, L"rate_limits"))
                    {
                        RateLimitSnapshot snapshot;
                        snapshot.limitId = String(*limits, L"limit_id");
                        snapshot.limitName = String(*limits, L"limit_name");
                        snapshot.planType = String(*limits, L"plan_type");
                        snapshot.capturedAt = timestamp;
                        if (auto const primary = Object(*limits, L"primary"))
                        {
                            snapshot.primaryUsedPercent = Number(*primary, L"used_percent");
                            snapshot.primaryWindowMinutes = static_cast<int>(Integer(*primary, L"window_minutes"));
                            snapshot.primaryResetsAt = Integer(*primary, L"resets_at");
                        }
                        if (auto const secondary = Object(*limits, L"secondary"))
                        {
                            snapshot.secondaryUsedPercent = Number(*secondary, L"used_percent");
                            snapshot.secondaryWindowMinutes = static_cast<int>(Integer(*secondary, L"window_minutes"));
                            snapshot.secondaryResetsAt = Integer(*secondary, L"resets_at");
                        }
                        limitBatch = std::move(snapshot);
                    }
                }
                else if (outerType == L"response_item" &&
                         (payloadType == L"function_call" || payloadType == L"custom_tool_call"))
                {
                    auto const name = String(*payload, L"name");
                    if (!name.empty())
                    {
                        toolBatch.push_back({
                            sourcePath, sourceOffset, current.sessionId, L"codex", L"Codex",
                            current.model, current.project, m_deviceId, current.turnId,
                            current.promptIndex, timestamp, LocalDay(timestamp), name,
                            String(*payload, L"call_id"), recordBytes
                        });
                    }
                }
                else if (outerType == L"response_item" &&
                         (payloadType == L"function_call_output" ||
                          payloadType == L"custom_tool_call_output"))
                {
                    auto const callId = String(*payload, L"call_id");
                    if (!callId.empty())
                    {
                        toolOutputBatch.push_back({
                            sourcePath, current.sessionId, callId, sourceOffset, recordBytes
                        });
                    }
                }

                if (current.trackingStarted && timestamp)
                {
                    session.startedAt = session.startedAt == 0
                        ? timestamp
                        : std::min(session.startedAt, timestamp);
                    session.updatedAt = std::max(session.updatedAt, timestamp);
                }
            flushIfNeeded();
        }

        flush();
        ++result.filesChanged;
    }
}
