//
// This file is a part of UERANSIM project.
// Copyright (c) 2023 ALİ GÜNGÖR.
//
// https://github.com/aligungr/UERANSIM/
// See README, LICENSE, and CONTRIBUTING files for licensing details.
//

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <gnb/nts.hpp>
#include <utils/logger.hpp>

namespace nr::gnb
{

struct TaskBase;

// UE context handed over from the source gNB, held until the UE itself shows up
// on the radio side and completes the reconfiguration. Keyed by the UE's STI,
// the only identity shared between the two gNBs.
struct XnPendingHandover
{
    int64_t amfUeNgapId{};
    std::vector<NmGnbXnToNgap::SessionInfo> sessions;
    int sliceSst{};
    int64_t requestedUs{};
};

class XnTask
{
  public:
    explicit XnTask(TaskBase *base);
    ~XnTask();

    void start();
    void stop();

    // Called by GnbRrcTask when a handed-over UE completes its move to us.
    // Returns false if no handover was pending for this STI.
    bool takePendingHandover(uint64_t sti, XnPendingHandover &out);

  private:
    void listenerLoop();
    void handleConnection(int connFd);

    TaskBase *m_base;
    std::unique_ptr<Logger> m_logger;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    int m_serverFd{-1};

    std::mutex m_pendingMutex;
    std::unordered_map<uint64_t, XnPendingHandover> m_pending;
};

} // namespace nr::gnb
