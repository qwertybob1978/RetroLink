#include <Dialogs.h>
#include <Events.h>
#include <AppleEvents.h>
#include <Files.h>
#include <Fonts.h>
#include <Menus.h>
#include <Multiverse.h>
#include <Processes.h>
#include <Quickdraw.h>
#include <TextEdit.h>
#include <Windows.h>
#include <string.h>
#include "designer_form.h"
#include "retro_icons.h"

static void DispatchUserControlEvent(DialogPtr dialog, short item);
static void DispatchNonVisualComponentEvent(DialogPtr dialog, short componentIndex, short eventIndex);
static void BridgeFinish(void);
static int HandleDesignerControl(short item);
static void ApplyRadioGroupSelection(short item);
static void CleanupDesignerWidgets(DialogPtr dialog);
static Handle DesignerDialogItem(DialogPtr dialog, short item);
static void InvalidateDesignerDialogItem(DialogPtr dialog, short item);
static DialogPtr gDesignerDialog = NULL;
static short gTelemetryFile = -1;
static short gTelemetryVolumeRefNum = 0;
static int gTelemetryFailed = 0;
static int gTestFailed = 0;
static int gTestSession = 0;
static const unsigned char gMacStudioAgentName[] = {
    14, 'M', 'a', 'c', 'S', 't', 'u', 'd', 'i', 'o', 'A', 'g', 'e', 'n', 't'
};

static OSErr MacStudioFindAgent(ProcessSerialNumber *agentProcess)
{
    ProcessSerialNumber currentProcess;
    unsigned char processName[64];
    ProcessInfoRec processInfo;
    OSErr error;

    if (agentProcess == NULL)
        return paramErr;

    memset(&currentProcess, 0, sizeof(currentProcess));
    for (;;)
    {
        error = GetNextProcess(&currentProcess);
        if (error != noErr)
            return error;

        memset(processName, 0, sizeof(processName));
        memset(&processInfo, 0, sizeof(processInfo));
        processInfo.processInfoLength = sizeof(processInfo);
        processInfo.processName = (StringPtr)processName;
        error = GetProcessInformation(&currentProcess, &processInfo);
        if (error != noErr)
            return error;
        if (processName[0] == 14 &&
            memcmp(processName, gMacStudioAgentName, sizeof(gMacStudioAgentName)) == 0)
        {
            *agentProcess = currentProcess;
            return noErr;
        }
    }
}

OSErr MacStudioDebugLog(unsigned char severity, const char *message)
{
    unsigned char request[515];
    unsigned short length = 0;
    ProcessSerialNumber agentProcess;
    AEAddressDesc target;
    AppleEvent event;
    AppleEvent reply;
    OSErr error;
    OSErr disposeError;
    Boolean targetCreated = false;
    Boolean eventCreated = false;
    Boolean replyCreated = false;

    if (message == NULL)
        return paramErr;
    while (length <= 512 && message[length] != '\0')
        ++length;
    if (length > 512)
        return paramErr;
    for (unsigned short index = 0; index < length; ++index)
    {
        unsigned char value = (unsigned char)message[index];
        if (value != '\t' && (value < 0x20 || value > 0x7E))
            return paramErr;
    }

    error = MacStudioFindAgent(&agentProcess);
    if (error != noErr)
        return error;

    memset(&target, 0, sizeof(target));
    memset(&event, 0, sizeof(event));
    memset(&reply, 0, sizeof(reply));
    error = AECreateDesc(
        typeProcessSerialNumber,
        &agentProcess,
        sizeof(agentProcess),
        &target);
    if (error != noErr)
        goto cleanup;
    targetCreated = true;
    error = AECreateAppleEvent(
        'MSAG',
        'DBUG',
        &target,
        kAutoGenerateReturnID,
        kAnyTransactionID,
        &event);
    if (error != noErr)
        goto cleanup;
    eventCreated = true;

    request[0] = severity;
    request[1] = (unsigned char)(length >> 8);
    request[2] = (unsigned char)length;
    if (length != 0)
        memcpy(request + 3, message, length);
    error = AEPutParamPtr(
        &event,
        keyDirectObject,
        typeChar,
        request,
        3 + length);
    if (error != noErr)
        goto cleanup;
    error = AESend(
        &event,
        &reply,
        kAEWaitReply,
        kAENormalPriority,
        kAEDefaultTimeout,
        (IdleUPP)0,
        (EventFilterUPP)0);
    if (error == noErr)
        replyCreated = true;

cleanup:
    if (replyCreated)
    {
        disposeError = AEDisposeDesc((AEDesc *)&reply);
        if (error == noErr)
            error = disposeError;
    }
    if (eventCreated)
    {
        disposeError = AEDisposeDesc((AEDesc *)&event);
        if (error == noErr)
            error = disposeError;
    }
    if (targetCreated)
    {
        disposeError = AEDisposeDesc((AEDesc *)&target);
        if (error == noErr)
            error = disposeError;
    }
    return error;
}
static void MacStudioDispatchNonVisualEventById(const char *componentId, short eventIndex);
static Boolean gMacStudioSerialOpen1 = false;
static short gMacStudioSerialInputRef1 = 0;
static short gMacStudioSerialOutputRef1 = 0;
static OSErr gMacStudioLastSerialError1 = noErr;
static unsigned char gMacStudioSerialBuffer1[512];
static short gMacStudioProtocolProgress2 = 0;

