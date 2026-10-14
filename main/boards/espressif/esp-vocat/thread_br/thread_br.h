#pragma once

// Start a background supervisor after the Wi-Fi station has connected.
// The original esp-vocat variant does not call this function.
void StartMiaobanThreadBorderRouter();

// Release all references to the STA netif before WifiManager destroys it.
// False means startup/shutdown is still in progress; the caller must keep STA alive.
bool StopMiaobanThreadBorderRouter();
