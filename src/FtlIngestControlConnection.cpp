/**
 * @file FtlIngestControlConnection.cpp
 * @author Hayden McAfee (hayden@outlook.com)
 * @date 2021-01-15
 * @copyright Copyright (c) 2021 Hayden McAfee
 */

#include "FtlIngestControlConnection.h"

#include "ConnectionTransports/ConnectionTransport.h"
#include "Utilities/Util.h"

#include <algorithm>
#include <fmt/core.h>
#include <openssl/hmac.h>
#include <spdlog/spdlog.h>

#pragma region Constructor/Destructor
FtlIngestControlConnection::FtlIngestControlConnection(
    std::unique_ptr<ConnectionTransport> transport,
    RequestKeyCallback onRequestKey,
    StartMediaPortCallback onStartMediaPort,
    ConnectionClosedCallback onConnectionClosed) : 
    transport(std::move(transport)),
    onRequestKey(onRequestKey),
    onStartMediaPort(onStartMediaPort),
    onConnectionClosed(onConnectionClosed)
{ 
    // Bind to transport events
    transport->SetOnBytesReceived(std::bind(
        &FtlIngestControlConnection::onTransportBytesReceived, this, std::placeholders::_1));
    transport->SetOnConnectionClosed(std::bind(
        &FtlIngestControlConnection::onTransportClosed, this));
}
#pragma endregion Constructor/Destructor

#pragma region Public functions
Result<void> FtlIngestControlConnection::StartAsync()
{
    return transport->StartAsync();
}

void FtlIngestControlConnection::Stop()
{
    // TODO
}
#pragma endregion Public functions

#pragma region Private functions
void FtlIngestControlConnection::onTransportBytesReceived(const std::vector<std::byte>& bytes)
{
    // Tack the new bytes onto the end of our running buffer
    commandBuffer.reserve(commandBuffer.size() + bytes.size());
    commandBuffer.insert(commandBuffer.end(), reinterpret_cast<const char*>(bytes.data()),
        (reinterpret_cast<const char*>(bytes.data()) + bytes.size()));

    // Check to see if we've read a complete command
    // (we only search backwards a little bit, since we've presumably already searched through
    // the previous payloads)
    int startDelimiterSearchIndex = std::max(0,
        static_cast<int>(commandBuffer.size() - bytes.size() - delimiterSequence.size()));
    int delimiterCharactersRead = 0;
    for (int i = startDelimiterSearchIndex; i < commandBuffer.size(); ++i)
    {
        if (commandBuffer.at(i) == delimiterSequence.at(delimiterCharactersRead))
        {
            ++delimiterCharactersRead;
            if (delimiterCharactersRead >= delimiterSequence.size())
            {
                // We've read a command, split it into its own string (minus the delimiter seq)
                std::string command(commandBuffer.begin(),
                    (commandBuffer.begin() + i + 1 - delimiterSequence.size()));

                // Delete the processed portion of the buffer (including the delimiter seq)
                commandBuffer.erase(commandBuffer.begin(), (commandBuffer.begin() + i + 1));
                delimiterCharactersRead = 0;
                processCommand(command);
            }
        }
        else
        {
            delimiterCharactersRead = 0;
        }
    }
}

void FtlIngestControlConnection::onTransportClosed()
{
    // TODO
}

void FtlIngestControlConnection::writeToTransport(const std::string& str)
{
    std::vector<std::byte> writeBytes;
    writeBytes.reserve(str.size());
    for (const char& c : str)
    {
        writeBytes.push_back(static_cast<std::byte>(c));
    }
    transport->Write(writeBytes);
}

void FtlIngestControlConnection::processCommand(const std::string& command)
{
    if (command.compare("HMAC") == 0)
    {
        processHmacCommand();
    }
    else if (command.substr(0,7).compare("CONNECT") == 0)
    {
        processConnectCommand(command);
    }
    else if (std::regex_match(command, attributePattern))
    {
        processAttributeCommand(command);
    }
    else if (command.compare(".") == 0)
    {
        processDotCommand();
    }
    else if (command.substr(0,4).compare("PING") == 0)
    {
        processPingCommand();
    }
    else
    {
        spdlog::warn("Unknown ingest command: {}", command);
    }
}