static void DispatchUserControlEvent(DialogPtr dialog, short item)
{
    switch (item)
    {
        case 3:
            TakeControlClick((RetroLinkForm)dialog, item);
            break;
        case 5:
            MacStudioUserControlClicked(dialog, MACSTUDIO_ID_CONTROLSERIALPORTSELECTOR, item);
            break;
        case 7:
            MacStudioUserControlClicked(dialog, MACSTUDIO_ID_CONTROLSERIALBAUDSELECTOR, item);
            break;
        case 8:
            SerialSettingsClick((RetroLinkForm)dialog, item);
            break;
        case 9:
            SerialCloseClick((RetroLinkForm)dialog, item);
            break;
        case 11:
            MacStudioUserControlClicked(dialog, MACSTUDIO_ID_CONTROLMACFILES, item);
            break;
        case 13:
            HostFilesClick((RetroLinkForm)dialog, item);
            break;
        case 14:
            RefreshMacClick((RetroLinkForm)dialog, item);
            break;
        case 15:
            RefreshHostClick((RetroLinkForm)dialog, item);
            break;
        case 18:
            CopyToDosClick((RetroLinkForm)dialog, item);
            break;
        case 19:
            CreateMacFolderClick((RetroLinkForm)dialog, item);
            break;
        case 20:
            RenameMacEntryClick((RetroLinkForm)dialog, item);
            break;
        case 21:
            DeleteMacEntryClick((RetroLinkForm)dialog, item);
            break;
        default:
            break;
    }
}
static void DispatchNonVisualComponentEvent(DialogPtr dialog, short componentIndex, short eventIndex)
{
    switch (componentIndex)
    {
        case 1:
            switch (eventIndex)
            {
                case 1:
                    SerialDataAvailable((RetroLinkForm)dialog, eventIndex);
                    break;
                case 2:
                    SerialError((RetroLinkForm)dialog, eventIndex);
                    break;
                default:
                    break;
            }
            break;
        case 2:
            switch (eventIndex)
            {
                case 1:
                    TransferProgress((RetroLinkForm)dialog, eventIndex);
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

static MenuHandle gPopupMenu5 = NULL;
static short gPopupSelection5 = 0;
static Str255 gPopupTitle5;
static MenuHandle gPopupMenu7 = NULL;
static short gPopupSelection7 = 0;
static Str255 gPopupTitle7;
static ListHandle gList11 = NULL;
static ListHandle gList13 = NULL;
static short gProgressValue16 = 0;
static short gIconIdByItem[29];
static const short gIconButtonItems[] = {3, 14, 15, 18, 19, 20, 21};
static const short gIconOverlayItems[] = {22, 23, 24, 25, 26, 27, 28};
static pascal void DrawProgressBar16(DialogPtr dialog, short item);
static pascal void DrawIconOverlay(DialogPtr dialog, short item);

static pascal void DrawProgressBar16(DialogPtr dialog, short item)
{
    short itemType;
    Handle itemHandle;
    Rect bounds;
    Rect fillBounds;
    GrafPtr previousPort;

    GetDialogItem(dialog, item, &itemType, &itemHandle, &bounds);
    (void)itemType;
    (void)itemHandle;
    GetPort(&previousPort);
    SetPort((GrafPtr)dialog);
    PenNormal();
    ForeColor(blackColor);
    EraseRect(&bounds);
    FrameRect(&bounds);

    fillBounds = bounds;
    fillBounds.left++;
    fillBounds.top++;
    fillBounds.right--;
    fillBounds.bottom--;
    if (fillBounds.right > fillBounds.left &&
        fillBounds.bottom > fillBounds.top)
    {
        fillBounds.right = (short)(
            fillBounds.left +
            (long)(fillBounds.right - fillBounds.left) * gProgressValue16 / 100);
        if (fillBounds.right > fillBounds.left)
            PaintRect(&fillBounds);
    }
    SetPort(previousPort);
}

static pascal void DrawIconOverlay(DialogPtr dialog, short item)
{
    short itemType;
    Handle itemHandle;
    Rect bounds;
    Rect runBounds;
    GrafPtr previousPort;
    const unsigned short *rows;
    short row;
    short column;

    GetDialogItem(dialog, item, &itemType, &itemHandle, &bounds);
    (void)itemType;
    (void)itemHandle;
    rows = RetroLinkGetIconRows(gIconIdByItem[item]);
    if (rows == NULL)
        return;

    GetPort(&previousPort);
    SetPort((GrafPtr)dialog);
    PenNormal();
    ForeColor(blackColor);
    for (row = 0; row < RETROLINK_ICON_HEIGHT; row++)
    {
        column = 0;
        while (column < RETROLINK_ICON_WIDTH)
        {
            short startColumn;
            if ((rows[row] & (0x8000U >> column)) == 0)
            {
                column++;
                continue;
            }
            startColumn = column;
            while (column < RETROLINK_ICON_WIDTH &&
                   (rows[row] & (0x8000U >> column)) != 0)
                column++;
            SetRect(
                &runBounds,
                (short)(bounds.left + startColumn),
                (short)(bounds.top + row),
                (short)(bounds.left + column),
                (short)(bounds.top + row + 1));
            PaintRect(&runBounds);
        }
    }
    SetPort(previousPort);
}

static int BridgeWriteRaw(const char *text)
{
    long count = (long)strlen(text);
    if (gTelemetryFile < 0 || gTelemetryFailed)
        return 0;
    if (FSWrite(gTelemetryFile, &count, text) != noErr ||
        count != (long)strlen(text))
    {
        gTelemetryFailed = 1;
        return 0;
    }
    return 1;
}

static void BridgeWriteField(const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    char encoded[256];
    short encodedLength = 0;
    while (*text != '\0')
    {
        unsigned char value = (unsigned char)*text++;
        short sequenceLength;
        if ((value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z') ||
            (value >= '0' && value <= '9') ||
            value == '.' || value == '_' || value == '-')
        {
            sequenceLength = 1;
        }
        else
        {
            sequenceLength = 3;
        }
        if (encodedLength + sequenceLength >= sizeof(encoded))
        {
            encoded[encodedLength] = '\0';
            if (!BridgeWriteRaw(encoded))
                return;
            encodedLength = 0;
        }
        if (sequenceLength == 1)
            encoded[encodedLength++] = (char)value;
        else
        {
            encoded[encodedLength++] = '%';
            encoded[encodedLength++] = hex[value >> 4];
            encoded[encodedLength++] = hex[value & 15];
        }
    }
    if (encodedLength > 0)
    {
        encoded[encodedLength] = '\0';
        BridgeWriteRaw(encoded);
    }
}

static void BridgeWriteNumber(int value)
{
    char digits[12];
    int cursor = 11;
    digits[cursor] = '\0';
    do
    {
        digits[--cursor] = (char)('0' + (value % 10));
        value /= 10;
    } while (value > 0 && cursor > 0);
    BridgeWriteRaw(&digits[cursor]);
}

static void BridgeWriteLine(void)
{
    BridgeWriteRaw("\r");
}

static void BridgeWriteError(const char *code, const char *message)
{
    BridgeWriteRaw("ERROR|");
    BridgeWriteField(code);
    BridgeWriteRaw("|");
    BridgeWriteField(message);
    BridgeWriteLine();
    gTestFailed = 1;
}

static void BridgeWriteEvent(const char *action, const char *id, const char *outcome)
{
    BridgeWriteRaw("EVENT|");
    BridgeWriteField(action);
    BridgeWriteRaw("|");
    BridgeWriteField(id);
    BridgeWriteRaw("|");
    BridgeWriteField(outcome);
    BridgeWriteLine();
    if (strcmp(outcome, "HANDLED") != 0)
        gTestFailed = 1;
}

static Handle DesignerDialogItem(DialogPtr dialog, short item)
{
    short itemType;
    Handle itemHandle;
    Rect itemBounds;
    GetDialogItem(dialog, item, &itemType, &itemHandle, &itemBounds);
    return itemHandle;
}

static short BridgeControlItem(const char *id)
{
    if (strcmp(id, "ConnectionStatus") == 0)
        return 1;
    if (strcmp(id, "RoleStatus") == 0)
        return 2;
    if (strcmp(id, "TakeControlButton") == 0)
        return 3;
    if (strcmp(id, "SerialPortLabel") == 0)
        return 4;
    if (strcmp(id, "SerialPortSelector") == 0)
        return 5;
    if (strcmp(id, "SerialBaudLabel") == 0)
        return 6;
    if (strcmp(id, "SerialBaudSelector") == 0)
        return 7;
    if (strcmp(id, "SerialSettingsButton") == 0)
        return 8;
    if (strcmp(id, "SerialCloseButton") == 0)
        return 9;
    if (strcmp(id, "MacVolumesLabel") == 0)
        return 10;
    if (strcmp(id, "MacFiles") == 0)
        return 11;
    if (strcmp(id, "HostFilesLabel") == 0)
        return 12;
    if (strcmp(id, "HostFiles") == 0)
        return 13;
    if (strcmp(id, "RefreshMacButton") == 0)
        return 14;
    if (strcmp(id, "RefreshHostButton") == 0)
        return 15;
    if (strcmp(id, "TransferProgressBar") == 0)
        return 16;
    if (strcmp(id, "ActionStatus") == 0)
        return 17;
    if (strcmp(id, "CopyToHostButton") == 0)
        return 18;
    if (strcmp(id, "CreateFolderButton") == 0)
        return 19;
    if (strcmp(id, "RenameEntryButton") == 0)
        return 20;
    if (strcmp(id, "DeleteEntryButton") == 0)
        return 21;
    return 0;
}

static short IconOverlayItemForButton(short buttonItem)
{
    short index;
    for (index = 0;
         index < (short)(sizeof(gIconButtonItems) / sizeof(gIconButtonItems[0]));
         index++)
    {
        if (gIconButtonItems[index] == buttonItem)
            return gIconOverlayItems[index];
    }
    return 0;
}

short MacStudioSetControlIcon(
    DialogPtr dialog,
    const char *controlId,
    short iconId)
{
    short buttonItem;
    short overlayItem;
    short buttonType;
    short overlayType;
    Handle buttonHandle;
    Handle overlayHandle;
    Rect buttonBounds;
    Rect iconBounds;

    if (dialog == NULL || controlId == NULL ||
        RetroLinkGetIconRows(iconId) == NULL)
        return paramErr;
    buttonItem = BridgeControlItem(controlId);
    overlayItem = IconOverlayItemForButton(buttonItem);
    if (overlayItem == 0)
        return paramErr;

    GetDialogItem(dialog, buttonItem, &buttonType, &buttonHandle, &buttonBounds);
    if ((buttonType & ctrlItem) == 0 || buttonHandle == NULL ||
        buttonBounds.right - buttonBounds.left < RETROLINK_ICON_WIDTH ||
        buttonBounds.bottom - buttonBounds.top < RETROLINK_ICON_HEIGHT)
        return paramErr;
    GetDialogItem(dialog, overlayItem, &overlayType, &overlayHandle, &iconBounds);
    (void)overlayHandle;

    iconBounds.left = (short)(buttonBounds.left +
        ((buttonBounds.right - buttonBounds.left) - RETROLINK_ICON_WIDTH) / 2);
    iconBounds.top = (short)(buttonBounds.top +
        ((buttonBounds.bottom - buttonBounds.top) - RETROLINK_ICON_HEIGHT) / 2);
    iconBounds.right = (short)(iconBounds.left + RETROLINK_ICON_WIDTH);
    iconBounds.bottom = (short)(iconBounds.top + RETROLINK_ICON_HEIGHT);
    gIconIdByItem[overlayItem] = iconId;
    SetDialogItem(
        dialog,
        overlayItem,
        overlayType,
        (Handle)DrawIconOverlay,
        &iconBounds);
    InvalidateDesignerDialogItem(dialog, overlayItem);
    return noErr;
}

static const char *BridgeControlId(short item)
{
    switch (item)
    {
        case 1:
            return "ConnectionStatus";
        case 2:
            return "RoleStatus";
        case 3:
            return "TakeControlButton";
        case 4:
            return "SerialPortLabel";
        case 5:
            return "SerialPortSelector";
        case 6:
            return "SerialBaudLabel";
        case 7:
            return "SerialBaudSelector";
        case 8:
            return "SerialSettingsButton";
        case 9:
            return "SerialCloseButton";
        case 10:
            return "MacVolumesLabel";
        case 11:
            return "MacFiles";
        case 12:
            return "HostFilesLabel";
        case 13:
            return "HostFiles";
        case 14:
            return "RefreshMacButton";
        case 15:
            return "RefreshHostButton";
        case 16:
            return "TransferProgressBar";
        case 17:
            return "ActionStatus";
        case 18:
            return "CopyToHostButton";
        case 19:
            return "CreateFolderButton";
        case 20:
            return "RenameEntryButton";
        case 21:
            return "DeleteEntryButton";
        default:
            return "";
    }
}

static short TrackIconButton(DialogPtr dialog, Point globalMouse)
{
    Point localMouse = globalMouse;
    GrafPtr previousPort;
    short index;
    short buttonItem;
    short itemType;
    Handle itemHandle;
    Rect bounds;
    short trackedValue;
    int result;
    const char *controlId;

    GetPort(&previousPort);
    SetPort((GrafPtr)dialog);
    GlobalToLocal(&localMouse);
    for (index = 0;
         index < (short)(sizeof(gIconButtonItems) / sizeof(gIconButtonItems[0]));
         index++)
    {
        buttonItem = gIconButtonItems[index];
        GetDialogItem(dialog, buttonItem, &itemType, &itemHandle, &bounds);
        if (!PtInRect(localMouse, &bounds))
            continue;

        trackedValue = TrackControl(
            (ControlHandle)itemHandle,
            globalMouse,
            (ControlActionUPP)0);
        SetPort((GrafPtr)dialog);
        DrawIconOverlay(dialog, gIconOverlayItems[index]);
        SetPort(previousPort);
        if (trackedValue != 0)
        {
            result = HandleDesignerControl(buttonItem);
            if (result == 0)
                DispatchUserControlEvent(dialog, buttonItem);
            ApplyRadioGroupSelection(buttonItem);
            controlId = BridgeControlId(buttonItem);
            if (*controlId != '\0')
                BridgeWriteEvent(
                    "CLICK",
                    controlId,
                    result == 0 ? "HANDLED" : "UNHANDLED");
        }
        return 1;
    }
    SetPort(previousPort);
    return 0;
}

static void InvalidateDesignerDialogItem(DialogPtr dialog, short item)
{
    short itemType;
    Handle itemHandle;
    Rect itemBounds;
    GrafPtr previousPort;
    GetDialogItem(dialog, item, &itemType, &itemHandle, &itemBounds);
    GetPort(&previousPort);
    SetPort((GrafPtr)dialog);
    InvalRect(&itemBounds);
    SetPort(previousPort);
}

short MacStudioGetControlText(DialogPtr dialog, const char *controlId, unsigned char *text)
{
    short item;
    short itemType;
    Handle itemHandle;
    Rect itemBounds;
    if (dialog == NULL || controlId == NULL || text == NULL)
        return paramErr;
    item = BridgeControlItem(controlId);
    if (item == 0)
        return paramErr;
    GetDialogItem(dialog, item, &itemType, &itemHandle, &itemBounds);
    if (itemHandle == NULL ||
        ((itemType & ctrlItem) == 0 && itemType != statText && itemType != editText))
        return paramErr;
    if ((itemType & ctrlItem) != 0)
        GetControlTitle((ControlHandle)itemHandle, text);
    else
        GetDialogItemText(itemHandle, text);
    return noErr;
}

short MacStudioSetControlText(DialogPtr dialog, const char *controlId, const unsigned char *text)
{
    short item;
    short itemType;
    Handle itemHandle;
    Rect itemBounds;
    if (dialog == NULL || controlId == NULL || text == NULL)
        return paramErr;
    item = BridgeControlItem(controlId);
    if (item == 0)
        return paramErr;
    GetDialogItem(dialog, item, &itemType, &itemHandle, &itemBounds);
    if (itemHandle == NULL ||
        ((itemType & ctrlItem) == 0 && itemType != statText && itemType != editText))
        return paramErr;
    if ((itemType & ctrlItem) != 0)
        SetControlTitle((ControlHandle)itemHandle, text);
    else
        SetDialogItemText(itemHandle, text);
    InvalidateDesignerDialogItem(dialog, item);
    return noErr;
}

short MacStudioGetControlValue(DialogPtr dialog, const char *controlId, short *value)
{
    short item;
    if (value == NULL)
        return paramErr;
    if (dialog == NULL || controlId == NULL)
        return paramErr;
    item = BridgeControlItem(controlId);
    if (item == 0)
        return paramErr;
    switch (item)
    {
        case 1:
        {
            break;
        }
        case 2:
        {
            break;
        }
        case 3:
        {
            break;
        }
        case 4:
        {
            break;
        }
        case 5:
        {
    *value = gPopupSelection5 - 1;
    return noErr;
            break;
        }
        case 6:
        {
            break;
        }
        case 7:
        {
    *value = gPopupSelection7 - 1;
    return noErr;
            break;
        }
        case 8:
        {
            break;
        }
        case 9:
        {
            break;
        }
        case 10:
        {
            break;
        }
        case 11:
        {
    Cell selectedCell;
    if (gList11 == NULL) return paramErr;
    selectedCell.h = 0;
    selectedCell.v = 0;
    *value = LGetSelect(true, &selectedCell, gList11) ? selectedCell.v : -1;
    return noErr;
            break;
        }
        case 12:
        {
            break;
        }
        case 13:
        {
    Cell selectedCell;
    if (gList13 == NULL) return paramErr;
    selectedCell.h = 0;
    selectedCell.v = 0;
    *value = LGetSelect(true, &selectedCell, gList13) ? selectedCell.v : -1;
    return noErr;
            break;
        }
        case 14:
        {
            break;
        }
        case 15:
        {
            break;
        }
        case 16:
        {
            *value = gProgressValue16;
            return noErr;
        }
        case 17:
        {
            break;
        }
        default:
            break;
    }
    return paramErr;
}

short MacStudioSetControlValue(DialogPtr dialog, const char *controlId, short value)
{
    short item;
    if (dialog == NULL || controlId == NULL)
        return paramErr;
    item = BridgeControlItem(controlId);
    if (item == 0)
        return paramErr;
    switch (item)
    {
        case 1:
        {
            break;
        }
        case 2:
        {
            break;
        }
        case 3:
        {
            break;
        }
        case 4:
        {
            break;
        }
        case 5:
        {
    if (value < 0 || value >= 2 || gPopupMenu5 == NULL)
        return paramErr;
    gPopupSelection5 = value + 1;
    GetMenuItemText(gPopupMenu5, gPopupSelection5, gPopupTitle5);
    SetControlTitle((ControlHandle)DesignerDialogItem(dialog, 5), gPopupTitle5);
    InvalidateDesignerDialogItem(dialog, item);
    return noErr;
            break;
        }
        case 6:
        {
            break;
        }
        case 7:
        {
    if (value < 0 || value >= 14 || gPopupMenu7 == NULL)
        return paramErr;
    gPopupSelection7 = value + 1;
    GetMenuItemText(gPopupMenu7, gPopupSelection7, gPopupTitle7);
    SetControlTitle((ControlHandle)DesignerDialogItem(dialog, 7), gPopupTitle7);
    InvalidateDesignerDialogItem(dialog, item);
    return noErr;
            break;
        }
        case 8:
        {
            break;
        }
        case 9:
        {
            break;
        }
        case 10:
        {
            break;
        }
        case 11:
        {
    Cell selectedCell;
    short row;
    if (value < -1 || value >= 1 || gList11 == NULL)
        return paramErr;
    selectedCell.h = 0;
    if (value == -1)
    {
        for (row = 0; row < 1; row++)
        {
            selectedCell.v = row;
            LSetSelect(false, selectedCell, gList11);
        }
    }
    else
    {
        selectedCell.v = value;
        LSetSelect(true, selectedCell, gList11);
    }
    InvalidateDesignerDialogItem(dialog, item);
    return noErr;
            break;
        }
        case 12:
        {
            break;
        }
        case 13:
        {
    Cell selectedCell;
    short row;
    if (value < -1 || value >= 1 || gList13 == NULL)
        return paramErr;
    selectedCell.h = 0;
    if (value == -1)
    {
        for (row = 0; row < 1; row++)
        {
            selectedCell.v = row;
            LSetSelect(false, selectedCell, gList13);
        }
    }
    else
    {
        selectedCell.v = value;
        LSetSelect(true, selectedCell, gList13);
    }
    InvalidateDesignerDialogItem(dialog, item);
    return noErr;
            break;
        }
        case 14:
        {
            break;
        }
        case 15:
        {
            break;
        }
        case 16:
        {
            if (value < 0 || value > 100)
                return paramErr;
            gProgressValue16 = value;
            DrawProgressBar16(dialog, item);
            return noErr;
        }
        case 17:
        {
            break;
        }
        default:
            break;
    }
    return paramErr;
}

short MacStudioGetControlBounds(DialogPtr dialog, const char *controlId, Rect *bounds)
{
    short item;
    short itemType;
    Handle itemHandle;
    if (dialog == NULL || controlId == NULL || bounds == NULL)
        return paramErr;
    item = BridgeControlItem(controlId);
    if (item == 0)
        return paramErr;
    GetDialogItem(dialog, item, &itemType, &itemHandle, bounds);
    bounds->top -= 20;
    bounds->bottom -= 20;
    return noErr;
}

short MacStudioSetControlBounds(DialogPtr dialog, const char *controlId, const Rect *bounds)
{
    short item;
    short itemType;
    Handle itemHandle;
    Rect oldBounds;
    Rect itemBounds;
    GrafPtr previousPort;
    short result = noErr;
    if (dialog == NULL || controlId == NULL || bounds == NULL)
        return paramErr;
    if (bounds->left < 0 || bounds->top < 0 || bounds->right <= bounds->left || bounds->bottom <= bounds->top || bounds->right > 462 || bounds->bottom > 316)
        return paramErr;
    item = BridgeControlItem(controlId);
    if (item == 0)
        return paramErr;
    GetDialogItem(dialog, item, &itemType, &itemHandle, &oldBounds);
    if (itemHandle == NULL && (itemType & ctrlItem) != 0)
        return paramErr;
    itemBounds = *bounds;
    itemBounds.top += 20;
    itemBounds.bottom += 20;
    GetPort(&previousPort);
    SetPort((GrafPtr)dialog);
    if ((itemType & ctrlItem) != 0)
    {
        MoveControl((ControlHandle)itemHandle, itemBounds.left, itemBounds.top);
        SizeControl((ControlHandle)itemHandle, (short)(itemBounds.right - itemBounds.left), (short)(itemBounds.bottom - itemBounds.top));
        SetDialogItem(dialog, item, itemType, itemHandle, &itemBounds);
    }
    else
        SetDialogItem(dialog, item, itemType, itemHandle, &itemBounds);
    switch (item)
    {
        case 1:
            break;
        case 2:
            break;
        case 3:
            break;
        case 4:
            break;
        case 5:
            break;
        case 6:
            break;
        case 7:
            break;
        case 8:
            break;
        case 9:
            break;
        case 10:
            break;
        case 11:
            if (gList11 == NULL) { result = paramErr; break; }
            LSize((short)(itemBounds.right - itemBounds.left), (short)(itemBounds.bottom - itemBounds.top), gList11);
            (**gList11).rView = itemBounds;
            LUpdate(((WindowPtr)dialog)->visRgn, gList11);
            break;
        case 12:
            break;
        case 13:
            if (gList13 == NULL) { result = paramErr; break; }
            LSize((short)(itemBounds.right - itemBounds.left), (short)(itemBounds.bottom - itemBounds.top), gList13);
            (**gList13).rView = itemBounds;
            LUpdate(((WindowPtr)dialog)->visRgn, gList13);
            break;
        case 14:
            break;
        case 15:
            break;
        case 16:
            DrawProgressBar16(dialog, item);
            break;
        case 17:
            break;
        case 18:
            break;
        default:
            result = paramErr;
            break;
    }
    if (result == noErr)
    {
        InvalRect(&oldBounds);
        InvalRect(&itemBounds);
    }
    SetPort(previousPort);
    return result;
}

short MacStudioGetControlConnectionStatusBounds(DialogPtr dialog, Rect *bounds)
{
    return MacStudioGetControlBounds(dialog, MACSTUDIO_ID_CONTROLCONNECTIONSTATUS, bounds);
}

short MacStudioSetControlConnectionStatusBounds(DialogPtr dialog, const Rect *bounds)
{
    return MacStudioSetControlBounds(dialog, MACSTUDIO_ID_CONTROLCONNECTIONSTATUS, bounds);
}

short MacStudioGetControlConnectionStatusText(DialogPtr dialog, unsigned char *text)
{
    return MacStudioGetControlText(dialog, MACSTUDIO_ID_CONTROLCONNECTIONSTATUS, text);
}

short MacStudioSetControlConnectionStatusText(DialogPtr dialog, const unsigned char *text)
{
    return MacStudioSetControlText(dialog, MACSTUDIO_ID_CONTROLCONNECTIONSTATUS, text);
}

short MacStudioGetControlRoleStatusBounds(DialogPtr dialog, Rect *bounds)
{
    return MacStudioGetControlBounds(dialog, MACSTUDIO_ID_CONTROLROLESTATUS, bounds);
}

short MacStudioSetControlRoleStatusBounds(DialogPtr dialog, const Rect *bounds)
{
    return MacStudioSetControlBounds(dialog, MACSTUDIO_ID_CONTROLROLESTATUS, bounds);
}

short MacStudioGetControlRoleStatusText(DialogPtr dialog, unsigned char *text)
{
    return MacStudioGetControlText(dialog, MACSTUDIO_ID_CONTROLROLESTATUS, text);
}

short MacStudioSetControlRoleStatusText(DialogPtr dialog, const unsigned char *text)
{
    return MacStudioSetControlText(dialog, MACSTUDIO_ID_CONTROLROLESTATUS, text);
}

short MacStudioGetControlTakeControlButtonBounds(DialogPtr dialog, Rect *bounds)
{
    return MacStudioGetControlBounds(dialog, MACSTUDIO_ID_CONTROLTAKECONTROLBUTTON, bounds);
}

short MacStudioSetControlTakeControlButtonBounds(DialogPtr dialog, const Rect *bounds)
{
    return MacStudioSetControlBounds(dialog, MACSTUDIO_ID_CONTROLTAKECONTROLBUTTON, bounds);
}

short MacStudioGetControlTakeControlButtonText(DialogPtr dialog, unsigned char *text)
{
    return MacStudioGetControlText(dialog, MACSTUDIO_ID_CONTROLTAKECONTROLBUTTON, text);
}

short MacStudioSetControlTakeControlButtonText(DialogPtr dialog, const unsigned char *text)
{
    return MacStudioSetControlText(dialog, MACSTUDIO_ID_CONTROLTAKECONTROLBUTTON, text);
}

short MacStudioGetControlSerialPortLabelBounds(DialogPtr dialog, Rect *bounds)
{
    return MacStudioGetControlBounds(dialog, MACSTUDIO_ID_CONTROLSERIALPORTLABEL, bounds);
}

short MacStudioSetControlSerialPortLabelBounds(DialogPtr dialog, const Rect *bounds)
{
    return MacStudioSetControlBounds(dialog, MACSTUDIO_ID_CONTROLSERIALPORTLABEL, bounds);
}

short MacStudioGetControlSerialPortLabelText(DialogPtr dialog, unsigned char *text)
{
    return MacStudioGetControlText(dialog, MACSTUDIO_ID_CONTROLSERIALPORTLABEL, text);
}

short MacStudioSetControlSerialPortLabelText(DialogPtr dialog, const unsigned char *text)
{
    return MacStudioSetControlText(dialog, MACSTUDIO_ID_CONTROLSERIALPORTLABEL, text);
}

short MacStudioGetControlSerialPortSelectorBounds(DialogPtr dialog, Rect *bounds)
{
    return MacStudioGetControlBounds(dialog, MACSTUDIO_ID_CONTROLSERIALPORTSELECTOR, bounds);
}

short MacStudioSetControlSerialPortSelectorBounds(DialogPtr dialog, const Rect *bounds)
{
    return MacStudioSetControlBounds(dialog, MACSTUDIO_ID_CONTROLSERIALPORTSELECTOR, bounds);
}

short MacStudioGetControlSerialPortSelectorText(DialogPtr dialog, unsigned char *text)
{
    return MacStudioGetControlText(dialog, MACSTUDIO_ID_CONTROLSERIALPORTSELECTOR, text);
}

short MacStudioSetControlSerialPortSelectorText(DialogPtr dialog, const unsigned char *text)
{
    return MacStudioSetControlText(dialog, MACSTUDIO_ID_CONTROLSERIALPORTSELECTOR, text);
}

short MacStudioGetControlSerialPortSelectorValue(DialogPtr dialog, short *value)
{
    return MacStudioGetControlValue(dialog, MACSTUDIO_ID_CONTROLSERIALPORTSELECTOR, value);
}

short MacStudioSetControlSerialPortSelectorValue(DialogPtr dialog, short value)
{
    return MacStudioSetControlValue(dialog, MACSTUDIO_ID_CONTROLSERIALPORTSELECTOR, value);
}

short MacStudioGetControlSerialBaudLabelBounds(DialogPtr dialog, Rect *bounds)
{
    return MacStudioGetControlBounds(dialog, MACSTUDIO_ID_CONTROLSERIALBAUDLABEL, bounds);
}

short MacStudioSetControlSerialBaudLabelBounds(DialogPtr dialog, const Rect *bounds)
{
    return MacStudioSetControlBounds(dialog, MACSTUDIO_ID_CONTROLSERIALBAUDLABEL, bounds);
}

short MacStudioGetControlSerialBaudLabelText(DialogPtr dialog, unsigned char *text)
{
    return MacStudioGetControlText(dialog, MACSTUDIO_ID_CONTROLSERIALBAUDLABEL, text);
}

short MacStudioSetControlSerialBaudLabelText(DialogPtr dialog, const unsigned char *text)
{
    return MacStudioSetControlText(dialog, MACSTUDIO_ID_CONTROLSERIALBAUDLABEL, text);
}

short MacStudioGetControlSerialBaudSelectorBounds(DialogPtr dialog, Rect *bounds)
{
    return MacStudioGetControlBounds(dialog, MACSTUDIO_ID_CONTROLSERIALBAUDSELECTOR, bounds);
}

short MacStudioSetControlSerialBaudSelectorBounds(DialogPtr dialog, const Rect *bounds)
{
    return MacStudioSetControlBounds(dialog, MACSTUDIO_ID_CONTROLSERIALBAUDSELECTOR, bounds);
}

short MacStudioGetControlSerialBaudSelectorText(DialogPtr dialog, unsigned char *text)
{
    return MacStudioGetControlText(dialog, MACSTUDIO_ID_CONTROLSERIALBAUDSELECTOR, text);
}

short MacStudioSetControlSerialBaudSelectorText(DialogPtr dialog, const unsigned char *text)
{
    return MacStudioSetControlText(dialog, MACSTUDIO_ID_CONTROLSERIALBAUDSELECTOR, text);
}

short MacStudioGetControlSerialBaudSelectorValue(DialogPtr dialog, short *value)
{
    return MacStudioGetControlValue(dialog, MACSTUDIO_ID_CONTROLSERIALBAUDSELECTOR, value);
}

short MacStudioSetControlSerialBaudSelectorValue(DialogPtr dialog, short value)
{
    return MacStudioSetControlValue(dialog, MACSTUDIO_ID_CONTROLSERIALBAUDSELECTOR, value);
}

short MacStudioGetControlSerialSettingsButtonBounds(DialogPtr dialog, Rect *bounds)
{
    return MacStudioGetControlBounds(dialog, MACSTUDIO_ID_CONTROLSERIALSETTINGSBUTTON, bounds);
}

short MacStudioSetControlSerialSettingsButtonBounds(DialogPtr dialog, const Rect *bounds)
{
    return MacStudioSetControlBounds(dialog, MACSTUDIO_ID_CONTROLSERIALSETTINGSBUTTON, bounds);
}

short MacStudioGetControlSerialSettingsButtonText(DialogPtr dialog, unsigned char *text)
{
    return MacStudioGetControlText(dialog, MACSTUDIO_ID_CONTROLSERIALSETTINGSBUTTON, text);
}

short MacStudioSetControlSerialSettingsButtonText(DialogPtr dialog, const unsigned char *text)
{
    return MacStudioSetControlText(dialog, MACSTUDIO_ID_CONTROLSERIALSETTINGSBUTTON, text);
}

short MacStudioGetControlSerialCloseButtonBounds(DialogPtr dialog, Rect *bounds)
{
    return MacStudioGetControlBounds(dialog, MACSTUDIO_ID_CONTROLSERIALCLOSEBUTTON, bounds);
}

short MacStudioSetControlSerialCloseButtonBounds(DialogPtr dialog, const Rect *bounds)
{
    return MacStudioSetControlBounds(dialog, MACSTUDIO_ID_CONTROLSERIALCLOSEBUTTON, bounds);
}

short MacStudioGetControlSerialCloseButtonText(DialogPtr dialog, unsigned char *text)
{
    return MacStudioGetControlText(dialog, MACSTUDIO_ID_CONTROLSERIALCLOSEBUTTON, text);
}

short MacStudioSetControlSerialCloseButtonText(DialogPtr dialog, const unsigned char *text)
{
    return MacStudioSetControlText(dialog, MACSTUDIO_ID_CONTROLSERIALCLOSEBUTTON, text);
}

short MacStudioGetControlMacVolumesLabelBounds(DialogPtr dialog, Rect *bounds)
{
    return MacStudioGetControlBounds(dialog, MACSTUDIO_ID_CONTROLMACVOLUMESLABEL, bounds);
}

short MacStudioSetControlMacVolumesLabelBounds(DialogPtr dialog, const Rect *bounds)
{
    return MacStudioSetControlBounds(dialog, MACSTUDIO_ID_CONTROLMACVOLUMESLABEL, bounds);
}

short MacStudioGetControlMacVolumesLabelText(DialogPtr dialog, unsigned char *text)
{
    return MacStudioGetControlText(dialog, MACSTUDIO_ID_CONTROLMACVOLUMESLABEL, text);
}

short MacStudioSetControlMacVolumesLabelText(DialogPtr dialog, const unsigned char *text)
{
    return MacStudioSetControlText(dialog, MACSTUDIO_ID_CONTROLMACVOLUMESLABEL, text);
}

short MacStudioGetControlMacFilesBounds(DialogPtr dialog, Rect *bounds)
{
    return MacStudioGetControlBounds(dialog, MACSTUDIO_ID_CONTROLMACFILES, bounds);
}

short MacStudioSetControlMacFilesBounds(DialogPtr dialog, const Rect *bounds)
{
    return MacStudioSetControlBounds(dialog, MACSTUDIO_ID_CONTROLMACFILES, bounds);
}

short MacStudioGetControlMacFilesValue(DialogPtr dialog, short *value)
{
    return MacStudioGetControlValue(dialog, MACSTUDIO_ID_CONTROLMACFILES, value);
}

short MacStudioSetControlMacFilesValue(DialogPtr dialog, short value)
{
    return MacStudioSetControlValue(dialog, MACSTUDIO_ID_CONTROLMACFILES, value);
}

short MacStudioGetControlHostFilesLabelBounds(DialogPtr dialog, Rect *bounds)
{
    return MacStudioGetControlBounds(dialog, MACSTUDIO_ID_CONTROLHOSTFILESLABEL, bounds);
}

short MacStudioSetControlHostFilesLabelBounds(DialogPtr dialog, const Rect *bounds)
{
    return MacStudioSetControlBounds(dialog, MACSTUDIO_ID_CONTROLHOSTFILESLABEL, bounds);
}

short MacStudioGetControlHostFilesLabelText(DialogPtr dialog, unsigned char *text)
{
    return MacStudioGetControlText(dialog, MACSTUDIO_ID_CONTROLHOSTFILESLABEL, text);
}

short MacStudioSetControlHostFilesLabelText(DialogPtr dialog, const unsigned char *text)
{
    return MacStudioSetControlText(dialog, MACSTUDIO_ID_CONTROLHOSTFILESLABEL, text);
}

short MacStudioGetControlHostFilesBounds(DialogPtr dialog, Rect *bounds)
{
    return MacStudioGetControlBounds(dialog, MACSTUDIO_ID_CONTROLHOSTFILES, bounds);
}

short MacStudioSetControlHostFilesBounds(DialogPtr dialog, const Rect *bounds)
{
    return MacStudioSetControlBounds(dialog, MACSTUDIO_ID_CONTROLHOSTFILES, bounds);
}

short MacStudioGetControlHostFilesValue(DialogPtr dialog, short *value)
{
    return MacStudioGetControlValue(dialog, MACSTUDIO_ID_CONTROLHOSTFILES, value);
}

short MacStudioSetControlHostFilesValue(DialogPtr dialog, short value)
{
    return MacStudioSetControlValue(dialog, MACSTUDIO_ID_CONTROLHOSTFILES, value);
}

short MacStudioGetControlRefreshMacButtonBounds(DialogPtr dialog, Rect *bounds)
{
    return MacStudioGetControlBounds(dialog, MACSTUDIO_ID_CONTROLREFRESHMACBUTTON, bounds);
}

short MacStudioSetControlRefreshMacButtonBounds(DialogPtr dialog, const Rect *bounds)
{
    return MacStudioSetControlBounds(dialog, MACSTUDIO_ID_CONTROLREFRESHMACBUTTON, bounds);
}

short MacStudioGetControlRefreshMacButtonText(DialogPtr dialog, unsigned char *text)
{
    return MacStudioGetControlText(dialog, MACSTUDIO_ID_CONTROLREFRESHMACBUTTON, text);
}

short MacStudioSetControlRefreshMacButtonText(DialogPtr dialog, const unsigned char *text)
{
    return MacStudioSetControlText(dialog, MACSTUDIO_ID_CONTROLREFRESHMACBUTTON, text);
}

short MacStudioGetControlRefreshHostButtonBounds(DialogPtr dialog, Rect *bounds)
{
    return MacStudioGetControlBounds(dialog, MACSTUDIO_ID_CONTROLREFRESHHOSTBUTTON, bounds);
}

short MacStudioSetControlRefreshHostButtonBounds(DialogPtr dialog, const Rect *bounds)
{
    return MacStudioSetControlBounds(dialog, MACSTUDIO_ID_CONTROLREFRESHHOSTBUTTON, bounds);
}

short MacStudioGetControlRefreshHostButtonText(DialogPtr dialog, unsigned char *text)
{
    return MacStudioGetControlText(dialog, MACSTUDIO_ID_CONTROLREFRESHHOSTBUTTON, text);
}

short MacStudioSetControlRefreshHostButtonText(DialogPtr dialog, const unsigned char *text)
{
    return MacStudioSetControlText(dialog, MACSTUDIO_ID_CONTROLREFRESHHOSTBUTTON, text);
}

short MacStudioGetControlCopyToHostButtonBounds(DialogPtr dialog, Rect *bounds)
{
    return MacStudioGetControlBounds(dialog, MACSTUDIO_ID_CONTROLCOPYTOHOSTBUTTON, bounds);
}

short MacStudioSetControlCopyToHostButtonBounds(DialogPtr dialog, const Rect *bounds)
{
    return MacStudioSetControlBounds(dialog, MACSTUDIO_ID_CONTROLCOPYTOHOSTBUTTON, bounds);
}

short MacStudioGetControlCopyToHostButtonText(DialogPtr dialog, unsigned char *text)
{
    return MacStudioGetControlText(dialog, MACSTUDIO_ID_CONTROLCOPYTOHOSTBUTTON, text);
}

short MacStudioSetControlCopyToHostButtonText(DialogPtr dialog, const unsigned char *text)
{
    return MacStudioSetControlText(dialog, MACSTUDIO_ID_CONTROLCOPYTOHOSTBUTTON, text);
}

short MacStudioGetControlTransferProgressBarBounds(DialogPtr dialog, Rect *bounds)
{
    return MacStudioGetControlBounds(dialog, MACSTUDIO_ID_CONTROLTRANSFERPROGRESSBAR, bounds);
}

short MacStudioSetControlTransferProgressBarBounds(DialogPtr dialog, const Rect *bounds)
{
    return MacStudioSetControlBounds(dialog, MACSTUDIO_ID_CONTROLTRANSFERPROGRESSBAR, bounds);
}

short MacStudioGetControlTransferProgressBarValue(DialogPtr dialog, short *value)
{
    return MacStudioGetControlValue(dialog, MACSTUDIO_ID_CONTROLTRANSFERPROGRESSBAR, value);
}

short MacStudioSetControlTransferProgressBarValue(DialogPtr dialog, short value)
{
    return MacStudioSetControlValue(dialog, MACSTUDIO_ID_CONTROLTRANSFERPROGRESSBAR, value);
}

short MacStudioGetControlActionStatusBounds(DialogPtr dialog, Rect *bounds)
{
    return MacStudioGetControlBounds(dialog, MACSTUDIO_ID_CONTROLACTIONSTATUS, bounds);
}

short MacStudioSetControlActionStatusBounds(DialogPtr dialog, const Rect *bounds)
{
    return MacStudioSetControlBounds(dialog, MACSTUDIO_ID_CONTROLACTIONSTATUS, bounds);
}

short MacStudioGetControlActionStatusText(DialogPtr dialog, unsigned char *text)
{
    return MacStudioGetControlText(dialog, MACSTUDIO_ID_CONTROLACTIONSTATUS, text);
}

short MacStudioSetControlActionStatusText(DialogPtr dialog, const unsigned char *text)
{
    return MacStudioSetControlText(dialog, MACSTUDIO_ID_CONTROLACTIONSTATUS, text);
}

short MacStudioGetListBoxSelection(DialogPtr dialog, const char *controlId, short *row)
{
    short item;
    ListHandle list = NULL;
    Cell cell;
    if (dialog == NULL || controlId == NULL || row == NULL)
        return paramErr;
    item = BridgeControlItem(controlId);
    switch (item)
    {
        case 11:
            list = gList11;
            break;
        case 13:
            list = gList13;
            break;
        default:
            return paramErr;
    }
    if (list == NULL)
        return paramErr;
    *row = -1;
    cell.h = 0;
    cell.v = 0;
    if (LGetSelect(true, &cell, list))
        *row = cell.v;
    return noErr;
}

short MacStudioSetListBoxItems(DialogPtr dialog, const char *controlId, const unsigned char *const *items, short itemCount)
{
    short item;
    short row;
    ListHandle list = NULL;
    if (dialog == NULL || controlId == NULL || itemCount < 0 || itemCount > 128 || (itemCount > 0 && items == NULL))
        return paramErr;
    item = BridgeControlItem(controlId);
    switch (item)
    {
        case 11:
            list = gList11;
            break;
        case 13:
            list = gList13;
            break;
        default:
            return paramErr;
    }
    if (list == NULL)
        return paramErr;
    for (row = 0; row < itemCount; row++)
    {
        if (items[row] == NULL)
            return paramErr;
    }
    LDelRow(0, 0, list);
    if (itemCount > 0)
        LAddRow(itemCount, 0, list);
    {
        Cell cell;
        cell.h = 0;
        for (row = 0; row < itemCount; row++)
        {
            cell.v = row;
            LSetCell((Ptr)(items[row] + 1), (short)items[row][0], cell, list);
        }
    }
    LUpdate(((WindowPtr)dialog)->visRgn, list);
    return noErr;
}

static int BridgeStart(void)
{
    ProcessSerialNumber processNumber;
    ProcessInfoRec processInfo;
    Str255 processName;
    FSSpec appSpec;
    FSSpec telemetrySpec;
    unsigned char telemetryName[256];
    const char *telemetryFileName = "MacStudio.out";
    OSErr error;
    error = GetCurrentProcess(&processNumber);
    if (error != noErr)
        return 0;
    memset(&processInfo, 0, sizeof(processInfo));
    processInfo.processInfoLength = sizeof(processInfo);
    processInfo.processName = processName;
    processInfo.processAppSpec = &appSpec;
    error = GetProcessInformation(&processNumber, &processInfo);
    if (error != noErr)
        return 0;
    telemetryName[0] = (unsigned char)strlen(telemetryFileName);
    memcpy(&telemetryName[1], telemetryFileName, telemetryName[0]);
    error = FSMakeFSSpec(appSpec.vRefNum, 2, (ConstStringPtr)telemetryName, &telemetrySpec);
    if (error == noErr)
    {
        error = FSpDelete(&telemetrySpec);
        if (error != noErr)
            return 0;
    }
    else if (error != fnfErr)
        return 0;
    error = FSpCreate(&telemetrySpec, 'MIDE', 'TEXT', smSystemScript);
    if (error != noErr)
        return 0;
    error = FSpOpenDF(&telemetrySpec, fsWrPerm, &gTelemetryFile);
    if (error != noErr)
        return 0;
    gTelemetryVolumeRefNum = telemetrySpec.vRefNum;
    BridgeWriteRaw("MACSTUDIO/2\rREADY\r");
    return !gTelemetryFailed;
}

static void BridgeWriteButtonMetadata(const char *controlId, const char *text)
{
    Rect bounds;
    BridgeWriteRaw("CONTROL|");
    BridgeWriteField(controlId);
    BridgeWriteRaw("|");
    BridgeWriteField("button");
    BridgeWriteRaw("|");
    BridgeWriteField(text);
    BridgeWriteRaw("|");
    if (MacStudioGetControlBounds(gDesignerDialog, controlId, &bounds) != noErr)
    {
        BridgeWriteError("BOUNDS", "Could not read runtime control bounds");
        return;
    }
    BridgeWriteNumber(bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.top);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.right - bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.bottom - bounds.top);
    BridgeWriteLine();
}

static void BridgeWriteMetadata(void)
{
    Rect bounds;
    BridgeWriteRaw("FORM|");
    BridgeWriteField("RetroLink LOCAL");
    BridgeWriteLine();
    BridgeWriteRaw("CONTROL|");
    BridgeWriteField("ConnectionStatus");
    BridgeWriteRaw("|");
    BridgeWriteField("static-text");
    BridgeWriteRaw("|");
    BridgeWriteField("Opening serial port...");
    BridgeWriteRaw("|");
    if (MacStudioGetControlBounds(gDesignerDialog, "ConnectionStatus", &bounds) != noErr)
    {
        BridgeWriteError("BOUNDS", "Could not read runtime control bounds");
        return;
    }
    BridgeWriteNumber(bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.top);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.right - bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.bottom - bounds.top);
    BridgeWriteLine();
    BridgeWriteRaw("CONTROL|");
    BridgeWriteField("RoleStatus");
    BridgeWriteRaw("|");
    BridgeWriteField("static-text");
    BridgeWriteRaw("|");
    BridgeWriteField("Role: waiting for REMOTE");
    BridgeWriteRaw("|");
    if (MacStudioGetControlBounds(gDesignerDialog, "RoleStatus", &bounds) != noErr)
    {
        BridgeWriteError("BOUNDS", "Could not read runtime control bounds");
        return;
    }
    BridgeWriteNumber(bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.top);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.right - bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.bottom - bounds.top);
    BridgeWriteLine();
    BridgeWriteRaw("CONTROL|");
    BridgeWriteField("TakeControlButton");
    BridgeWriteRaw("|");
    BridgeWriteField("button");
    BridgeWriteRaw("|");
    BridgeWriteField("Take Control");
    BridgeWriteRaw("|");
    if (MacStudioGetControlBounds(gDesignerDialog, "TakeControlButton", &bounds) != noErr)
    {
        BridgeWriteError("BOUNDS", "Could not read runtime control bounds");
        return;
    }
    BridgeWriteNumber(bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.top);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.right - bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.bottom - bounds.top);
    BridgeWriteLine();
    BridgeWriteRaw("CONTROL|");
    BridgeWriteField("SerialPortLabel");
    BridgeWriteRaw("|");
    BridgeWriteField("static-text");
    BridgeWriteRaw("|");
    BridgeWriteField("Port");
    BridgeWriteRaw("|");
    if (MacStudioGetControlBounds(gDesignerDialog, "SerialPortLabel", &bounds) != noErr)
    {
        BridgeWriteError("BOUNDS", "Could not read runtime control bounds");
        return;
    }
    BridgeWriteNumber(bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.top);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.right - bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.bottom - bounds.top);
    BridgeWriteLine();
    BridgeWriteRaw("CONTROL|");
    BridgeWriteField("SerialPortSelector");
    BridgeWriteRaw("|");
    BridgeWriteField("popup-menu");
    BridgeWriteRaw("|");
    BridgeWriteField("Modem (Serial A)");
    BridgeWriteRaw("|");
    if (MacStudioGetControlBounds(gDesignerDialog, "SerialPortSelector", &bounds) != noErr)
    {
        BridgeWriteError("BOUNDS", "Could not read runtime control bounds");
        return;
    }
    BridgeWriteNumber(bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.top);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.right - bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.bottom - bounds.top);
    BridgeWriteLine();
    BridgeWriteRaw("CONTROL|");
    BridgeWriteField("SerialBaudLabel");
    BridgeWriteRaw("|");
    BridgeWriteField("static-text");
    BridgeWriteRaw("|");
    BridgeWriteField("Baud");
    BridgeWriteRaw("|");
    if (MacStudioGetControlBounds(gDesignerDialog, "SerialBaudLabel", &bounds) != noErr)
    {
        BridgeWriteError("BOUNDS", "Could not read runtime control bounds");
        return;
    }
    BridgeWriteNumber(bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.top);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.right - bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.bottom - bounds.top);
    BridgeWriteLine();
    BridgeWriteRaw("CONTROL|");
    BridgeWriteField("SerialBaudSelector");
    BridgeWriteRaw("|");
    BridgeWriteField("popup-menu");
    BridgeWriteRaw("|");
    BridgeWriteField("9600");
    BridgeWriteRaw("|");
    if (MacStudioGetControlBounds(gDesignerDialog, "SerialBaudSelector", &bounds) != noErr)
    {
        BridgeWriteError("BOUNDS", "Could not read runtime control bounds");
        return;
    }
    BridgeWriteNumber(bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.top);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.right - bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.bottom - bounds.top);
    BridgeWriteLine();
    BridgeWriteRaw("CONTROL|");
    BridgeWriteField("SerialSettingsButton");
    BridgeWriteRaw("|");
    BridgeWriteField("button");
    BridgeWriteRaw("|");
    BridgeWriteField("Apply");
    BridgeWriteRaw("|");
    if (MacStudioGetControlBounds(gDesignerDialog, "SerialSettingsButton", &bounds) != noErr)
    {
        BridgeWriteError("BOUNDS", "Could not read runtime control bounds");
        return;
    }
    BridgeWriteNumber(bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.top);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.right - bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.bottom - bounds.top);
    BridgeWriteLine();
    BridgeWriteRaw("CONTROL|");
    BridgeWriteField("SerialCloseButton");
    BridgeWriteRaw("|");
    BridgeWriteField("button");
    BridgeWriteRaw("|");
    BridgeWriteField("Close");
    BridgeWriteRaw("|");
    if (MacStudioGetControlBounds(gDesignerDialog, "SerialCloseButton", &bounds) != noErr)
    {
        BridgeWriteError("BOUNDS", "Could not read runtime control bounds");
        return;
    }
    BridgeWriteNumber(bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.top);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.right - bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.bottom - bounds.top);
    BridgeWriteLine();
    BridgeWriteRaw("CONTROL|");
    BridgeWriteField("MacVolumesLabel");
    BridgeWriteRaw("|");
    BridgeWriteField("static-text");
    BridgeWriteRaw("|");
    BridgeWriteField("LOCAL");
    BridgeWriteRaw("|");
    if (MacStudioGetControlBounds(gDesignerDialog, "MacVolumesLabel", &bounds) != noErr)
    {
        BridgeWriteError("BOUNDS", "Could not read runtime control bounds");
        return;
    }
    BridgeWriteNumber(bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.top);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.right - bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.bottom - bounds.top);
    BridgeWriteLine();
    BridgeWriteRaw("CONTROL|");
    BridgeWriteField("MacFiles");
    BridgeWriteRaw("|");
    BridgeWriteField("list-box");
    BridgeWriteRaw("|");
    BridgeWriteField("");
    BridgeWriteRaw("|");
    if (MacStudioGetControlBounds(gDesignerDialog, "MacFiles", &bounds) != noErr)
    {
        BridgeWriteError("BOUNDS", "Could not read runtime control bounds");
        return;
    }
    BridgeWriteNumber(bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.top);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.right - bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.bottom - bounds.top);
    BridgeWriteLine();
    BridgeWriteRaw("CONTROL|");
    BridgeWriteField("HostFilesLabel");
    BridgeWriteRaw("|");
    BridgeWriteField("static-text");
    BridgeWriteRaw("|");
    BridgeWriteField("REMOTE");
    BridgeWriteRaw("|");
    if (MacStudioGetControlBounds(gDesignerDialog, "HostFilesLabel", &bounds) != noErr)
    {
        BridgeWriteError("BOUNDS", "Could not read runtime control bounds");
        return;
    }
    BridgeWriteNumber(bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.top);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.right - bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.bottom - bounds.top);
    BridgeWriteLine();
    BridgeWriteRaw("CONTROL|");
    BridgeWriteField("HostFiles");
    BridgeWriteRaw("|");
    BridgeWriteField("list-box");
    BridgeWriteRaw("|");
    BridgeWriteField("");
    BridgeWriteRaw("|");
    if (MacStudioGetControlBounds(gDesignerDialog, "HostFiles", &bounds) != noErr)
    {
        BridgeWriteError("BOUNDS", "Could not read runtime control bounds");
        return;
    }
    BridgeWriteNumber(bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.top);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.right - bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.bottom - bounds.top);
    BridgeWriteLine();
    BridgeWriteRaw("CONTROL|");
    BridgeWriteField("RefreshMacButton");
    BridgeWriteRaw("|");
    BridgeWriteField("button");
    BridgeWriteRaw("|");
    BridgeWriteField("[L] Refresh");
    BridgeWriteRaw("|");
    if (MacStudioGetControlBounds(gDesignerDialog, "RefreshMacButton", &bounds) != noErr)
    {
        BridgeWriteError("BOUNDS", "Could not read runtime control bounds");
        return;
    }
    BridgeWriteNumber(bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.top);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.right - bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.bottom - bounds.top);
    BridgeWriteLine();
    BridgeWriteRaw("CONTROL|");
    BridgeWriteField("RefreshHostButton");
    BridgeWriteRaw("|");
    BridgeWriteField("button");
    BridgeWriteRaw("|");
    BridgeWriteField("[R] Refresh");
    BridgeWriteRaw("|");
    if (MacStudioGetControlBounds(gDesignerDialog, "RefreshHostButton", &bounds) != noErr)
    {
        BridgeWriteError("BOUNDS", "Could not read runtime control bounds");
        return;
    }
    BridgeWriteNumber(bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.top);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.right - bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.bottom - bounds.top);
    BridgeWriteLine();
    BridgeWriteRaw("CONTROL|");
    BridgeWriteField("ActionStatus");
    BridgeWriteRaw("|");
    BridgeWriteField("static-text");
    BridgeWriteRaw("|");
    BridgeWriteField("Waiting for the REMOTE RetroLink session.");
    BridgeWriteRaw("|");
    if (MacStudioGetControlBounds(gDesignerDialog, "ActionStatus", &bounds) != noErr)
    {
        BridgeWriteError("BOUNDS", "Could not read runtime control bounds");
        return;
    }
    BridgeWriteNumber(bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.top);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.right - bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.bottom - bounds.top);
    BridgeWriteLine();
    BridgeWriteRaw("CONTROL|");
    BridgeWriteField("CopyToHostButton");
    BridgeWriteRaw("|");
    BridgeWriteField("button");
    BridgeWriteRaw("|");
    BridgeWriteField("[C] Copy REMOTE");
    BridgeWriteRaw("|");
    if (MacStudioGetControlBounds(gDesignerDialog, "CopyToHostButton", &bounds) != noErr)
    {
        BridgeWriteError("BOUNDS", "Could not read runtime control bounds");
        return;
    }
    BridgeWriteNumber(bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.top);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.right - bounds.left);
    BridgeWriteRaw("|");
    BridgeWriteNumber(bounds.bottom - bounds.top);
    BridgeWriteLine();
    BridgeWriteButtonMetadata("CreateFolderButton", "[N] New Folder");
    BridgeWriteButtonMetadata("RenameEntryButton", "[R] Rename");
    BridgeWriteButtonMetadata("DeleteEntryButton", "[D] Delete");
}

