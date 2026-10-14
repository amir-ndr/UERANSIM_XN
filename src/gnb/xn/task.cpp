//
// This file is a part of UERANSIM project.
// Copyright (c) 2023 ALİ GÜNGÖR.
//
// https://github.com/aligungr/UERANSIM/
// See README, LICENSE, and CONTRIBUTING files for licensing details.
//

#include "task.hpp"

#include <gnb/ngap/task.hpp>
#include <gnb/nts.hpp>
#include <gnb/types.hpp>
#include <utils/common.hpp>
#include <utils/logger.hpp>

#include <chrono>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nr::gnb
{

// ── Wire helpers ──────────────────────────────────────────────────────────────

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

static std::string RecvMsg(int fd)
{
    uint32_t nlen = 0;
    if (!RecvAll(fd, reinterpret_cast<char *>(&nlen), 4))
        return {};
    uint32_t len = ntohl(nlen);
    if (len == 0 || len > 1024 * 1024)
        return {};
    std::string buf(len, '\0');
    if (!RecvAll(fd, buf.data(), len))
        return {};
    return buf;
}

static bool SendMsg(int fd, const std::string &msg)
{
    uint32_t nlen = htonl(static_cast<uint32_t>(msg.size()));
    if (::send(fd, &nlen, 4, MSG_NOSIGNAL) != 4)
        return false;
    if (::send(fd, msg.data(), msg.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(msg.size()))
        return false;
    return true;
}

// ── Minimal JSON field extractor ──────────────────────────────────────────────

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

// Parses the "psi,upTeid,upAddrHex,qfi1+qfi2|psi2,..." session list built by
// NgapTask::triggerXnHandover on the source gNB.
static std::vector<NmGnbXnToNgap::SessionInfo> ParseSessions(const std::string &sessStr)
{
    std::vector<NmGnbXnToNgap::SessionInfo> sessions;

    std::string remaining = sessStr;
    while (!remaining.empty())
    {
        std::string entry;
        auto bar = remaining.find('|');
        if (bar != std::string::npos)
        {
            entry     = remaining.substr(0, bar);
            remaining = remaining.substr(bar + 1);
        }
        else
        {
            entry = remaining;
            remaining.clear();
        }

        size_t p0 = 0, p1 = entry.find(',', p0);
        if (p1 == std::string::npos) continue;
        NmGnbXnToNgap::SessionInfo si;
        si.psi = std::stoi(entry.substr(p0, p1 - p0));

        p0 = p1 + 1; p1 = entry.find(',', p0);
        if (p1 == std::string::npos) continue;
        si.upTeid = static_cast<uint32_t>(std::stoul(entry.substr(p0, p1 - p0)));

        p0 = p1 + 1; p1 = entry.find(',', p0);
        if (p1 == std::string::npos) continue;
        std::string addrHex = entry.substr(p0, p1 - p0);

        std::vector<uint8_t> addrBytes;
        for (size_t i = 0; i + 1 < addrHex.size(); i += 2)
            addrBytes.push_back(static_cast<uint8_t>(std::stoul(addrHex.substr(i, 2), nullptr, 16)));
        si.upAddr = OctetString(std::move(addrBytes));

        p0 = p1 + 1;
        std::string qfisStr = entry.substr(p0);
        size_t q0 = 0;
        while (q0 <= qfisStr.size())
        {
            auto q1 = qfisStr.find('+', q0);
            std::string tok = (q1 != std::string::npos) ? qfisStr.substr(q0, q1 - q0) : qfisStr.substr(q0);
            if (!tok.empty())
                si.qfis.push_back(std::stol(tok));
            if (q1 == std::string::npos) break;
            q0 = q1 + 1;
        }

        sessions.push_back(std::move(si));
    }

    return sessions;
}

// ── Dispatcher contact ────────────────────────────────────────────────────────

static std::string ContactDispatcher(const std::string &host, uint16_t port,
                                      const std::string &payload)
{
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return {};

    struct timeval tv{5, 0};
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0 ||
        ::connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        ::close(s);
        return {};
    }

    SendMsg(s, payload);
    std::string resp = RecvMsg(s);
    ::close(s);
    return resp;
}

// ── XnTask ────────────────────────────────────────────────────────────────────

XnTask::XnTask(TaskBase *base) : m_base(base)
{
    m_logger = m_base->logBase->makeUniqueLogger("xn");
}

XnTask::~XnTask()
{
    stop();
}

void XnTask::start()
{
    if (m_base->config->xnAddress.empty())
        return;

    m_running = true;
    m_thread = std::thread(&XnTask::listenerLoop, this);
}

void XnTask::stop()
{
    m_running = false;
    if (m_serverFd >= 0)
    {
        ::shutdown(m_serverFd, SHUT_RDWR);
        ::close(m_serverFd);
        m_serverFd = -1;
    }
    if (m_thread.joinable())
        m_thread.join();
}

void XnTask::listenerLoop()
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        m_logger->err("Xn server: socket() failed: %s", std::strerror(errno));
        m_running = false;
        return;
    }

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_base->config->xnPort);
    if (::inet_pton(AF_INET, m_base->config->xnAddress.c_str(), &addr.sin_addr) <= 0)
    {
        m_logger->err("Xn server: invalid xnAddress: %s", m_base->config->xnAddress.c_str());
        ::close(fd);
        m_running = false;
        return;
    }

    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        m_logger->err("Xn server: bind() failed on %s:%d — %s",
                      m_base->config->xnAddress.c_str(), m_base->config->xnPort, std::strerror(errno));
        ::close(fd);
        m_running = false;
        return;
    }

    ::listen(fd, 10);
    m_serverFd = fd;

    m_logger->info("Xn server listening on %s:%d",
                   m_base->config->xnAddress.c_str(), m_base->config->xnPort);

    while (m_running)
    {
        int connFd = ::accept(m_serverFd, nullptr, nullptr);
        if (connFd < 0)
            break;
        handleConnection(connFd);
    }
}

