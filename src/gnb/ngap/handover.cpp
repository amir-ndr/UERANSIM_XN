//
// Xn handover extension for UERANSIM.
// Copyright (c) 2026 Amir Naderi.
//
// Part of a modified version of UERANSIM by Ali Güngör
// (https://github.com/aligungr/UERANSIM/), distributed under the same
// license (AGPL-3.0). See README.md and LICENSE for details.
//
// Source side: Xn handover preparation and handover trigger.
//

#include "task.hpp"

#include <gnb/gtp/task.hpp>
#include <gnb/rls/task.hpp>
#include <gnb/rrc/task.hpp>

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <utils/common.hpp>
#include <utils/logger.hpp>

namespace nr::gnb
{

// Receive exactly n bytes.
static bool RecvAll(int fd, char *buf, size_t n)
{
    size_t got = 0;
    while (got < n)
    {
        ssize_t r = ::recv(fd, buf + got, n - got, 0);
        if (r <= 0)
            return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

// Receive one length-prefixed JSON message.
static std::string RecvMsg(int fd)
{
    uint32_t nlen = 0;
    if (!RecvAll(fd, reinterpret_cast<char *>(&nlen), 4))
        return {};
    uint32_t len = ntohl(nlen);
    if (len == 0 || len > 65536)
        return {};
    std::string buf(len, '\0');
    if (!RecvAll(fd, buf.data(), len))
        return {};
    return buf;
}

// Extracts a top-level JSON field. Mirrors the extractor in xn/task.cpp; the
// Xn protocol here is deliberately a small hand-rolled JSON, not XnAP.
static std::string JsonGetString(const std::string &json, const std::string &key)
{
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos)
        return {};
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos)
        return {};
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos)
        return {};
    if (json[pos] == '"')
    {
        size_t end = json.find('"', pos + 1);
        if (end == std::string::npos)
            return {};
        return json.substr(pos + 1, end - pos - 1);
    }
    size_t end = json.find_first_of(",}", pos);
    return json.substr(pos, end != std::string::npos ? end - pos : std::string::npos);
}

// Send one length-prefixed JSON message.
static bool SendMsg(int fd, const std::string &msg)
{
    uint32_t nlen = htonl(static_cast<uint32_t>(msg.size()));
    if (::send(fd, &nlen, 4, MSG_NOSIGNAL) != 4)
        return false;
    if (::send(fd, msg.data(), msg.size(), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(msg.size()))
        return false;
    return true;
}

// Open a TCP connection to gnb2 and send the XnHandoverRequest.
// Returns the connected socket fd, or -1 on error.
static int XnConnect(const std::string &host, uint16_t port, const std::string &request)
{
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    // Long timeout: must cover gnb2's dispatcher round-trip + UE handoff
    struct timeval tv{30, 0};
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0 ||
        ::connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        ::close(sock);
        return -1;
    }

    if (!SendMsg(sock, request))
    {
        ::close(sock);
        return -1;
    }

    return sock;
}