static int BridgeProcessRequest(void)
{
    ProcessSerialNumber processNumber;
    ProcessInfoRec processInfo;
    Str255 processName;
    FSSpec appSpec;
    FSSpec requestSpec;
    unsigned char requestName[256];
    const char *requestFileName = "MacStudio.in";
    OSErr error;
    short requestFile;
    long requestLength;
    long bytesRead;
    char request[4097];
    char *line;
    int quitSeen = 0;
    error = GetCurrentProcess(&processNumber);
    if (error != noErr)
    {
        BridgeWriteError("PROCESS_INFO", "Could not identify the application volume");
        return 1;
    }
    memset(&processInfo, 0, sizeof(processInfo));
    processInfo.processInfoLength = sizeof(processInfo);
    processInfo.processName = processName;
    processInfo.processAppSpec = &appSpec;
    error = GetProcessInformation(&processNumber, &processInfo);
    if (error != noErr)
    {
        BridgeWriteError("PROCESS_INFO", "Could not locate the application volume");
        return 1;
    }
    requestName[0] = (unsigned char)strlen(requestFileName);
    memcpy(&requestName[1], requestFileName, requestName[0]);
    error = FSMakeFSSpec(appSpec.vRefNum, 2, (ConstStringPtr)requestName, &requestSpec);
    if (error == fnfErr)
        return 0;
    if (error != noErr)
    {
        BridgeWriteError("REQUEST_FILE", "Could not locate the request file");
        return 1;
    }
    error = FSpOpenDF(&requestSpec, fsRdPerm, &requestFile);
    if (error != noErr)
    {
        BridgeWriteError("REQUEST_FILE", "Could not open the request file");
        return 1;
    }
    gTestSession = 1;
    error = GetEOF(requestFile, &requestLength);
    if (error != noErr || requestLength < 1 || requestLength > 4096)
    {
        FSClose(requestFile);
        BridgeWriteError("REQUEST_SIZE", "Request file must contain 1 to 4096 bytes");
        return 1;
    }
    bytesRead = requestLength;
    error = FSRead(requestFile, &bytesRead, request);
    FSClose(requestFile);
    if (error != noErr || bytesRead != requestLength)
    {
        BridgeWriteError("REQUEST_READ", "Could not read the complete request file");
        return 1;
    }
    request[requestLength] = '\0';
    line = strtok(request, "\r\n");
    if (line == NULL || strcmp(line, "MACSTUDIO/2") != 0)
    {
        BridgeWriteError("PROTOCOL", "Request protocol header is invalid");
        return 1;
    }
    while ((line = strtok(NULL, "\r\n")) != NULL)
    {
        if (strcmp(line, "QUERY_FORM") == 0)
            continue;
        if (strncmp(line, "CLICK|", 6) == 0)
        {
            const char *id = line + 6;
            short item = BridgeControlItem(id);
            int result;
            if (item == 0)
            {
                BridgeWriteError("UNKNOWN_CONTROL", id);
                continue;
            }
            result = HandleDesignerControl(item);
            if (result == 0)
                DispatchUserControlEvent(gDesignerDialog, item);
            ApplyRadioGroupSelection(item);
            switch (item)
            {
                case 1:
                    break;
                case 2:
                    break;
                case 3:
                    break;
                case 4:
                    break;
                case 5:
                    if (gTestSession)
                    {
                        gPopupSelection5 = 1;
                        GetMenuItemText(gPopupMenu5, gPopupSelection5, gPopupTitle5);
                        SetControlTitle((ControlHandle)DesignerDialogItem(gDesignerDialog, 5), gPopupTitle5);
                        if (gPopupSelection5 != 1)
                            BridgeWriteError("POPUP_SELECTION", "Popup selection did not match");
                    }
                    else if (gPopupMenu5 != NULL)
                    {
                        long selection;
                        Point popupPoint;
                        GetMouse(&popupPoint);
                        LocalToGlobal(&popupPoint);
                        selection = PopUpMenuSelect(gPopupMenu5, popupPoint.v, popupPoint.h, gPopupSelection5);
                        if ((short)(selection & 0xFFFF) > 0)
                        {
                            gPopupSelection5 = (short)(selection & 0xFFFF);
                            GetMenuItemText(gPopupMenu5, gPopupSelection5, gPopupTitle5);
                            SetControlTitle((ControlHandle)DesignerDialogItem(gDesignerDialog, 5), gPopupTitle5);
                        }
                    }
                    break;
                case 6:
                    break;
                case 7:
                    if (gTestSession)
                    {
                        gPopupSelection7 = 9;
                        GetMenuItemText(gPopupMenu7, gPopupSelection7, gPopupTitle7);
                        SetControlTitle((ControlHandle)DesignerDialogItem(gDesignerDialog, 7), gPopupTitle7);
                        if (gPopupSelection7 != 9)
                            BridgeWriteError("POPUP_SELECTION", "Popup selection did not match");
                    }
                    else if (gPopupMenu7 != NULL)
                    {
                        long selection;
                        Point popupPoint;
                        GetMouse(&popupPoint);
                        LocalToGlobal(&popupPoint);
                        selection = PopUpMenuSelect(gPopupMenu7, popupPoint.v, popupPoint.h, gPopupSelection7);
                        if ((short)(selection & 0xFFFF) > 0)
                        {
                            gPopupSelection7 = (short)(selection & 0xFFFF);
                            GetMenuItemText(gPopupMenu7, gPopupSelection7, gPopupTitle7);
                            SetControlTitle((ControlHandle)DesignerDialogItem(gDesignerDialog, 7), gPopupTitle7);
                        }
                    }
                    break;
                case 8:
                    break;
                case 9:
                    break;
                case 10:
                    break;
                case 11:
                    break;
                case 12:
                    break;
                case 13:
                    break;
                case 14:
                    break;
                case 15:
                    break;
                case 16:
                    break;
                default:
                    break;
            }
            BridgeWriteEvent("CLICK", id, result == 0 ? "HANDLED" : "UNHANDLED");
            continue;
        }
        if (strcmp(line, "QUIT") == 0)
        {
            quitSeen = 1;
            if (strtok(NULL, "\r\n") != NULL)
                BridgeWriteError("PROTOCOL", "QUIT must be the final request command");
            break;
        }
        BridgeWriteError("UNKNOWN_COMMAND", line);
    }
    if (!quitSeen)
        BridgeWriteError("PROTOCOL", "Request did not contain QUIT");
    return 1;
}

