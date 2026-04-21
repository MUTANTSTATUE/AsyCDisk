#include "Database.h"
#include "Logger.h"

Database& Database::GetInstance() {
    static Database instance;
    return instance;
}

Database::~Database() {
    Close();
}

bool Database::Open(const std::string& db_path) {
    std::lock_guard lock(mutex_);
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        LOG_ERROR("Cannot open database: {}", sqlite3_errmsg(db_));
        return false;
    }

    LOG_INFO("Database opened successfully: {}", db_path);

    // Initialize Schema
    const char* schema = 
        "CREATE TABLE IF NOT EXISTS users ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username TEXT UNIQUE NOT NULL,"
        "  password TEXT NOT NULL,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE TABLE IF NOT EXISTS files ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  owner_id INTEGER NOT NULL,"
        "  parent_id INTEGER DEFAULT 0,"
        "  filename TEXT NOT NULL,"
        "  filesize INTEGER DEFAULT 0,"
        "  is_dir INTEGER DEFAULT 0,"
        "  file_path TEXT,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  FOREIGN KEY(owner_id) REFERENCES users(id),"
        "  UNIQUE(owner_id, parent_id, filename)"
        ");";

    char* errMsg = nullptr;
    rc = sqlite3_exec(db_, schema, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        LOG_ERROR("SQL error during schema init: {}", errMsg);
        sqlite3_free(errMsg);
        return false;
    }

    // Add a default test user if not exists
    Execute("INSERT OR IGNORE INTO users (username, password) VALUES ('admin', 'admin123');");

    return true;
}

void Database::Close() {
    std::lock_guard lock(mutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
        LOG_INFO("Database closed.");
    }
}

bool Database::Execute(const std::string& sql) {
    std::lock_guard lock(mutex_);
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        LOG_ERROR("SQL error in Execute: {} | SQL: {}", errMsg, sql);
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

nlohmann::json Database::Query(const std::string& sql) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt;
    nlohmann::json result = nlohmann::json::array();

    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR("SQL error in Query prepare: {} | SQL: {}", sqlite3_errmsg(db_), sql);
        return result;
    }

    int colCount = sqlite3_column_count(stmt);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        nlohmann::json row;
        for (int i = 0; i < colCount; i++) {
            const char* colName = sqlite3_column_name(stmt, i);
            int colType = sqlite3_column_type(stmt, i);

            if (colType == SQLITE_INTEGER) {
                row[colName] = sqlite3_column_int64(stmt, i);
            } else if (colType == SQLITE_FLOAT) {
                row[colName] = sqlite3_column_double(stmt, i);
            } else if (colType == SQLITE_TEXT) {
                row[colName] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
            } else if (colType == SQLITE_NULL) {
                row[colName] = nullptr;
            }
        }
        result.push_back(row);
    }

    sqlite3_finalize(stmt);
    return result;
}

bool Database::AuthenticateUser(const std::string& username, const std::string& password, int& user_id) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt;
    const char* sql = "SELECT id FROM users WHERE username = ? AND password = ?;";
    
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR("Auth prepare failed: {}", sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, password.c_str(), -1, SQLITE_STATIC);

    bool success = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        user_id = sqlite3_column_int(stmt, 0);
        success = true;
    }

    sqlite3_finalize(stmt);
    return success;
}

bool Database::RegisterUser(const std::string& username, const std::string& password) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt;
    const char* sql = "INSERT INTO users (username, password) VALUES (?, ?);";
    
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR("Register prepare failed: {}", sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, password.c_str(), -1, SQLITE_STATIC);

    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!success) {
        LOG_ERROR("Register execution failed: {}", sqlite3_errmsg(db_));
    }

    sqlite3_finalize(stmt);
    return success;
}

nlohmann::json Database::ListFiles(int user_id, int parent_id) {
    std::string sql = "SELECT id, filename, filesize, is_dir, created_at FROM files WHERE owner_id = " + 
                      std::to_string(user_id) + " AND parent_id = " + std::to_string(parent_id) + ";";
    return Query(sql);
}

nlohmann::json Database::GetFile(int user_id, int parent_id, const std::string& filename) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt;
    const char* sql = "SELECT id, owner_id, parent_id, filename, filesize, is_dir, file_path, created_at FROM files WHERE owner_id = ? AND parent_id = ? AND filename = ?;";
    
    nlohmann::json result = nlohmann::json::object();
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return result;

    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_int(stmt, 2, parent_id);
    sqlite3_bind_text(stmt, 3, filename.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        for (int i = 0; i < sqlite3_column_count(stmt); i++) {
            const char* colName = sqlite3_column_name(stmt, i);
            int colType = sqlite3_column_type(stmt, i);
            if (colType == SQLITE_INTEGER) result[colName] = sqlite3_column_int64(stmt, i);
            else if (colType == SQLITE_TEXT) result[colName] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
        }
    }
    sqlite3_finalize(stmt);
    return result;
}

nlohmann::json Database::GetFileById(int user_id, int file_id) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt;
    const char* sql = "SELECT id, owner_id, parent_id, filename, filesize, is_dir, file_path, created_at FROM files WHERE owner_id = ? AND id = ?;";
    
    nlohmann::json result = nlohmann::json::object();
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return result;

    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_int(stmt, 2, file_id);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        for (int i = 0; i < sqlite3_column_count(stmt); i++) {
            const char* colName = sqlite3_column_name(stmt, i);
            int colType = sqlite3_column_type(stmt, i);
            if (colType == SQLITE_INTEGER) result[colName] = sqlite3_column_int64(stmt, i);
            else if (colType == SQLITE_TEXT) result[colName] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
        }
    }
    sqlite3_finalize(stmt);
    return result;
}

bool Database::AddFile(int owner_id, int parent_id, const std::string& filename, size_t size, bool is_dir, const std::string& path) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt;
    const char* sql = "INSERT OR REPLACE INTO files (owner_id, parent_id, filename, filesize, is_dir, file_path) VALUES (?, ?, ?, ?, ?, ?);";
    
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR("AddFile prepare failed: {}", sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_int(stmt, 1, owner_id);
    sqlite3_bind_int(stmt, 2, parent_id);
    sqlite3_bind_text(stmt, 3, filename.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, size);
    sqlite3_bind_int(stmt, 5, is_dir ? 1 : 0);
    sqlite3_bind_text(stmt, 6, path.c_str(), -1, SQLITE_STATIC);

    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!success) {
        LOG_ERROR("AddFile execution failed: {}", sqlite3_errmsg(db_));
    }

    sqlite3_finalize(stmt);
    return success;
}

bool Database::DeleteFile(int owner_id, int parent_id, const std::string& filename) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt* stmt;
    const char* sql = "DELETE FROM files WHERE owner_id = ? AND parent_id = ? AND filename = ?;";
    
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR("DeleteFile prepare failed: {}", sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_int(stmt, 1, owner_id);
    sqlite3_bind_int(stmt, 2, parent_id);
    sqlite3_bind_text(stmt, 3, filename.c_str(), -1, SQLITE_STATIC);

    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!success) {
        LOG_ERROR("DeleteFile execution failed: {}", sqlite3_errmsg(db_));
    }

    sqlite3_finalize(stmt);
    return success;
}
