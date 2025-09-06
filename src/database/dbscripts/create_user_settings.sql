-- Create table: user_settings
-- Purpose: Store arbitrary key/value user settings
-- Columns:
--   key   TEXT PRIMARY KEY
--   value TEXT NOT NULL

CREATE TABLE IF NOT EXISTS user_settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

-- Optional index for faster LIKE searches on key (SQLite will use PK index)
-- CREATE INDEX IF NOT EXISTS idx_user_settings_key ON user_settings(key);