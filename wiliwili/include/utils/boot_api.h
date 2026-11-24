/*
 * C entry points of the boot log.
 *
 * The Switch 2 service wrapper (userAppInit, before any C++ object exists) and
 * the SDL video backend both have to write to it, so the interface is plain C.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Appends one line to {configDir}/boot.log. Implemented in boot_diagnostics.cpp. */
void wiliwili_boot_note(const char* stage, const char* detail);

/* Writes the reason to the boot log, shows it on a text console and exits.
 * Implemented by the Switch 2 wrapper; callers only use it on Switch 2. */
void wiliwili_boot_panic(const char* reason);

#ifdef __cplusplus
}
#endif
