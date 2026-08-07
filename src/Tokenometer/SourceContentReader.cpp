#include "SourceContentReader.h"

#include <windows.h>
#include <shlobj.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace tokenometer
{
    namespace
    {
        namespace json = winrt::Windows::Data::Json;

        constexpr int64_t maximumSegmentBytes = 1024 * 1024;
        constexpr std::wstring_view redacted = L"[REDACTED]";

        class FileHandle final
        {
        public:
            explicit FileHandle(HANDLE value) noexcept : m_value(value) {}
            ~FileHandle()
            {
                if (m_value != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(m_value);
                }
            }

            FileHandle(FileHandle const&) = delete;
            FileHandle& operator=(FileHandle const&) = delete;

            [[nodiscard]] HANDLE Get() const noexcept { return m_value; }

        private:
            HANDLE m_value{ INVALID_HANDLE_VALUE };
        };

        std::filesystem::path DefaultCodexRoot()
        {
            DWORD const environmentLength = GetEnvironmentVariableW(L"CODEX_HOME", nullptr, 0);
            if (environmentLength > 1)
            {
                std::wstring value(static_cast<size_t>(environmentLength), L'\0');
                DWORD const copied = GetEnvironmentVariableW(
                    L"CODEX_HOME",
                    value.data(),
                    environmentLength);
                if (copied > 0 && copied < environmentLength)
                {
                    value.resize(copied);
                    return value;
                }
            }

            PWSTR profile{};
            if (FAILED(SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, &profile)))
            {
                throw std::runtime_error("The Windows profile directory is unavailable");
            }
            std::filesystem::path result(profile);
            CoTaskMemFree(profile);
            return result / L".codex";
        }

        bool EqualPathComponent(
            std::filesystem::path const& left,
            std::filesystem::path const& right) noexcept
        {
            auto const leftText = left.native();
            auto const rightText = right.native();
            return CompareStringOrdinal(
                       leftText.c_str(),
                       static_cast<int>(leftText.size()),
                       rightText.c_str(),
                       static_cast<int>(rightText.size()),
                       TRUE) == CSTR_EQUAL;
        }

        std::filesystem::path OpenedPath(HANDLE file)
        {
            DWORD const required = GetFinalPathNameByHandleW(
                file,
                nullptr,
                0,
                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (required == 0)
            {
                throw std::runtime_error("The opened transcript path is unavailable");
            }
            std::wstring value(static_cast<size_t>(required) + 1, L'\0');
            DWORD const copied = GetFinalPathNameByHandleW(
                file,
                value.data(),
                static_cast<DWORD>(value.size()),
                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (copied == 0 || copied >= value.size())
            {
                throw std::runtime_error("The opened transcript path is unavailable");
            }
            value.resize(copied);
            constexpr std::wstring_view uncPrefix = L"\\\\?\\UNC\\";
            constexpr std::wstring_view devicePrefix = L"\\\\?\\";
            if (value.starts_with(uncPrefix))
            {
                value = L"\\\\" + value.substr(uncPrefix.size());
            }
            else if (value.starts_with(devicePrefix))
            {
                value.erase(0, devicePrefix.size());
            }
            return value;
        }

        bool SamePath(
            std::filesystem::path const& left,
            std::filesystem::path const& right) noexcept
        {
            auto leftPart = left.begin();
            auto rightPart = right.begin();
            while (leftPart != left.end() && rightPart != right.end())
            {
                if (!EqualPathComponent(*leftPart, *rightPart)) return false;
                ++leftPart;
                ++rightPart;
            }
            return leftPart == left.end() && rightPart == right.end();
        }

        bool IsWithin(
            std::filesystem::path const& candidate,
            std::filesystem::path const& directory) noexcept
        {
            auto candidatePart = candidate.begin();
            for (auto directoryPart = directory.begin(); directoryPart != directory.end();
                 ++directoryPart, ++candidatePart)
            {
                if (candidatePart == candidate.end() || !EqualPathComponent(*candidatePart, *directoryPart))
                {
                    return false;
                }
            }
            return candidatePart != candidate.end();
        }

        std::wstring Lower(std::wstring_view value)
        {
            std::wstring result(value);
            std::ranges::transform(result, result.begin(), [](wchar_t character)
            {
                return static_cast<wchar_t>(std::towlower(character));
            });
            return result;
        }

        bool IsRolloutFile(std::filesystem::path const& path)
        {
            auto const name = Lower(path.filename().wstring());
            constexpr std::wstring_view prefix = L"rollout-";
            constexpr std::wstring_view suffix = L".jsonl";
            return name.size() > prefix.size() + suffix.size() &&
                   name.starts_with(prefix) && name.ends_with(suffix);
        }

        std::filesystem::path CanonicalAllowedPath(
            std::filesystem::path const& codexRoot,
            std::filesystem::path const& requested)
        {
            std::error_code error;
            auto const candidate = std::filesystem::canonical(requested, error);
            if (error || !std::filesystem::is_regular_file(candidate, error) || error ||
                !IsRolloutFile(candidate))
            {
                throw std::runtime_error("The source transcript is not an allowed Codex rollout file");
            }

            bool allowed{};
            for (auto const* directoryName : { L"sessions", L"archived_sessions" })
            {
                error.clear();
                auto const directory = std::filesystem::canonical(codexRoot / directoryName, error);
                if (!error && IsWithin(candidate, directory))
                {
                    allowed = true;
                    break;
                }
            }
            if (!allowed)
            {
                throw std::runtime_error("The source transcript is outside the Codex transcript directories");
            }
            return candidate;
        }

        void ValidateRange(int64_t offset, int64_t length)
        {
            if (offset < 0 || length < 0 || length > maximumSegmentBytes)
            {
                throw std::runtime_error("The source transcript locator is outside the safe read range");
            }
        }

        std::string ReadSegment(HANDLE file, int64_t fileSize, int64_t offset, int64_t length)
        {
            if (length == 0)
            {
                return {};
            }
            if (offset > fileSize || length > fileSize - offset)
            {
                throw std::runtime_error("The source transcript locator exceeds the file size");
            }

            LARGE_INTEGER position{};
            position.QuadPart = offset;
            if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
            {
                throw std::runtime_error("The source transcript could not be positioned");
            }

            std::string result(static_cast<size_t>(length), '\0');
            size_t total{};
            while (total < result.size())
            {
                DWORD read{};
                DWORD const requested = static_cast<DWORD>(result.size() - total);
                if (!ReadFile(file, result.data() + total, requested, &read, nullptr) || read == 0)
                {
                    throw std::runtime_error("The source transcript record could not be read");
                }
                total += read;
            }

            if (!result.empty() && result.back() == '\n') result.pop_back();
            if (!result.empty() && result.back() == '\r') result.pop_back();
            if (result.find_first_of("\r\n") != std::string::npos ||
                result.find('\0') != std::string::npos)
            {
                throw std::runtime_error("The source transcript locator is not one JSONL record");
            }
            return result;
        }

        std::wstring String(json::JsonObject const& object, wchar_t const* name)
        {
            if (!object.HasKey(name)) return {};
            auto const value = object.Lookup(name);
            return value && value.ValueType() == json::JsonValueType::String
                ? std::wstring(value.GetString())
                : std::wstring{};
        }

        json::JsonObject Object(json::JsonObject const& object, wchar_t const* name)
        {
            if (!object.HasKey(name))
            {
                throw std::runtime_error("The source transcript record has no payload");
            }
            auto const value = object.Lookup(name);
            if (!value || value.ValueType() != json::JsonValueType::Object)
            {
                throw std::runtime_error("The source transcript payload is invalid");
            }
            return value.GetObject();
        }

        bool IsSensitiveKey(std::wstring_view key)
        {
            std::wstring normalized;
            normalized.reserve(key.size());
            for (wchar_t character : key)
            {
                if (std::iswalnum(character))
                {
                    normalized.push_back(static_cast<wchar_t>(std::towlower(character)));
                }
            }
            constexpr std::array sensitiveSuffixes{
                std::wstring_view{ L"apikey" },
                std::wstring_view{ L"token" },
                std::wstring_view{ L"password" },
                std::wstring_view{ L"authorization" },
                std::wstring_view{ L"secret" },
                std::wstring_view{ L"privatekey" }
            };
            if (normalized == L"key") return true;
            return std::ranges::any_of(sensitiveSuffixes, [&](std::wstring_view suffix)
            {
                return normalized.ends_with(suffix);
            });
        }

        void Redact(json::IJsonValue const& value, unsigned depth = 0)
        {
            if (!value) return;
            if (depth > 64)
            {
                throw std::runtime_error("The tool content is nested too deeply to display safely");
            }
            if (value.ValueType() == json::JsonValueType::Object)
            {
                auto const object = value.GetObject();
                std::vector<winrt::hstring> keys;
                keys.reserve(object.Size());
                for (auto const& item : object)
                {
                    keys.push_back(item.Key());
                }
                for (auto const& key : keys)
                {
                    if (IsSensitiveKey(key))
                    {
                        object.SetNamedValue(
                            key,
                            json::JsonValue::CreateStringValue(winrt::hstring(redacted)));
                    }
                    else
                    {
                        Redact(object.Lookup(key), depth + 1);
                    }
                }
            }
            else if (value.ValueType() == json::JsonValueType::Array)
            {
                auto const array = value.GetArray();
                for (uint32_t index = 0; index < array.Size(); ++index)
                {
                    Redact(array.GetAt(index), depth + 1);
                }
            }
        }

        std::wstring RedactStructuredText(std::wstring_view text)
        {
            json::IJsonValue value{ nullptr };
            try
            {
                value = json::JsonValue::Parse(winrt::hstring(text));
            }
            catch (...)
            {
                return std::wstring(text);
            }
            Redact(value);
            return value.Stringify().c_str();
        }

        std::wstring DisplayValue(json::IJsonValue const& value)
        {
            if (!value)
            {
                throw std::runtime_error("The source transcript field is missing");
            }
            if (value.ValueType() == json::JsonValueType::String)
            {
                return RedactStructuredText(value.GetString());
            }
            Redact(value);
            return value.Stringify().c_str();
        }

        std::wstring ExtractInput(std::string_view record, ToolCallDetail const& locator)
        {
            auto const root = json::JsonObject::Parse(winrt::to_hstring(record));
            if (String(root, L"type") != L"response_item")
            {
                throw std::runtime_error("The input locator does not identify a response item");
            }
            auto const payload = Object(root, L"payload");
            auto const type = String(payload, L"type");
            if (type != L"function_call" && type != L"custom_tool_call")
            {
                throw std::runtime_error("The input locator does not identify a tool call");
            }
            if (!locator.callId.empty() && String(payload, L"call_id") != locator.callId)
            {
                throw std::runtime_error("The input locator call identifier does not match");
            }
            if (!locator.name.empty() && String(payload, L"name") != locator.name)
            {
                throw std::runtime_error("The input locator tool name does not match");
            }
            wchar_t const* field = type == L"function_call" ? L"arguments" : L"input";
            if (!payload.HasKey(field))
            {
                throw std::runtime_error("The tool call has no displayable input");
            }
            return DisplayValue(payload.Lookup(field));
        }

        std::wstring ExtractOutput(std::string_view record, ToolCallDetail const& locator)
        {
            auto const root = json::JsonObject::Parse(winrt::to_hstring(record));
            if (String(root, L"type") != L"response_item")
            {
                throw std::runtime_error("The output locator does not identify a response item");
            }
            auto const payload = Object(root, L"payload");
            auto const type = String(payload, L"type");
            if (type != L"function_call_output" && type != L"custom_tool_call_output")
            {
                throw std::runtime_error("The output locator does not identify a tool result");
            }
            if (!locator.callId.empty() && String(payload, L"call_id") != locator.callId)
            {
                throw std::runtime_error("The output locator call identifier does not match");
            }
            if (!payload.HasKey(L"output"))
            {
                throw std::runtime_error("The tool result has no displayable output");
            }
            return DisplayValue(payload.Lookup(L"output"));
        }

        bool Rejects(SourceContentReader const& reader, ToolCallDetail const& locator)
        {
            try
            {
                (void)reader.Read(locator);
                return false;
            }
            catch (...)
            {
                return true;
            }
        }
    }

    SourceContentReader::SourceContentReader() : SourceContentReader(DefaultCodexRoot()) {}

    SourceContentReader::SourceContentReader(std::filesystem::path codexRoot)
        : m_codexRoot(std::move(codexRoot))
    {
    }

    ToolCallContent SourceContentReader::Read(ToolCallDetail const& locator) const
    {
        ValidateRange(locator.inputOffset, locator.inputLength);
        ValidateRange(locator.outputOffset, locator.outputLength);
        auto const sourcePath = CanonicalAllowedPath(m_codexRoot, locator.sourcePath);

        FileHandle file(CreateFileW(
            sourcePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        if (file.Get() == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error("The source transcript could not be opened");
        }
        if (!SamePath(OpenedPath(file.Get()), sourcePath))
        {
            throw std::runtime_error("The opened transcript no longer matches the allowed path");
        }

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.Get(), &size) || size.QuadPart < 0)
        {
            throw std::runtime_error("The source transcript size is unavailable");
        }

        ToolCallContent result;
        if (locator.inputLength > 0)
        {
            result.input = ExtractInput(
                ReadSegment(file.Get(), size.QuadPart, locator.inputOffset, locator.inputLength),
                locator);
        }
        if (locator.outputLength > 0)
        {
            result.output = ExtractOutput(
                ReadSegment(file.Get(), size.QuadPart, locator.outputOffset, locator.outputLength),
                locator);
        }
        return result;
    }

    bool SourceContentReader::SelfTest()
    {
        std::filesystem::path root;
        try
        {
            root = std::filesystem::temp_directory_path() /
                   (L"Tokenometer-SourceReader-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                    std::to_wstring(GetTickCount64()));
            auto const sessionDirectory = root / L"sessions" / L"2026" / L"08" / L"08";
            auto const archiveDirectory = root / L"archived_sessions";
            std::filesystem::create_directories(sessionDirectory);
            std::filesystem::create_directories(archiveDirectory);

            std::string const input = R"json({"type":"response_item","payload":{"type":"function_call","name":"shell_command","call_id":"call-1","arguments":"{\"command\":\"echo safe\",\"OPENAI_API_KEY\":\"secret-key\",\"nested\":{\"GITHUB_TOKEN\":\"secret-token\"}}"}})json";
            std::string const output = R"json({"type":"response_item","payload":{"type":"function_call_output","call_id":"call-1","output":"{\"status\":\"ok\",\"proxy_authorization\":\"Bearer secret\",\"database_password\":\"secret-password\"}"}})json";
            auto const fixture = sessionDirectory / L"rollout-reader-fixture.jsonl";
            {
                std::ofstream stream(fixture, std::ios::binary);
                stream << input << '\n' << output << '\n';
            }

            ToolCallDetail locator;
            locator.sourcePath = fixture.wstring();
            locator.name = L"shell_command";
            locator.callId = L"call-1";
            locator.inputLength = static_cast<int64_t>(input.size() + 1);
            locator.outputOffset = locator.inputLength;
            locator.outputLength = static_cast<int64_t>(output.size() + 1);

            SourceContentReader reader(root);
            auto const content = reader.Read(locator);
            if (content.input.find(L"echo safe") == std::wstring::npos ||
                content.output.find(L"\"status\":\"ok\"") == std::wstring::npos ||
                content.input.find(redacted) == std::wstring::npos ||
                content.output.find(redacted) == std::wstring::npos ||
                content.input.find(L"secret-key") != std::wstring::npos ||
                content.input.find(L"secret-token") != std::wstring::npos ||
                content.output.find(L"Bearer secret") != std::wstring::npos ||
                content.output.find(L"secret-password") != std::wstring::npos)
            {
                std::filesystem::remove_all(root);
                return false;
            }

            auto invalid = locator;
            invalid.inputOffset = -1;
            if (!Rejects(reader, invalid))
            {
                std::filesystem::remove_all(root);
                return false;
            }
            invalid = locator;
            invalid.inputLength = maximumSegmentBytes + 1;
            if (!Rejects(reader, invalid))
            {
                std::filesystem::remove_all(root);
                return false;
            }

            auto const outsideDirectory = root / L"outside";
            std::filesystem::create_directories(outsideDirectory);
            auto const outside = outsideDirectory / L"rollout-reader-fixture.jsonl";
            std::filesystem::copy_file(fixture, outside);
            invalid = locator;
            invalid.sourcePath = outside.wstring();
            if (!Rejects(reader, invalid))
            {
                std::filesystem::remove_all(root);
                return false;
            }

            auto const forbidden = root / L"auth.json";
            std::filesystem::copy_file(fixture, forbidden);
            invalid.sourcePath = forbidden.wstring();
            if (!Rejects(reader, invalid))
            {
                std::filesystem::remove_all(root);
                return false;
            }

            std::filesystem::remove_all(root);
            return true;
        }
        catch (...)
        {
            std::error_code ignored;
            if (!root.empty()) std::filesystem::remove_all(root, ignored);
            return false;
        }
    }

    bool TestSourceContentReader()
    {
        return SourceContentReader::SelfTest();
    }
}