static int HandleDesignerControl(short item)
{
    switch (item)
    {
        case 1:
            return 0;
        case 2:
            return 0;
        case 3:
            return 0;
        case 4:
            return 0;
        case 5:
            return 0;
        case 6:
            return 0;
        case 7:
            return 0;
        case 8:
            return 0;
        case 9:
            return 0;
        case 10:
            return 0;
        case 11:
            return 0;
        case 12:
            return 0;
        case 13:
            return 0;
        case 14:
            return 0;
        case 15:
            return 0;
        case 16:
            return 0;
        case 18:
            return 0;
        case 19:
            return 0;
        case 20:
            return 0;
        case 21:
            return 0;
        default:
            return 1;
    }
}

static void ApplyRadioGroupSelection(short item)
{
}

static void CleanupDesignerWidgets(DialogPtr dialog)
{
    if (gList11 != NULL) LDispose(gList11);
    if (gList13 != NULL) LDispose(gList13);
    if (gMacStudioSerialOpen1)
        MacStudioSerialClose("SerialPort1");
}

static void DrawTestCompletionMarker(DialogPtr dialog)
{
    static const unsigned char marker[7][7] =
    {
        { 1, 1, 1, 1, 1, 1, 1 },
        { 1, 0, 0, 0, 0, 0, 1 },
        { 1, 0, 1, 1, 1, 0, 1 },
        { 1, 0, 1, 0, 1, 0, 1 },
        { 1, 0, 1, 1, 1, 0, 1 },
        { 1, 0, 0, 0, 0, 0, 1 },
        { 1, 1, 1, 1, 1, 1, 1 },
    };
    Rect cell;
    short row;
    short column;
    SetPort((GrafPtr)dialog);
    for (row = 0; row < 7; row++)
    {
        for (column = 0; column < 7; column++)
        {
            SetRect(&cell, 435 + column * 3, 265 + row * 3, 435 + (column + 1) * 3, 265 + (row + 1) * 3);
            ForeColor(marker[row][column] ? blackColor : whiteColor);
            PaintRect(&cell);
        }
    }
}

