#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <uv.h>

struct FileTask {
    uint32_t stream_id = 0;
    uv_file file_handle = -1;
    uint64_t file_offset = 0;
    uint64_t total_filesize = 0;
    std::string current_filename;
    std::string full_path;
    int parent_id = 0;
    bool is_uploading = false;
    uint32_t pending_fs_reqs = 0;
    bool closing_pending = false;
    std::vector<char> file_read_buf;
};

