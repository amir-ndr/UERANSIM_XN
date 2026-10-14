//
// This file is a part of UERANSIM project.
// NTN Xn handover extension: RRC-level connected-mode mobility.
//
// Source side: builds the RRCReconfiguration carrying reconfigurationWithSync
// that tells the UE to move to the target cell, without ever releasing it.
// Target side: accepts the RRCReconfigurationComplete of a UE that arrives
// from another gNB and binds it to the pending Xn handover.
//

#include "task.hpp"

#include <gnb/ngap/task.hpp>
#include <gnb/rls/task.hpp>
#include <gnb/xn/task.hpp>
#include <lib/rrc/encode.hpp>
#include <utils/common.hpp>

#include <asn/rrc/ASN_RRC_CellGroupConfig.h>
#include <asn/rrc/ASN_RRC_DL-DCCH-Message.h>
#include <asn/rrc/ASN_RRC_DL-DCCH-MessageType.h>
#include <asn/rrc/ASN_RRC_RRCReconfiguration-IEs.h>
#include <asn/rrc/ASN_RRC_RRCReconfiguration-v1530-IEs.h>
#include <asn/rrc/ASN_RRC_RRCReconfiguration.h>
#include <asn/rrc/ASN_RRC_RRCReconfigurationComplete.h>
#include <asn/rrc/ASN_RRC_ReconfigurationWithSync.h>
#include <asn/rrc/ASN_RRC_ServingCellConfigCommon.h>
#include <asn/rrc/ASN_RRC_SpCellConfig.h>

namespace nr::gnb
{

// 3GPP identifies a handover target by physCellId, which is constrained to
// 0..1007. UERANSIM has no PCI concept and identifies cells by NCI, so the NCI
// travels in that field; this is the resulting limit on usable NCI values.
static constexpr int64_t MAX_TARGET_NCI = 1007;

void GnbRrcTask::sendXnHandoverCommand(int ueId, int64_t targetNci)
{
    auto *ue = tryFindUe(ueId);
    if (ue == nullptr)
    {
        m_logger->err("[xn-ho] SOURCE: cannot hand over UE[%d], no RRC context", ueId);
        return;
    }

    if (targetNci < 0 || targetNci > MAX_TARGET_NCI)
    {
        m_logger->err("[xn-ho] SOURCE: target nci 0x%llx exceeds physCellId range [0..%lld], handover aborted",
                      (unsigned long long)targetNci, (long long)MAX_TARGET_NCI);
        return;
    }

    // reconfigurationWithSync lives inside a CellGroupConfig, which travels as
    // a separately encoded OCTET STRING in masterCellGroup.
    auto *cellGroup = asn::New<ASN_RRC_CellGroupConfig>();
    cellGroup->cellGroupId = 0;

    auto *spCell = asn::New<ASN_RRC_SpCellConfig>();
    auto *sync = asn::New<ASN_RRC_ReconfigurationWithSync>();

    auto *common = asn::New<ASN_RRC_ServingCellConfigCommon>();
    common->physCellId = asn::New<ASN_RRC_PhysCellId_t>();
    *common->physCellId = static_cast<long>(targetNci);
    common->dmrs_TypeA_Position = ASN_RRC_ServingCellConfigCommon__dmrs_TypeA_Position_pos2;
    common->ss_PBCH_BlockPower = 0;

    sync->spCellConfigCommon = common;
    sync->newUE_Identity = static_cast<ASN_RRC_RNTI_Value_t>(ueId & 0xFFFF);
    sync->t304 = ASN_RRC_ReconfigurationWithSync__t304_ms1000;

    spCell->reconfigurationWithSync = sync;
    cellGroup->spCellConfig = spCell;

    OctetString encodedCellGroup = rrc::encode::EncodeS(asn_DEF_ASN_RRC_CellGroupConfig, cellGroup);
    asn::Free(asn_DEF_ASN_RRC_CellGroupConfig, cellGroup);

    if (encodedCellGroup.length() == 0)
    {
        m_logger->err("[xn-ho] SOURCE: CellGroupConfig encoding failed, handover aborted");
        return;
    }

    auto *pdu = asn::New<ASN_RRC_DL_DCCH_Message>();
    pdu->message.present = ASN_RRC_DL_DCCH_MessageType_PR_c1;
    pdu->message.choice.c1 = asn::NewFor(pdu->message.choice.c1);
    pdu->message.choice.c1->present = ASN_RRC_DL_DCCH_MessageType__c1_PR_rrcReconfiguration;

    auto &reconf = pdu->message.choice.c1->choice.rrcReconfiguration = asn::New<ASN_RRC_RRCReconfiguration>();
    reconf->rrc_TransactionIdentifier = getNextTid();
    reconf->criticalExtensions.present = ASN_RRC_RRCReconfiguration__criticalExtensions_PR_rrcReconfiguration;

    auto *ies = asn::New<ASN_RRC_RRCReconfiguration_IEs>();
    auto *v1530 = asn::New<ASN_RRC_RRCReconfiguration_v1530_IEs>();

    v1530->masterCellGroup = asn::New<OCTET_STRING_t>();
    asn::SetOctetString(*v1530->masterCellGroup, encodedCellGroup);

    ies->nonCriticalExtension = v1530;
    reconf->criticalExtensions.choice.rrcReconfiguration = ies;

    sendRrcMessage(ueId, pdu);
    asn::Free(asn_DEF_ASN_RRC_DL_DCCH_Message, pdu);

    m_logger->info("[xn-ho] SOURCE: RRCReconfiguration(reconfigurationWithSync) -> UE[%d], target nci=0x%llx", ueId,
                   (unsigned long long)targetNci);

    // Drop the local RRC context without sending an RRC Release: the UE is
    // moving while staying RRC-CONNECTED, it must not fall back to idle.
    delete ue;
    m_ueCtx.erase(ueId);
}

void GnbRrcTask::receiveRrcReconfigurationComplete(int ueId, const ASN_RRC_RRCReconfigurationComplete &msg)
{
    // A UE that completed a handover into this cell. Its RLS session already
    // exists (it has been heartbeating us all along), and the STI carried by
    // that session is what ties it to the XnHandoverRequest the source sent.
    uint64_t sti = m_base->rlsTask->stiOf(ueId);
    if (sti == 0)
    {
        m_logger->err("[xn-ho] TARGET: RRCReconfigurationComplete from UE[%d] with no RLS session", ueId);
        return;
    }

    XnPendingHandover pending{};
    if (!m_base->xnTask->takePendingHandover(sti, pending))
    {
        m_logger->err("[xn-ho] TARGET: unexpected RRCReconfigurationComplete from UE[%d], no pending handover", ueId);
        return;
    }

    if (tryFindUe(ueId) == nullptr)
        createUe(ueId);

    m_logger->info("[xn-ho] TARGET: UE arrived, ueId=%d sti=0x%llx, creating RRC context", ueId,
                   (unsigned long long)sti);

    auto w = std::make_unique<NmGnbXnToNgap>(NmGnbXnToNgap::TRIGGER_PATH_SWITCH);
    w->ueId = ueId;
    w->amfUeNgapId = pending.amfUeNgapId;
    w->sessions = std::move(pending.sessions);
    w->sliceSst = pending.sliceSst;
    w->requestedUs = pending.requestedUs;
    w->arrivedUs = utils::CurrentTimeMicros();
    m_base->ngapTask->push(std::move(w));
}

} // namespace nr::gnb
