//
// This file is a part of UERANSIM project.
// Copyright (c) 2023 ALİ GÜNGÖR.
//
// https://github.com/aligungr/UERANSIM/
// See README, LICENSE, and CONTRIBUTING files for licensing details.
//

#pragma once

#include "types.hpp"

#include <utility>

#include <lib/app/cli_base.hpp>
#include <lib/app/cli_cmd.hpp>
#include <lib/asn/utils.hpp>
#include <lib/rls/rls_base.hpp>
#include <lib/rrc/rrc.hpp>
#include <lib/sctp/sctp.hpp>
#include <utils/network.hpp>
#include <utils/nts.hpp>
#include <utils/octet_string.hpp>
#include <utils/unique_buffer.hpp>

extern "C"
{
    struct ASN_NGAP_FiveG_S_TMSI;
    struct ASN_NGAP_TAIListForPaging;
}

namespace nr::gnb
{

struct NmGnbRlsToRrc : NtsMessage
{
    enum PR
    {
        SIGNAL_DETECTED,
        UPLINK_RRC,
    } present;

    // SIGNAL_DETECTED
    // UPLINK_RRC
    int ueId{};

    // UPLINK_RRC
    OctetString data;
    rrc::RrcChannel rrcChannel{};

    explicit NmGnbRlsToRrc(PR present) : NtsMessage(NtsMessageType::GNB_RLS_TO_RRC), present(present)
    {
    }
};

struct NmGnbRlsToGtp : NtsMessage
{
    enum PR
    {
        DATA_PDU_DELIVERY,
    } present;

    // DATA_PDU_DELIVERY
    int ueId{};
    int psi{};
    OctetString pdu;

    explicit NmGnbRlsToGtp(PR present) : NtsMessage(NtsMessageType::GNB_RLS_TO_GTP), present(present)
    {
    }
};

struct NmGnbGtpToRls : NtsMessage
{
    enum PR
    {
        DATA_PDU_DELIVERY,
    } present;

    // DATA_PDU_DELIVERY
    int ueId{};
    int psi{};
    OctetString pdu{};

    explicit NmGnbGtpToRls(PR present) : NtsMessage(NtsMessageType::GNB_GTP_TO_RLS), present(present)
    {
    }
};

struct NmGnbRlsToRls : NtsMessage
{
    enum PR
    {
        SIGNAL_DETECTED,
        SIGNAL_LOST,
        RECEIVE_RLS_MESSAGE,
        DOWNLINK_RRC,
        DOWNLINK_DATA,
        UPLINK_RRC,
        UPLINK_DATA,
        RADIO_LINK_FAILURE,
        TRANSMISSION_FAILURE,
    } present;

    // SIGNAL_DETECTED
    // SIGNAL_LOST
    // DOWNLINK_RRC
    // DOWNLINK_DATA
    // UPLINK_DATA
    // UPLINK_RRC
    int ueId{};

    // RECEIVE_RLS_MESSAGE
    std::unique_ptr<rls::RlsMessage> msg{};

    // DOWNLINK_DATA
    // UPLINK_DATA
    int psi{};

    // DOWNLINK_DATA
    // DOWNLINK_RRC
    // UPLINK_DATA
    // UPLINK_RRC
    OctetString data;

    // DOWNLINK_RRC
    uint32_t pduId{};

    // DOWNLINK_RRC
    // UPLINK_RRC
    rrc::RrcChannel rrcChannel{};

    // RADIO_LINK_FAILURE
    rls::ERlfCause rlfCause{};

    // TRANSMISSION_FAILURE
    std::vector<rls::PduInfo> pduList;

    explicit NmGnbRlsToRls(PR present) : NtsMessage(NtsMessageType::GNB_RLS_TO_RLS), present(present)
    {
    }
};

struct NmGnbRrcToRls : NtsMessage
{
    enum PR
    {
        RRC_PDU_DELIVERY,
    } present;

    // RRC_PDU_DELIVERY
    int ueId{};
    rrc::RrcChannel channel{};
    OctetString pdu{};

    explicit NmGnbRrcToRls(PR present) : NtsMessage(NtsMessageType::GNB_RRC_TO_RLS), present(present)
    {
    }
};

struct NmGnbNgapToRrc : NtsMessage
{
    enum PR
    {
        RADIO_POWER_ON,
        NAS_DELIVERY,
        AN_RELEASE,
        PAGING,
        XN_HANDOVER_COMMAND,
    } present;

    // NAS_DELIVERY
    // AN_RELEASE
    // XN_HANDOVER_COMMAND
    int ueId{};

    // XN_HANDOVER_COMMAND: NCI of the target cell the UE must move to
    int64_t targetNci{};

    // NAS_DELIVERY
    OctetString pdu{};

    // PAGING
    asn::Unique<ASN_NGAP_FiveG_S_TMSI> uePagingTmsi{};
    asn::Unique<ASN_NGAP_TAIListForPaging> taiListForPaging{};

    explicit NmGnbNgapToRrc(PR present) : NtsMessage(NtsMessageType::GNB_NGAP_TO_RRC), present(present)
    {
    }
};

struct NmGnbRrcToNgap : NtsMessage
{
    enum PR
    {
        INITIAL_NAS_DELIVERY,
        UPLINK_NAS_DELIVERY,
        RADIO_LINK_FAILURE
    } present;

