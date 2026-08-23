#include "Util.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace Util
{
    namespace
    {
        constexpr const char* B64_ALPHABET =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        /* 0..63 for a base64 digit, -1 for padding/whitespace (skipped), -2 for garbage. */
        int B64Value(char aChar)
        {
            unsigned char c = (unsigned char)aChar;
            if (c >= 'A' && c <= 'Z') { return c - 'A'; }
            if (c >= 'a' && c <= 'z') { return c - 'a' + 26; }
            if (c >= '0' && c <= '9') { return c - '0' + 52; }
            if (c == '+') { return 62; }
            if (c == '/') { return 63; }
            if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') { return -1; }
            return -2;
        }

        uint32_t RotL(uint32_t aValue, int aBits)
        {
            return (aValue << aBits) | (aValue >> (32 - aBits));
        }
    }

    bool Base64Decode(const std::string& aInput, std::vector<uint8_t>& aOut)
    {
        aOut.clear();
        aOut.reserve((aInput.size() / 4) * 3 + 3);

        uint32_t accumulator = 0;
        int      bits        = 0;

        for (char ch : aInput)
        {
            int value = B64Value(ch);
            if (value == -1) { continue; }          /* padding and whitespace carry no bits */
            if (value == -2) { aOut.clear(); return false; }

            accumulator = (accumulator << 6) | (uint32_t)value;
            bits += 6;

            if (bits >= 8)
            {
                bits -= 8;
                aOut.push_back((uint8_t)((accumulator >> bits) & 0xFF));
            }
        }

        /* A trailing group of one base64 digit encodes no whole byte. That is malformed input,
           not the missing padding we otherwise tolerate. */
        if (bits >= 6) { aOut.clear(); return false; }

        return true;
    }

    std::string Base64Encode(const uint8_t* aData, size_t aSize)
    {
        std::string out;
        if (aData == nullptr || aSize == 0) { return out; }

        out.reserve(((aSize + 2) / 3) * 4);

        size_t i = 0;
        while (i + 3 <= aSize)
        {
            uint32_t triple = ((uint32_t)aData[i] << 16) | ((uint32_t)aData[i + 1] << 8) | aData[i + 2];
            out.push_back(B64_ALPHABET[(triple >> 18) & 0x3F]);
            out.push_back(B64_ALPHABET[(triple >> 12) & 0x3F]);
            out.push_back(B64_ALPHABET[(triple >> 6) & 0x3F]);
            out.push_back(B64_ALPHABET[triple & 0x3F]);
            i += 3;
        }

        size_t remaining = aSize - i;
        if (remaining == 1)
        {
            uint32_t triple = (uint32_t)aData[i] << 16;
            out.push_back(B64_ALPHABET[(triple >> 18) & 0x3F]);
            out.push_back(B64_ALPHABET[(triple >> 12) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        }
        else if (remaining == 2)
        {
            uint32_t triple = ((uint32_t)aData[i] << 16) | ((uint32_t)aData[i + 1] << 8);
            out.push_back(B64_ALPHABET[(triple >> 18) & 0x3F]);
            out.push_back(B64_ALPHABET[(triple >> 12) & 0x3F]);
            out.push_back(B64_ALPHABET[(triple >> 6) & 0x3F]);
            out.push_back('=');
        }

        return out;
    }

    void Sha1(const uint8_t* aData, size_t aSize, uint8_t aOut[20])
    {
        if (aOut == nullptr) { return; }
        if (aData == nullptr) { aSize = 0; }

        uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };

        auto process = [&h](const uint8_t* aBlock)
        {
            uint32_t w[80];
            for (int i = 0; i < 16; ++i)
            {
                w[i] = ((uint32_t)aBlock[i * 4] << 24) | ((uint32_t)aBlock[i * 4 + 1] << 16)
                     | ((uint32_t)aBlock[i * 4 + 2] << 8) | (uint32_t)aBlock[i * 4 + 3];
            }
            for (int i = 16; i < 80; ++i)
            {
                w[i] = RotL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            }

            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (int i = 0; i < 80; ++i)
            {
                uint32_t f = 0;
                uint32_t k = 0;
                if (i < 20)      { f = (b & c) | ((~b) & d);          k = 0x5A827999u; }
                else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
                else             { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }

                uint32_t temp = RotL(a, 5) + f + e + k + w[i];
                e = d;
                d = c;
                c = RotL(b, 30);
                b = a;
                a = temp;
            }

            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
        };

        size_t offset = 0;
        while (offset + 64 <= aSize)
        {
            process(aData + offset);
            offset += 64;
        }

        /* Two blocks wide: the 0x80 terminator and the 8-byte length can spill into a second one. */
        uint8_t tail[128];
        memset(tail, 0, sizeof(tail));

        size_t remaining = aSize - offset;
        if (remaining > 0) { memcpy(tail, aData + offset, remaining); }
        tail[remaining] = 0x80;

        size_t tailBlocks = (remaining + 1 + 8 > 64) ? 2 : 1;
        uint64_t bitCount = (uint64_t)aSize * 8;
        size_t   lengthAt = tailBlocks * 64 - 8;
        for (int i = 0; i < 8; ++i)
        {
            tail[lengthAt + i] = (uint8_t)((bitCount >> (56 - i * 8)) & 0xFF);
        }

        for (size_t i = 0; i < tailBlocks; ++i)
        {
            process(tail + i * 64);
        }

        for (int i = 0; i < 5; ++i)
        {
            aOut[i * 4]     = (uint8_t)((h[i] >> 24) & 0xFF);
            aOut[i * 4 + 1] = (uint8_t)((h[i] >> 16) & 0xFF);
            aOut[i * 4 + 2] = (uint8_t)((h[i] >> 8) & 0xFF);
            aOut[i * 4 + 3] = (uint8_t)(h[i] & 0xFF);
        }
    }

    std::string WebSocketAcceptKey(const std::string& aClientKey)
    {
        /* RFC 6455 §4.2.2: SHA-1 of the client key plus the fixed GUID, base64'd. */
        std::string combined = aClientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

        uint8_t digest[20];
        Sha1((const uint8_t*)combined.data(), combined.size(), digest);

        return Base64Encode(digest, sizeof(digest));
    }

    bool IEquals(const std::string& aLhs, const std::string& aRhs)
    {
        if (aLhs.size() != aRhs.size()) { return false; }

        for (size_t i = 0; i < aLhs.size(); ++i)
        {
            if (tolower((unsigned char)aLhs[i]) != tolower((unsigned char)aRhs[i])) { return false; }
        }

        return true;
    }

    std::string ToLower(std::string aValue)
    {
        for (char& ch : aValue)
        {
            ch = (char)tolower((unsigned char)ch);
        }
        return aValue;
    }

    std::string Trim(const std::string& aValue)
    {
        size_t begin = 0;
        size_t end   = aValue.size();

        while (begin < end && isspace((unsigned char)aValue[begin]))     { ++begin; }
        while (end > begin && isspace((unsigned char)aValue[end - 1]))   { --end; }

        return aValue.substr(begin, end - begin);
    }

    std::vector<std::string> Split(const std::string& aValue, char aDelimiter)
    {
        std::vector<std::string> parts;

        size_t start = 0;
        while (true)
        {
            size_t pos = aValue.find(aDelimiter, start);
            if (pos == std::string::npos)
            {
                parts.push_back(aValue.substr(start));
                break;
            }

            parts.push_back(aValue.substr(start, pos - start));
            start = pos + 1;
        }

        return parts;
    }
}
