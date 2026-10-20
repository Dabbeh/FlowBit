#include "etwclass.h"

#include <cwchar>

namespace net {
namespace etwclass {

namespace {

// Ids from docs/etw-kernel-network-schema.md, measured on this build. Kept only as the fallback
// when a record arrives without a name.
constexpr unsigned short kTcpSend = 10;
constexpr unsigned short kTcpRecv = 11;
constexpr unsigned short kTcpRetransmit = 14;
constexpr unsigned short kTcpIpv6Send = 26;
constexpr unsigned short kTcpIpv6Recv = 27;
constexpr unsigned short kTcpIpv6Retransmit = 30;

bool contains(const wchar_t *haystack, const wchar_t *needle)
{
    return haystack && std::wcsstr(haystack, needle) != nullptr;
}

} // namespace

Kind classify(const wchar_t *name, unsigned short id)
{
    Kind kind;

    if (name && *name) {
        kind.fromName = true;

        // Protocol first. A name that is neither TCP nor UDP (TcpConnect, TcpDisconnect,
        // TcpCopy, TcpFailure, ...) is not a byte carrier and is ignored.
        const bool tcp = contains(name, L"Tcp");
        const bool udp = contains(name, L"Udp");
        if (!tcp && !udp)
            return kind;

        // "Retransmit" must be checked before direction: it contains neither Send nor Recv, but
        // ordering it first keeps the intent obvious.
        if (contains(name, L"Retransmit")) {
            kind.transfer = true;
            kind.retransmit = true;
            kind.udp = udp;
            return kind;
        }

        const bool send = contains(name, L"Send");
        const bool recv = contains(name, L"Recv");
        if (!send && !recv)
            return kind;   // connect / disconnect / copy / failure: no bytes counted here

        kind.transfer = true;
        kind.send = send;
        kind.udp = udp;
        return kind;
    }

    // No name: the id table, TCP only. This is the pre-existing behaviour, kept so a build whose
    // provider omits names still works exactly as before.
    const bool isV6 = (id == kTcpIpv6Send || id == kTcpIpv6Recv || id == kTcpIpv6Retransmit);
    const bool transfer = (id == kTcpSend || id == kTcpRecv || id == kTcpRetransmit || isV6);
    if (!transfer)
        return kind;

    kind.transfer = true;
    kind.retransmit = (id == kTcpRetransmit || id == kTcpIpv6Retransmit);
    kind.send = (id == kTcpSend || id == kTcpIpv6Send || id == kTcpRetransmit
                 || id == kTcpIpv6Retransmit);
    return kind;
}

} // namespace etwclass
} // namespace net
