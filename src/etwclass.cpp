#include "etwclass.h"

#include <cwchar>
#include <cwctype>

namespace net {
namespace etwclass {

namespace {

// Microsoft-Windows-Kernel-Network (provider {7dd42a49-5329-4832-8dfd-43d979153a88}).
//
// Source: Microsoft's own generated event mapping,
//   github.com/microsoft/Tx/blob/master/Generated/Microsoft_Windows_Kernel_Network.cs
// which lists every event this provider emits, by id, with its manifest name. The UDP entries are
// the ones this decoder had been missing:
//
//   42 KNetEvt_SendIPV4Udp   "UDPv4: %2 bytes transmitted ..."
//   43 KNetEvt_RecvIPV4Udp   "UDPv4: %2 bytes received ..."
//   58 KNetEvt_SendIPV6Udp   "UDPv6: %2 bytes transmitted ..."
//   59 KNetEvt_RecvIPV6Udp   "UDPv6: %2 bytes received ..."
//
// Ids 49 (UDP failure) carry no bytes. The TCP ids are the ones the repo's schema notes measured.
constexpr unsigned short kTcpSend = 10;
constexpr unsigned short kTcpRecv = 11;
constexpr unsigned short kTcpRetransmit = 14;
constexpr unsigned short kTcpIpv6Send = 26;
constexpr unsigned short kTcpIpv6Recv = 27;
constexpr unsigned short kTcpIpv6Retransmit = 30;
constexpr unsigned short kUdpSendV4 = 42;
constexpr unsigned short kUdpRecvV4 = 43;
constexpr unsigned short kUdpSendV6 = 58;
constexpr unsigned short kUdpRecvV6 = 59;

// Case-insensitive substring search.
//
// The manifest names are not written in one uniform style: the event symbols are
// "KNetEvt_SendIPV4Udp", while the message names are "Data_sent_over_UDP_protocol_". A
// case-sensitive match on "Udp"/"Send" would miss the second form entirely - which is exactly the
// kind of silent miss this decoder has already been bitten by. Case is therefore ignored, and both
// the imperative and past forms of the verbs are accepted.
bool contains(const wchar_t *haystack, const wchar_t *needle)
{
    if (!haystack || !needle || !*needle)
        return false;
    const size_t needleLen = std::wcslen(needle);
    for (const wchar_t *p = haystack; *p; ++p) {
        size_t i = 0;
        while (i < needleLen && p[i] && std::towlower(p[i]) == std::towlower(needle[i]))
            ++i;
        if (i == needleLen)
            return true;
    }
    return false;
}

} // namespace

Kind classify(const wchar_t *name, unsigned short id)
{
    Kind kind;

    // IDS FIRST. They are authoritative and were confirmed against Microsoft's generated mapping
    // of this provider, so they cannot be misread.
    //
    // The name is only a FALLBACK for an id this table does not know, and that order matters.
    // Classifying by name first broke event counting outright on the reporting machine: the
    // provider's manifest names its TCP events "Data_sent_" and "Data_received_" - no "tcp" in the
    // text - so a name-first rule that requires a protocol word rejected EVERY event, and
    // Diagnostics showed an active session with 0 events after 78 seconds of monitoring.
    switch (id) {
    case kTcpSend:
    case kTcpIpv6Send:
        kind.transfer = true;
        kind.send = true;
        return kind;
    case kTcpRecv:
    case kTcpIpv6Recv:
        kind.transfer = true;
        return kind;
    case kTcpRetransmit:
    case kTcpIpv6Retransmit:
        kind.transfer = true;
        kind.retransmit = true;
        return kind;
    case kUdpSendV4:
    case kUdpSendV6:
        kind.transfer = true;
        kind.send = true;
        kind.udp = true;
        return kind;
    case kUdpRecvV4:
    case kUdpRecvV6:
        kind.transfer = true;
        kind.udp = true;
        return kind;
    default:
        break;   // unknown id: fall through to the name
    }

    if (!name || !*name)
        return kind;

    kind.fromName = true;

    if (contains(name, L"retransmit")) {
        kind.transfer = true;
        kind.retransmit = true;
        return kind;
    }

    const bool send = contains(name, L"send") || contains(name, L"sent");
    const bool recv = contains(name, L"recv") || contains(name, L"receiv");
    if (!send && !recv)
        return kind;   // connect / disconnect / copy / failure: no bytes counted

    kind.transfer = true;
    kind.send = send;
    // Only claim a protocol when the name actually says so; "Data_sent_" says nothing and must
    // not be guessed at.
    kind.udp = contains(name, L"udp");
    return kind;
}

} // namespace etwclass
} // namespace net
