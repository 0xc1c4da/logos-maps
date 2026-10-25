#include "engine/canon.h"

#include <openssl/evp.h>

#include <array>
#include <stdexcept>

namespace maps::engine {

std::string canonical(const nlohmann::json& value) {
    // -1 = no indentation, ' ' ignored, ensure_ascii = false (UTF-8 literal),
    // error_handler = strict. Keys come out sorted because nlohmann::json's
    // object type is std::map<std::string, ...>.
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
}

std::string sha256Hex(std::string_view bytes) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> out{};
    unsigned int len = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), out.data(), &len, EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("EVP_Digest(sha256) failed");
    }
    static const char hex[] = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        s.push_back(hex[out[i] >> 4]);
        s.push_back(hex[out[i] & 0xF]);
    }
    return s;
}

std::string digest(const nlohmann::json& value) {
    return sha256Hex(canonical(value));
}

}  // namespace maps::engine
