#include "SurfacePreferences.h"

#include "Database.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace tokenometer
{
    namespace
    {
        constexpr std::wstring_view StateKey = L"surface_preferences_v1";
        constexpr std::wstring_view Header = L"TSP3;";
        constexpr std::wstring_view LegacyHeader = L"TSP2;";
        constexpr size_t MaxSerializedCharacters = 12 * 1024;
        constexpr size_t MaxAccountCharacters = 40;
        constexpr size_t MaxCustomTextCharacters = 48;

        bool IsSafeText(std::wstring_view value, size_t maximum) noexcept
        {
            if (value.size() > maximum) return false;
            for (size_t index = 0; index < value.size(); ++index)
            {
                uint32_t const character = static_cast<uint16_t>(value[index]);
                if (character < 0x20 || character == 0x7f) return false;
                if (character >= 0xd800 && character <= 0xdbff)
                {
                    if (++index >= value.size()) return false;
                    uint32_t const low = static_cast<uint16_t>(value[index]);
                    if (low < 0xdc00 || low > 0xdfff) return false;
                }
                else if (character >= 0xdc00 && character <= 0xdfff)
                {
                    return false;
                }
            }
            return true;
        }

        size_t UnsignedCharacters(uint64_t value) noexcept
        {
            size_t result = 2; // One digit and the trailing semicolon.
            while (value >= 10)
            {
                value /= 10;
                ++result;
            }
            return result;
        }

        size_t SignedCharacters(int64_t value) noexcept
        {
            uint64_t magnitude = value < 0
                ? static_cast<uint64_t>(-(value + 1)) + 1
                : static_cast<uint64_t>(value);
            return UnsignedCharacters(magnitude) + (value < 0 ? 1 : 0);
        }

        size_t TextCharacters(std::wstring_view value) noexcept
        {
            return UnsignedCharacters(value.size()) + value.size() + 1;
        }

        size_t SerializedCharacters(SurfacePreferences const& value) noexcept
        {
            size_t result = Header.size();
            result += UnsignedCharacters(value.launchToTray);
            result += UnsignedCharacters(value.closeToTray);
            result += UnsignedCharacters(static_cast<uint8_t>(value.theme));
            result += SignedCharacters(value.glassOpacityPercent);
            result += UnsignedCharacters(value.blurEnabled);
            result += UnsignedCharacters(value.transparentWindow);
            result += UnsignedCharacters(value.providerColors);
            result += UnsignedCharacters(value.bubbleAlwaysOnTop);
            result += UnsignedCharacters(value.hoverPreview);
            result += UnsignedCharacters(value.hasBubblePosition);
            result += SignedCharacters(value.bubbleX);
            result += SignedCharacters(value.bubbleY);
            result += UnsignedCharacters(static_cast<uint8_t>(value.layoutPreset));
            result += UnsignedCharacters(value.customLayout.size());
            for (auto const& item : value.customLayout)
            {
                result += UnsignedCharacters(static_cast<uint8_t>(item.kind));
                result += UnsignedCharacters(static_cast<uint8_t>(item.tool));
                result += TextCharacters(item.accountLabel);
                result += UnsignedCharacters(static_cast<uint8_t>(item.quotaWindow));
                result += UnsignedCharacters(static_cast<uint8_t>(item.font));
                result += TextCharacters(item.customText);
            }
            result += UnsignedCharacters(value.tools.size());
            for (auto const& tool : value.tools)
            {
                result += UnsignedCharacters(static_cast<uint8_t>(tool.tool));
                result += UnsignedCharacters(tool.visible);
                result += UnsignedCharacters(tool.pinned);
            }
            result += UnsignedCharacters(value.overviewModules.size());
            for (auto const& module : value.overviewModules)
            {
                result += UnsignedCharacters(static_cast<uint8_t>(module.module));
                result += UnsignedCharacters(module.visible);
            }
            return result;
        }

        void AppendUnsigned(std::wstring& output, uint64_t value)
        {
            output.append(std::to_wstring(value));
            output.push_back(L';');
        }

        void AppendSigned(std::wstring& output, int64_t value)
        {
            output.append(std::to_wstring(value));
            output.push_back(L';');
        }

        void AppendText(std::wstring& output, std::wstring_view value)
        {
            output.append(std::to_wstring(value.size()));
            output.push_back(L':');
            output.append(value);
            output.push_back(L';');
        }

        class Reader final
        {
        public:
            Reader(std::wstring_view value, size_t headerSize)
                : m_value(value), m_position(headerSize) {}

            bool ReadUnsigned(uint64_t& result) noexcept
            {
                if (m_position >= m_value.size() ||
                    m_value[m_position] < L'0' || m_value[m_position] > L'9')
                {
                    return false;
                }
                uint64_t value{};
                size_t const start = m_position;
                while (m_position < m_value.size() && m_value[m_position] != L';')
                {
                    wchar_t const character = m_value[m_position++];
                    if (character < L'0' || character > L'9') return false;
                    uint64_t const digit = static_cast<uint64_t>(character - L'0');
                    if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
                    value = value * 10 + digit;
                }
                if (m_position >= m_value.size() || m_value[m_position++] != L';') return false;
                if (m_position - start > 2 && m_value[start] == L'0') return false;
                result = value;
                return true;
            }

            bool ReadSigned(int64_t& result) noexcept
            {
                if (m_position >= m_value.size()) return false;
                bool const negative = m_value[m_position] == L'-';
                if (negative) ++m_position;
                if (m_position >= m_value.size() ||
                    m_value[m_position] < L'0' || m_value[m_position] > L'9')
                {
                    return false;
                }
                uint64_t magnitude{};
                size_t const digits = m_position;
                uint64_t const limit = negative
                    ? static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1
                    : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
                while (m_position < m_value.size() && m_value[m_position] != L';')
                {
                    wchar_t const character = m_value[m_position++];
                    if (character < L'0' || character > L'9') return false;
                    uint64_t const digit = static_cast<uint64_t>(character - L'0');
                    if (magnitude > (limit - digit) / 10) return false;
                    magnitude = magnitude * 10 + digit;
                }
                if (m_position >= m_value.size() || m_value[m_position++] != L';') return false;
                if (m_position - digits > 2 && m_value[digits] == L'0') return false;
                if (negative && magnitude == 0) return false;
                if (negative && magnitude == limit)
                {
                    result = std::numeric_limits<int64_t>::min();
                }
                else
                {
                    result = negative
                        ? -static_cast<int64_t>(magnitude)
                        : static_cast<int64_t>(magnitude);
                }
                return true;
            }

            bool ReadText(std::wstring& result, size_t maximum)
            {
                if (m_position >= m_value.size() ||
                    m_value[m_position] < L'0' || m_value[m_position] > L'9')
                {
                    return false;
                }
                size_t length{};
                size_t const start = m_position;
                while (m_position < m_value.size() && m_value[m_position] != L':')
                {
                    wchar_t const character = m_value[m_position++];
                    if (character < L'0' || character > L'9') return false;
                    size_t const digit = static_cast<size_t>(character - L'0');
                    if (length > (maximum - digit) / 10) return false;
                    length = length * 10 + digit;
                }
                if (m_position >= m_value.size() || m_value[m_position++] != L':') return false;
                if (m_position - start > 2 && m_value[start] == L'0') return false;
                if (length > maximum || length > m_value.size() - m_position) return false;
                result.assign(m_value.substr(m_position, length));
                m_position += length;
                if (m_position >= m_value.size() || m_value[m_position++] != L';') return false;
                return true;
            }

            [[nodiscard]] bool Done() const noexcept
            {
                return m_position == m_value.size();
            }

        private:
            std::wstring_view m_value;
            size_t m_position{};
        };

        bool ReadBool(Reader& reader, bool& value) noexcept
        {
            uint64_t serialized{};
            if (!reader.ReadUnsigned(serialized) || serialized > 1) return false;
            value = serialized != 0;
            return true;
        }

        template<typename Enum>
        bool ReadEnum(Reader& reader, Enum& value, Enum maximum) noexcept
        {
            uint64_t serialized{};
            if (!reader.ReadUnsigned(serialized) ||
                serialized > static_cast<uint64_t>(maximum))
            {
                return false;
            }
            value = static_cast<Enum>(serialized);
            return true;
        }
    }

    bool SurfacePreferences::IsValid() const noexcept
    {
        if (theme > SurfaceTheme::Light || layoutPreset > SurfaceLayoutPreset::Custom ||
            (glassOpacityPercent != 25 && glassOpacityPercent != 50 &&
             glassOpacityPercent != 75 && glassOpacityPercent != 90) ||
            bubbleX < -100000 || bubbleX > 100000 ||
            bubbleY < -100000 || bubbleY > 100000 ||
            customLayout.size() > MaxLayoutItems || tools.size() > MaxToolEntries)
        {
            return false;
        }
        for (auto const& item : customLayout)
        {
            if (item.kind > SurfaceLayoutItemKind::CustomText ||
                item.tool > SurfaceTool::ChatGpt ||
                item.quotaWindow > SurfaceQuotaWindow::Weekly ||
                item.font > SurfaceFontStyle::Emphasis ||
                !IsSafeText(item.accountLabel, MaxAccountCharacters) ||
                !IsSafeText(item.customText, MaxCustomTextCharacters))
            {
                return false;
            }
        }
        for (size_t index = 0; index < tools.size(); ++index)
        {
            if (tools[index].tool > SurfaceTool::ChatGpt) return false;
            for (size_t prior = 0; prior < index; ++prior)
            {
                if (tools[index].tool == tools[prior].tool) return false;
            }
        }
        if (overviewModules.size() != 5) return false;
        bool anyOverviewModuleVisible = false;
        for (size_t index = 0; index < overviewModules.size(); ++index)
        {
            if (overviewModules[index].module > OverviewModule::RecentSessions) return false;
            anyOverviewModuleVisible = anyOverviewModuleVisible || overviewModules[index].visible;
            for (size_t prior = 0; prior < index; ++prior)
            {
                if (overviewModules[index].module == overviewModules[prior].module) return false;
            }
        }
        if (!anyOverviewModuleVisible) return false;
        return SerializedCharacters(*this) <= MaxSerializedCharacters;
    }

    std::wstring SurfacePreferences::Serialize() const
    {
        if (!IsValid()) throw std::invalid_argument("Surface preferences are invalid");

        std::wstring output(Header);
        output.reserve(SerializedCharacters(*this));
        AppendUnsigned(output, launchToTray);
        AppendUnsigned(output, closeToTray);
        AppendUnsigned(output, static_cast<uint8_t>(theme));
        AppendSigned(output, glassOpacityPercent);
        AppendUnsigned(output, blurEnabled);
        AppendUnsigned(output, transparentWindow);
        AppendUnsigned(output, providerColors);
        AppendUnsigned(output, bubbleAlwaysOnTop);
        AppendUnsigned(output, hoverPreview);
        AppendUnsigned(output, hasBubblePosition);
        AppendSigned(output, bubbleX);
        AppendSigned(output, bubbleY);
        AppendUnsigned(output, static_cast<uint8_t>(layoutPreset));
        AppendUnsigned(output, customLayout.size());
        for (auto const& item : customLayout)
        {
            AppendUnsigned(output, static_cast<uint8_t>(item.kind));
            AppendUnsigned(output, static_cast<uint8_t>(item.tool));
            AppendText(output, item.accountLabel);
            AppendUnsigned(output, static_cast<uint8_t>(item.quotaWindow));
            AppendUnsigned(output, static_cast<uint8_t>(item.font));
            AppendText(output, item.customText);
        }
        AppendUnsigned(output, tools.size());
        for (auto const& tool : tools)
        {
            AppendUnsigned(output, static_cast<uint8_t>(tool.tool));
            AppendUnsigned(output, tool.visible);
            AppendUnsigned(output, tool.pinned);
        }
        AppendUnsigned(output, overviewModules.size());
        for (auto const& module : overviewModules)
        {
            AppendUnsigned(output, static_cast<uint8_t>(module.module));
            AppendUnsigned(output, module.visible);
        }
        return output;
    }

    std::optional<SurfacePreferences> SurfacePreferences::Deserialize(
        std::wstring_view value) noexcept
    {
        try
        {
            if (value.size() > MaxSerializedCharacters ||
                (!value.starts_with(Header) && !value.starts_with(LegacyHeader)))
            {
                return std::nullopt;
            }

            bool const legacy = value.starts_with(LegacyHeader);
            Reader reader(value, legacy ? LegacyHeader.size() : Header.size());
            SurfacePreferences result;
            int64_t number{};
            uint64_t count{};
            if (!ReadBool(reader, result.launchToTray) ||
                !ReadBool(reader, result.closeToTray) ||
                !ReadEnum(reader, result.theme, SurfaceTheme::Light) ||
                !reader.ReadSigned(number) ||
                number < std::numeric_limits<int>::min() ||
                number > std::numeric_limits<int>::max())
            {
                return std::nullopt;
            }
            result.glassOpacityPercent = static_cast<int>(number);
            if (!ReadBool(reader, result.blurEnabled) ||
                !ReadBool(reader, result.transparentWindow) ||
                !ReadBool(reader, result.providerColors) ||
                !ReadBool(reader, result.bubbleAlwaysOnTop) ||
                !ReadBool(reader, result.hoverPreview) ||
                !ReadBool(reader, result.hasBubblePosition) ||
                !reader.ReadSigned(number) ||
                number < std::numeric_limits<int>::min() ||
                number > std::numeric_limits<int>::max())
            {
                return std::nullopt;
            }
            result.bubbleX = static_cast<int>(number);
            if (!reader.ReadSigned(number) ||
                number < std::numeric_limits<int>::min() ||
                number > std::numeric_limits<int>::max())
            {
                return std::nullopt;
            }
            result.bubbleY = static_cast<int>(number);
            if (!ReadEnum(reader, result.layoutPreset, SurfaceLayoutPreset::Custom) ||
                !reader.ReadUnsigned(count) || count > MaxLayoutItems)
            {
                return std::nullopt;
            }
            result.customLayout.clear();
            result.customLayout.reserve(static_cast<size_t>(count));
            for (uint64_t index = 0; index < count; ++index)
            {
                SurfaceLayoutItem item;
                if (!ReadEnum(reader, item.kind, SurfaceLayoutItemKind::CustomText) ||
                    !ReadEnum(reader, item.tool, SurfaceTool::ChatGpt) ||
                    !reader.ReadText(item.accountLabel, MaxAccountCharacters) ||
                    !ReadEnum(reader, item.quotaWindow, SurfaceQuotaWindow::Weekly) ||
                    !ReadEnum(reader, item.font, SurfaceFontStyle::Emphasis) ||
                    !reader.ReadText(item.customText, MaxCustomTextCharacters))
                {
                    return std::nullopt;
                }
                result.customLayout.push_back(std::move(item));
            }
            if (!reader.ReadUnsigned(count) || count > MaxToolEntries)
            {
                return std::nullopt;
            }
            result.tools.clear();
            result.tools.reserve(static_cast<size_t>(count));
            for (uint64_t index = 0; index < count; ++index)
            {
                SurfaceToolPreference tool;
                if (!ReadEnum(reader, tool.tool, SurfaceTool::ChatGpt) ||
                    !ReadBool(reader, tool.visible) || !ReadBool(reader, tool.pinned))
                {
                    return std::nullopt;
                }
                result.tools.push_back(tool);
            }
            if (!legacy)
            {
                if (!reader.ReadUnsigned(count) || count != 5)
                {
                    return std::nullopt;
                }
                result.overviewModules.clear();
                result.overviewModules.reserve(static_cast<size_t>(count));
                for (uint64_t index = 0; index < count; ++index)
                {
                    OverviewModulePreference module;
                    if (!ReadEnum(reader, module.module, OverviewModule::RecentSessions) ||
                        !ReadBool(reader, module.visible))
                    {
                        return std::nullopt;
                    }
                    result.overviewModules.push_back(module);
                }
            }
            if (!reader.Done() || !result.IsValid()) return std::nullopt;
            return result;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    SurfacePreferences SurfacePreferences::Load(Database& database)
    {
        auto const stored = database.GetAppState(StateKey);
        if (!stored) return {};
        auto parsed = Deserialize(*stored);
        return parsed ? std::move(*parsed) : SurfacePreferences{};
    }

    void SurfacePreferences::Save(Database& database) const
    {
        database.SetAppState(StateKey, Serialize());
    }

    bool SurfacePreferences::SelfTest()
    {
        try
        {
            Database database(L":memory:");
            database.Initialize();
            if (!SurfacePreferences{}.IsValid()) return false;

            SurfacePreferences fixture;
            fixture.launchToTray = true;
            fixture.closeToTray = false;
            fixture.theme = SurfaceTheme::Light;
            fixture.glassOpacityPercent = 90;
            fixture.blurEnabled = false;
            fixture.transparentWindow = true;
            fixture.providerColors = false;
            fixture.bubbleAlwaysOnTop = false;
            fixture.hoverPreview = false;
            fixture.hasBubblePosition = true;
            fixture.bubbleX = -2480;
            fixture.bubbleY = 126;
            fixture.layoutPreset = SurfaceLayoutPreset::Custom;
            fixture.customLayout = {
                { SurfaceLayoutItemKind::QuotaBar, SurfaceTool::Codex,
                  L"个人;帐户", SurfaceQuotaWindow::Weekly, SurfaceFontStyle::Mono, L"" },
                { SurfaceLayoutItemKind::CustomText, SurfaceTool::ChatGpt,
                  L"团队", SurfaceQuotaWindow::Nearest, SurfaceFontStyle::Emphasis, L"余量 42%" },
            };
            fixture.tools = {
                { SurfaceTool::ChatGpt, true, false },
                { SurfaceTool::Codex, false, true },
            };
            fixture.overviewModules = {
                { OverviewModule::RecentSessions, true },
                { OverviewModule::TokenSummary, true },
                { OverviewModule::ActivityHeatmap, false },
                { OverviewModule::TokenActivity, true },
                { OverviewModule::CodexLimits, false },
            };
            fixture.Save(database);
            if (Load(database) != fixture) return false;

            auto serialized = fixture.Serialize();
            auto decoded = Deserialize(serialized);
            if (!decoded || *decoded != fixture ||
                Deserialize(serialized + L"garbage") ||
                Deserialize(serialized.substr(0, serialized.size() - 1)) ||
                Deserialize(L"TSP1;"))
            {
                return false;
            }

            std::wstring overviewSuffix;
            AppendUnsigned(overviewSuffix, fixture.overviewModules.size());
            for (auto const& module : fixture.overviewModules)
            {
                AppendUnsigned(overviewSuffix, static_cast<uint8_t>(module.module));
                AppendUnsigned(overviewSuffix, module.visible);
            }
            auto legacy = serialized.substr(0, serialized.size() - overviewSuffix.size());
            legacy.replace(0, Header.size(), LegacyHeader);
            auto migrated = Deserialize(legacy);
            auto expectedMigration = fixture;
            expectedMigration.overviewModules = SurfacePreferences{}.overviewModules;
            if (!migrated || *migrated != expectedMigration) return false;

            SurfacePreferences maximum;
            maximum.customLayout.assign(MaxLayoutItems, SurfaceLayoutItem{});
            for (auto& item : maximum.customLayout)
            {
                item.accountLabel.assign(MaxAccountCharacters, L'a');
                item.customText.assign(MaxCustomTextCharacters, L'b');
            }
            if (!maximum.IsValid() || maximum.Serialize().size() > MaxSerializedCharacters)
            {
                return false;
            }

            auto invalid = fixture;
            invalid.glassOpacityPercent = 42;
            if (invalid.IsValid()) return false;
            invalid = fixture;
            invalid.customLayout.resize(MaxLayoutItems + 1);
            if (invalid.IsValid()) return false;
            invalid = fixture;
            invalid.tools.push_back(invalid.tools.front());
            if (invalid.IsValid()) return false;
            invalid = fixture;
            invalid.overviewModules.pop_back();
            if (invalid.IsValid()) return false;
            invalid = fixture;
            invalid.overviewModules.back() = invalid.overviewModules.front();
            if (invalid.IsValid()) return false;
            invalid = fixture;
            for (auto& module : invalid.overviewModules) module.visible = false;
            if (invalid.IsValid()) return false;
            invalid = fixture;
            invalid.customLayout.front().customText = L"line\nbreak";
            if (invalid.IsValid()) return false;

            database.SetAppState(StateKey, L"corrupt");
            return Load(database) == SurfacePreferences{};
        }
        catch (...)
        {
            return false;
        }
    }
}
