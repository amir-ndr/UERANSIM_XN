//
// This file is a part of UERANSIM project.
// Copyright (c) 2023 ALİ GÜNGÖR.
//
// https://github.com/aligungr/UERANSIM/
// See README, LICENSE, and CONTRIBUTING files for licensing details.
//

#include "task.hpp"

namespace nr::gnb
{

NgapAmfContext *NgapTask::selectAmf(int ueId, int32_t &requestedSliceType)
{
    NgapAmfContext *anyConnected = nullptr;

    for (auto &amf : m_amfCtx) {
        if (amf.second->state != EAmfState::CONNECTED)
            continue;
        if (anyConnected == nullptr)
            anyConnected = amf.second;

        if (requestedSliceType < 0)
            continue; // no slice filter — collect fallback, keep looping

        for (const auto &plmnSupport : amf.second->plmnSupportList) {
            for (const auto &singleSlice : plmnSupport->sliceSupportList.slices) {
                if (static_cast<int32_t>(singleSlice.sst) == requestedSliceType)
                    return amf.second;
            }
        }
    }

    // Service Request / messages without NSSAI: use any connected AMF
    return anyConnected;
}

NgapAmfContext *NgapTask::selectNewAmfForReAllocation(int ueId, int initiatedAmfId, int amfSetId)
{
    // TODO an arbitrary AMF is selected for now
    return findAmfContext(initiatedAmfId);
}

} // namespace nr::gnb
