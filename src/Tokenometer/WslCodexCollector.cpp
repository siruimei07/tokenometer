#include "WslCodexCollector.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <limits>
#include <iterator>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>

namespace tokenometer
{
    namespace
    {
        using namespace std::chrono_literals;
        using namespace std::string_view_literals;
        using Deadline = std::chrono::steady_clock::time_point;

        constexpr size_t maximumListBytes = 64 * 1024;
        constexpr size_t maximumRootBytes = 16 * 1024;
        constexpr size_t maximumMetadataBytes = 16 * 1024 * 1024;
        constexpr size_t metadataPageRecords = 3'000;
        constexpr size_t maximumOversizedScanStates = 128;
        constexpr size_t maximumTranscriptRecordBytes = 16 * 1024 * 1024;
        constexpr size_t transcriptChunkBytes = maximumTranscriptRecordBytes + 2;
        constexpr int64_t maximumDetailBytes = 1024 * 1024;
        constexpr auto distributionBudget = 60s;

        std::optional<std::chrono::milliseconds> OperationTimeout(
            Deadline deadline,
            std::chrono::milliseconds maximum)
        {
            if (deadline == Deadline::max()) return maximum;
            auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining <= 0ms) return std::nullopt;
            return std::min(maximum, remaining);
        }

        constexpr std::wstring_view rootScript =
            L"if [ -n \"${CODEX_HOME:-}\" ]; then root=$CODEX_HOME; "
            L"else root=$HOME/.codex; fi; "
            L"case \"$root\" in /*) ;; *) exit 126 ;; esac; "
            L"while [ \"$root\" != / ] && [ \"${root%/}\" != \"$root\" ]; do root=${root%/}; done; "
            L"parent=${root%/*}; [ -n \"$parent\" ] || parent=/; "
            L"[ -d \"$parent\" ] && [ -r \"$parent\" ] && [ -x \"$parent\" ] || exit 126; "
            L"[ -e \"$root\" ] || { printf '\\0'; exit 0; }; "
            L"root=$(readlink -f -- \"$root\") || exit 1; "
            L"[ -d \"$root\" ] && [ -r \"$root\" ] && [ -x \"$root\" ] || exit 126; "
            L"printf '%s\\0' \"$root\"";
        constexpr std::wstring_view enumerateScript =
            L"directory=$1; parent=${directory%/*}; "
            L"[ -d \"$parent\" ] && [ -r \"$parent\" ] && [ -x \"$parent\" ] || exit 126; "
            L"[ -e \"$directory\" ] || exit 0; "
            L"[ -d \"$directory\" ] && [ -r \"$directory\" ] && [ -x \"$directory\" ] || exit 126; "
            L"for tool in find sort sed dd stat readlink; do "
            L"\"$tool\" --version >/dev/null 2>&1 || exit 125; done; "
            L"set -o pipefail || exit 125; first=$2; last=$((first + $3 - 1)); "
            L"find -P \"$directory\" -type f -name 'rollout-*.jsonl' "
            L"-printf '%D\\t%i\\t%s\\t%T@\\t%p\\0' | "
            L"sort -z -t $'\\t' -k4,4nr -k5,5r | "
            L"sed -z -n \"${first},${last}p\"";
        constexpr std::wstring_view readScript =
            L"exec 3<\"$1\" || exit 1; target=$(readlink -f /proc/self/fd/3) || exit 1; "
            L"[ \"$target\" = \"$1\" ] || exit 1; case \"$target\" in "
            L"\"$2/sessions/\"*|\"$2/archived_sessions/\"*) ;; *) exit 1 ;; esac; "
            L"actual=$(stat -Lc '%d:%i' /proc/self/fd/3) || exit 1; "
            L"[ -z \"$5\" ] || [ \"$actual\" = \"$5:$6\" ] || exit 1; "
            L"exec dd if=/proc/self/fd/3 iflag=skip_bytes,count_bytes status=none "
            L"skip=\"$3\" count=\"$4\"";

        struct RemoteFile
        {
            std::wstring path;
            std::wstring device;
            std::wstring inode;
            int64_t size{};
            int64_t modifiedAt{};
            std::wstring sessionId;
        };

        struct WslLocator
        {
            std::wstring distribution;
            std::wstring path;
        };

        struct RemotePage
        {
            std::vector<RemoteFile> files;
            size_t records{};
        };

        size_t AdvancePageCursor(size_t cursor, size_t records, size_t restart)
        {
            if (records < metadataPageRecords) return restart;
            constexpr size_t maximumShellInteger =
                static_cast<size_t>(std::numeric_limits<int64_t>::max());
            return cursor > maximumShellInteger - records ? restart : cursor + records;
        }

        size_t CursorFromState(
            std::optional<std::wstring> const& value,
            size_t fallback)
        {
            if (!value || value->empty()) return fallback;
            constexpr size_t maximumShellInteger =
                static_cast<size_t>(std::numeric_limits<int64_t>::max());
            size_t parsed{};
            for (wchar_t const character : *value)
            {
                if (character < L'0' || character > L'9') return fallback;
                size_t const digit = static_cast<size_t>(character - L'0');
                if (parsed > (maximumShellInteger - digit) / 10) return fallback;
                parsed = parsed * 10 + digit;
            }
            return parsed == 0 ? fallback : parsed;
        }