int MacStudioRunForm(void)
{
    int formRunning = 1;
    int bridgeStarted = 0;
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    DialogPtr dialog = GetNewDialog(128, NULL, (WindowPtr)-1);
    if (dialog == NULL)
    {
        BridgeWriteError("FORM_RESOURCE", "Could not create the form dialog");
        BridgeFinish();
        return 1;
    }
    gDesignerDialog = dialog;
    ShowWindow((WindowPtr)dialog);
    bridgeStarted = BridgeStart();
    if (MacStudioSetControlIcon(
            dialog,
            "TakeControlButton",
            RETROLINK_ICON_ACCOUNT_SWITCH) != noErr ||
        MacStudioSetControlIcon(
            dialog,
            "RefreshMacButton",
            RETROLINK_ICON_REFRESH) != noErr ||
        MacStudioSetControlIcon(
            dialog,
            "RefreshHostButton",
            RETROLINK_ICON_REFRESH) != noErr ||
        MacStudioSetControlIcon(
            dialog,
            "CopyToHostButton",
            RETROLINK_ICON_UPLOAD) != noErr ||
        MacStudioSetControlIcon(
            dialog,
            "CreateFolderButton",
            RETROLINK_ICON_FOLDER_PLUS) != noErr ||
        MacStudioSetControlIcon(
            dialog,
            "RenameEntryButton",
            RETROLINK_ICON_PENCIL) != noErr ||
        MacStudioSetControlIcon(
            dialog,
            "DeleteEntryButton",
            RETROLINK_ICON_DELETE) != noErr)
    {
        BridgeWriteError("ICON_INIT", "Could not initialize icon buttons");
        BridgeFinish();
        CleanupDesignerWidgets(dialog);
        DisposeDialog(dialog);
        return 1;
    }

    gPopupMenu5 = GetMenu(261);
    gPopupSelection5 = 1;
    if (gPopupMenu5 != NULL)
    {
        GetMenuItemText(gPopupMenu5, gPopupSelection5, gPopupTitle5);
        SetControlTitle((ControlHandle)DesignerDialogItem(dialog, 5), gPopupTitle5);
    }
    gPopupMenu7 = GetMenu(263);
    gPopupSelection7 = 9;
    if (gPopupMenu7 != NULL)
    {
        GetMenuItemText(gPopupMenu7, gPopupSelection7, gPopupTitle7);
        SetControlTitle((ControlHandle)DesignerDialogItem(dialog, 7), gPopupTitle7);
    }
    Rect listView11 = { 118, 12, 250, 222 };
    Rect listData11 = { 0, 0, 0, 1 };
    Point cellSize11;
    cellSize11.h = 208;
    cellSize11.v = 16;
    gList11 = LNew(&listView11, &listData11, cellSize11, 0, (WindowPtr)dialog, true, false, false, true);
    if (gList11 != NULL)
    {
        Cell listCell11;
        listCell11.h = 0;
        LAddRow(1, 0, gList11);
        listCell11.v = 0;
        LSetCell("Startup Disk", 12, listCell11, gList11);
    }
    Rect listView13 = { 118, 234, 250, 462 };
    Rect listData13 = { 0, 0, 0, 1 };
    Point cellSize13;
    cellSize13.h = 226;
    cellSize13.v = 16;
    gList13 = LNew(&listView13, &listData13, cellSize13, 0, (WindowPtr)dialog, true, false, false, true);
    if (gList13 != NULL)
    {
        Cell listCell13;
        listCell13.h = 0;
        LAddRow(1, 0, gList13);
        listCell13.v = 0;
        LSetCell("Waiting for host listing", 24, listCell13, gList13);
    }
    {
        short itemType;
        Handle itemHandle;
        Rect itemBounds;
        GetDialogItem(dialog, 16, &itemType, &itemHandle, &itemBounds);
        (void)itemHandle;
        SetDialogItem(
            dialog,
            16,
            itemType,
            (Handle)DrawProgressBar16,
            &itemBounds);
        DrawProgressBar16(dialog, 16);
    }
    MacStudioFormDidLoad((RetroLinkForm)dialog);
    if (bridgeStarted)
        BridgeWriteMetadata();
    if (bridgeStarted && BridgeProcessRequest())
    {
        BridgeFinish();
        if (gTestSession)
        {
            DrawTestCompletionMarker(dialog);
            EventRecord event;
            while (1)
                WaitNextEvent(everyEvent, &event, 30, NULL);
        }
        CleanupDesignerWidgets(dialog);
        DisposeDialog(dialog);
        return (gTestFailed || gTelemetryFailed) ? 1 : 0;
    }

    EventRecord event;
    short item = 0;
    while (formRunning)
    {
        short clickedItemType = 0;
        Handle clickedItemHandle;
        Rect clickedItemBounds;
        Boolean clickedControl = false;

        item = 0;
        WaitNextEvent(everyEvent, &event, 30, NULL);
        if (event.what == mouseDown)
        {
            WindowPtr clickedWindow = NULL;
            short windowPart = FindWindow(event.where, &clickedWindow);
            if (clickedWindow == (WindowPtr)dialog)
            {
                if (windowPart == inDrag)
                {
                    DragWindow((WindowPtr)dialog, event.where, &qd.screenBits.bounds);
                    continue;
                }
                if (windowPart == inGoAway)
                {
                    if (TrackGoAway((WindowPtr)dialog, event.where))
                        formRunning = 0;
                    continue;
                }
                if (windowPart == inContent &&
                    TrackIconButton(dialog, event.where))
                    continue;
            }
        }
        if (gMacStudioSerialOpen1)
        {
            LONGINT serialAvailable = 0;
            OSErr serialStatus = SerGetBuf(gMacStudioSerialInputRef1, &serialAvailable);
            if (serialStatus != noErr)
            {
                gMacStudioLastSerialError1 = serialStatus;
                DispatchNonVisualComponentEvent(dialog, 1, 2);
            }
            else if (serialAvailable > 0)
                DispatchNonVisualComponentEvent(dialog, 1, 1);
        }
        if (IsDialogEvent(&event))
            DialogSelect(&event, &dialog, &item);
        if (item > 0)
        {
            GetDialogItem(
                dialog,
                item,
                &clickedItemType,
                &clickedItemHandle,
                &clickedItemBounds);
            clickedControl = (clickedItemType & ctrlItem) != 0;
        }
        if (item > 0)
            ApplyRadioGroupSelection(item);
        if (event.what == mouseDown && !clickedControl)
        {
            Point where = event.where;
            short listItem = 0;
            SetPort((GrafPtr)dialog);
            GlobalToLocal(&where);
            if (gList11 != NULL && PtInRect(where, &(**gList11).rView))
            {
                LClick(where, event.modifiers, gList11);
                listItem = 11;
            }
            else if (gList13 != NULL && PtInRect(where, &(**gList13).rView))
            {
                LClick(where, event.modifiers, gList13);
                listItem = 13;
            }
            if (listItem != 0)
            {
                DispatchUserControlEvent(dialog, listItem);
                BridgeWriteEvent("CLICK", BridgeControlId(listItem), "HANDLED");
                continue;
            }
        }
        if (item <= 0)
            continue;
        const char *controlId = BridgeControlId(item);
        int result = HandleDesignerControl(item);
        if (result == 0)
            DispatchUserControlEvent(dialog, item);
        ApplyRadioGroupSelection(item);
        if (*controlId != '\0')
        {
            BridgeWriteEvent("CLICK", controlId, result == 0 ? "HANDLED" : "UNHANDLED");
        }
        else
        {
            BridgeWriteError("UNKNOWN_CONTROL", "Dialog returned an unknown item number");
        }
        switch (item)
        {
            case 1:
                break;
            case 2:
                break;
            case 3:
                break;
            case 4:
                break;
            case 5:
                    if (gTestSession)
                    {
                        gPopupSelection5 = 1;
                        GetMenuItemText(gPopupMenu5, gPopupSelection5, gPopupTitle5);
                        SetControlTitle((ControlHandle)DesignerDialogItem(gDesignerDialog, 5), gPopupTitle5);
                        if (gPopupSelection5 != 1)
                            BridgeWriteError("POPUP_SELECTION", "Popup selection did not match");
                    }
                    else if (gPopupMenu5 != NULL)
                    {
                        long selection;
                        Point popupPoint;
                        GetMouse(&popupPoint);
                        LocalToGlobal(&popupPoint);
                        selection = PopUpMenuSelect(gPopupMenu5, popupPoint.v, popupPoint.h, gPopupSelection5);
                        if ((short)(selection & 0xFFFF) > 0)
                        {
                            gPopupSelection5 = (short)(selection & 0xFFFF);
                            GetMenuItemText(gPopupMenu5, gPopupSelection5, gPopupTitle5);
                            SetControlTitle((ControlHandle)DesignerDialogItem(gDesignerDialog, 5), gPopupTitle5);
                        }
                    }
                break;
            case 6:
                break;
            case 7:
                    if (gTestSession)
                    {
                        gPopupSelection7 = 9;
                        GetMenuItemText(gPopupMenu7, gPopupSelection7, gPopupTitle7);
                        SetControlTitle((ControlHandle)DesignerDialogItem(gDesignerDialog, 7), gPopupTitle7);
                        if (gPopupSelection7 != 9)
                            BridgeWriteError("POPUP_SELECTION", "Popup selection did not match");
                    }
                    else if (gPopupMenu7 != NULL)
                    {
                        long selection;
                        Point popupPoint;
                        GetMouse(&popupPoint);
                        LocalToGlobal(&popupPoint);
                        selection = PopUpMenuSelect(gPopupMenu7, popupPoint.v, popupPoint.h, gPopupSelection7);
                        if ((short)(selection & 0xFFFF) > 0)
                        {
                            gPopupSelection7 = (short)(selection & 0xFFFF);
                            GetMenuItemText(gPopupMenu7, gPopupSelection7, gPopupTitle7);
                            SetControlTitle((ControlHandle)DesignerDialogItem(gDesignerDialog, 7), gPopupTitle7);
                        }
                    }
                break;
            case 8:
                break;
            case 9:
                break;
            case 10:
                break;
            case 11:
                break;
            case 12:
                break;
            case 13:
                break;
            case 14:
                break;
            case 15:
                break;
            case 16:
                break;
            default:
                break;
        }
    }
    CleanupDesignerWidgets(dialog);
    DisposeDialog(dialog);
    BridgeFinish();
    return (gTestFailed || gTelemetryFailed || !bridgeStarted) ? 1 : 0;
}

static void BridgeFinish(void)
{
    if (gTelemetryFile >= 0)
    {
        BridgeWriteRaw("DONE\r");
        FSClose(gTelemetryFile);
        gTelemetryFile = -1;
        FlushVol(0, gTelemetryVolumeRefNum);
    }
}

static short MacStudioNonVisualComponentIndex(const char *componentId)
{
    if (componentId == NULL)
        return 0;
    if (strcmp(componentId, "SerialPort1") == 0)
        return 1;
    if (strcmp(componentId, "ProtocolTransfer1") == 0)
        return 2;
    return 0;
}

static void MacStudioDispatchNonVisualEventById(const char *componentId, short eventIndex)
{
    short componentIndex = MacStudioNonVisualComponentIndex(componentId);
    if (componentIndex > 0)
        DispatchNonVisualComponentEvent(gDesignerDialog, componentIndex, eventIndex);
}

static const unsigned char gMacStudioSerialModemInputDriverName1[] = { 4, 46, 65, 73, 110 };
static const unsigned char gMacStudioSerialModemOutputDriverName1[] = { 5, 46, 65, 79, 117, 116 };
static const unsigned char gMacStudioSerialPrinterInputDriverName1[] = { 4, 46, 66, 73, 110 };
static const unsigned char gMacStudioSerialPrinterOutputDriverName1[] = { 5, 46, 66, 79, 117, 116 };
static short gMacStudioSerialPortOverride1 = 0;
static long gMacStudioSerialBaudOverride1 = 0;

static short MacStudioSerialBaudConfiguration(long baudRate)
{
    switch (baudRate)
    {
        case 300: return baud300;
        case 600: return baud600;
        case 1200: return baud1200;
        case 1800: return baud1800;
        case 2400: return baud2400;
        case 3600: return baud3600;
        case 4800: return baud4800;
        case 7200: return baud7200;
        case 9600: return baud9600;
        case 14400: return baud14400;
        case 19200: return baud19200;
        case 28800: return baud28800;
        case 38400: return baud38400;
        case 57600: return baud57600;
        default: return -1;
    }
}

static short MacStudioSerialConfiguration(
    const char *componentId, short *inputRef, short *outputRef,
    short *configuration, SerShk *handshake)
{
    if (strcmp(componentId, "SerialPort1") == 0)
    {
        *inputRef = gMacStudioSerialPortOverride1 == 0 ? MODEMIRNUM : gMacStudioSerialPortOverride1 == 1 ? MODEMIRNUM : PRNTRIRNUM;
        *outputRef = gMacStudioSerialPortOverride1 == 0 ? MODEMORNUM : gMacStudioSerialPortOverride1 == 1 ? MODEMORNUM : PRNTRORNUM;
        *configuration = (gMacStudioSerialBaudOverride1 == 0 ? baud9600 : MacStudioSerialBaudConfiguration(gMacStudioSerialBaudOverride1)) | data8 | noParity | stop10;
        handshake->fXOn = 0;
        handshake->fCTS = 0;
        handshake->xOn = 0x11;
        handshake->xOff = 0x13;
        handshake->errs = 0;
        handshake->evts = 0;
        handshake->fInX = 0;
        handshake->null = 0;
        return 1;
    }
    return 0;
}

