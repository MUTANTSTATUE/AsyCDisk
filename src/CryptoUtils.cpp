#include "CryptoUtils.h"
#include <openssl/sha.h>
#include <cstring>
#include <algorithm>

void CryptoUtils::ProcessCTR(const std::vector<uint8_t>& key, uint64_t offset, std::vector<char>& data) {
    if (data.empty()) return;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    
    // 我们使用固定的 IV（全 0），在实际生产中应该针对每个文件生成一个随机 Salt
    // 但为了支持 Seek，IV 的计数器部分必须根据 offset 调整
    uint8_t iv[16] = {0};
    
    // AES-CTR 的 IV 是一个 16 字节的计数器。
    // 如果 offset > 0，我们需要将 IV 增加 offset / 16
    uint64_t counter = offset / 16;
    for (int i = 15; i >= 0 && counter > 0; i--) {
        uint16_t val = (uint16_t)iv[i] + (counter & 0xFF);
        iv[i] = val & 0xFF;
        counter >>= 8;
        if (val > 0xFF) counter++; // Carry
    }

    EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), NULL, key.data(), iv);

    // 如果 offset 不是 16 的倍数，需要先丢弃前 offset % 16 字节的流
    int skip = offset % 16;
    if (skip > 0) {
        uint8_t dummy_in[16] = {0};
        uint8_t dummy_out[32];
        int outl;
        EVP_EncryptUpdate(ctx, dummy_out, &outl, dummy_in, skip);
    }

    int outl;
    EVP_EncryptUpdate(ctx, (uint8_t*)data.data(), &outl, (uint8_t*)data.data(), data.size());

    EVP_CIPHER_CTX_free(ctx);
}

std::vector<uint8_t> CryptoUtils::DeriveKey(const std::string& password) {
    std::vector<uint8_t> key(32);
    SHA256((const uint8_t*)password.c_str(), password.length(), key.data());
    return key;
}
