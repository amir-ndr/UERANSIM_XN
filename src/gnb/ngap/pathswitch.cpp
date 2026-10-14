//
// This file is a part of UERANSIM project.
// NTN Xn handover extension: NGAP PathSwitchRequest to AMF (gnb2 side).
//

#include "encode.hpp"
#include "task.hpp"
#include "utils.hpp"

#include <gnb/gtp/task.hpp>
#include <lib/asn/utils.hpp>
#include <utils/common.hpp>

#include <fstream>
#include <iomanip>

#include <asn/ngap/ASN_NGAP_GTPTunnel.h>
#include <asn/ngap/ASN_NGAP_PathSwitchRequest.h>
#include <asn/ngap/ASN_NGAP_PathSwitchRequestAcknowledge.h>
#include <asn/ngap/ASN_NGAP_PathSwitchRequestAcknowledgeTransfer.h>
#include <asn/ngap/ASN_NGAP_PathSwitchRequestTransfer.h>
#include <asn/ngap/ASN_NGAP_PDUSessionResourceSwitchedItem.h>
#include <asn/ngap/ASN_NGAP_PDUSessionResourceSwitchedList.h>
#include <asn/ngap/ASN_NGAP_PDUSessionResourceToBeSwitchedDLItem.h>
#include <asn/ngap/ASN_NGAP_PDUSessionResourceToBeSwitchedDLList.h>
#include <asn/ngap/ASN_NGAP_ProtocolIE-Field.h>
#include <asn/ngap/ASN_NGAP_QosFlowAcceptedItem.h>
#include <asn/ngap/ASN_NGAP_QosFlowAcceptedList.h>
#include <asn/ngap/ASN_NGAP_QosFlowSetupRequestItem.h>
#include <asn/ngap/ASN_NGAP_QosFlowSetupRequestList.h>
#include <asn/ngap/ASN_NGAP_UESecurityCapabilities.h>

