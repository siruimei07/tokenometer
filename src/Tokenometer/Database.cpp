#include "Database.h"

#include <windows.h>
#include <shlobj.h>
#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace tokenometer
{
    namespace
    {
        void ThrowIfCancelled(std::stop_token stopToken)
        {
            if (stopToken.stop_requested())
            {
                throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            }
        }

        std::string Utf8(std::wstring_view value)
        {
            if (value.empty())
            {
                return {};
            }
            int const size = WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                value.data(),
                static_cast<int>(value.size()),
                nullptr,
                0,
                nullptr,
                nullptr);
            if (size <= 0)
            {
                throw std::runtime_error("UTF-8 conversion failed");
            }
            std::string result(static_cast<size_t>(size), '\0');
            if (!WideCharToMultiByte(
                    CP_UTF8,
                    WC_ERR_INVALID_CHARS,
                    value.data(),
                    static_cast<int>(value.size()),
                    result.data(),
                    size,
                    nullptr,
                    nullptr))
            {
                throw std::runtime_error("UTF-8 conversion failed");
            }
            return result;
        }

        std::wstring Wide(char const* value)
        {
            if (!value || !*value)
            {
                return {};
            }
            int const length = static_cast<int>(std::char_traits<char>::length(value));
            int const size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, length, nullptr, 0);
            if (size <= 0)
            {
                return {};
            }
            std::wstring result(static_cast<size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, length, result.data(), size);
            return result;
        }

        class Statement final
        {
        public:
            Statement(sqlite3* database, char const* sql) : m_database(database)
            {
                if (sqlite3_prepare_v2(database, sql, -1, &m_statement, nullptr) != SQLITE_OK)
                {
                    throw std::runtime_error(sqlite3_errmsg(database));
                }
            }

            ~Statement()
            {
                sqlite3_finalize(m_statement);
            }

            Statement(Statement const&) = delete;
            Statement& operator=(Statement const&) = delete;

            void Bind(int index, std::wstring_view value)
            {
                auto const utf8 = Utf8(value);
                if (sqlite3_bind_text(
                        m_statement,
                        index,
                        utf8.c_str(),
                        static_cast<int>(utf8.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK)
                {
                    throw std::runtime_error(sqlite3_errmsg(m_database));
                }
            }

            void Bind(int index, int value)
            {
                if (sqlite3_bind_int(m_statement, index, value) != SQLITE_OK)
                {
                    throw std::runtime_error(sqlite3_errmsg(m_database));
                }
            }

            void Bind(int index, int64_t value)
            {
                if (sqlite3_bind_int64(m_statement, index, value) != SQLITE_OK)
                {
                    throw std::runtime_error(sqlite3_errmsg(m_database));
                }
            }

            void Bind(int index, double value)
            {
                if (value < 0)
                {
                    if (sqlite3_bind_null(m_statement, index) != SQLITE_OK)
                    {
                        throw std::runtime_error(sqlite3_errmsg(m_database));
                    }
                }
                else if (sqlite3_bind_double(m_statement, index, value) != SQLITE_OK)
                {
                    throw std::runtime_error(sqlite3_errmsg(m_database));
                }
            }

            bool Step()
            {
                int const result = sqlite3_step(m_statement);
                if (result == SQLITE_ROW)
                {
                    return true;
                }
                if (result != SQLITE_DONE)
                {
                    throw std::runtime_error(sqlite3_errmsg(m_database));
                }
                return false;
            }

            void Reset()
            {
                if (sqlite3_reset(m_statement) != SQLITE_OK ||
                    sqlite3_clear_bindings(m_statement) != SQLITE_OK)
                {
                    throw std::runtime_error(sqlite3_errmsg(m_database));
                }
            }

            [[nodiscard]] int64_t Int64(int column) const
            {
                return sqlite3_column_int64(m_statement, column);
            }

            [[nodiscard]] int Int(int column) const
            {
                return sqlite3_column_int(m_statement, column);
            }

            [[nodiscard]] double Double(int column, double fallback = -1.0) const
            {
                return sqlite3_column_type(m_statement, column) == SQLITE_NULL
                    ? fallback
                    : sqlite3_column_double(m_statement, column);
            }

            [[nodiscard]] std::wstring Text(int column) const
            {
                return Wide(reinterpret_cast<char const*>(sqlite3_column_text(m_statement, column)));
            }

        private:
            sqlite3* m_database{};
            sqlite3_stmt* m_statement{};
        };

        int64_t UnixNow()
        {
            return std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        std::wstring LocalCalendarDay(int daysAgo = 0)
        {
            SYSTEMTIME local{};
            GetLocalTime(&local);
            local.wHour = 12;
            local.wMinute = 0;
            local.wSecond = 0;
            local.wMilliseconds = 0;

            FILETIME value{};
            if (!SystemTimeToFileTime(&local, &value))
            {
                throw std::runtime_error("The local calendar date is unavailable");
            }
            ULARGE_INTEGER ticks{};
            ticks.LowPart = value.dwLowDateTime;
            ticks.HighPart = value.dwHighDateTime;
            constexpr uint64_t ticksPerDay = 86400ULL * 10'000'000ULL;
            uint64_t const availableDays = ticks.QuadPart / ticksPerDay;
            uint64_t const requestedDays = std::min<uint64_t>(
                static_cast<uint64_t>(std::max(daysAgo, 0)),
                availableDays);
            ticks.QuadPart -= requestedDays * ticksPerDay;
            value.dwLowDateTime = ticks.LowPart;
            value.dwHighDateTime = ticks.HighPart;
            if (!FileTimeToSystemTime(&value, &local))
            {
                throw std::runtime_error("The local calendar date is unavailable");
            }

            std::array<wchar_t, 16> text{};
            swprintf_s(text.data(), text.size(), L"%04u-%02u-%02u", local.wYear, local.wMonth, local.wDay);
            return text.data();
        }

        void BindCounts(Statement& statement, int first, TokenCounts const& counts)
        {
            statement.Bind(first, counts.input);
            statement.Bind(first + 1, counts.cachedInput);
            statement.Bind(first + 2, counts.cacheWriteInput);
            statement.Bind(first + 3, counts.output);
            statement.Bind(first + 4, counts.reasoningOutput);
            statement.Bind(first + 5, counts.reportedTotal);
        }

        TokenCounts ReadCounts(Statement const& statement, int first)
        {
            return {
                statement.Int64(first),
                statement.Int64(first + 1),
                statement.Int64(first + 2),
                statement.Int64(first + 3),
                statement.Int64(first + 4),
                statement.Int64(first + 5)
            };
        }
    }

    Database::Database(std::filesystem::path path) : m_path(std::move(path))
    {
        if (m_path != L":memory:")
        {
            std::filesystem::create_directories(m_path.parent_path());
        }
        auto const utf8Path = Utf8(m_path.wstring());
        int const result = sqlite3_open_v2(
            utf8Path.c_str(),
            &m_database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr);
        if (result != SQLITE_OK)
        {
            std::string const message = m_database ? sqlite3_errmsg(m_database) : "SQLite open failed";
            if (m_database)
            {
                sqlite3_close(m_database);
                m_database = nullptr;
            }
            throw std::runtime_error(message);
        }
    }

    Database::~Database()
    {
        if (m_database)
        {
            sqlite3_close(m_database);
        }
    }

    std::filesystem::path Database::DefaultDataDirectory()
    {
        PWSTR rawPath{};
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &rawPath)))
        {
            throw std::runtime_error("Local application data directory is unavailable");
        }
        std::filesystem::path path(rawPath);
        CoTaskMemFree(rawPath);
        return path / L"Tokenometer";
    }

    void Database::Initialize()
    {
        std::scoped_lock lock(m_mutex);
        Execute("PRAGMA foreign_keys=ON;");
        Execute("PRAGMA busy_timeout=3000;");
        Execute("PRAGMA synchronous=NORMAL;");
        Execute("PRAGMA temp_store=MEMORY;");
        Execute("PRAGMA journal_size_limit=67108864;");
        Execute("PRAGMA wal_autocheckpoint=1000;");
        if (m_path != L":memory:")
        {
            Execute("PRAGMA journal_mode=WAL;");
            Execute("PRAGMA auto_vacuum=INCREMENTAL;");
        }

        int version{};
        {
            Statement versionStatement(m_database, "PRAGMA user_version;");
            versionStatement.Step();
            version = versionStatement.Int(0);
        }
        if (version > 6)
        {
            throw std::runtime_error("The usage database was created by a newer Tokenometer version");
        }

        if (version == 0)
        {
            Transaction([&]
            {
                Execute(R"sql(
            CREATE TABLE IF NOT EXISTS schema_migrations(
                version INTEGER PRIMARY KEY,
                applied_at INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS app_state(
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS devices(
                id TEXT PRIMARY KEY,
                display_name TEXT NOT NULL,
                last_seen INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS source_files(
                path TEXT PRIMARY KEY,
                file_identity TEXT NOT NULL UNIQUE,
                size INTEGER NOT NULL DEFAULT 0,
                modified_at INTEGER NOT NULL DEFAULT 0,
                offset INTEGER NOT NULL DEFAULT 0,
                session_id TEXT NOT NULL DEFAULT '',
                project TEXT NOT NULL DEFAULT '',
                model TEXT NOT NULL DEFAULT '',
                turn_id TEXT NOT NULL DEFAULT '',
                prompt_index INTEGER NOT NULL DEFAULT 0,
                tracking_started INTEGER NOT NULL DEFAULT 1,
                forked INTEGER NOT NULL DEFAULT 0,
                session_created_at INTEGER NOT NULL DEFAULT 0,
                cumulative_input INTEGER NOT NULL DEFAULT 0,
                cumulative_cached INTEGER NOT NULL DEFAULT 0,
                cumulative_cache_write INTEGER NOT NULL DEFAULT 0,
                cumulative_output INTEGER NOT NULL DEFAULT 0,
                cumulative_reasoning INTEGER NOT NULL DEFAULT 0,
                cumulative_total INTEGER NOT NULL DEFAULT 0
            );
            CREATE TABLE IF NOT EXISTS sessions(
                id TEXT PRIMARY KEY,
                source_path TEXT NOT NULL,
                source_kind TEXT NOT NULL,
                account_id TEXT NOT NULL DEFAULT 'current',
                title TEXT NOT NULL DEFAULT '',
                project TEXT NOT NULL DEFAULT '',
                model TEXT NOT NULL DEFAULT '',
                device_id TEXT NOT NULL DEFAULT '',
                started_at INTEGER NOT NULL DEFAULT 0,
                updated_at INTEGER NOT NULL DEFAULT 0,
                message_count INTEGER NOT NULL DEFAULT 0
            );
            CREATE INDEX IF NOT EXISTS sessions_updated_idx ON sessions(updated_at DESC);
            CREATE TABLE IF NOT EXISTS ingested_records(
                session_id TEXT NOT NULL,
                source_offset INTEGER NOT NULL,
                record_kind INTEGER NOT NULL,
                PRIMARY KEY(session_id, source_offset, record_kind)
            ) WITHOUT ROWID;
            CREATE TABLE IF NOT EXISTS usage_events(
                id INTEGER PRIMARY KEY,
                source_path TEXT NOT NULL,
                source_offset INTEGER NOT NULL,
                session_id TEXT NOT NULL,
                turn_id TEXT NOT NULL DEFAULT '',
                prompt_index INTEGER NOT NULL DEFAULT 0,
                timestamp INTEGER NOT NULL,
                model TEXT NOT NULL DEFAULT '',
                input_tokens INTEGER NOT NULL DEFAULT 0,
                cached_input_tokens INTEGER NOT NULL DEFAULT 0,
                cache_write_input_tokens INTEGER NOT NULL DEFAULT 0,
                output_tokens INTEGER NOT NULL DEFAULT 0,
                reasoning_output_tokens INTEGER NOT NULL DEFAULT 0,
                reported_total_tokens INTEGER NOT NULL DEFAULT 0,
                UNIQUE(session_id, source_offset)
            );
            CREATE INDEX IF NOT EXISTS usage_events_time_idx ON usage_events(timestamp);
            CREATE INDEX IF NOT EXISTS usage_events_session_idx ON usage_events(session_id, timestamp);
            CREATE TABLE IF NOT EXISTS prompt_events(
                id INTEGER PRIMARY KEY,
                source_path TEXT NOT NULL,
                source_offset INTEGER NOT NULL,
                session_id TEXT NOT NULL,
                turn_id TEXT NOT NULL DEFAULT '',
                prompt_index INTEGER NOT NULL,
                timestamp INTEGER NOT NULL,
                UNIQUE(session_id, source_offset)
            );
            CREATE TABLE IF NOT EXISTS tool_events(
                id INTEGER PRIMARY KEY,
                source_path TEXT NOT NULL,
                source_offset INTEGER NOT NULL,
                session_id TEXT NOT NULL,
                turn_id TEXT NOT NULL DEFAULT '',
                prompt_index INTEGER NOT NULL DEFAULT 0,
                timestamp INTEGER NOT NULL,
                name TEXT NOT NULL,
                call_id TEXT NOT NULL DEFAULT '',
                input_length INTEGER NOT NULL DEFAULT 0,
                output_offset INTEGER NOT NULL DEFAULT 0,
                output_length INTEGER NOT NULL DEFAULT 0,
                UNIQUE(session_id, source_offset)
            );
            CREATE INDEX IF NOT EXISTS tool_events_session_idx ON tool_events(session_id, timestamp);
            CREATE INDEX IF NOT EXISTS tool_events_call_idx ON tool_events(session_id, call_id);
            CREATE TABLE IF NOT EXISTS turn_usage(
                session_id TEXT NOT NULL,
                turn_id TEXT NOT NULL DEFAULT '',
                prompt_index INTEGER NOT NULL DEFAULT 0,
                model TEXT NOT NULL DEFAULT '',
                first_timestamp INTEGER NOT NULL,
                last_timestamp INTEGER NOT NULL,
                input_tokens INTEGER NOT NULL DEFAULT 0,
                cached_input_tokens INTEGER NOT NULL DEFAULT 0,
                cache_write_input_tokens INTEGER NOT NULL DEFAULT 0,
                output_tokens INTEGER NOT NULL DEFAULT 0,
                reasoning_output_tokens INTEGER NOT NULL DEFAULT 0,
                reported_total_tokens INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY(session_id, turn_id, prompt_index)
            );
            CREATE INDEX IF NOT EXISTS turn_usage_session_idx
                ON turn_usage(session_id, first_timestamp DESC);
            CREATE TABLE IF NOT EXISTS turn_tools(
                session_id TEXT NOT NULL,
                turn_id TEXT NOT NULL DEFAULT '',
                prompt_index INTEGER NOT NULL DEFAULT 0,
                name TEXT NOT NULL,
                first_timestamp INTEGER NOT NULL,
                last_timestamp INTEGER NOT NULL,
                call_count INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY(session_id, turn_id, prompt_index, name)
            );
            CREATE TABLE IF NOT EXISTS hourly_usage(
                source_path TEXT NOT NULL,
                hour_start INTEGER NOT NULL,
                day TEXT NOT NULL,
                session_id TEXT NOT NULL,
                source_kind TEXT NOT NULL,
                account_id TEXT NOT NULL DEFAULT 'current',
                tool TEXT NOT NULL,
                model TEXT NOT NULL DEFAULT '',
                project TEXT NOT NULL DEFAULT '',
                device_id TEXT NOT NULL DEFAULT '',
                input_tokens INTEGER NOT NULL DEFAULT 0,
                cached_input_tokens INTEGER NOT NULL DEFAULT 0,
                cache_write_input_tokens INTEGER NOT NULL DEFAULT 0,
                output_tokens INTEGER NOT NULL DEFAULT 0,
                reasoning_output_tokens INTEGER NOT NULL DEFAULT 0,
                reported_total_tokens INTEGER NOT NULL DEFAULT 0,
                messages INTEGER NOT NULL DEFAULT 0,
                tool_calls INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY(source_path, hour_start, day, session_id, source_kind, account_id, tool, model, project, device_id)
            );
            CREATE INDEX IF NOT EXISTS hourly_usage_time_idx ON hourly_usage(hour_start);
            CREATE INDEX IF NOT EXISTS hourly_usage_day_idx ON hourly_usage(day, hour_start);
            CREATE TABLE IF NOT EXISTS daily_usage(
                source_path TEXT NOT NULL,
                day TEXT NOT NULL,
                session_id TEXT NOT NULL,
                source_kind TEXT NOT NULL,
                account_id TEXT NOT NULL DEFAULT 'current',
                tool TEXT NOT NULL,
                model TEXT NOT NULL DEFAULT '',
                project TEXT NOT NULL DEFAULT '',
                device_id TEXT NOT NULL DEFAULT '',
                first_timestamp INTEGER NOT NULL,
                last_timestamp INTEGER NOT NULL,
                input_tokens INTEGER NOT NULL DEFAULT 0,
                cached_input_tokens INTEGER NOT NULL DEFAULT 0,
                cache_write_input_tokens INTEGER NOT NULL DEFAULT 0,
                output_tokens INTEGER NOT NULL DEFAULT 0,
                reasoning_output_tokens INTEGER NOT NULL DEFAULT 0,
                reported_total_tokens INTEGER NOT NULL DEFAULT 0,
                messages INTEGER NOT NULL DEFAULT 0,
                tool_calls INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY(source_path, day, session_id, source_kind, account_id, tool, model, project, device_id)
            );
            CREATE INDEX IF NOT EXISTS daily_usage_day_idx ON daily_usage(day);
            CREATE INDEX IF NOT EXISTS daily_usage_model_idx ON daily_usage(model, day);
            CREATE INDEX IF NOT EXISTS daily_usage_project_idx ON daily_usage(project, day);
            CREATE INDEX IF NOT EXISTS daily_usage_account_idx ON daily_usage(account_id, day);
            CREATE TABLE IF NOT EXISTS rate_limits(
                provider TEXT NOT NULL,
                account_id TEXT NOT NULL,
                limit_id TEXT NOT NULL DEFAULT '',
                limit_name TEXT NOT NULL DEFAULT '',
                primary_used_percent REAL,
                primary_window_minutes INTEGER NOT NULL DEFAULT 0,
                primary_resets_at INTEGER NOT NULL DEFAULT 0,
                secondary_used_percent REAL,
                secondary_window_minutes INTEGER NOT NULL DEFAULT 0,
                secondary_resets_at INTEGER NOT NULL DEFAULT 0,
                plan_type TEXT NOT NULL DEFAULT '',
                captured_at INTEGER NOT NULL,
                PRIMARY KEY(provider, account_id)
            );
            CREATE TABLE IF NOT EXISTS chatgpt_import_sources(
                account_id TEXT NOT NULL,
                source_path TEXT NOT NULL,
                source_hash TEXT NOT NULL,
                modified_at INTEGER NOT NULL,
                size INTEGER NOT NULL,
                imported_at INTEGER NOT NULL,
                PRIMARY KEY(account_id, source_path)
            );
            CREATE TABLE IF NOT EXISTS chatgpt_estimated_sessions(
                account_id TEXT NOT NULL,
                session_id TEXT NOT NULL,
                source_path TEXT NOT NULL,
                source_hash TEXT NOT NULL,
                source_kind TEXT NOT NULL DEFAULT 'chatgpt-export',
                measurement_kind TEXT NOT NULL DEFAULT 'estimated' CHECK(measurement_kind='estimated'),
                model TEXT NOT NULL DEFAULT '',
                started_at INTEGER NOT NULL DEFAULT 0,
                updated_at INTEGER NOT NULL DEFAULT 0,
                messages INTEGER NOT NULL DEFAULT 0,
                prompts INTEGER NOT NULL DEFAULT 0,
                estimated_input_tokens INTEGER NOT NULL DEFAULT 0,
                estimated_output_tokens INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY(account_id, session_id),
                FOREIGN KEY(account_id, source_path)
                    REFERENCES chatgpt_import_sources(account_id, source_path) ON DELETE CASCADE
            );
            CREATE INDEX IF NOT EXISTS chatgpt_estimated_sessions_updated_idx
                ON chatgpt_estimated_sessions(account_id, updated_at DESC);
            CREATE TABLE IF NOT EXISTS chatgpt_estimated_prompts(
                account_id TEXT NOT NULL,
                session_id TEXT NOT NULL,
                prompt_index INTEGER NOT NULL,
                turn_id TEXT NOT NULL DEFAULT '',
                measurement_kind TEXT NOT NULL DEFAULT 'estimated' CHECK(measurement_kind='estimated'),
                timestamp INTEGER NOT NULL DEFAULT 0,
                day TEXT NOT NULL DEFAULT 'unknown',
                model TEXT NOT NULL DEFAULT '',
                messages INTEGER NOT NULL DEFAULT 0,
                estimated_input_tokens INTEGER NOT NULL DEFAULT 0,
                estimated_output_tokens INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY(account_id, session_id, prompt_index),
                FOREIGN KEY(account_id, session_id)
                    REFERENCES chatgpt_estimated_sessions(account_id, session_id) ON DELETE CASCADE
            );
            CREATE INDEX IF NOT EXISTS chatgpt_estimated_prompts_time_idx
                ON chatgpt_estimated_prompts(timestamp);
            CREATE TABLE IF NOT EXISTS chatgpt_estimated_daily(
                account_id TEXT NOT NULL,
                session_id TEXT NOT NULL,
                day TEXT NOT NULL,
                source_kind TEXT NOT NULL DEFAULT 'chatgpt-export',
                measurement_kind TEXT NOT NULL DEFAULT 'estimated' CHECK(measurement_kind='estimated'),
                model TEXT NOT NULL DEFAULT '',
                estimated_input_tokens INTEGER NOT NULL DEFAULT 0,
                estimated_output_tokens INTEGER NOT NULL DEFAULT 0,
                messages INTEGER NOT NULL DEFAULT 0,
                prompts INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY(account_id, session_id, day, model),
                FOREIGN KEY(account_id, session_id)
                    REFERENCES chatgpt_estimated_sessions(account_id, session_id) ON DELETE CASCADE
            );
            CREATE INDEX IF NOT EXISTS chatgpt_estimated_daily_day_idx
                ON chatgpt_estimated_daily(day, account_id);
            INSERT OR IGNORE INTO schema_migrations(version, applied_at)
                VALUES(1, CAST(strftime('%s','now') AS INTEGER));
            INSERT OR IGNORE INTO schema_migrations(version, applied_at)
                VALUES(2, CAST(strftime('%s','now') AS INTEGER));
            INSERT OR IGNORE INTO schema_migrations(version, applied_at)
                VALUES(3, CAST(strftime('%s','now') AS INTEGER));
            INSERT OR IGNORE INTO schema_migrations(version, applied_at)
                VALUES(4, CAST(strftime('%s','now') AS INTEGER));
            INSERT OR IGNORE INTO schema_migrations(version, applied_at)
                VALUES(5, CAST(strftime('%s','now') AS INTEGER));
            INSERT OR IGNORE INTO schema_migrations(version, applied_at)
                VALUES(6, CAST(strftime('%s','now') AS INTEGER));
            PRAGMA user_version=6;
                )sql");
            });
        }
        else if (version == 1)
        {
            Transaction([&]
            {
                Execute(R"sql(
                    CREATE TABLE IF NOT EXISTS app_state(
                        key TEXT PRIMARY KEY,
                        value TEXT NOT NULL
                    );
                    CREATE TABLE IF NOT EXISTS devices(
                        id TEXT PRIMARY KEY,
                        display_name TEXT NOT NULL,
                        last_seen INTEGER NOT NULL
                    );
                    CREATE TABLE IF NOT EXISTS ingested_records(
                        session_id TEXT NOT NULL,
                        source_offset INTEGER NOT NULL,
                        record_kind INTEGER NOT NULL,
                        PRIMARY KEY(session_id, source_offset, record_kind)
                    ) WITHOUT ROWID;
                    ALTER TABLE tool_events ADD COLUMN call_id TEXT NOT NULL DEFAULT '';
                    ALTER TABLE tool_events ADD COLUMN input_length INTEGER NOT NULL DEFAULT 0;
                    ALTER TABLE tool_events ADD COLUMN output_offset INTEGER NOT NULL DEFAULT 0;
                    ALTER TABLE tool_events ADD COLUMN output_length INTEGER NOT NULL DEFAULT 0;
                    CREATE INDEX IF NOT EXISTS tool_events_call_idx ON tool_events(session_id, call_id);
                    CREATE TABLE IF NOT EXISTS turn_usage(
                        session_id TEXT NOT NULL,
                        turn_id TEXT NOT NULL DEFAULT '',
                        prompt_index INTEGER NOT NULL DEFAULT 0,
                        model TEXT NOT NULL DEFAULT '',
                        first_timestamp INTEGER NOT NULL,
                        last_timestamp INTEGER NOT NULL,
                        input_tokens INTEGER NOT NULL DEFAULT 0,
                        cached_input_tokens INTEGER NOT NULL DEFAULT 0,
                        cache_write_input_tokens INTEGER NOT NULL DEFAULT 0,
                        output_tokens INTEGER NOT NULL DEFAULT 0,
                        reasoning_output_tokens INTEGER NOT NULL DEFAULT 0,
                        reported_total_tokens INTEGER NOT NULL DEFAULT 0,
                        PRIMARY KEY(session_id, turn_id, prompt_index)
                    );
                    CREATE INDEX IF NOT EXISTS turn_usage_session_idx
                        ON turn_usage(session_id, first_timestamp DESC);
                    CREATE TABLE IF NOT EXISTS turn_tools(
                        session_id TEXT NOT NULL,
                        turn_id TEXT NOT NULL DEFAULT '',
                        prompt_index INTEGER NOT NULL DEFAULT 0,
                        name TEXT NOT NULL,
                        first_timestamp INTEGER NOT NULL,
                        last_timestamp INTEGER NOT NULL,
                        call_count INTEGER NOT NULL DEFAULT 0,
                        PRIMARY KEY(session_id, turn_id, prompt_index, name)
                    );
                    CREATE TABLE IF NOT EXISTS hourly_usage(
                        source_path TEXT NOT NULL,
                        hour_start INTEGER NOT NULL,
                        day TEXT NOT NULL,
                        session_id TEXT NOT NULL,
                        source_kind TEXT NOT NULL,
                        tool TEXT NOT NULL,
                        model TEXT NOT NULL DEFAULT '',
                        project TEXT NOT NULL DEFAULT '',
                        device_id TEXT NOT NULL DEFAULT '',
                        input_tokens INTEGER NOT NULL DEFAULT 0,
                        cached_input_tokens INTEGER NOT NULL DEFAULT 0,
                        cache_write_input_tokens INTEGER NOT NULL DEFAULT 0,
                        output_tokens INTEGER NOT NULL DEFAULT 0,
                        reasoning_output_tokens INTEGER NOT NULL DEFAULT 0,
                        reported_total_tokens INTEGER NOT NULL DEFAULT 0,
                        messages INTEGER NOT NULL DEFAULT 0,
                        tool_calls INTEGER NOT NULL DEFAULT 0,
                        PRIMARY KEY(source_path, hour_start, session_id, source_kind, tool, model, project, device_id)
                    );
                    CREATE INDEX IF NOT EXISTS hourly_usage_time_idx ON hourly_usage(hour_start);
                    ALTER TABLE sessions ADD COLUMN account_id TEXT NOT NULL DEFAULT 'current';
                    ALTER TABLE daily_usage ADD COLUMN account_id TEXT NOT NULL DEFAULT 'current';
                    ALTER TABLE hourly_usage ADD COLUMN account_id TEXT NOT NULL DEFAULT 'current';
                    CREATE INDEX IF NOT EXISTS daily_usage_account_idx ON daily_usage(account_id, day);
                    DELETE FROM usage_events;
                    DELETE FROM prompt_events;
                    DELETE FROM tool_events;
                    DELETE FROM turn_usage;
                    DELETE FROM turn_tools;
                    DELETE FROM hourly_usage;
                    DELETE FROM daily_usage;
                    DELETE FROM sessions;
                    DELETE FROM source_files;
                    INSERT OR IGNORE INTO schema_migrations(version, applied_at)
                        VALUES(2, CAST(strftime('%s','now') AS INTEGER));
                    INSERT OR IGNORE INTO schema_migrations(version, applied_at)
                        VALUES(3, CAST(strftime('%s','now') AS INTEGER));
                    PRAGMA user_version=3;
                )sql");
            });
            version = 3;
        }
        else if (version == 2)
        {
            Transaction([&]
            {
                Execute(R"sql(
                    ALTER TABLE sessions ADD COLUMN account_id TEXT NOT NULL DEFAULT 'current';
                    ALTER TABLE daily_usage ADD COLUMN account_id TEXT NOT NULL DEFAULT 'current';
                    ALTER TABLE hourly_usage ADD COLUMN account_id TEXT NOT NULL DEFAULT 'current';
                    CREATE INDEX IF NOT EXISTS daily_usage_account_idx ON daily_usage(account_id, day);
                    INSERT OR IGNORE INTO schema_migrations(version, applied_at)
                        VALUES(3, CAST(strftime('%s','now') AS INTEGER));
                    PRAGMA user_version=3;
                )sql");
            });
            version = 3;
        }

        if (version == 3)
        {
            Transaction([&]
            {
                Execute(R"sql(
                    ALTER TABLE hourly_usage RENAME TO hourly_usage_v3;
                    CREATE TABLE hourly_usage(
                        source_path TEXT NOT NULL,
                        hour_start INTEGER NOT NULL,
                        day TEXT NOT NULL,
                        session_id TEXT NOT NULL,
                        source_kind TEXT NOT NULL,
                        account_id TEXT NOT NULL DEFAULT 'current',
                        tool TEXT NOT NULL,
                        model TEXT NOT NULL DEFAULT '',
                        project TEXT NOT NULL DEFAULT '',
                        device_id TEXT NOT NULL DEFAULT '',
                        input_tokens INTEGER NOT NULL DEFAULT 0,
                        cached_input_tokens INTEGER NOT NULL DEFAULT 0,
                        cache_write_input_tokens INTEGER NOT NULL DEFAULT 0,
                        output_tokens INTEGER NOT NULL DEFAULT 0,
                        reasoning_output_tokens INTEGER NOT NULL DEFAULT 0,
                        reported_total_tokens INTEGER NOT NULL DEFAULT 0,
                        messages INTEGER NOT NULL DEFAULT 0,
                        tool_calls INTEGER NOT NULL DEFAULT 0,
                        PRIMARY KEY(source_path, hour_start, session_id, source_kind, account_id, tool, model, project, device_id)
                    );
                    INSERT INTO hourly_usage(
                        source_path, hour_start, day, session_id, source_kind, account_id, tool,
                        model, project, device_id, input_tokens, cached_input_tokens,
                        cache_write_input_tokens, output_tokens, reasoning_output_tokens,
                        reported_total_tokens, messages, tool_calls)
                    SELECT source_path, hour_start, day, session_id, source_kind, account_id, tool,
                           model, project, device_id, input_tokens, cached_input_tokens,
                           cache_write_input_tokens, output_tokens, reasoning_output_tokens,
                           reported_total_tokens, messages, tool_calls
                    FROM hourly_usage_v3;
                    DROP TABLE hourly_usage_v3;
                    CREATE INDEX hourly_usage_time_idx ON hourly_usage(hour_start);

                    ALTER TABLE daily_usage RENAME TO daily_usage_v3;
                    CREATE TABLE daily_usage(
                        source_path TEXT NOT NULL,
                        day TEXT NOT NULL,
                        session_id TEXT NOT NULL,
                        source_kind TEXT NOT NULL,
                        account_id TEXT NOT NULL DEFAULT 'current',
                        tool TEXT NOT NULL,
                        model TEXT NOT NULL DEFAULT '',
                        project TEXT NOT NULL DEFAULT '',
                        device_id TEXT NOT NULL DEFAULT '',
                        first_timestamp INTEGER NOT NULL,
                        last_timestamp INTEGER NOT NULL,
                        input_tokens INTEGER NOT NULL DEFAULT 0,
                        cached_input_tokens INTEGER NOT NULL DEFAULT 0,
                        cache_write_input_tokens INTEGER NOT NULL DEFAULT 0,
                        output_tokens INTEGER NOT NULL DEFAULT 0,
                        reasoning_output_tokens INTEGER NOT NULL DEFAULT 0,
                        reported_total_tokens INTEGER NOT NULL DEFAULT 0,
                        messages INTEGER NOT NULL DEFAULT 0,
                        tool_calls INTEGER NOT NULL DEFAULT 0,
                        PRIMARY KEY(source_path, day, session_id, source_kind, account_id, tool, model, project, device_id)
                    );
                    INSERT INTO daily_usage(
                        source_path, day, session_id, source_kind, account_id, tool, model,
                        project, device_id, first_timestamp, last_timestamp, input_tokens,
                        cached_input_tokens, cache_write_input_tokens, output_tokens,
                        reasoning_output_tokens, reported_total_tokens, messages, tool_calls)
                    SELECT source_path, day, session_id, source_kind, account_id, tool, model,
                           project, device_id, first_timestamp, last_timestamp, input_tokens,
                           cached_input_tokens, cache_write_input_tokens, output_tokens,
                           reasoning_output_tokens, reported_total_tokens, messages, tool_calls
                    FROM daily_usage_v3;
                    DROP TABLE daily_usage_v3;
                    CREATE INDEX daily_usage_day_idx ON daily_usage(day);
                    CREATE INDEX daily_usage_model_idx ON daily_usage(model, day);
                    CREATE INDEX daily_usage_project_idx ON daily_usage(project, day);
                    CREATE INDEX daily_usage_account_idx ON daily_usage(account_id, day);
                    INSERT OR IGNORE INTO schema_migrations(version, applied_at)
                        VALUES(4, CAST(strftime('%s','now') AS INTEGER));
                    PRAGMA user_version=4;
                )sql");
            });
            version = 4;
        }

        if (version == 4)
        {
            Transaction([&]
            {
                Execute(R"sql(
                    ALTER TABLE hourly_usage RENAME TO hourly_usage_v4;
                    CREATE TABLE hourly_usage(
                        source_path TEXT NOT NULL,
                        hour_start INTEGER NOT NULL,
                        day TEXT NOT NULL,
                        session_id TEXT NOT NULL,
                        source_kind TEXT NOT NULL,
                        account_id TEXT NOT NULL DEFAULT 'current',
                        tool TEXT NOT NULL,
                        model TEXT NOT NULL DEFAULT '',
                        project TEXT NOT NULL DEFAULT '',
                        device_id TEXT NOT NULL DEFAULT '',
                        input_tokens INTEGER NOT NULL DEFAULT 0,
                        cached_input_tokens INTEGER NOT NULL DEFAULT 0,
                        cache_write_input_tokens INTEGER NOT NULL DEFAULT 0,
                        output_tokens INTEGER NOT NULL DEFAULT 0,
                        reasoning_output_tokens INTEGER NOT NULL DEFAULT 0,
                        reported_total_tokens INTEGER NOT NULL DEFAULT 0,
                        messages INTEGER NOT NULL DEFAULT 0,
                        tool_calls INTEGER NOT NULL DEFAULT 0,
                        PRIMARY KEY(source_path, hour_start, day, session_id, source_kind, account_id, tool, model, project, device_id)
                    );
                    INSERT INTO hourly_usage(
                        source_path, hour_start, day, session_id, source_kind, account_id, tool,
                        model, project, device_id, input_tokens, cached_input_tokens,
                        cache_write_input_tokens, output_tokens, reasoning_output_tokens,
                        reported_total_tokens, messages, tool_calls)
                    SELECT source_path, hour_start, day, session_id, source_kind, account_id, tool,
                           model, project, device_id, input_tokens, cached_input_tokens,
                           cache_write_input_tokens, output_tokens, reasoning_output_tokens,
                           reported_total_tokens, messages, tool_calls
                    FROM hourly_usage_v4;
                    DROP TABLE hourly_usage_v4;
                    CREATE INDEX hourly_usage_time_idx ON hourly_usage(hour_start);
                    CREATE INDEX hourly_usage_day_idx ON hourly_usage(day, hour_start);
                    INSERT OR IGNORE INTO schema_migrations(version, applied_at)
                        VALUES(5, CAST(strftime('%s','now') AS INTEGER));
                    PRAGMA user_version=5;
                )sql");
            });
            version = 5;
        }

        if (version == 5)
        {
            Transaction([&]
            {
                Execute(R"sql(
                    CREATE TABLE chatgpt_import_sources(
                        account_id TEXT NOT NULL,
                        source_path TEXT NOT NULL,
                        source_hash TEXT NOT NULL,
                        modified_at INTEGER NOT NULL,
                        size INTEGER NOT NULL,
                        imported_at INTEGER NOT NULL,
                        PRIMARY KEY(account_id, source_path)
                    );
                    CREATE TABLE chatgpt_estimated_sessions(
                        account_id TEXT NOT NULL,
                        session_id TEXT NOT NULL,
                        source_path TEXT NOT NULL,
                        source_hash TEXT NOT NULL,
                        source_kind TEXT NOT NULL DEFAULT 'chatgpt-export',
                        measurement_kind TEXT NOT NULL DEFAULT 'estimated' CHECK(measurement_kind='estimated'),
                        model TEXT NOT NULL DEFAULT '',
                        started_at INTEGER NOT NULL DEFAULT 0,
                        updated_at INTEGER NOT NULL DEFAULT 0,
                        messages INTEGER NOT NULL DEFAULT 0,
                        prompts INTEGER NOT NULL DEFAULT 0,
                        estimated_input_tokens INTEGER NOT NULL DEFAULT 0,
                        estimated_output_tokens INTEGER NOT NULL DEFAULT 0,
                        PRIMARY KEY(account_id, session_id),
                        FOREIGN KEY(account_id, source_path)
                            REFERENCES chatgpt_import_sources(account_id, source_path) ON DELETE CASCADE
                    );
                    CREATE INDEX chatgpt_estimated_sessions_updated_idx
                        ON chatgpt_estimated_sessions(account_id, updated_at DESC);
                    CREATE TABLE chatgpt_estimated_prompts(
                        account_id TEXT NOT NULL,
                        session_id TEXT NOT NULL,
                        prompt_index INTEGER NOT NULL,
                        turn_id TEXT NOT NULL DEFAULT '',
                        measurement_kind TEXT NOT NULL DEFAULT 'estimated' CHECK(measurement_kind='estimated'),
                        timestamp INTEGER NOT NULL DEFAULT 0,
                        day TEXT NOT NULL DEFAULT 'unknown',
                        model TEXT NOT NULL DEFAULT '',
                        messages INTEGER NOT NULL DEFAULT 0,
                        estimated_input_tokens INTEGER NOT NULL DEFAULT 0,
                        estimated_output_tokens INTEGER NOT NULL DEFAULT 0,
                        PRIMARY KEY(account_id, session_id, prompt_index),
                        FOREIGN KEY(account_id, session_id)
                            REFERENCES chatgpt_estimated_sessions(account_id, session_id) ON DELETE CASCADE
                    );
                    CREATE INDEX chatgpt_estimated_prompts_time_idx
                        ON chatgpt_estimated_prompts(timestamp);
                    CREATE TABLE chatgpt_estimated_daily(
                        account_id TEXT NOT NULL,
                        session_id TEXT NOT NULL,
                        day TEXT NOT NULL,
                        source_kind TEXT NOT NULL DEFAULT 'chatgpt-export',
                        measurement_kind TEXT NOT NULL DEFAULT 'estimated' CHECK(measurement_kind='estimated'),
                        model TEXT NOT NULL DEFAULT '',
                        estimated_input_tokens INTEGER NOT NULL DEFAULT 0,
                        estimated_output_tokens INTEGER NOT NULL DEFAULT 0,
                        messages INTEGER NOT NULL DEFAULT 0,
                        prompts INTEGER NOT NULL DEFAULT 0,
                        PRIMARY KEY(account_id, session_id, day, model),
                        FOREIGN KEY(account_id, session_id)
                            REFERENCES chatgpt_estimated_sessions(account_id, session_id) ON DELETE CASCADE
                    );
                    CREATE INDEX chatgpt_estimated_daily_day_idx
                        ON chatgpt_estimated_daily(day, account_id);
                    INSERT OR IGNORE INTO schema_migrations(version, applied_at)
                        VALUES(6, CAST(strftime('%s','now') AS INTEGER));
                    PRAGMA user_version=6;
                )sql");
            });
        }
    }

    std::wstring Database::GetOrCreateDeviceId(
        std::wstring_view displayName,
        std::wstring_view stateKey)
    {
        if (stateKey.empty() || stateKey.size() > 512)
        {
            throw std::invalid_argument("Device state key is invalid");
        }
        std::scoped_lock lock(m_mutex);
        Statement existing(m_database, "SELECT value FROM app_state WHERE key=?1;");
        existing.Bind(1, stateKey);
        std::wstring id;
        if (existing.Step())
        {
            id = existing.Text(0);
        }
        if (id.empty())
        {
            GUID guid{};
            if (FAILED(CoCreateGuid(&guid)))
            {
                throw std::runtime_error("Could not create a device identifier");
            }
            wchar_t text[40]{};
            StringFromGUID2(guid, text, 40);
            id = text;
            Statement save(m_database, R"sql(
                INSERT INTO app_state(key, value) VALUES(?1, ?2)
                ON CONFLICT(key) DO UPDATE SET value=excluded.value;
            )sql");
            save.Bind(1, stateKey);
            save.Bind(2, id);
            save.Step();
        }

        Statement device(m_database, R"sql(
            INSERT INTO devices(id, display_name, last_seen) VALUES(?1,?2,?3)
            ON CONFLICT(id) DO UPDATE SET display_name=excluded.display_name,
                                         last_seen=excluded.last_seen;
        )sql");
        device.Bind(1, id);
        device.Bind(2, displayName);
        device.Bind(3, UnixNow());
        device.Step();
        return id;
    }

    std::optional<std::wstring> Database::GetAppState(std::wstring_view key)
    {
        if (key.empty() || key.size() > 512)
        {
            throw std::invalid_argument("Application state key is invalid");
        }
        std::scoped_lock lock(m_mutex);
        Statement statement(m_database, "SELECT value FROM app_state WHERE key=?1;");
        statement.Bind(1, key);
        if (!statement.Step()) return std::nullopt;
        return statement.Text(0);
    }

    void Database::SetAppState(std::wstring_view key, std::wstring_view value)
    {
        if (key.empty() || key.size() > 512 || value.size() > 16 * 1024)
        {
            throw std::invalid_argument("Application state value is invalid");
        }
        std::scoped_lock lock(m_mutex);
        Statement statement(m_database, R"sql(
            INSERT INTO app_state(key, value) VALUES(?1, ?2)
            ON CONFLICT(key) DO UPDATE SET value=excluded.value;
        )sql");
        statement.Bind(1, key);
        statement.Bind(2, value);
        statement.Step();
    }

    bool Database::HasSessionSource(
        std::wstring_view sessionId,
        std::wstring_view sourceKind)
    {
        std::scoped_lock lock(m_mutex);
        Statement statement(m_database, R"sql(
            SELECT 1 FROM sessions WHERE id=?1 AND source_kind=?2 LIMIT 1;
        )sql");
        statement.Bind(1, sessionId);
        statement.Bind(2, sourceKind);
        return statement.Step();
    }

    void Database::Transaction(std::function<void()> const& work)
    {
        std::scoped_lock lock(m_mutex);
        Execute("BEGIN IMMEDIATE;");
        try
        {
            work();
            Execute("COMMIT;");
        }
        catch (...)
        {
            sqlite3_exec(m_database, "ROLLBACK;", nullptr, nullptr, nullptr);
            throw;
        }
    }

    std::optional<SourceProgress> Database::GetSourceProgress(
        std::wstring_view path,
        std::wstring_view fileIdentity)
    {
        std::scoped_lock lock(m_mutex);
        Statement statement(m_database, R"sql(
            SELECT path, file_identity, size, modified_at, offset, session_id, project, model,
                   turn_id, prompt_index, tracking_started, forked, session_created_at,
                   cumulative_input, cumulative_cached,
                   cumulative_cache_write, cumulative_output, cumulative_reasoning, cumulative_total
            FROM source_files
            WHERE path=?1 OR (?2<>'' AND file_identity=?2)
            ORDER BY CASE WHEN path=?1 THEN 0 ELSE 1 END LIMIT 1;
        )sql");
        statement.Bind(1, path);
        statement.Bind(2, fileIdentity);
        if (!statement.Step())
        {
            return std::nullopt;
        }
        SourceProgress progress;
        progress.path = statement.Text(0);
        progress.fileIdentity = statement.Text(1);
        progress.size = statement.Int64(2);
        progress.modifiedAt = statement.Int64(3);
        progress.offset = statement.Int64(4);
        progress.sessionId = statement.Text(5);
        progress.project = statement.Text(6);
        progress.model = statement.Text(7);
        progress.turnId = statement.Text(8);
        progress.promptIndex = statement.Int(9);
        progress.trackingStarted = statement.Int(10) != 0;
        progress.forked = statement.Int(11) != 0;
        progress.sessionCreatedAt = statement.Int64(12);
        progress.cumulative = ReadCounts(statement, 13);
        return progress;
    }

    void Database::SaveSourceProgress(SourceProgress const& progress)
    {
        std::scoped_lock lock(m_mutex);
        Statement statement(m_database, R"sql(
            INSERT INTO source_files(
                path, file_identity, size, modified_at, offset, session_id, project, model,
                turn_id, prompt_index, tracking_started, forked, session_created_at,
                cumulative_input, cumulative_cached,
                cumulative_cache_write, cumulative_output, cumulative_reasoning, cumulative_total)
            VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19)
            ON CONFLICT(file_identity) DO UPDATE SET
                path=excluded.path, size=excluded.size,
                modified_at=excluded.modified_at, offset=excluded.offset,
                session_id=excluded.session_id, project=excluded.project, model=excluded.model,
                turn_id=excluded.turn_id, prompt_index=excluded.prompt_index,
                tracking_started=excluded.tracking_started, forked=excluded.forked,
                session_created_at=excluded.session_created_at,
                cumulative_input=excluded.cumulative_input,
                cumulative_cached=excluded.cumulative_cached,
                cumulative_cache_write=excluded.cumulative_cache_write,
                cumulative_output=excluded.cumulative_output,
                cumulative_reasoning=excluded.cumulative_reasoning,
                cumulative_total=excluded.cumulative_total;
        )sql");
        statement.Bind(1, progress.path);
        statement.Bind(2, progress.fileIdentity);
        statement.Bind(3, progress.size);
        statement.Bind(4, progress.modifiedAt);
        statement.Bind(5, progress.offset);
        statement.Bind(6, progress.sessionId);
        statement.Bind(7, progress.project);
        statement.Bind(8, progress.model);
        statement.Bind(9, progress.turnId);
        statement.Bind(10, progress.promptIndex);
        statement.Bind(11, progress.trackingStarted ? 1 : 0);
        statement.Bind(12, progress.forked ? 1 : 0);
        statement.Bind(13, progress.sessionCreatedAt);
        BindCounts(statement, 14, progress.cumulative);
        statement.Step();
    }

    void Database::ResetSourceFile(std::wstring_view path)
    {
        std::scoped_lock lock(m_mutex);
        std::wstring sessionId;
        {
            Statement source(m_database, R"sql(
                SELECT session_id FROM source_files WHERE path=?1
                UNION ALL
                SELECT id FROM sessions WHERE source_path=?1
                LIMIT 1;
            )sql");
            source.Bind(1, path);
            if (source.Step()) sessionId = source.Text(0);
        }
        if (!sessionId.empty())
        {
            for (auto const* sql : {
                     "DELETE FROM turn_usage WHERE session_id=?1;",
                     "DELETE FROM turn_tools WHERE session_id=?1;",
                     "DELETE FROM usage_events WHERE session_id=?1;",
                     "DELETE FROM prompt_events WHERE session_id=?1;",
                     "DELETE FROM tool_events WHERE session_id=?1;",
                     "DELETE FROM hourly_usage WHERE session_id=?1;",
                     "DELETE FROM daily_usage WHERE session_id=?1;",
                     "DELETE FROM ingested_records WHERE session_id=?1;",
                     "DELETE FROM sessions WHERE id=?1;",
                     "DELETE FROM source_files WHERE session_id=?1;" })
            {
                Statement statement(m_database, sql);
                statement.Bind(1, sessionId);
                statement.Step();
            }
        }
        else
        {
            Statement statement(m_database, "DELETE FROM source_files WHERE path=?1;");
            statement.Bind(1, path);
            statement.Step();
        }
    }

    void Database::UpsertSession(SessionRecord const& session)
    {
        if (session.id.empty())
        {
            return;
        }
        std::scoped_lock lock(m_mutex);
        bool promoteFromWsl{};
        std::wstring promotedAccount = session.accountId;
        std::wstring promotedDevice = session.deviceId;
        std::wstring previousProject;
        std::wstring previousModel;
        if (session.sourceKind == L"codex")
        {
            Statement existing(m_database, R"sql(
                SELECT source_kind, account_id, device_id, project, model FROM sessions WHERE id=?1;
            )sql");
            existing.Bind(1, session.id);
            if (existing.Step() && existing.Text(0) == L"codex-wsl")
            {
                promoteFromWsl = true;
                if (promotedAccount.empty()) promotedAccount = existing.Text(1);
                if (promotedDevice.empty()) promotedDevice = existing.Text(2);
                previousProject = existing.Text(3);
                previousModel = existing.Text(4);
            }
        }

        if (promoteFromWsl) Execute("SAVEPOINT promote_codex_session;");
        try
        {
            if (promoteFromWsl)
            {
                Statement daily(m_database, R"sql(
                    INSERT INTO daily_usage(
                        source_path, day, session_id, source_kind, account_id, tool, model,
                        project, device_id, first_timestamp, last_timestamp, input_tokens,
                        cached_input_tokens, cache_write_input_tokens, output_tokens,
                        reasoning_output_tokens, reported_total_tokens, messages, tool_calls)
                    SELECT ?2, day, session_id, 'codex', ?3, tool,
                           CASE WHEN ?5<>'' AND model=?7 THEN ?5 ELSE model END,
                           CASE WHEN ?6<>'' AND project=?8 THEN ?6 ELSE project END, ?4,
                           first_timestamp, last_timestamp, input_tokens, cached_input_tokens,
                           cache_write_input_tokens, output_tokens, reasoning_output_tokens,
                           reported_total_tokens, messages, tool_calls
                    FROM daily_usage
                    WHERE session_id=?1 AND source_kind='codex-wsl'
                    ON CONFLICT(source_path, day, session_id, source_kind, account_id, tool, model, project, device_id)
                    DO UPDATE SET
                        first_timestamp=MIN(daily_usage.first_timestamp, excluded.first_timestamp),
                        last_timestamp=MAX(daily_usage.last_timestamp, excluded.last_timestamp),
                        input_tokens=daily_usage.input_tokens+excluded.input_tokens,
                        cached_input_tokens=daily_usage.cached_input_tokens+excluded.cached_input_tokens,
                        cache_write_input_tokens=daily_usage.cache_write_input_tokens+excluded.cache_write_input_tokens,
                        output_tokens=daily_usage.output_tokens+excluded.output_tokens,
                        reasoning_output_tokens=daily_usage.reasoning_output_tokens+excluded.reasoning_output_tokens,
                        reported_total_tokens=daily_usage.reported_total_tokens+excluded.reported_total_tokens,
                        messages=daily_usage.messages+excluded.messages,
                        tool_calls=daily_usage.tool_calls+excluded.tool_calls;
                )sql");
                daily.Bind(1, session.id);
                daily.Bind(2, session.sourcePath);
                daily.Bind(3, promotedAccount);
                daily.Bind(4, promotedDevice);
                daily.Bind(5, session.model);
                daily.Bind(6, session.project);
                daily.Bind(7, previousModel);
                daily.Bind(8, previousProject);
                daily.Step();

                Statement hourly(m_database, R"sql(
                    INSERT INTO hourly_usage(
                        source_path, hour_start, day, session_id, source_kind, account_id, tool,
                        model, project, device_id, input_tokens, cached_input_tokens,
                        cache_write_input_tokens, output_tokens, reasoning_output_tokens,
                        reported_total_tokens, messages, tool_calls)
                    SELECT ?2, hour_start, day, session_id, 'codex', ?3, tool,
                           CASE WHEN ?5<>'' AND model=?7 THEN ?5 ELSE model END,
                           CASE WHEN ?6<>'' AND project=?8 THEN ?6 ELSE project END, ?4,
                           input_tokens, cached_input_tokens, cache_write_input_tokens,
                           output_tokens, reasoning_output_tokens, reported_total_tokens,
                           messages, tool_calls
                    FROM hourly_usage
                    WHERE session_id=?1 AND source_kind='codex-wsl'
                    ON CONFLICT(source_path, hour_start, day, session_id, source_kind, account_id, tool, model, project, device_id)
                    DO UPDATE SET
                        input_tokens=hourly_usage.input_tokens+excluded.input_tokens,
                        cached_input_tokens=hourly_usage.cached_input_tokens+excluded.cached_input_tokens,
                        cache_write_input_tokens=hourly_usage.cache_write_input_tokens+excluded.cache_write_input_tokens,
                        output_tokens=hourly_usage.output_tokens+excluded.output_tokens,
                        reasoning_output_tokens=hourly_usage.reasoning_output_tokens+excluded.reasoning_output_tokens,
                        reported_total_tokens=hourly_usage.reported_total_tokens+excluded.reported_total_tokens,
                        messages=hourly_usage.messages+excluded.messages,
                        tool_calls=hourly_usage.tool_calls+excluded.tool_calls;
                )sql");
                hourly.Bind(1, session.id);
                hourly.Bind(2, session.sourcePath);
                hourly.Bind(3, promotedAccount);
                hourly.Bind(4, promotedDevice);
                hourly.Bind(5, session.model);
                hourly.Bind(6, session.project);
                hourly.Bind(7, previousModel);
                hourly.Bind(8, previousProject);
                hourly.Step();

                for (auto const* sql : {
                         "DELETE FROM daily_usage WHERE session_id=?1 AND source_kind='codex-wsl';",
                         "DELETE FROM hourly_usage WHERE session_id=?1 AND source_kind='codex-wsl';" })
                {
                    Statement removeWsl(m_database, sql);
                    removeWsl.Bind(1, session.id);
                    removeWsl.Step();
                }
                Statement promoteUsageDetails(m_database, R"sql(
                    UPDATE usage_events
                    SET source_path=?2,
                        model=CASE WHEN ?3<>'' AND model=?4 THEN ?3 ELSE model END
                    WHERE session_id=?1;
                )sql");
                promoteUsageDetails.Bind(1, session.id);
                promoteUsageDetails.Bind(2, session.sourcePath);
                promoteUsageDetails.Bind(3, session.model);
                promoteUsageDetails.Bind(4, previousModel);
                promoteUsageDetails.Step();
                for (auto const* sql : {
                         "UPDATE prompt_events SET source_path=?2 WHERE session_id=?1;",
                         "UPDATE tool_events SET source_path=?2 WHERE session_id=?1;" })
                {
                    Statement promoteDetails(m_database, sql);
                    promoteDetails.Bind(1, session.id);
                    promoteDetails.Bind(2, session.sourcePath);
                    promoteDetails.Step();
                }
                Statement promoteTurns(m_database, R"sql(
                    UPDATE turn_usage SET model=?2
                    WHERE session_id=?1 AND ?2<>'' AND model=?3;
                )sql");
                promoteTurns.Bind(1, session.id);
                promoteTurns.Bind(2, session.model);
                promoteTurns.Bind(3, previousModel);
                promoteTurns.Step();
            }

            Statement statement(m_database, R"sql(
                INSERT INTO sessions(
                    id, source_path, source_kind, account_id, title, project, model, device_id,
                    started_at, updated_at, message_count)
                VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)
                ON CONFLICT(id) DO UPDATE SET
                    source_path=CASE
                        WHEN sessions.source_kind='codex' AND excluded.source_kind='codex-wsl'
                        THEN sessions.source_path ELSE excluded.source_path END,
                    source_kind=CASE
                        WHEN sessions.source_kind='codex' OR excluded.source_kind='codex'
                        THEN 'codex' ELSE excluded.source_kind END,
                    account_id=CASE WHEN excluded.account_id='' THEN sessions.account_id ELSE excluded.account_id END,
                    title=CASE WHEN excluded.title='' THEN sessions.title ELSE excluded.title END,
                    project=CASE WHEN excluded.project='' THEN sessions.project ELSE excluded.project END,
                    model=CASE WHEN excluded.model='' THEN sessions.model ELSE excluded.model END,
                    device_id=CASE
                        WHEN sessions.source_kind='codex' AND excluded.source_kind='codex-wsl'
                        THEN sessions.device_id
                        WHEN excluded.device_id='' THEN sessions.device_id ELSE excluded.device_id END,
                    started_at=CASE WHEN sessions.started_at=0 THEN excluded.started_at
                                    WHEN excluded.started_at=0 THEN sessions.started_at
                                    ELSE MIN(sessions.started_at, excluded.started_at) END,
                    updated_at=MAX(sessions.updated_at, excluded.updated_at),
                    message_count=MAX(sessions.message_count, excluded.message_count);
            )sql");
            statement.Bind(1, session.id);
            statement.Bind(2, session.sourcePath);
            statement.Bind(3, session.sourceKind);
            statement.Bind(4, session.accountId);
            statement.Bind(5, session.title);
            statement.Bind(6, session.project);
            statement.Bind(7, session.model);
            statement.Bind(8, session.deviceId);
            statement.Bind(9, session.startedAt);
            statement.Bind(10, session.updatedAt);
            statement.Bind(11, session.messageCount);
            statement.Step();
            if (promoteFromWsl) Execute("RELEASE promote_codex_session;");
        }
        catch (...)
        {
            if (promoteFromWsl)
            {
                sqlite3_exec(m_database, "ROLLBACK TO promote_codex_session;", nullptr, nullptr, nullptr);
                sqlite3_exec(m_database, "RELEASE promote_codex_session;", nullptr, nullptr, nullptr);
            }
            throw;
        }
    }

    bool Database::InsertUsageEvent(UsageEvent const& event)
    {
        std::scoped_lock lock(m_mutex);
        Statement claim(m_database, R"sql(
            INSERT OR IGNORE INTO ingested_records(session_id, source_offset, record_kind)
            VALUES(?1,?2,1);
        )sql");
        claim.Bind(1, event.sessionId);
        claim.Bind(2, event.sourceOffset);
        claim.Step();
        if (sqlite3_changes(m_database) == 0)
        {
            return false;
        }
        Statement eventStatement(m_database, R"sql(
            INSERT INTO usage_events(
                source_path, source_offset, session_id, turn_id, prompt_index, timestamp, model,
                input_tokens, cached_input_tokens, cache_write_input_tokens, output_tokens,
                reasoning_output_tokens, reported_total_tokens)
            VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13);
        )sql");
        eventStatement.Bind(1, event.sourcePath);
        eventStatement.Bind(2, event.sourceOffset);
        eventStatement.Bind(3, event.sessionId);
        eventStatement.Bind(4, event.turnId);
        eventStatement.Bind(5, event.promptIndex);
        eventStatement.Bind(6, event.timestamp);
        eventStatement.Bind(7, event.model);
        BindCounts(eventStatement, 8, event.counts);
        eventStatement.Step();
        Statement turnStatement(m_database, R"sql(
            INSERT INTO turn_usage(
                session_id, turn_id, prompt_index, model, first_timestamp, last_timestamp,
                input_tokens, cached_input_tokens, cache_write_input_tokens, output_tokens,
                reasoning_output_tokens, reported_total_tokens)
            VALUES(?1,?2,?3,?4,?5,?5,?6,?7,?8,?9,?10,?11)
            ON CONFLICT(session_id, turn_id, prompt_index) DO UPDATE SET
                model=CASE WHEN excluded.model='' THEN turn_usage.model ELSE excluded.model END,
                first_timestamp=MIN(first_timestamp, excluded.first_timestamp),
                last_timestamp=MAX(last_timestamp, excluded.last_timestamp),
                input_tokens=input_tokens+excluded.input_tokens,
                cached_input_tokens=cached_input_tokens+excluded.cached_input_tokens,
                cache_write_input_tokens=cache_write_input_tokens+excluded.cache_write_input_tokens,
                output_tokens=output_tokens+excluded.output_tokens,
                reasoning_output_tokens=reasoning_output_tokens+excluded.reasoning_output_tokens,
                reported_total_tokens=reported_total_tokens+excluded.reported_total_tokens;
        )sql");
        turnStatement.Bind(1, event.sessionId);
        turnStatement.Bind(2, event.turnId);
        turnStatement.Bind(3, event.promptIndex);
        turnStatement.Bind(4, event.model);
        turnStatement.Bind(5, event.timestamp);
        BindCounts(turnStatement, 6, event.counts);
        turnStatement.Step();

        Statement hourlyStatement(m_database, R"sql(
            INSERT INTO hourly_usage(
                source_path, hour_start, day, session_id, source_kind, account_id, tool, model,
                project, device_id, input_tokens, cached_input_tokens, cache_write_input_tokens,
                output_tokens, reasoning_output_tokens, reported_total_tokens)
            VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16)
            ON CONFLICT(source_path, hour_start, day, session_id, source_kind, account_id, tool, model, project, device_id)
            DO UPDATE SET
                account_id=excluded.account_id,
                input_tokens=input_tokens+excluded.input_tokens,
                cached_input_tokens=cached_input_tokens+excluded.cached_input_tokens,
                cache_write_input_tokens=cache_write_input_tokens+excluded.cache_write_input_tokens,
                output_tokens=output_tokens+excluded.output_tokens,
                reasoning_output_tokens=reasoning_output_tokens+excluded.reasoning_output_tokens,
                reported_total_tokens=reported_total_tokens+excluded.reported_total_tokens;
        )sql");
        hourlyStatement.Bind(1, event.sourcePath);
        hourlyStatement.Bind(2, event.timestamp > 0 ? event.timestamp - event.timestamp % 3600 : 0);
        hourlyStatement.Bind(3, event.day);
        hourlyStatement.Bind(4, event.sessionId);
        hourlyStatement.Bind(5, event.sourceKind);
        hourlyStatement.Bind(6, event.accountId);
        hourlyStatement.Bind(7, event.tool);
        hourlyStatement.Bind(8, event.model);
        hourlyStatement.Bind(9, event.project);
        hourlyStatement.Bind(10, event.deviceId);
        BindCounts(hourlyStatement, 11, event.counts);
        hourlyStatement.Step();

        Statement dailyStatement(m_database, R"sql(
            INSERT INTO daily_usage(
                source_path, day, session_id, source_kind, account_id, tool, model, project, device_id,
                first_timestamp, last_timestamp, input_tokens, cached_input_tokens,
                cache_write_input_tokens, output_tokens, reasoning_output_tokens,
                reported_total_tokens, messages, tool_calls)
            VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?10,?11,?12,?13,?14,?15,?16,0,0)
            ON CONFLICT(source_path, day, session_id, source_kind, account_id, tool, model, project, device_id)
            DO UPDATE SET
                account_id=excluded.account_id,
                first_timestamp=MIN(first_timestamp, excluded.first_timestamp),
                last_timestamp=MAX(last_timestamp, excluded.last_timestamp),
                input_tokens=input_tokens+excluded.input_tokens,
                cached_input_tokens=cached_input_tokens+excluded.cached_input_tokens,
                cache_write_input_tokens=cache_write_input_tokens+excluded.cache_write_input_tokens,
                output_tokens=output_tokens+excluded.output_tokens,
                reasoning_output_tokens=reasoning_output_tokens+excluded.reasoning_output_tokens,
                reported_total_tokens=reported_total_tokens+excluded.reported_total_tokens;
        )sql");
        dailyStatement.Bind(1, event.sourcePath);
        dailyStatement.Bind(2, event.day);
        dailyStatement.Bind(3, event.sessionId);
        dailyStatement.Bind(4, event.sourceKind);
        dailyStatement.Bind(5, event.accountId);
        dailyStatement.Bind(6, event.tool);
        dailyStatement.Bind(7, event.model);
        dailyStatement.Bind(8, event.project);
        dailyStatement.Bind(9, event.deviceId);
        dailyStatement.Bind(10, event.timestamp);
        BindCounts(dailyStatement, 11, event.counts);
        dailyStatement.Step();
        return true;
    }

    bool Database::InsertToolEvent(ToolEvent const& event)
    {
        std::scoped_lock lock(m_mutex);
        Statement claim(m_database, R"sql(
            INSERT OR IGNORE INTO ingested_records(session_id, source_offset, record_kind)
            VALUES(?1,?2,3);
        )sql");
        claim.Bind(1, event.sessionId);
        claim.Bind(2, event.sourceOffset);
        claim.Step();
        if (sqlite3_changes(m_database) == 0)
        {
            return false;
        }
        Statement eventStatement(m_database, R"sql(
            INSERT INTO tool_events(
                source_path, source_offset, session_id, turn_id, prompt_index, timestamp, name,
                call_id, input_length)
            VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);
        )sql");
        eventStatement.Bind(1, event.sourcePath);
        eventStatement.Bind(2, event.sourceOffset);
        eventStatement.Bind(3, event.sessionId);
        eventStatement.Bind(4, event.turnId);
        eventStatement.Bind(5, event.promptIndex);
        eventStatement.Bind(6, event.timestamp);
        eventStatement.Bind(7, event.name);
        eventStatement.Bind(8, event.callId);
        eventStatement.Bind(9, event.inputLength);
        eventStatement.Step();
        Statement turnStatement(m_database, R"sql(
            INSERT INTO turn_tools(
                session_id, turn_id, prompt_index, name, first_timestamp, last_timestamp, call_count)
            VALUES(?1,?2,?3,?4,?5,?5,1)
            ON CONFLICT(session_id, turn_id, prompt_index, name) DO UPDATE SET
                first_timestamp=MIN(first_timestamp, excluded.first_timestamp),
                last_timestamp=MAX(last_timestamp, excluded.last_timestamp),
                call_count=call_count+1;
        )sql");
        turnStatement.Bind(1, event.sessionId);
        turnStatement.Bind(2, event.turnId);
        turnStatement.Bind(3, event.promptIndex);
        turnStatement.Bind(4, event.name);
        turnStatement.Bind(5, event.timestamp);
        turnStatement.Step();

        Statement hourlyStatement(m_database, R"sql(
            INSERT INTO hourly_usage(
                source_path, hour_start, day, session_id, source_kind, account_id, tool, model,
                project, device_id, tool_calls)
            VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,1)
            ON CONFLICT(source_path, hour_start, day, session_id, source_kind, account_id, tool, model, project, device_id)
            DO UPDATE SET account_id=excluded.account_id, tool_calls=tool_calls+1;
        )sql");
        hourlyStatement.Bind(1, event.sourcePath);
        hourlyStatement.Bind(2, event.timestamp > 0 ? event.timestamp - event.timestamp % 3600 : 0);
        hourlyStatement.Bind(3, event.day);
        hourlyStatement.Bind(4, event.sessionId);
        hourlyStatement.Bind(5, event.sourceKind);
        hourlyStatement.Bind(6, event.accountId);
        hourlyStatement.Bind(7, event.tool);
        hourlyStatement.Bind(8, event.model);
        hourlyStatement.Bind(9, event.project);
        hourlyStatement.Bind(10, event.deviceId);
        hourlyStatement.Step();

        Statement dailyStatement(m_database, R"sql(
            INSERT INTO daily_usage(
                source_path, day, session_id, source_kind, account_id, tool, model, project, device_id,
                first_timestamp, last_timestamp, tool_calls)
            VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?10,1)
            ON CONFLICT(source_path, day, session_id, source_kind, account_id, tool, model, project, device_id)
            DO UPDATE SET account_id=excluded.account_id,
                          last_timestamp=MAX(last_timestamp, excluded.last_timestamp),
                          tool_calls=tool_calls+1;
        )sql");
        dailyStatement.Bind(1, event.sourcePath);
        dailyStatement.Bind(2, event.day);
        dailyStatement.Bind(3, event.sessionId);
        dailyStatement.Bind(4, event.sourceKind);
        dailyStatement.Bind(5, event.accountId);
        dailyStatement.Bind(6, event.tool);
        dailyStatement.Bind(7, event.model);
        dailyStatement.Bind(8, event.project);
        dailyStatement.Bind(9, event.deviceId);
        dailyStatement.Bind(10, event.timestamp);
        dailyStatement.Step();
        return true;
    }

    bool Database::AttachToolOutput(ToolOutputEvent const& event)
    {
        if (event.callId.empty())
        {
            return false;
        }
        std::scoped_lock lock(m_mutex);
        Statement statement(m_database, R"sql(
            UPDATE tool_events
            SET output_offset=?3, output_length=?4,
                source_path=CASE WHEN ?5='' THEN source_path ELSE ?5 END
            WHERE session_id=?1 AND call_id=?2;
        )sql");
        statement.Bind(1, event.sessionId);
        statement.Bind(2, event.callId);
        statement.Bind(3, event.outputOffset);
        statement.Bind(4, event.outputLength);
        statement.Bind(5, event.sourcePath);
        statement.Step();
        return sqlite3_changes(m_database) > 0;
    }

    bool Database::InsertPromptEvent(PromptEvent const& event)
    {
        std::scoped_lock lock(m_mutex);
        Statement claim(m_database, R"sql(
            INSERT OR IGNORE INTO ingested_records(session_id, source_offset, record_kind)
            VALUES(?1,?2,2);
        )sql");
        claim.Bind(1, event.sessionId);
        claim.Bind(2, event.sourceOffset);
        claim.Step();
        if (sqlite3_changes(m_database) == 0)
        {
            return false;
        }
        Statement eventStatement(m_database, R"sql(
            INSERT INTO prompt_events(
                source_path, source_offset, session_id, turn_id, prompt_index, timestamp)
            VALUES(?1,?2,?3,?4,?5,?6);
        )sql");
        eventStatement.Bind(1, event.sourcePath);
        eventStatement.Bind(2, event.sourceOffset);
        eventStatement.Bind(3, event.sessionId);
        eventStatement.Bind(4, event.turnId);
        eventStatement.Bind(5, event.promptIndex);
        eventStatement.Bind(6, event.timestamp);
        eventStatement.Step();
        Statement turnStatement(m_database, R"sql(
            INSERT INTO turn_usage(
                session_id, turn_id, prompt_index, model, first_timestamp, last_timestamp)
            VALUES(?1,?2,?3,?4,?5,?5)
            ON CONFLICT(session_id, turn_id, prompt_index) DO UPDATE SET
                model=CASE WHEN excluded.model='' THEN turn_usage.model ELSE excluded.model END,
                first_timestamp=MIN(first_timestamp, excluded.first_timestamp),
                last_timestamp=MAX(last_timestamp, excluded.last_timestamp);
        )sql");
        turnStatement.Bind(1, event.sessionId);
        turnStatement.Bind(2, event.turnId);
        turnStatement.Bind(3, event.promptIndex);
        turnStatement.Bind(4, event.model);
        turnStatement.Bind(5, event.timestamp);
        turnStatement.Step();

        Statement hourlyStatement(m_database, R"sql(
            INSERT INTO hourly_usage(
                source_path, hour_start, day, session_id, source_kind, account_id, tool, model,
                project, device_id, messages)
            VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,1)
            ON CONFLICT(source_path, hour_start, day, session_id, source_kind, account_id, tool, model, project, device_id)
            DO UPDATE SET account_id=excluded.account_id, messages=messages+1;
        )sql");
        hourlyStatement.Bind(1, event.sourcePath);
        hourlyStatement.Bind(2, event.timestamp > 0 ? event.timestamp - event.timestamp % 3600 : 0);
        hourlyStatement.Bind(3, event.day);
        hourlyStatement.Bind(4, event.sessionId);
        hourlyStatement.Bind(5, event.sourceKind);
        hourlyStatement.Bind(6, event.accountId);
        hourlyStatement.Bind(7, event.tool);
        hourlyStatement.Bind(8, event.model);
        hourlyStatement.Bind(9, event.project);
        hourlyStatement.Bind(10, event.deviceId);
        hourlyStatement.Step();

        Statement dailyStatement(m_database, R"sql(
            INSERT INTO daily_usage(
                source_path, day, session_id, source_kind, account_id, tool, model, project, device_id,
                first_timestamp, last_timestamp, messages)
            VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?10,1)
            ON CONFLICT(source_path, day, session_id, source_kind, account_id, tool, model, project, device_id)
            DO UPDATE SET account_id=excluded.account_id,
                          last_timestamp=MAX(last_timestamp, excluded.last_timestamp),
                          messages=messages+1;
        )sql");
        dailyStatement.Bind(1, event.sourcePath);
        dailyStatement.Bind(2, event.day);
        dailyStatement.Bind(3, event.sessionId);
        dailyStatement.Bind(4, event.sourceKind);
        dailyStatement.Bind(5, event.accountId);
        dailyStatement.Bind(6, event.tool);
        dailyStatement.Bind(7, event.model);
        dailyStatement.Bind(8, event.project);
        dailyStatement.Bind(9, event.deviceId);
        dailyStatement.Bind(10, event.timestamp);
        dailyStatement.Step();

        Statement sessionStatement(m_database, R"sql(
            UPDATE sessions SET message_count=MAX(message_count, ?2), updated_at=MAX(updated_at, ?3)
            WHERE id=?1;
        )sql");
        sessionStatement.Bind(1, event.sessionId);
        sessionStatement.Bind(2, event.promptIndex);
        sessionStatement.Bind(3, event.timestamp);
        sessionStatement.Step();
        return true;
    }

    void Database::UpsertRateLimit(RateLimitSnapshot const& snapshot)
    {
        std::scoped_lock lock(m_mutex);
        Statement statement(m_database, R"sql(
            INSERT INTO rate_limits(
                provider, account_id, limit_id, limit_name, primary_used_percent,
                primary_window_minutes, primary_resets_at, secondary_used_percent,
                secondary_window_minutes, secondary_resets_at, plan_type, captured_at)
            VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)
            ON CONFLICT(provider, account_id) DO UPDATE SET
                limit_id=excluded.limit_id, limit_name=excluded.limit_name,
                primary_used_percent=excluded.primary_used_percent,
                primary_window_minutes=excluded.primary_window_minutes,
                primary_resets_at=excluded.primary_resets_at,
                secondary_used_percent=excluded.secondary_used_percent,
                secondary_window_minutes=excluded.secondary_window_minutes,
                secondary_resets_at=excluded.secondary_resets_at,
                plan_type=excluded.plan_type, captured_at=excluded.captured_at;
        )sql");
        statement.Bind(1, snapshot.provider);
        statement.Bind(2, snapshot.accountId);
        statement.Bind(3, snapshot.limitId);
        statement.Bind(4, snapshot.limitName);
        statement.Bind(5, snapshot.primaryUsedPercent);
        statement.Bind(6, snapshot.primaryWindowMinutes);
        statement.Bind(7, snapshot.primaryResetsAt);
        statement.Bind(8, snapshot.secondaryUsedPercent);
        statement.Bind(9, snapshot.secondaryWindowMinutes);
        statement.Bind(10, snapshot.secondaryResetsAt);
        statement.Bind(11, snapshot.planType);
        statement.Bind(12, snapshot.capturedAt);
        statement.Step();
    }

    bool Database::IsChatGPTExportCurrent(ChatGPTExportBatch const& batch)
    {
        std::scoped_lock lock(m_mutex);
        Statement statement(m_database, R"sql(
            SELECT 1 FROM chatgpt_import_sources
            WHERE account_id=?1 AND source_path=?2 AND source_hash=?3
              AND modified_at=?4 AND size=?5;
        )sql");
        statement.Bind(1, batch.accountId);
        statement.Bind(2, batch.sourcePath);
        statement.Bind(3, batch.sourceHash);
        statement.Bind(4, batch.sourceModifiedAt);
        statement.Bind(5, batch.sourceSize);
        return statement.Step();
    }

    void Database::ReplaceChatGPTExport(
        ChatGPTExportBatch const& batch,
        std::stop_token stopToken)
    {
        ThrowIfCancelled(stopToken);
        if (batch.accountId.empty() || batch.sourcePath.empty() || batch.sourceHash.empty())
        {
            throw std::invalid_argument("ChatGPT export metadata is incomplete");
        }

        std::unordered_map<std::wstring, std::vector<ChatGPTPromptEstimate const*>> promptsBySession;
        for (auto const& prompt : batch.prompts)
        {
            ThrowIfCancelled(stopToken);
            promptsBySession[prompt.sessionId].push_back(&prompt);
        }

        ThrowIfCancelled(stopToken);
        Transaction([&]
        {
            Statement insertSource(m_database, R"sql(
                INSERT INTO chatgpt_import_sources(
                    account_id, source_path, source_hash, modified_at, size, imported_at)
                VALUES(?1,?2,'',?3,?4,?5)
                ON CONFLICT(account_id, source_path) DO UPDATE SET
                    source_hash='', modified_at=excluded.modified_at,
                    size=excluded.size, imported_at=excluded.imported_at;
            )sql");
            insertSource.Bind(1, batch.accountId);
            insertSource.Bind(2, batch.sourcePath);
            insertSource.Bind(3, batch.sourceModifiedAt);
            insertSource.Bind(4, batch.sourceSize);
            insertSource.Bind(5, UnixNow());
            insertSource.Step();
        });

        for (auto const& session : batch.sessions)
        {
            ThrowIfCancelled(stopToken);
            Transaction([&]
            {
                ThrowIfCancelled(stopToken);
            Statement deleteSession(m_database, R"sql(
                DELETE FROM chatgpt_estimated_sessions WHERE account_id=?1 AND session_id=?2;
            )sql");
                deleteSession.Bind(1, batch.accountId);
                deleteSession.Bind(2, session.id);
                deleteSession.Step();

                Statement insertSession(m_database, R"sql(
                INSERT INTO chatgpt_estimated_sessions(
                    account_id, session_id, source_path, source_hash,
                    source_kind, measurement_kind, model,
                    started_at, updated_at, messages, prompts,
                    estimated_input_tokens, estimated_output_tokens)
                VALUES(?1,?2,?3,?4,'chatgpt-export','estimated',?5,?6,?7,?8,?9,?10,?11);
            )sql");
                insertSession.Bind(1, batch.accountId);
                insertSession.Bind(2, session.id);
                insertSession.Bind(3, batch.sourcePath);
                insertSession.Bind(4, batch.sourceHash);
                insertSession.Bind(5, session.model);
                insertSession.Bind(6, session.startedAt);
                insertSession.Bind(7, session.updatedAt);
                insertSession.Bind(8, session.messages);
                insertSession.Bind(9, session.prompts);
                insertSession.Bind(10, session.estimatedInputTokens);
                insertSession.Bind(11, session.estimatedOutputTokens);
                insertSession.Step();

                Statement insertPrompt(m_database, R"sql(
                INSERT INTO chatgpt_estimated_prompts(
                    account_id, session_id, prompt_index, turn_id, measurement_kind,
                    timestamp, day, model, messages,
                    estimated_input_tokens, estimated_output_tokens)
                VALUES(?1,?2,?3,?4,'estimated',?5,?6,?7,?8,?9,?10);
                )sql");
                auto const prompts = promptsBySession.find(session.id);
                if (prompts != promptsBySession.end())
                {
                    for (auto const* prompt : prompts->second)
                    {
                        ThrowIfCancelled(stopToken);
                        insertPrompt.Bind(1, batch.accountId);
                        insertPrompt.Bind(2, prompt->sessionId);
                        insertPrompt.Bind(3, prompt->promptIndex);
                        insertPrompt.Bind(4, prompt->turnId);
                        insertPrompt.Bind(5, prompt->timestamp);
                        insertPrompt.Bind(6, prompt->day);
                        insertPrompt.Bind(7, prompt->model);
                        insertPrompt.Bind(8, prompt->messages);
                        insertPrompt.Bind(9, prompt->estimatedInputTokens);
                        insertPrompt.Bind(10, prompt->estimatedOutputTokens);
                        insertPrompt.Step();
                        insertPrompt.Reset();
                    }
                }

                ThrowIfCancelled(stopToken);
                Statement daily(m_database, R"sql(
                INSERT INTO chatgpt_estimated_daily(
                    account_id, session_id, day, source_kind, measurement_kind, model,
                    estimated_input_tokens, estimated_output_tokens, messages, prompts)
                SELECT p.account_id, p.session_id, p.day, 'chatgpt-export', 'estimated', p.model,
                       SUM(p.estimated_input_tokens), SUM(p.estimated_output_tokens),
                       SUM(p.messages), COUNT(*)
                FROM chatgpt_estimated_prompts p
                WHERE p.account_id=?1 AND p.session_id=?2
                GROUP BY p.account_id, p.session_id, p.day, p.model;
            )sql");
                daily.Bind(1, batch.accountId);
                daily.Bind(2, session.id);
                daily.Step();
            });
        }

        ThrowIfCancelled(stopToken);
        Transaction([&]
        {
            ThrowIfCancelled(stopToken);
            Statement deleteStale(m_database, R"sql(
                DELETE FROM chatgpt_estimated_sessions
                WHERE account_id=?1 AND source_path=?2 AND source_hash<>?3;
            )sql");
            deleteStale.Bind(1, batch.accountId);
            deleteStale.Bind(2, batch.sourcePath);
            deleteStale.Bind(3, batch.sourceHash);
            deleteStale.Step();

            Statement completeSource(m_database, R"sql(
                UPDATE chatgpt_import_sources
                SET source_hash=?3, modified_at=?4, size=?5, imported_at=?6
                WHERE account_id=?1 AND source_path=?2;
            )sql");
            completeSource.Bind(1, batch.accountId);
            completeSource.Bind(2, batch.sourcePath);
            completeSource.Bind(3, batch.sourceHash);
            completeSource.Bind(4, batch.sourceModifiedAt);
            completeSource.Bind(5, batch.sourceSize);
            completeSource.Bind(6, UnixNow());
            completeSource.Step();

            Statement pruneSources(m_database, R"sql(
                DELETE FROM chatgpt_import_sources
                WHERE account_id=?1 AND source_path<>?2
                  AND NOT EXISTS(
                      SELECT 1 FROM chatgpt_estimated_sessions s
                      WHERE s.account_id=chatgpt_import_sources.account_id
                        AND s.source_path=chatgpt_import_sources.source_path);
            )sql");
            pruneSources.Bind(1, batch.accountId);
            pruneSources.Bind(2, batch.sourcePath);
            pruneSources.Step();
        });
    }

    UsageTotals Database::GetTotals(int64_t since)
    {
        std::scoped_lock lock(m_mutex);
        Statement statement(m_database, R"sql(
            SELECT COALESCE(SUM(input_tokens),0), COALESCE(SUM(cached_input_tokens),0),
                   COALESCE(SUM(cache_write_input_tokens),0), COALESCE(SUM(output_tokens),0),
                   COALESCE(SUM(reasoning_output_tokens),0), COALESCE(SUM(reported_total_tokens),0),
                   COALESCE(SUM(messages),0), COALESCE(SUM(tool_calls),0),
                   COUNT(DISTINCT CASE WHEN reported_total_tokens>0 OR messages>0 THEN day END),
                   COUNT(DISTINCT session_id)
            FROM daily_usage WHERE (?1=0 OR last_timestamp>=?1);
        )sql");
        statement.Bind(1, since);
        statement.Step();
        UsageTotals result;
        result.counts = ReadCounts(statement, 0);
        result.messages = statement.Int64(6);
        result.toolCalls = statement.Int64(7);
        result.activeDays = statement.Int64(8);
        result.sessions = statement.Int64(9);
        Statement estimated(m_database, R"sql(
            SELECT COALESCE(SUM(estimated_input_tokens + estimated_output_tokens),0),
                   COUNT(DISTINCT account_id || char(31) || session_id)
            FROM chatgpt_estimated_prompts WHERE (?1=0 OR timestamp>=?1);
        )sql");
        estimated.Bind(1, since);
        estimated.Step();
        result.estimatedTokens = estimated.Int64(0);
        result.estimatedSessions = estimated.Int64(1);
        return result;
    }

    std::vector<DailyUsage> Database::GetDailyUsage(int days)
    {
        std::scoped_lock lock(m_mutex);
        std::string sql = R"sql(
            SELECT day, source_kind, tool, model, project, device_id, account_id,
                   SUM(input_tokens), SUM(cached_input_tokens), SUM(cache_write_input_tokens),
                   SUM(output_tokens), SUM(reasoning_output_tokens), SUM(reported_total_tokens),
                   SUM(messages), SUM(tool_calls)
            FROM daily_usage )sql";
        if (days > 0)
        {
            sql += "WHERE day>=?1 AND day<=?2 ";
        }
        sql += R"sql(
            GROUP BY day, source_kind, tool, model, project, device_id, account_id
            ORDER BY day ASC;
        )sql";
        Statement statement(m_database, sql.c_str());
        if (days > 0)
        {
            statement.Bind(1, LocalCalendarDay(days - 1));
            statement.Bind(2, LocalCalendarDay());
        }
        std::vector<DailyUsage> result;
        while (statement.Step())
        {
            DailyUsage row;
            row.day = statement.Text(0);
            row.sourceKind = statement.Text(1);
            row.tool = statement.Text(2);
            row.model = statement.Text(3);
            row.project = statement.Text(4);
            row.deviceId = statement.Text(5);
            row.accountId = statement.Text(6);
            row.counts = ReadCounts(statement, 7);
            row.messages = statement.Int64(13);
            row.toolCalls = statement.Int64(14);
            result.push_back(std::move(row));
        }
        return result;
    }

    std::vector<HourlyUsage> Database::GetHourlyUsage(int days)
    {
        std::scoped_lock lock(m_mutex);
        std::string sql = R"sql(
            SELECT hour_start, day, source_kind, tool, model, project, device_id, account_id,
                   SUM(input_tokens), SUM(cached_input_tokens), SUM(cache_write_input_tokens),
                   SUM(output_tokens), SUM(reasoning_output_tokens), SUM(reported_total_tokens),
                   SUM(messages), SUM(tool_calls)
            FROM hourly_usage )sql";
        if (days > 0)
        {
            sql += "WHERE day>=?1 AND day<=?2 ";
        }
        sql += R"sql(
            GROUP BY hour_start, day, source_kind, tool, model, project, device_id, account_id
            ORDER BY hour_start ASC;
        )sql";
        Statement statement(m_database, sql.c_str());
        if (days > 0)
        {
            statement.Bind(1, LocalCalendarDay(days - 1));
            statement.Bind(2, LocalCalendarDay());
        }
        std::vector<HourlyUsage> result;
        while (statement.Step())
        {
            HourlyUsage row;
            row.hourStart = statement.Int64(0);
            row.day = statement.Text(1);
            row.sourceKind = statement.Text(2);
            row.tool = statement.Text(3);
            row.model = statement.Text(4);
            row.project = statement.Text(5);
            row.deviceId = statement.Text(6);
            row.accountId = statement.Text(7);
            row.counts = ReadCounts(statement, 8);
            row.messages = statement.Int64(14);
            row.toolCalls = statement.Int64(15);
            result.push_back(std::move(row));
        }
        return result;
    }

    std::vector<BreakdownRow> Database::GetBreakdown(
        std::wstring_view dimension,
        int64_t since,
        int limit)
    {
        std::scoped_lock lock(m_mutex);
        char const* key = "tool";
        if (dimension == L"model") key = "model";
        else if (dimension == L"project") key = "project";
        else if (dimension == L"device") key = "device_id";
        else if (dimension == L"session") key = "session_id";
        else if (dimension == L"source") key = "source_kind";
        else if (dimension == L"account") key = "account_id";

        std::string sql = "SELECT ";
        sql += key;
        sql += R"sql(,
                   SUM(input_tokens), SUM(cached_input_tokens), SUM(cache_write_input_tokens),
                   SUM(output_tokens), SUM(reasoning_output_tokens), SUM(reported_total_tokens),
                   COUNT(DISTINCT session_id), SUM(messages), SUM(tool_calls)
                FROM daily_usage WHERE (?1=0 OR last_timestamp>=?1)
                GROUP BY )sql";
        sql += key;
        sql += " ORDER BY SUM(reported_total_tokens) DESC LIMIT ?2;";

        Statement statement(m_database, sql.c_str());
        statement.Bind(1, since);
        statement.Bind(2, std::clamp(limit, 1, 200));
        std::vector<BreakdownRow> result;
        while (statement.Step())
        {
            BreakdownRow row;
            row.key = statement.Text(0);
            row.counts = ReadCounts(statement, 1);
            row.sessions = statement.Int64(7);
            row.messages = statement.Int64(8);
            row.toolCalls = statement.Int64(9);
            result.push_back(std::move(row));
        }
        return result;
    }

    std::vector<SessionSummary> Database::GetRecentSessions(int limit)
    {
        std::scoped_lock lock(m_mutex);
        Statement statement(m_database, R"sql(
            SELECT s.id, COALESCE(NULLIF(s.title,''), s.id), s.project, s.model, s.device_id,
                   s.account_id, s.started_at, s.updated_at,
                   MAX(COALESCE(SUM(d.messages),0), s.message_count),
                   COALESCE(SUM(d.tool_calls),0), COALESCE(SUM(d.input_tokens),0),
                   COALESCE(SUM(d.cached_input_tokens),0), COALESCE(SUM(d.cache_write_input_tokens),0),
                   COALESCE(SUM(d.output_tokens),0), COALESCE(SUM(d.reasoning_output_tokens),0),
                    COALESCE(SUM(d.reported_total_tokens),0), s.source_kind
            FROM sessions s LEFT JOIN daily_usage d
              ON d.session_id=s.id AND d.account_id=s.account_id
            GROUP BY s.id
            ORDER BY s.updated_at DESC LIMIT ?1;
        )sql");
        statement.Bind(1, std::clamp(limit, 1, 200));
        std::vector<SessionSummary> result;
        while (statement.Step())
        {
            SessionSummary row;
            row.id = statement.Text(0);
            row.title = statement.Text(1);
            row.project = statement.Text(2);
            row.model = statement.Text(3);
            row.deviceId = statement.Text(4);
            row.accountId = statement.Text(5);
            row.startedAt = statement.Int64(6);
            row.updatedAt = statement.Int64(7);
            row.messages = statement.Int64(8);
            row.toolCalls = statement.Int64(9);
            row.counts = ReadCounts(statement, 10);
            row.sourceKind = statement.Text(16);
            result.push_back(std::move(row));
        }
        return result;
    }

    std::vector<TurnSummary> Database::GetSessionTurns(std::wstring_view sessionId, int limit)
    {
        std::scoped_lock lock(m_mutex);
        Statement statement(m_database, R"sql(
            SELECT u.session_id, u.turn_id, u.prompt_index, u.first_timestamp, u.model,
                   COALESCE((SELECT GROUP_CONCAT(DISTINCT t.name)
                             FROM turn_tools t
                             WHERE t.session_id=u.session_id AND t.turn_id=u.turn_id
                               AND t.prompt_index=u.prompt_index),''),
                   u.input_tokens, u.cached_input_tokens, u.cache_write_input_tokens,
                   u.output_tokens, u.reasoning_output_tokens, u.reported_total_tokens
            FROM turn_usage u WHERE u.session_id=?1
            ORDER BY u.first_timestamp DESC LIMIT ?2;
        )sql");
        statement.Bind(1, sessionId);
        statement.Bind(2, std::clamp(limit, 1, 500));
        std::vector<TurnSummary> result;
        while (statement.Step())
        {
            TurnSummary row;
            row.sessionId = statement.Text(0);
            row.turnId = statement.Text(1);
            row.promptIndex = statement.Int(2);
            row.timestamp = statement.Int64(3);
            row.model = statement.Text(4);
            row.tools = statement.Text(5);
            row.counts = ReadCounts(statement, 6);
            result.push_back(std::move(row));
        }
        return result;
    }

    std::vector<ToolCallDetail> Database::GetToolCalls(
        std::wstring_view sessionId,
        int promptIndex)
    {
        std::scoped_lock lock(m_mutex);
        Statement statement(m_database, R"sql(
            SELECT COALESCE(NULLIF(s.source_path,''), t.source_path),
                   t.name, t.call_id, t.source_offset, t.input_length,
                   t.output_offset, t.output_length
            FROM tool_events t LEFT JOIN sessions s ON s.id=t.session_id
            WHERE t.session_id=?1 AND t.prompt_index=?2
            ORDER BY t.timestamp, t.source_offset;
        )sql");
        statement.Bind(1, sessionId);
        statement.Bind(2, promptIndex);
        std::vector<ToolCallDetail> result;
        while (statement.Step())
        {
            result.push_back({
                statement.Text(0), statement.Text(1), statement.Text(2), statement.Int64(3),
                statement.Int64(4), statement.Int64(5), statement.Int64(6)
            });
        }
        return result;
    }

    std::optional<RateLimitSnapshot> Database::GetLatestRateLimit(
        std::wstring_view provider,
        std::wstring_view accountId)
    {
        std::scoped_lock lock(m_mutex);
        Statement statement(m_database, R"sql(
            SELECT provider, account_id, limit_id, limit_name, primary_used_percent,
                   primary_window_minutes, primary_resets_at, secondary_used_percent,
                   secondary_window_minutes, secondary_resets_at, plan_type, captured_at
            FROM rate_limits WHERE provider=?1 AND account_id=?2;
        )sql");
        statement.Bind(1, provider);
        statement.Bind(2, accountId);
        if (!statement.Step())
        {
            return std::nullopt;
        }
        RateLimitSnapshot result;
        result.provider = statement.Text(0);
        result.accountId = statement.Text(1);
        result.limitId = statement.Text(2);
        result.limitName = statement.Text(3);
        result.primaryUsedPercent = statement.Double(4);
        result.primaryWindowMinutes = statement.Int(5);
        result.primaryResetsAt = statement.Int64(6);
        result.secondaryUsedPercent = statement.Double(7);
        result.secondaryWindowMinutes = statement.Int(8);
        result.secondaryResetsAt = statement.Int64(9);
        result.planType = statement.Text(10);
        result.capturedAt = statement.Int64(11);
        return result;
    }

    std::vector<ChatGPTSessionEstimate> Database::GetChatGPTEstimatedSessions(
        std::wstring_view accountId,
        int limit)
    {
        std::scoped_lock lock(m_mutex);
        Statement statement(m_database, R"sql(
            SELECT session_id, source_kind, account_id, model, started_at, updated_at,
                   messages, prompts, estimated_input_tokens, estimated_output_tokens
            FROM chatgpt_estimated_sessions
            WHERE (?1='' OR account_id=?1)
            ORDER BY updated_at DESC, session_id
            LIMIT ?2;
        )sql");
        statement.Bind(1, accountId);
        statement.Bind(2, std::clamp(limit, 1, 500));
        std::vector<ChatGPTSessionEstimate> result;
        while (statement.Step())
        {
            ChatGPTSessionEstimate row;
            row.id = statement.Text(0);
            row.sourceKind = statement.Text(1);
            row.accountId = statement.Text(2);
            row.model = statement.Text(3);
            row.startedAt = statement.Int64(4);
            row.updatedAt = statement.Int64(5);
            row.messages = statement.Int64(6);
            row.prompts = statement.Int64(7);
            row.estimatedInputTokens = statement.Int64(8);
            row.estimatedOutputTokens = statement.Int64(9);
            result.push_back(std::move(row));
        }
        return result;
    }

    UsageTotals Database::GetChatGPTEstimatedTotals(std::wstring_view accountId)
    {
        std::scoped_lock lock(m_mutex);
        Statement statement(m_database, R"sql(
            SELECT COALESCE(SUM(s.estimated_input_tokens),0),
                   COALESCE(SUM(s.estimated_output_tokens),0),
                   COALESCE(SUM(s.messages),0), COUNT(*)
            FROM chatgpt_estimated_sessions s
            WHERE (?1='' OR s.account_id=?1);
        )sql");
        statement.Bind(1, accountId);
        statement.Step();
        UsageTotals result;
        result.counts.input = statement.Int64(0);
        result.counts.output = statement.Int64(1);
        result.counts.reportedTotal = result.counts.input + result.counts.output;
        result.messages = statement.Int64(2);
        result.estimatedTokens = result.counts.reportedTotal;
        result.estimatedSessions = statement.Int64(3);
        return result;
    }

    std::vector<BreakdownRow> Database::GetChatGPTEstimatedBreakdown(
        std::wstring_view dimension,
        int64_t since,
        int limit)
    {
        std::scoped_lock lock(m_mutex);
        char const* key{};
        if (dimension == L"tool") key = "'ChatGPT'";
        else if (dimension == L"model") key = "COALESCE(NULLIF(model,''),'unclassified')";
        else if (dimension == L"session") key = "account_id || ' / ' || session_id";
        else if (dimension == L"account") key = "account_id";
        else return {};

        std::string sql = "SELECT ";
        sql += key;
        sql += R"sql(,
                   COALESCE(SUM(estimated_input_tokens),0),
                   COALESCE(SUM(estimated_output_tokens),0),
                   COALESCE(SUM(estimated_input_tokens + estimated_output_tokens),0),
                   COUNT(DISTINCT account_id || char(31) || session_id),
                   COALESCE(SUM(messages),0)
            FROM chatgpt_estimated_prompts
            WHERE (?1=0 OR timestamp>=?1)
            GROUP BY 1
            ORDER BY 4 DESC
            LIMIT ?2;
        )sql";
        Statement statement(m_database, sql.c_str());
        statement.Bind(1, since);
        statement.Bind(2, std::clamp(limit, 1, 200));
        std::vector<BreakdownRow> result;
        while (statement.Step())
        {
            BreakdownRow row;
            row.key = statement.Text(0);
            row.counts.input = statement.Int64(1);
            row.counts.output = statement.Int64(2);
            row.counts.reportedTotal = statement.Int64(3);
            row.sessions = statement.Int64(4);
            row.messages = statement.Int64(5);
            row.measurement = MeasurementKind::Estimated;
            result.push_back(std::move(row));
        }
        return result;
    }

    std::vector<ChatGPTPromptEstimate> Database::GetChatGPTEstimatedPrompts(
        std::wstring_view accountId,
        std::wstring_view sessionId,
        int limit)
    {
        std::scoped_lock lock(m_mutex);
        Statement statement(m_database, R"sql(
            SELECT session_id, turn_id, prompt_index, timestamp, day, model, messages,
                   estimated_input_tokens, estimated_output_tokens
            FROM chatgpt_estimated_prompts
            WHERE account_id=?1 AND session_id=?2
            ORDER BY prompt_index DESC
            LIMIT ?3;
        )sql");
        statement.Bind(1, accountId);
        statement.Bind(2, sessionId);
        statement.Bind(3, std::clamp(limit, 1, 2000));
        std::vector<ChatGPTPromptEstimate> result;
        while (statement.Step())
        {
            ChatGPTPromptEstimate row;
            row.sessionId = statement.Text(0);
            row.turnId = statement.Text(1);
            row.promptIndex = statement.Int(2);
            row.timestamp = statement.Int64(3);
            row.day = statement.Text(4);
            row.model = statement.Text(5);
            row.messages = statement.Int64(6);
            row.estimatedInputTokens = statement.Int64(7);
            row.estimatedOutputTokens = statement.Int64(8);
            result.push_back(std::move(row));
        }
        return result;
    }

    std::vector<ChatGPTEstimatedDailyUsage> Database::GetChatGPTEstimatedDailyUsage(
        int days,
        std::wstring_view accountId)
    {
        std::scoped_lock lock(m_mutex);
        std::string sql = R"sql(
            SELECT day, source_kind, model, account_id,
                   SUM(estimated_input_tokens), SUM(estimated_output_tokens),
                   SUM(messages), SUM(prompts)
            FROM chatgpt_estimated_daily WHERE 1=1 )sql";
        int bindIndex = 1;
        int accountIndex{};
        int firstDayIndex{};
        int lastDayIndex{};
        if (!accountId.empty())
        {
            accountIndex = bindIndex++;
            sql += "AND account_id=?" + std::to_string(accountIndex) + " ";
        }
        if (days > 0)
        {
            firstDayIndex = bindIndex++;
            lastDayIndex = bindIndex++;
            sql += "AND day>=?" + std::to_string(firstDayIndex) +
                   " AND day<=?" + std::to_string(lastDayIndex) + " ";
        }
        sql += R"sql(
            GROUP BY day, source_kind, model, account_id
            ORDER BY day ASC;
        )sql";

        Statement statement(m_database, sql.c_str());
        if (accountIndex) statement.Bind(accountIndex, accountId);
        if (firstDayIndex)
        {
            statement.Bind(firstDayIndex, LocalCalendarDay(days - 1));
            statement.Bind(lastDayIndex, LocalCalendarDay());
        }
        std::vector<ChatGPTEstimatedDailyUsage> result;
        while (statement.Step())
        {
            ChatGPTEstimatedDailyUsage row;
            row.day = statement.Text(0);
            row.sourceKind = statement.Text(1);
            row.model = statement.Text(2);
            row.accountId = statement.Text(3);
            row.estimatedInputTokens = statement.Int64(4);
            row.estimatedOutputTokens = statement.Int64(5);
            row.messages = statement.Int64(6);
            row.prompts = statement.Int64(7);
            result.push_back(std::move(row));
        }
        return result;
    }

    std::vector<ChatGPTEstimatedHourlyUsage> Database::GetChatGPTEstimatedHourlyUsage(
        int days,
        std::wstring_view accountId)
    {
        std::scoped_lock lock(m_mutex);
        std::string sql = R"sql(
            SELECT (timestamp / 3600) * 3600, day, model, account_id,
                   SUM(estimated_input_tokens), SUM(estimated_output_tokens),
                   SUM(messages), COUNT(*)
            FROM chatgpt_estimated_prompts
            WHERE timestamp>0 )sql";
        int bindIndex = 1;
        int accountIndex{};
        int firstDayIndex{};
        int lastDayIndex{};
        if (!accountId.empty())
        {
            accountIndex = bindIndex++;
            sql += "AND account_id=?" + std::to_string(accountIndex) + " ";
        }
        if (days > 0)
        {
            firstDayIndex = bindIndex++;
            lastDayIndex = bindIndex++;
            sql += "AND day>=?" + std::to_string(firstDayIndex) +
                   " AND day<=?" + std::to_string(lastDayIndex) + " ";
        }
        sql += R"sql(
            GROUP BY 1, day, model, account_id
            ORDER BY 1 ASC;
        )sql";

        Statement statement(m_database, sql.c_str());
        if (accountIndex) statement.Bind(accountIndex, accountId);
        if (firstDayIndex)
        {
            statement.Bind(firstDayIndex, LocalCalendarDay(days - 1));
            statement.Bind(lastDayIndex, LocalCalendarDay());
        }
        std::vector<ChatGPTEstimatedHourlyUsage> result;
        while (statement.Step())
        {
            ChatGPTEstimatedHourlyUsage row;
            row.hourStart = statement.Int64(0);
            row.day = statement.Text(1);
            row.model = statement.Text(2);
            row.accountId = statement.Text(3);
            row.estimatedInputTokens = statement.Int64(4);
            row.estimatedOutputTokens = statement.Int64(5);
            row.messages = statement.Int64(6);
            row.prompts = statement.Int64(7);
            result.push_back(std::move(row));
        }
        return result;
    }

    std::vector<DeviceSummary> Database::GetDeviceSummaries(int limit)
    {
        std::scoped_lock lock(m_mutex);
        Statement statement(m_database, R"sql(
            SELECT d.id, d.display_name, d.last_seen,
                   EXISTS(SELECT 1 FROM app_state a
                          WHERE a.value=d.id AND a.key LIKE 'wsl_device_id:%'),
                   COALESCE(SUM(u.input_tokens),0),
                   COALESCE(SUM(u.cached_input_tokens),0),
                   COALESCE(SUM(u.cache_write_input_tokens),0),
                   COALESCE(SUM(u.output_tokens),0),
                   COALESCE(SUM(u.reasoning_output_tokens),0),
                   COALESCE(SUM(u.reported_total_tokens),0),
                   COUNT(DISTINCT u.account_id || char(31) || u.session_id)
            FROM devices d
            LEFT JOIN daily_usage u ON u.device_id=d.id
            GROUP BY d.id, d.display_name, d.last_seen
            ORDER BY 4 ASC, d.last_seen DESC
            LIMIT ?1;
        )sql");
        statement.Bind(1, std::clamp(limit, 1, 100));
        std::vector<DeviceSummary> result;
        while (statement.Step())
        {
            DeviceSummary row;
            row.id = statement.Text(0);
            row.displayName = statement.Text(1);
            row.lastSeen = statement.Int64(2);
            row.kind = statement.Int(3) != 0 ? DeviceKind::Wsl : DeviceKind::Windows;
            row.counts = ReadCounts(statement, 4);
            row.sessions = statement.Int64(10);
            result.push_back(std::move(row));
        }
        return result;
    }

    void Database::PruneDetails(int usageDays, int toolDays, int hourlyDays)
    {
        std::scoped_lock lock(m_mutex);
        int64_t const usageCutoff = UnixNow() - static_cast<int64_t>(std::max(usageDays, 1)) * 86400;
        int64_t const toolCutoff = UnixNow() - static_cast<int64_t>(std::max(toolDays, 1)) * 86400;
        int64_t const hourlyCutoff = UnixNow() - static_cast<int64_t>(std::max(hourlyDays, 1)) * 86400;
        for (auto const& item : {
                 std::pair{ "DELETE FROM usage_events WHERE timestamp<?1;", usageCutoff },
                 std::pair{ "DELETE FROM prompt_events WHERE timestamp<?1;", usageCutoff },
                 std::pair{ "DELETE FROM tool_events WHERE timestamp<?1;", toolCutoff },
                 std::pair{ "DELETE FROM hourly_usage WHERE hour_start<?1;", hourlyCutoff } })
        {
            Statement statement(m_database, item.first);
            statement.Bind(1, item.second);
            statement.Step();
        }
    }

    bool Database::PruneDetailsIfDue(int usageDays, int toolDays, int hourlyDays)
    {
        std::scoped_lock lock(m_mutex);
        int64_t const now = UnixNow();
        int64_t lastRun{};
        {
            Statement statement(m_database, R"sql(
                SELECT CAST(value AS INTEGER)
                FROM app_state WHERE key='last_retention_prune_at';
            )sql");
            if (statement.Step()) lastRun = statement.Int64(0);
        }
        if (lastRun > 0 && now >= lastRun && now - lastRun < 86400)
        {
            return false;
        }

        Transaction([&]
        {
            PruneDetails(usageDays, toolDays, hourlyDays);
            Statement statement(m_database, R"sql(
                INSERT INTO app_state(key, value)
                VALUES('last_retention_prune_at', ?1)
                ON CONFLICT(key) DO UPDATE SET value=excluded.value;
            )sql");
            statement.Bind(1, now);
            statement.Step();
        });
        return true;
    }

    void Database::Optimize()
    {
        std::scoped_lock lock(m_mutex);
        Execute("PRAGMA optimize;");
        if (m_path != L":memory:")
        {
            Execute("PRAGMA wal_checkpoint(PASSIVE);");
            Execute("PRAGMA incremental_vacuum(256);");
        }
    }

    bool Database::SelfTest()
    {
        try
        {
            Database database(L":memory:");
            database.Initialize();
            auto const deviceId = database.GetOrCreateDeviceId(L"Test device");
            if (deviceId.empty() || database.GetOrCreateDeviceId(L"Renamed device") != deviceId) return false;
            auto const wslDeviceId = database.GetOrCreateDeviceId(
                L"Test device · WSL · Ubuntu",
                L"wsl_device_id:test-device:Ubuntu");
            if (wslDeviceId.empty() || wslDeviceId == deviceId ||
                database.GetOrCreateDeviceId(
                    L"Renamed WSL device",
                    L"wsl_device_id:test-device:Ubuntu") != wslDeviceId)
            {
                return false;
            }
            SessionRecord session{ L"session-1", L"fixture.jsonl", L"codex", L"Fixture", L"D:\\work", L"gpt-test", deviceId, 100, 100, 0 };
            session.accountId = L"account-a";
            database.UpsertSession(session);
            if (!database.HasSessionSource(session.id, L"codex") ||
                database.HasSessionSource(session.id, L"codex-wsl")) return false;

            PromptEvent prompt{ L"fixture.jsonl", 10, session.id, L"codex", L"Codex", L"",
                                session.project, session.deviceId, L"turn-1", 1, 100, L"1970-01-01" };
            prompt.accountId = session.accountId;
            if (!database.InsertPromptEvent(prompt) || database.InsertPromptEvent(prompt)) return false;

            UsageEvent usage{ L"fixture.jsonl", 20, session.id, L"codex", L"Codex", session.model,
                              session.project, session.deviceId, L"turn-1", 1, 101, L"1970-01-01",
                              { 1200, 900, 0, 300, 80, 1500 } };
            usage.accountId = session.accountId;
            if (!database.InsertUsageEvent(usage) || database.InsertUsageEvent(usage)) return false;

            ToolEvent tool{ L"fixture.jsonl", 30, session.id, L"codex", L"Codex", session.model,
                            session.project, session.deviceId, L"turn-1", 1, 102, L"1970-01-01",
                            L"shell_command", L"call-1", 80 };
            tool.accountId = session.accountId;
            if (!database.InsertToolEvent(tool) || database.InsertToolEvent(tool)) return false;
            if (!database.AttachToolOutput({ L"fixture.jsonl", session.id, L"call-1", 120, 60 })) return false;

            SourceProgress progress;
            progress.path = L"fixture.jsonl";
            progress.fileIdentity = L"fixture-id";
            progress.size = 500;
            progress.offset = 400;
            progress.sessionId = session.id;
            progress.model = session.model;
            progress.cumulative = usage.counts;
            database.SaveSourceProgress(progress);

            auto const loaded = database.GetSourceProgress(progress.path);
            auto const totals = database.GetTotals();
            auto const sessions = database.GetRecentSessions();
            auto const turns = database.GetSessionTurns(session.id);
            auto const tools = database.GetToolCalls(session.id, 1);
            auto const hours = database.GetHourlyUsage(0);
            int64_t hourlyMessages{};
            int64_t hourlyToolCalls{};
            bool hourlyAccountsValid = !hours.empty();
            for (auto const& hour : hours)
            {
                hourlyMessages += hour.messages;
                hourlyToolCalls += hour.toolCalls;
                hourlyAccountsValid = hourlyAccountsValid && hour.accountId == session.accountId;
            }
            bool const initialValid =
                loaded && loaded->offset == 400 && loaded->cumulative.input == 1200 &&
                totals.counts.reportedTotal == 1500 && totals.messages == 1 && totals.toolCalls == 1 &&
                sessions.size() == 1 && sessions.front().accountId == session.accountId &&
                sessions.front().counts.cachedInput == 900 &&
                turns.size() == 1 && turns.front().model == session.model &&
                turns.front().counts.reportedTotal == 1500 &&
                tools.size() == 1 && tools.front().sourcePath == L"fixture.jsonl" &&
                tools.front().inputLength == 80 && tools.front().outputLength == 60 &&
                hourlyAccountsValid &&
                hourlyMessages == 1 && hourlyToolCalls == 1;
            if (!initialValid) return false;

            database.PruneDetails(1, 1, 1);
            if (!database.GetToolCalls(session.id, 1).empty() ||
                !database.GetHourlyUsage(0).empty() ||
                database.GetSessionTurns(session.id).size() != 1 ||
                database.GetTotals().counts.reportedTotal != 1500)
            {
                return false;
            }

            database.Transaction([&] { database.ResetSourceFile(progress.path); });
            if (database.GetTotals().counts.reportedTotal != 0 ||
                !database.GetSessionTurns(session.id).empty())
            {
                return false;
            }
            database.Transaction([&]
            {
                database.UpsertSession(session);
                if (!database.InsertPromptEvent(prompt)) throw std::runtime_error("prompt rebuild failed");
                if (!database.InsertUsageEvent(usage)) throw std::runtime_error("usage rebuild failed");
                if (!database.InsertToolEvent(tool)) throw std::runtime_error("tool rebuild failed");
                database.SaveSourceProgress(progress);
            });
            SessionRecord second = session;
            second.id = L"session-2";
            second.sourcePath = L"fixture-2.jsonl";
            second.title = L"Second account";
            second.accountId = L"account-b";
            second.deviceId = wslDeviceId;
            PromptEvent secondPrompt = prompt;
            secondPrompt.sourcePath = second.sourcePath;
            secondPrompt.sessionId = second.id;
            secondPrompt.accountId = second.accountId;
            secondPrompt.deviceId = second.deviceId;
            UsageEvent secondUsage = usage;
            secondUsage.sourcePath = second.sourcePath;
            secondUsage.sessionId = second.id;
            secondUsage.accountId = second.accountId;
            secondUsage.deviceId = second.deviceId;
            secondUsage.counts = { 200, 50, 0, 50, 0, 250 };
            ToolEvent secondTool = tool;
            secondTool.sourcePath = second.sourcePath;
            secondTool.sessionId = second.id;
            secondTool.name = L"other_tool";
            secondTool.accountId = second.accountId;
            secondTool.deviceId = second.deviceId;
            database.Transaction([&]
            {
                database.UpsertSession(second);
                if (!database.InsertPromptEvent(secondPrompt)) throw std::runtime_error("second prompt failed");
                if (!database.InsertUsageEvent(secondUsage)) throw std::runtime_error("second usage failed");
                if (!database.InsertToolEvent(secondTool)) throw std::runtime_error("second tool failed");
            });

            auto const accounts = database.GetBreakdown(L"account");
            auto const firstTools = database.GetToolCalls(session.id, 1);
            auto const secondTools = database.GetToolCalls(second.id, 1);
            auto const firstTurns = database.GetSessionTurns(session.id);
            auto findAccount = [&](std::wstring_view id) -> BreakdownRow const*
            {
                auto const found = std::find_if(accounts.begin(), accounts.end(), [&](auto const& row)
                {
                    return row.key == id;
                });
                return found == accounts.end() ? nullptr : &*found;
            };
            auto const accountA = findAccount(session.accountId);
            auto const accountB = findAccount(second.accountId);
            auto const recentSessions = database.GetRecentSessions();
            bool const groupingValid =
                database.GetTotals().counts.reportedTotal == 1750 &&
                accountA && accountA->counts.cachedInput == 900 &&
                accountA->counts.UncachedInput() == 300 &&
                accountB && accountB->counts.reportedTotal == 250 &&
                !database.GetBreakdown(L"tool").empty() &&
                !database.GetBreakdown(L"model").empty() &&
                database.GetBreakdown(L"session").size() == 2 &&
                !database.GetBreakdown(L"device").empty() &&
                !database.GetBreakdown(L"project").empty() &&
                !database.GetBreakdown(L"source").empty() &&
                recentSessions.size() == 2 &&
                recentSessions.front().sourceKind == L"codex" &&
                firstTools.size() == 1 && firstTools.front().name == L"shell_command" &&
                secondTools.size() == 1 && secondTools.front().name == L"other_tool" &&
                firstTurns.size() == 1 && firstTurns.front().tools == L"shell_command";
            if (!groupingValid) return false;

            auto const duplicateWslId = database.GetOrCreateDeviceId(
                L"Renamed WSL device",
                L"wsl_device_id:test-device:Debian");
            auto const devicesBeforeChatGpt = database.GetDeviceSummaries(10);
            auto const findDevice = [&](std::wstring_view id) -> DeviceSummary const*
            {
                auto const found = std::find_if(
                    devicesBeforeChatGpt.begin(), devicesBeforeChatGpt.end(), [&](auto const& item)
                    {
                        return item.id == id;
                    });
                return found == devicesBeforeChatGpt.end() ? nullptr : &*found;
            };
            auto const localDevice = findDevice(deviceId);
            auto const ubuntuDevice = findDevice(wslDeviceId);
            auto const debianDevice = findDevice(duplicateWslId);
            int64_t devicesBeforeChatGptTotal{};
            for (auto const& device : devicesBeforeChatGpt)
            {
                devicesBeforeChatGptTotal += device.counts.DisplayTotal();
            }
            if (duplicateWslId.empty() || duplicateWslId == wslDeviceId ||
                !localDevice || localDevice->kind != DeviceKind::Windows ||
                localDevice->counts.DisplayTotal() != 1500 ||
                !ubuntuDevice || ubuntuDevice->kind != DeviceKind::Wsl ||
                ubuntuDevice->counts.DisplayTotal() != 250 ||
                !debianDevice || debianDevice->kind != DeviceKind::Wsl ||
                ubuntuDevice->displayName != debianDevice->displayName ||
                devicesBeforeChatGptTotal != 1750)
            {
                return false;
            }

            auto importEstimate = [&](std::wstring_view account, std::wstring_view model,
                                      int64_t input, int64_t output, int64_t timestamp)
            {
                ChatGPTExportBatch batch;
                batch.sourcePath = std::wstring{ L"export-" } + std::wstring{ account } + L".json";
                batch.sourceHash = std::wstring{ L"hash-" } + std::wstring{ account };
                batch.sourceModifiedAt = timestamp;
                batch.sourceSize = 100;
                batch.accountId = account;

                ChatGPTSessionEstimate estimate;
                estimate.id = L"shared-chat-session";
                estimate.accountId = account;
                estimate.model = model;
                estimate.startedAt = timestamp;
                estimate.updatedAt = timestamp + 3600;
                estimate.messages = 2;
                estimate.prompts = 2;
                estimate.estimatedInputTokens = input;
                estimate.estimatedOutputTokens = output;
                batch.sessions.push_back(estimate);

                ChatGPTPromptEstimate first;
                first.sessionId = estimate.id;
                first.turnId = L"turn-1";
                first.promptIndex = 0;
                first.timestamp = timestamp;
                first.day = L"1970-01-01";
                first.model = model;
                first.messages = 1;
                first.estimatedInputTokens = input / 2;
                first.estimatedOutputTokens = output / 2;
                auto secondPrompt = first;
                secondPrompt.turnId = L"turn-2";
                secondPrompt.promptIndex = 1;
                secondPrompt.timestamp += 3600;
                secondPrompt.estimatedInputTokens = input - first.estimatedInputTokens;
                secondPrompt.estimatedOutputTokens = output - first.estimatedOutputTokens;
                batch.prompts = { first, secondPrompt };
                database.ReplaceChatGPTExport(batch);
            };
            importEstimate(L"chat-a", L"gpt-a", 1000, 200, 3600);
            importEstimate(L"chat-b", L"gpt-b", 600, 100, 7200);

            auto const allChatGpt = database.GetChatGPTEstimatedTotals();
            auto const chatA = database.GetChatGPTEstimatedTotals(L"chat-a");
            auto const chatSessions = database.GetChatGPTEstimatedSessions({}, 10);
            auto const chatSessionBreakdown = database.GetChatGPTEstimatedBreakdown(L"session");
            auto const chatModels = database.GetChatGPTEstimatedBreakdown(L"model");
            auto const chatAccounts = database.GetChatGPTEstimatedBreakdown(L"account");
            auto const chatTools = database.GetChatGPTEstimatedBreakdown(L"tool");
            auto const chatDaily = database.GetChatGPTEstimatedDailyUsage(0);
            auto const chatHourly = database.GetChatGPTEstimatedHourlyUsage(0);
            int64_t devicesAfterChatGpt{};
            for (auto const& device : database.GetDeviceSummaries(10))
            {
                devicesAfterChatGpt += device.counts.DisplayTotal();
            }
            if (allChatGpt.estimatedTokens != 1900 || allChatGpt.estimatedSessions != 2 ||
                allChatGpt.counts.input != 1600 || allChatGpt.counts.output != 300 ||
                chatA.estimatedTokens != 1200 || chatA.estimatedSessions != 1 ||
                chatSessions.size() != 2 ||
                std::ranges::any_of(chatSessions, [](auto const& item)
                {
                    return item.sourceKind != L"chatgpt-export";
                }) ||
                chatSessionBreakdown.size() != 2 || chatModels.size() != 2 ||
                chatAccounts.size() != 2 || chatTools.size() != 1 ||
                chatTools.front().measurement != MeasurementKind::Estimated ||
                chatTools.front().CacheAvailable() ||
                chatDaily.size() != 2 || chatHourly.size() != 4 ||
                database.GetChatGPTEstimatedBreakdown(L"device").size() != 0 ||
                database.GetChatGPTEstimatedBreakdown(L"project").size() != 0 ||
                database.GetTotals().counts.reportedTotal != 1750 ||
                database.GetTotals().estimatedTokens != 1900 ||
                devicesAfterChatGpt != devicesBeforeChatGptTotal)
            {
                return false;
            }

            Database promoted(L":memory:");
            promoted.Initialize();
            SessionRecord wslSession{
                L"wsl-first-session", L"wsl://Ubuntu/home/test/.codex/active.jsonl",
                L"codex-wsl", L"WSL first", L"/home/test/project", L"gpt-test",
                L"wsl-device", 200, 200, 0
            };
            wslSession.accountId = L"wsl-account";
            promoted.UpsertSession(wslSession);
            PromptEvent wslPrompt{
                wslSession.sourcePath, 10, wslSession.id, wslSession.sourceKind, L"Codex",
                wslSession.model, wslSession.project, wslSession.deviceId, L"turn-1", 1,
                200, L"1970-01-01"
            };
            wslPrompt.accountId = wslSession.accountId;
            UsageEvent wslUsage{
                wslSession.sourcePath, 20, wslSession.id, wslSession.sourceKind, L"Codex",
                wslSession.model, wslSession.project, wslSession.deviceId, L"turn-1", 1,
                201, L"1970-01-01", { 1200, 900, 0, 300, 80, 1500 }
            };
            wslUsage.accountId = wslSession.accountId;
            UsageEvent archivedWslUsage = wslUsage;
            archivedWslUsage.sourcePath = L"wsl://Ubuntu/home/test/.codex/archive.jsonl";
            archivedWslUsage.sourceOffset = 21;
            archivedWslUsage.timestamp = 202;
            archivedWslUsage.counts = { 200, 50, 0, 50, 0, 250 };
            ToolEvent wslTool{
                wslSession.sourcePath, 30, wslSession.id, wslSession.sourceKind, L"Codex",
                wslSession.model, wslSession.project, wslSession.deviceId, L"turn-1", 1,
                203, L"1970-01-01", L"shell_command", L"wsl-call", 40
            };
            wslTool.accountId = wslSession.accountId;
            if (!promoted.InsertPromptEvent(wslPrompt) ||
                !promoted.InsertUsageEvent(wslUsage) ||
                !promoted.InsertUsageEvent(archivedWslUsage) ||
                !promoted.InsertToolEvent(wslTool))
            {
                return false;
            }
            auto const wslSessionsBeforePromotion = promoted.GetRecentSessions();
            if (wslSessionsBeforePromotion.size() != 1 ||
                wslSessionsBeforePromotion.front().sourceKind != L"codex-wsl")
            {
                return false;
            }

            SessionRecord localSession = wslSession;
            localSession.sourcePath = L"C:\\Users\\test\\.codex\\session.jsonl";
            localSession.sourceKind = L"codex";
            localSession.accountId = L"local-account";
            localSession.deviceId = L"local-device";
            localSession.project = L"C:\\work\\project";
            localSession.model = L"gpt-local";
            localSession.updatedAt = 300;
            promoted.Transaction([&] { promoted.UpsertSession(localSession); });
            promoted.UpsertSession(localSession);

            PromptEvent localPrompt = wslPrompt;
            localPrompt.sourcePath = localSession.sourcePath;
            localPrompt.sourceKind = localSession.sourceKind;
            localPrompt.accountId = localSession.accountId;
            localPrompt.deviceId = localSession.deviceId;
            localPrompt.project = localSession.project;
            localPrompt.model = localSession.model;
            UsageEvent localUsage = wslUsage;
            localUsage.sourcePath = localSession.sourcePath;
            localUsage.sourceKind = localSession.sourceKind;
            localUsage.accountId = localSession.accountId;
            localUsage.deviceId = localSession.deviceId;
            localUsage.project = localSession.project;
            localUsage.model = localSession.model;
            UsageEvent localArchivedUsage = archivedWslUsage;
            localArchivedUsage.sourcePath = localSession.sourcePath;
            localArchivedUsage.sourceKind = localSession.sourceKind;
            localArchivedUsage.accountId = localSession.accountId;
            localArchivedUsage.deviceId = localSession.deviceId;
            localArchivedUsage.project = localSession.project;
            localArchivedUsage.model = localSession.model;
            ToolEvent localTool = wslTool;
            localTool.sourcePath = localSession.sourcePath;
            localTool.sourceKind = localSession.sourceKind;
            localTool.accountId = localSession.accountId;
            localTool.deviceId = localSession.deviceId;
            localTool.project = localSession.project;
            localTool.model = localSession.model;
            if (promoted.InsertPromptEvent(localPrompt) ||
                promoted.InsertUsageEvent(localUsage) ||
                promoted.InsertUsageEvent(localArchivedUsage) ||
                promoted.InsertToolEvent(localTool))
            {
                return false;
            }

            auto const promotedDays = promoted.GetDailyUsage(0);
            auto const promotedHours = promoted.GetHourlyUsage(0);
            auto const promotedTotals = promoted.GetTotals();
            auto const promotedSessions = promoted.GetRecentSessions();
            auto const promotedTurns = promoted.GetSessionTurns(localSession.id);
            auto const promotedTools = promoted.GetToolCalls(localSession.id, 1);
            Statement promotedDetails(promoted.m_database, R"sql(
                SELECT COUNT(*), COALESCE(SUM(source_path=?2),0),
                       (SELECT COUNT(*) FROM usage_events
                        WHERE session_id=?1 AND model=?3)
                FROM (
                    SELECT source_path FROM usage_events WHERE session_id=?1
                    UNION ALL SELECT source_path FROM prompt_events WHERE session_id=?1
                    UNION ALL SELECT source_path FROM tool_events WHERE session_id=?1
                );
            )sql");
            promotedDetails.Bind(1, localSession.id);
            promotedDetails.Bind(2, localSession.sourcePath);
            promotedDetails.Bind(3, localSession.model);
            if (!promotedDetails.Step() || promotedDetails.Int64(0) != 4 ||
                promotedDetails.Int64(1) != 4 || promotedDetails.Int64(2) != 2 ||
                !promoted.HasSessionSource(localSession.id, L"codex") ||
                promoted.HasSessionSource(localSession.id, L"codex-wsl") ||
                promotedDays.size() != 1 ||
                promotedDays.front().sourceKind != localSession.sourceKind ||
                promotedDays.front().accountId != localSession.accountId ||
                promotedDays.front().deviceId != localSession.deviceId ||
                promotedDays.front().project != localSession.project ||
                promotedDays.front().model != localSession.model ||
                promotedDays.front().counts.reportedTotal != 1750 ||
                promotedDays.front().messages != 1 || promotedDays.front().toolCalls != 1 ||
                promotedHours.size() != 1 ||
                promotedHours.front().sourceKind != localSession.sourceKind ||
                promotedHours.front().accountId != localSession.accountId ||
                promotedHours.front().deviceId != localSession.deviceId ||
                promotedHours.front().project != localSession.project ||
                promotedHours.front().model != localSession.model ||
                promotedHours.front().counts.reportedTotal != 1750 ||
                promotedHours.front().messages != 1 || promotedHours.front().toolCalls != 1 ||
                promotedTotals.counts.reportedTotal != 1750 ||
                promotedTotals.messages != 1 || promotedTotals.toolCalls != 1 ||
                promotedSessions.size() != 1 ||
                promotedSessions.front().sourceKind != localSession.sourceKind ||
                promotedSessions.front().accountId != localSession.accountId ||
                promotedSessions.front().deviceId != localSession.deviceId ||
                promotedSessions.front().project != localSession.project ||
                promotedSessions.front().model != localSession.model ||
                promotedSessions.front().counts.reportedTotal != 1750 ||
                promotedTurns.size() != 1 || promotedTurns.front().counts.reportedTotal != 1750 ||
                promotedTurns.front().model != localSession.model ||
                promotedTurns.front().tools != L"shell_command" ||
                promotedTools.size() != 1 || promotedTools.front().sourcePath != localSession.sourcePath)
            {
                return false;
            }

            Database accountKeys(L":memory:");
            accountKeys.Initialize();
            accountKeys.SetAppState(L"test_cursor", L"3001");
            if (accountKeys.GetAppState(L"missing_state") ||
                accountKeys.GetAppState(L"test_cursor") != std::optional<std::wstring>{L"3001"})
            {
                return false;
            }
            UsageEvent accountAUsage = usage;
            accountAUsage.sourcePath = L"shared.jsonl";
            accountAUsage.sessionId = L"shared-session";
            accountAUsage.sourceOffset = 1;
            accountAUsage.accountId = L"account-a";
            accountAUsage.counts = { 100, 25, 0, 10, 0, 110 };
            UsageEvent accountBUsage = accountAUsage;
            accountBUsage.sourceOffset = 2;
            accountBUsage.accountId = L"account-b";
            accountBUsage.counts = { 200, 100, 0, 20, 0, 220 };
            UsageEvent followingDayUsage = accountAUsage;
            followingDayUsage.sourceOffset = 3;
            followingDayUsage.day = L"1970-01-02";
            if (!accountKeys.InsertUsageEvent(accountAUsage) ||
                !accountKeys.InsertUsageEvent(accountBUsage) ||
                !accountKeys.InsertUsageEvent(followingDayUsage) ||
                accountKeys.GetBreakdown(L"account").size() != 2 ||
                accountKeys.GetHourlyUsage(0).size() != 3)
            {
                return false;
            }

            Database calendarDays(L":memory:");
            calendarDays.Initialize();
            int64_t const now = UnixNow();
            UsageEvent todayUsage = usage;
            todayUsage.sourcePath = L"today.jsonl";
            todayUsage.sourceOffset = 1;
            todayUsage.sessionId = L"today";
            todayUsage.timestamp = now;
            todayUsage.day = LocalCalendarDay();
            todayUsage.counts = { 10, 0, 0, 0, 0, 10 };
            UsageEvent yesterdayUsage = todayUsage;
            yesterdayUsage.sourcePath = L"yesterday.jsonl";
            yesterdayUsage.sessionId = L"yesterday";
            yesterdayUsage.timestamp = now - 86400;
            yesterdayUsage.day = LocalCalendarDay(1);
            yesterdayUsage.counts = { 20, 0, 0, 0, 0, 20 };
            if (!calendarDays.InsertUsageEvent(todayUsage) ||
                !calendarDays.InsertUsageEvent(yesterdayUsage) ||
                calendarDays.GetDailyUsage(1).size() != 1 ||
                calendarDays.GetDailyUsage(2).size() != 2 ||
                calendarDays.GetHourlyUsage(1).size() != 1 ||
                calendarDays.GetHourlyUsage(2).size() != 2 ||
                !calendarDays.PruneDetailsIfDue() ||
                calendarDays.PruneDetailsIfDue())
            {
                return false;
            }

            Database migrated(L":memory:");
            migrated.Execute(R"sql(
                CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL);
                CREATE TABLE sessions(
                    id TEXT PRIMARY KEY,
                    source_path TEXT NOT NULL DEFAULT '',
                    source_kind TEXT NOT NULL DEFAULT 'codex'
                );
                CREATE TABLE daily_usage(
                    source_path TEXT NOT NULL,
                    day TEXT NOT NULL,
                    session_id TEXT NOT NULL,
                    source_kind TEXT NOT NULL,
                    tool TEXT NOT NULL,
                    model TEXT NOT NULL DEFAULT '',
                    project TEXT NOT NULL DEFAULT '',
                    device_id TEXT NOT NULL DEFAULT '',
                    first_timestamp INTEGER NOT NULL DEFAULT 0,
                    last_timestamp INTEGER NOT NULL DEFAULT 0,
                    input_tokens INTEGER NOT NULL DEFAULT 0,
                    cached_input_tokens INTEGER NOT NULL DEFAULT 0,
                    cache_write_input_tokens INTEGER NOT NULL DEFAULT 0,
                    output_tokens INTEGER NOT NULL DEFAULT 0,
                    reasoning_output_tokens INTEGER NOT NULL DEFAULT 0,
                    reported_total_tokens INTEGER NOT NULL DEFAULT 0,
                    messages INTEGER NOT NULL DEFAULT 0,
                    tool_calls INTEGER NOT NULL DEFAULT 0,
                    PRIMARY KEY(source_path, day, session_id, source_kind, tool, model, project, device_id)
                );
                CREATE TABLE hourly_usage(
                    source_path TEXT NOT NULL,
                    hour_start INTEGER NOT NULL,
                    day TEXT NOT NULL,
                    session_id TEXT NOT NULL,
                    source_kind TEXT NOT NULL,
                    tool TEXT NOT NULL,
                    model TEXT NOT NULL DEFAULT '',
                    project TEXT NOT NULL DEFAULT '',
                    device_id TEXT NOT NULL DEFAULT '',
                    input_tokens INTEGER NOT NULL DEFAULT 0,
                    cached_input_tokens INTEGER NOT NULL DEFAULT 0,
                    cache_write_input_tokens INTEGER NOT NULL DEFAULT 0,
                    output_tokens INTEGER NOT NULL DEFAULT 0,
                    reasoning_output_tokens INTEGER NOT NULL DEFAULT 0,
                    reported_total_tokens INTEGER NOT NULL DEFAULT 0,
                    messages INTEGER NOT NULL DEFAULT 0,
                    tool_calls INTEGER NOT NULL DEFAULT 0,
                    PRIMARY KEY(source_path, hour_start, session_id, source_kind, tool, model, project, device_id)
                );
                INSERT INTO daily_usage(
                    source_path, day, session_id, source_kind, tool, first_timestamp,
                    last_timestamp, reported_total_tokens)
                VALUES('migration.jsonl', '2026-01-01', 'migration-session', 'codex',
                       'Codex', 1, 1, 183);
                INSERT INTO hourly_usage(
                    source_path, hour_start, day, session_id, source_kind, tool,
                    reported_total_tokens)
                VALUES('migration.jsonl', 0, '2026-01-01', 'migration-session', 'codex',
                       'Codex', 270);
                PRAGMA user_version=2;
            )sql");
            migrated.Initialize();
            Statement migratedVersion(migrated.m_database, "PRAGMA user_version;");
            if (!migratedVersion.Step() || migratedVersion.Int(0) != 6) return false;
            auto hasAccountColumn = [&](char const* table)
            {
                std::string sql = "PRAGMA table_info(";
                sql += table;
                sql += ");";
                Statement columns(migrated.m_database, sql.c_str());
                while (columns.Step())
                {
                    if (columns.Text(1) == L"account_id") return true;
                }
                return false;
            };
            auto columnIsPartOfPrimaryKey = [&](char const* table, std::wstring_view column)
            {
                std::string sql = "PRAGMA table_info(";
                sql += table;
                sql += ");";
                Statement columns(migrated.m_database, sql.c_str());
                while (columns.Step())
                {
                    if (columns.Text(1) == column) return columns.Int(5) > 0;
                }
                return false;
            };
            auto const migratedDays = migrated.GetDailyUsage(0);
            auto const migratedHours = migrated.GetHourlyUsage(0);
            return hasAccountColumn("sessions") &&
                   hasAccountColumn("daily_usage") &&
                   hasAccountColumn("hourly_usage") &&
                   columnIsPartOfPrimaryKey("daily_usage", L"account_id") &&
                   columnIsPartOfPrimaryKey("hourly_usage", L"account_id") &&
                   columnIsPartOfPrimaryKey("hourly_usage", L"day") &&
                   migratedDays.size() == 1 && migratedDays.front().counts.reportedTotal == 183 &&
                   migratedHours.size() == 1 && migratedHours.front().counts.reportedTotal == 270;
        }
        catch (...)
        {
            return false;
        }
    }

    void Database::Execute(char const* sql)
    {
        char* error{};
        int const result = sqlite3_exec(m_database, sql, nullptr, nullptr, &error);
        if (result != SQLITE_OK)
        {
            std::string message = error ? error : sqlite3_errmsg(m_database);
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

    [[noreturn]] void Database::ThrowDatabaseError(char const* action) const
    {
        throw std::runtime_error(std::string(action) + ": " + sqlite3_errmsg(m_database));
    }
}
