///----------------------------------------------------------------------------------------------------
/// Server.cpp: the localhost transport, on raw Winsock2.
///
/// Threads: one accept loop, one per accepted socket, and one that reaps stale poll sessions.
/// They reach the render thread only through the queues drained by TakeInbound() and TakeEvents().
///
/// The wire behaviour is a contract with the website, which ships separately.
/// See ../gw2.app-blishhud/docs/protocol.md.
///----------------------------------------------------------------------------------------------------
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601   /* inet_pton lives behind Vista+ in the MinGW headers */
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include "Server.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Addon.h"
#include "Util/Util.h"
#include "json/json.hpp"

using json = nlohmann::json;

namespace
{
    ///------------------------------------------------------------------------------------------------
    /// Constants
    ///------------------------------------------------------------------------------------------------

    /* Ceilings so a hostile or broken peer cannot drive us out of memory. Keep them off the wire:
       the client probes for its own workable body size, and an advertised limit would break that. */
    constexpr size_t MAX_HEADER_BYTES = 64 * 1024;
    constexpr size_t MAX_BODY_BYTES   = 48u * 1024 * 1024;
    constexpr size_t MAX_WS_MESSAGE   = 8u * 1024 * 1024;
    constexpr size_t MAX_FRAG_BYTES   = 16u * 1024 * 1024;   /* one reassembled poll message */
    constexpr size_t MAX_EGRESS_DEPTH = 512;                 /* module to website messages are tiny */

    constexpr int64_t HANDSHAKE_TIMEOUT_MS   = 5000;    /* WS only; expiry closes 4001 */
    constexpr int64_t POLL_SESSION_TIMEOUT_MS = 20000;  /* see MaintenanceLoop */
    constexpr int64_t PING_INTERVAL_MS       = 30000;
    constexpr int64_t PEER_SILENCE_LIMIT_MS  = 90000;   /* ~two missed pongs */
    constexpr int64_t SUPERSEDE_GRACE_MS     = 10000;   /* then force the old socket down */
    constexpr int64_t SUPERSEDED_ID_TTL_MS   = 60000;
    constexpr size_t  SUPERSEDED_ID_SLOTS    = 8;

    constexpr int SELECT_TICK_MS  = 50;      /* egress latency on an idle WebSocket */
    constexpr int SEND_TIMEOUT_MS = 2000;
    constexpr int RECV_TIMEOUT_MS = 15000;   /* one HTTP request must arrive within this */

    constexpr uint16_t CLOSE_SUPERSEDED        = 4000;
    constexpr uint16_t CLOSE_HANDSHAKE_TIMEOUT = 4001;
    constexpr uint16_t CLOSE_PROTOCOL          = 4002;
    constexpr uint16_t CLOSE_NORMAL            = 1000;

    constexpr uint8_t OP_CONTINUATION = 0x0;
    constexpr uint8_t OP_TEXT         = 0x1;
    constexpr uint8_t OP_BINARY       = 0x2;
    constexpr uint8_t OP_CLOSE        = 0x8;
    constexpr uint8_t OP_PING         = 0x9;
    constexpr uint8_t OP_PONG         = 0xA;

    constexpr const char* BODY_426 = "This endpoint expects a WebSocket connection.";

    ///------------------------------------------------------------------------------------------------
    /// Small helpers
    ///------------------------------------------------------------------------------------------------

    int64_t NowMs()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    bool SendAll(SOCKET aSocket, const char* aData, size_t aSize)
    {
        size_t sent = 0;
        while (sent < aSize)
        {
            int chunk = (int)((aSize - sent > 32768) ? 32768 : (aSize - sent));
            int n = send(aSocket, aData + sent, chunk, 0);
            if (n <= 0) { return false; }
            sent += (size_t)n;
        }
        return true;
    }

    void SetTimeouts(SOCKET aSocket)
    {
        DWORD recvMs = RECV_TIMEOUT_MS;
        DWORD sendMs = SEND_TIMEOUT_MS;
        setsockopt(aSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvMs, sizeof(recvMs));
        setsockopt(aSocket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sendMs, sizeof(sendMs));
    }

    /* nlohmann never throws here: parse() is called in its non-throwing mode and dump() replaces
       anything it cannot encode. Both matter: a throw escaping into a network thread kills GW2. */
    json ParseJson(const std::string& aText)
    {
        return json::parse(aText, nullptr, false);
    }

    std::string DumpJson(const json& aValue)
    {
        return aValue.dump(-1, ' ', false, json::error_handler_t::replace);
    }

    std::string JsonString(const json& aObject, const char* aKey)
    {
        if (!aObject.is_object()) { return ""; }
        auto it = aObject.find(aKey);
        if (it == aObject.end() || !it->is_string()) { return ""; }
        return it->get<std::string>();
    }

    ///------------------------------------------------------------------------------------------------
    /// Origin allow-list
    ///
    /// This is the entire authorisation model, checked on the WebSocket handshake and again on
    /// POST /poll. CORS alone would only hide the response from a hostile page, after the side
    /// effects had already run.
    ///------------------------------------------------------------------------------------------------

    bool ParseIPv4(const std::string& aHost, uint8_t aOut[4])
    {
        int      octet  = 0;
        int      digits = 0;
        unsigned value  = 0;

        for (size_t i = 0; i <= aHost.size(); ++i)
        {
            char ch = (i < aHost.size()) ? aHost[i] : '.';
            if (ch >= '0' && ch <= '9')
            {
                if (++digits > 3) { return false; }
                value = value * 10 + (unsigned)(ch - '0');
                if (value > 255) { return false; }
            }
            else if (ch == '.')
            {
                if (digits == 0 || octet > 3) { return false; }
                aOut[octet++] = (uint8_t)value;
                value  = 0;
                digits = 0;
            }
            else
            {
                return false;
            }
        }

        return octet == 4;
    }

    bool IsLoopbackOrPrivateIp(const std::string& aHost)
    {
        uint8_t v4[4];
        if (ParseIPv4(aHost, v4))
        {
            if (v4[0] == 127) { return true; }                              /* 127.0.0.0/8 */
            if (v4[0] == 10)  { return true; }                              /* 10.0.0.0/8 */
            if (v4[0] == 172 && v4[1] >= 16 && v4[1] <= 31) { return true; }/* 172.16.0.0/12 */
            if (v4[0] == 192 && v4[1] == 168) { return true; }              /* 192.168.0.0/16 */
            if (v4[0] == 169 && v4[1] == 254) { return true; }              /* 169.254.0.0/16 */
            return false;
        }

        uint8_t v6[16];
        if (inet_pton(AF_INET6, aHost.c_str(), v6) == 1)
        {
            bool loopback = true;
            for (int i = 0; i < 15; ++i) { if (v6[i] != 0) { loopback = false; break; } }
            if (loopback && v6[15] == 1) { return true; }                   /* ::1 */

            if (v6[0] == 0xFE && (v6[1] & 0xC0) == 0x80) { return true; }   /* fe80::/10 */
            if ((v6[0] & 0xFE) == 0xFC) { return true; }                    /* fc00::/7 */
            return false;
        }

        return false;
    }

