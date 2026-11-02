#include "Dialogs.r"

resource 'DLOG' (128) {
    { 18, 16, 340, 480 },
    noGrowDocProc,
    visible,
    goAway,
    0,
    128,
    "RetroLink 0.1 LOCAL",
    centerMainScreen
};

resource 'vers' (1) {
    0x00, 0x01, 0x80, 0x00,
    0,
    "0.1",
    "RetroLink 0.1"
};

resource 'DITL' (128) {
    {
        { 24, 12, 40, 450 }, StaticText { enabled, "Opening serial port..." };
        { 42, 12, 58, 322 }, StaticText { enabled, "Role: waiting for REMOTE" };
        { 40, 338, 60, 450 }, Button { enabled, "Take Control" };
        { 64, 12, 80, 46 }, StaticText { enabled, "Port" };
        { 62, 48, 82, 164 }, Button { enabled, "Printer (Serial B)" };
        { 64, 168, 80, 204 }, StaticText { enabled, "Baud" };
        { 62, 206, 82, 278 }, Button { enabled, "9600" };
        { 60, 282, 84, 352 }, Button { enabled, "Apply" };
        { 60, 358, 84, 450 }, Button { enabled, "Close" };
        { 88, 12, 104, 222 }, StaticText { enabled, "LOCAL" };
        { 106, 12, 220, 222 }, UserItem { enabled };
        { 88, 234, 104, 450 }, StaticText { enabled, "REMOTE" };
        { 106, 234, 220, 450 }, UserItem { enabled };
        { 224, 12, 246, 112 }, Button { enabled, "[L] Refresh" };
        { 224, 120, 246, 220 }, Button { enabled, "[R] Refresh" };
        { 284, 12, 298, 450 }, UserItem { enabled };
        { 302, 12, 318, 450 }, StaticText { enabled, "Waiting for the REMOTE RetroLink session." };
        { 224, 234, 246, 354 }, Button { enabled, "[C] Copy REMOTE" };
        { 254, 12, 276, 122 }, Button { enabled, "[N] New Folder" };
        { 254, 128, 276, 228 }, Button { enabled, "[R] Rename" };
        { 254, 234, 276, 334 }, Button { enabled, "[D] Delete" }
    }
};

resource 'DLOG' (129) {
    { 100, 100, 206, 360 },
    dBoxProc,
    visible,
    goAway,
    0,
    129,
    "RetroLink Name",
    centerMainScreen
};

resource 'DITL' (129) {
    {
        { 12, 12, 30, 248 }, StaticText { enabled, "Enter a name:" };
        { 36, 12, 58, 248 }, EditText { enabled, "" };
        { 72, 12, 96, 112 }, Button { enabled, "OK" };
        { 72, 136, 96, 236 }, Button { enabled, "Cancel" }
    }
};

resource 'MENU' (261) {
    261,
    textMenuProc,
    0x7FFFFFFF,
    enabled,
    "Printer (Serial B)",
    {
        "Modem (Serial A)", noIcon, noKey, noMark, plain;
        "Printer (Serial B)", noIcon, noKey, noMark, plain;
    }
};

resource 'MENU' (263) {
    263,
    textMenuProc,
    0x7FFFFFFF,
    enabled,
    "9600",
    {
        "300", noIcon, noKey, noMark, plain;
        "600", noIcon, noKey, noMark, plain;
        "1200", noIcon, noKey, noMark, plain;
        "1800", noIcon, noKey, noMark, plain;
        "2400", noIcon, noKey, noMark, plain;
        "3600", noIcon, noKey, noMark, plain;
        "4800", noIcon, noKey, noMark, plain;
        "7200", noIcon, noKey, noMark, plain;
        "9600", noIcon, noKey, noMark, plain;
        "14400", noIcon, noKey, noMark, plain;
        "19200", noIcon, noKey, noMark, plain;
        "28800", noIcon, noKey, noMark, plain;
        "38400", noIcon, noKey, noMark, plain;
        "57600", noIcon, noKey, noMark, plain;
    }
};

#include "Processes.r"

resource 'SIZE' (-1) {
    reserved,
    acceptSuspendResumeEvents,
    reserved,
    canBackground,
    doesActivateOnFGSwitch,
    backgroundAndForeground,
    dontGetFrontClicks,
    ignoreChildDiedEvents,
    is32BitCompatible,
    notHighLevelEventAware,
    onlyLocalHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    reserved,
    reserved,
    reserved,
    256 * 1024,
    256 * 1024
};