    // INITIAL_NAS_DELIVERY
    // UPLINK_NAS_DELIVERY
    // RADIO_LINK_FAILURE
    int ueId{};

    // INITIAL_NAS_DELIVERY
    // UPLINK_NAS_DELIVERY
    OctetString pdu{};

    // INITIAL_NAS_DELIVERY
    int64_t rrcEstablishmentCause{};
    std::optional<GutiMobileIdentity> sTmsi{};

    explicit NmGnbRrcToNgap(PR present) : NtsMessage(NtsMessageType::GNB_RRC_TO_NGAP), present(present)
    {
    }
};

struct NmGnbNgapToGtp : NtsMessage
{
    enum PR
    {
        UE_CONTEXT_UPDATE,
        UE_CONTEXT_RELEASE,
        SESSION_CREATE,
        SESSION_RELEASE,
    } present;

    // UE_CONTEXT_UPDATE
    std::unique_ptr<GtpUeContextUpdate> update{};

    // SESSION_CREATE
    PduSessionResource *resource{};

    // UE_CONTEXT_RELEASE
    // SESSION_RELEASE
    int ueId{};

    // SESSION_RELEASE
    int psi{};

    explicit NmGnbNgapToGtp(PR present) : NtsMessage(NtsMessageType::GNB_NGAP_TO_GTP), present(present)
    {
    }
};

struct NmGnbSctp : NtsMessage
{
    enum PR
    {
        CONNECTION_REQUEST,
        CONNECTION_CLOSE,
        ASSOCIATION_SETUP,
        ASSOCIATION_SHUTDOWN,
        RECEIVE_MESSAGE,
        SEND_MESSAGE,
        UNHANDLED_NOTIFICATION,
    } present;

    // CONNECTION_REQUEST
    // CONNECTION_CLOSE
    // ASSOCIATION_SETUP
    // ASSOCIATION_SHUTDOWN
    // RECEIVE_MESSAGE
    // SEND_MESSAGE
    // UNHANDLED_NOTIFICATION
    int clientId{};

    // CONNECTION_REQUEST
    std::string localAddress{};
    uint16_t localPort{};
    std::string remoteAddress{};
    uint16_t remotePort{};
    sctp::PayloadProtocolId ppid{};
    NtsTask *associatedTask{};

    // ASSOCIATION_SETUP
    int associationId{};
    int inStreams{};
    int outStreams{};

    // RECEIVE_MESSAGE
    // SEND_MESSAGE
    UniqueBuffer buffer{};
    uint16_t stream{};

    explicit NmGnbSctp(PR present) : NtsMessage(NtsMessageType::GNB_SCTP), present(present)
    {
    }
};

struct NmGnbStatusUpdate : NtsMessage
{
    static constexpr const int NGAP_IS_UP = 1;

    const int what;

    // NGAP_IS_UP
    bool isNgapUp{};

    explicit NmGnbStatusUpdate(const int what) : NtsMessage(NtsMessageType::GNB_STATUS_UPDATE), what(what)
    {
    }
};

struct NmGnbXnToNgap : NtsMessage
{
    enum PR
    {
        TRIGGER_PATH_SWITCH,
    } present;

    struct SessionInfo
    {
        int psi{};
        uint32_t upTeid{};
        OctetString upAddr{};
        std::vector<long> qfis{};
    };

    int64_t amfUeNgapId{};
    std::vector<SessionInfo> sessions;

    // Local RLS/RRC ueId of the arrived UE. The NGAP context is created with
    // this id so that the path-switched context is the same one the UE is
    // actually connected through.
    int ueId{};
    int sliceSst{};

    // Timestamps for the handover latency row (microseconds since epoch):
    // when the XnHandoverRequest arrived, and when the UE completed the move.
    int64_t requestedUs{};
    int64_t arrivedUs{};

    explicit NmGnbXnToNgap(PR present)
        : NtsMessage(NtsMessageType::GNB_XN_TO_NGAP), present(present)
    {
    }
};

// Xn handover request coming from the CLI. It is deliberately handled as a
// message on the NGAP task rather than inline in the CLI handler: the CLI
// handler runs with RRC/RLS/GTP paused, and a paused task only resumes on a
// 20ms polling boundary, which would add ~10ms of pure simulator latency
// between the handover being triggered and the command reaching the UE.
struct NmGnbCliToNgap : NtsMessage
{
    enum PR
    {
        TRIGGER_XN_HANDOVER,
    } present;

    int ueId{};
    std::string targetGnbAddress{};
    uint16_t targetGnbPort{};
    int sliceSst{};

    explicit NmGnbCliToNgap(PR present) : NtsMessage(NtsMessageType::GNB_CLI_TO_NGAP), present(present)
    {
    }
};

struct NmGnbCliCommand : NtsMessage
{
    std::unique_ptr<app::GnbCliCommand> cmd;
    InetAddress address;

    NmGnbCliCommand(std::unique_ptr<app::GnbCliCommand> cmd, InetAddress address)
        : NtsMessage(NtsMessageType::GNB_CLI_COMMAND), cmd(std::move(cmd)), address(address)
    {
    }
};

} // namespace nr::gnb