void FtlIngestControlConnection::processHmacCommand()
{
    // Calculate a new random HMAC payload, then send it.
    // We'll need to send it out as a string of hex bytes (00 - ff)
    hmacPayload = Util::GenerateRandomBinaryPayload(HMAC_PAYLOAD_SIZE);
    std::string hmacString = Util::ByteArrayToHexString(
        reinterpret_cast<std::byte*>(&hmacPayload[0]), hmacPayload.size());
    writeToTransport(fmt::format("200 {}\n", hmacString));
}

void FtlIngestControlConnection::processConnectCommand(const std::string& command)
{
    std::smatch matches;

    if (std::regex_search(command, matches, connectPattern) &&
        (matches.size() >= 3))
    {
        std::string channelIdStr = matches[1].str();
        std::string hmacHashStr = matches[2].str();

        uint32_t requestedChannelId = static_cast<uint32_t>(std::stoul(channelIdStr));
        std::vector<std::byte> hmacHash = Util::HexStringToByteArray(hmacHashStr);

        // Try to fetch the key for this channel
        Result<std::vector<std::byte>> keyResult = onRequestKey(requestedChannelId);
        if (keyResult.IsError)
        {
            // Couldn't look up the key, so let's close the connection
            spdlog::warn("Couldn't look up HMAC key for channel {}: {}", requestedChannelId,
                keyResult.ErrorMessage);
            stopConnection();
            return;
        }
        std::vector<std::byte> key = keyResult.Value;

        std::byte buffer[512];
        uint32_t bufferLength;
        HMAC(EVP_sha512(), reinterpret_cast<const unsigned char*>(key.data()), key.size(),
            reinterpret_cast<const unsigned char*>(hmacPayload.data()), hmacPayload.size(),
            reinterpret_cast<unsigned char*>(buffer), &bufferLength);

        // Do the hashed values match?
        bool match = true;
        if (bufferLength != hmacHash.size())
        {
            match = false;
        }
        else
        {
            for (unsigned int i = 0; i < bufferLength; ++i)
            {
                if (hmacHash.at(i) != buffer[i])
                {
                    match = false;
                    break;
                }
            }
        }

        if (match)
        {
            isAuthenticated = true;
            channelId = requestedChannelId;
            writeToTransport("200\n");
        }
        else
        {
            spdlog::info("Client provided invalid HMAC hash for channel {}, disconnecting...",
                requestedChannelId);
            stopConnection();
            return;
        }
    }
    else
    {
        // TODO: Handle error, disconnect client
        spdlog::info("Malformed CONNECT request, disconnecting: {}", command);
        stopConnection();
        return;
    }
}

