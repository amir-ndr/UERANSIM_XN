//
// This file is a part of UERANSIM project.
// NTN Xn handover extension: UE side of connected-mode mobility.
//
// Executes an RRCReconfiguration carrying reconfigurationWithSync by moving the
// serving cell to the target while staying RRC-CONNECTED. Nothing here touches
// NAS: an intra-AMF, intra-TA handover is invisible above RRC, and dropping to
// idle would trigger a re-registration, which is exactly what this avoids.
//

#include "task.hpp"

#include <lib/rrc/encode.hpp>
#include <ue/nas/task.hpp>
#include <ue/rls/task.hpp>

#include <asn/rrc/ASN_RRC_CellGroupConfig.h>
#include <asn/rrc/ASN_RRC_RRCReconfiguration-IEs.h>
#include <asn/rrc/ASN_RRC_RRCReconfiguration-v1530-IEs.h>
#include <asn/rrc/ASN_RRC_RRCReconfiguration.h>
#include <asn/rrc/ASN_RRC_RRCReconfigurationComplete-IEs.h>
#include <asn/rrc/ASN_RRC_RRCReconfigurationComplete.h>
#include <asn/rrc/ASN_RRC_ReconfigurationWithSync.h>
#include <asn/rrc/ASN_RRC_ServingCellConfigCommon.h>
#include <asn/rrc/ASN_RRC_SpCellConfig.h>
#include <asn/rrc/ASN_RRC_UL-DCCH-Message.h>

namespace nr::ue
{

void UeRrcTask::receiveRrcReconfiguration(const ASN_RRC_RRCReconfiguration &msg)
{
    if (msg.criticalExtensions.present != ASN_RRC_RRCReconfiguration__criticalExtensions_PR_rrcReconfiguration)
        return;

    auto *ies = msg.criticalExtensions.choice.rrcReconfiguration;
    if (ies == nullptr || ies->nonCriticalExtension == nullptr ||
        ies->nonCriticalExtension->masterCellGroup == nullptr)
    {
        m_logger->err("[xn-ho] UE: RRCReconfiguration without masterCellGroup, ignoring");
        return;
    }

    auto *cellGroup = rrc::encode::Decode<ASN_RRC_CellGroupConfig>(
        asn_DEF_ASN_RRC_CellGroupConfig, *ies->nonCriticalExtension->masterCellGroup);
    if (cellGroup == nullptr)
    {
        m_logger->err("[xn-ho] UE: CellGroupConfig decoding failed, ignoring reconfiguration");
        return;
    }

    int64_t targetNci = -1;
    if (cellGroup->spCellConfig != nullptr && cellGroup->spCellConfig->reconfigurationWithSync != nullptr)
    {
        auto *sync = cellGroup->spCellConfig->reconfigurationWithSync;
        if (sync->spCellConfigCommon != nullptr && sync->spCellConfigCommon->physCellId != nullptr)
            targetNci = static_cast<int64_t>(*sync->spCellConfigCommon->physCellId);
    }

    asn::Free(asn_DEF_ASN_RRC_CellGroupConfig, cellGroup);

    if (targetNci < 0)
    {
        // A reconfiguration without sync is not a handover; nothing else in
        // this fork sends one, so treat it as unsupported rather than silent.
        m_logger->err("[xn-ho] UE: RRCReconfiguration without reconfigurationWithSync, ignoring");
        return;
    }

    m_logger->info("[xn-ho] UE: handover command received, target nci=0x%llx", (unsigned long long)targetNci);

    // The target cell is already in coverage: the UE has been heartbeating
    // every gNB in gnbSearchList all along, and learned each cell's NCI
    // from its SIB1.
    int targetCellId = 0;
    for (auto &item : m_cellDesc)
    {
        if (item.second.sib1.hasSib1 && item.second.sib1.nci == targetNci)
        {
            targetCellId = item.first;
            break;
        }
    }

    if (targetCellId == 0)
    {
        m_logger->err("[xn-ho] UE: target nci=0x%llx is not in coverage, staying on the current cell",
                      (unsigned long long)targetNci);
        return;
    }

    auto lastCell = m_base->shCtx.currentCell.get();
    if (targetCellId == lastCell.cellId)
    {
        m_logger->warn("[xn-ho] UE: already served by cell[%d], nothing to do", targetCellId);
        return;
    }

    auto &targetCell = m_cellDesc[targetCellId];

    ActiveCellInfo cellInfo{};
    cellInfo.cellId = targetCellId;
    cellInfo.plmn = targetCell.sib1.plmn;
    cellInfo.tac = targetCell.sib1.tac;
    cellInfo.category = ECellCategory::SUITABLE_CELL;

    m_base->shCtx.currentCell.set(cellInfo);

    auto w = std::make_unique<NmUeRrcToRls>(NmUeRrcToRls::ASSIGN_CURRENT_CELL);
    w->cellId = targetCellId;
    m_base->rlsTask->push(std::move(w));

    m_logger->info("[xn-ho] UE: serving cell %d -> %d, staying RRC-CONNECTED", lastCell.cellId, targetCellId);

    // NOTE: no ACTIVE_CELL_CHANGED to NAS and no state switch. NAS must not see
    // this as a cell reselection, otherwise it would start a registration
    // procedure and defeat the purpose of the handover.

    auto *pdu = asn::New<ASN_RRC_UL_DCCH_Message>();
    pdu->message.present = ASN_RRC_UL_DCCH_MessageType_PR_c1;
    pdu->message.choice.c1 = asn::NewFor(pdu->message.choice.c1);
    pdu->message.choice.c1->present = ASN_RRC_UL_DCCH_MessageType__c1_PR_rrcReconfigurationComplete;

    auto &complete = pdu->message.choice.c1->choice.rrcReconfigurationComplete =
        asn::New<ASN_RRC_RRCReconfigurationComplete>();
    complete->rrc_TransactionIdentifier = msg.rrc_TransactionIdentifier;
    complete->criticalExtensions.present =
        ASN_RRC_RRCReconfigurationComplete__criticalExtensions_PR_rrcReconfigurationComplete;
    complete->criticalExtensions.choice.rrcReconfigurationComplete =
        asn::New<ASN_RRC_RRCReconfigurationComplete_IEs>();

    // Routed by shCtx.currentCell, which now points at the target.
    sendRrcMessage(pdu);
    asn::Free(asn_DEF_ASN_RRC_UL_DCCH_Message, pdu);

    m_logger->info("[xn-ho] UE: RRCReconfigurationComplete -> cell[%d]", targetCellId);
}

} // namespace nr::ue
