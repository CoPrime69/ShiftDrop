#include "Hash.h"

#include <windows.h>
#include <bcrypt.h>
#include <wil/resource.h>

#include <vector>

namespace sd
{
    std::string Sha256File(const std::wstring& path, uint64_t* size)
    {
        wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file)
            return {};

        BCRYPT_ALG_HANDLE alg = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
            return {};
        auto closeAlg = wil::scope_exit([&] { BCryptCloseAlgorithmProvider(alg, 0); });
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0)
            return {};
        auto closeHash = wil::scope_exit([&] { BCryptDestroyHash(hash); });

        std::vector<BYTE> buffer(1 << 20);
        uint64_t total = 0;
        for (;;)
        {
            DWORD read = 0;
            if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
                return {};
            if (read == 0)
                break;
            total += read;
            if (BCryptHashData(hash, buffer.data(), read, 0) != 0)
                return {};
        }
        BYTE digest[32]{};
        if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0)
            return {};
        if (size)
            *size = total;

        static const char hex[] = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (BYTE b : digest)
        {
            out.push_back(hex[b >> 4]);
            out.push_back(hex[b & 15]);
        }
        return out;
    }
}