void FtlIngestControlConnection::processAttributeCommand(const std::string& command)
{
    if (!isAuthenticated)
    {
        spdlog::info("Client attempted to send attributes before auth. Disconnecting...");
        stopConnection();
        return;
    }
    if (isStreaming)
    {
        
        spdlog::info("Client attempted to send attributes after stream started. Disconnecting...");
        stopConnection();
        return;
    }

    std::smatch matches;

    if (std::regex_match(command, matches, attributePattern) &&
        matches.size() >= 3)
    {
        std::string key = matches[1].str();
        std::string value = matches[2].str();

        if (key.compare("VendorName") == 0)
        {
            mediaMetadata.VendorName = value;
        }
        else if (key.compare("VendorVersion") == 0)
        {
            mediaMetadata.VendorVersion = value;
        }
        else if (key.compare("Video") == 0)
        {
            mediaMetadata.HasVideo = (value.compare("true") == 0);
        }
        else if (key.compare("Audio") == 0)
        {
            mediaMetadata.HasAudio = (value.compare("true") == 0);
        }
        else if (key.compare("VideoCodec") == 0)
        {
            mediaMetadata.VideoCodec = SupportedVideoCodecs::ParseVideoCodec(value);
        }
        else if (key.compare("AudioCodec") == 0)
        {
            mediaMetadata.AudioCodec = SupportedAudioCodecs::ParseAudioCodec(value);
        }
        else if (key.compare("VideoWidth") == 0)
        {
            try
            {
                mediaMetadata.VideoWidth = std::stoul(value);
            }
            catch(const std::exception& e)
            {
                spdlog::warn("Client provided invalid video width value: {}", value);
            }
        }
        else if (key.compare("VideoHeight") == 0)
        {
            try
            {
                mediaMetadata.VideoHeight = std::stoul(value);
            }
            catch(const std::exception& e)
            {
                spdlog::warn("Client provided invalid video height value: {}", value);
            }
        }
        else if (key.compare("VideoIngestSSRC") == 0)
        {
            try
            {
                mediaMetadata.VideoSsrc = std::stoul(value);
            }
            catch(const std::exception& e)
            {
                spdlog::warn("Client provided invalid video ssrc value: {}", value);
            }
        }
        else if (key.compare("AudioIngestSSRC") == 0)
        {
            try
            {
                mediaMetadata.AudioSsrc = std::stoul(value);
            }
            catch(const std::exception& e)
            {
                spdlog::warn("Client provided invalid audio ssrc value: {}", value);
            }
        }
        else if (key.compare("VideoPayloadType") == 0)
        {
            try
            {
                mediaMetadata.VideoPayloadType = std::stoul(value);
            }
            catch(const std::exception& e)
            {
                spdlog::warn("Client provided invalid video payload type value: {}", value);
            }
        }
        else if (key.compare("AudioPayloadType") == 0)
        {
            try
            {
                mediaMetadata.AudioPayloadType = std::stoul(value);
            }
            catch(const std::exception& e)
            {
                spdlog::warn("Client provided invalid audio payload type value: {}", value);
            }
        }
        else
        {
            spdlog::warn("Received unrecognized attribute from client: {}: {}", key, value);
        }
    }
    else
    {
        spdlog::warn("Received malformed attribute command from client: {}", command);
    }
}

void FtlIngestControlConnection::processDotCommand()
{
    // Validate our state before we fire up a stream
    if (!isAuthenticated)
    {
        spdlog::warn("Client attempted to start stream without valid authentication.");
        stopConnection();
        return;
    }
    else if (!mediaMetadata.HasAudio && !mediaMetadata.HasVideo)
    {
        spdlog::warn(
            "Client attempted to start stream without HasAudio and HasVideo attributes set.");
        stopConnection();
        return;
    }
    else if (mediaMetadata.HasAudio && 
        (mediaMetadata.AudioPayloadType == 0 || 
        mediaMetadata.AudioSsrc == 0 || 
        mediaMetadata.AudioCodec == AudioCodecKind::Unsupported))
    {
        spdlog::warn("Client attempted to start audio stream without valid AudioPayloadType/"
            "AudioIngestSSRC/AudioCodec.");
        stopConnection();
        return;
    }
    else if (mediaMetadata.HasVideo && 
        (mediaMetadata.VideoPayloadType == 0 ||
        mediaMetadata.VideoSsrc == 0 ||
        mediaMetadata.VideoCodec == VideoCodecKind::Unsupported))
    {
        spdlog::warn("Client attempted to start video stream without valid VideoPayloadType/"
            "VideoIngestSSRC/VideoCodec.");
        stopConnection();
        return;
    }

    Result<uint16_t> mediaPortResult = onStartMediaPort(mediaMetadata);
    if (mediaPortResult.IsError)
    {
        spdlog::error("Could not assign media port for FTL connection.");
        stopConnection();
        return;
    }
    uint16_t mediaPort = mediaPortResult.Value;
    isStreaming = true;
    writeToTransport(fmt::format("200 hi. Use UDP port {}\n", mediaPort));
}

void FtlIngestControlConnection::processPingCommand()
{
    // TODO: Rate limit this.
    writeToTransport("201\n");
}
#pragma endregion Private functions