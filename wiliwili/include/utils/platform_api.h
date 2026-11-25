/*
 * The C seam between the Switch 2 platform half and the app half.
 *
 * Same C seam as boot_api.h, for the same reason: the platform backend is built
 * into the UI framework's static library while the network stack, the player and
 * the settings UI live in wiliwili. Both directions cross here in C, so the link
 * order stays simple:
 *
 *   wiliwili_network_reset()      app half,  called by the platform half
 *   wiliwili_pre_playback_probe() platform half, called by the app half
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* A text field whose content must not be shown: a password, a token, or a proxy
 * URL that embeds credentials. Borealis' KeyboardKeyDisableBitmask owns bits
 * 0..7 and every one of those forbids a character, so masking a field needs a
 * flag of its own, outside that range. The platform keyboard strips this bit
 * before it maps the rest onto SwkbdKeyDisableBitmask. */
#define KEYBOARD_SENSITIVE_INPUT (1 << 30)

/* Called by the platform after a sleep/wake or a network change: the console can
 * come back on a different network, so curl's shared DNS answers and pooled
 * sockets are dropped and the next request starts from a fresh connection.
 * Implemented in wiliwili/source/utils/platform_api.cpp. */
void wiliwili_network_reset(void);

/* Called by the playback core immediately before it opens a stream: one
 * sanitized line about the console audio route and socket state, recorded in
 * boot.log so a silent stream failure has a cause to read. Implemented in
 * platform/switch2/borealis/switch_platform.cpp. */
void wiliwili_pre_playback_probe(void);

/* 1 when a proxy URL embeds credentials (http://user:pass@host), which makes the
 * URL itself a secret. Pure, implemented in wiliwili/source/utils/platform_api.cpp,
 * and shared with the settings screen that opens the keyboard for it. */
int wiliwili_proxy_is_sensitive(const char* url);

/* Called by the Switch 2 display layer whenever output mode or refresh changes
 * so performance diagnostics never guess physical vs render resolution. */
void wiliwili_report_display_metrics(int refreshHz, int vrr, int outputW, int outputH, int renderW, int renderH,
                                     int docked);

#ifdef __cplusplus
}
#endif