namespace nr::gnb
{

void NgapTask::sendPathSwitchRequest(int ueId, int64_t amfUeNgapId,
                                      const std::vector<NmGnbXnToNgap::SessionInfo> &sessions,
                                      int sliceSst, int64_t requestedUs, int64_t arrivedUs)
{
    if (sessions.empty())
    {
        m_logger->err("PathSwitchRequest: no PDU sessions to switch");
        return;
    }

    // Find a connected AMF
    NgapAmfContext *amf = nullptr;
    for (auto &[id, ctx] : m_amfCtx)
    {
        if (ctx->state == EAmfState::CONNECTED)
        {
            amf = ctx;
            break;
        }
    }
    if (amf == nullptr)
    {
        m_logger->err("PathSwitchRequest: no connected AMF");
        return;
    }

    // The context is created with the UE's local RLS/RRC id, so the path we
    // switch here belongs to the UE that actually arrived on our radio side.
    if (m_ueCtx.count(ueId))
    {
        m_logger->err("PathSwitchRequest: UE[%d] already has an NGAP context", ueId);
        return;
    }

    auto *ue            = new NgapUeContext(ueId);
    ue->ranUeNgapId     = ++m_ueNgapIdCounter;
    ue->amfUeNgapId     = amfUeNgapId;
    ue->associatedAmfId = amf->ctxId;
    ue->xnSliceSst      = sliceSst;
    ue->xnRequestedUs   = requestedUs;
    ue->xnArrivedUs     = arrivedUs;

    amf->nextStream = (amf->nextStream + 1) % amf->association.outStreams;
    if ((amf->nextStream == 0) && (amf->association.outStreams > 1))
        amf->nextStream += 1;
    ue->uplinkStream = amf->nextStream;

    m_ueCtx[ue->ctxId] = ue;

    // Build PDUSessionResourceToBeSwitchedDLList, one item per session
    std::vector<ASN_NGAP_PDUSessionResourceToBeSwitchedDLItem *> switchItems;

    for (auto &si : sessions)
    {
        uint32_t downTeid = ++m_downlinkTeidCounter;

        // Store for use when PathSwitchRequestAcknowledge arrives
        NgapUeContext::PduSessionXnInfo xnInfo;
        xnInfo.upTeid   = si.upTeid;
        xnInfo.upAddr   = si.upAddr.copy();
        xnInfo.downTeid = downTeid;
        xnInfo.qfis     = si.qfis;
        ue->xnSessionInfo[si.psi] = std::move(xnInfo);
        ue->pduSessions.insert(si.psi);

        // PathSwitchRequestTransfer: gnb2's new DL GTP endpoint + accepted QFIs
        auto *tr = asn::New<ASN_NGAP_PathSwitchRequestTransfer>();

        auto &dlTnl   = tr->dL_NGU_UP_TNLInformation;
        dlTnl.present = ASN_NGAP_UPTransportLayerInformation_PR_gTPTunnel;
        dlTnl.choice.gTPTunnel = asn::New<ASN_NGAP_GTPTunnel>();

        std::string gtpIp = m_base->config->gtpAdvertiseIp.value_or(m_base->config->gtpIp);
        asn::SetBitString(dlTnl.choice.gTPTunnel->transportLayerAddress,
                          utils::IpToOctetString(gtpIp));
        asn::SetOctetString4(dlTnl.choice.gTPTunnel->gTP_TEID, (octet4)downTeid);

        for (long qfi : si.qfis)
        {
            auto *qosItem              = asn::New<ASN_NGAP_QosFlowAcceptedItem>();
            qosItem->qosFlowIdentifier = qfi;
            asn::SequenceAdd(tr->qosFlowAcceptedList, qosItem);
        }

        OctetString encodedTr =
            ngap_encode::EncodeS(asn_DEF_ASN_NGAP_PathSwitchRequestTransfer, tr);
        asn::Free(asn_DEF_ASN_NGAP_PathSwitchRequestTransfer, tr);

        if (encodedTr.length() == 0)
        {
            m_logger->err("PathSwitchRequestTransfer encoding failed for psi=%d", si.psi);
            continue;
        }

        auto *item        = asn::New<ASN_NGAP_PDUSessionResourceToBeSwitchedDLItem>();
        item->pDUSessionID = si.psi;
        asn::SetOctetString(item->pathSwitchRequestTransfer, encodedTr);
        switchItems.push_back(item);
    }

    if (switchItems.empty())
    {
        m_logger->err("PathSwitchRequest: all session transfer encodings failed");
        deleteUeContext(ue->ctxId);
        return;
    }

    // ── Build PathSwitchRequest PDU ──────────────────────────────────────────

    std::vector<ASN_NGAP_PathSwitchRequestIEs *> ies;

    // Source-AMF-UE-NGAP-ID (the original ID assigned to gnb1's UE by the AMF)
    auto *ieSourceAmf     = asn::New<ASN_NGAP_PathSwitchRequestIEs>();
    ieSourceAmf->id         = ASN_NGAP_ProtocolIE_ID_id_SourceAMF_UE_NGAP_ID;
    ieSourceAmf->criticality = ASN_NGAP_Criticality_reject;
    ieSourceAmf->value.present = ASN_NGAP_PathSwitchRequestIEs__value_PR_AMF_UE_NGAP_ID;
    asn::SetSigned64(amfUeNgapId, ieSourceAmf->value.choice.AMF_UE_NGAP_ID);
    ies.push_back(ieSourceAmf);

    // UESecurityCapabilities (mandatory — use representative NR NEA1+NEA2 / NIA1+NIA2)
    auto *ieSec      = asn::New<ASN_NGAP_PathSwitchRequestIEs>();
    ieSec->id         = ASN_NGAP_ProtocolIE_ID_id_UESecurityCapabilities;
    ieSec->criticality = ASN_NGAP_Criticality_reject;
    ieSec->value.present = ASN_NGAP_PathSwitchRequestIEs__value_PR_UESecurityCapabilities;
    auto &secCap = ieSec->value.choice.UESecurityCapabilities;
    asn::SetBitStringInt<16>(0xC000, secCap.nRencryptionAlgorithms);
    asn::SetBitStringInt<16>(0xC000, secCap.nRintegrityProtectionAlgorithms);
    asn::SetBitStringInt<16>(0,      secCap.eUTRAencryptionAlgorithms);
    asn::SetBitStringInt<16>(0,      secCap.eUTRAintegrityProtectionAlgorithms);
    ies.push_back(ieSec);

    // PDUSessionResourceToBeSwitchedDLList
    auto *ieSwitchList    = asn::New<ASN_NGAP_PathSwitchRequestIEs>();
    ieSwitchList->id         = ASN_NGAP_ProtocolIE_ID_id_PDUSessionResourceToBeSwitchedDLList;
    ieSwitchList->criticality = ASN_NGAP_Criticality_reject;
    ieSwitchList->value.present =
        ASN_NGAP_PathSwitchRequestIEs__value_PR_PDUSessionResourceToBeSwitchedDLList;
    for (auto *item : switchItems)
        asn::SequenceAdd(
            ieSwitchList->value.choice.PDUSessionResourceToBeSwitchedDLList, item);
    ies.push_back(ieSwitchList);

    auto *pdu = asn::ngap::NewMessagePdu<ASN_NGAP_PathSwitchRequest>(ies);
    sendNgapUeAssociated(ue->ctxId, pdu);

    m_logger->info("[xn-ho] TARGET: PathSwitchRequest -> AMF | ueId=%d srcAmfId=%ld ranUeId=%ld sessions=%zu",
                   ue->ctxId, amfUeNgapId, ue->ranUeNgapId, sessions.size());
}

void NgapTask::receivePathSwitchRequestAcknowledge(int amfId,
                                                    ASN_NGAP_PathSwitchRequestAcknowledge *msg)
{
    m_logger->debug("PathSwitchRequestAcknowledge received");

    auto *ue = findUeByNgapIdPair(amfId, ngap_utils::FindNgapIdPair(msg));
    if (ue == nullptr)
        return;

    // Register the UE with the GTP task before creating sessions
    auto w0 = std::make_unique<NmGnbNgapToGtp>(NmGnbNgapToGtp::UE_CONTEXT_UPDATE);
    w0->update = std::make_unique<GtpUeContextUpdate>(true, ue->ctxId, ue->ueAmbr);
    m_base->gtpTask->push(std::move(w0));

    int switchedCount = 0;

    auto *ie = asn::ngap::GetProtocolIe(msg, ASN_NGAP_ProtocolIE_ID_id_PDUSessionResourceSwitchedList);
    if (ie)
    {
        auto &list = ie->PDUSessionResourceSwitchedList.list;
        for (int i = 0; i < list.count; i++)
        {
            auto *item = list.array[i];
            int    psi = static_cast<int>(item->pDUSessionID);

            if (!ue->xnSessionInfo.count(psi))
            {
                m_logger->err("PathSwitchAck: no stored xnSessionInfo for psi=%d", psi);
                continue;
            }
            auto &xnInfo = ue->xnSessionInfo[psi];

            // Decode transfer — AMF may provide an updated UPF uplink endpoint
            auto *tr = ngap_encode::Decode<ASN_NGAP_PathSwitchRequestAcknowledgeTransfer>(
                asn_DEF_ASN_NGAP_PathSwitchRequestAcknowledgeTransfer,
                item->pathSwitchRequestAcknowledgeTransfer);
            if (tr && tr->uL_NGU_UP_TNLInformation &&
                tr->uL_NGU_UP_TNLInformation->present ==
                    ASN_NGAP_UPTransportLayerInformation_PR_gTPTunnel)
            {
                auto *gtp = tr->uL_NGU_UP_TNLInformation->choice.gTPTunnel;
                if (gtp)
                {
                    xnInfo.upTeid = static_cast<uint32_t>(asn::GetOctet4(gtp->gTP_TEID));
                    xnInfo.upAddr = asn::GetOctetString(gtp->transportLayerAddress);
                }
            }
            if (tr)
                asn::Free(asn_DEF_ASN_NGAP_PathSwitchRequestAcknowledgeTransfer, tr);

            // Build PduSessionResource for GTP task
            auto *resource = new PduSessionResource(ue->ctxId, psi);
            resource->sessionType        = PduSessionType::IPv4;
            resource->upTunnel.teid      = xnInfo.upTeid;
            resource->upTunnel.address   = xnInfo.upAddr.copy();

            std::string gtpIp = m_base->config->gtpAdvertiseIp.value_or(m_base->config->gtpIp);
            resource->downTunnel.address = utils::IpToOctetString(gtpIp);
            resource->downTunnel.teid    = xnInfo.downTeid;

            // Populate qosFlows — GTP task reads list.array[0]->qosFlowIdentifier
            auto *qosList = asn::New<ASN_NGAP_QosFlowSetupRequestList>();
            for (long qfi : xnInfo.qfis)
            {
                auto *qosItem              = asn::New<ASN_NGAP_QosFlowSetupRequestItem>();
                qosItem->qosFlowIdentifier = qfi;
                asn::SequenceAdd(*qosList, qosItem);
            }
            resource->qosFlows = asn::WrapUnique(qosList, asn_DEF_ASN_NGAP_QosFlowSetupRequestList);

            auto w = std::make_unique<NmGnbNgapToGtp>(NmGnbNgapToGtp::SESSION_CREATE);
            w->resource = resource;
            m_base->gtpTask->push(std::move(w));

            switchedCount++;
        }
    }

    m_logger->info("PDU session resource(s) setup for UE[%d] count[%d]",
                   ue->ctxId, switchedCount);

    // Microsecond timestamps: with the handover down in the single-digit
    // millisecond range, millisecond resolution would quantise these to 0 or 1.
    int64_t completedUs = utils::CurrentTimeMicros();
    double arrivalMs = (ue->xnArrivedUs - ue->xnRequestedUs) / 1000.0;
    double pswMs     = (completedUs - ue->xnArrivedUs) / 1000.0;
    double totalMs   = (completedUs - ue->xnRequestedUs) / 1000.0;

    m_logger->info("[xn-ho] TARGET: COMPLETE ueId=%d sst=%d sessions=%d | "
                   "arrival=%.3fms psw=%.3fms total=%.3fms",
                   ue->ctxId, ue->xnSliceSst, switchedCount,
                   arrivalMs, pswMs, totalMs);

    // One row per handover: t0 = XnHandoverRequest received, t1 = UE completed
    // its move, t2 = AMF acknowledged the path switch.
    {
        std::ofstream csv("/home/amirndr/5g-lab/xn_ho_latency.csv", std::ios::app);
        if (csv.is_open())
        {
            csv << ue->xnSliceSst << ","
                << std::fixed << std::setprecision(3)
                << arrivalMs << "," << pswMs << "," << totalMs << "\n";
        }
    }
}

} // namespace nr::gnb
