/*
 * Switch 2 entry point and boot error handler.
 *
 * Same job as borealis' switch_wrapper.c (applet exit lock, sockets, RomFS, the
 * system services the UI reads from), except that every stage is recorded in
 * {configDir}/boot.log and a failure is reported instead of being swallowed:
 * on a console a half initialized service used to end as a black screen with
 * nothing to read afterwards.
 *
 * This file lives in the wiliwili repository rather than in library/borealis so
 * the Borealis submodule stays at its pinned commit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <unistd.h>

#include "utils/boot_api.h"

static int nxlink_sock = -1;

static void bootStage(const char* stage, Result rc) {
    if (R_FAILED(rc)) {
        char message[96];
        snprintf(message, sizeof(message), "failed: 0x%08x", (unsigned)rc);
        wiliwili_boot_note(stage, message);
        return;
    }
    wiliwili_boot_note(stage, "ok");
}

void userAppInit(void) {
    wiliwili_boot_note("boot", "userAppInit");

    appletLockExit();

    // Network: the UI loads several images and API calls at once, so the applet
    // needs more BSD sessions than the default three.
    SocketInitConfig cfg = *socketGetDefaultInitConfig();
    AppletType at        = appletGetAppletType();
    if (at == AppletType_Application || at == AppletType_SystemApplication) {
        cfg.num_bsd_sessions = 12;
        cfg.sb_efficiency    = 8;
    } else {
        cfg.num_bsd_sessions = 2;
        cfg.sb_efficiency    = 1;
    }
    bootStage("socket", socketInitialize(&cfg));

#ifdef DEBUG
    nxlink_sock = nxlinkStdio();
#endif

    // RomFS first: with USE_LIBROMFS=OFF all of resources/ comes from the NRO's
    // own romfs section.
    bootStage("romfs", romfsInit());
    bootStage("pl", plInitialize(PlServiceType_User));
    bootStage("setsys", setsysInitialize());
    bootStage("set", setInitialize());
    bootStage("psm", psmInitialize());
    bootStage("nifm", nifmInitialize(NifmServiceType_User));
    bootStage("lbl", lblInitialize());

    wiliwili_boot_note("boot", "userAppInit done");
}

void userAppExit(void) {
    wiliwili_boot_note("boot", "userAppExit");

    lblExit();
    nifmExit();
    psmExit();
    setExit();
    setsysExit();
    plExit();
    romfsExit();

    if (nxlink_sock != -1) close(nxlink_sock);

    socketExit();
    appletUnlockExit();
}

/*
 * Last resort when the app cannot start: write the reason where it survives the
 * session, then show it on a text console and wait for the user to acknowledge
 * it before the applet closes.
 */
void wiliwili_boot_panic(const char* reason) {
    wiliwili_boot_note("panic", reason != nullptr ? reason : "unknown boot failure");

    consoleInit(NULL);
    printf("\nwiliwili-NS2 could not start.\n\n%s\n\n", reason != nullptr ? reason : "unknown boot failure");
    printf("A copy of this message is in /config/wiliwili-ns2/boot.log\n\n");
    printf("Press + to close.\n\n");
    consoleUpdate(NULL);

    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
        svcSleepThread(1000000000ULL / 60);
    }

    consoleExit(NULL);
    appletUnlockExit();
    exit(EXIT_FAILURE);
}
