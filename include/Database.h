#pragma once

#include <sqlite3.h>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <mutex>

class Database {
public:
    static Database& GetInstance();

    bool Open(const std::string& db_path);
    void Close();

    // Execute non-query SQL (INSERT, UPDATE, DELETE, CREATE)
    bool Execute(const std::string& sql);

    // Execute query SQL (SELECT)
    nlohmann::json Query(const std::string& sql);

    // Business Logic Helpers
    bool AuthenticateUser(const std::string& username, const std::string& password, int& user_id);
    bool RegisterUser(const std::string& username, const std::string& password);
    nlohmann::json ListFiles(int user_id, int parent_id);
    nlohmann::json GetFile(int user_id, int parent_id, const std::string& filename);
    nlohmann::json GetFileById(int user_id, int file_id);
    bool AddFile(int owner_id, int parent_id, const std::string& filename, size_t size, bool is_dir, const std::string& path);
    bool DeleteFile(int owner_id, int parent_id, const std::string& filename);

private:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    sqlite3* db_ = nullptr;
    std::recursive_mutex mutex_;
};