    bool IsAllowedOrigin(const std::string& aOrigin)
    {
        if (aOrigin.empty()) { return false; }

        /* Scheme and port are left unchecked on purpose: the website moves between them. */
        size_t schemeEnd = aOrigin.find("://");
        if (schemeEnd == std::string::npos || schemeEnd == 0) { return false; }

        size_t hostStart = schemeEnd + 3;
        if (hostStart >= aOrigin.size()) { return false; }

        std::string host;
        if (aOrigin[hostStart] == '[')
        {
            size_t end = aOrigin.find(']', hostStart);
            if (end == std::string::npos) { return false; }
            host = aOrigin.substr(hostStart + 1, end - hostStart - 1);
        }
        else
        {
            size_t end = aOrigin.find_first_of(":/?#", hostStart);
            host = (end == std::string::npos) ? aOrigin.substr(hostStart)
                                              : aOrigin.substr(hostStart, end - hostStart);
        }

        if (host.empty()) { return false; }

        std::string lower = Util::ToLower(host);
        if (lower == "localhost") { return true; }
        if (lower == "gw2.app") { return true; }
        if (lower.size() > 8 && lower.compare(lower.size() - 8, 8, ".gw2.app") == 0) { return true; }

        return IsLoopbackOrPrivateIp(lower);
    }

    ///------------------------------------------------------------------------------------------------
    /// HTTP request parsing
    ///------------------------------------------------------------------------------------------------

    struct HttpRequest
    {
        std::string Method;
        std::string Target;
        std::string Path;
        std::string Body;
        std::vector<std::pair<std::string, std::string>> Headers;

        std::string Get(const char* aName) const
        {
            for (const auto& h : Headers)
            {
                if (Util::IEquals(h.first, aName)) { return h.second; }
            }
            return "";
        }

        bool Has(const char* aName) const
        {
            for (const auto& h : Headers)
            {
                if (Util::IEquals(h.first, aName)) { return true; }
            }
            return false;
        }
    };

    /* Case-insensitive token search, so "Connection: keep-alive, Upgrade" matches "upgrade". */
    bool HeaderContainsToken(const std::string& aValue, const char* aToken)
    {
        std::string haystack = Util::ToLower(aValue);
        std::string needle   = Util::ToLower(aToken);
        if (needle.empty()) { return false; }

        for (const std::string& part : Util::Split(haystack, ','))
        {
            if (Util::Trim(part) == needle) { return true; }
        }
        return haystack.find(needle) != std::string::npos;
    }

    enum class ReadResult { Ok, Dead, Malformed };

    ReadResult ReadHttpRequest(SOCKET aSocket, HttpRequest& aOut, std::string& aLeftover,
                               const std::atomic<bool>& aRunning)
    {
        std::string buffer;
        size_t      headerEnd = std::string::npos;

        char chunk[8192];
        while (aRunning.load())
        {
            headerEnd = buffer.find("\r\n\r\n");
            if (headerEnd != std::string::npos) { break; }
            if (buffer.size() > MAX_HEADER_BYTES) { return ReadResult::Malformed; }

            int n = recv(aSocket, chunk, sizeof(chunk), 0);
            if (n <= 0) { return ReadResult::Dead; }
            buffer.append(chunk, (size_t)n);
        }

        if (headerEnd == std::string::npos) { return ReadResult::Dead; }

        size_t lineEnd = buffer.find("\r\n");
        if (lineEnd == std::string::npos || lineEnd == 0) { return ReadResult::Malformed; }

        std::string requestLine = buffer.substr(0, lineEnd);
        size_t sp1 = requestLine.find(' ');
        if (sp1 == std::string::npos) { return ReadResult::Malformed; }
        size_t sp2 = requestLine.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) { return ReadResult::Malformed; }