static void MacStudioSetSerialOpenState(short componentIndex, Boolean isOpen)
{
    switch (componentIndex)
    {
        case 1:
            gMacStudioSerialOpen1 = isOpen;
            break;
        default:
            break;
    }
}

static Boolean MacStudioSerialIsOpen(short componentIndex)
{
    switch (componentIndex)
    {
        case 1:
            return gMacStudioSerialOpen1;
        default:
            return false;
    }
}

static void MacStudioSetSerialReferences(
    short componentIndex, short inputRef, short outputRef)
{
    switch (componentIndex)
    {
        case 1:
            gMacStudioSerialInputRef1 = inputRef;
            gMacStudioSerialOutputRef1 = outputRef;
            break;
        default:
            break;
    }
}

static Boolean MacStudioGetSerialReferences(
    short componentIndex, short *inputRef, short *outputRef)
{
    if (inputRef == NULL || outputRef == NULL)
        return false;
    switch (componentIndex)
    {
        case 1:
            if (!gMacStudioSerialOpen1)
                return false;
            *inputRef = gMacStudioSerialInputRef1;
            *outputRef = gMacStudioSerialOutputRef1;
            if (*inputRef == 0 || *outputRef == 0)
                return false;
            return true;
        default:
            return false;
    }
}

static OSErr MacStudioOpenSerialDrivers(
    short componentIndex, short *inputRef, short *outputRef)
{
    short configuredInputRef;
    short configuredOutputRef;
    OSErr error;
    if (inputRef == NULL || outputRef == NULL)
        return paramErr;
    configuredInputRef = *inputRef;
    configuredOutputRef = *outputRef;
    if ((configuredInputRef != MODEMIRNUM &&
         configuredInputRef != PRNTRIRNUM) ||
        (configuredOutputRef != MODEMORNUM &&
         configuredOutputRef != PRNTRORNUM))
    {
        return paramErr;
    }
    *inputRef = 0;
    *outputRef = 0;
    switch (componentIndex)
    {
        case 1:
            error = OpenDriver(
                configuredInputRef == PRNTRIRNUM
                    ? gMacStudioSerialPrinterInputDriverName1
                    : gMacStudioSerialModemInputDriverName1,
                inputRef);
            if (error != noErr)
                return error;
            error = OpenDriver(
                configuredOutputRef == PRNTRORNUM
                    ? gMacStudioSerialPrinterOutputDriverName1
                    : gMacStudioSerialModemOutputDriverName1,
                outputRef);
            if (error != noErr)
            {
                CloseDriver(*inputRef);
                *inputRef = 0;
                return error;
            }
            return noErr;
        default:
            return paramErr;
    }
}

static OSErr MacStudioSetSerialBuffer(short componentIndex, short inputRef)
{
    switch (componentIndex)
    {
        case 1:
            return SerSetBuf(inputRef, (Ptr)gMacStudioSerialBuffer1, (short)sizeof(gMacStudioSerialBuffer1));
        default:
            return paramErr;
    }
}

static void MacStudioSetSerialOverrides(
    short componentIndex, short portIndex, long baudRate)
{
    switch (componentIndex)
    {
        case 1:
            gMacStudioSerialPortOverride1 = portIndex;
            gMacStudioSerialBaudOverride1 = baudRate;
            break;
        default:
            break;
    }
}

short MacStudioSerialOpen(const char *componentId)
{
    short inputRef = 0;
    short outputRef = 0;
    short configuration = 0;
    short componentIndex;
    SerShk handshake;
    OSErr error;
    if (componentId == NULL)
        return paramErr;
    componentIndex = MacStudioSerialConfiguration(
        componentId, &inputRef, &outputRef, &configuration, &handshake);
    if (componentIndex == 0)
        return paramErr;
    if (MacStudioSerialIsOpen(componentIndex))
        return noErr;
    error = MacStudioOpenSerialDrivers(componentIndex, &inputRef, &outputRef);
    if (error != noErr)
        goto serial_open_failed;
    error = SerReset(inputRef, configuration);
    if (error == noErr)
        error = SerReset(outputRef, configuration);
    if (error == noErr)
        error = MacStudioSetSerialBuffer(componentIndex, inputRef);
    if (error == noErr)
        error = SerHShake(inputRef, &handshake);
    if (error != noErr)
    {
        if (outputRef != 0)
            CloseDriver(outputRef);
        if (inputRef != 0)
            CloseDriver(inputRef);
        goto serial_open_failed;
    }
    MacStudioSetSerialReferences(componentIndex, inputRef, outputRef);
    MacStudioSetSerialOpenState(componentIndex, true);
    return noErr;
serial_open_failed:
    gMacStudioLastSerialError1 = error;
    MacStudioSetSerialOpenState(componentIndex, false);
    MacStudioSetSerialReferences(componentIndex, 0, 0);
    MacStudioDispatchNonVisualEventById(componentId, 2);
    return error;
}

short MacStudioSerialOpenConfigured(
    const char *componentId, short portIndex, long baudRate)
{
    short inputRef = 0;
    short outputRef = 0;
    short configuration = 0;
    short componentIndex;
    SerShk handshake;
    OSErr error;
    if (componentId == NULL || portIndex < 0 || portIndex > 2)
        return paramErr;
    if (baudRate != 0 && MacStudioSerialBaudConfiguration(baudRate) < 0)
        return paramErr;
    componentIndex = MacStudioSerialConfiguration(
        componentId, &inputRef, &outputRef, &configuration, &handshake);
    if (componentIndex == 0)
        return paramErr;
    if (MacStudioSerialIsOpen(componentIndex))
    {
        error = MacStudioSerialClose(componentId);
        if (error != noErr)
            return error;
    }
    MacStudioSetSerialOverrides(componentIndex, portIndex, baudRate);
    return MacStudioSerialOpen(componentId);
}

static OSErr MacStudioKillSerialIO(short serialRefNum)
{
    ParamBlockRec parameterBlock;
    memset(&parameterBlock, 0, sizeof(parameterBlock));
    parameterBlock.cntrlParam.ioCRefNum = serialRefNum;
    return PBKillIOSync(&parameterBlock);
}

short MacStudioSerialClose(const char *componentId)
{
    short inputRef = 0;
    short outputRef = 0;
    short configuration = 0;
    short componentIndex;
    SerShk handshake;
    OSErr error;
    OSErr closeError;
    if (componentId == NULL)
        return paramErr;
    componentIndex = MacStudioSerialConfiguration(
        componentId, &inputRef, &outputRef, &configuration, &handshake);
    if (componentIndex == 0)
        return paramErr;
    if (!MacStudioGetSerialReferences(componentIndex, &inputRef, &outputRef))
        return noErr;
    error = MacStudioKillSerialIO(inputRef);
    closeError = MacStudioKillSerialIO(outputRef);
    if (error == noErr)
        error = closeError;
    closeError = CloseDriver(outputRef);
    if (error == noErr)
        error = closeError;
    closeError = CloseDriver(inputRef);
    if (error == noErr)
        error = closeError;
    MacStudioSetSerialReferences(componentIndex, 0, 0);
    MacStudioSetSerialOpenState(componentIndex, false);
    if (error != noErr)
    {
        gMacStudioLastSerialError1 = error;
        MacStudioDispatchNonVisualEventById(componentId, 2);
    }
    return error;
}

short MacStudioGetLastSerialError(const char *componentId)
{
    if (componentId == NULL || strcmp(componentId, "SerialPort1") != 0)
        return paramErr;
    return gMacStudioLastSerialError1;
}

short MacStudioSerialRead(
    const char *componentId, unsigned char *data,
    long capacity, long *readCount)
{
    short inputRef = 0;
    short outputRef = 0;
    short configuration = 0;
    short componentIndex;
    long available = 0;
    LONGINT count;
    SerShk handshake;
    OSErr error;
    if (componentId == NULL || data == NULL || readCount == NULL || capacity < 0)
        return paramErr;
    *readCount = 0;
    componentIndex = MacStudioSerialConfiguration(
        componentId, &inputRef, &outputRef, &configuration, &handshake);
    if (componentIndex == 0 || !MacStudioGetSerialReferences(componentIndex, &inputRef, &outputRef))
        return paramErr;
    if (capacity == 0)
        return noErr;
    error = SerGetBuf(inputRef, &available);
    if (error != noErr)
        goto serial_read_error;
    if (available <= 0)
        return noErr;
    count = available < capacity ? available : capacity;
    error = FSRead(inputRef, &count, data);
    if (error != noErr)
        goto serial_read_error;
    *readCount = count;
    return noErr;
serial_read_error:
    gMacStudioLastSerialError1 = error;
    MacStudioDispatchNonVisualEventById(componentId, 2);
    return error;
}

short MacStudioSerialWrite(
    const char *componentId, const unsigned char *data, long length)
{
    short inputRef = 0;
    short outputRef = 0;
    short configuration = 0;
    short componentIndex;
    LONGINT count;
    SerShk handshake;
    OSErr error;
    if (componentId == NULL || (data == NULL && length > 0) || length < 0)
        return paramErr;
    componentIndex = MacStudioSerialConfiguration(
        componentId, &inputRef, &outputRef, &configuration, &handshake);
    if (componentIndex == 0 || !MacStudioGetSerialReferences(componentIndex, &inputRef, &outputRef))
        return paramErr;
    if (length == 0)
        return noErr;
    count = length;
    error = FSWrite(outputRef, &count, data);
    if (error == noErr && count != length)
        error = ioErr;
    if (error != noErr)
    {
        gMacStudioLastSerialError1 = error;
        MacStudioDispatchNonVisualEventById(componentId, 2);
    }
    return error;
}

#include <OSUtils.h>
#include <Serial.h>
#include <stdio.h>

#define MACSTUDIO_PROTOCOL_ON_PROGRESS 1
#define MACSTUDIO_PROTOCOL_ON_COMPLETE 2
#define MACSTUDIO_PROTOCOL_ON_ERROR 3

static short MacStudioProtocolConfiguration(
    const char *componentId, const char **serialId, short *mode,
    short *timeoutSeconds, const char **fileName, short *componentIndex)
{
    if (strcmp(componentId, "ProtocolTransfer1") == 0)
    {
        *serialId = "SerialPort1";
        *mode = 1;
        *timeoutSeconds = 10;
        *fileName = "TRANSFER.BIN";
        *componentIndex = 2;
        return 1;
    }
    return 0;
}

static short MacStudioProtocolProgressValue(short componentIndex)
{
    switch (componentIndex)
    {
        case 2:
            return gMacStudioProtocolProgress2;
        default:
            return 0;
    }
}

static void MacStudioProtocolProgressStore(short componentIndex, short value)
{
    short *progress = NULL;
    switch (componentIndex)
    {
        case 2:
            progress = &gMacStudioProtocolProgress2;
            break;
        default:
            return;
    }
    if (*progress != value)
    {
        *progress = value;
        DispatchNonVisualComponentEvent(gDesignerDialog, componentIndex, MACSTUDIO_PROTOCOL_ON_PROGRESS);
    }
}

static short MacStudioProtocolFailure(const char *componentId, OSErr error)
{
    MacStudioDispatchNonVisualEventById(componentId, MACSTUDIO_PROTOCOL_ON_ERROR);
    return error;
}

static void MacStudioProtocolComplete(const char *componentId)
{
    MacStudioDispatchNonVisualEventById(componentId, MACSTUDIO_PROTOCOL_ON_COMPLETE);
}

static short MacStudioProtocolPercent(long amount, long total)
{
    short percent = 0;
    short index;
    long remainder = 0;
    if (total <= 0 || amount >= total)
        return 100;
    if (amount <= 0)
        return 0;
    for (index = 0; index < 100; ++index)
    {
        if (remainder >= total - amount)
        {
            remainder -= total - amount;
            ++percent;
        }
        else
            remainder += amount;
    }
    return percent;
}

#define MS_XMODEM_SOH 0x01
#define MS_XMODEM_STX 0x02
#define MS_XMODEM_EOT 0x04
#define MS_XMODEM_ACK 0x06
#define MS_XMODEM_NAK 0x15
#define MS_XMODEM_CAN 0x18
#define MS_XMODEM_CRC 'C'
#define MS_XMODEM_SUB 0x1A

#define MS_ZPAD 0x2A
#define MS_ZDLE 0x18
#define MS_ZBIN 'A'
#define MS_ZHEX 'B'
#define MS_ZBIN32 'C'
#define MS_ZRQINIT 0
#define MS_ZRINIT 1
#define MS_ZACK 3
#define MS_ZFILE 4
#define MS_ZSKIP 5
#define MS_ZNAK 6
#define MS_ZABORT 7
#define MS_ZFIN 8
#define MS_ZRPOS 9
#define MS_ZDATA 10
#define MS_ZEOF 11
#define MS_ZCRCE 0x68
#define MS_ZCRCG 0x69
#define MS_ZCRCQ 0x6A
#define MS_ZCRCW 0x6B
#define MS_Z_CANFC32 0x20
#define MS_Z_ESCCTL 0x40

static OSErr MacStudioProtocolReadByte(
    const char *serialId, short timeoutSeconds, unsigned char *value)
{
    ULONGINT start = TickCount();
    ULONGINT timeoutTicks = (ULONGINT)timeoutSeconds * 60;
    while ((ULONGINT)(TickCount() - start) < timeoutTicks)
    {
        long count = 0;
        OSErr error = MacStudioSerialRead(serialId, value, 1, &count);
        if (error != noErr)
            return error;
        if (count > 0)
            return noErr;
        Delay(1, NULL);
    }
    return ioErr;
}

static OSErr MacStudioProtocolWriteByte(
    const char *serialId, unsigned char value)
{
    return MacStudioSerialWrite(serialId, &value, 1);
}

static unsigned short MacStudioCrc16Update(
    unsigned short crc, unsigned char value)
{
    short bit;
    crc ^= (unsigned short)value << 8;
    for (bit = 0; bit < 8; ++bit)
        crc = (crc & 0x8000)
            ? (unsigned short)((crc << 1) ^ 0x1021)
            : (unsigned short)(crc << 1);
    return crc;
}

static unsigned short MacStudioCrc16(
    const unsigned char *data, long length)
{
    unsigned short crc = 0;
    long index;
    for (index = 0; index < length; ++index)
        crc = MacStudioCrc16Update(crc, data[index]);
    return crc;
}

static unsigned short MacStudioZCrc16(
    const unsigned char *data, long length)
{
    return (unsigned short)~MacStudioCrc16(data, length);
}

static unsigned long MacStudioCrc32Update(
    unsigned long crc, unsigned char value)
{
    short bit;
    crc ^= value;
    for (bit = 0; bit < 8; ++bit)
        crc = (crc & 1)
            ? (crc >> 1) ^ 0xEDB88320UL
            : crc >> 1;
    return crc;
}

static unsigned long MacStudioCrc32(
    const unsigned char *data, long length)
{
    unsigned long crc = 0xFFFFFFFFUL;
    long index;
    for (index = 0; index < length; ++index)
        crc = MacStudioCrc32Update(crc, data[index]);
    return ~crc;
}

static short MacStudioXmodemReadControl(
    const char *serialId, short timeoutSeconds, short mode,
    unsigned char *value)
{
    short attempts;
    for (attempts = 0; attempts < 10; ++attempts)
    {
        OSErr error = MacStudioProtocolReadByte(
            serialId, timeoutSeconds, value);
        if (error != noErr)
            return error;
        if (*value == MS_XMODEM_CAN)
        {
            unsigned char second = 0;
            if (MacStudioProtocolReadByte(
                    serialId, timeoutSeconds, &second) == noErr &&
                second == MS_XMODEM_CAN)
                return abortErr;
        }
        if (*value == MS_XMODEM_ACK || *value == MS_XMODEM_NAK ||
            *value == MS_XMODEM_CRC)
            return noErr;
    }
    return ioErr;
}

static OSErr MacStudioXmodemSend(
    const char *serialId, short mode, short timeoutSeconds,
    const unsigned char *data, long length, short componentIndex)
{
    unsigned char response = 0;
    unsigned char block[1024];
    unsigned char packet[1029];
    unsigned short crc;
    long offset = 0;
    short blockNumber = 1;
    short blockSize = mode == 2 ? 1024 : 128;
    short retries;
    OSErr error;

    for (retries = 0; retries < 10; ++retries)
    {
        error = MacStudioProtocolReadByte(
            serialId, timeoutSeconds, &response);
        if (error == noErr &&
            ((mode == 0 && response == MS_XMODEM_NAK) ||
             (mode != 0 && response == MS_XMODEM_CRC)))
            break;
        if (error != noErr && retries == 9)
            return error;
    }
    if (retries == 10)
        return ioErr;

    while (offset < length)
    {
        long count = length - offset;
        long index;
        if (count > blockSize)
            count = blockSize;
        for (index = 0; index < blockSize; ++index)
            block[index] = index < count
                ? data[offset + index]
                : MS_XMODEM_SUB;
        packet[0] = blockSize == 1024
            ? MS_XMODEM_STX
            : MS_XMODEM_SOH;
        packet[1] = (unsigned char)blockNumber;
        packet[2] = (unsigned char)~blockNumber;
        memcpy(packet + 3, block, blockSize);
        if (mode == 0)
        {
            unsigned char checksum = 0;
            for (index = 0; index < blockSize; ++index)
                checksum = (unsigned char)(checksum + block[index]);
            packet[3 + blockSize] = checksum;
        }
        else
        {
            crc = MacStudioCrc16(block, blockSize);
            packet[3 + blockSize] = (unsigned char)(crc >> 8);
            packet[4 + blockSize] = (unsigned char)crc;
        }

        for (retries = 0; retries < 10; ++retries)
        {
            error = MacStudioSerialWrite(
                serialId, packet, 3 + blockSize + (mode == 0 ? 1 : 2));
            if (error != noErr)
                return error;
            error = MacStudioXmodemReadControl(
                serialId, timeoutSeconds, mode, &response);
            if (error == noErr && response == MS_XMODEM_ACK)
                break;
            if (error == abortErr)
                return error;
            if (error != noErr && retries == 9)
                return error;
        }
        if (retries == 10)
            return ioErr;
        offset += count;
        ++blockNumber;
        MacStudioProtocolProgressStore(
            componentIndex, MacStudioProtocolPercent(offset, length));
    }

    for (retries = 0; retries < 10; ++retries)
    {
        error = MacStudioProtocolWriteByte(serialId, MS_XMODEM_EOT);
        if (error != noErr)
            return error;
        error = MacStudioProtocolReadByte(
            serialId, timeoutSeconds, &response);
        if (error == noErr && response == MS_XMODEM_ACK)
            return noErr;
        if (error != noErr && retries == 9)
            return error;
    }
    return ioErr;
}

static OSErr MacStudioXmodemReceive(
    const char *serialId, short mode, short timeoutSeconds,
    unsigned char *data, long capacity, long *length,
    short componentIndex, long expectedLength)
{
    unsigned char packet[1024];
    unsigned char response = mode == 0
        ? MS_XMODEM_NAK
        : MS_XMODEM_CRC;
    unsigned char expectedBlock = 1;
    unsigned char previousBlock = 0;
    short blockSize;
    short retries = 0;
    OSErr error;

    *length = 0;
    error = MacStudioProtocolWriteByte(serialId, response);
    if (error != noErr)
        return error;
    while (retries < 10)
    {
        error = MacStudioProtocolReadByte(
            serialId, timeoutSeconds, &response);
        if (error != noErr)
        {
            error = MacStudioProtocolWriteByte(
                serialId, mode == 0 ? MS_XMODEM_NAK : MS_XMODEM_NAK);
            if (error != noErr)
                return error;
            ++retries;
            continue;
        }
        if (response == MS_XMODEM_EOT)
        {
            long expectedPaddedLength;
            error = MacStudioProtocolWriteByte(
                serialId, MS_XMODEM_ACK);
            if (error != noErr)
                return error;
            if (expectedLength >= 0)
            {
                expectedPaddedLength =
                    ((expectedLength + 127) / 128) * 128;
                if (*length != expectedPaddedLength)
                    return ioErr;
                *length = expectedLength;
            }
            else
            {
                while (*length > 0 &&
                    data[*length - 1] == MS_XMODEM_SUB)
                    --*length;
            }
            MacStudioProtocolProgressStore(componentIndex, 100);
            return noErr;
        }
        if (response == MS_XMODEM_CAN)
            return abortErr;
        if (response != MS_XMODEM_SOH &&
            !(mode == 2 && response == MS_XMODEM_STX))
        {
            error = MacStudioProtocolWriteByte(
                serialId, MS_XMODEM_NAK);
            if (error != noErr)
                return error;
            ++retries;
            continue;
        }

        blockSize = response == MS_XMODEM_STX ? 1024 : 128;
        {
            unsigned char blockNumber;
            unsigned char complement;
            unsigned short receivedCrc = 0;
            unsigned short actualCrc;
            unsigned char checksum = 0;
            long index;
            error = MacStudioProtocolReadByte(
                serialId, timeoutSeconds, &blockNumber);
            if (error == noErr)
                error = MacStudioProtocolReadByte(
                    serialId, timeoutSeconds, &complement);
            for (index = 0; error == noErr && index < blockSize; ++index)
                error = MacStudioProtocolReadByte(
                    serialId, timeoutSeconds, &packet[index]);
            if (error == noErr && mode == 0)
            {
                unsigned char receivedChecksum;
                error = MacStudioProtocolReadByte(
                    serialId, timeoutSeconds, &receivedChecksum);
                for (index = 0; index < blockSize; ++index)
                    checksum = (unsigned char)(checksum + packet[index]);
                if (error == noErr && checksum != receivedChecksum)
                    error = ioErr;
            }
            else if (error == noErr)
            {
                unsigned char high;
                unsigned char low = 0;
                high = 0;
                error = MacStudioProtocolReadByte(
                    serialId, timeoutSeconds, &high);
                if (error == noErr)
                    error = MacStudioProtocolReadByte(
                        serialId, timeoutSeconds, &low);
                if (error == noErr)
                {
                    receivedCrc = ((unsigned short)high << 8) | low;
                    actualCrc = MacStudioCrc16(packet, blockSize);
                    if (receivedCrc != actualCrc)
                        error = ioErr;
                }
            }
            if (error == noErr &&
                (unsigned char)(blockNumber + complement) != 0xFF)
                error = ioErr;
            if (error == noErr && blockNumber == expectedBlock)
            {
                if (*length + blockSize > capacity)
                {
                    MacStudioProtocolWriteByte(serialId, MS_XMODEM_CAN);
                    MacStudioProtocolWriteByte(serialId, MS_XMODEM_CAN);
                    return memFullErr;
                }
                memcpy(data + *length, packet, blockSize);
                *length += blockSize;
                previousBlock = blockNumber;
                ++expectedBlock;
                retries = 0;
                error = MacStudioProtocolWriteByte(
                    serialId, MS_XMODEM_ACK);
                if (error != noErr)
                    return error;
                if (capacity > 0)
                {
                    short percent = MacStudioProtocolPercent(*length, capacity);
                    if (percent > 99)
                        percent = 99;
                    MacStudioProtocolProgressStore(
                        componentIndex, percent);
                }
            }
            else if (error == noErr && blockNumber == previousBlock)
            {
                error = MacStudioProtocolWriteByte(
                    serialId, MS_XMODEM_ACK);
            }
            else
            {
                error = MacStudioProtocolWriteByte(
                    serialId, MS_XMODEM_NAK);
                ++retries;
            }
            if (error != noErr)
                return error;
        }
    }
    return ioErr;
}

static short MacStudioZHexValue(unsigned char value)
{
    if (value >= '0' && value <= '9')
        return (short)(value - '0');
    if (value >= 'a' && value <= 'f')
        return (short)(value - 'a' + 10);
    if (value >= 'A' && value <= 'F')
        return (short)(value - 'A' + 10);
    return -1;
}

static void MacStudioZPutEscaped(
    unsigned char *output, long *length, unsigned char value)
{
    if (value < 0x20 || value == 0x7F ||
        value == 0x90 || value == 0x91 ||
        value == 0x93 || value == MS_ZDLE)
    {
        output[(*length)++] = MS_ZDLE;
        output[(*length)++] = value ^ 0x40;
    }
    else
        output[(*length)++] = value;
}

static OSErr MacStudioZReadEscapedByte(
    const char *serialId, short timeoutSeconds, unsigned char *value)
{
    unsigned char escaped;
    OSErr error = MacStudioProtocolReadByte(
        serialId, timeoutSeconds, value);
    if (error != noErr || *value != MS_ZDLE)
        return error;
    error = MacStudioProtocolReadByte(
        serialId, timeoutSeconds, &escaped);
    if (error == noErr)
        *value = escaped ^ 0x40;
    return error;
}

static OSErr MacStudioZWriteHeader(
    const char *serialId, unsigned char type,
    const unsigned char fields[4], short useCrc32, short hex)
{
    unsigned char bytes[9];
    unsigned char output[32];
    unsigned char hexDigits[] = "0123456789abcdef";
    long outputLength = 0;
    short index;
    bytes[0] = type;
    memcpy(bytes + 1, fields, 4);
    if (hex)
    {
        unsigned short crc = MacStudioZCrc16(bytes, 5);
        output[outputLength++] = MS_ZPAD;
        output[outputLength++] = MS_ZPAD;
        output[outputLength++] = MS_ZDLE;
        output[outputLength++] = MS_ZHEX;
        for (index = 0; index < 5; ++index)
        {
            output[outputLength++] = hexDigits[bytes[index] >> 4];
            output[outputLength++] = hexDigits[bytes[index] & 0x0F];
        }
        output[outputLength++] = hexDigits[(crc >> 12) & 0x0F];
        output[outputLength++] = hexDigits[(crc >> 8) & 0x0F];
        output[outputLength++] = hexDigits[(crc >> 4) & 0x0F];
        output[outputLength++] = hexDigits[crc & 0x0F];
        output[outputLength++] = '\r';
        output[outputLength++] = '\n';
        if (type != MS_ZACK && type != MS_ZFIN)
            output[outputLength++] = 0x11;
    }
    else if (useCrc32)
    {
        unsigned long crc = MacStudioCrc32(bytes, 5);
        output[outputLength++] = MS_ZPAD;
        output[outputLength++] = MS_ZPAD;
        output[outputLength++] = MS_ZDLE;
        output[outputLength++] = MS_ZBIN32;
        for (index = 0; index < 5; ++index)
            MacStudioZPutEscaped(output, &outputLength, bytes[index]);
        for (index = 0; index < 4; ++index)
            MacStudioZPutEscaped(
                output, &outputLength, (unsigned char)(crc >> (index * 8)));
    }
    else
    {
        unsigned short crc = MacStudioZCrc16(bytes, 5);
        output[outputLength++] = MS_ZPAD;
        output[outputLength++] = MS_ZPAD;
        output[outputLength++] = MS_ZDLE;
        output[outputLength++] = MS_ZBIN;
        for (index = 0; index < 5; ++index)
            MacStudioZPutEscaped(output, &outputLength, bytes[index]);
        MacStudioZPutEscaped(output, &outputLength, (unsigned char)(crc >> 8));
        MacStudioZPutEscaped(output, &outputLength, (unsigned char)crc);
    }
    return MacStudioSerialWrite(serialId, output, outputLength);
}

static OSErr MacStudioZReadHeader(
    const char *serialId, short timeoutSeconds, unsigned char *type,
    unsigned char fields[4], short *useCrc32)
{
    unsigned char value;
    unsigned char format;
    unsigned char bytes[9];
    unsigned char crcBytes[4];
    short nibble;
    short index;
    OSErr error;
    for (;;)
    {
        error = MacStudioProtocolReadByte(
            serialId, timeoutSeconds, &value);
        if (error != noErr)
            return error;
        if (value != MS_ZPAD)
            continue;
        error = MacStudioProtocolReadByte(
            serialId, timeoutSeconds, &format);
        if (error != noErr)
            return error;
        if (format == MS_ZPAD)
        {
            error = MacStudioProtocolReadByte(
                serialId, timeoutSeconds, &format);
            if (error != noErr)
                return error;
        }
        if (format != MS_ZDLE)
            continue;
        error = MacStudioProtocolReadByte(
            serialId, timeoutSeconds, &format);
        if (error != noErr)
            return error;
        break;
    }

    if (format == MS_ZHEX)
    {
        unsigned short receivedCrc = 0;
        unsigned short actualCrc;
        for (index = 0; index < 7; ++index)
        {
            unsigned char high;
            unsigned char low;
            error = MacStudioProtocolReadByte(
                serialId, timeoutSeconds, &high);
            if (error == noErr)
                error = MacStudioProtocolReadByte(
                    serialId, timeoutSeconds, &low);
            if (error != noErr)
                return error;
            nibble = MacStudioZHexValue(high);
            if (nibble < 0)
                return ioErr;
            bytes[index] = (unsigned char)(nibble << 4);
            nibble = MacStudioZHexValue(low);
            if (nibble < 0)
                return ioErr;
            bytes[index] |= (unsigned char)nibble;
        }
        error = MacStudioProtocolReadByte(
            serialId, timeoutSeconds, &value);
        if (error == noErr && value == '\r')
        {
            error = MacStudioProtocolReadByte(
                serialId, timeoutSeconds, &value);
            if (error == noErr && value != '\n')
                error = ioErr;
        }
        else if (error == noErr)
            error = ioErr;
        if (error != noErr)
            return error;
        receivedCrc = ((unsigned short)bytes[5] << 8) | bytes[6];
        actualCrc = MacStudioZCrc16(bytes, 5);
        if (receivedCrc != actualCrc)
            return ioErr;
        *type = bytes[0];
        memcpy(fields, bytes + 1, 4);
        *useCrc32 = 0;
        return noErr;
    }

    if (format == MS_ZBIN32)
        *useCrc32 = 1;
    else if (format == MS_ZBIN)
        *useCrc32 = 0;
    else
        return ioErr;
    for (index = 0; index < 5; ++index)
    {
        error = MacStudioZReadEscapedByte(
            serialId, timeoutSeconds, &bytes[index]);
        if (error != noErr)
            return error;
    }
    if (*useCrc32)
    {
        unsigned long receivedCrc = 0;
        unsigned long actualCrc = MacStudioCrc32(bytes, 5);
        for (index = 0; index < 4; ++index)
        {
            error = MacStudioZReadEscapedByte(
                serialId, timeoutSeconds, &crcBytes[index]);
            if (error != noErr)
                return error;
            receivedCrc |= (unsigned long)crcBytes[index] << (index * 8);
        }
        if (receivedCrc != actualCrc)
            return ioErr;
    }
    else
    {
        unsigned short receivedCrc;
        unsigned short actualCrc = MacStudioZCrc16(bytes, 5);
        error = MacStudioZReadEscapedByte(
            serialId, timeoutSeconds, &crcBytes[0]);
        if (error == noErr)
            error = MacStudioZReadEscapedByte(
                serialId, timeoutSeconds, &crcBytes[1]);
        if (error != noErr)
            return error;
        receivedCrc = ((unsigned short)crcBytes[0] << 8) | crcBytes[1];
        if (receivedCrc != actualCrc)
            return ioErr;
    }
    *type = bytes[0];
    memcpy(fields, bytes + 1, 4);
    return noErr;
}

static OSErr MacStudioZSendSubpacket(
    const char *serialId, const unsigned char *data, long length,
    unsigned char endMarker, short useCrc32)
{
    unsigned char output[2064];
    unsigned char crcInput[1025];
    unsigned long crc;
    unsigned short crc16;
    long outputLength = 0;
    long index;
    if (length < 0 || length > 1024)
        return paramErr;
    memcpy(crcInput, data, length);
    crcInput[length] = endMarker;
    crc = MacStudioCrc32(crcInput, length + 1);
    for (index = 0; index < length; ++index)
        MacStudioZPutEscaped(output, &outputLength, data[index]);
    output[outputLength++] = MS_ZDLE;
    output[outputLength++] = endMarker;
    if (useCrc32)
    {
        for (index = 0; index < 4; ++index)
            MacStudioZPutEscaped(
                output, &outputLength, (unsigned char)(crc >> (index * 8)));
    }
    else
    {
        crc16 = MacStudioZCrc16(crcInput, length + 1);
        MacStudioZPutEscaped(
            output, &outputLength, (unsigned char)(crc16 >> 8));
        MacStudioZPutEscaped(
            output, &outputLength, (unsigned char)crc16);
    }
    return MacStudioSerialWrite(serialId, output, outputLength);
}