void NgapTask::triggerXnHandover(int ueId, const std::string &targetGnbAddress,
                                  uint16_t targetGnbPort, int sliceSst)
{
    if (m_ueCtx.count(ueId) == 0)
    {
        m_logger->err("Xn handover: UE %d not found", ueId);
        return;
    }

    auto *ue        = m_ueCtx[ueId];
    int64_t amfUeNgapId = ue->amfUeNgapId;
    int64_t ranUeNgapId = ue->ranUeNgapId;

    // The UE's STI is the only identity the target gNB can use to recognise
    // this UE when it arrives on its radio side.
    uint64_t sti = m_base->rlsTask->stiOf(ueId);
    if (sti == 0)
    {
        m_logger->err("[xn-ho] SOURCE: UE[%d] has no RLS session, cannot hand it over", ueId);
        return;
    }

    uint64_t t1 = utils::CurrentTimeMillis();

    // ── Build sessions string for the target's PathSwitchRequest ────────────
    // Format: "psi,upTeid,upAddrHex,qfi1+qfi2|psi2,..."
    std::string sessionsStr;
    for (auto &[psi, xnInfo] : ue->xnSessionInfo)
    {
        if (!sessionsStr.empty()) sessionsStr += "|";

        char hexBuf[3];
        std::string addrHex;
        for (int i = 0; i < xnInfo.upAddr.length(); i++)
        {
            std::snprintf(hexBuf, sizeof(hexBuf), "%02x",
                          static_cast<unsigned>(xnInfo.upAddr.data()[i]));
            addrHex += hexBuf;
        }

        std::string qfisStr;
        for (size_t i = 0; i < xnInfo.qfis.size(); i++)
        {
            if (i) qfisStr += "+";
            qfisStr += std::to_string(xnInfo.qfis[i]);
        }
        if (qfisStr.empty()) qfisStr = "1";

        sessionsStr += std::to_string(psi) + "," +
                       std::to_string(xnInfo.upTeid) + "," +
                       addrHex + "," + qfisStr;
    }

    // ── Step 1: Xn Preparation ───────────────────────────────────────────────
    // Hand the UE context to the target. It holds it until the UE itself
    // arrives; nothing is committed towards the AMF yet.
    m_logger->info("[xn-ho] SOURCE: handover requested ue=%d target=%s:%d sti=0x%llx",
                   ueId, targetGnbAddress.c_str(), targetGnbPort, (unsigned long long)sti);

    std::string xnPayload =
        "{\"type\":\"XnHandoverRequest\""
        ",\"ueId\":"          + std::to_string(ueId) +
        ",\"sti\":"           + std::to_string(sti) +
        ",\"amfUeNgapId\":"   + std::to_string(amfUeNgapId) +
        ",\"ranUeNgapId\":"   + std::to_string(ranUeNgapId) +
        ",\"sliceSst\":"      + std::to_string(sliceSst) +
        ",\"sourceGnb\":\""   + m_base->config->xnAddress + "\"" +
        ",\"sessions\":\""    + sessionsStr + "\""
        "}";

    int xnSock = XnConnect(targetGnbAddress, targetGnbPort, xnPayload);
    if (xnSock < 0)
    {
        m_logger->err("[xn-ho] SOURCE: preparation failed — target gNB unreachable at %s:%d",
                      targetGnbAddress.c_str(), targetGnbPort);
        return;
    }

    std::string xnAck = RecvMsg(xnSock);
    ::close(xnSock);

    if (xnAck.empty())
    {
        m_logger->err("[xn-ho] SOURCE: no XnHandoverAck received, handover aborted");
        return;
    }

    m_logger->debug("Xn: HandoverAck received: %s", xnAck.c_str());

    if (JsonGetString(xnAck, "status") != "OK")
    {
        m_logger->err("[xn-ho] SOURCE: target rejected the handover (%s), UE stays here",
                      JsonGetString(xnAck, "reason").c_str());
        return;
    }

    std::string targetNciStr = JsonGetString(xnAck, "targetNci");
    if (targetNciStr.empty())
    {
        m_logger->err("[xn-ho] SOURCE: XnHandoverAck carries no targetNci, handover aborted");
        return;
    }
    int64_t targetNci = std::stoll(targetNciStr);

    uint64_t t2 = utils::CurrentTimeMillis();
    m_logger->info("[xn-ho] SOURCE: XnHandoverAck ok in %llums, target nci=0x%llx",
                   (unsigned long long)(t2 - t1), (unsigned long long)targetNci);

    // ── Step 2: Order the UE to move ─────────────────────────────────────────
    // The RRC task builds and sends the RRCReconfiguration (reconfigurationWithSync)
    // and then drops its context WITHOUT an RRC Release, so the UE moves while
    // staying RRC-CONNECTED. This is queued rather than done inline: the CLI
    // handler has all tasks paused, so it is delivered once we return.
    {
        auto w = std::make_unique<NmGnbNgapToRrc>(NmGnbNgapToRrc::XN_HANDOVER_COMMAND);
        w->ueId = ueId;
        w->targetNci = targetNci;
        m_base->rrcTask->push(std::move(w));
    }

    // ── Step 3: Release our own context ──────────────────────────────────────
    // We deliberately do not wait for the target to confirm the UE arrived:
    // this runs with RRC and RLS paused, so the handover command above could
    // never be delivered while we block. The target owns the rest of the
    // procedure (path switch) from here.
    {
        auto w = std::make_unique<NmGnbNgapToGtp>(NmGnbNgapToGtp::UE_CONTEXT_RELEASE);
        w->ueId = ueId;
        m_base->gtpTask->push(std::move(w));

        deleteUeContext(ueId);
    }

    m_logger->info("[xn-ho] SOURCE: context released locally (no RRC Release sent) | ue=%d sst=%d",
                   ueId, sliceSst);
}

} // namespace nr::gnb