        aOut.Method = requestLine.substr(0, sp1);
        aOut.Target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);

        size_t queryAt = aOut.Target.find_first_of("?#");
        aOut.Path = (queryAt == std::string::npos) ? aOut.Target : aOut.Target.substr(0, queryAt);

        /* Obsolete HTTP/1.1 continuation lines are not honoured. */
        size_t pos = lineEnd + 2;
        while (pos < headerEnd)
        {
            size_t end = buffer.find("\r\n", pos);
            if (end == std::string::npos || end > headerEnd) { end = headerEnd; }

            std::string line = buffer.substr(pos, end - pos);
            pos = end + 2;

            size_t colon = line.find(':');
            if (colon == std::string::npos) { continue; }

            aOut.Headers.emplace_back(Util::Trim(line.substr(0, colon)),
                                      Util::Trim(line.substr(colon + 1)));
        }

        /* Content-Length only. The website never sends a chunked body, so we refuse one. */
        std::string rest = buffer.substr(headerEnd + 4);

        if (Util::IEquals(aOut.Get("Transfer-Encoding"), "chunked")) { return ReadResult::Malformed; }

        std::string lengthHeader = aOut.Get("Content-Length");
        size_t      contentLength = 0;
        if (!lengthHeader.empty())
        {
            for (char ch : lengthHeader)
            {
                if (ch < '0' || ch > '9') { return ReadResult::Malformed; }
                contentLength = contentLength * 10 + (size_t)(ch - '0');
                if (contentLength > MAX_BODY_BYTES) { return ReadResult::Malformed; }
            }
        }

        if (contentLength > 0)
        {
            aOut.Body = rest.substr(0, (rest.size() < contentLength) ? rest.size() : contentLength);
            aLeftover = (rest.size() > contentLength) ? rest.substr(contentLength) : "";

            while (aOut.Body.size() < contentLength && aRunning.load())
            {
                size_t want = contentLength - aOut.Body.size();
                int n = recv(aSocket, chunk, (int)((want < sizeof(chunk)) ? want : sizeof(chunk)), 0);
                if (n <= 0) { return ReadResult::Dead; }
                aOut.Body.append(chunk, (size_t)n);
            }

            if (aOut.Body.size() < contentLength) { return ReadResult::Dead; }
        }
        else
        {
            aLeftover = rest;
        }

        return ReadResult::Ok;
    }

    ///------------------------------------------------------------------------------------------------
    /// HTTP responses
    ///------------------------------------------------------------------------------------------------

    /* One request per TCP connection: every response declares Content-Length and then closes. */
    void SendHttpResponse(SOCKET aSocket, const char* aStatus, const std::string& aCorsHeaders,
                          const char* aContentType, const std::string& aBody)
    {
        std::string response = "HTTP/1.1 ";
        response += aStatus;
        response += "\r\n";
        response += aCorsHeaders;
        if (aContentType != nullptr && *aContentType != '\0')
        {
            response += "Content-Type: ";
            response += aContentType;
            response += "\r\n";
        }
        response += "Content-Length: " + std::to_string(aBody.size()) + "\r\n";
        response += "Connection: close\r\n\r\n";
        response += aBody;

        SendAll(aSocket, response.data(), response.size());
    }

    std::string BuildCorsHeaders(const std::string& aOrigin)
    {
        std::string headers;

        /* Echoed, never "*", which would let any page read the response. */
        if (!aOrigin.empty() && IsAllowedOrigin(aOrigin))
        {
            headers += "Access-Control-Allow-Origin: " + aOrigin + "\r\n";
            headers += "Vary: Origin\r\n";
        }

        headers += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
        headers += "Access-Control-Allow-Headers: Upgrade, Connection, Content-Type\r\n";
        /* Mandatory: we are a private-network target for a page on the public gw2.app origin. */
        headers += "Access-Control-Allow-Private-Network: true\r\n";
        /* The browser default of 5 s would re-preflight continuously through a ~2 Hz poll loop. */
        headers += "Access-Control-Max-Age: 86400\r\n";

        return headers;
    }

    ///------------------------------------------------------------------------------------------------
    /// Message validation
    ///
    /// The dispatcher applies messages and the transport treats them as opaque JSON. These checks
    /// live here anyway, because closing 4002 on a violation is something only the transport can do.
    ///------------------------------------------------------------------------------------------------

    struct MessageCheck
    {
        bool        Valid = false;
        std::string Type;
        std::string Reason;   /* close reason when !Valid */
    };

    MessageCheck CheckMessage(const std::string& aJson)
    {
        MessageCheck check;

        json root = ParseJson(aJson);
        if (root.is_discarded() || !root.is_object())
        {
            check.Reason = "bad json";
            return check;
        }

        auto typeIt = root.find("type");
        if (typeIt == root.end() || !typeIt->is_string())
        {
            check.Reason = "missing 'type' field";
            return check;
        }

        check.Type = typeIt->get<std::string>();

        if (check.Type == "state")
        {
            auto protoIt = root.find("protocol");
            if (protoIt == root.end() || !protoIt->is_number_integer())
            {
                check.Reason = "state missing 'protocol' field";
                return check;
            }

            int proto = protoIt->get<int>();
            if (proto < 1 || proto > Addon::PROTOCOL_VERSION)
            {
                check.Reason = "unsupported protocol version " + std::to_string(proto);
                return check;
            }
        }
        else if (check.Type == "entry" || check.Type == "hover_image")
        {
            if (JsonString(root, "listId").empty())
            {
                check.Reason = "missing 'listId'";
                return check;
            }
        }
        else if (check.Type != "synced")
        {
            check.Reason = "unknown message type '" + check.Type + "'";
            return check;
        }

        check.Valid = true;
        return check;
    }

    ///------------------------------------------------------------------------------------------------
    /// Shared state
    ///------------------------------------------------------------------------------------------------

    struct WsSession;
    struct PollChannel;

    std::atomic<bool> g_running{ false };
    std::atomic<bool> g_started{ false };
    std::atomic<bool> g_clientConnected{ false };
    std::atomic<int>  g_connThreads{ 0 };

    std::atomic<SOCKET> g_listener{ INVALID_SOCKET };
    std::thread g_acceptThread;
    std::thread g_maintThread;

    std::mutex               g_queueMutex;
    std::deque<std::string>  g_inbound;
    std::deque<Server::Event> g_events;

    std::mutex              g_egressMutex;
    std::deque<std::string> g_egress;

    /* Guards the "who is the active client" tuple and the socket/session registries. */
    std::mutex g_clientMutex;
    uint64_t   g_activeWsId = 0;                        /* 0 == no active WebSocket */
    std::shared_ptr<PollChannel>                     g_activePoll;
    std::map<uint64_t, std::shared_ptr<WsSession>>   g_wsSessions;
    std::deque<std::pair<std::string, int64_t>>      g_supersededPolls;   /* id -> expiry */
    std::atomic<uint64_t> g_nextConnId{ 1 };

    std::mutex               g_socketMutex;
    std::map<uint64_t, SOCKET> g_liveSockets;

    void PushEvent(Server::Event aEvent)
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_events.push_back(aEvent);
    }

    void PushInbound(std::string aJson)
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_inbound.push_back(std::move(aJson));
    }

    void ClearEgress()
    {
        std::lock_guard<std::mutex> lock(g_egressMutex);
        g_egress.clear();
    }

    std::vector<std::string> DrainEgress()
    {
        std::vector<std::string> out;

        std::lock_guard<std::mutex> lock(g_egressMutex);
        out.reserve(g_egress.size());
        while (!g_egress.empty())
        {
            out.push_back(std::move(g_egress.front()));
            g_egress.pop_front();
        }

        return out;
    }

    /* Called with g_clientMutex held. */
    void RememberSupersededPoll(const std::string& aSessionId)
    {
        if (aSessionId.empty()) { return; }

        g_supersededPolls.emplace_back(aSessionId, NowMs() + SUPERSEDED_ID_TTL_MS);
        while (g_supersededPolls.size() > SUPERSEDED_ID_SLOTS) { g_supersededPolls.pop_front(); }
    }

    /* Called with g_clientMutex held. A few expiring slots rather than one, so two sessions
       displaced in quick succession are both remembered. */
    bool WasRecentlySuperseded(const std::string& aSessionId)
    {
        int64_t now = NowMs();
        for (auto it = g_supersededPolls.begin(); it != g_supersededPolls.end(); )
        {
            if (it->second <= now) { it = g_supersededPolls.erase(it); continue; }
            if (it->first == aSessionId) { return true; }
            ++it;
        }
        return false;
    }

    ///------------------------------------------------------------------------------------------------
    /// WebSocket session
    ///------------------------------------------------------------------------------------------------

    struct WsSession
    {
        SOCKET      Socket = INVALID_SOCKET;
        uint64_t    Id     = 0;
        std::string Remote;

        std::mutex           WriteMutex;
        std::atomic<bool>    Superseded{ false };
        std::atomic<int64_t> ForceCloseAt{ 0 };
        std::atomic<bool>    Closed{ false };

        /* Under the write mutex: another thread may be sending this session its 4000 close frame,
           and Windows recycles handles fast enough that the send could otherwise land on somebody
           else's connection. */
        void CloseSocket()
        {
            std::lock_guard<std::mutex> lock(WriteMutex);
            if (Closed.exchange(true)) { return; }

            shutdown(Socket, SD_BOTH);
            closesocket(Socket);
            Socket = INVALID_SOCKET;
        }

        bool SendFrame(uint8_t aOpcode, const char* aPayload, size_t aSize)
        {
            uint8_t header[10];
            size_t  headerSize = 0;

            header[0] = (uint8_t)(0x80 | (aOpcode & 0x0F));   /* server frames are never fragmented */

            if (aSize < 126)
            {
                header[1]  = (uint8_t)aSize;
                headerSize = 2;
            }
            else if (aSize <= 0xFFFF)
            {
                header[1]  = 126;
                header[2]  = (uint8_t)((aSize >> 8) & 0xFF);
                header[3]  = (uint8_t)(aSize & 0xFF);
                headerSize = 4;
            }
            else
            {
                header[1] = 127;
                for (int i = 0; i < 8; ++i)
                {
                    header[2 + i] = (uint8_t)((uint64_t)aSize >> (56 - i * 8) & 0xFF);
                }
                headerSize = 10;
            }

            std::lock_guard<std::mutex> lock(WriteMutex);
            if (Closed.load()) { return false; }

            if (!SendAll(Socket, (const char*)header, headerSize)) { return false; }
            if (aSize > 0 && aPayload != nullptr)
            {
                if (!SendAll(Socket, aPayload, aSize)) { return false; }
            }
            return true;
        }

        bool SendText(const std::string& aJson)
        {
            return SendFrame(OP_TEXT, aJson.data(), aJson.size());
        }

        void SendClose(uint16_t aCode, const std::string& aReason)
        {
            std::string payload;
            payload.push_back((char)((aCode >> 8) & 0xFF));
            payload.push_back((char)(aCode & 0xFF));

            /* A control frame payload is capped at 125 bytes, two of which are the code. */
            payload.append(aReason, 0, 123);

            SendFrame(OP_CLOSE, payload.data(), payload.size());
        }
    };

    bool IsActiveWs(uint64_t aId)
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        return g_activeWsId == aId;
    }

    struct WsFrame
    {
        bool     Fin    = false;
        uint8_t  Opcode = 0;
        std::string Payload;
    };

    enum class FrameResult { Ok, NeedMore, Error };

    /* Never trusts the length field: an oversized declaration is refused before a byte is buffered. */
    FrameResult TryParseFrame(std::string& aBuffer, WsFrame& aOut, std::string& aError)
    {
        if (aBuffer.size() < 2) { return FrameResult::NeedMore; }

        const uint8_t* raw = (const uint8_t*)aBuffer.data();

        aOut.Fin    = (raw[0] & 0x80) != 0;
        aOut.Opcode = (uint8_t)(raw[0] & 0x0F);

        if ((raw[0] & 0x70) != 0)
        {
            aError = "reserved bits set";
            return FrameResult::Error;
        }

        bool     masked  = (raw[1] & 0x80) != 0;
        uint64_t length  = (uint64_t)(raw[1] & 0x7F);
        size_t   offset  = 2;

        if (length == 126)
        {
            if (aBuffer.size() < 4) { return FrameResult::NeedMore; }
            length = ((uint64_t)raw[2] << 8) | raw[3];
            offset = 4;
        }
        else if (length == 127)
        {
            if (aBuffer.size() < 10) { return FrameResult::NeedMore; }
            length = 0;
            for (int i = 0; i < 8; ++i) { length = (length << 8) | raw[2 + i]; }
            if ((length >> 63) != 0)
            {
                aError = "bad frame length";
                return FrameResult::Error;
            }
            offset = 10;
        }

        if (length > MAX_WS_MESSAGE)
        {
            aError = "message too large";
            return FrameResult::Error;
        }

        /* RFC 6455 §5.1: every client-to-server frame is masked. */
        if (!masked)
        {
            aError = "expected masked frames";
            return FrameResult::Error;
        }

        if (aOut.Opcode >= 0x8)
        {
            if (length > 125 || !aOut.Fin)
            {
                aError = "bad control frame";
                return FrameResult::Error;
            }
        }

        if (aBuffer.size() < offset + 4 + (size_t)length) { return FrameResult::NeedMore; }

        uint8_t mask[4];
        memcpy(mask, aBuffer.data() + offset, 4);
        offset += 4;

        aOut.Payload.assign(aBuffer, offset, (size_t)length);
        for (size_t i = 0; i < aOut.Payload.size(); ++i)
        {
            aOut.Payload[i] = (char)((uint8_t)aOut.Payload[i] ^ mask[i % 4]);
        }

        aBuffer.erase(0, offset + (size_t)length);
        return FrameResult::Ok;
    }

    void SupersedeWs(const std::shared_ptr<WsSession>& aSession)
    {
        if (!aSession) { return; }

        /* The socket stays open until the peer echoes the close or the grace timer fires. Closing
           it here would cut the TCP connection before the frame flushed, and the browser would see
           1006 instead of the 4000 it branches on. */
        aSession->SendClose(CLOSE_SUPERSEDED, "superseded");
        aSession->Superseded.store(true);
        aSession->ForceCloseAt.store(NowMs() + SUPERSEDE_GRACE_MS);
    }

    ///------------------------------------------------------------------------------------------------
    /// Poll session
    ///------------------------------------------------------------------------------------------------

    struct PollChannel
    {
        std::string          Id;
        std::atomic<int64_t> LastPollMs{ 0 };
        std::atomic<bool>    Superseded{ false };

        /* Two polls can be in flight on one session, so the whole ingest runs under this mutex
           and the reassembly state below is only ever touched while it is held. */
        std::mutex  Mutex;
        bool        StateSeen = false;
        std::string FragId;
        int         FragNextSeq = 0;
        std::string FragData;

        void ResetFragment()
        {
            FragId.clear();
            FragData.clear();
            FragNextSeq = 0;
        }

        /* True with the reassembled message once the last slice lands. Every failure path drops
           the partial and waits for the sender to start over at seq 0. */
        bool AcceptFragment(const json& aMessage, std::string& aOut)
        {
            auto fragIt = aMessage.find("__frag");
            if (fragIt == aMessage.end() || !fragIt->is_object()) { return false; }

            std::string id = JsonString(*fragIt, "id");
            if (id.empty()) { return false; }

            int  seq   = -1;
            auto seqIt = fragIt->find("seq");
            if (seqIt != fragIt->end() && seqIt->is_number_integer()) { seq = seqIt->get<int>(); }

            bool finalSlice = false;
            auto finalIt    = fragIt->find("final");
            if (finalIt != fragIt->end() && finalIt->is_boolean()) { finalSlice = finalIt->get<bool>(); }

            std::string data = JsonString(aMessage, "data");

            if (id != FragId)
            {
                /* A new id replaces the partial: this is how the browser supersedes a hover frame
                   it has already re-captured. */
                if (seq != 0)
                {
                    ResetFragment();
                    return false;
                }

                FragId      = id;
                FragNextSeq = 0;
                FragData.clear();
            }

            if (seq != FragNextSeq)
            {
                ResetFragment();
                return false;
            }

            if (FragData.size() + data.size() > MAX_FRAG_BYTES)
            {
                ResetFragment();
                return false;
            }

            FragData += data;
            ++FragNextSeq;

            if (!finalSlice) { return false; }

            aOut = FragData;
            ResetFragment();
            return true;
        }
    };

    std::string BuildPollResponse(const std::vector<std::string>& aMessages, bool aClose, bool aResync)
    {
        std::string body = "{\"messages\":[";

        bool first = true;
        for (const std::string& message : aMessages)
        {
            /* A queued string that does not parse is dropped: the body has to stay valid JSON. */
            json parsed = ParseJson(message);
            if (parsed.is_discarded()) { continue; }

            if (!first) { body += ","; }
            body += message;
            first = false;
        }

        body += "],\"close\":";
        body += aClose ? "{\"code\":4000,\"reason\":\"superseded\"}" : "null";
        body += ",\"serverProtocol\":" + std::to_string(Addon::PROTOCOL_VERSION);

        /* Repeated from `subscribe`, because a polling client needs it on its very first poll,
           before it has sent anything. Both modules answer on this port. */
        body += ",\"module\":\""; body += Addon::MODULE_ID; body += "\"";
        if (aResync) { body += ",\"resync\":true"; }
        body += "}";

        return body;
    }

    ///------------------------------------------------------------------------------------------------
    /// Supersession: one active client at a time, last writer wins, across both transports
    ///------------------------------------------------------------------------------------------------

    /* ClientReplaced keeps the catalog, ConnectionLost (raised on teardown, not here) drops it.
       That distinction is what makes opening a second browser tab non-destructive. */
    void TakeOver(uint64_t aWsId, const std::shared_ptr<PollChannel>& aPoll)
    {
        std::shared_ptr<WsSession>   previousWs;
        std::shared_ptr<PollChannel> previousPoll;
        bool                         hadPrevious = false;

        {
            std::lock_guard<std::mutex> lock(g_clientMutex);

            if (g_activeWsId != 0 && g_activeWsId != aWsId)
            {
                auto it = g_wsSessions.find(g_activeWsId);
                if (it != g_wsSessions.end()) { previousWs = it->second; }
                hadPrevious = true;
            }

            if (g_activePoll && (!aPoll || g_activePoll->Id != aPoll->Id))
            {
                previousPoll = g_activePoll;
                hadPrevious  = true;
            }

            g_activeWsId = aWsId;
            g_activePoll = aPoll;

            if (previousPoll)
            {
                previousPoll->Superseded.store(true);
                RememberSupersededPoll(previousPoll->Id);
            }
        }

        /* Anything still queued belonged to the client we just displaced. */
        ClearEgress();

        if (previousWs) { SupersedeWs(previousWs); }

        g_clientConnected.store(true);
        PushEvent(hadPrevious ? Server::Event::ClientReplaced : Server::Event::Connected);
    }

    /* The active client went away for real: the catalog is dropped. */
    void MarkDisconnected()
    {
        g_clientConnected.store(false);
        ClearEgress();
        PushEvent(Server::Event::ConnectionLost);
    }

    ///------------------------------------------------------------------------------------------------
    /// POST /poll
    ///------------------------------------------------------------------------------------------------

    void HandlePoll(SOCKET aSocket, const HttpRequest& aRequest, const std::string& aCors)
    {
        json root = ParseJson(aRequest.Body);
        if (root.is_discarded() || !root.is_object())
        {
            Addon::Log(LOGL_WARNING, "Bad poll request JSON.");
            SendHttpResponse(aSocket, "400 Bad Request", aCors, nullptr, "");
            return;
        }

        std::string session = JsonString(root, "session");
        if (session.empty())
        {
            SendHttpResponse(aSocket, "400 Bad Request", aCors, nullptr, "");
            return;
        }

        /* The close beacon: the field just has to be there and not null, the value is never read.
           The website sends it on tab teardown so we free the session without waiting 20 s. */
        auto closeIt = root.find("close");
        if (closeIt != root.end() && !closeIt->is_null())
        {
            bool wasActive = false;
            {
                std::lock_guard<std::mutex> lock(g_clientMutex);
                if (g_activePoll && g_activePoll->Id == session)
                {
                    g_activePoll->Superseded.store(true);
                    RememberSupersededPoll(session);
                    g_activePoll.reset();
                    wasActive = true;
                }
            }

            if (wasActive) { MarkDisconnected(); }

            SendHttpResponse(aSocket, "200 OK", aCors, "application/json",
                             BuildPollResponse({}, false, false));
            return;
        }

        std::shared_ptr<PollChannel> channel;
        bool resync = false;
        bool stray  = false;

        {
            std::lock_guard<std::mutex> lock(g_clientMutex);

            if (g_activePoll && g_activePoll->Id == session)
            {
                channel = g_activePoll;
                channel->LastPollMs.store(NowMs());
            }
            else if (WasRecentlySuperseded(session))
            {
                /* A stray poll from a session we already displaced. Answer 4000 and touch nothing:
                   it must not re-register and steal the connection back. */
                stray = true;
            }
        }

        if (stray)
        {
            SendHttpResponse(aSocket, "200 OK", aCors, "application/json",
                             BuildPollResponse({}, true, false));
            return;
        }

        if (!channel)
        {
            channel = std::make_shared<PollChannel>();
            channel->Id = session;
            channel->LastPollMs.store(NowMs());

            TakeOver(0, channel);
            resync = true;

            Addon::Log(LOGL_INFO, "Poll session registered: %s", session.c_str());
        }

        bool superseded = false;
        {
            std::lock_guard<std::mutex> lock(channel->Mutex);

            auto messagesIt = root.find("messages");
            if (messagesIt != root.end() && messagesIt->is_array())
            {
                for (const json& element : *messagesIt)
                {
                    if (!element.is_object()) { continue; }

                    std::string messageJson;
                    if (element.contains("__frag"))
                    {
                        std::string reassembled;
                        if (!channel->AcceptFragment(element, reassembled)) { continue; }
                        messageJson = reassembled;
                    }
                    else
                    {
                        messageJson = DumpJson(element);
                    }

                    MessageCheck check = CheckMessage(messageJson);
                    if (!check.Valid)
                    {
                        /* Lenient by design: polling skips the offending message and keeps the
                           session, where the WebSocket transport would close. */
                        Addon::Log(LOGL_WARNING, "Skipping bad poll message: %s", check.Reason.c_str());
                        continue;
                    }

                    /* Lenient handshake: a resynced client's stale pre-state messages belong to the
                       old catalogue, so drop them rather than close. */
                    if (!channel->StateSeen)
                    {
                        if (check.Type != "state") { continue; }
                        channel->StateSeen = true;
                    }

                    {
                        std::lock_guard<std::mutex> lock2(g_clientMutex);
                        if (g_activePoll != channel) { superseded = true; }
                    }
                    if (superseded) { break; }

                    PushInbound(std::move(messageJson));
                }
            }
        }

        std::vector<std::string> outbound;
        if (!superseded && !channel->Superseded.load()) { outbound = DrainEgress(); }

        SendHttpResponse(aSocket, "200 OK", aCors, "application/json",
                         BuildPollResponse(outbound, superseded || channel->Superseded.load(), resync));
    }

    ///------------------------------------------------------------------------------------------------
    /// WebSocket connection
    ///------------------------------------------------------------------------------------------------

    void RunWebSocket(const std::shared_ptr<WsSession>& aSession, std::string aLeftover)
    {
        int64_t startMs    = NowMs();
        int64_t lastRxMs   = startMs;
        int64_t lastPingMs = startMs;

        bool        stateSeen = false;
        std::string rx        = std::move(aLeftover);
        std::string message;
        bool        inMessage = false;

        char chunk[65536];   /* chunk size, not a message limit */

        try
        {
        while (g_running.load() && !aSession->Closed.load())
        {
            int64_t now = NowMs();

            int64_t forceAt = aSession->ForceCloseAt.load();
            if (forceAt != 0 && now >= forceAt) { break; }

            bool active = IsActiveWs(aSession->Id);

            /* Single writer per socket: only this thread drains egress for a WebSocket. */
            if (active)
            {
                for (const std::string& outbound : DrainEgress())
                {
                    if (!aSession->SendText(outbound))
                    {
                        Addon::Log(LOGL_WARNING, "Failed to send to client.");
                        break;
                    }
                }
            }

            if (!stateSeen && !aSession->Superseded.load() && now - startMs > HANDSHAKE_TIMEOUT_MS)
            {
                Addon::Log(LOGL_INFO, "Handshake timeout; closing WS from %s", aSession->Remote.c_str());
                aSession->SendClose(CLOSE_HANDSHAKE_TIMEOUT, "handshake timeout");
                break;
            }

            if (now - lastPingMs >= PING_INTERVAL_MS)
            {
                if (!aSession->SendFrame(OP_PING, nullptr, 0)) { break; }
                lastPingMs = now;
            }

            if (now - lastRxMs >= PEER_SILENCE_LIMIT_MS)
            {
                Addon::Log(LOGL_INFO, "WS from %s went silent; dropping.", aSession->Remote.c_str());
                break;
            }

            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(aSession->Socket, &readable);

            timeval timeout;
            timeout.tv_sec  = 0;
            timeout.tv_usec = SELECT_TICK_MS * 1000;

            int ready = select(0, &readable, nullptr, nullptr, &timeout);
            if (ready == SOCKET_ERROR) { break; }
            if (ready == 0) { continue; }

            int received = recv(aSession->Socket, chunk, sizeof(chunk), 0);
            if (received == 0) { break; }
            if (received < 0)
            {
                int error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK || error == WSAETIMEDOUT) { continue; }
                break;
            }

            rx.append(chunk, (size_t)received);
            lastRxMs = NowMs();

            bool closing = false;
            while (!closing)
            {
                WsFrame     frame;
                std::string error;

                FrameResult result = TryParseFrame(rx, frame, error);
                if (result == FrameResult::NeedMore) { break; }
                if (result == FrameResult::Error)
                {
                    aSession->SendClose(CLOSE_PROTOCOL, error);
                    closing = true;
                    break;
                }

                if (frame.Opcode == OP_CLOSE)
                {
                    aSession->SendClose(CLOSE_NORMAL, "");
                    closing = true;
                    break;
                }

                if (frame.Opcode == OP_PING)
                {
                    aSession->SendFrame(OP_PONG, frame.Payload.data(), frame.Payload.size());
                    continue;
                }

                if (frame.Opcode == OP_PONG) { continue; }

                if (frame.Opcode == OP_BINARY)
                {
                    /* All payloads are UTF-8 JSON text; a binary frame is a protocol violation. */
                    aSession->SendClose(CLOSE_PROTOCOL, "expected text frames");
                    closing = true;
                    break;
                }

                if (frame.Opcode == OP_TEXT)
                {
                    if (inMessage)
                    {
                        aSession->SendClose(CLOSE_PROTOCOL, "interleaved message");
                        closing = true;
                        break;
                    }
                    message   = std::move(frame.Payload);
                    inMessage = true;
                }
                else if (frame.Opcode == OP_CONTINUATION)
                {
                    if (!inMessage)
                    {
                        aSession->SendClose(CLOSE_PROTOCOL, "unexpected continuation");
                        closing = true;
                        break;
                    }
                    if (message.size() + frame.Payload.size() > MAX_WS_MESSAGE)
                    {
                        aSession->SendClose(CLOSE_PROTOCOL, "message too large");
                        closing = true;
                        break;
                    }
                    message += frame.Payload;
                }
                else
                {
                    aSession->SendClose(CLOSE_PROTOCOL, "unknown opcode");
                    closing = true;
                    break;
                }

                if (!frame.Fin) { continue; }

                inMessage = false;

                MessageCheck check = CheckMessage(message);
                if (!check.Valid)
                {
                    /* Strict by design: the WebSocket transport closes on any violation. */
                    aSession->SendClose(CLOSE_PROTOCOL, check.Reason);
                    closing = true;
                    break;
                }

                if (!stateSeen)
                {
                    if (check.Type != "state")
                    {
                        aSession->SendClose(CLOSE_PROTOCOL, "first message must be 'state'");
                        closing = true;
                        break;
                    }
                    stateSeen = true;
                }

                /* A newer client may have taken over since the recv returned, and this session's
                   messages must not reach its catalog. */
                if (!IsActiveWs(aSession->Id)) { continue; }

                PushInbound(std::move(message));
                message.clear();
            }

            if (closing) { break; }
        }
        }
        catch (const std::exception& e)
        {
            Addon::Log(LOGL_WARNING, "WS connection from %s ended: %s", aSession->Remote.c_str(), e.what());
        }
        catch (...)
        {
            Addon::Log(LOGL_WARNING, "WS connection from %s ended.", aSession->Remote.c_str());
        }

        /* Only the active client's departure is a real disconnect: a superseded one has already
           been replaced and must leave the catalog alone. Unregister before closing, so nothing
           can pick this session up and write to a dead handle. */
        {
            std::lock_guard<std::mutex> lock(g_socketMutex);
            g_liveSockets.erase(aSession->Id);
        }

        bool wasActive = false;
        {
            std::lock_guard<std::mutex> lock(g_clientMutex);
            if (g_activeWsId == aSession->Id)
            {
                g_activeWsId = 0;
                wasActive    = true;
            }
            g_wsSessions.erase(aSession->Id);
        }

        aSession->CloseSocket();

        if (wasActive) { MarkDisconnected(); }

        Addon::Log(LOGL_DEBUG, "WS from %s closed.", aSession->Remote.c_str());
    }

    bool IsWebSocketUpgrade(const HttpRequest& aRequest)
    {
        if (!Util::IEquals(aRequest.Method, "GET")) { return false; }
        if (!HeaderContainsToken(aRequest.Get("Upgrade"), "websocket")) { return false; }
        if (!HeaderContainsToken(aRequest.Get("Connection"), "upgrade")) { return false; }
        return !aRequest.Get("Sec-WebSocket-Key").empty();
    }

    ///------------------------------------------------------------------------------------------------
    /// Connection thread
    ///------------------------------------------------------------------------------------------------

    /* Returns true when the handler has already closed the socket. The WebSocket path owns its
       socket for the life of the session and closes it under the session's write mutex. */
    bool HandleConnection(SOCKET aSocket, uint64_t aConnId, std::string aRemote)
    {
        HttpRequest request;
        std::string leftover;

        ReadResult read = ReadHttpRequest(aSocket, request, leftover, g_running);
        if (read != ReadResult::Ok)
        {
            if (read == ReadResult::Malformed)
            {
                SendHttpResponse(aSocket, "400 Bad Request", "", nullptr, "");
            }
            return false;
        }

        std::string origin = request.Get("Origin");

        /* Logged before any branching and with the declared body size: on a Wine client this is the
           only trace left of a request that dies below us. */
        Addon::Log(LOGL_DEBUG, "%s %s len=%lu from %s", request.Method.c_str(), request.Target.c_str(),
                   (unsigned long)request.Body.size(), aRemote.c_str());

        /* An upgrade is honoured on any path. The website connects to "/". */
        if (IsWebSocketUpgrade(request))
        {
            if (!IsAllowedOrigin(origin))
            {
                /* CORS does not gate WebSockets, so the Origin is checked on the handshake itself. */
                Addon::Log(LOGL_WARNING, "Rejected WS handshake from %s (origin '%s').",
                           aRemote.c_str(), origin.c_str());
                SendHttpResponse(aSocket, "403 Forbidden", "", nullptr, "");
                return false;
            }

            std::string accept = Util::WebSocketAcceptKey(request.Get("Sec-WebSocket-Key"));

            std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                                   "Upgrade: websocket\r\n"
                                   "Connection: Upgrade\r\n"
                                   "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";

            if (!SendAll(aSocket, response.data(), response.size())) { return false; }

            Addon::Log(LOGL_INFO, "WS client connected from %s", aRemote.c_str());

            auto session = std::make_shared<WsSession>();
            session->Socket = aSocket;
            session->Id     = aConnId;
            session->Remote = aRemote;

            {
                std::lock_guard<std::mutex> lock(g_clientMutex);
                g_wsSessions[aConnId] = session;
            }

            TakeOver(aConnId, nullptr);
            RunWebSocket(session, std::move(leftover));
            return true;
        }

        std::string cors = BuildCorsHeaders(origin);

        if (Util::IEquals(request.Method, "OPTIONS"))
        {
            SendHttpResponse(aSocket, "204 No Content", cors, nullptr, "");
            return false;
        }

        if (Util::IEquals(request.Method, "POST") && request.Path == "/poll")
        {
            if (!IsAllowedOrigin(origin))
            {
                /* CORS only hides the response; this handler has side effects. */
                Addon::Log(LOGL_WARNING, "Rejected poll from %s (origin '%s').",
                           aRemote.c_str(), origin.c_str());
                SendHttpResponse(aSocket, "403 Forbidden", cors, nullptr, "");
                return false;
            }

            HandlePoll(aSocket, request, cors);
            return false;
        }

        /* A browser that gets this falls back to polling. */
        SendHttpResponse(aSocket, "426 Upgrade Required", cors, "text/plain; charset=utf-8", BODY_426);
        return false;
    }

    void ConnectionThread(SOCKET aSocket, uint64_t aConnId, std::string aRemote)
    {
        bool alreadyClosed = false;

        try
        {
            alreadyClosed = HandleConnection(aSocket, aConnId, aRemote);
        }
        catch (const std::exception& e)
        {
            /* A throw escaping a network thread would take Guild Wars 2 with it. */
            Addon::Log(LOGL_WARNING, "Error handling HTTP request: %s", e.what());
        }
        catch (...)
        {
            Addon::Log(LOGL_WARNING, "Error handling HTTP request.");
        }

        if (!alreadyClosed)
        {
            /* Unregister before closing: Stop() only ever touches sockets it can still see in the
               registry, and it holds the same mutex while it does. */
            {
                std::lock_guard<std::mutex> lock(g_socketMutex);
                g_liveSockets.erase(aConnId);
            }

            {
                std::lock_guard<std::mutex> lock(g_clientMutex);
                g_wsSessions.erase(aConnId);
            }

            shutdown(aSocket, SD_BOTH);
            closesocket(aSocket);
        }

        g_connThreads.fetch_sub(1);
    }

    ///------------------------------------------------------------------------------------------------
    /// Accept loop and maintenance
    ///------------------------------------------------------------------------------------------------

    void AcceptLoop()
    {
        while (g_running.load())
        {
            SOCKET listener = g_listener.load();
            if (listener == INVALID_SOCKET) { break; }

            sockaddr_in peer{};
            int         peerSize = sizeof(peer);

            SOCKET client = accept(listener, (sockaddr*)&peer, &peerSize);
            if (client == INVALID_SOCKET)
            {
                if (!g_running.load()) { break; }

                /* Never let one bad accept kill the server. */
                int error = WSAGetLastError();
                if (error == WSAEINTR || error == WSAENOTSOCK || error == WSAEINVAL) { break; }

                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }

            if (!g_running.load())
            {
                closesocket(client);
                break;
            }

            SetTimeouts(client);

            char remote[64];
            unsigned long address = ntohl(peer.sin_addr.s_addr);
            snprintf(remote, sizeof(remote), "%lu.%lu.%lu.%lu:%u",
                     (address >> 24) & 0xFF, (address >> 16) & 0xFF, (address >> 8) & 0xFF,
                     address & 0xFF, (unsigned)ntohs(peer.sin_port));

            uint64_t connId = g_nextConnId.fetch_add(1);

            {
                std::lock_guard<std::mutex> lock(g_socketMutex);
                g_liveSockets[connId] = client;
            }

            g_connThreads.fetch_add(1);

            try
            {
                std::thread(ConnectionThread, client, connId, std::string(remote)).detach();
            }
            catch (...)
            {
                g_connThreads.fetch_sub(1);
                {
                    std::lock_guard<std::mutex> lock(g_socketMutex);
                    g_liveSockets.erase(connId);
                }
                closesocket(client);
            }
        }
    }

    /* Reaps a poll session that has gone silent. A live client polls ~2x/second, so 20 s of nothing
       means it is gone. The value is deliberately high to ride out transient loopback stalls. */
    void MaintenanceLoop()
    {
        while (g_running.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!g_running.load()) { break; }

            bool wasActive = false;
            {
                std::lock_guard<std::mutex> lock(g_clientMutex);
                if (g_activePoll && NowMs() - g_activePoll->LastPollMs.load() > POLL_SESSION_TIMEOUT_MS)
                {
                    Addon::Log(LOGL_INFO, "Poll session timed out: %s", g_activePoll->Id.c_str());
                    g_activePoll->Superseded.store(true);
                    RememberSupersededPoll(g_activePoll->Id);
                    g_activePoll.reset();
                    wasActive = true;
                }
            }

            if (wasActive) { MarkDisconnected(); }
        }
    }
}

