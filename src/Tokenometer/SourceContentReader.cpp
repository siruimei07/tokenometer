#include "SourceContentReader.h"
#include "WslCodexCollector.h"

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
        constexpr size_t maximumSanitizedCharacters =
            static_cast<size_t>(maximumSegmentBytes) * 2;
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

        std::wstring NormalizeKey(std::wstring_view key)
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
            return normalized;
        }

        bool IsSensitiveKey(std::wstring_view key)
        {
            auto const normalized = NormalizeKey(key);
            constexpr std::array sensitiveSuffixes{
                std::wstring_view{ L"apikey" },
                std::wstring_view{ L"token" },
                std::wstring_view{ L"password" },
                std::wstring_view{ L"authorization" },
                std::wstring_view{ L"secret" },
                std::wstring_view{ L"privatekey" },
                std::wstring_view{ L"secretaccesskey" },
                std::wstring_view{ L"accesskey" },
                std::wstring_view{ L"accesskeyid" },
                std::wstring_view{ L"accountkey" },
                std::wstring_view{ L"cookie" },
                std::wstring_view{ L"passwd" },
                std::wstring_view{ L"pwd" },
                std::wstring_view{ L"passphrase" },
                std::wstring_view{ L"connectionstring" },
                std::wstring_view{ L"databaseurl" },
                std::wstring_view{ L"webhookurl" },
                std::wstring_view{ L"credential" },
                std::wstring_view{ L"credentials" }
            };
            if (normalized == L"key") return true;
            return std::ranges::any_of(sensitiveSuffixes, [&](std::wstring_view suffix)
            {
                return normalized.ends_with(suffix);
            });
        }

        struct RedactionRange
        {
            size_t begin{};
            size_t end{};
        };

        bool IsKeyCharacter(wchar_t character)
        {
            return std::iswalnum(character) || character == L'_' ||
                   character == L'-' || character == L'.';
        }

        bool IsTokenCharacter(wchar_t character)
        {
            return std::iswalnum(character) || character == L'_' || character == L'-';
        }

        bool EqualAsciiInsensitive(wchar_t left, wchar_t right)
        {
            return std::towlower(left) == std::towlower(right);
        }

        bool StartsWithInsensitive(
            std::wstring_view text,
            size_t offset,
            std::wstring_view prefix)
        {
            if (offset > text.size() || prefix.size() > text.size() - offset) return false;
            for (size_t index = 0; index < prefix.size(); ++index)
            {
                if (!EqualAsciiInsensitive(text[offset + index], prefix[index])) return false;
            }
            return true;
        }

        void AddRange(std::vector<RedactionRange>& ranges, size_t begin, size_t end)
        {
            if (begin < end) ranges.push_back({ begin, end });
        }

        size_t SkipHorizontalSpace(std::wstring_view text, size_t offset)
        {
            while (offset < text.size() &&
                   (text[offset] == L' ' || text[offset] == L'\t'))
            {
                ++offset;
            }
            return offset;
        }

        size_t QuotedValueEnd(std::wstring_view text, size_t offset, wchar_t quote)
        {
            bool escaped{};
            while (offset < text.size())
            {
                wchar_t const character = text[offset];
                if (character == quote && !escaped)
                {
                    if (offset + 1 < text.size() && text[offset + 1] == quote)
                    {
                        offset += 2;
                        continue;
                    }
                    break;
                }
                if (character == L'\\')
                {
                    escaped = !escaped;
                }
                else
                {
                    escaped = false;
                }
                ++offset;
            }
            return offset;
        }

        void FindAssignedSecrets(
            std::wstring_view text,
            std::vector<RedactionRange>& ranges)
        {
            size_t offset{};
            while (offset < text.size())
            {
                if (!IsKeyCharacter(text[offset]) ||
                    (offset > 0 && IsKeyCharacter(text[offset - 1])))
                {
                    ++offset;
                    continue;
                }

                size_t const keyBegin = offset;
                while (offset < text.size() && IsKeyCharacter(text[offset])) ++offset;
                size_t const keyEnd = offset;
                auto const key = text.substr(keyBegin, keyEnd - keyBegin);
                if (!IsSensitiveKey(key)) continue;
                auto const normalized = NormalizeKey(key);

                bool const commandOption = key.size() > 2 && key.starts_with(L"--");
                size_t cursor = keyEnd;
                if (cursor < text.size() &&
                    (text[cursor] == L'\'' || text[cursor] == L'\"'))
                {
                    ++cursor;
                }
                size_t const beforeSpace = cursor;
                cursor = SkipHorizontalSpace(text, cursor);
                if (cursor < text.size() &&
                    (text[cursor] == L':' || text[cursor] == L'='))
                {
                    ++cursor;
                }
                else if (!(commandOption && cursor > beforeSpace))
                {
                    continue;
                }
                cursor = SkipHorizontalSpace(text, cursor);
                if (cursor >= text.size() || text[cursor] == L'\r' || text[cursor] == L'\n')
                {
                    continue;
                }

                if (text[cursor] == L'\'' || text[cursor] == L'\"')
                {
                    wchar_t const quote = text[cursor++];
                    AddRange(ranges, cursor, QuotedValueEnd(text, cursor, quote));
                    continue;
                }

                size_t end = cursor;
                bool const headerValue = normalized.ends_with(L"authorization") ||
                                         normalized.ends_with(L"cookie");
                while (end < text.size() && text[end] != L'\r' && text[end] != L'\n' &&
                       text[end] != L'\'' && text[end] != L'\"' &&
                       (headerValue || (!std::iswspace(text[end]) &&
                           text[end] != L',' && text[end] != L';' && text[end] != L'&' &&
                           text[end] != L'}' && text[end] != L']' && text[end] != L')')))
                {
                    ++end;
                }
                while (end > cursor && std::iswspace(text[end - 1])) --end;
                AddRange(ranges, cursor, end);
            }
        }

        void FindAuthorizationSchemes(
            std::wstring_view text,
            std::vector<RedactionRange>& ranges)
        {
            for (size_t offset = 0; offset < text.size(); ++offset)
            {
                std::wstring_view scheme;
                if (StartsWithInsensitive(text, offset, L"bearer")) scheme = L"bearer";
                else if (StartsWithInsensitive(text, offset, L"basic")) scheme = L"basic";
                else continue;

                if ((offset > 0 && std::iswalnum(text[offset - 1])) ||
                    (offset + scheme.size() < text.size() &&
                     std::iswalnum(text[offset + scheme.size()])))
                {
                    continue;
                }
                size_t cursor = offset + scheme.size();
                size_t const beforeSpace = cursor;
                cursor = SkipHorizontalSpace(text, cursor);
                if (cursor == beforeSpace || cursor >= text.size()) continue;

                size_t begin = cursor;
                size_t end{};
                if (text[begin] == L'\'' || text[begin] == L'\"')
                {
                    wchar_t const quote = text[begin++];
                    end = QuotedValueEnd(text, begin, quote);
                }
                else
                {
                    end = begin;
                    while (end < text.size() && !std::iswspace(text[end]) &&
                           text[end] != L'\'' && text[end] != L'\"' &&
                           text[end] != L',' && text[end] != L';')
                    {
                        ++end;
                    }
                }
                auto const credential = text.substr(begin, end - begin);
                bool highConfidence{};
                if (scheme == L"basic")
                {
                    bool upper{};
                    bool lower{};
                    bool marker{};
                    bool valid = credential.size() >= 12;
                    for (wchar_t const character : credential)
                    {
                        upper = upper || std::iswupper(character);
                        lower = lower || std::iswlower(character);
                        marker = marker || std::iswdigit(character) ||
                                 character == L'+' || character == L'/' || character == L'=';
                        valid = valid && (std::iswalnum(character) ||
                            character == L'+' || character == L'/' || character == L'=');
                    }
                    highConfidence = valid && upper && lower && marker;
                }
                else
                {
                    bool const hasDigit = std::ranges::any_of(
                        credential,
                        [](wchar_t character) { return std::iswdigit(character); });
                    bool const hasSeparator = std::ranges::any_of(
                        credential,
                        [](wchar_t character)
                        {
                            return character == L'_' || character == L'-' ||
                                   character == L'.' || character == L'+' || character == L'/';
                        });
                    highConfidence = credential.size() >= 20 ||
                        (credential.size() >= 12 && hasDigit && hasSeparator);
                }
                if (highConfidence)
                {
                    AddRange(ranges, begin, end);
                    offset = end - 1;
                }
            }
        }

        void FindPrefixedTokens(
            std::wstring_view text,
            std::vector<RedactionRange>& ranges)
        {
            for (size_t offset = 0; offset < text.size(); ++offset)
            {
                if (offset > 0 && IsTokenCharacter(text[offset - 1])) continue;

                size_t minimumLength{};
                if (StartsWithInsensitive(text, offset, L"github_pat_")) minimumLength = 20;
                else if (offset + 4 <= text.size() &&
                         StartsWithInsensitive(text, offset, L"gh") &&
                         std::wstring_view(L"pousr").find(static_cast<wchar_t>(
                             std::towlower(text[offset + 2]))) != std::wstring_view::npos &&
                         text[offset + 3] == L'_')
                {
                    minimumLength = 20;
                }
                else if (StartsWithInsensitive(text, offset, L"sk-")) minimumLength = 12;
                else if (StartsWithInsensitive(text, offset, L"xox"))
                {
                    size_t dash = offset + 3;
                    while (dash < text.size() && std::iswalnum(text[dash]) &&
                           dash - offset <= 8)
                    {
                        ++dash;
                    }
                    if (dash >= text.size() || dash == offset + 3 || text[dash] != L'-')
                    {
                        continue;
                    }
                    minimumLength = 16;
                }
                else
                {
                    continue;
                }

                size_t end = offset;
                while (end < text.size() && IsTokenCharacter(text[end])) ++end;
                if (end - offset >= minimumLength)
                {
                    AddRange(ranges, offset, end);
                    offset = end - 1;
                }
            }
        }

        bool IsBase64UrlCharacter(wchar_t character)
        {
            return std::iswalnum(character) || character == L'_' || character == L'-';
        }

        void FindJsonWebTokens(
            std::wstring_view text,
            std::vector<RedactionRange>& ranges)
        {
            for (size_t offset = 0; offset + 3 < text.size(); ++offset)
            {
                if (text.substr(offset, 3) != L"eyJ" ||
                    (offset > 0 && (IsBase64UrlCharacter(text[offset - 1]) ||
                                    text[offset - 1] == L'.')))
                {
                    continue;
                }

                size_t first = offset;
                while (first < text.size() && IsBase64UrlCharacter(text[first])) ++first;
                if (first == text.size() || text[first] != L'.' || first - offset < 4) continue;
                size_t second = first + 1;
                while (second < text.size() && IsBase64UrlCharacter(text[second])) ++second;
                if (second == text.size() || text[second] != L'.' || second - first <= 4) continue;
                size_t end = second + 1;
                while (end < text.size() && IsBase64UrlCharacter(text[end])) ++end;
                if (end - second <= 4 || end - offset < 24) continue;
                AddRange(ranges, offset, end);
                offset = end - 1;
            }
        }

        size_t FindInsensitive(
            std::wstring_view text,
            std::wstring_view needle,
            size_t offset = 0)
        {
            if (needle.empty()) return offset <= text.size() ? offset : std::wstring_view::npos;
            while (offset <= text.size() && needle.size() <= text.size() - offset)
            {
                if (StartsWithInsensitive(text, offset, needle)) return offset;
                ++offset;
            }
            return std::wstring_view::npos;
        }

        void FindPrivateKeyBlocks(
            std::wstring_view text,
            std::vector<RedactionRange>& ranges)
        {
            constexpr std::wstring_view beginPrefix = L"-----BEGIN ";
            constexpr std::wstring_view privateKey = L"PRIVATE KEY";
            size_t offset{};
            while ((offset = FindInsensitive(text, beginPrefix, offset)) != std::wstring_view::npos)
            {
                size_t const labelBegin = offset + beginPrefix.size();
                size_t const labelEnd = text.find(L"-----", labelBegin);
                if (labelEnd == std::wstring_view::npos || labelEnd - labelBegin > 128)
                {
                    offset += beginPrefix.size();
                    continue;
                }
                auto const label = text.substr(labelBegin, labelEnd - labelBegin);
                if (FindInsensitive(label, privateKey) == std::wstring_view::npos)
                {
                    offset = labelEnd + 5;
                    continue;
                }

                std::wstring endMarker = L"-----END ";
                endMarker.append(label);
                endMarker += L"-----";
                size_t const marker = FindInsensitive(text, endMarker, labelEnd + 5);
                size_t const end = marker == std::wstring_view::npos
                    ? text.size()
                    : marker + endMarker.size();
                AddRange(ranges, offset, end);
                offset = end;
            }
        }

        std::wstring RedactPlainText(std::wstring_view text)
        {
            if (text.size() > maximumSanitizedCharacters)
            {
                throw std::runtime_error("The tool content is too large to redact safely");
            }

            std::vector<RedactionRange> ranges;
            FindAssignedSecrets(text, ranges);
            FindAuthorizationSchemes(text, ranges);
            FindPrefixedTokens(text, ranges);
            FindJsonWebTokens(text, ranges);
            FindPrivateKeyBlocks(text, ranges);
            if (ranges.empty()) return std::wstring(text);

            std::ranges::sort(ranges, [](auto const& left, auto const& right)
            {
                return left.begin < right.begin;
            });
            std::wstring result;
            result.reserve(text.size());
            size_t cursor{};
            for (auto const& range : ranges)
            {
                if (range.end <= cursor) continue;
                if (range.begin < cursor)
                {
                    cursor = range.end;
                    continue;
                }
                if (range.begin > cursor)
                {
                    result.append(text.substr(cursor, range.begin - cursor));
                }
                result.append(redacted);
                cursor = range.end;
            }
            if (cursor < text.size()) result.append(text.substr(cursor));
            return result;
        }

        std::wstring RedactStructuredText(std::wstring_view text, unsigned depth = 0);

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
                    else if (auto const child = object.Lookup(key);
                             child && child.ValueType() == json::JsonValueType::String)
                    {
                        object.SetNamedValue(
                            key,
                            json::JsonValue::CreateStringValue(winrt::hstring(
                                RedactStructuredText(child.GetString(), depth + 1))));
                    }
                    else
                    {
                        Redact(child, depth + 1);
                    }
                }
            }
            else if (value.ValueType() == json::JsonValueType::Array)
            {
                auto const array = value.GetArray();
                for (uint32_t index = 0; index < array.Size(); ++index)
                {
                    auto const child = array.GetAt(index);
                    if (child && child.ValueType() == json::JsonValueType::String)
                    {
                        array.SetAt(
                            index,
                            json::JsonValue::CreateStringValue(winrt::hstring(
                                RedactStructuredText(child.GetString(), depth + 1))));
                    }
                    else
                    {
                        Redact(child, depth + 1);
                    }
                }
            }
        }

        std::wstring RedactStructuredText(std::wstring_view text, unsigned depth)
        {
            if (depth > 64)
            {
                throw std::runtime_error("The tool content is nested too deeply to display safely");
            }
            json::IJsonValue value{ nullptr };
            try
            {
                value = json::JsonValue::Parse(winrt::hstring(text));
            }
            catch (...)
            {
                return RedactPlainText(text);
            }
            Redact(value, depth);
            return RedactPlainText(value.Stringify().c_str());
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
            return RedactPlainText(value.Stringify().c_str());
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

    ToolCallContent SourceContentReader::Read(
        ToolCallDetail const& locator,
        std::stop_token stopToken) const
    {
        ValidateRange(locator.inputOffset, locator.inputLength);
        ValidateRange(locator.outputOffset, locator.outputLength);
        if (locator.sourcePath.starts_with(L"wsl://"))
        {
            auto read = [&](int64_t offset, int64_t length)
            {
                auto content = WslCodexCollector::ReadSourceRange(
                    locator.sourcePath,
                    offset,
                    length,
                    stopToken);
                if (!content)
                {
                    throw std::runtime_error("The WSL transcript range is unavailable");
                }
                return std::move(*content);
            };

            ToolCallContent result;
            if (locator.inputLength > 0)
            {
                result.input = ExtractInput(read(locator.inputOffset, locator.inputLength), locator);
            }
            if (locator.outputLength > 0)
            {
                result.output = ExtractOutput(read(locator.outputOffset, locator.outputLength), locator);
            }
            return result;
        }
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

            std::string const input = R"json({"type":"response_item","payload":{"type":"function_call","name":"shell_command","call_id":"call-1","arguments":"{\"command\":\"echo safe && curl -H 'Cookie: nested-cookie-value' && printf 'Bearer nested-bearer-value-123'\",\"OPENAI_API_KEY\":\"secret-key\",\"nested\":{\"GITHUB_TOKEN\":\"secret-token\"},\"encoded\":\"{\\\"password\\\":\\\"encoded-secret-value\\\"}\"}"}})json";
            std::string const output = R"json({"type":"response_item","payload":{"type":"function_call_output","call_id":"call-1","output":"{\"status\":\"ok\",\"proxy_authorization\":\"Bearer secret\",\"database_password\":\"secret-password\",\"cookie\":\"structured-cookie-value\"}"}})json";
            std::string const plainInput = R"json({"type":"response_item","payload":{"type":"function_call","name":"shell_command","call_id":"call-2","arguments":"curl -H 'Authorization: Bearer tiny' -H 'Authorization: ApiKey odd' --password hunter2-secret --token=option-secret-123 PASSWORD='abc''def' TOKEN=\"ghi\"\"jkl\" echo visible-marker"}})json";
            auto const openAiToken = std::string{ "s" } + "k-abcdefghijklmnop";
            auto const openAiProjectToken = std::string{ "s" } + "k-proj-abcdefghijklmnopqrstuv";
            auto const githubClassicToken = std::string{ "gh" } + "p_ABCDEFGHIJKLMNOPQRSTUVWXYZ123456";
            auto const githubOAuthToken = std::string{ "gh" } + "o_ABCDEFGHIJKLMNOPQRSTUVWXYZ123456";
            auto const githubFineGrainedToken =
                std::string{ "github" } + "_pat_ABCDEFGHIJKLMNOPQRSTUVWXYZ123456";
            auto const slackToken = std::string{ "xox" } + "b-123456789012-abcdefghijklmnop";
            auto const jsonWebToken = std::string{ "ey" } +
                "JhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.c2lnbmF0dXJlX3ZhbHVl";
            auto const basicCredential = std::string{ "QW" } + "xhZGRpbjpvcGVuIHNlc2FtZQ==";
            auto const privateKeyBegin = std::string{ "-----BEGIN OPENSSH PRIVATE" } + " KEY-----";
            auto const privateKeyEnd = std::string{ "-----END OPENSSH PRIVATE" } + " KEY-----";
            std::string plainOutput =
                R"json({"type":"response_item","payload":{"type":"function_call_output","call_id":"call-2","output":"status ok\nOPENAI_API_KEY=assigned-secret-value\nAWS_SECRET_ACCESS_KEY=aws-secret-access-value\nAWS_ACCESS_KEY_ID=aws-access-id-value\nAccountKey=account-key-value\nPASSPHRASE=passphrase-value\nDATABASE_URL=postgres://user:database-password@localhost/db\nAuthorization: Basic )json" +
                basicCredential +
                R"json(\nSet-Cookie: session=cookie-secret-value; HttpOnly\n)json" +
                privateKeyBegin + R"json(\nprivate-key-material\n)json" + privateKeyEnd +
                R"json(\nstandalone )json";
            for (auto const& token : {
                     openAiToken,
                     openAiProjectToken,
                     githubClassicToken,
                     githubOAuthToken,
                     githubFineGrainedToken,
                     slackToken,
                     jsonWebToken })
            {
                plainOutput += token + " ";
            }
            plainOutput += R"json(\ntoken usage, basic configuration, basic command-line, and bearer instrument remain visible"}})json";
            auto const fixture = sessionDirectory / L"rollout-reader-fixture.jsonl";
            {
                std::ofstream stream(fixture, std::ios::binary);
                stream << input << '\n' << output << '\n'
                       << plainInput << '\n' << plainOutput << '\n';
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
                content.input.find(L"nested-cookie-value") != std::wstring::npos ||
                content.input.find(L"nested-bearer-value") != std::wstring::npos ||
                content.input.find(L"encoded-secret-value") != std::wstring::npos ||
                content.output.find(L"Bearer secret") != std::wstring::npos ||
                content.output.find(L"secret-password") != std::wstring::npos ||
                content.output.find(L"structured-cookie-value") != std::wstring::npos)
            {
                std::filesystem::remove_all(root);
                return false;
            }

            ToolCallDetail plainLocator = locator;
            plainLocator.callId = L"call-2";
            plainLocator.inputOffset = locator.outputOffset + locator.outputLength;
            plainLocator.inputLength = static_cast<int64_t>(plainInput.size() + 1);
            plainLocator.outputOffset = plainLocator.inputOffset + plainLocator.inputLength;
            plainLocator.outputLength = static_cast<int64_t>(plainOutput.size() + 1);
            auto const plainContent = reader.Read(plainLocator);
            auto const wide = [](std::string const& value)
            {
                return std::wstring(value.begin(), value.end());
            };
            if (plainContent.input.find(L"visible-marker") == std::wstring::npos ||
                plainContent.output.find(
                    L"token usage, basic configuration, basic command-line, and bearer instrument remain visible") ==
                    std::wstring::npos ||
                plainContent.input.find(redacted) == std::wstring::npos ||
                plainContent.output.find(redacted) == std::wstring::npos ||
                plainContent.input.find(L"Bearer tiny") != std::wstring::npos ||
                plainContent.input.find(L"ApiKey odd") != std::wstring::npos ||
                plainContent.input.find(L"hunter2-secret") != std::wstring::npos ||
                plainContent.input.find(L"option-secret-123") != std::wstring::npos ||
                plainContent.input.find(L"abc''def") != std::wstring::npos ||
                plainContent.input.find(L"ghi\"\"jkl") != std::wstring::npos ||
                plainContent.output.find(L"assigned-secret-value") != std::wstring::npos ||
                plainContent.output.find(L"aws-secret-access-value") != std::wstring::npos ||
                plainContent.output.find(L"aws-access-id-value") != std::wstring::npos ||
                plainContent.output.find(L"account-key-value") != std::wstring::npos ||
                plainContent.output.find(L"passphrase-value") != std::wstring::npos ||
                plainContent.output.find(L"database-password") != std::wstring::npos ||
                plainContent.output.find(L"private-key-material") != std::wstring::npos ||
                plainContent.output.find(wide(basicCredential)) != std::wstring::npos ||
                plainContent.output.find(L"cookie-secret-value") != std::wstring::npos ||
                plainContent.output.find(L"HttpOnly") != std::wstring::npos ||
                plainContent.output.find(wide(openAiToken)) != std::wstring::npos ||
                plainContent.output.find(wide(openAiProjectToken)) != std::wstring::npos ||
                plainContent.output.find(wide(githubClassicToken)) != std::wstring::npos ||
                plainContent.output.find(wide(githubOAuthToken)) != std::wstring::npos ||
                plainContent.output.find(wide(githubFineGrainedToken)) != std::wstring::npos ||
                plainContent.output.find(wide(slackToken)) != std::wstring::npos ||
                plainContent.output.find(wide(jsonWebToken)) != std::wstring::npos)
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
