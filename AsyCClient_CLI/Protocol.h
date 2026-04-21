#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Protocol {

// Magic Number: "ACDK" (AsyCDisk)
// Stored as a 32-bit integer for fast verification
constexpr uint32_t MAGIC_NUMBER = 0x4B444341;

// Protocol Version
constexpr uint8_t CURRENT_VERSION = 1;

// #pragma pack(push, 1) ensures no struct padding, so it maps exactly to bytes
// on the network
#pragma pack(push, 1)
struct Header {
  uint32_t magic;      // 4 bytes: Magic Number (0x4B444341)
  uint8_t version;     // 1 byte:  Protocol Version
  uint16_t command;    // 2 bytes: Command ID
  uint16_t status;     // 2 bytes: Status Code (0 for req, 200/40x/50x for resp)
  uint32_t json_len;   // 4 bytes: Length of the JSON payload
  uint64_t binary_len; // 8 bytes: Length of the raw Binary payload
};
#pragma pack(pop)

// Ensure the header size is exactly 21 bytes
static_assert(sizeof(Header) == 21, "Header size must be 21 bytes");
constexpr size_t HEADER_SIZE = sizeof(Header);

// Defined Command IDs
enum class Command : uint16_t {
  Ping = 0,
  Login = 1,
  ListDir = 2,
  MakeDir = 3,
  Remove = 4,
  Register = 5,

  // File Transfer
  UploadReq = 10,  // Request to upload a file (contains file metadata in JSON)
  UploadData = 11, // Actual file binary data chunk
  DownloadReq =
      12, // Request to download a file or chunk (supports Range for videos)
  DownloadData = 13 // Server sending the requested file binary data chunk
};

// Logical Message Structure (useful for parsing small to medium messages)
// Note: For very large files, binary_payload will be handled via
// streaming/chunks, not buffered entirely in memory.
struct Message {
  Header header;
  nlohmann::json json_payload;
  std::vector<char> binary_payload;
};

} // namespace Protocol