namespace Server
{
    bool Start(uint16_t aPort)
    {
        if (g_started.load()) { return true; }

        WSADATA wsa{};
        int startup = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (startup != 0)
        {
            Addon::Log(LOGL_CRITICAL, "WSAStartup failed (%d).", startup);
            return false;
        }

        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET)
        {
            Addon::Log(LOGL_CRITICAL, "Could not create the listening socket (%d).", WSAGetLastError());
            WSACleanup();
            return false;
        }

        /* Loopback only: whatever the origin allow-list admits, nothing off this machine can
           reach the port.

           No SO_REUSEADDR on purpose. On Windows it would let any other local process bind this
           port and hijack an unauthenticated control channel, and Windows rebinds over the
           TIME_WAIT connections the poll transport leaves behind without it. */
        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_port        = htons(aPort);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (bind(listener, (sockaddr*)&address, sizeof(address)) == SOCKET_ERROR)
        {
            Addon::Log(LOGL_CRITICAL, "Could not bind 127.0.0.1:%u (%d). Is another copy running?",
                       (unsigned)aPort, WSAGetLastError());
            closesocket(listener);
            WSACleanup();
            return false;
        }

        if (listen(listener, SOMAXCONN) == SOCKET_ERROR)
        {
            Addon::Log(LOGL_CRITICAL, "Could not listen on 127.0.0.1:%u (%d).",
                       (unsigned)aPort, WSAGetLastError());
            closesocket(listener);
            WSACleanup();
            return false;
        }

