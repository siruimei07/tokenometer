#include "Database.h"

#include <windows.h>
#include <shlobj.h>
#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace tokenometer
{
    namespace
    {
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
        if (version > 4)
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
                PRIMARY KEY(source_path, hour_start, session_id, source_kind, account_id, tool, model, project, device_id)
            );
            CREATE INDEX IF NOT EXISTS hourly_usage_time_idx ON hourly_usage(hour_start);
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
            INSERT OR IGNORE INTO schema_migrations(version, applied_at)
                VALUES(1, CAST(strftime('%s','now') AS INTEGER));
            INSERT OR IGNORE INTO schema_migrations(version, applied_at)
                VALUES(2, CAST(strftime('%s','now') AS INTEGER));
            INSERT OR IGNORE INTO schema_migrations(version, applied_at)
                VALUES(3, CAST(strftime('%s','now') AS INTEGER));
            INSERT OR IGNORE INTO schema_migrations(version, applied_at)
                VALUES(4, CAST(strftime('%s','now') AS INTEGER));
            PRAGMA user_version=4;
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
        }
    }

    std::wstring Database::GetOrCreateDeviceId(std::wstring_view displayName)
    {
        std::scoped_lock lock(m_mutex);
        Statement existing(m_database, "SELECT value FROM app_state WHERE key='local_device_id';");
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
                INSERT INTO app_state(key, value) VALUES('local_device_id', ?1)
                ON CONFLICT(key) DO UPDATE SET value=excluded.value;
            )sql");
            save.Bind(1, id);
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
        Statement statement(m_database, R"sql(
            INSERT INTO sessions(
                id, source_path, source_kind, account_id, title, project, model, device_id,
                started_at, updated_at, message_count)
            VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)
            ON CONFLICT(id) DO UPDATE SET
                source_path=excluded.source_path,
                account_id=CASE WHEN excluded.account_id='' THEN sessions.account_id ELSE excluded.account_id END,
                title=CASE WHEN excluded.title='' THEN sessions.title ELSE excluded.title END,
                project=CASE WHEN excluded.project='' THEN sessions.project ELSE excluded.project END,
                model=CASE WHEN excluded.model='' THEN sessions.model ELSE excluded.model END,
                device_id=CASE WHEN excluded.device_id='' THEN sessions.device_id ELSE excluded.device_id END,
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
            ON CONFLICT(source_path, hour_start, session_id, source_kind, account_id, tool, model, project, device_id)
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
            ON CONFLICT(source_path, hour_start, session_id, source_kind, account_id, tool, model, project, device_id)
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
            ON CONFLICT(source_path, hour_start, session_id, source_kind, account_id, tool, model, project, device_id)
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
        return result;
    }

    std::vector<DailyUsage> Database::GetDailyUsage(int days)
    {
        std::scoped_lock lock(m_mutex);
        int64_t const since = days > 0 ? UnixNow() - static_cast<int64_t>(days) * 86400 : 0;
        Statement statement(m_database, R"sql(
            SELECT day, source_kind, tool, model, project, device_id, account_id,
                   SUM(input_tokens), SUM(cached_input_tokens), SUM(cache_write_input_tokens),
                   SUM(output_tokens), SUM(reasoning_output_tokens), SUM(reported_total_tokens),
                   SUM(messages), SUM(tool_calls)
            FROM daily_usage
            WHERE (?1=0 OR last_timestamp>=?1)
            GROUP BY day, source_kind, tool, model, project, device_id, account_id
            ORDER BY day ASC;
        )sql");
        statement.Bind(1, since);
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
        int64_t const since = days > 0 ? UnixNow() - static_cast<int64_t>(days) * 86400 : 0;
        Statement statement(m_database, R"sql(
            SELECT hour_start, day, source_kind, tool, model, project, device_id, account_id,
                   SUM(input_tokens), SUM(cached_input_tokens), SUM(cache_write_input_tokens),
                   SUM(output_tokens), SUM(reasoning_output_tokens), SUM(reported_total_tokens),
                   SUM(messages), SUM(tool_calls)
            FROM hourly_usage
            WHERE (?1=0 OR hour_start>=?1)
            GROUP BY hour_start, day, source_kind, tool, model, project, device_id, account_id
            ORDER BY hour_start ASC;
        )sql");
        statement.Bind(1, since);
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
                   COALESCE(SUM(d.reported_total_tokens),0)
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
            SessionRecord session{ L"session-1", L"fixture.jsonl", L"codex", L"Fixture", L"D:\\work", L"gpt-test", L"test-device", 100, 100, 0 };
            session.accountId = L"account-a";
            database.UpsertSession(session);

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
            PromptEvent secondPrompt = prompt;
            secondPrompt.sourcePath = second.sourcePath;
            secondPrompt.sessionId = second.id;
            secondPrompt.accountId = second.accountId;
            UsageEvent secondUsage = usage;
            secondUsage.sourcePath = second.sourcePath;
            secondUsage.sessionId = second.id;
            secondUsage.accountId = second.accountId;
            secondUsage.counts = { 200, 50, 0, 50, 0, 250 };
            ToolEvent secondTool = tool;
            secondTool.sourcePath = second.sourcePath;
            secondTool.sessionId = second.id;
            secondTool.name = L"other_tool";
            secondTool.accountId = second.accountId;
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
                firstTools.size() == 1 && firstTools.front().name == L"shell_command" &&
                secondTools.size() == 1 && secondTools.front().name == L"other_tool" &&
                firstTurns.size() == 1 && firstTurns.front().tools == L"shell_command";
            if (!groupingValid) return false;

            Database accountKeys(L":memory:");
            accountKeys.Initialize();
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
            if (!accountKeys.InsertUsageEvent(accountAUsage) ||
                !accountKeys.InsertUsageEvent(accountBUsage) ||
                accountKeys.GetBreakdown(L"account").size() != 2 ||
                accountKeys.GetHourlyUsage(0).size() != 2)
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
                PRAGMA user_version=2;
            )sql");
            migrated.Initialize();
            Statement migratedVersion(migrated.m_database, "PRAGMA user_version;");
            if (!migratedVersion.Step() || migratedVersion.Int(0) != 4) return false;
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
            auto accountIsPartOfPrimaryKey = [&](char const* table)
            {
                std::string sql = "PRAGMA table_info(";
                sql += table;
                sql += ");";
                Statement columns(migrated.m_database, sql.c_str());
                while (columns.Step())
                {
                    if (columns.Text(1) == L"account_id") return columns.Int(5) > 0;
                }
                return false;
            };
            return hasAccountColumn("sessions") &&
                   hasAccountColumn("daily_usage") &&
                   hasAccountColumn("hourly_usage") &&
                   accountIsPartOfPrimaryKey("daily_usage") &&
                   accountIsPartOfPrimaryKey("hourly_usage");
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
