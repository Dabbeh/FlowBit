#pragma once

#include <windows.h>
#include <windivert.h>

namespace net {

struct LoadedDriverProbe {
    bool available = false;
    DWORD error = ERROR_SUCCESS;
};

// Check the loaded driver without installing it or intercepting any traffic.
// WinDivert marks its service for deletion after a successful start, so the
// registry's Start/DeleteFlag values alone cannot establish driver availability.
inline LoadedDriverProbe probeLoadedDriver()
{
    const HANDLE handle = WinDivertOpen(
        "false", WINDIVERT_LAYER_NETWORK, 0,
        WINDIVERT_FLAG_NO_INSTALL | WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY);
    if (handle == INVALID_HANDLE_VALUE)
        return {false, GetLastError()};

    WinDivertClose(handle);
    return {true, ERROR_SUCCESS};
}

} // namespace net