        g_listener.store(listener);
        g_running.store(true);
        g_started.store(true);

        try
        {
            g_acceptThread = std::thread(AcceptLoop);
            g_maintThread  = std::thread(MaintenanceLoop);
        }
        catch (...)
        {
            Addon::Log(LOGL_CRITICAL, "Could not start the server threads.");
            g_running.store(false);
            g_started.store(false);
            closesocket(g_listener.exchange(INVALID_SOCKET));
            if (g_acceptThread.joinable()) { g_acceptThread.join(); }
            WSACleanup();
            return false;
        }

        Addon::Log(LOGL_INFO, "Listening on 127.0.0.1:%u", (unsigned)aPort);
        return true;
    }

    void Stop()
    {
        if (!g_started.load()) { return; }

        g_running.store(false);

        /* Break the accept loop. */
        SOCKET listener = g_listener.exchange(INVALID_SOCKET);
        if (listener != INVALID_SOCKET)
        {
            closesocket(listener);
        }

        /* Unblock every connection thread: a half-shutdown makes their recv return immediately.
           The threads own their sockets and close them on the way out. */
        {
            std::lock_guard<std::mutex> lock(g_socketMutex);
            for (const auto& entry : g_liveSockets)
            {
                shutdown(entry.second, SD_BOTH);
            }
        }

        if (g_acceptThread.joinable()) { g_acceptThread.join(); }
        if (g_maintThread.joinable())  { g_maintThread.join(); }

        /* Connection threads are detached, so wait for them to drain before the DLL unloads.
           A thread still running our code after that is a crash. */
        for (int i = 0; i < 500 && g_connThreads.load() > 0; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (g_connThreads.load() > 0)
        {
            Addon::Log(LOGL_WARNING, "%d connection thread(s) still running at shutdown.",
                       g_connThreads.load());
        }

        {
            std::lock_guard<std::mutex> lock(g_clientMutex);
            g_activeWsId = 0;
            g_activePoll.reset();
            g_wsSessions.clear();
            g_supersededPolls.clear();
        }

        {
            std::lock_guard<std::mutex> lock(g_socketMutex);
            g_liveSockets.clear();
        }

        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            g_inbound.clear();
            g_events.clear();
        }

        ClearEgress();

        g_clientConnected.store(false);
        g_started.store(false);

        WSACleanup();
    }

    bool IsClientConnected()
    {
        return g_clientConnected.load();
    }

    void Send(const std::string& aJson)
    {
        if (!g_clientConnected.load() || aJson.empty()) { return; }

        std::lock_guard<std::mutex> lock(g_egressMutex);
        g_egress.push_back(aJson);

        /* A message the client never collects is lost. There is no retry. */
        while (g_egress.size() > MAX_EGRESS_DEPTH) { g_egress.pop_front(); }
    }

    std::vector<std::string> TakeInbound()
    {
        std::vector<std::string> out;

        std::lock_guard<std::mutex> lock(g_queueMutex);
        out.reserve(g_inbound.size());
        while (!g_inbound.empty())
        {
            out.push_back(std::move(g_inbound.front()));
            g_inbound.pop_front();
        }

        return out;
    }

    std::vector<Event> TakeEvents()
    {
        std::vector<Event> out;

        std::lock_guard<std::mutex> lock(g_queueMutex);
        out.reserve(g_events.size());
        while (!g_events.empty())
        {
            out.push_back(g_events.front());
            g_events.pop_front();
        }

        return out;
    }
}
