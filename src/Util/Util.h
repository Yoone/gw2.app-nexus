///----------------------------------------------------------------------------------------------------
/// Util.h: small primitives the transport needs, standard library only.
///----------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Util
{
    /* Returns false on malformed input rather than throwing. Tolerates missing padding. */
    bool Base64Decode(const std::string& aInput, std::vector<uint8_t>& aOut);
    std::string Base64Encode(const uint8_t* aData, size_t aSize);

    void Sha1(const uint8_t* aData, size_t aSize, uint8_t aOut[20]);

    /* The Sec-WebSocket-Accept value to answer a client's Sec-WebSocket-Key with. */
    std::string WebSocketAcceptKey(const std::string& aClientKey);

    bool IEquals(const std::string& aLhs, const std::string& aRhs);
    std::string ToLower(std::string aValue);
    std::string Trim(const std::string& aValue);
    std::vector<std::string> Split(const std::string& aValue, char aDelimiter);
}
