#include "ChatGPTExportImporter.h"

#include "Database.h"

#include <windows.h>
#include <bcrypt.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tokenometer
{
    namespace
    {
        namespace json = winrt::Windows::Data::Json;

        constexpr size_t maximumConversationBytes = 64 * 1024 * 1024;
        constexpr size_t maximumBranchNodes = 100'000;
        constexpr size_t maximumIdentifierChars = 512;
        constexpr size_t maximumModelChars = 256;

        struct FileMetadata
        {
            int64_t size{};
            int64_t modifiedAt{};

            bool operator==(FileMetadata const&) const = default;
        };

        struct ParsedConversation
        {
            ChatGPTSessionEstimate session;
            std::vector<ChatGPTPromptEstimate> prompts;
        };

        struct TextMeasure
        {
            bool present{};
            size_t utf8Bytes{};
        };

        struct HashResources
        {
            BCRYPT_ALG_HANDLE algorithm{};
            BCRYPT_HASH_HANDLE hash{};

            ~HashResources()
            {
                if (hash) BCryptDestroyHash(hash);
                if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
            }
        };

        [[noreturn]] void InvalidExport()
        {
            throw std::runtime_error("The selected file is not a valid ChatGPT conversation export");
        }

        void ThrowIfCancelled(std::stop_token stopToken)
        {
            if (stopToken.stop_requested())
            {
                throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            }
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

        FileMetadata ReadMetadata(std::filesystem::path const& path)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
                (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                throw std::runtime_error("The selected ChatGPT export file is unavailable");
            }
            ULARGE_INTEGER size{};
            size.LowPart = data.nFileSizeLow;
            size.HighPart = data.nFileSizeHigh;
            if (size.QuadPart > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            {
                throw std::runtime_error("The selected ChatGPT export file is too large");
            }
            return { static_cast<int64_t>(size.QuadPart), FileTimeToUnix(data.ftLastWriteTime) };
        }

        std::wstring Lower(std::wstring value)
        {
            std::ranges::transform(value, value.begin(), [](wchar_t value)
            {
                return static_cast<wchar_t>(std::towlower(value));
            });
            return value;
        }

        bool IsSupportedFileName(std::filesystem::path const& path)
        {
            if (Lower(path.filename().wstring()) == L"conversations.json") return true;
            if (Lower(path.extension().wstring()) != L".json") return false;

            auto stem = Lower(path.stem().wstring());
            if (stem.starts_with(L"conversations"))
            {
                stem.erase(0, std::wstring_view{ L"conversations" }.size());
                while (!stem.empty() && (stem.front() == L'-' || stem.front() == L'_' || stem.front() == L'.'))
                {
                    stem.erase(stem.begin());
                }
            }
            return !stem.empty() && std::ranges::all_of(stem, [](wchar_t value)
            {
                return value >= L'0' && value <= L'9';
            });
        }

        std::wstring NormalizeAccount(std::wstring_view value)
        {
            auto first = value.begin();
            auto last = value.end();
            while (first != last && std::iswspace(*first)) ++first;
            while (last != first && std::iswspace(*(last - 1))) --last;
            std::wstring result(first, last);
            if (result.empty() || result.size() > 256 || result.find(L'\0') != result.npos)
            {
                throw std::invalid_argument("A valid ChatGPT account label is required");
            }
            return result;
        }

        std::wstring Sha256(std::filesystem::path const& path, std::stop_token stopToken)
        {
            ThrowIfCancelled(stopToken);
            HashResources resources;
            if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                    &resources.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
            {
                throw std::runtime_error("SHA-256 is unavailable");
            }

            DWORD objectLength{};
            DWORD hashLength{};
            DWORD received{};
            if (!BCRYPT_SUCCESS(BCryptGetProperty(
                    resources.algorithm, BCRYPT_OBJECT_LENGTH,
                    reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &received, 0)) ||
                !BCRYPT_SUCCESS(BCryptGetProperty(
                    resources.algorithm, BCRYPT_HASH_LENGTH,
                    reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &received, 0)))
            {
                throw std::runtime_error("SHA-256 initialization failed");
            }

            std::vector<unsigned char> object(objectLength);
            std::vector<unsigned char> digest(hashLength);
            if (!BCRYPT_SUCCESS(BCryptCreateHash(
                    resources.algorithm, &resources.hash, object.data(), objectLength, nullptr, 0, 0)))
            {
                throw std::runtime_error("SHA-256 initialization failed");
            }

            std::ifstream stream(path, std::ios::binary);
            if (!stream) throw std::runtime_error("The selected ChatGPT export file cannot be read");
            std::array<char, 64 * 1024> buffer{};
            while (stream)
            {
                ThrowIfCancelled(stopToken);
                stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                auto const size = stream.gcount();
                if (size > 0 && !BCRYPT_SUCCESS(BCryptHashData(
                        resources.hash,
                        reinterpret_cast<PUCHAR>(buffer.data()),
                        static_cast<ULONG>(size),
                        0)))
                {
                    throw std::runtime_error("SHA-256 calculation failed");
                }
            }
            if (!stream.eof()) throw std::runtime_error("The selected ChatGPT export file cannot be read");
            if (!BCRYPT_SUCCESS(BCryptFinishHash(
                    resources.hash, digest.data(), static_cast<ULONG>(digest.size()), 0)))
            {
                throw std::runtime_error("SHA-256 calculation failed");
            }

            constexpr wchar_t digits[] = L"0123456789abcdef";
            std::wstring result;
            result.reserve(digest.size() * 2);
            for (auto byte : digest)
            {
                result.push_back(digits[byte >> 4]);
                result.push_back(digits[byte & 0x0f]);
            }
            return result;
        }

        std::optional<json::JsonObject> Object(json::JsonObject const& object, wchar_t const* name)
        {
            if (!object.HasKey(name)) return std::nullopt;
            auto const value = object.Lookup(name);
            return value && value.ValueType() == json::JsonValueType::Object
                ? std::optional{ value.GetObject() }
                : std::nullopt;
        }

        std::wstring String(json::JsonObject const& object, wchar_t const* name)
        {
            if (!object.HasKey(name)) return {};
            auto const value = object.Lookup(name);
            return value && value.ValueType() == json::JsonValueType::String
                ? std::wstring(value.GetString())
                : std::wstring{};
        }

        bool Boolean(json::JsonObject const& object, wchar_t const* name)
        {
            if (!object.HasKey(name)) return false;
            auto const value = object.Lookup(name);
            return value && value.ValueType() == json::JsonValueType::Boolean && value.GetBoolean();
        }

        int64_t Timestamp(json::JsonObject const& object, wchar_t const* name)
        {
            if (!object.HasKey(name)) return 0;
            auto const value = object.Lookup(name);
            if (!value || value.ValueType() != json::JsonValueType::Number) return 0;
            double const number = value.GetNumber();
            return std::isfinite(number) && number > 0 &&
                   number <= static_cast<double>(std::numeric_limits<int64_t>::max())
                ? static_cast<int64_t>(std::floor(number))
                : 0;
        }

        size_t Utf8Bytes(std::wstring_view value)
        {
            if (value.empty()) return 0;
            int const size = WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                nullptr, 0, nullptr, nullptr);
            return size > 0 ? static_cast<size_t>(size) : 0;
        }

        TextMeasure MeasureText(json::JsonObject const& message)
        {
            auto const content = Object(message, L"content");
            if (!content) return {};
            if (content->HasKey(L"parts"))
            {
                auto const partsValue = content->Lookup(L"parts");
                if (partsValue && partsValue.ValueType() == json::JsonValueType::Array)
                {
                    TextMeasure result;
                    for (auto const& part : partsValue.GetArray())
                    {
                        if (!part || part.ValueType() != json::JsonValueType::String) continue;
                        if (result.present) ++result.utf8Bytes;
                        result.present = true;
                        result.utf8Bytes += Utf8Bytes(std::wstring_view{ part.GetString() });
                    }
                    if (result.present) return result;
                }
            }
            if (content->HasKey(L"text"))
            {
                auto const text = content->Lookup(L"text");
                if (text && text.ValueType() == json::JsonValueType::String)
                {
                    return { true, Utf8Bytes(std::wstring_view{ text.GetString() }) };
                }
            }
            return {};
        }

        int64_t EstimateMessageTokens(std::wstring_view role, size_t textBytes)
        {
            if (textBytes == 0) return 0;
            // Deliberately estimated: UTF-8 bytes of role + visible text + four framing bytes, /4 rounded up.
            size_t const bytes = textBytes + Utf8Bytes(role) + 4;
            return static_cast<int64_t>((bytes + 3) / 4);
        }

        std::wstring LocalDay(int64_t timestamp)
        {
            if (timestamp <= 0) return L"unknown";
            ULARGE_INTEGER ticks{};
            ticks.QuadPart = (static_cast<uint64_t>(timestamp) + 11'644'473'600ULL) * 10'000'000ULL;
            FILETIME utc{ ticks.LowPart, ticks.HighPart };
            SYSTEMTIME utcTime{};
            SYSTEMTIME local{};
            DYNAMIC_TIME_ZONE_INFORMATION timeZone{};
            if (!FileTimeToSystemTime(&utc, &utcTime) ||
                GetDynamicTimeZoneInformation(&timeZone) == TIME_ZONE_ID_INVALID ||
                !SystemTimeToTzSpecificLocalTimeEx(&timeZone, &utcTime, &local))
            {
                return L"unknown";
            }
            std::array<wchar_t, 16> value{};
            swprintf_s(value.data(), value.size(), L"%04u-%02u-%02u", local.wYear, local.wMonth, local.wDay);
            return value.data();
        }

        bool LooksLikeConversation(json::JsonObject const& conversation)
        {
            auto id = String(conversation, L"conversation_id");
            if (id.empty()) id = String(conversation, L"id");
            return !id.empty() && Object(conversation, L"mapping").has_value() &&
                   !String(conversation, L"current_node").empty();
        }

        std::optional<ParsedConversation> ParseConversation(
            json::JsonObject const& conversation,
            std::wstring_view accountId,
            std::stop_token stopToken)
        {
            ThrowIfCancelled(stopToken);
            auto sessionId = String(conversation, L"conversation_id");
            if (sessionId.empty()) sessionId = String(conversation, L"id");
            auto const mapping = Object(conversation, L"mapping");
            auto current = String(conversation, L"current_node");
            if (sessionId.empty() || sessionId.size() > maximumIdentifierChars ||
                !mapping || current.empty() || current.size() > maximumIdentifierChars)
            {
                return std::nullopt;
            }

            std::vector<std::pair<std::wstring, json::JsonObject>> branch;
            std::unordered_set<std::wstring> seen;
            while (!current.empty())
            {
                ThrowIfCancelled(stopToken);
                if (branch.size() >= maximumBranchNodes || !seen.insert(current).second)
                {
                    return std::nullopt;
                }
                winrt::hstring const key{ current };
                if (!mapping->HasKey(key)) return std::nullopt;
                auto const nodeValue = mapping->Lookup(key);
                if (!nodeValue || nodeValue.ValueType() != json::JsonValueType::Object)
                {
                    return std::nullopt;
                }
                auto const node = nodeValue.GetObject();
                branch.emplace_back(current, node);
                if (!node.HasKey(L"parent")) break;
                auto const parent = node.Lookup(L"parent");
                if (!parent || parent.ValueType() == json::JsonValueType::Null) break;
                if (parent.ValueType() != json::JsonValueType::String) return std::nullopt;
                current = parent.GetString();
                if (current.size() > maximumIdentifierChars) return std::nullopt;
            }
            std::ranges::reverse(branch);

            auto defaultModel = String(conversation, L"default_model_slug");
            if (defaultModel.empty()) defaultModel = String(conversation, L"model_slug");
            if (defaultModel.size() > maximumModelChars) defaultModel.clear();
            if (defaultModel.empty()) defaultModel = L"Unknown";
            int64_t const conversationCreated = Timestamp(conversation, L"create_time");
            int64_t const conversationUpdated = Timestamp(conversation, L"update_time");
            int64_t firstMessage{};
            int64_t lastMessage{};
            int promptIndex{};
            std::optional<ChatGPTPromptEstimate> currentPrompt;
            ParsedConversation result;

            auto finishPrompt = [&]
            {
                if (!currentPrompt) return;
                result.prompts.push_back(std::move(*currentPrompt));
                currentPrompt.reset();
            };

            for (auto const& [nodeKey, node] : branch)
            {
                ThrowIfCancelled(stopToken);
                auto const message = Object(node, L"message");
                if (!message) continue;
                auto const author = Object(*message, L"author");
                auto const metadata = Object(*message, L"metadata");
                if (!author || (metadata && Boolean(*metadata, L"is_visually_hidden_from_conversation")))
                {
                    continue;
                }
                auto const role = String(*author, L"role");
                if (role != L"user" && role != L"assistant") continue;

                int64_t const messageTime = Timestamp(*message, L"create_time");
                if (messageTime > 0)
                {
                    if (firstMessage == 0 || messageTime < firstMessage) firstMessage = messageTime;
                    lastMessage = std::max(lastMessage, messageTime);
                }
                auto const text = MeasureText(*message);
                int64_t const estimate = EstimateMessageTokens(role, text.utf8Bytes);

                if (role == L"user")
                {
                    finishPrompt();
                    ChatGPTPromptEstimate prompt;
                    prompt.sessionId = sessionId;
                    prompt.turnId = String(*message, L"id");
                    if (prompt.turnId.empty()) prompt.turnId = String(node, L"id");
                    if (prompt.turnId.empty()) prompt.turnId = nodeKey;
                    if (prompt.turnId.size() > maximumIdentifierChars) prompt.turnId.clear();
                    prompt.promptIndex = ++promptIndex;
                    prompt.timestamp = messageTime > 0 ? messageTime : conversationCreated;
                    prompt.day = LocalDay(prompt.timestamp);
                    prompt.model = defaultModel;
                    prompt.messages = 1;
                    prompt.estimatedInputTokens = estimate;
                    currentPrompt = std::move(prompt);
                    continue;
                }

                if (!currentPrompt || !text.present || text.utf8Bytes == 0) continue;
                currentPrompt->estimatedOutputTokens += estimate;
                ++currentPrompt->messages;
                if (metadata)
                {
                    auto model = String(*metadata, L"model_slug");
                    if (model.empty()) model = String(*metadata, L"default_model_slug");
                    if (model.size() > maximumModelChars) model.clear();
                    if (!model.empty()) currentPrompt->model = std::move(model);
                }
            }
            finishPrompt();
            if (result.prompts.empty()) return std::nullopt;

            result.session.id = std::move(sessionId);
            result.session.accountId = accountId;
            result.session.startedAt = conversationCreated > 0 ? conversationCreated : firstMessage;
            result.session.updatedAt = conversationUpdated > 0 ? conversationUpdated : lastMessage;
            result.session.prompts = static_cast<int64_t>(result.prompts.size());
            result.session.model = defaultModel;
            for (auto const& prompt : result.prompts)
            {
                result.session.messages += prompt.messages;
                result.session.estimatedInputTokens += prompt.estimatedInputTokens;
                result.session.estimatedOutputTokens += prompt.estimatedOutputTokens;
                if (!prompt.model.empty() && prompt.model != L"Unknown") result.session.model = prompt.model;
            }
            return result;
        }

        void ForEachObject(
            std::filesystem::path const& path,
            std::function<void(json::JsonObject const&)> const& callback,
            std::stop_token stopToken)
        {
            ThrowIfCancelled(stopToken);
            std::ifstream stream(path, std::ios::binary);
            if (!stream) throw std::runtime_error("The selected ChatGPT export file cannot be read");
            std::array<char, 3> prefix{};
            stream.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
            auto const prefixSize = stream.gcount();
            stream.clear();
            if (prefixSize != 3 || static_cast<unsigned char>(prefix[0]) != 0xef ||
                static_cast<unsigned char>(prefix[1]) != 0xbb ||
                static_cast<unsigned char>(prefix[2]) != 0xbf)
            {
                stream.seekg(0);
            }

            bool opened{};
            bool closed{};
            bool collecting{};
            bool inString{};
            bool escaped{};
            bool needsValue{ true };
            bool sawValue{};
            size_t depth{};
            std::string objectText;
            char value{};
            size_t bytesUntilCancellationCheck = 64 * 1024;
            while (stream.get(value))
            {
                if (--bytesUntilCancellationCheck == 0)
                {
                    ThrowIfCancelled(stopToken);
                    bytesUntilCancellationCheck = 64 * 1024;
                }
                if (collecting)
                {
                    if (objectText.size() >= maximumConversationBytes)
                    {
                        throw std::runtime_error("A ChatGPT conversation exceeds the 64 MiB import limit");
                    }
                    objectText.push_back(value);
                    if (inString)
                    {
                        if (escaped) escaped = false;
                        else if (value == '\\') escaped = true;
                        else if (value == '"') inString = false;
                    }
                    else if (value == '"')
                    {
                        inString = true;
                    }
                    else if (value == '{')
                    {
                        ++depth;
                    }
                    else if (value == '}')
                    {
                        if (depth == 0) InvalidExport();
                        if (--depth == 0)
                        {
                            ThrowIfCancelled(stopToken);
                            std::optional<json::JsonObject> parsed;
                            try
                            {
                                parsed = json::JsonObject::Parse(winrt::to_hstring(objectText));
                            }
                            catch (...)
                            {
                                InvalidExport();
                            }
                            ThrowIfCancelled(stopToken);
                            callback(*parsed);
                            ThrowIfCancelled(stopToken);
                            objectText.clear();
                            collecting = false;
                            needsValue = false;
                            sawValue = true;
                        }
                    }
                    continue;
                }

                if (std::isspace(static_cast<unsigned char>(value))) continue;
                if (!opened)
                {
                    if (value != '[') InvalidExport();
                    opened = true;
                    continue;
                }
                if (closed) InvalidExport();
                if (value == '{')
                {
                    if (!needsValue) InvalidExport();
                    collecting = true;
                    inString = false;
                    escaped = false;
                    depth = 1;
                    objectText.assign(1, value);
                }
                else if (value == ',')
                {
                    if (needsValue || !sawValue) InvalidExport();
                    needsValue = true;
                }
                else if (value == ']')
                {
                    if (needsValue && sawValue) InvalidExport();
                    closed = true;
                }
                else
                {
                    InvalidExport();
                }
            }
            ThrowIfCancelled(stopToken);
            if (!stream.eof() || !opened || !closed || collecting) InvalidExport();
        }
    }

    ChatGPTExportImporter::ChatGPTExportImporter(Database& database) noexcept : m_database(database)
    {
    }

    ChatGPTImportResult ChatGPTExportImporter::Import(
        std::filesystem::path const& selectedFile,
        std::wstring_view accountId,
        std::stop_token stopToken)
    {
        ThrowIfCancelled(stopToken);
        if (!IsSupportedFileName(selectedFile))
        {
            throw std::invalid_argument("Select conversations.json or a numbered conversation JSON file");
        }
        auto const account = NormalizeAccount(accountId);
        std::error_code error;
        auto const path = std::filesystem::canonical(selectedFile, error);
        if (error) throw std::runtime_error("The selected ChatGPT export file is unavailable");

        auto const initialMetadata = ReadMetadata(path);
        auto const sourceHash = Sha256(path, stopToken);
        ThrowIfCancelled(stopToken);
        if (ReadMetadata(path) != initialMetadata)
        {
            throw std::runtime_error("The ChatGPT export changed while it was being read");
        }

        ChatGPTExportBatch batch;
        batch.sourcePath = path.filename().wstring();
        batch.sourceHash = sourceHash;
        batch.sourceModifiedAt = initialMetadata.modifiedAt;
        batch.sourceSize = initialMetadata.size;
        batch.accountId = account;
        ChatGPTImportResult result;
        result.sourceHash = sourceHash;
        if (m_database.IsChatGPTExportCurrent(batch))
        {
            result.unchanged = true;
            return result;
        }

        std::unordered_map<std::wstring, ParsedConversation> conversations;
        int64_t elements{};
        int64_t skipped{};
        ForEachObject(path, [&](json::JsonObject const& conversation)
        {
            ThrowIfCancelled(stopToken);
            ++elements;
            if (!LooksLikeConversation(conversation))
            {
                ++skipped;
                return;
            }
            auto parsed = ParseConversation(conversation, account, stopToken);
            if (!parsed)
            {
                ++skipped;
                return;
            }
            conversations.insert_or_assign(parsed->session.id, std::move(*parsed));
        }, stopToken);
        if (elements > 0 && conversations.empty()) InvalidExport();
        if (ReadMetadata(path) != initialMetadata)
        {
            throw std::runtime_error("The ChatGPT export changed while it was being read");
        }

        for (auto& [id, conversation] : conversations)
        {
            ThrowIfCancelled(stopToken);
            result.messages += conversation.session.messages;
            result.estimatedTokens += conversation.session.EstimatedTokens();
            result.prompts += conversation.session.prompts;
            batch.sessions.push_back(std::move(conversation.session));
            batch.prompts.insert(
                batch.prompts.end(),
                std::make_move_iterator(conversation.prompts.begin()),
                std::make_move_iterator(conversation.prompts.end()));
        }
        result.conversations = static_cast<int64_t>(batch.sessions.size());
        result.skippedConversations = skipped;
        ThrowIfCancelled(stopToken);
        m_database.ReplaceChatGPTExport(batch, stopToken);
        return result;
    }

    bool ChatGPTExportImporter::SelfTest()
    {
        auto const root = std::filesystem::temp_directory_path() /
            (L"TokenometerChatGPTImportTest-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code cleanupError;
        std::filesystem::remove_all(root, cleanupError);
        try
        {
            std::filesystem::create_directories(root);
            auto const fixture = root / L"conversations-001.json";
            auto writeFixture = [&](std::string_view answer)
            {
                std::string jsonText = R"json([
  {
    "id":"conversation-1",
    "create_time":1767225600.9,
    "update_time":1767225900.4,
    "default_model_slug":"fallback-model",
    "current_node":"assistant-2",
    "mapping":{
      "root":{"id":"root","parent":null,"message":null},
      "user-1":{"id":"user-node","parent":"root","message":{"id":"user-message","author":{"role":"user"},"create_time":1767225601.8,"content":{"parts":["hello"]},"metadata":{}}},
      "assistant-old":{"parent":"user-1","message":{"author":{"role":"assistant"},"create_time":1767225602,"content":{"parts":["THIS ABANDONED BRANCH MUST NEVER COUNT"]},"metadata":{"model_slug":"old-model"}}},
      "assistant-1":{"parent":"user-1","message":{"author":{"role":"assistant"},"create_time":1767225603,"content":{"parts":["ok"]},"metadata":{"model_slug":"gpt-test"}}},
      "hidden-user":{"parent":"assistant-1","message":{"author":{"role":"user"},"create_time":1767225604,"content":{"parts":["hidden"]},"metadata":{"is_visually_hidden_from_conversation":true}}},
      "user-2":{"parent":"hidden-user","message":{"author":{"role":"user"},"create_time":1767225605,"content":{"parts":[{"asset_pointer":"file-service://x"},"你好😀"]},"metadata":{}}},
      "assistant-2":{"parent":"user-2","message":{"author":{"role":"assistant"},"create_time":1767225606,"content":{"text":"__ANSWER__"},"metadata":{"model_slug":"gpt-test"}}}
    }
  },
  {"conversation_id":"broken","current_node":"missing","mapping":{}}
])json";
                auto const marker = jsonText.find("__ANSWER__");
                jsonText.replace(marker, std::string_view{ "__ANSWER__" }.size(), answer);
                std::ofstream stream(fixture, std::ios::binary | std::ios::trunc);
                stream.write(jsonText.data(), static_cast<std::streamsize>(jsonText.size()));
            };
            writeFixture("short answer");

            Database database(L":memory:");
            database.Initialize();
            UsageEvent exact{ L"exact.jsonl", 1, L"exact-session", L"codex", L"Codex", L"gpt-test",
                              L"", L"device", L"turn", 1, 1767225600, L"2026-01-01",
                              { 700, 0, 0, 77, 0, 777 } };
            if (!database.InsertUsageEvent(exact)) return false;

            ChatGPTExportImporter importer(database);
            auto const first = importer.Import(fixture, L" account-a ");
            auto const sessions = database.GetChatGPTEstimatedSessions(L"account-a");
            auto const prompts = database.GetChatGPTEstimatedPrompts(L"account-a", L"conversation-1");
            auto const daily = database.GetChatGPTEstimatedDailyUsage(0, L"account-a");
            auto const totals = database.GetTotals();
            auto const accountTotals = database.GetChatGPTEstimatedTotals(L"account-a");
            if (first.unchanged || first.conversations != 1 || first.prompts != 2 ||
                first.messages != 4 || first.skippedConversations != 1 || first.estimatedTokens <= 0 ||
                sessions.size() != 1 || sessions.front().sourceKind != L"chatgpt-export" ||
                prompts.size() != 2 || daily.empty() || daily.front().sourceKind != L"chatgpt-export" ||
                totals.counts.reportedTotal != 777 || totals.estimatedTokens != first.estimatedTokens ||
                totals.estimatedSessions != 1 || accountTotals.estimatedTokens != first.estimatedTokens ||
                accountTotals.estimatedSessions != 1)
            {
                return false;
            }

            auto const unchanged = importer.Import(fixture, L"account-a");
            if (!unchanged.unchanged || database.GetTotals().estimatedTokens != first.estimatedTokens)
            {
                return false;
            }

            writeFixture("this replacement answer is intentionally much longer than the first answer");
            auto const replaced = importer.Import(fixture, L"account-a");
            auto const replacedTotals = database.GetTotals();
            if (replaced.unchanged || replaced.estimatedTokens <= first.estimatedTokens ||
                replacedTotals.counts.reportedTotal != 777 ||
                replacedTotals.estimatedTokens != replaced.estimatedTokens ||
                replacedTotals.estimatedSessions != 1)
            {
                return false;
            }

            auto const unsupported = root / L"other.json";
            std::ofstream(unsupported, std::ios::binary) << "[]";
            bool rejected{};
            try
            {
                (void)importer.Import(unsupported, L"account-a");
            }
            catch (std::invalid_argument const&)
            {
                rejected = true;
            }
            std::stop_source stopped;
            stopped.request_stop();
            bool cancelled{};
            try
            {
                (void)importer.Import(fixture, L"account-cancelled", stopped.get_token());
            }
            catch (std::system_error const& error)
            {
                cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
            }
            if (!cancelled || database.GetChatGPTEstimatedTotals(L"account-cancelled").estimatedSessions != 0)
            {
                return false;
            }
            std::filesystem::remove_all(root, cleanupError);
            return rejected;
        }
        catch (...)
        {
            std::filesystem::remove_all(root, cleanupError);
            return false;
        }
    }
}
