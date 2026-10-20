// Classification of Microsoft-Windows-Kernel-Network events by NAME, with ids only as a fallback.
//
// WHY BY NAME
//
// The decoder used to recognise only TCP send/receive by hardcoded event ids (10/11 for IPv4, 26/27
// for IPv6). That made every other protocol invisible - UDP in particular, which matters because
// DNS, QUIC, WireGuard, game traffic and most video calls are UDP. Windows' own Resource Monitor
// reads the same provider and shows all of it.
//
// The ids for UDP are not documented in any header on this machine and the provider's manifest is
// not on disk, so guessing them would have produced exactly the class of bug this project keeps
// finding: a decoder that silently mislabels traffic. TDH hands over the event's own name
// ("KNetEvt_TcpSend", "KNetEvt_UdpRecv", ...), which is stable, self-describing, and covers IPv4
// and IPv6 variants without a table.
//
// Ids remain a fallback for the case where a record carries no name.

#pragma once

#include <QtGlobal>

namespace net {
namespace etwclass {

struct Kind {
    bool transfer = false;     // carries bytes we should count
    bool send = false;         // direction: true = sent by an application, false = received
    bool retransmit = false;   // carries bytes already counted once
    bool udp = false;          // protocol, for diagnostics only
    bool fromName = false;     // classification came from the event name, not the id table
};

// `name` may be null; `id` is always used as the fallback.
Kind classify(const wchar_t *name, unsigned short id);

} // namespace etwclass
} // namespace net
