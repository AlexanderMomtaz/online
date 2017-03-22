/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef INCLUDED_LOOLWEBSOCKET_HPP
#define INCLUDED_LOOLWEBSOCKET_HPP

#include "config.h"

#include <cstdlib>
#include <mutex>
#include <thread>

#include <Poco/Net/WebSocket.h>

#include <Common.hpp>
#include <Protocol.hpp>
#include <Log.hpp>

/// WebSocket that is thread safe, and handles large frames transparently.
/// Careful - sendFrame and receiveFrame are _not_ virtual,
/// we need to make sure that we use LOOLWebSocket all over the place.
/// It would be a kind of more natural to encapsulate Poco::Net::WebSocket
/// instead of inheriting (from that reason,) but that would requite much
/// larger code changes.
class LOOLWebSocket : public Poco::Net::WebSocket
{
private:
    std::mutex _mutexRead;
    std::mutex _mutexWrite;

#if ENABLE_DEBUG
    static std::chrono::milliseconds getWebSocketDelay()
    {
        unsigned long baseDelay = 0;
        unsigned long jitter = 0;
        if (std::getenv("LOOL_WS_DELAY"))
        {
            baseDelay = std::stoul(std::getenv("LOOL_WS_DELAY"));
        }
        if (std::getenv("LOOL_WS_JITTER"))
        {
            jitter = std::stoul(std::getenv("LOOL_WS_JITTER"));
        }

        return std::chrono::milliseconds(baseDelay + (jitter > 0 ? (std::rand() % jitter) : 0));
    }

    void setMinSocketBufferSize()
    {
        // Lets set it to zero as system will automatically adjust it to minimum
        setSendBufferSize(0);
        LOG_INF("Send buffer size for web socket set to minimum: " << getSendBufferSize());
    }
#endif

public:
    LOOLWebSocket(const Socket& socket) :
        Poco::Net::WebSocket(socket)
    {
    }

    LOOLWebSocket(Poco::Net::HTTPServerRequest& request,
                  Poco::Net::HTTPServerResponse& response) :
        Poco::Net::WebSocket(request, response)
    {
#if ENABLE_DEBUG
        setMinSocketBufferSize();
#endif
    }

    LOOLWebSocket(Poco::Net::HTTPClientSession& cs,
                  Poco::Net::HTTPRequest& request,
                  Poco::Net::HTTPResponse& response) :
        Poco::Net::WebSocket(cs, request, response)
    {
#if ENABLE_DEBUG
        setMinSocketBufferSize();
#endif
    }

    LOOLWebSocket(Poco::Net::HTTPClientSession& cs,
                  Poco::Net::HTTPRequest& request,
                  Poco::Net::HTTPResponse& response,
                  Poco::Net::HTTPCredentials& credentials) :
        Poco::Net::WebSocket(cs, request, response, credentials)
    {
#if ENABLE_DEBUG
        setMinSocketBufferSize();
#endif
    }

    /// Wrapper for Poco::Net::WebSocket::receiveFrame() that handles PING frames
    /// (by replying with a PONG frame) and PONG frames. PONG frames are ignored.
    /// Should we also factor out the handling of non-final and continuation frames into this?
    int receiveFrame(char* buffer, const int length, int& flags)
    {
#if ENABLE_DEBUG
        // Delay receiving the frame
        std::this_thread::sleep_for(getWebSocketDelay());
#endif
        // Timeout given is in microseconds.
        static const Poco::Timespan waitTime(POLL_TIMEOUT_MS * 1000);

        while (poll(waitTime, Poco::Net::Socket::SELECT_READ))
        {
            std::unique_lock<std::mutex> lockRead(_mutexRead);
            const int n = Poco::Net::WebSocket::receiveFrame(buffer, length, flags);
            lockRead.unlock();

            if (n <= 0)
            {
                LOG_TRC("Got nothing (" << n << ")");
            }
            else
            {
                LOG_TRC("Got frame: " << LOOLProtocol::getAbbreviatedFrameDump(buffer, n, flags));
            }
            if ((flags & WebSocket::FRAME_OP_BITMASK) == WebSocket::FRAME_OP_PING)
            {
                // Echo back the ping message.
                std::unique_lock<std::mutex> lock(_mutexWrite);
                if (Poco::Net::WebSocket::sendFrame(buffer, n, static_cast<int>(WebSocket::FRAME_FLAG_FIN) | WebSocket::FRAME_OP_PONG) != n)
                {
                    LOG_WRN("Sending Pong failed.");
                    return -1;
                }
            }
            else if ((flags & WebSocket::FRAME_OP_BITMASK) == WebSocket::FRAME_OP_PONG)
            {
                // In case we do send pings in the future.
            }
            else
            {
                return n;
            }
        }

        // Not ready for read.
        return -1;
    }

    /// Wrapper for Poco::Net::WebSocket::sendFrame() that handles large frames.
    int sendFrame(const char* buffer, const int length, const int flags = FRAME_TEXT)
    {
#if ENABLE_DEBUG
        // Delay sending the frame
        std::this_thread::sleep_for(getWebSocketDelay());
#endif
        std::unique_lock<std::mutex> lock(_mutexWrite);

        if (length >= LARGE_MESSAGE_SIZE)
        {
            const std::string nextmessage = "nextmessage: size=" + std::to_string(length);
            const int size = nextmessage.size();

            if (Poco::Net::WebSocket::sendFrame(nextmessage.data(), size) == size)
            {
                LOG_TRC("Sent long message preample: " + nextmessage);
            }
            else
            {
                LOG_WRN("Failed to send long message preample.");
                return -1;
            }
        }

        const int result = Poco::Net::WebSocket::sendFrame(buffer, length, flags);

        lock.unlock();

        if (result != length)
        {
            LOG_ERR("Sent incomplete message, expected " << length << " bytes but sent " << result <<
                    " while sending: " << LOOLProtocol::getAbbreviatedMessage(buffer, length));
        }
        else
        {
            LOG_TRC("Sent frame: " << LOOLProtocol::getAbbreviatedMessage(buffer, length));
        }

        return result;
    }
};

#endif

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */