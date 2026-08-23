///----------------------------------------------------------------------------------------------------
/// Server.h: the localhost transport the website connects to.
///
/// The module is the server. It listens on 38473 and speaks JSON over a WebSocket, falling back
/// to HTTP polling. Wire format: ../gw2.app-blishhud/docs/protocol.md.
///
/// Everything here runs on the server's own threads and reaches the render thread only through
/// the queues below. Touching ImGui, textures or UI state from those threads is a data race.
///----------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Server
{
    enum class Event
    {
        Connected,        /* a client finished the handshake */
        ConnectionLost,   /* the client went away: drop the catalog and close the windows */
        ClientReplaced    /* a new client took over: keep the catalog */
    };

    /* Returns false when the port is already taken. */
    bool Start(uint16_t aPort);
    void Stop();

    bool IsClientConnected();

    /* Queues a JSON message for the active client. No-op when disconnected. Thread-safe. */
    void Send(const std::string& aJson);

    /* Render thread only. */
    std::vector<std::string> TakeInbound();
    std::vector<Event>       TakeEvents();
}