        int64_t UnixNow()
        {
            return std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        WslProcessResult RunDefault(
            std::vector<std::wstring> const& arguments,
            WslProcessOptions const& options,
            std::stop_token stopToken)
        {
            return WslProcessRunner::Run(arguments, options, stopToken);
        }

        bool Succeeded(WslProcessResult const& result)
        {
            return result.started && !result.timedOut && !result.cancelled &&
                   !result.standardOutputTruncated && !result.remoteCleanupFailed &&
                   result.exitCode && *result.exitCode == 0;
        }

        std::optional<std::wstring> Utf8(std::span<uint8_t const> bytes)
        {
            if (bytes.empty()) return std::wstring{};
            if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return std::nullopt;
            int const length = MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                reinterpret_cast<char const*>(bytes.data()),
                static_cast<int>(bytes.size()),
                nullptr,
                0);
            if (length <= 0) return std::nullopt;
            std::wstring result(static_cast<size_t>(length), L'\0');
            if (MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    reinterpret_cast<char const*>(bytes.data()),
                    static_cast<int>(bytes.size()),
                    result.data(),
                    length) != length)
            {
                return std::nullopt;
            }
            return result;
        }

        std::optional<std::string> Utf8(std::wstring_view value)
        {
            if (value.empty()) return std::string{};
            if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return std::nullopt;
            int const length = WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                value.data(),
                static_cast<int>(value.size()),
                nullptr,
                0,
                nullptr,
                nullptr);
            if (length <= 0) return std::nullopt;
            std::string result(static_cast<size_t>(length), '\0');
            if (WideCharToMultiByte(
                    CP_UTF8,
                    WC_ERR_INVALID_CHARS,
                    value.data(),
                    static_cast<int>(value.size()),
                    result.data(),
                    length,
                    nullptr,
                    nullptr) != length)
            {
                return std::nullopt;
            }
            return result;
        }

        bool HasValidUtf16(std::wstring_view value)
        {
            for (size_t index = 0; index < value.size(); ++index)
            {
                auto const unit = static_cast<uint16_t>(value[index]);
                if (unit >= 0xD800 && unit <= 0xDBFF)
                {
                    if (++index >= value.size()) return false;
                    auto const low = static_cast<uint16_t>(value[index]);
                    if (low < 0xDC00 || low > 0xDFFF) return false;
                }
                else if (unit >= 0xDC00 && unit <= 0xDFFF)
                {
                    return false;
                }
            }
            return true;
        }

        std::optional<std::wstring> DecodeListText(std::vector<uint8_t> const& bytes)
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
            if (!utf16) return Utf8(bytes);
            if ((bytes.size() & 1u) != 0) return std::nullopt;

            size_t offset = bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE ? 2 : 0;
            std::wstring value;
            value.reserve((bytes.size() - offset) / 2);
            for (; offset < bytes.size(); offset += 2)
            {
                value.push_back(static_cast<wchar_t>(
                    static_cast<uint16_t>(bytes[offset]) |
                    (static_cast<uint16_t>(bytes[offset + 1]) << 8)));
            }
            return HasValidUtf16(value) ? std::optional{std::move(value)} : std::nullopt;
        }

        std::wstring Trim(std::wstring_view value)
        {
            size_t first{};
            while (first < value.size() && std::iswspace(value[first])) ++first;
            size_t last = value.size();
            while (last > first && std::iswspace(value[last - 1])) --last;
            return std::wstring{value.substr(first, last - first)};
        }

        bool ValidDistribution(std::wstring_view value)
        {
            if (value.empty() || value.size() > 128 || value == L"." || value == L"..") return false;
            return std::ranges::none_of(value, [](wchar_t character)
            {
                return character == L'/' || character == L'\\' || character == L'\0' ||
                       std::iswcntrl(character) != 0;
            });
        }

        std::optional<std::vector<std::wstring>> ParseDistributions(
            std::vector<uint8_t> const& bytes)
        {
            auto decoded = DecodeListText(bytes);
            if (!decoded) return std::nullopt;
            std::set<std::wstring> unique;
            size_t start{};
            for (size_t index = 0; index <= decoded->size(); ++index)
            {
                if (index != decoded->size() && (*decoded)[index] != L'\r' &&
                    (*decoded)[index] != L'\n' && (*decoded)[index] != L'\0')
                {
                    continue;
                }
                auto value = Trim(std::wstring_view{*decoded}.substr(start, index - start));
                if (!value.empty())
                {
                    if (!ValidDistribution(value)) return std::nullopt;
                    unique.insert(std::move(value));
                }
                start = index + 1;
            }
            return std::vector<std::wstring>{unique.begin(), unique.end()};
        }

        bool ValidAbsoluteLinuxPath(std::wstring_view value)
        {
            if (value.size() < 2 || value.size() > 4096 || value.front() != L'/' ||
                value.back() == L'/' || value.find(L'\\') != std::wstring_view::npos)
            {
                return false;
            }
            size_t start = 1;
            while (start <= value.size())
            {
                size_t const slash = value.find(L'/', start);
                size_t const end = slash == std::wstring_view::npos ? value.size() : slash;
                auto const part = value.substr(start, end - start);
                if (part.empty() || part == L"." || part == L"..") return false;
                if (std::ranges::any_of(part, [](wchar_t character)
                    {
                        return character == L'\0' || std::iswcntrl(character) != 0;
                    }))
                {
                    return false;
                }
                if (slash == std::wstring_view::npos) break;
                start = slash + 1;
            }
            return true;
        }

        bool IsRolloutPath(std::wstring_view path, std::wstring_view root)
        {
            if (!ValidAbsoluteLinuxPath(path) || !ValidAbsoluteLinuxPath(root)) return false;
            std::wstring const sessions = std::wstring{root} + L"/sessions/";
            std::wstring const archived = std::wstring{root} + L"/archived_sessions/";
            if (!path.starts_with(sessions) && !path.starts_with(archived)) return false;
            size_t const slash = path.find_last_of(L'/');
            auto const name = slash == std::wstring_view::npos ? path : path.substr(slash + 1);
            return name.size() > 15 && name.starts_with(L"rollout-") && name.ends_with(L".jsonl");
        }

        std::optional<int64_t> NonNegativeInteger(std::wstring_view value, bool allowFraction)
        {
            if (allowFraction)
            {
                size_t const dot = value.find(L'.');
                if (dot != std::wstring_view::npos) value = value.substr(0, dot);
            }
            if (value.empty()) return std::nullopt;
            int64_t result{};
            auto const utf8 = Utf8(value);
            if (!utf8) return std::nullopt;
            auto const parsed = std::from_chars(utf8->data(), utf8->data() + utf8->size(), result);
            if (parsed.ec != std::errc{} || parsed.ptr != utf8->data() + utf8->size() || result < 0)
            {
                return std::nullopt;
            }
            return result;
        }

        bool Digits(std::wstring_view value)
        {
            return !value.empty() && std::ranges::all_of(value, [](wchar_t character)
            {
                return character >= L'0' && character <= L'9';
            });
        }

        bool ValidSessionId(std::wstring_view value)
        {
            if (value.size() != 36) return false;
            for (size_t index = 0; index < value.size(); ++index)
            {
                if (index == 8 || index == 13 || index == 18 || index == 23)
                {
                    if (value[index] != L'-') return false;
                }
                else if (!std::iswxdigit(value[index]))
                {
                    return false;
                }
            }
            return true;
        }

        std::optional<std::wstring> SessionId(std::wstring_view path)
        {
            size_t const slash = path.find_last_of(L'/');
            auto const name = slash == std::wstring_view::npos ? path : path.substr(slash + 1);
            if (!name.starts_with(L"rollout-") || !name.ends_with(L".jsonl")) return std::nullopt;
            auto const stem = name.substr(0, name.size() - 6);
            if (stem.size() < 36) return std::nullopt;
            auto value = std::wstring{stem.substr(stem.size() - 36)};
            return ValidSessionId(value) ? std::optional{std::move(value)} : std::nullopt;
        }

        std::optional<std::vector<RemoteFile>> ParseFiles(
            std::vector<uint8_t> const& bytes,
            std::wstring_view root)
        {
            std::vector<RemoteFile> files;
            size_t start{};
            while (start < bytes.size())
            {
                auto const end = std::find(bytes.begin() + static_cast<ptrdiff_t>(start), bytes.end(), 0);
                if (end == bytes.end()) return std::nullopt;
                size_t const finish = static_cast<size_t>(end - bytes.begin());
                auto text = Utf8(std::span<uint8_t const>{bytes.data() + start, finish - start});
                start = finish + 1;
                if (!text) continue;

                std::array<size_t, 4> tabs{};
                size_t cursor{};
                bool validFields = true;
                for (auto& tab : tabs)
                {
                    tab = text->find(L'\t', cursor);
                    if (tab == std::wstring::npos)
                    {
                        validFields = false;
                        break;
                    }
                    cursor = tab + 1;
                }
                if (!validFields) continue;
                RemoteFile file;
                file.device = text->substr(0, tabs[0]);
                file.inode = text->substr(tabs[0] + 1, tabs[1] - tabs[0] - 1);
                auto const size = NonNegativeInteger(
                    std::wstring_view{*text}.substr(tabs[1] + 1, tabs[2] - tabs[1] - 1), false);
                auto const modified = NonNegativeInteger(
                    std::wstring_view{*text}.substr(tabs[2] + 1, tabs[3] - tabs[2] - 1), true);
                file.path = text->substr(tabs[3] + 1);
                auto const session = SessionId(file.path);
                if (!Digits(file.device) || !Digits(file.inode) || !size || !modified || !session ||
                    !IsRolloutPath(file.path, root))
                {
                    continue;
                }
                file.size = *size;
                file.modifiedAt = *modified;
                file.sessionId = *session;
                files.push_back(std::move(file));
            }
            std::ranges::sort(files, [](RemoteFile const& left, RemoteFile const& right)
            {
                if (left.modifiedAt != right.modifiedAt) return left.modifiedAt > right.modifiedAt;
                return left.path < right.path;
            });
            return files;
        }

        std::wstring PercentEncode(std::wstring_view value)
        {
            auto const bytes = Utf8(value);
            if (!bytes) return {};
            constexpr wchar_t hex[] = L"0123456789ABCDEF";
            std::wstring result;
            result.reserve(bytes->size() * 3);
            for (uint8_t const byte : *bytes)
            {
                bool const unreserved = (byte >= 'A' && byte <= 'Z') ||
                    (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
                    byte == '-' || byte == '.' || byte == '_' || byte == '~';
                if (unreserved)
                {
                    result.push_back(static_cast<wchar_t>(byte));
                }
                else
                {
                    result.push_back(L'%');
                    result.push_back(hex[byte >> 4]);
                    result.push_back(hex[byte & 0x0F]);
                }
            }
            return result;
        }

        int Hex(wchar_t value)
        {
            if (value >= L'0' && value <= L'9') return value - L'0';
            if (value >= L'a' && value <= L'f') return value - L'a' + 10;
            if (value >= L'A' && value <= L'F') return value - L'A' + 10;
            return -1;
        }

        std::optional<std::wstring> PercentDecode(std::wstring_view value)
        {
            std::vector<uint8_t> bytes;
            bytes.reserve(value.size());
            for (size_t index = 0; index < value.size(); ++index)
            {
                if (value[index] == L'%')
                {
                    if (index + 2 >= value.size()) return std::nullopt;
                    int const high = Hex(value[index + 1]);
                    int const low = Hex(value[index + 2]);
                    if (high < 0 || low < 0) return std::nullopt;
                    bytes.push_back(static_cast<uint8_t>((high << 4) | low));
                    index += 2;
                }
                else
                {
                    if (value[index] > 0x7F) return std::nullopt;
                    bytes.push_back(static_cast<uint8_t>(value[index]));
                }
            }
            return Utf8(bytes);
        }

        std::wstring MakeLocator(std::wstring_view distribution, std::wstring_view path)
        {
            return L"wsl://" + PercentEncode(distribution) + L"/" + PercentEncode(path);
        }

        std::optional<WslLocator> ParseLocator(std::wstring_view locator)
        {
            constexpr std::wstring_view prefix = L"wsl://";
            if (!locator.starts_with(prefix)) return std::nullopt;
            locator.remove_prefix(prefix.size());
            size_t const separator = locator.find(L'/');
            if (separator == std::wstring_view::npos) return std::nullopt;
            auto distribution = PercentDecode(locator.substr(0, separator));
            auto path = PercentDecode(locator.substr(separator + 1));
            if (!distribution || !path || !ValidDistribution(*distribution) ||
                !ValidAbsoluteLinuxPath(*path))
            {
                return std::nullopt;
            }
            return WslLocator{std::move(*distribution), std::move(*path)};
        }

        std::vector<std::wstring> ShellCommand(
            std::wstring_view distribution,
            std::wstring_view script,
            std::initializer_list<std::wstring> arguments = {})
        {
            std::vector<std::wstring> result{
                L"--distribution", std::wstring{distribution}, L"--exec", L"/bin/sh",
                L"-c", std::wstring{script}, L"tokenometer"
            };
            result.insert(result.end(), arguments.begin(), arguments.end());
            return result;
        }

        std::vector<std::wstring> EnumerateCommand(
            std::wstring_view distribution,
            std::wstring_view directory,
            size_t firstRecord)
        {
            return {
                L"--distribution", std::wstring{distribution}, L"--exec", L"/bin/bash",
                L"-c", std::wstring{enumerateScript}, L"tokenometer", std::wstring{directory},
                std::to_wstring(firstRecord), std::to_wstring(metadataPageRecords)
            };
        }

        std::optional<std::vector<std::wstring>> RunningDistributions(
            WslCodexCollector::Runner const& runner,
            std::stop_token stopToken,
            std::chrono::milliseconds timeout = 5s)
        {
            if (timeout <= 0ms) return std::nullopt;
            WslProcessOptions options;
            options.timeout = timeout;
            options.maximumStandardOutputBytes = maximumListBytes;
            options.maximumStandardErrorBytes = maximumListBytes;
            auto result = runner(
                {L"--list", L"--running", L"--quiet"},
                options,
                stopToken);
            if (!Succeeded(result)) return std::nullopt;
            return ParseDistributions(result.standardOutput);
        }

        bool IsRunning(
            WslCodexCollector::Runner const& runner,
            std::wstring_view distribution,
            std::stop_token stopToken,
            std::chrono::milliseconds timeout = 5s)
        {
            auto running = RunningDistributions(runner, stopToken, timeout);
            return running && std::ranges::binary_search(*running, distribution);
        }

        std::optional<std::wstring> ResolveRoot(
            WslCodexCollector::Runner const& runner,
            std::wstring_view distribution,
            std::stop_token stopToken,
            Deadline deadline = Deadline::max())
        {
            auto runningTimeout = OperationTimeout(deadline, 5s);
            if (!runningTimeout || !IsRunning(runner, distribution, stopToken, *runningTimeout))
            {
                return std::nullopt;
            }
            auto rootTimeout = OperationTimeout(deadline, 5s);
            if (!rootTimeout) return std::nullopt;
            WslProcessOptions options;
            options.timeout = *rootTimeout;
            options.maximumStandardOutputBytes = maximumRootBytes;
            options.maximumStandardErrorBytes = maximumListBytes;
            auto result = runner(ShellCommand(distribution, rootScript), options, stopToken);
            if (!Succeeded(result)) return std::nullopt;
            auto const terminator = std::find(
                result.standardOutput.begin(), result.standardOutput.end(), 0);
            if (terminator == result.standardOutput.end() || terminator + 1 != result.standardOutput.end())
            {
                return std::nullopt;
            }
            auto root = Utf8(std::span<uint8_t const>{
                result.standardOutput.data(),
                static_cast<size_t>(terminator - result.standardOutput.begin())});
            if (!root) return std::nullopt;
            if (root->empty()) return std::optional{std::wstring{}};
            return ValidAbsoluteLinuxPath(*root) ? std::optional{std::move(*root)} : std::nullopt;
        }

        std::optional<RemotePage> EnumeratePage(
            WslCodexCollector::Runner const& runner,
            std::wstring_view distribution,
            std::wstring_view root,
            std::wstring_view directory,
            size_t firstRecord,
            std::stop_token stopToken,
            Deadline deadline)
        {
            auto runningTimeout = OperationTimeout(deadline, 5s);
            if (!runningTimeout || !IsRunning(runner, distribution, stopToken, *runningTimeout))
            {
                return std::nullopt;
            }
            auto enumerationTimeout = OperationTimeout(deadline, 10s);
            if (!enumerationTimeout) return std::nullopt;
            WslProcessOptions options;
            options.timeout = *enumerationTimeout;
            options.maximumStandardOutputBytes = maximumMetadataBytes;
            options.maximumStandardErrorBytes = maximumListBytes;
            auto page = runner(
                EnumerateCommand(distribution, directory, firstRecord),
                options,
                stopToken);
            if (!Succeeded(page)) return std::nullopt;
            size_t const records = static_cast<size_t>(std::ranges::count(
                page.standardOutput,
                static_cast<uint8_t>(0)));
            if (records > metadataPageRecords) return std::nullopt;
            auto parsed = ParseFiles(page.standardOutput, root);
            if (!parsed) return std::nullopt;
            return RemotePage{std::move(*parsed), records};
        }

        std::optional<std::string> ReadBytes(
            WslCodexCollector::Runner const& runner,
            std::wstring_view distribution,
            std::wstring_view root,
            std::wstring_view path,
            int64_t offset,
            int64_t length,
            std::chrono::milliseconds timeout,
            std::stop_token stopToken,
            std::wstring_view expectedDevice = {},
            std::wstring_view expectedInode = {},
            Deadline deadline = Deadline::max())
        {
            if (offset < 0 || length < 0 ||
                static_cast<uint64_t>(length) > WslProcessRunner::MaximumPipeBytes)
            {
                return std::nullopt;
            }
            auto runningTimeout = OperationTimeout(deadline, 5s);
            if (!runningTimeout || !IsRunning(runner, distribution, stopToken, *runningTimeout))
            {
                return std::nullopt;
            }
            auto readTimeout = OperationTimeout(deadline, timeout);
            if (!readTimeout) return std::nullopt;
            WslProcessOptions options;
            options.timeout = *readTimeout;
            options.maximumStandardOutputBytes = static_cast<size_t>(length);
            options.maximumStandardErrorBytes = maximumListBytes;
            auto result = runner(
                ShellCommand(distribution, readScript, {
                    std::wstring{path}, std::wstring{root}, std::to_wstring(offset),
                    std::to_wstring(length), std::wstring{expectedDevice},
                    std::wstring{expectedInode}}),
                options,
                stopToken);
            if (!Succeeded(result) || result.standardOutput.size() != static_cast<size_t>(length))
            {
                return std::nullopt;
            }
            return std::string{
                reinterpret_cast<char const*>(result.standardOutput.data()),
                result.standardOutput.size()};
        }

        std::optional<std::string> ReadRange(
            WslCodexCollector::Runner const& runner,
            std::wstring_view locator,
            int64_t offset,
            int64_t length,
            std::stop_token stopToken)
        {
            if (offset < 0 || length < 0 || length > maximumDetailBytes) return std::nullopt;
            auto const parsed = ParseLocator(locator);
            if (!parsed) return std::nullopt;
            auto distributions = RunningDistributions(runner, stopToken);
            if (!distributions || !std::ranges::binary_search(*distributions, parsed->distribution))
            {
                return std::nullopt;
            }
            auto root = ResolveRoot(runner, parsed->distribution, stopToken);
            if (!root || root->empty() || !IsRolloutPath(parsed->path, *root)) return std::nullopt;
            return ReadBytes(
                runner,
                parsed->distribution,
                *root,
                parsed->path,
                offset,
                length,
                10s,
                stopToken);
        }

        std::wstring HostName()
        {
            std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1> value{};
            DWORD size = static_cast<DWORD>(value.size());
            if (!GetComputerNameW(value.data(), &size) || size == 0) return L"Windows";
            return std::wstring{value.data(), size};
        }

        void Add(CollectionResult& destination, CollectionResult const& source)
        {
            destination.filesVisited += source.filesVisited;
            destination.filesChanged += source.filesChanged;
            destination.usageEvents += source.usageEvents;
            destination.promptEvents += source.promptEvents;
            destination.toolEvents += source.toolEvents;
            destination.malformedRecords += source.malformedRecords;
            destination.oversizedRecords += source.oversizedRecords;
            destination.ioErrors += source.ioErrors;
            destination.bytesRead += source.bytesRead;
            destination.completedAt = std::max(destination.completedAt, source.completedAt);
        }

        std::vector<uint8_t> Bytes(std::string_view value)
        {
            return {value.begin(), value.end()};
        }

        WslProcessResult Success(std::vector<uint8_t> output = {})
        {
            WslProcessResult result;
            result.started = true;
            result.exitCode = 0;
            result.standardOutput = std::move(output);
            return result;
        }
    }

    WslCodexCollector::WslCodexCollector(Database& database, Runner runner) :
        m_database(database),
        m_parser(database, {}),
        m_runner(runner ? std::move(runner) : Runner{RunDefault}),
        m_hostName(HostName())
    {
        m_localDeviceId = database.GetOrCreateDeviceId(m_hostName);
    }

    WslCollectionResult WslCodexCollector::CollectOnce(std::stop_token stopToken)
    {
        WslCollectionResult result;
        if (stopToken.stop_requested())
        {
            result.completedAt = UnixNow();
            result.usage.completedAt = result.completedAt;
            return result;
        }
        auto distributions = RunningDistributions(m_runner, stopToken);
        if (!distributions)
        {
            if (!stopToken.stop_requested()) ++result.errors;
            result.completedAt = UnixNow();
            return result;
        }
        result.discoverySucceeded = true;
        result.distributionsFound = static_cast<int>(distributions->size());

        for (auto const& distribution : *distributions)
        {
            if (stopToken.stop_requested()) break;
            auto const startedAt = std::chrono::steady_clock::now();
            auto const deadline = startedAt + distributionBudget;
            std::wstring deviceId;
            try
            {
                deviceId = m_database.GetOrCreateDeviceId(
                    m_hostName + L" \u00b7 WSL \u00b7 " + distribution,
                    L"wsl_device_id:" + m_localDeviceId + L":" + distribution);
            }
            catch (...)
            {
                ++result.errors;
                continue;
            }
            bool distributionHadError{};
            bool distributionWasPartial{};
            int successfulPages{};
            auto recordStatus = [&](DeviceSyncStatus status, std::wstring_view error = {})
            {
                if (!stopToken.stop_requested())
                {
                    m_database.RecordDeviceSync(deviceId, status, error);
                }
            };

            auto root = ResolveRoot(m_runner, distribution, stopToken, deadline);
            if (!root)
            {
                if (!stopToken.stop_requested())
                {
                    ++result.errors;
                    recordStatus(DeviceSyncStatus::Failed, L"WSL Codex root could not be resolved");
                }
                continue;
            }
            ++result.distributionsScanned;
            if (root->empty())
            {
                recordStatus(DeviceSyncStatus::Synced);
                continue;
            }
            std::wstring const accountId = L"wsl-unknown:" + deviceId;
            std::wstring const activeCursorKey = L"wsl_active_cursor:" + deviceId;
            std::wstring const archiveCursorKey = L"wsl_archive_cursor:" + deviceId;

            auto loadCursor = [&](auto& cursors, std::wstring_view stateKey, size_t fallback)
                -> size_t&
            {
                auto [iterator, inserted] = cursors.try_emplace(distribution, fallback);
                if (inserted)
                {
                    iterator->second = CursorFromState(
                        m_database.GetAppState(stateKey), fallback);
                }
                return iterator->second;
            };

            auto processFiles = [&](std::vector<RemoteFile> const& files) -> bool
            {
                bool completedAll = true;
                result.filesFound += static_cast<int>(files.size());
                for (auto const& file : files)
                {
                    if (stopToken.stop_requested())
                    {
                        return false;
                    }
                    if (std::chrono::steady_clock::now() - startedAt >= distributionBudget)
                    {
                        distributionWasPartial = true;
                        return false;
                    }
                    if (m_database.HasSessionSource(file.sessionId, L"codex"))
                    {
                        continue;
                    }
                    std::wstring const locator = MakeLocator(distribution, file.path);
                    std::wstring const identity = L"wsl:" + deviceId + L":" + file.device +
                        L":" + file.inode + L":" + file.sessionId;
                    auto progress = m_database.GetSourceProgress(locator, identity);
                    int64_t offset = progress && progress->fileIdentity == identity &&
                        file.size >= progress->offset ? progress->offset : 0;

                    if (offset == file.size)
                    {
                        m_oversizedScanCursors.erase(identity);
                        ExternalCodexTranscript transcript;
                        transcript.sourcePath = locator;
                        transcript.fileIdentity = identity;
                        transcript.sessionId = file.sessionId;
                        transcript.deviceId = deviceId;
                        transcript.sourceKind = L"codex-wsl";
                        transcript.accountId = accountId;
                        transcript.size = file.size;
                        transcript.modifiedAt = file.modifiedAt;
                        transcript.contentOffset = offset;
                        transcript.collectRateLimits = false;
                        auto const collected = m_parser.CollectExternal(transcript, stopToken);
                        distributionWasPartial = distributionWasPartial || collected.HasPartialErrors();
                        Add(result.usage, collected);
                        continue;
                    }

                    while (offset < file.size && !stopToken.stop_requested() &&
                           std::chrono::steady_clock::now() - startedAt < distributionBudget)
                    {
                        int64_t const length = std::min<int64_t>(
                            file.size - offset,
                            static_cast<int64_t>(transcriptChunkBytes));
                        auto content = ReadBytes(
                            m_runner,
                            distribution,
                            *root,
                            file.path,
                            offset,
                            length,
                            30s,
                            stopToken,
                            file.device,
                            file.inode,
                            deadline);
                        if (!content)
                        {
                            if (!stopToken.stop_requested())
                            {
                                ++result.errors;
                                distributionHadError = true;
                            }
                            completedAll = false;
                            break;
                        }
                        bool const oversizedPrefix =
                            content->size() > maximumTranscriptRecordBytes &&
                            content->find('\n') == std::string::npos;

                        ExternalCodexTranscript transcript;
                        transcript.sourcePath = locator;
                        transcript.fileIdentity = identity;
                        transcript.sessionId = file.sessionId;
                        transcript.deviceId = deviceId;
                        transcript.sourceKind = L"codex-wsl";
                        transcript.accountId = accountId;
                        transcript.size = file.size;
                        transcript.modifiedAt = file.modifiedAt;
                        transcript.contentOffset = offset;
                        transcript.content = std::move(*content);
                        transcript.collectRateLimits = false;
                        auto const collected = m_parser.CollectExternal(transcript, stopToken);
                        distributionWasPartial = distributionWasPartial || collected.HasPartialErrors();
                        Add(result.usage, collected);

                        auto next = m_database.GetSourceProgress(locator, identity);
                        if (!next || next->offset <= offset)
                        {
                            if (oversizedPrefix && next && next->offset == offset)
                            {
                                int64_t scanOffset = offset + length;
                                if (auto const saved = m_oversizedScanCursors.find(identity);
                                    saved != m_oversizedScanCursors.end())
                                {
                                    if (saved->second.recordOffset == offset &&
                                        saved->second.scanOffset > scanOffset &&
                                        saved->second.scanOffset <= file.size)
                                    {
                                        scanOffset = saved->second.scanOffset;
                                    }
                                    else
                                    {
                                        m_oversizedScanCursors.erase(saved);
                                    }
                                }
                                int64_t skipTo{};
                                bool scanFailed{};
                                while (scanOffset < file.size && !stopToken.stop_requested() &&
                                       std::chrono::steady_clock::now() - startedAt < distributionBudget)
                                {
                                    int64_t const scanLength = std::min<int64_t>(
                                        file.size - scanOffset,
                                        static_cast<int64_t>(WslProcessRunner::MaximumPipeBytes));
                                    auto scan = ReadBytes(
                                        m_runner,
                                        distribution,
                                        *root,
                                        file.path,
                                        scanOffset,
                                        scanLength,
                                        30s,
                                        stopToken,
                                        file.device,
                                        file.inode,
                                        deadline);
                                    if (!scan)
                                    {
                                        scanFailed = true;
                                        break;
                                    }
                                    size_t const newline = scan->find('\n');
                                    if (newline != std::string::npos)
                                    {
                                        skipTo = scanOffset + static_cast<int64_t>(newline) + 1;
                                        break;
                                    }
                                    scanOffset += scanLength;
                                    if (!m_oversizedScanCursors.contains(identity) &&
                                        m_oversizedScanCursors.size() >= maximumOversizedScanStates)
                                    {
                                        m_oversizedScanCursors.erase(m_oversizedScanCursors.begin());
                                    }
                                    m_oversizedScanCursors[identity] = {offset, scanOffset};
                                }
                                if (!scanFailed && !stopToken.stop_requested() &&
                                    (skipTo > 0 || scanOffset >= file.size))
                                {
                                    if (skipTo == 0) skipTo = file.size;
                                    m_oversizedScanCursors.erase(identity);
                                    next->offset = skipTo;
                                    next->size = file.size;
                                    next->modifiedAt = file.modifiedAt;
                                    m_database.Transaction([&]
                                    {
                                        m_database.SaveSourceProgress(*next);
                                    });
                                    ++result.usage.oversizedRecords;
                                    distributionWasPartial = true;
                                    result.usage.bytesRead += skipTo - offset;
                                    offset = skipTo;
                                    continue;
                                }
                                if (scanFailed)
                                {
                                    if (!stopToken.stop_requested())
                                    {
                                        ++result.errors;
                                        distributionHadError = true;
                                    }
                                    completedAll = false;
                                }
                            }
                            break;
                        }
                        m_oversizedScanCursors.erase(identity);
                        offset = next->offset;
                    }
                    if (stopToken.stop_requested())
                    {
                        return false;
                    }
                    if (std::chrono::steady_clock::now() - startedAt >= distributionBudget)
                    {
                        distributionWasPartial = true;
                        return false;
                    }
                }
                return completedAll;
            };

            auto collectPage = [&](std::wstring const& directory, size_t firstRecord)
                -> std::optional<std::pair<size_t, bool>>
            {
                if (stopToken.stop_requested() ||
                    std::chrono::steady_clock::now() - startedAt >= distributionBudget)
                {
                    if (!stopToken.stop_requested()) distributionWasPartial = true;
                    return std::nullopt;
                }
                auto page = EnumeratePage(
                    m_runner, distribution, *root, directory, firstRecord, stopToken, deadline);
                if (!page)
                {
                    if (!stopToken.stop_requested())
                    {
                        ++result.errors;
                        distributionHadError = true;
                    }
                    return std::nullopt;
                }
                ++successfulPages;
                bool const completed = processFiles(page->files);
                return std::pair{page->records, completed};
            };

            std::wstring const sessions = *root + L"/sessions";
            auto const newestActive = collectPage(sessions, 1);
            if (newestActive && newestActive->second &&
                newestActive->first == metadataPageRecords)
            {
                auto& cursor = loadCursor(
                    m_activeBacklogCursors,
                    activeCursorKey,
                    metadataPageRecords + 1);
                if (cursor <= metadataPageRecords) cursor = metadataPageRecords + 1;
                if (auto const backlog = collectPage(sessions, cursor))
                {
                    if (backlog->second)
                    {
                        cursor = AdvancePageCursor(
                            cursor, backlog->first, metadataPageRecords + 1);
                        m_database.SetAppState(activeCursorKey, std::to_wstring(cursor));
                    }
                }
            }

            std::wstring const archived = *root + L"/archived_sessions";
            auto& archiveCursor = loadCursor(m_archiveCursors, archiveCursorKey, 1);
            if (auto const page = collectPage(archived, archiveCursor))
            {
                if (page->second)
                {
                    archiveCursor = AdvancePageCursor(archiveCursor, page->first, 1);
                    m_database.SetAppState(
                        archiveCursorKey,
                        std::to_wstring(archiveCursor));
                }
            }

            if (!stopToken.stop_requested())
            {
                if (distributionHadError)
                {
                    recordStatus(
                        successfulPages == 0
                            ? DeviceSyncStatus::Failed
                            : DeviceSyncStatus::PartialError,
                        L"One or more WSL Codex files could not be collected");
                }
                else if (distributionWasPartial)
                {
                    recordStatus(
                        DeviceSyncStatus::PartialError,
                        L"The WSL Codex scan was incomplete or skipped invalid records");
                }
                else
                {
                    recordStatus(DeviceSyncStatus::Synced);
                }
            }
        }
        result.completedAt = UnixNow();
        result.usage.completedAt = result.completedAt;
        return result;
    }

    std::optional<std::string> WslCodexCollector::ReadSourceRange(
        std::wstring_view locator,
        int64_t offset,
        int64_t length,
        std::stop_token stopToken)
    {
        return ReadRange(Runner{RunDefault}, locator, offset, length, stopToken);
    }

    bool WslCodexCollector::SelfTest()
    {
        try
        {
            std::wstring const distribution = L"\u5f00\u53d1 \u73af\u5883 \"$(noop)\"";
            std::wstring remotePath =
                L"/home/test/.codex/sessions/2026/08/08/"
                L"rollout-2026-08-08-33333333-3333-3333-3333-333333333333.jsonl";
            std::string const head =
                R"({"timestamp":"2026-08-08T00:00:00.000Z","type":"session_meta","payload":{"id":"33333333-3333-3333-3333-333333333333","cwd":"/home/test/project"}})" "\n"
                R"({"timestamp":"2026-08-08T00:00:01.000Z","type":"turn_context","payload":{"turn_id":"turn-1","model":"gpt-wsl","cwd":"/home/test/project"}})" "\n"
                R"({"timestamp":"2026-08-08T00:00:02.000Z","type":"event_msg","payload":{"type":"user_message","message":"private"}})" "\n";
            std::string const tool =
                R"({"timestamp":"2026-08-08T00:00:03.000Z","type":"response_item","payload":{"type":"function_call","name":"shell_command","call_id":"call-wsl","arguments":"{\"command\":\"echo safe\"}"}})" "\n";
            std::string const output =
                R"({"timestamp":"2026-08-08T00:00:04.000Z","type":"response_item","payload":{"type":"function_call_output","call_id":"call-wsl","output":"ok"}})" "\n";
            std::string const firstUsage =
                R"({"timestamp":"2026-08-08T00:00:05.000Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":40,"cached_input_tokens":10,"cache_write_input_tokens":0,"output_tokens":2,"reasoning_output_tokens":1,"total_tokens":42},"last_token_usage":{"input_tokens":40,"cached_input_tokens":10,"cache_write_input_tokens":0,"output_tokens":2,"reasoning_output_tokens":1,"total_tokens":42}}}})" "\n";
            std::string remote = head + tool + output + firstUsage;
            int64_t modified = 1'786'150'805;
            std::vector<std::vector<std::wstring>> calls;
            int64_t forcedReadFailureOffset{-1};
            bool forcedReadFailurePending{};
            std::wstring forcedRootPermissionDistribution;
            std::wstring missingRootDistribution;
            std::wstring forcedEnumerationFailureDistribution;

            Runner fake = [&](std::vector<std::wstring> const& arguments,
                              WslProcessOptions const&,
                              std::stop_token)
            {
                calls.push_back(arguments);
                if (arguments == std::vector<std::wstring>{L"--list", L"--running", L"--quiet"})
                {
                    std::wstring text = distribution + L"\r\nUbuntu\r\n" + distribution + L"\r\n";
                    std::vector<uint8_t> bytes;
                    for (wchar_t const value : text)
                    {
                        bytes.push_back(static_cast<uint8_t>(value & 0xFF));
                        bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
                    }
                    return Success(std::move(bytes));
                }
                if (arguments.size() >= 6 && arguments[5] == rootScript)
                {
                    if (arguments[1] == forcedRootPermissionDistribution)
                    {
                        WslProcessResult denied;
                        denied.started = true;
                        denied.exitCode = 126;
                        return denied;
                    }
                    if (arguments[1] == missingRootDistribution)
                    {
                        return Success(Bytes("\0"sv));
                    }
                    return Success(Bytes("/home/test/.codex\0"sv));
                }
                if (arguments.size() >= 6 && arguments[5] == enumerateScript)
                {
                    if (arguments[1] == forcedEnumerationFailureDistribution &&
                        arguments.size() >= 8 && arguments[7].ends_with(L"/sessions"))
                    {
                        WslProcessResult denied;
                        denied.started = true;
                        denied.exitCode = 126;
                        return denied;
                    }
                    if (arguments[1] == L"Ubuntu")
                    {
                        if (arguments.size() >= 10 && arguments[8] == L"1")
                        {
                            return Success(std::vector<uint8_t>(metadataPageRecords, 0));
                        }
                        return Success();
                    }
                    if (arguments.size() < 8 ||
                        !remotePath.starts_with(arguments[7] + L"/"))
                    {
                        return Success();
                    }
                    auto path = Utf8(remotePath);
                    if (!path) return WslProcessResult{};
                    std::string metadata =
                        "42\t98\t1\t1.000000000\t/home/test/.codex/sessions/rollout-backup.jsonl";
                    metadata.push_back('\0');
                    metadata += "42\t99\t" + std::to_string(remote.size()) + "\t" +
                        std::to_string(modified) + ".000000000\t" + *path + '\0';
                    return Success(Bytes(metadata));
                }
                if (arguments.size() >= 11 && arguments[5] == readScript)
                {
                    int64_t const offset = std::stoll(arguments[9]);
                    int64_t const length = std::stoll(arguments[10]);
                    if (arguments[7] != remotePath || offset < 0 || length < 0 ||
                        offset + length > static_cast<int64_t>(remote.size()))
                    {
                        return WslProcessResult{};
                    }
                    if (forcedReadFailurePending && offset == forcedReadFailureOffset)
                    {
                        forcedReadFailurePending = false;
                        return WslProcessResult{};
                    }
                    return Success(Bytes(std::string_view{remote}.substr(
                        static_cast<size_t>(offset), static_cast<size_t>(length))));
                }
                return WslProcessResult{};
            };

            Database database(L":memory:");
            database.Initialize();
            WslCodexCollector collector(database, fake);
            auto const first = collector.CollectOnce();
            auto const second = collector.CollectOnce();
            auto const accounts = database.GetBreakdown(L"account");
            auto const firstDevices = database.GetDeviceSummaries(10);
            auto const findDeviceByName = [&](std::wstring_view suffix)
                -> DeviceSummary const*
            {
                auto const found = std::find_if(
                    firstDevices.begin(), firstDevices.end(), [&](auto const& device)
                    {
                        return device.kind == DeviceKind::Wsl &&
                            device.displayName.ends_with(suffix);
                    });
                return found == firstDevices.end() ? nullptr : &*found;
            };
            auto const fixtureDistributionDevice = findDeviceByName(distribution);
            auto const ubuntuDevice = findDeviceByName(L"Ubuntu");
            if (!first.discoverySucceeded || first.distributionsFound != 2 ||
                first.distributionsScanned != 2 ||
                first.filesFound != 1 || first.errors != 0 || first.usage.usageEvents != 1 ||
                first.usage.promptEvents != 1 || first.usage.toolEvents != 1 ||
                second.usage.usageEvents != 0 || database.GetTotals().counts.reportedTotal != 42 ||
                !fixtureDistributionDevice || !ubuntuDevice ||
                fixtureDistributionDevice->syncStatus != DeviceSyncStatus::Synced ||
                ubuntuDevice->syncStatus != DeviceSyncStatus::Synced ||
                fixtureDistributionDevice->lastSuccess <= 0 ||
                ubuntuDevice->lastSuccess <= 0 ||
                std::ranges::none_of(firstDevices, [](auto const& device)
                {
                    return device.kind == DeviceKind::Windows &&
                        device.syncStatus == DeviceSyncStatus::Never &&
                        device.lastAttempt == 0 && device.lastSuccess == 0;
                }) ||
                std::ranges::none_of(accounts, [](auto const& row)
                {
                    return row.key.starts_with(L"wsl-unknown:") &&
                        row.counts.reportedTotal == 42;
                }))
            {
                return false;
            }

            std::string const partial =
                R"({"timestamp":"2026-08-08T00:00:06.000Z","type":"event_msg","payload":{"type":"token_count")";
            remote += partial;
            ++modified;
            auto const incomplete = collector.CollectOnce();
            remote +=
                R"(,"info":{"total_token_usage":{"input_tokens":50,"cached_input_tokens":12,"cache_write_input_tokens":0,"output_tokens":2,"reasoning_output_tokens":1,"total_tokens":52},"last_token_usage":{"input_tokens":10,"cached_input_tokens":2,"cache_write_input_tokens":0,"output_tokens":0,"reasoning_output_tokens":0,"total_tokens":10}}}})" "\n";
            ++modified;
            auto const completed = collector.CollectOnce();
            if (incomplete.usage.usageEvents != 0 || completed.usage.usageEvents != 1 ||
                database.GetTotals().counts.reportedTotal != 52)
            {
                return false;
            }

            int64_t const oversizedRecordOffset = static_cast<int64_t>(remote.size());
            remote.append(40 * 1024 * 1024, 'x');
            remote.push_back('\n');
            remote +=
                R"({"timestamp":"2026-08-08T00:00:07.000Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":58,"cached_input_tokens":14,"cache_write_input_tokens":0,"output_tokens":2,"reasoning_output_tokens":1,"total_tokens":60},"last_token_usage":{"input_tokens":8,"cached_input_tokens":2,"cache_write_input_tokens":0,"output_tokens":0,"reasoning_output_tokens":0,"total_tokens":8}}}})" "\n";
            int64_t const firstScanOffset = oversizedRecordOffset +
                static_cast<int64_t>(transcriptChunkBytes);
            forcedReadFailureOffset = firstScanOffset +
                static_cast<int64_t>(WslProcessRunner::MaximumPipeBytes);
            forcedReadFailurePending = true;
            ++modified;
            auto const interruptedOversized = collector.CollectOnce();
            size_t const resumeCallStart = calls.size();
            auto const afterOversized = collector.CollectOnce();
            bool resumedAtSavedOffset{};
            bool repeatedCompletedScanChunk{};
            for (size_t index = resumeCallStart; index < calls.size(); ++index)
            {
                auto const& call = calls[index];
                if (call.size() < 11 || call[5] != readScript) continue;
                int64_t const offset = std::stoll(call[9]);
                resumedAtSavedOffset = resumedAtSavedOffset || offset == forcedReadFailureOffset;
                repeatedCompletedScanChunk = repeatedCompletedScanChunk || offset == firstScanOffset;
            }
            if (interruptedOversized.usage.oversizedRecords != 0 ||
                interruptedOversized.usage.usageEvents != 0 || forcedReadFailurePending ||
                !resumedAtSavedOffset || repeatedCompletedScanChunk ||
                afterOversized.usage.oversizedRecords != 1 ||
                afterOversized.usage.usageEvents != 1 ||
                database.GetTotals().counts.reportedTotal != 60)
            {
                return false;
            }
            auto const afterOversizedDevices = database.GetDeviceSummaries(10);
            auto currentStatus = [&](std::wstring_view id)
            {
                auto const found = std::find_if(
                    afterOversizedDevices.begin(), afterOversizedDevices.end(), [&](auto const& device)
                    {
                        return device.id == id;
                    });
                return found == afterOversizedDevices.end()
                    ? DeviceSyncStatus::Never
                    : found->syncStatus;
            };
            if (currentStatus(fixtureDistributionDevice->id) != DeviceSyncStatus::PartialError ||
                currentStatus(ubuntuDevice->id) != DeviceSyncStatus::Synced)
            {
                return false;
            }

            remotePath =
                L"/home/test/.codex/archived_sessions/"
                L"rollout-2026-08-08-33333333-3333-3333-3333-333333333333.jsonl";
            ++modified;
            auto const archived = collector.CollectOnce();
            auto const tools = database.GetToolCalls(
                L"33333333-3333-3333-3333-333333333333", 1);
            if (archived.usage.usageEvents != 0 || tools.size() != 1 ||
                tools.front().sourcePath != MakeLocator(distribution, remotePath))
            {
                return false;
            }

            auto const range = ReadRange(
                fake,
                tools.front().sourcePath,
                tools.front().inputOffset,
                tools.front().inputLength,
                {});
            if (!range || range->find("shell_command") == std::string::npos ||
                ReadRange(fake, L"wsl://Ubuntu/%2Fhome%2F..%2Fauth.json", 0, 1, {}))
            {
                return false;
            }
            for (auto const& call : calls)
            {
                if (call.size() >= 6 &&
                    (call[5].find(distribution) != std::wstring::npos ||
                     call[5].find(remotePath) != std::wstring::npos))
                {
                    return false;
                }
            }

            bool archiveCursorAdvanced{};
            for (auto const& call : calls)
            {
                if (call.size() >= 10 && call[1] == L"Ubuntu" &&
                    call[5] == enumerateScript &&
                    call[7].ends_with(L"/archived_sessions") && call[8] == L"3001")
                {
                    archiveCursorAdvanced = true;
                    break;
                }
            }
            if (!archiveCursorAdvanced) return false;

            std::wstring fixtureDeviceId;
            for (auto const& account : accounts)
            {
                constexpr std::wstring_view prefix = L"wsl-unknown:";
                if (account.key.starts_with(prefix))
                {
                    fixtureDeviceId = account.key.substr(prefix.size());
                    break;
                }
            }
            if (fixtureDeviceId.empty()) return false;
            database.SetAppState(
                L"wsl_archive_cursor:" + fixtureDeviceId,
                L"3001");
            calls.clear();
            WslCodexCollector resumed(database, fake);
            static_cast<void>(resumed.CollectOnce());
            bool resumedArchiveCursor{};
            for (auto const& call : calls)
            {
                if (call.size() >= 10 && call[1] == distribution &&
                    call[5] == enumerateScript &&
                    call[7].ends_with(L"/archived_sessions") && call[8] == L"3001")
                {
                    resumedArchiveCursor = true;
                    break;
                }
            }
            if (!resumedArchiveCursor) return false;

            forcedRootPermissionDistribution = L"Ubuntu";
            auto const rootFailure = collector.CollectOnce();
            forcedRootPermissionDistribution.clear();
            auto const afterRootFailure = database.GetDeviceSummaries(10);
            auto statusFor = [](std::vector<DeviceSummary> const& devices, std::wstring_view id)
            {
                auto const found = std::find_if(devices.begin(), devices.end(), [&](auto const& device)
                {
                    return device.id == id;
                });
                return found == devices.end() ? DeviceSyncStatus::Never : found->syncStatus;
            };
            if (rootFailure.errors != 1 || rootFailure.distributionsScanned != 1 ||
                statusFor(afterRootFailure, fixtureDistributionDevice->id) !=
                    DeviceSyncStatus::Synced ||
                statusFor(afterRootFailure, ubuntuDevice->id) != DeviceSyncStatus::Failed)
            {
                return false;
            }

            missingRootDistribution = L"Ubuntu";
            auto const noCodexRoot = collector.CollectOnce();
            missingRootDistribution.clear();
            auto const afterNoRoot = database.GetDeviceSummaries(10);
            auto const noRootDevice = std::find_if(
                afterNoRoot.begin(), afterNoRoot.end(), [&](auto const& device)
                {
                    return device.id == ubuntuDevice->id;
                });
            if (noCodexRoot.errors != 0 || noCodexRoot.distributionsScanned != 2 ||
                noRootDevice == afterNoRoot.end() ||
                noRootDevice->syncStatus != DeviceSyncStatus::Synced ||
                noRootDevice->lastSuccess <= 0 || !noRootDevice->lastError.empty())
            {
                return false;
            }

            forcedEnumerationFailureDistribution = L"Ubuntu";
            auto const enumerationFailure = collector.CollectOnce();
            forcedEnumerationFailureDistribution.clear();
            auto const afterEnumerationFailure = database.GetDeviceSummaries(10);
            if (enumerationFailure.errors != 1 ||
                enumerationFailure.distributionsScanned != 2 ||
                statusFor(afterEnumerationFailure, fixtureDistributionDevice->id) !=
                    DeviceSyncStatus::Synced ||
                statusFor(afterEnumerationFailure, ubuntuDevice->id) !=
                    DeviceSyncStatus::PartialError)
            {
                return false;
            }

            auto syncSnapshot = [&]
            {
                std::unordered_map<std::wstring, std::pair<int64_t, DeviceSyncStatus>> snapshot;
                for (auto const& device : database.GetDeviceSummaries(100))
                {
                    snapshot.emplace(device.id, std::pair{device.lastAttempt, device.syncStatus});
                }
                return snapshot;
            };
            auto const beforeStopped = syncSnapshot();
            std::stop_source stopped;
            stopped.request_stop();
            static_cast<void>(collector.CollectOnce(stopped.get_token()));
            if (syncSnapshot() != beforeStopped) return false;

            auto const beforeUnavailable = syncSnapshot();
            WslCodexCollector unavailable(database, [](
                std::vector<std::wstring> const&,
                WslProcessOptions const&,
                std::stop_token)
            {
                return WslProcessResult{};
            });
            auto const unavailableResult = unavailable.CollectOnce();
            if (unavailableResult.discoverySucceeded || unavailableResult.errors != 1 ||
                syncSnapshot() != beforeUnavailable)
            {
                return false;
            }

            auto utf8List = ParseDistributions(Bytes("Ubuntu\n\xE5\xBC\x80\xE5\x8F\x91\xE7\x8E\xAF\xE5\xA2\x83\n"));
            return utf8List && utf8List->size() == 2;
        }
        catch (...)
        {
            return false;
        }
    }
}