void XnTask::handleConnection(int connFd)
{
    struct timeval tv{5, 0};
    ::setsockopt(connFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(connFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::string msg = RecvMsg(connFd);
    if (msg.empty())
    {
        ::close(connFd);
        return;
    }

    m_logger->debug("Xn: received: %s", msg.c_str());

    int64_t requestedUs = utils::CurrentTimeMicros();

    std::string ueIdStr  = JsonGetString(msg, "ueId");
    std::string amfUeStr = JsonGetString(msg, "amfUeNgapId");
    std::string ranUeStr = JsonGetString(msg, "ranUeNgapId");
    std::string sstStr   = JsonGetString(msg, "sliceSst");
    std::string srcGnb   = JsonGetString(msg, "sourceGnb");
    std::string stiStr   = JsonGetString(msg, "sti");
    std::string sessStr  = JsonGetString(msg, "sessions");

    if (ueIdStr.empty())  ueIdStr  = "0";
    if (amfUeStr.empty()) amfUeStr = "0";
    if (ranUeStr.empty()) ranUeStr = "0";
    if (sstStr.empty())   sstStr   = "1";

    int sst = std::stoi(sstStr);
    uint64_t sti = stiStr.empty() ? 0 : std::stoull(stiStr);

    auto reject = [this, connFd](const char *reason) {
        m_logger->err("[xn-ho] TARGET: rejecting XnHandoverRequest — %s", reason);
        SendMsg(connFd, std::string{"{\"type\":\"XnHandoverAck\",\"status\":\"REJECT\",\"reason\":\""} + reason + "\"}");
        ::close(connFd);
    };

    // The STI is what lets us recognise this UE when it shows up on our radio
    // side; without it the handover cannot be correlated at all.
    if (sti == 0)
        return reject("missing UE sti");

    auto sessions = ParseSessions(sessStr);
    if (sessions.empty())
        return reject("no PDU sessions in request");

    int64_t amfUeNgapId = std::stoll(amfUeStr);
    if (amfUeNgapId == 0)
        return reject("missing amfUeNgapId");

    m_logger->info("[xn-ho] TARGET: XnHandoverRequest from %s | sti=0x%llx sst=%d sessions=%d",
                   srcGnb.c_str(), (unsigned long long)sti, sst, static_cast<int>(sessions.size()));

    // Optional NF-chain selection step. Purely informational for the handover
    // itself; the AMF used is whichever one this gNB is already associated with.
    double pswMs = 0.0;
    std::string selectedAmf = "127.0.0.5";

    if (!m_base->config->dispatcherAddress.empty())
    {
        std::string pswPayload =
            "{\"type\":\"PathSwitchRequest\""
            ",\"ueId\":" + ueIdStr +
            ",\"amfUeNgapId\":" + amfUeStr +
            ",\"ranUeNgapId\":" + ranUeStr +
            ",\"sliceSst\":" + sstStr +
            ",\"targetGnb\":\"" + m_base->config->xnAddress + "\""
            ",\"sourceGnb\":\"" + srcGnb + "\""
            "}";

        auto t0 = std::chrono::steady_clock::now();
        std::string pswResp = ContactDispatcher(m_base->config->dispatcherAddress,
                                                 m_base->config->dispatcherPort, pswPayload);
        pswMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();

        if (pswResp.empty())
            m_logger->err("Xn: dispatcher unreachable at %s:%d",
                          m_base->config->dispatcherAddress.c_str(), m_base->config->dispatcherPort);
        else
        {
            m_logger->debug("Xn: dispatcher reply (%.1fms): %s", pswMs, pswResp.c_str());
            std::string amf = JsonGetString(pswResp, "selectedAmf");
            if (!amf.empty())
                selectedAmf = amf;
        }
    }

    // Hold the UE context until the UE itself completes the move to us. The
    // NGAP path switch is deliberately NOT sent yet: doing it here would build
    // a context for a UE that has not arrived and may never arrive.
    {
        XnPendingHandover pending{};
        pending.amfUeNgapId = amfUeNgapId;
        pending.sessions    = std::move(sessions);
        pending.sliceSst    = sst;
        pending.requestedUs = requestedUs;

        std::lock_guard<std::mutex> lock{m_pendingMutex};
        m_pending[sti] = std::move(pending);
    }

    char pswBuf[32];
    std::snprintf(pswBuf, sizeof(pswBuf), "%.1f", pswMs);

    // targetNci tells the source which cell to point the UE at.
    char nciBuf[32];
    std::snprintf(nciBuf, sizeof(nciBuf), "%lld", (long long)m_base->config->nci);

    std::string ack =
        "{\"type\":\"XnHandoverAck\""
        ",\"ueId\":" + ueIdStr +
        ",\"sliceSst\":" + sstStr +
        ",\"status\":\"OK\""
        ",\"targetCell\":\"" + m_base->config->name + "\""
        ",\"targetNci\":" + nciBuf +
        ",\"selectedAmf\":\"" + selectedAmf + "\""
        ",\"pswLatencyMs\":" + pswBuf +
        "}";

    SendMsg(connFd, ack);
    ::close(connFd);

    m_logger->info("[xn-ho] TARGET: XnHandoverAck sent, awaiting UE arrival | nci=0x%llx",
                   (unsigned long long)m_base->config->nci);
}

bool XnTask::takePendingHandover(uint64_t sti, XnPendingHandover &out)
{
    std::lock_guard<std::mutex> lock{m_pendingMutex};

    auto it = m_pending.find(sti);
    if (it == m_pending.end())
        return false;

    out = std::move(it->second);
    m_pending.erase(it);
    return true;
}

} // namespace nr::gnb
