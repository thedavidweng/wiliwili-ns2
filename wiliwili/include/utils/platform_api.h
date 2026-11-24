/*
 * What the Switch 2 platform half asks the app half to do.
 *
 * Same C seam as boot_api.h, for the same reason: the platform backend is built
 * into the UI framework's static library, and the network stack lives in
 * wiliwili. Keeping these two calls in C keeps the link order simple.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Called by the platform after a sleep/wake: the console can come back on a
 * different network, so curl's shared DNS answers are dropped and the next
 * request resolves again. Implemented in wiliwili/source/utils/platform_api.cpp. */
void wiliwili_network_reset(void);

#ifdef __cplusplus
}
#endif
