#pragma once
#include <vector>
#include <string>
#include <openssl/evp.h>
#include <openssl/aes.h>

class CryptoUtils {
public:
    // AES-256-CTR 模式：加密和解密是同一个逻辑
    // key: 32 字节密钥
    // offset: 文件偏移量（用于计算对应的 IV 计数器）
    // data: 输入数据，处理后直接原地修改（In-place）
    static void ProcessCTR(const std::vector<uint8_t>& key, uint64_t offset, std::vector<char>& data);

    // 辅助：从字符串（如用户密码）生成 32 字节密钥
    static std::vector<uint8_t> DeriveKey(const std::string& password);
};
