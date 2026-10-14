//
// This file is a part of UERANSIM project.
// Copyright (c) 2023 ALİ GÜNGÖR.
//
// https://github.com/aligungr/UERANSIM/
// See README, LICENSE, and CONTRIBUTING files for licensing details.
//

#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <gnb/types.hpp>
#include <lib/rls/rls_pdu.hpp>
#include <lib/udp/server.hpp>
#include <utils/nts.hpp>

namespace nr::gnb
{

class RlsUdpTask : public NtsTask
{
  private:
    struct UeInfo
    {
        uint64_t sti{};
        InetAddress address;
        int64_t lastSeen{};
    };

  private:
    std::unique_ptr<Logger> m_logger;
    udp::UdpServer *m_server;
    NtsTask *m_ctlTask;
    uint64_t m_sti;
    Vector3 m_phyLocation;
    int64_t m_lastLoop;
    std::unordered_map<uint64_t, int> m_stiToUe;
    std::unordered_map<int, UeInfo> m_ueMap;
    int m_newIdCounter;

    // Guards m_stiToUe/m_ueMap. They are mutated on this task's thread, but
    // stiOf() is called from the NGAP and RRC task threads during Xn handover.
    std::mutex m_ueMapMutex;

  public:
    explicit RlsUdpTask(TaskBase *base, uint64_t sti, Vector3 phyLocation);
    ~RlsUdpTask() override = default;

  protected:
    void onStart() override;
    void onLoop() override;
    void onQuit() override;

  private:
    void receiveRlsPdu(const InetAddress &addr, std::unique_ptr<rls::RlsMessage> &&msg);
    void sendRlsPdu(const InetAddress &addr, const rls::RlsMessage &msg);
    void heartbeatCycle(int64_t time);

  public:
    void initialize(NtsTask *ctlTask);
    void send(int ueId, const rls::RlsMessage &msg);

    // Returns the UE-generated STI for a local RLS ueId, or 0 if unknown.
    // The STI is the only UE identity shared across gNBs, so it is what
    // correlates a UE between the source and target of an Xn handover.
    uint64_t stiOf(int ueId);
};

} // namespace nr::gnb