static OSErr MacStudioZReadSubpacket(
    const char *serialId, short timeoutSeconds, short useCrc32,
    unsigned char *data, long capacity, long *length,
    unsigned char *endMarker)
{
    unsigned long crc32 = 0xFFFFFFFFUL;
    unsigned short crc16 = 0;
    unsigned char value;
    unsigned char escaped;
    unsigned char crcBytes[4];
    unsigned long receivedCrc = 0;
    unsigned short receivedCrc16 = 0;
    long index;
    OSErr error;
    *length = 0;
    for (;;)
    {
        error = MacStudioProtocolReadByte(
            serialId, timeoutSeconds, &value);
        if (error != noErr)
            return error;
        if (value == MS_ZDLE)
        {
            error = MacStudioProtocolReadByte(
                serialId, timeoutSeconds, &escaped);
            if (error != noErr)
                return error;
            if (escaped == MS_ZCRCE || escaped == MS_ZCRCG ||
                escaped == MS_ZCRCQ || escaped == MS_ZCRCW)
            {
                *endMarker = escaped;
                break;
            }
            value = escaped ^ 0x40;
        }
        if (*length >= capacity)
            return memFullErr;
        data[(*length)++] = value;
        if (useCrc32)
            crc32 = MacStudioCrc32Update(crc32, value);
        else
            crc16 = MacStudioCrc16Update(crc16, value);
    }
    if (useCrc32)
        crc32 = MacStudioCrc32Update(crc32, *endMarker) ^ 0xFFFFFFFFUL;
    else
        crc16 = (unsigned short)~MacStudioCrc16Update(crc16, *endMarker);
    for (index = 0; index < (useCrc32 ? 4 : 2); ++index)
    {
        error = MacStudioZReadEscapedByte(
            serialId, timeoutSeconds, &crcBytes[index]);
        if (error != noErr)
            return error;
        if (useCrc32)
            receivedCrc |= (unsigned long)crcBytes[index] << (index * 8);
        else
            receivedCrc16 = (unsigned short)(
                (receivedCrc16 << 8) | crcBytes[index]);
    }
    if ((useCrc32 && receivedCrc != crc32) ||
        (!useCrc32 && receivedCrc16 != crc16))
        return ioErr;
    return noErr;
}

static void MacStudioZSetPosition(
    unsigned char fields[4], unsigned long position)
{
    fields[0] = (unsigned char)position;
    fields[1] = (unsigned char)(position >> 8);
    fields[2] = (unsigned char)(position >> 16);
    fields[3] = (unsigned char)(position >> 24);
}

static unsigned long MacStudioZGetPosition(
    const unsigned char fields[4])
{
    return (unsigned long)fields[0] |
        ((unsigned long)fields[1] << 8) |
        ((unsigned long)fields[2] << 16) |
        ((unsigned long)fields[3] << 24);
}

static OSErr MacStudioZSendCancel(const char *serialId)
{
    unsigned char cancel[8];
    short index;
    for (index = 0; index < 8; ++index)
        cancel[index] = MS_ZDLE;
    return MacStudioSerialWrite(serialId, cancel, sizeof(cancel));
}

static OSErr MacStudioZSendReceiverInit(const char *serialId)
{
    unsigned char fields[4] = { 0, 4, 0,
        MS_Z_CANFC32 | MS_Z_ESCCTL | 0x03 };
    return MacStudioZWriteHeader(
        serialId, MS_ZRINIT, fields, 0, 1);
}

static OSErr MacStudioZmodemSend(
    const char *serialId, short timeoutSeconds, const char *fileName,
    const unsigned char *data, long length, short componentIndex)
{
    unsigned char fields[4] = { 0, 0, 0, 0 };
    unsigned char type;
    unsigned char metadata[256];
    unsigned char nameLength = 0;
    short useCrc32 = 0;
    short peerFlags = 0;
    short attempts;
    short restartCount = 0;
    long offset;
    long metadataLength;
    long textLength;
    OSErr error;

    for (attempts = 0; attempts < 10; ++attempts)
    {
        error = MacStudioZWriteHeader(
            serialId, MS_ZRQINIT, fields, 0, 1);
        if (error != noErr)
            return error;
        error = MacStudioZReadHeader(
            serialId, timeoutSeconds, &type, fields, &useCrc32);
        if (error == noErr && type == MS_ZRINIT)
        {
            peerFlags = fields[3];
            break;
        }
    }
    if (attempts == 10)
        return ioErr;
    useCrc32 = (peerFlags & MS_Z_CANFC32) != 0;

    while (nameLength < 63 && fileName[nameLength] != '\0')
    {
        metadata[nameLength] = (unsigned char)fileName[nameLength];
        ++nameLength;
    }
    metadata[nameLength] = 0;
    textLength = sprintf(
        (char *)metadata + nameLength + 1,
        "%ld 0 0 0 1 0", length);
    metadataLength = nameLength + 1 + textLength + 1;
    fields[0] = 1;
    fields[1] = 0;
    fields[2] = 0;
    fields[3] = 0;
    error = MacStudioZWriteHeader(
        serialId, MS_ZFILE, fields, useCrc32, 0);
    if (error == noErr)
        error = MacStudioZSendSubpacket(
            serialId, metadata, metadataLength, MS_ZCRCW, useCrc32);
    if (error != noErr)
        return error;
    error = MacStudioZReadHeader(
        serialId, timeoutSeconds, &type, fields, &useCrc32);
    if (error != noErr || type == MS_ZSKIP)
        return error == noErr ? ioErr : error;
    if (type != MS_ZRPOS)
        return ioErr;
    offset = (long)MacStudioZGetPosition(fields);

    for (;;)
    {
        short restart = 0;
        if (++restartCount > 10)
            return ioErr;
        while (offset < length || (length == 0 && offset == 0))
        {
            unsigned char endMarker = MS_ZCRCW;
            long count = length - offset;
            if (count > 512)
                count = 512;
            fields[0] = (unsigned char)offset;
            fields[1] = (unsigned char)(offset >> 8);
            fields[2] = (unsigned char)(offset >> 16);
            fields[3] = (unsigned char)(offset >> 24);
            error = MacStudioZWriteHeader(
                serialId, MS_ZDATA, fields, useCrc32, 0);
            if (error == noErr)
                error = MacStudioZSendSubpacket(
                    serialId,
                    length > 0 ? data + offset :
                        (const unsigned char *)"",
                    count, endMarker, useCrc32);
            if (error != noErr)
                return error;
            if (length == 0)
                offset = 1;
            else
                offset += count;
            error = MacStudioZReadHeader(
                serialId, timeoutSeconds, &type, fields, &useCrc32);
            if (error != noErr)
                return error;
            if (type == MS_ZRPOS)
            {
                offset = (long)MacStudioZGetPosition(fields);
                restart = 1;
                break;
            }
            if (type != MS_ZACK)
                return ioErr;
            if (length > 0)
                MacStudioProtocolProgressStore(
                    componentIndex, MacStudioProtocolPercent(offset, length));
            if (length == 0)
                break;
        }
        if (restart)
            continue;

        MacStudioZSetPosition(fields, (unsigned long)length);
        error = MacStudioZWriteHeader(
            serialId, MS_ZEOF, fields, 0, 1);
        if (error != noErr)
            return error;
        error = MacStudioZReadHeader(
            serialId, timeoutSeconds, &type, fields, &useCrc32);
        if (error != noErr)
            return error;
        if (type == MS_ZRPOS)
        {
            offset = (long)MacStudioZGetPosition(fields);
            continue;
        }
        if (type != MS_ZRINIT)
            return ioErr;
        break;
    }

    error = MacStudioZWriteHeader(
        serialId, MS_ZFIN, fields, 0, 1);
    if (error != noErr)
        return error;
    error = MacStudioZReadHeader(
        serialId, timeoutSeconds, &type, fields, &useCrc32);
    if (error != noErr || type != MS_ZFIN)
        return error == noErr ? ioErr : error;
    error = MacStudioProtocolWriteByte(serialId, 'O');
    if (error == noErr)
        error = MacStudioProtocolWriteByte(serialId, 'O');
    return error;
}

static OSErr MacStudioZmodemReceive(
    const char *serialId, short timeoutSeconds, unsigned char *data,
    long capacity, long *length, short componentIndex)
{
    unsigned char type;
    unsigned char fields[4];
    short useCrc32;
    unsigned char metadata[256];
    unsigned char endMarker;
    long metadataLength;
    long packetLength;
    long total = 0;
    short attempts = 0;
    OSErr error;
    *length = 0;

    for (;;)
    {
        error = MacStudioZReadHeader(
            serialId, timeoutSeconds, &type, fields, &useCrc32);
        if (error != noErr)
        {
            if (++attempts >= 10)
                return error;
            error = MacStudioZSendReceiverInit(serialId);
            if (error != noErr)
                return error;
            continue;
        }
        attempts = 0;
        if (type == MS_ZRQINIT)
        {
            error = MacStudioZSendReceiverInit(serialId);
            if (error != noErr)
                return error;
            continue;
        }
        if (type != MS_ZFILE)
            return ioErr;
        error = MacStudioZReadSubpacket(
            serialId, timeoutSeconds, useCrc32, metadata,
            sizeof(metadata), &metadataLength, &endMarker);
        if (error != noErr || endMarker != MS_ZCRCW)
            return error == noErr ? ioErr : error;
        break;
    }

    fields[0] = 0;
    fields[1] = 0;
    fields[2] = 0;
    fields[3] = 0;
    error = MacStudioZWriteHeader(
        serialId, MS_ZRPOS, fields, 0, 1);
    if (error != noErr)
        return error;

    for (;;)
    {
        error = MacStudioZReadHeader(
            serialId, timeoutSeconds, &type, fields, &useCrc32);
        if (error != noErr)
            return error;
        if (type == MS_ZDATA)
        {
            unsigned long position = MacStudioZGetPosition(fields);
            if (position != (unsigned long)total)
            {
                MacStudioZSetPosition(fields, (unsigned long)total);
                error = MacStudioZWriteHeader(
                    serialId, MS_ZRPOS, fields, 0, 1);
                if (error != noErr)
                    return error;
                continue;
            }
            for (;;)
            {
                if (total > capacity)
                {
                    MacStudioZSendCancel(serialId);
                    return memFullErr;
                }
                error = MacStudioZReadSubpacket(
                    serialId, timeoutSeconds, useCrc32, data + total,
                    capacity - total, &packetLength, &endMarker);
                if (error == memFullErr)
                {
                    MacStudioZSendCancel(serialId);
                    return error;
                }
                if (error != noErr)
                {
                    MacStudioZSetPosition(fields, (unsigned long)total);
                    MacStudioZWriteHeader(
                        serialId, MS_ZRPOS, fields, 0, 1);
                    break;
                }
                total += packetLength;
                if (capacity > 0)
                {
                    short percent = MacStudioProtocolPercent(total, capacity);
                    if (percent > 99)
                        percent = 99;
                    MacStudioProtocolProgressStore(
                        componentIndex, percent);
                }
                if (endMarker == MS_ZCRCW || endMarker == MS_ZCRCQ)
                {
                    MacStudioZSetPosition(fields, (unsigned long)total);
                    error = MacStudioZWriteHeader(
                        serialId, MS_ZACK, fields, 0, 1);
                    if (error != noErr)
                        return error;
                }
                if (endMarker == MS_ZCRCE || endMarker == MS_ZCRCW)
                    break;
            }
            continue;
        }
        if (type == MS_ZEOF)
        {
            if (MacStudioZGetPosition(fields) != (unsigned long)total)
            {
                MacStudioZSetPosition(fields, (unsigned long)total);
                error = MacStudioZWriteHeader(
                    serialId, MS_ZRPOS, fields, 0, 1);
                if (error != noErr)
                    return error;
                continue;
            }
            error = MacStudioZSendReceiverInit(serialId);
            if (error != noErr)
                return error;
            break;
        }
        if (type == MS_ZABORT)
            return abortErr;
        if (type == MS_ZFIN)
        {
            error = MacStudioZWriteHeader(
                serialId, MS_ZFIN, fields, 0, 1);
            if (error == noErr)
                error = MacStudioProtocolWriteByte(serialId, 'O');
            if (error == noErr)
                error = MacStudioProtocolWriteByte(serialId, 'O');
            if (error == noErr)
                return ioErr;
            return error;
        }
        return ioErr;
    }

    for (attempts = 0; attempts < 10; ++attempts)
    {
        error = MacStudioZReadHeader(
            serialId, timeoutSeconds, &type, fields, &useCrc32);
        if (error == noErr && type == MS_ZFIN)
            break;
        if (error != noErr)
            return error;
    }
    if (attempts == 10)
        return ioErr;
    error = MacStudioZWriteHeader(
        serialId, MS_ZFIN, fields, 0, 1);
    if (error == noErr)
        error = MacStudioProtocolReadByte(
            serialId, timeoutSeconds, &type);
    if (error == noErr && type == 'O')
        error = MacStudioProtocolReadByte(
            serialId, timeoutSeconds, &type);
    if (error != noErr || type != 'O')
        return error == noErr ? ioErr : error;
    *length = total;
    MacStudioProtocolProgressStore(componentIndex, 100);
    return noErr;
}

short MacStudioProtocolSend(
    const char *componentId, const unsigned char *data, long length)
{
    const char *serialId;
    const char *fileName;
    short mode;
    short timeoutSeconds;
    short componentIndex;
    short serialIndex;
    OSErr error;
    if (componentId == NULL || (data == NULL && length > 0) || length < 0)
        return paramErr;
    if (!MacStudioProtocolConfiguration(
            componentId, &serialId, &mode, &timeoutSeconds,
            &fileName, &componentIndex))
        return paramErr;
    serialIndex = MacStudioNonVisualComponentIndex(serialId);
    if (serialIndex == 0)
        return MacStudioProtocolFailure(componentId, paramErr);
    if (!MacStudioSerialIsOpen(serialIndex))
    {
        error = MacStudioSerialOpen(serialId);
        if (error != noErr)
            return MacStudioProtocolFailure(componentId, error);
    }
    MacStudioProtocolProgressStore(componentIndex, 0);
    error = mode == 3
        ? MacStudioZmodemSend(
            serialId, timeoutSeconds, fileName, data, length,
            componentIndex)
        : MacStudioXmodemSend(
            serialId, mode, timeoutSeconds, data, length,
            componentIndex);
    if (error != noErr)
    {
        if (mode == 3)
            MacStudioZSendCancel(serialId);
        return MacStudioProtocolFailure(componentId, error);
    }
    MacStudioProtocolProgressStore(componentIndex, 100);
    MacStudioProtocolComplete(componentId);
    return noErr;
}

static short MacStudioProtocolReceiveCore(
    const char *componentId, unsigned char *data, long capacity,
    long expectedLength, long *length)
{
    const char *serialId;
    const char *fileName;
    short mode;
    short timeoutSeconds;
    short componentIndex;
    short serialIndex;
    OSErr error;
    if (componentId == NULL || data == NULL || length == NULL ||
        capacity < 0 || expectedLength < -1 ||
        (expectedLength >= 0 &&
         (expectedLength > capacity ||
          expectedLength > 0x7FFFFFFFL - 127)))
        return paramErr;
    *length = 0;
    if (!MacStudioProtocolConfiguration(
            componentId, &serialId, &mode, &timeoutSeconds,
            &fileName, &componentIndex))
        return paramErr;
    if (expectedLength >= 0 && mode == 3)
        return paramErr;
    serialIndex = MacStudioNonVisualComponentIndex(serialId);
    if (serialIndex == 0)
        return MacStudioProtocolFailure(componentId, paramErr);
    if (!MacStudioSerialIsOpen(serialIndex))
    {
        error = MacStudioSerialOpen(serialId);
        if (error != noErr)
            return MacStudioProtocolFailure(componentId, error);
    }
    MacStudioProtocolProgressStore(componentIndex, 0);
    error = mode == 3
        ? MacStudioZmodemReceive(
            serialId, timeoutSeconds, data, capacity, length,
            componentIndex)
        : MacStudioXmodemReceive(
            serialId, mode, timeoutSeconds, data, capacity, length,
            componentIndex, expectedLength);
    if (error != noErr)
    {
        if (mode == 3)
            MacStudioZSendCancel(serialId);
        return MacStudioProtocolFailure(componentId, error);
    }
    MacStudioProtocolProgressStore(componentIndex, 100);
    MacStudioProtocolComplete(componentId);
    return noErr;
}

short MacStudioProtocolReceive(
    const char *componentId, unsigned char *data, long capacity,
    long *length)
{
    return MacStudioProtocolReceiveCore(
        componentId, data, capacity, -1, length);
}

short MacStudioProtocolReceiveExact(
    const char *componentId, unsigned char *data, long capacity,
    long expectedLength, long *length)
{
    if (expectedLength < 0)
        return paramErr;
    return MacStudioProtocolReceiveCore(
        componentId, data, capacity, expectedLength, length);
}

short MacStudioProtocolGetProgress(
    const char *componentId, short *percentage)
{
    const char *serialId;
    const char *fileName;
    short mode;
    short timeoutSeconds;
    short componentIndex;
    if (componentId == NULL || percentage == NULL)
        return paramErr;
    if (!MacStudioProtocolConfiguration(
            componentId, &serialId, &mode, &timeoutSeconds,
            &fileName, &componentIndex))
        return paramErr;
    *percentage = MacStudioProtocolProgressValue(componentIndex);
    return noErr;
}
