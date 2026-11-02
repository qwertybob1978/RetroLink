#include <Gestalt.h>
#include <Files.h>
#include <Memory.h>
#include <OSUtils.h>
#include <Errors.h>
#include <stdio.h>
#include <string.h>
#include "designer_form.h"

#ifndef gestaltSystemVersion
#define gestaltSystemVersion 'sysv'
#endif
#ifndef gestaltMachineType
#define gestaltMachineType 'mach'
#endif

#define RETROLINK_HEADER_SIZE 30
#define RETROLINK_MAX_PAYLOAD 1024
#define RETROLINK_MAX_FRAME (RETROLINK_HEADER_SIZE + RETROLINK_MAX_PAYLOAD + 2)
#define RETROLINK_MAX_LIST_ENTRIES 32
#define RETROLINK_MAX_LIST_ROWS 34
#define RETROLINK_UI_LIST_ENTRIES 6
#define RETROLINK_MAC_MAX_VOLUMES 8
#define RETROLINK_MAC_MAX_PATH_COMPONENTS 16
#define RETROLINK_MAC_MAX_PATH_BYTES 512
#define RETROLINK_MAC_MAX_NAME_BYTES 31
#define RETROLINK_MAC_MAX_UTF8_NAME_BYTES 93
#define RETROLINK_WINDOWS_SHARE_PROVIDER 0x80000001UL
#define RETROLINK_LIST_COMPLETE 0xFFFFFFFFUL
#define RETROLINK_HFS_ROOT_DIRECTORY_ID 2L
#define RETROLINK_MAC_EPOCH_OFFSET 3029529600UL
#define RETROLINK_MAC_DIRECTORY_FLAG 0x10
#define RETROLINK_LIST_PAGE_HEADER_SIZE 11
#define RETROLINK_LIST_ENTRY_FIXED_SIZE 20
#define RETROLINK_TRANSFER_MAX_BYTES 524288UL
#define RETROLINK_TRANSFER_READY 0x21
#define RETROLINK_TRANSFER_COMPLETE 0x22
#define RETROLINK_COPY_PREPARE 0x20

static unsigned char gFrameBuffer[RETROLINK_MAX_FRAME];
static long gFrameLength = 0;
static unsigned char gSessionId[16];
static unsigned char gHostEndpointId[16];
static unsigned char gLocalEndpointId[16];
static unsigned char gActiveEndpointId[16];
static unsigned long gRoleEpoch = 0;
static unsigned long gNextRequestId = 1;
static Boolean gConnected = false;
static Boolean gSerialPortOpen = false;
static Boolean gEndpointInitialized = false;
static Boolean gPendingHostList = false;
static Boolean gHostHasMore = false;
static unsigned long gHostListRequestId = 0;
static unsigned long gHostRequestedCursor = 0;
static unsigned long gHostNextCursor = RETROLINK_LIST_COMPLETE;
static short gHostPathCount = 0;
static unsigned char gHostPathLengths[16];
static unsigned char gHostPathComponents[16][255];
static unsigned char gHostEntryCount = 0;
static unsigned char gHostEntryKinds[RETROLINK_MAX_LIST_ENTRIES];
static unsigned char gHostEntryNameLengths[RETROLINK_MAX_LIST_ENTRIES];
static unsigned char gHostEntryNames[RETROLINK_MAX_LIST_ENTRIES][255];
static unsigned char gHostEntryDisplay[RETROLINK_MAX_LIST_ENTRIES][256];
static const unsigned char *gHostItems[RETROLINK_MAX_LIST_ROWS];
static unsigned char gHostVisibleKinds[RETROLINK_MAX_LIST_ROWS];
static unsigned char gHostVisibleEntries[RETROLINK_MAX_LIST_ROWS];
static unsigned char gHostMessage[256];
typedef struct
{
    short volumeRefNum;
    unsigned long providerId;
    unsigned char name[32];
} RetroLinkMacVolume;

typedef struct
{
    unsigned char kind;
    unsigned short attributes;
    unsigned char utf8NameLength;
    unsigned char utf8Name[RETROLINK_MAC_MAX_UTF8_NAME_BYTES];
    unsigned char macRomanName[32];
    unsigned long dataForkSize;
    unsigned long resourceForkSize;
    unsigned long modifiedTime;
} RetroLinkMacEntry;

typedef struct
{
    unsigned short resultCode;
    unsigned long cursor;
    unsigned long nextCursor;
    unsigned char entryCount;
    RetroLinkMacEntry entries[RETROLINK_MAX_LIST_ENTRIES];
} RetroLinkMacListPage;

static const unsigned short gMacRomanUnicode[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
    0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
    0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
    0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
    0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7
};
static RetroLinkMacVolume gMacVolumes[RETROLINK_MAC_MAX_VOLUMES];
static short gMacVolumeCount = 0;
static RetroLinkMacListPage gMacResponsePage;
static RetroLinkMacListPage gMacUiPage;
static short gMacUiVolumeIndex = -1;
static short gMacUiPathCount = 0;
static unsigned char gMacUiPathComponents[RETROLINK_MAC_MAX_PATH_COMPONENTS][32];
static unsigned char gMacUiVisibleKinds[RETROLINK_MAX_LIST_ROWS];
static unsigned char gMacUiVisibleEntries[RETROLINK_MAX_LIST_ROWS];
static Boolean gPendingMacFilesClick = false;
static short gActiveFilePane = 0;
static Boolean gPendingHostOperation = false;
static unsigned long gPendingHostOperationRequestId = 0;
static unsigned long gPendingHostOperationTick = 0;
static unsigned char gPendingHostOperationKind = 0;
static const unsigned char *gMacItems[RETROLINK_MAX_LIST_ROWS];
static unsigned char gMacMessage[256];
static unsigned short gTransferTemporaryCounter = 0;
static Boolean gOutboundTransferPending = false;
static Boolean gOutboundTransferReady = false;
static Boolean gOutboundTransferSent = false;
static unsigned long gOutboundTransferRequestId = 0;
static unsigned long gOutboundTransferLength = 0;
static unsigned long gOutboundTransferCrc = 0;
static Ptr gOutboundTransferBuffer = NULL;

static void RequestMacControl(RetroLinkForm form);

static unsigned short ReadBigEndian16(const unsigned char *data)
{
    return (unsigned short)(((unsigned short)data[0] << 8) | data[1]);
}

static unsigned long ReadBigEndian32(const unsigned char *data)
{
    return ((unsigned long)data[0] << 24) |
        ((unsigned long)data[1] << 16) |
        ((unsigned long)data[2] << 8) |
        (unsigned long)data[3];
}

static unsigned long long ReadBigEndian64(const unsigned char *data)
{
    unsigned long long value = 0;
    short index;
    for (index = 0; index < 8; index++)
        value = (value << 8) | data[index];
    return value;
}

static void WriteBigEndian16(unsigned char *data, unsigned short value)
{
    data[0] = (unsigned char)(value >> 8);
    data[1] = (unsigned char)value;
}

static void WriteBigEndian32(unsigned char *data, unsigned long value)
{
    data[0] = (unsigned char)(value >> 24);
    data[1] = (unsigned char)(value >> 16);
    data[2] = (unsigned char)(value >> 8);
    data[3] = (unsigned char)value;
}

static void WriteBigEndian64(unsigned char *data, unsigned long long value)
{
    data[0] = (unsigned char)(value >> 56);
    data[1] = (unsigned char)(value >> 48);
    data[2] = (unsigned char)(value >> 40);
    data[3] = (unsigned char)(value >> 32);
    data[4] = (unsigned char)(value >> 24);
    data[5] = (unsigned char)(value >> 16);
    data[6] = (unsigned char)(value >> 8);
    data[7] = (unsigned char)value;
}

static short MacRomanToUtf8(
    const unsigned char *pascalName,
    unsigned char *utf8Name,
    unsigned short capacity,
    unsigned char *utf8Length)
{
    unsigned short inputIndex;
    unsigned short outputLength = 0;

    for (inputIndex = 0; inputIndex < pascalName[0]; inputIndex++)
    {
        unsigned long codePoint = pascalName[inputIndex + 1] < 0x80
            ? pascalName[inputIndex + 1]
            : gMacRomanUnicode[pascalName[inputIndex + 1] - 0x80];
        unsigned char encoded[3];
        unsigned short encodedLength;

        if (codePoint < 0x80)
        {
            encoded[0] = (unsigned char)codePoint;
            encodedLength = 1;
        }
        else if (codePoint < 0x800)
        {
            encoded[0] = (unsigned char)(0xC0 | (codePoint >> 6));
            encoded[1] = (unsigned char)(0x80 | (codePoint & 0x3F));
            encodedLength = 2;
        }
        else
        {
            encoded[0] = (unsigned char)(0xE0 | (codePoint >> 12));
            encoded[1] = (unsigned char)(0x80 | ((codePoint >> 6) & 0x3F));
            encoded[2] = (unsigned char)(0x80 | (codePoint & 0x3F));
            encodedLength = 3;
        }

        if (outputLength + encodedLength > capacity)
            return 0;
        memcpy(utf8Name + outputLength, encoded, encodedLength);
        outputLength = (unsigned short)(outputLength + encodedLength);
    }

    *utf8Length = (unsigned char)outputLength;
    return 1;
}

static short Utf8ToMacRoman(
    const unsigned char *utf8Name,
    unsigned char utf8Length,
    unsigned char *pascalName)
{
    unsigned short inputIndex = 0;
    unsigned short outputLength = 0;

    while (inputIndex < utf8Length)
    {
        unsigned char first = utf8Name[inputIndex];
        unsigned long codePoint;
        unsigned short sequenceLength;
        unsigned short tableIndex;
        unsigned char macRomanValue;

        if (first < 0x80)
        {
            codePoint = first;
            sequenceLength = 1;
        }
        else if (first >= 0xC2 && first <= 0xDF)
        {
            sequenceLength = 2;
            if (inputIndex + sequenceLength > utf8Length ||
                (utf8Name[inputIndex + 1] & 0xC0) != 0x80)
                return 0;
            codePoint = ((unsigned long)(first & 0x1F) << 6) |
                (unsigned long)(utf8Name[inputIndex + 1] & 0x3F);
        }
        else if (first >= 0xE0 && first <= 0xEF)
        {
            sequenceLength = 3;
            if (inputIndex + sequenceLength > utf8Length ||
                (utf8Name[inputIndex + 1] & 0xC0) != 0x80 ||
                (utf8Name[inputIndex + 2] & 0xC0) != 0x80 ||
                (first == 0xE0 && utf8Name[inputIndex + 1] < 0xA0) ||
                (first == 0xED && utf8Name[inputIndex + 1] > 0x9F))
                return 0;
            codePoint = ((unsigned long)(first & 0x0F) << 12) |
                ((unsigned long)(utf8Name[inputIndex + 1] & 0x3F) << 6) |
                (unsigned long)(utf8Name[inputIndex + 2] & 0x3F);
        }
        else if (first >= 0xF0 && first <= 0xF4)
        {
            sequenceLength = 4;
            if (inputIndex + sequenceLength > utf8Length ||
                (utf8Name[inputIndex + 1] & 0xC0) != 0x80 ||
                (utf8Name[inputIndex + 2] & 0xC0) != 0x80 ||
                (utf8Name[inputIndex + 3] & 0xC0) != 0x80 ||
                (first == 0xF0 && utf8Name[inputIndex + 1] < 0x90) ||
                (first == 0xF4 && utf8Name[inputIndex + 1] > 0x8F))
                return 0;
            codePoint = ((unsigned long)(first & 0x07) << 18) |
                ((unsigned long)(utf8Name[inputIndex + 1] & 0x3F) << 12) |
                ((unsigned long)(utf8Name[inputIndex + 2] & 0x3F) << 6) |
                (unsigned long)(utf8Name[inputIndex + 3] & 0x3F);
        }
        else
        {
            return 0;
        }

        if (codePoint == 0 || codePoint == '\\' || codePoint == ':')
            return 0;
        if (codePoint < 0x80)
        {
            macRomanValue = (unsigned char)codePoint;
        }
        else
        {
            macRomanValue = 0;
            for (tableIndex = 0; tableIndex < 128; tableIndex++)
            {
                if (gMacRomanUnicode[tableIndex] == codePoint)
                {
                    macRomanValue = (unsigned char)(tableIndex + 0x80);
                    break;
                }
            }
            if (macRomanValue == 0)
                return 0;
        }
        if (outputLength >= RETROLINK_MAC_MAX_NAME_BYTES)
            return 0;
        pascalName[outputLength + 1] = macRomanValue;
        outputLength++;
        inputIndex = (unsigned short)(inputIndex + sequenceLength);
    }

    if (outputLength == 0)
        return 0;
    pascalName[0] = (unsigned char)outputLength;
    return 1;
}

static OSErr EnumerateMacVolumes(void)
{
    short volumeIndex;

    gMacVolumeCount = 0;
    for (volumeIndex = 1; volumeIndex <= RETROLINK_MAC_MAX_VOLUMES; volumeIndex++)
    {
        HParamBlockRec volumeInfo;
        unsigned char volumeName[32];
        OSErr error;

        memset(&volumeInfo, 0, sizeof(volumeInfo));
        memset(volumeName, 0, sizeof(volumeName));
        volumeInfo.volumeParam.ioNamePtr = (StringPtr)volumeName;
        volumeInfo.volumeParam.ioVolIndex = volumeIndex;
        error = PBHGetVInfoSync((HParmBlkPtr)&volumeInfo);
        if (error == nsvErr || error == fnfErr)
            return noErr;
        if (error != noErr)
            return error;
        if (volumeName[0] == 0)
            return paramErr;

        gMacVolumes[gMacVolumeCount].volumeRefNum =
            volumeInfo.volumeParam.ioVRefNum;
        gMacVolumes[gMacVolumeCount].providerId =
            (unsigned long)(gMacVolumeCount + 1);
        memcpy(gMacVolumes[gMacVolumeCount].name, volumeName, volumeName[0] + 1);
        gMacVolumeCount++;
    }
    return noErr;
}

static unsigned short CalculateCrc16(const unsigned char *data, long length)
{
    unsigned short crc = 0xFFFF;
    long index;
    short bit;

    for (index = 0; index < length; index++)
    {
        crc ^= (unsigned short)((unsigned short)data[index] << 8);
        for (bit = 0; bit < 8; bit++)
        {
            if ((crc & 0x8000) != 0)
                crc = (unsigned short)((crc << 1) ^ 0x1021);
            else
                crc = (unsigned short)(crc << 1);
        }
    }
    return crc;
}

static void SetText(RetroLinkForm form, const char *controlId, const char *value)
{
    unsigned char pascalText[256];
    long length = (long)strlen(value);

    if (length > 255)
        length = 255;
    pascalText[0] = (unsigned char)length;
    memcpy(pascalText + 1, value, (size_t)length);
    MacStudioSetControlText(form, controlId, pascalText);
}

static void UpdateRoleText(RetroLinkForm form)
{
    if (!gConnected)
        SetText(form, "RoleStatus", "Role: waiting for REMOTE");
    else if (memcmp(gActiveEndpointId, gLocalEndpointId, 16) == 0)
        SetText(form, "RoleStatus", "Role: LOCAL ACTIVE");
    else
        SetText(form, "RoleStatus", "Role: REMOTE ACTIVE");
}

static void InitializeEndpointId(void)
{
    unsigned long seconds = 0;
    unsigned long ticks = (unsigned long)TickCount();
    short index;

    GetDateTime(&seconds);
    for (index = 0; index < 16; index++)
    {
        short shift = (short)((index & 3) * 8);
        gLocalEndpointId[index] = (unsigned char)(
            (seconds >> shift) ^
            (ticks >> ((3 - (index & 3)) * 8)) ^
            (unsigned long)(index * 37 + 0x5D));
    }
    gEndpointInitialized = true;
}

static short SendFrame(
    unsigned char messageType,
    unsigned long requestId,
    const unsigned char *payload,
    unsigned short payloadLength)
{
    unsigned char encoded[RETROLINK_MAX_FRAME];
    long frameLength = RETROLINK_HEADER_SIZE + payloadLength + 2;
    unsigned short crc;

    if (payloadLength > RETROLINK_MAX_PAYLOAD ||
        (payloadLength > 0 && payload == NULL))
        return paramErr;

    encoded[0] = 'R';
    encoded[1] = 'L';
    encoded[2] = 'N';
    encoded[3] = 'K';
    encoded[4] = 1;
    encoded[5] = messageType;
    encoded[6] = 0;
    encoded[7] = 0;
    memcpy(encoded + 8, gSessionId, 16);
    WriteBigEndian32(encoded + 24, requestId);
    WriteBigEndian16(encoded + 28, payloadLength);
    if (payloadLength > 0)
        memcpy(encoded + RETROLINK_HEADER_SIZE, payload, payloadLength);
    crc = CalculateCrc16(encoded, frameLength - 2);
    WriteBigEndian16(encoded + frameLength - 2, crc);
    return MacStudioSerialWrite("SerialPort1", encoded, frameLength);
}

static void SetHostListMessage(RetroLinkForm form, const char *message)
{
    long length = (long)strlen(message);
    if (length > 255)
        length = 255;
    gHostMessage[0] = (unsigned char)length;
    memcpy(gHostMessage + 1, message, (size_t)length);
    gHostItems[0] = gHostMessage;
    gHostVisibleKinds[0] = 4;
    MacStudioSetListBoxItems(form, "HostFiles", gHostItems, 1);
}

static void RebuildHostList(RetroLinkForm form)
{
    static const unsigned char parentRow[] = {
        12, 'U', 'p', ' ', 't', 'o', ' ', 'p', 'a', 'r', 'e', 'n', 't'
    };
    static const unsigned char moreRow[] = {7, 'M', 'o', 'r', 'e', '.', '.', '.'};
    short visibleCount = 0;
    short index;

    if (gHostPathCount > 0)
    {
        gHostItems[visibleCount] = parentRow;
        gHostVisibleKinds[visibleCount] = 2;
        gHostVisibleEntries[visibleCount] = 0;
        visibleCount++;
    }

    for (index = 0; index < gHostEntryCount; index++)
    {
        gHostItems[visibleCount] = gHostEntryDisplay[index];
        gHostVisibleKinds[visibleCount] = gHostEntryKinds[index];
        gHostVisibleEntries[visibleCount] = (unsigned char)index;
        visibleCount++;
    }

    if (gHostHasMore && visibleCount < RETROLINK_MAX_LIST_ROWS)
    {
        gHostItems[visibleCount] = moreRow;
        gHostVisibleKinds[visibleCount] = 3;
        gHostVisibleEntries[visibleCount] = 0;
        visibleCount++;
    }

    MacStudioSetListBoxItems(form, "HostFiles", gHostItems, visibleCount);
}

static short IsValidUtf8Name(
    const unsigned char *name,
    unsigned char length,
    short allowSlash)
{
    unsigned short index = 0;

    while (index < length)
    {
        unsigned char first = name[index];
        unsigned short sequenceLength;
        unsigned char second;

        if (first == 0 ||
            (!allowSlash && first == '/') ||
            first == '\\' ||
            first == ':')
            return 0;
        if (first <= 0x7F)
        {
            sequenceLength = 1;
        }
        else if (first >= 0xC2 && first <= 0xDF)
        {
            sequenceLength = 2;
        }
        else if (first >= 0xE0 && first <= 0xEF)
        {
            sequenceLength = 3;
        }
        else if (first >= 0xF0 && first <= 0xF4)
        {
            sequenceLength = 4;
        }
        else
        {
            return 0;
        }

        if (index + sequenceLength > length)
            return 0;
        if (sequenceLength > 1)
        {
            second = name[index + 1];
            if ((second & 0xC0) != 0x80 ||
                (first == 0xE0 && second < 0xA0) ||
                (first == 0xED && second > 0x9F) ||
                (first == 0xF0 && second < 0x90) ||
                (first == 0xF4 && second > 0x8F))
                return 0;
            if (sequenceLength > 2 &&
                ((name[index + 2] & 0xC0) != 0x80 ||
                 (sequenceLength == 4 && (name[index + 3] & 0xC0) != 0x80)))
                return 0;
        }
        index += sequenceLength;
    }
    return 1;
}

static void SetMacListMessage(RetroLinkForm form, const char *message)
{
    long length = (long)strlen(message);
    if (length > 255)
        length = 255;
    gMacMessage[0] = (unsigned char)length;
    memcpy(gMacMessage + 1, message, (size_t)length);
    gMacItems[0] = gMacMessage;
    gMacUiVisibleKinds[0] = 5;
    MacStudioSetListBoxItems(form, "MacFiles", gMacItems, 1);
}

static void SetMacVolumesLabel(RetroLinkForm form, const unsigned char *text)
{
    MacStudioSetControlText(form, "MacVolumesLabel", text);
}

static short FindMacVolumeIndex(unsigned long providerId)
{
    short index;
    for (index = 0; index < gMacVolumeCount; index++)
    {
        if (gMacVolumes[index].providerId == providerId)
            return index;
    }
    return -1;
}

static OSErr ResolveMacDirectory(
    short volumeIndex,
    short pathCount,
    const unsigned char pathComponents[][32],
    long *directoryId)
{
    long currentDirectoryId = RETROLINK_HFS_ROOT_DIRECTORY_ID;
    short componentIndex;

    if (volumeIndex < 0 || volumeIndex >= gMacVolumeCount ||
        pathCount < 0 || pathCount > RETROLINK_MAC_MAX_PATH_COMPONENTS ||
        directoryId == NULL)
        return paramErr;

    for (componentIndex = 0; componentIndex < pathCount; componentIndex++)
    {
        CInfoPBRec catalogInfo;
        unsigned char componentName[32];
        OSErr error;

        if (pathComponents[componentIndex][0] == 0 ||
            pathComponents[componentIndex][0] > RETROLINK_MAC_MAX_NAME_BYTES)
            return paramErr;
        memcpy(
            componentName,
            pathComponents[componentIndex],
            pathComponents[componentIndex][0] + 1);
        memset(&catalogInfo, 0, sizeof(catalogInfo));
        catalogInfo.dirInfo.ioNamePtr = (StringPtr)componentName;
        catalogInfo.dirInfo.ioVRefNum = gMacVolumes[volumeIndex].volumeRefNum;
        catalogInfo.dirInfo.ioFDirIndex = 0;
        catalogInfo.dirInfo.ioDrDirID = currentDirectoryId;
        error = PBGetCatInfoSync(&catalogInfo);
        if (error != noErr)
            return error;
        if ((catalogInfo.dirInfo.ioFlAttrib & RETROLINK_MAC_DIRECTORY_FLAG) == 0 ||
            catalogInfo.dirInfo.ioDrDirID <= 0)
            return fnfErr;
        currentDirectoryId = catalogInfo.dirInfo.ioDrDirID;
    }

    *directoryId = currentDirectoryId;
    return noErr;
}

static OSErr GetMacCatalogEntry(
    short volumeIndex,
    long directoryId,
    unsigned long catalogIndex,
    CInfoPBRec *catalogInfo,
    unsigned char *entryName)
{
    if (catalogIndex == 0 || catalogIndex > 32767)
        return paramErr;
    memset(catalogInfo, 0, sizeof(*catalogInfo));
    memset(entryName, 0, 32);
    catalogInfo->hFileInfo.ioNamePtr = (StringPtr)entryName;
    catalogInfo->hFileInfo.ioVRefNum = gMacVolumes[volumeIndex].volumeRefNum;
    catalogInfo->hFileInfo.ioFDirIndex = (short)catalogIndex;
    catalogInfo->hFileInfo.ioDirID = directoryId;
    return PBGetCatInfoSync(catalogInfo);
}

static OSErr LoadMacDirectoryPage(
    short volumeIndex,
    short pathCount,
    const unsigned char pathComponents[][32],
    unsigned long cursor,
    unsigned short entryLimit,
    RetroLinkMacListPage *page)
{
    long directoryId;
    unsigned long catalogIndex;
    unsigned short payloadBytes = RETROLINK_LIST_PAGE_HEADER_SIZE;
    OSErr error;

    memset(page, 0, sizeof(*page));
    page->cursor = cursor;
    page->nextCursor = RETROLINK_LIST_COMPLETE;
    if (cursor >= 32767 || entryLimit == 0 ||
        entryLimit > RETROLINK_MAX_LIST_ENTRIES)
    {
        page->resultCode = 1;
        return noErr;
    }

    error = ResolveMacDirectory(
        volumeIndex,
        pathCount,
        pathComponents,
        &directoryId);
    if (error != noErr)
    {
        page->resultCode = error == fnfErr ? 1 :
            error == paramErr ? 2 : 8;
        return noErr;
    }

    catalogIndex = cursor + 1;
    for (;; catalogIndex++)
    {
        CInfoPBRec catalogInfo;
        unsigned char entryName[32];
        unsigned char utf8Length;
        RetroLinkMacEntry *entry;

        error = GetMacCatalogEntry(
            volumeIndex,
            directoryId,
            catalogIndex,
            &catalogInfo,
            entryName);
        if (error == fnfErr)
        {
            page->nextCursor = RETROLINK_LIST_COMPLETE;
            return noErr;
        }
        if (error != noErr)
        {
            page->resultCode = error == paramErr ? 2 : 8;
            page->entryCount = 0;
            return noErr;
        }

        if (page->entryCount >= entryLimit)
        {
            page->nextCursor = catalogIndex - 1;
            return noErr;
        }
        if (entryName[0] == 0 ||
            !MacRomanToUtf8(
                entryName,
                page->entries[page->entryCount].utf8Name,
                RETROLINK_MAC_MAX_UTF8_NAME_BYTES,
                &utf8Length))
        {
            page->resultCode = 8;
            page->entryCount = 0;
            return noErr;
        }
        if (payloadBytes + RETROLINK_LIST_ENTRY_FIXED_SIZE + utf8Length >
            RETROLINK_MAX_PAYLOAD)
        {
            page->nextCursor = catalogIndex - 1;
            return noErr;
        }

        entry = &page->entries[page->entryCount];
        entry->kind = (catalogInfo.hFileInfo.ioFlAttrib &
            RETROLINK_MAC_DIRECTORY_FLAG) != 0 ? 1 : 0;
        entry->attributes = 0;
        entry->utf8NameLength = utf8Length;
        memcpy(entry->macRomanName, entryName, entryName[0] + 1);
        entry->dataForkSize = (unsigned long)catalogInfo.hFileInfo.ioFlLgLen;
        entry->resourceForkSize =
            (unsigned long)catalogInfo.hFileInfo.ioFlRLgLen;
        entry->modifiedTime =
            catalogInfo.hFileInfo.ioFlMdDat >= RETROLINK_MAC_EPOCH_OFFSET
            ? catalogInfo.hFileInfo.ioFlMdDat - RETROLINK_MAC_EPOCH_OFFSET
            : 0;
        page->entryCount++;
        payloadBytes = (unsigned short)(
            payloadBytes + RETROLINK_LIST_ENTRY_FIXED_SIZE + utf8Length);
    }
}

static short SendMacListPage(
    unsigned long requestId,
    const RetroLinkMacListPage *page)
{
    unsigned char payload[RETROLINK_MAX_PAYLOAD];
    unsigned short offset = RETROLINK_LIST_PAGE_HEADER_SIZE;
    unsigned char index;

    WriteBigEndian16(payload, page->resultCode);
    WriteBigEndian32(payload + 2, page->cursor);
    WriteBigEndian32(payload + 6, page->nextCursor);
    payload[10] = page->resultCode == 0 ? page->entryCount : 0;
    if (page->resultCode == 0)
    {
        for (index = 0; index < page->entryCount; index++)
        {
            const RetroLinkMacEntry *entry = &page->entries[index];
            payload[offset] = entry->kind;
            WriteBigEndian16(payload + offset + 1, entry->attributes);
            payload[offset + 3] = entry->utf8NameLength;
            memcpy(payload + offset + 4, entry->utf8Name, entry->utf8NameLength);
            offset = (unsigned short)(offset + 4 + entry->utf8NameLength);
            WriteBigEndian64(payload + offset, entry->dataForkSize);
            WriteBigEndian32(payload + offset + 8, entry->resourceForkSize);
            WriteBigEndian32(payload + offset + 12, entry->modifiedTime);
            offset = (unsigned short)(offset + 16);
        }
    }
    return SendFrame(0x11, requestId, payload, offset);
}

static void SendMacProtocolError(
    RetroLinkForm form,
    unsigned long requestId,
    unsigned short errorCode,
    unsigned char messageType,
    const char *reason)
{
    unsigned char payload[132];
    unsigned short reasonLength = (unsigned short)strlen(reason);
    if (reasonLength > 128)
        reasonLength = 128;
    WriteBigEndian16(payload, errorCode);
    payload[2] = messageType;
    payload[3] = (unsigned char)reasonLength;
    memcpy(payload + 4, reason, reasonLength);
    if (SendFrame(0x7F, requestId, payload, (unsigned short)(reasonLength + 4)) != noErr)
        SetText(form, "ActionStatus", "Could not report the rejected directory request.");
    else
        SetText(form, "ActionStatus", reason);
}

static void HandleMacListRequest(
    RetroLinkForm form,
    const unsigned char *payload,
    unsigned short length,
    unsigned long requestId)
{
    unsigned long providerId;
    unsigned long cursor;
    unsigned short entryLimit;
    unsigned short offset = 20;
    unsigned short pathEnd;
    unsigned char pathCount;
    unsigned char pathIndex;
    unsigned char pathComponents[RETROLINK_MAC_MAX_PATH_COMPONENTS][32];
    short volumeIndex;
    char status[48];
    OSErr sendResult;

    if (length < 31)
    {
        SendMacProtocolError(form, requestId, 1, 0x10, "Malformed LIST_REQUEST payload.");
        return;
    }
    if (ReadBigEndian32(payload) != gRoleEpoch ||
        memcmp(payload + 4, gActiveEndpointId, 16) != 0 ||
        memcmp(payload + 4, gHostEndpointId, 16) != 0)
    {
        SendMacProtocolError(form, requestId, 2, 0x10, "Requester does not hold the active role.");
        return;
    }

    providerId = ReadBigEndian32(payload + offset);
    offset += 4;
    pathCount = payload[offset++];
    if (pathCount > RETROLINK_MAC_MAX_PATH_COMPONENTS)
    {
        SendMacProtocolError(form, requestId, 2, 0x10, "Path has too many components.");
        return;
    }
    pathEnd = (unsigned short)(length - 6);
    memset(pathComponents, 0, sizeof(pathComponents));
    for (pathIndex = 0; pathIndex < pathCount; pathIndex++)
    {
        unsigned char componentLength;
        if (offset >= pathEnd)
        {
            SendMacProtocolError(form, requestId, 1, 0x10, "Path component is truncated.");
            return;
        }
        componentLength = payload[offset++];
        if (componentLength == 0 ||
            componentLength > pathEnd - offset ||
            !IsValidUtf8Name(payload + offset, componentLength, 1) ||
            !Utf8ToMacRoman(
                payload + offset,
                componentLength,
                pathComponents[pathIndex]) ||
            (pathComponents[pathIndex][0] == 1 &&
             pathComponents[pathIndex][1] == '.') ||
            (pathComponents[pathIndex][0] == 2 &&
             pathComponents[pathIndex][1] == '.' &&
             pathComponents[pathIndex][2] == '.'))
        {
            SendMacProtocolError(form, requestId, 2, 0x10, "Path contains an invalid MacRoman name.");
            return;
        }
        offset = (unsigned short)(offset + componentLength);
    }
    if (offset != pathEnd)
    {
        SendMacProtocolError(form, requestId, 1, 0x10, "LIST_REQUEST has trailing or missing data.");
        return;
    }

    cursor = ReadBigEndian32(payload + offset);
    entryLimit = ReadBigEndian16(payload + offset + 4);
    if (cursor == RETROLINK_LIST_COMPLETE ||
        entryLimit == 0 || entryLimit > RETROLINK_MAX_LIST_ENTRIES)
    {
        SendMacProtocolError(form, requestId, 2, 0x10, "LIST_REQUEST cursor or limit is invalid.");
        return;
    }
    volumeIndex = FindMacVolumeIndex(providerId);
    if (volumeIndex < 0)
    {
        memset(&gMacResponsePage, 0, sizeof(gMacResponsePage));
        gMacResponsePage.resultCode = 7;
        gMacResponsePage.cursor = cursor;
        gMacResponsePage.nextCursor = RETROLINK_LIST_COMPLETE;
    }
    else
    {
        LoadMacDirectoryPage(
            volumeIndex,
            pathCount,
            pathComponents,
            cursor,
            entryLimit,
            &gMacResponsePage);
    }

    sendResult = SendMacListPage(requestId, &gMacResponsePage);
    if (sendResult == noErr)
    {
        sprintf(
            status,
            "Mac directory sent, result %u.",
            (unsigned int)gMacResponsePage.resultCode);
        SetText(form, "ActionStatus", status);
    }
    else
        SetText(form, "ActionStatus", "Could not send the LOCAL volume directory.");
}

static void RebuildMacFilesList(RetroLinkForm form)
{
    static const unsigned char parentRow[] = {
        12, 'U', 'p', ' ', 't', 'o', ' ', 'p', 'a', 'r', 'e', 'n', 't'
    };
    static const unsigned char moreRow[] = {7, 'M', 'o', 'r', 'e', '.', '.', '.'};
    short visibleCount = 0;
    short index;

    if (gMacUiVolumeIndex < 0)
    {
        for (index = 0; index < gMacVolumeCount; index++)
        {
            gMacItems[visibleCount] = gMacVolumes[index].name;
            gMacUiVisibleKinds[visibleCount] = 4;
            gMacUiVisibleEntries[visibleCount] = (unsigned char)index;
            visibleCount++;
        }
        if (visibleCount == 0)
        {
            SetMacListMessage(form, "No mounted LOCAL volumes");
            return;
        }
    }
    else
    {
        gMacItems[visibleCount] = parentRow;
        gMacUiVisibleKinds[visibleCount] = 2;
        gMacUiVisibleEntries[visibleCount] = 0;
        visibleCount++;

        for (index = 0; index < gMacUiPage.entryCount; index++)
        {
            gMacItems[visibleCount] = gMacUiPage.entries[index].macRomanName;
            gMacUiVisibleKinds[visibleCount] = gMacUiPage.entries[index].kind;
            gMacUiVisibleEntries[visibleCount] = (unsigned char)index;
            visibleCount++;
        }

        if (gMacUiPage.nextCursor != RETROLINK_LIST_COMPLETE &&
            visibleCount < RETROLINK_MAX_LIST_ROWS)
        {
            gMacItems[visibleCount] = moreRow;
            gMacUiVisibleKinds[visibleCount] = 3;
            gMacUiVisibleEntries[visibleCount] = 0;
            visibleCount++;
        }
    }

    MacStudioSetListBoxItems(form, "MacFiles", gMacItems, visibleCount);
}

static void RefreshMacFilesList(RetroLinkForm form, unsigned long cursor)
{
    if (gMacUiVolumeIndex < 0)
    {
        static const unsigned char volumesLabel[] = {
            11, 'M', 'a', 'c', ' ', 'v', 'o', 'l', 'u', 'm', 'e', 's'
        };
        SetMacVolumesLabel(form, volumesLabel);
        RebuildMacFilesList(form);
        return;
    }

    SetMacVolumesLabel(form, gMacVolumes[gMacUiVolumeIndex].name);
    LoadMacDirectoryPage(
        gMacUiVolumeIndex,
        gMacUiPathCount,
        gMacUiPathComponents,
        cursor,
        RETROLINK_UI_LIST_ENTRIES,
        &gMacUiPage);
    if (gMacUiPage.resultCode != 0)
    {
        SetMacListMessage(form, "Directory unavailable");
        SetText(form, "ActionStatus", "The LOCAL volume directory could not be read.");
        return;
    }

    RebuildMacFilesList(form);
    if (cursor > 0)
        SetText(form, "ActionStatus", "More LOCAL directory entries loaded.");
}

static void HandleMacFilesClick(RetroLinkForm form)
{
    short row = -1;
    short result;
    unsigned char kind;
    unsigned char entry;

    gActiveFilePane = 0;
    if (gConnected &&
        memcmp(gActiveEndpointId, gLocalEndpointId, 16) != 0)
    {
        gPendingMacFilesClick = true;
        RequestMacControl(form);
        return;
    }
    gPendingMacFilesClick = false;

    result = MacStudioGetListBoxSelection(form, "MacFiles", &row);
    if (result != noErr || row < 0 || row >= RETROLINK_MAX_LIST_ROWS)
    {
        SetText(form, "ActionStatus", "Select an item in the LOCAL file list.");
        return;
    }

    kind = gMacUiVisibleKinds[row];
    entry = gMacUiVisibleEntries[row];
    if (kind == 4)
    {
        if (entry >= gMacVolumeCount)
            return;
        gMacUiVolumeIndex = entry;
        gMacUiPathCount = 0;
        RefreshMacFilesList(form, 0);
        SetText(form, "ActionStatus", "LOCAL volume opened.");
    }
    else if (kind == 2)
    {
        if (gMacUiPathCount > 0)
            gMacUiPathCount--;
        else
            gMacUiVolumeIndex = -1;
        RefreshMacFilesList(form, 0);
        SetText(form, "ActionStatus", "Parent directory opened.");
    }
    else if (kind == 3)
    {
        RefreshMacFilesList(form, gMacUiPage.nextCursor);
    }
    else if (kind == 1)
    {
        if (entry >= gMacUiPage.entryCount ||
            gMacUiPathCount >= RETROLINK_MAC_MAX_PATH_COMPONENTS)
        {
            SetText(form, "ActionStatus", "The selected LOCAL path is too deep.");
            return;
        }
        memcpy(
            gMacUiPathComponents[gMacUiPathCount],
            gMacUiPage.entries[entry].macRomanName,
            gMacUiPage.entries[entry].macRomanName[0] + 1);
        gMacUiPathCount++;
        RefreshMacFilesList(form, 0);
        SetText(form, "ActionStatus", "LOCAL directory opened.");
    }
    else if (kind == 0)
    {
        SetText(form, "ActionStatus", "Selected a LOCAL volume file.");
    }
}

static void SetHostEntry(unsigned char row, const unsigned char *name, unsigned char length,
                         unsigned char kind)
{
    unsigned short index;
    unsigned short displayLength = 0;

    gHostEntryKinds[row] = kind;
    gHostEntryNameLengths[row] = length;
    memcpy(gHostEntryNames[row], name, length);
    if (kind == 1)
        gHostEntryDisplay[row][1 + displayLength++] = '>';
    for (index = 0; index < length && displayLength < 255; index++)
    {
        unsigned char value = name[index];
        if (value >= 32 && value <= 126)
            gHostEntryDisplay[row][1 + displayLength++] = value;
        else if ((value & 0xC0) != 0x80)
            gHostEntryDisplay[row][1 + displayLength++] = '?';
    }
    if (displayLength == 0)
        gHostEntryDisplay[row][1 + displayLength++] = '?';
    gHostEntryDisplay[row][0] = (unsigned char)displayLength;
}

static short RequestHostList(RetroLinkForm form, unsigned long cursor)
{
    unsigned char payload[600];
    unsigned short offset = 0;
    unsigned short pathLength = 5;
    short index;
    unsigned long requestId;

    if (!gConnected ||
        memcmp(gActiveEndpointId, gLocalEndpointId, 16) != 0)
    {
        SetText(form, "ActionStatus", "Take Control before browsing the REMOTE files.");
        return paramErr;
    }

    for (index = 0; index < gHostPathCount; index++)
        pathLength = (unsigned short)(pathLength + 1 + gHostPathLengths[index]);
    if (pathLength > 512 ||
        4 + 16 + pathLength + 6 > sizeof(payload))
    {
        SetText(form, "ActionStatus", "The host path is too long to request.");
        return paramErr;
    }

    WriteBigEndian32(payload, gRoleEpoch);
    memcpy(payload + 4, gLocalEndpointId, 16);
    offset = 20;
    WriteBigEndian32(payload + offset, RETROLINK_WINDOWS_SHARE_PROVIDER);
    offset += 4;
    payload[offset++] = (unsigned char)gHostPathCount;
    for (index = 0; index < gHostPathCount; index++)
    {
        payload[offset++] = gHostPathLengths[index];
        memcpy(payload + offset, gHostPathComponents[index], gHostPathLengths[index]);
        offset = (unsigned short)(offset + gHostPathLengths[index]);
    }
    WriteBigEndian32(payload + offset, cursor);
    offset = (unsigned short)(offset + 4);
    WriteBigEndian16(payload + offset, RETROLINK_UI_LIST_ENTRIES);
    offset = (unsigned short)(offset + 2);

    requestId = gNextRequestId++;
    if (requestId == 0)
    {
        requestId = gNextRequestId++;
        if (requestId == 0)
            requestId = 1;
    }
    gHostListRequestId = requestId;
    gHostRequestedCursor = cursor;
    gHostEntryCount = 0;
    gHostHasMore = false;
    SetHostListMessage(form, "Loading...");
    if (SendFrame(0x10, requestId, payload, offset) == noErr)
    {
        SetText(form, "ActionStatus", "Requesting the REMOTE directory.");
        return noErr;
    }

    SetHostListMessage(form, "Request failed");
    SetText(form, "ActionStatus", "Could not request the REMOTE directory listing.");
    return ioErr;
}

static void RequestMacControl(RetroLinkForm form)
{
    unsigned char payload[21];
    unsigned long requestId;

    if (!gConnected)
    {
        SetText(form, "ActionStatus", "No REMOTE RetroLink session is connected.");
        return;
    }

    gPendingHostList = true;
    memcpy(payload, gLocalEndpointId, 16);
    WriteBigEndian32(payload + 16, gRoleEpoch);
    payload[20] = 2;
    requestId = gNextRequestId++;
    if (requestId == 0)
    {
        requestId = gNextRequestId++;
        if (requestId == 0)
            requestId = 1;
    }
    if (SendFrame(0x03, requestId, payload, sizeof(payload)) == noErr)
        SetText(form, "ActionStatus", "Control request sent to REMOTE.");
    else
        SetText(form, "ActionStatus", "Control request failed. Check the serial link.");
}

static void HandleHostListPage(
    RetroLinkForm form,
    const unsigned char *payload,
    unsigned short length,
    unsigned long requestId)
{
    unsigned short resultCode;
    unsigned long cursor;
    unsigned long nextCursor;
    unsigned char entryCount;
    unsigned short offset = 11;
    unsigned char index;

    if (requestId != gHostListRequestId || length < 11)
        return;

    resultCode = ReadBigEndian16(payload);
    cursor = ReadBigEndian32(payload + 2);
    nextCursor = ReadBigEndian32(payload + 6);
    entryCount = payload[10];
    if (cursor != gHostRequestedCursor ||
        entryCount > RETROLINK_MAX_LIST_ENTRIES)
    {
        SetHostListMessage(form, "Invalid listing");
        SetText(form, "ActionStatus", "The REMOTE side returned an invalid listing.");
        return;
    }
    if (resultCode != 0)
    {
        SetHostListMessage(form, "Share unavailable");
        SetText(form, "ActionStatus", "The REMOTE directory could not be listed.");
        return;
    }

    gHostEntryCount = 0;
    for (index = 0; index < entryCount; index++)
    {
        unsigned char kind;
        unsigned char nameLength;

        if (length - offset < 4)
        {
            SetHostListMessage(form, "Invalid listing");
            SetText(form, "ActionStatus", "The REMOTE side returned a truncated entry.");
            return;
        }

        kind = payload[offset];
        nameLength = payload[offset + 3];
        offset = (unsigned short)(offset + 4);
        if ((kind != 0 && kind != 1) ||
            nameLength == 0 ||
            length - offset < nameLength + 16 ||
            !IsValidUtf8Name(payload + offset, nameLength, 0))
        {
            SetHostListMessage(form, "Invalid listing");
            SetText(form, "ActionStatus", "The REMOTE side returned an invalid entry name.");
            return;
        }

        SetHostEntry(index, payload + offset, nameLength, kind);
        offset = (unsigned short)(offset + nameLength + 16);
        gHostEntryCount++;
    }
    if (offset != length)
    {
        SetHostListMessage(form, "Invalid listing");
        SetText(form, "ActionStatus", "The REMOTE side returned trailing listing data.");
        return;
    }

    gHostNextCursor = nextCursor;
    gHostHasMore = nextCursor != RETROLINK_LIST_COMPLETE;
    RebuildHostList(form);
    SetText(form, "ActionStatus", "REMOTE directory loaded.");
}

static void HandleHostFilesClick(RetroLinkForm form)
{
    short row = -1;
    short result;
    unsigned char kind;
    unsigned char entry;
    unsigned short pathLength = 5;
    short index;

    gActiveFilePane = 1;
    if (!gConnected)
    {
        SetText(form, "ActionStatus", "Connect the REMOTE RetroLink peer before browsing.");
        return;
    }
    if (memcmp(gActiveEndpointId, gLocalEndpointId, 16) != 0)
    {
        RequestMacControl(form);
        return;
    }

    result = MacStudioGetListBoxSelection(form, "HostFiles", &row);
    if (result != noErr || row < 0)
        return;
    if (row >= RETROLINK_MAX_LIST_ROWS)
        return;

    kind = gHostVisibleKinds[row];
    entry = gHostVisibleEntries[row];
    if (kind == 2)
    {
        if (gHostPathCount > 0)
            gHostPathCount--;
        RequestHostList(form, 0);
    }
    else if (kind == 3)
    {
        RequestHostList(form, gHostNextCursor);
    }
    else if (kind == 1)
    {
        if (entry >= gHostEntryCount || gHostPathCount >= 16)
        {
            SetText(form, "ActionStatus", "The selected host path is too deep.");
            return;
        }
        for (index = 0; index < gHostPathCount; index++)
            pathLength = (unsigned short)(pathLength + 1 + gHostPathLengths[index]);
        if (pathLength + 1 + gHostEntryNameLengths[entry] > 512)
        {
            SetText(form, "ActionStatus", "The selected host path is too long.");
            return;
        }
        gHostPathLengths[gHostPathCount] = gHostEntryNameLengths[entry];
        memcpy(
            gHostPathComponents[gHostPathCount],
            gHostEntryNames[entry],
            gHostPathLengths[gHostPathCount]);
        gHostPathCount++;
        RequestHostList(form, 0);
    }
    else if (kind == 0)
    {
        SetText(form, "ActionStatus", "Selected a REMOTE file.");
    }
}

static void SendHelloAck(RetroLinkForm form, const unsigned char *helloPayload)
{
    unsigned char payload[RETROLINK_MAX_PAYLOAD];
    unsigned short payloadLength = 37;
    unsigned char rootCount = 0;
    short volumeIndex;
    long systemVersion = 0;
    long machineType = 0;
    short systemMajor = 7;
    short systemMinor = 0;
    short result;

    if (!gEndpointInitialized)
        InitializeEndpointId();
    memcpy(gHostEndpointId, helloPayload + 5, 16);

    if (Gestalt(gestaltSystemVersion, &systemVersion) == noErr)
    {
        systemMajor = (short)((systemVersion >> 8) & 0xFF);
        systemMinor = (short)((systemVersion >> 4) & 0x0F);
    }
    Gestalt(gestaltMachineType, &machineType);

    payload[0] = 1;
    payload[1] = 1;
    payload[2] = 0;
    payload[3] = 0;
    payload[4] = (unsigned char)systemMajor;
    payload[5] = (unsigned char)systemMinor;
    WriteBigEndian16(payload + 6, (unsigned short)machineType);
    memcpy(payload + 8, gLocalEndpointId, 16);
    WriteBigEndian32(payload + 24, 0x00000041);
    WriteBigEndian32(payload + 28, 0x00000100);
    WriteBigEndian32(payload + 32, 0);
    payload[36] = 0;
    for (volumeIndex = 0; volumeIndex < gMacVolumeCount; volumeIndex++)
    {
        unsigned char utf8Name[RETROLINK_MAC_MAX_UTF8_NAME_BYTES];
        unsigned char utf8NameLength;
        if (!MacRomanToUtf8(
            gMacVolumes[volumeIndex].name,
            utf8Name,
            sizeof(utf8Name),
            &utf8NameLength))
            continue;
        WriteBigEndian32(
            payload + payloadLength,
            gMacVolumes[volumeIndex].providerId);
        payloadLength = (unsigned short)(payloadLength + 4);
        payload[payloadLength++] = utf8NameLength;
        memcpy(payload + payloadLength, utf8Name, utf8NameLength);
        payloadLength = (unsigned short)(payloadLength + utf8NameLength);
        payload[payloadLength++] = 0;
        rootCount++;
    }
    payload[36] = rootCount;

    result = SendFrame(0x02, 0, payload, payloadLength);
    if (result == noErr)
    {
        gConnected = true;
        gRoleEpoch = 0;
        memcpy(gActiveEndpointId, gHostEndpointId, 16);
        SetText(form, "ConnectionStatus", "Connected to REMOTE RetroLink at 9600 baud.");
        SetText(form, "ActionStatus", "Choose a panel and use Take Control to request the active role.");
        UpdateRoleText(form);
    }
    else
    {
        gConnected = false;
        SetText(form, "ConnectionStatus", "HELLO_ACK send failed. Check the serial cable and baud.");
    }
}

static unsigned long CalculateCrc32(const unsigned char *data, long length)
{
    unsigned long crc = 0xFFFFFFFFUL;
    long index;
    short bit;

    for (index = 0; index < length; index++)
    {
        crc ^= data[index];
        for (bit = 0; bit < 8; bit++)
            crc = (crc & 1) != 0
                ? (crc >> 1) ^ 0xEDB88320UL
                : crc >> 1;
    }
    return ~crc;
}

void TransferProgress(RetroLinkForm form, short item)
{
    short percentage = 0;
    char status[48];

    (void)item;
    if (MacStudioProtocolGetProgress("ProtocolTransfer1", &percentage) != noErr)
    {
        SetText(form, "ActionStatus", "Could not read transfer progress.");
        return;
    }
    if (MacStudioSetControlValue(form, "TransferProgressBar", percentage) != noErr)
    {
        SetText(form, "ActionStatus", "Could not update transfer progress.");
        return;
    }
    sprintf(status, "XMODEM transfer: %d%%", (int)percentage);
    SetText(form, "ActionStatus", status);
}

static short SkipTransferPath(
    const unsigned char *payload,
    unsigned short payloadLength,
    unsigned short *offset)
{
    unsigned short pathStart = *offset;
    unsigned char componentCount;
    unsigned char index;

    if (payloadLength - *offset < 5)
        return 0;
    componentCount = payload[*offset + 4];
    if (componentCount > RETROLINK_MAC_MAX_PATH_COMPONENTS)
        return 0;
    *offset = (unsigned short)(*offset + 5);
    for (index = 0; index < componentCount; index++)
    {
        unsigned char componentLength;
        if (*offset >= payloadLength)
            return 0;
        componentLength = payload[(*offset)++];
        if (componentLength == 0 || payloadLength - *offset < componentLength ||
            !IsValidUtf8Name(payload + *offset, componentLength, 1))
            return 0;
        *offset = (unsigned short)(*offset + componentLength);
    }
    return *offset - pathStart <= RETROLINK_MAC_MAX_PATH_BYTES;
}

static short ReadTransferMacPath(
    const unsigned char *payload,
    unsigned short payloadLength,
    unsigned short *offset,
    short *volumeIndex,
    short *pathCount,
    unsigned char components[][32])
{
    unsigned short pathStart = *offset;
    unsigned long providerId;
    unsigned char count;
    unsigned char index;

    if (payloadLength - *offset < 5)
        return 0;
    providerId = ReadBigEndian32(payload + *offset);
    count = payload[*offset + 4];
    *offset = (unsigned short)(*offset + 5);
    if (count > RETROLINK_MAC_MAX_PATH_COMPONENTS)
        return 0;
    *volumeIndex = FindMacVolumeIndex(providerId);
    if (*volumeIndex < 0)
        return 0;
    *pathCount = count;
    for (index = 0; index < count; index++)
    {
        unsigned char componentLength;
        unsigned char *component = components[index];
        if (*offset >= payloadLength)
            return 0;
        componentLength = payload[(*offset)++];
        if (componentLength == 0 || payloadLength - *offset < componentLength ||
            !IsValidUtf8Name(payload + *offset, componentLength, 1) ||
            !Utf8ToMacRoman(payload + *offset, componentLength, component) ||
            component[0] == 0 || component[0] > RETROLINK_MAC_MAX_NAME_BYTES)
            return 0;
        *offset = (unsigned short)(*offset + componentLength);
    }
    return *offset - pathStart <= RETROLINK_MAC_MAX_PATH_BYTES;
}

static unsigned short MacOperationResultCode(OSErr error)
{
    switch (error)
    {
        case noErr:
            return 0;
        case fnfErr:
        case dirNFErr:
        case nsvErr:
            return 1;
        case paramErr:
        case bdNamErr:
            return 2;
        case wPrErr:
        case fLckdErr:
        case vLckdErr:
            return 3;
        case dupFNErr:
            return 4;
        case fBsyErr:
            return 5;
        case dskFulErr:
        case memFullErr:
            return 6;
        default:
            return 8;
    }
}

static short ReadOperationName(
    const unsigned char *payload,
    unsigned short payloadLength,
    unsigned short *offset,
    unsigned char *macRomanName)
{
    unsigned char utf8Length;

    if (*offset >= payloadLength)
        return 0;
    utf8Length = payload[(*offset)++];
    if (utf8Length == 0 ||
        payloadLength - *offset < utf8Length ||
        !IsValidUtf8Name(payload + *offset, utf8Length, 1) ||
        !Utf8ToMacRoman(payload + *offset, utf8Length, macRomanName) ||
        macRomanName[0] == 0 ||
        macRomanName[0] > RETROLINK_MAC_MAX_NAME_BYTES ||
        (macRomanName[0] == 1 && macRomanName[1] == '.') ||
        (macRomanName[0] == 2 &&
         macRomanName[1] == '.' && macRomanName[2] == '.'))
    {
        return 0;
    }
    *offset = (unsigned short)(*offset + utf8Length);
    return 1;
}

static void SendOperationResult(
    unsigned long requestId,
    unsigned char operationKind,
    unsigned short resultCode,
    const unsigned char *canonicalMacRomanName)
{
    unsigned char payload[4 + RETROLINK_MAC_MAX_UTF8_NAME_BYTES];
    unsigned char nameLength = 0;
    unsigned short payloadLength = 4;

    WriteBigEndian16(payload, resultCode);
    payload[2] = operationKind;
    payload[3] = 0;
    if (resultCode == 0 && canonicalMacRomanName != NULL)
    {
        if (!MacRomanToUtf8(
                canonicalMacRomanName,
                payload + 4,
                RETROLINK_MAC_MAX_UTF8_NAME_BYTES,
                &nameLength))
        {
            WriteBigEndian16(payload, 8);
        }
        else
        {
            payload[3] = nameLength;
            payloadLength = (unsigned short)(4 + nameLength);
        }
    }
    SendFrame(0x15, requestId, payload, payloadLength);
}

static OSErr GetMacNamedEntry(
    short volumeIndex,
    long directoryId,
    const unsigned char *entryName,
    CInfoPBRec *catalogInfo)
{
    memset(catalogInfo, 0, sizeof(*catalogInfo));
    catalogInfo->hFileInfo.ioNamePtr = (StringPtr)entryName;
    catalogInfo->hFileInfo.ioVRefNum = gMacVolumes[volumeIndex].volumeRefNum;
    catalogInfo->hFileInfo.ioFDirIndex = 0;
    catalogInfo->hFileInfo.ioDirID = directoryId;
    return PBGetCatInfoSync(catalogInfo);
}

static void HandleFileOperationRequest(
    RetroLinkForm form,
    const unsigned char *payload,
    unsigned short payloadLength,
    unsigned long requestId,
    unsigned char operationKind)
{
    unsigned long roleEpoch;
    unsigned short offset = 20;
    unsigned short resultCode = 2;
    unsigned char sourceComponents[RETROLINK_MAC_MAX_PATH_COMPONENTS][32];
    unsigned char destinationComponents[RETROLINK_MAC_MAX_PATH_COMPONENTS][32];
    short sourceVolumeIndex = -1;
    short destinationVolumeIndex = -1;
    short sourcePathCount = 0;
    short destinationPathCount = 0;
    long sourceParentId = 0;
    long destinationParentId = 0;
    unsigned char sourceName[32];
    unsigned char destinationName[32];
    const unsigned char *canonicalName = NULL;
    char errorText[48];
    OSErr error = paramErr;

    memset(sourceComponents, 0, sizeof(sourceComponents));
    memset(destinationComponents, 0, sizeof(destinationComponents));
    if (payloadLength < 20)
    {
        SendOperationResult(requestId, operationKind, 2, NULL);
        return;
    }

    roleEpoch = ReadBigEndian32(payload);
    if (roleEpoch != gRoleEpoch ||
        memcmp(payload + 4, gHostEndpointId, 16) != 0 ||
        memcmp(gActiveEndpointId, gHostEndpointId, 16) != 0)
    {
        SendOperationResult(requestId, operationKind, 3, NULL);
        SetText(form, "ActionStatus", "Rejected a file operation from a non-active endpoint.");
        return;
    }

    if (operationKind == 0x12)
    {
        FSSpec directorySpec;
        if (!ReadTransferMacPath(
                payload,
                payloadLength,
                &offset,
                &destinationVolumeIndex,
                &destinationPathCount,
                destinationComponents) ||
            !ReadOperationName(
                payload,
                payloadLength,
                &offset,
                destinationName) ||
            offset != payloadLength)
        {
            SendOperationResult(requestId, operationKind, 2, NULL);
            SetText(form, "ActionStatus", "Rejected an invalid LOCAL folder request.");
            return;
        }
        error = ResolveMacDirectory(
            destinationVolumeIndex,
            destinationPathCount,
            (const unsigned char (*)[32])destinationComponents,
            &destinationParentId);
        if (error == noErr)
        {
            memset(&directorySpec, 0, sizeof(directorySpec));
            directorySpec.vRefNum = gMacVolumes[destinationVolumeIndex].volumeRefNum;
            directorySpec.parID = destinationParentId;
            memcpy(
                directorySpec.name,
                destinationName,
                destinationName[0] + 1);
            error = FSpDirCreate(
                &directorySpec,
                smSystemScript,
                &sourceParentId);
        }
        canonicalName = destinationName;
    }
    else if (operationKind == 0x13)
    {
        CMovePBRec moveParameters;
        if (!ReadTransferMacPath(
                payload,
                payloadLength,
                &offset,
                &sourceVolumeIndex,
                &sourcePathCount,
                sourceComponents) ||
            !ReadTransferMacPath(
                payload,
                payloadLength,
                &offset,
                &destinationVolumeIndex,
                &destinationPathCount,
                destinationComponents) ||
            !ReadOperationName(
                payload,
                payloadLength,
                &offset,
                destinationName) ||
            offset != payloadLength)
        {
            SendOperationResult(requestId, operationKind, 2, NULL);
            SetText(form, "ActionStatus", "Rejected an invalid LOCAL rename request.");
            return;
        }
        if (sourceVolumeIndex != destinationVolumeIndex ||
            sourcePathCount == 0)
        {
            SendOperationResult(requestId, operationKind, 7, NULL);
            SetText(form, "ActionStatus", "LOCAL rename requires a source and one volume.");
            return;
        }
        memcpy(
            sourceName,
            sourceComponents[sourcePathCount - 1],
            sourceComponents[sourcePathCount - 1][0] + 1);
        error = ResolveMacDirectory(
            sourceVolumeIndex,
            sourcePathCount - 1,
            (const unsigned char (*)[32])sourceComponents,
            &sourceParentId);
        if (error == noErr)
            error = ResolveMacDirectory(
                destinationVolumeIndex,
                destinationPathCount,
                (const unsigned char (*)[32])destinationComponents,
                &destinationParentId);
        if (error == noErr)
        {
            if (sourceParentId == destinationParentId)
            {
                error = HRename(
                    gMacVolumes[sourceVolumeIndex].volumeRefNum,
                    sourceParentId,
                    (ConstStringPtr)sourceName,
                    (ConstStringPtr)destinationName);
            }
            else
            {
                memset(&moveParameters, 0, sizeof(moveParameters));
                moveParameters.ioNamePtr = (StringPtr)sourceName;
                moveParameters.ioVRefNum = gMacVolumes[sourceVolumeIndex].volumeRefNum;
                moveParameters.ioNewName = (StringPtr)destinationName;
                moveParameters.ioNewDirID = destinationParentId;
                moveParameters.ioDirID = sourceParentId;
                error = PBHMoveRenameSync((HParmBlkPtr)&moveParameters);
            }
        }
        canonicalName = destinationName;
    }
    else if (operationKind == 0x14)
    {
        CInfoPBRec catalogInfo;
        unsigned char childName[32];
        long objectDirectoryId;
        if (!ReadTransferMacPath(
                payload,
                payloadLength,
                &offset,
                &sourceVolumeIndex,
                &sourcePathCount,
                sourceComponents) ||
            sourcePathCount == 0 ||
            payloadLength - offset != 1 ||
            payload[offset] != 0)
        {
            SendOperationResult(requestId, operationKind, 2, NULL);
            SetText(form, "ActionStatus", "Rejected an invalid LOCAL delete request.");
            return;
        }
        memcpy(
            sourceName,
            sourceComponents[sourcePathCount - 1],
            sourceComponents[sourcePathCount - 1][0] + 1);
        error = ResolveMacDirectory(
            sourceVolumeIndex,
            sourcePathCount - 1,
            (const unsigned char (*)[32])sourceComponents,
            &sourceParentId);
        if (error == noErr)
            error = GetMacNamedEntry(
                sourceVolumeIndex,
                sourceParentId,
                sourceName,
                &catalogInfo);
        if (error == noErr &&
            (catalogInfo.hFileInfo.ioFlAttrib & RETROLINK_MAC_DIRECTORY_FLAG) != 0)
        {
            objectDirectoryId = catalogInfo.dirInfo.ioDrDirID;
            error = GetMacCatalogEntry(
                sourceVolumeIndex,
                objectDirectoryId,
                1,
                &catalogInfo,
                childName);
            if (error == noErr)
                error = fBsyErr;
            else if (error == fnfErr)
                error = noErr;
        }
        if (error == noErr)
            error = HDelete(
                gMacVolumes[sourceVolumeIndex].volumeRefNum,
                sourceParentId,
                (ConstStr255Param)sourceName);
    }
    else
    {
        SendOperationResult(requestId, operationKind, 7, NULL);
        return;
    }

    resultCode = MacOperationResultCode(error);
    SendOperationResult(requestId, operationKind, resultCode, canonicalName);
    if (resultCode == 0)
    {
        SetText(form, "ActionStatus", "LOCAL file operation completed.");
        if (sourceVolumeIndex >= 0 &&
            sourceVolumeIndex == gMacUiVolumeIndex)
            RefreshMacFilesList(form, 0);
        else if (destinationVolumeIndex >= 0 &&
                 destinationVolumeIndex == gMacUiVolumeIndex)
            RefreshMacFilesList(form, 0);
    }
    else
    {
        sprintf(errorText, "LOCAL filesystem error: %d", (int)error);
        SetText(form, "ActionStatus", errorText);
    }
}

static short IsValidMacRomanName(const unsigned char *name)
{
    unsigned char index;
    if (name == NULL || name[0] == 0 ||
        name[0] > RETROLINK_MAC_MAX_NAME_BYTES ||
        (name[0] == 1 && name[1] == '.') ||
        (name[0] == 2 && name[1] == '.' && name[2] == '.'))
    {
        return 0;
    }
    for (index = 1; index <= name[0]; index++)
    {
        if (name[index] == 0 || name[index] == ':' ||
            name[index] < 0x20)
            return 0;
    }
    return 1;
}

static short PromptForMacName(
    RetroLinkForm form,
    const unsigned char *promptText,
    const unsigned char *initialName,
    unsigned char *name)
{
    DialogPtr promptDialog;
    short item;
    short itemType;
    Handle itemHandle;
    Rect itemBounds;

    promptDialog = GetNewDialog(129, NULL, (WindowPtr)-1);
    if (promptDialog == NULL)
    {
        SetText(form, "ActionStatus", "Could not open the name prompt dialog.");
        return 0;
    }
    GetDialogItem(promptDialog, 1, &itemType, &itemHandle, &itemBounds);
    SetDialogItemText(itemHandle, promptText);
    GetDialogItem(promptDialog, 2, &itemType, &itemHandle, &itemBounds);
    if (initialName != NULL)
        SetDialogItemText(itemHandle, initialName);
    ShowWindow((WindowPtr)promptDialog);
    SelectDialogItemText(promptDialog, 2, 0, 32767);
    for (;;)
    {
        ModalDialog(NULL, &item);
        if (item == 3)
        {
            GetDialogItem(promptDialog, 2, &itemType, &itemHandle, &itemBounds);
            GetDialogItemText(itemHandle, name);
            DisposeDialog(promptDialog);
            return 1;
        }
        if (item == 4)
        {
            DisposeDialog(promptDialog);
            return 0;
        }
    }
}

static short AppendHostPathRef(
    unsigned char *payload,
    unsigned short *offset,
    const unsigned char *extraName,
    unsigned char extraNameLength)
{
    short componentIndex;
    unsigned char componentCount = (unsigned char)gHostPathCount;
    unsigned short pathStart = *offset;

    if (gHostPathCount < 0 ||
        gHostPathCount > RETROLINK_MAC_MAX_PATH_COMPONENTS ||
        (extraName != NULL &&
         (extraNameLength == 0 ||
          gHostPathCount >= RETROLINK_MAC_MAX_PATH_COMPONENTS)))
    {
        return 0;
    }
    if (extraName != NULL)
        componentCount++;
    if ((unsigned long)*offset + 5 > RETROLINK_MAX_PAYLOAD)
        return 0;

    WriteBigEndian32(payload + *offset, RETROLINK_WINDOWS_SHARE_PROVIDER);
    *offset = (unsigned short)(*offset + 4);
    payload[(*offset)++] = componentCount;
    for (componentIndex = 0; componentIndex < gHostPathCount; componentIndex++)
    {
        unsigned char componentLength = gHostPathLengths[componentIndex];
        if (componentLength == 0 ||
            !IsValidUtf8Name(
                gHostPathComponents[componentIndex],
                componentLength,
                0) ||
            (unsigned long)*offset + 1 + componentLength > RETROLINK_MAX_PAYLOAD)
        {
            return 0;
        }
        payload[(*offset)++] = componentLength;
        memcpy(
            payload + *offset,
            gHostPathComponents[componentIndex],
            componentLength);
        *offset = (unsigned short)(*offset + componentLength);
    }
    if (extraName != NULL)
    {
        if (!IsValidUtf8Name(extraName, extraNameLength, 0) ||
            (unsigned long)*offset + 1 + extraNameLength > RETROLINK_MAX_PAYLOAD)
        {
            return 0;
        }
        payload[(*offset)++] = extraNameLength;
        memcpy(payload + *offset, extraName, extraNameLength);
        *offset = (unsigned short)(*offset + extraNameLength);
    }
    return *offset - pathStart <= RETROLINK_MAC_MAX_PATH_BYTES;
}

static short SendHostOperation(
    RetroLinkForm form,
    unsigned char operationKind,
    const unsigned char *payload,
    unsigned short payloadLength)
{
    unsigned long requestId;

    if (!gConnected ||
        memcmp(gActiveEndpointId, gLocalEndpointId, 16) != 0)
    {
        SetText(form, "ActionStatus", "Connect and take control before changing REMOTE files.");
        return 0;
    }
    if (gPendingHostOperation)
    {
        SetText(form, "ActionStatus", "Wait for the pending REMOTE file operation.");
        return 0;
    }
    requestId = gNextRequestId++;
    if (requestId == 0)
        requestId = gNextRequestId++;
    gPendingHostOperation = true;
    gPendingHostOperationRequestId = requestId;
    gPendingHostOperationTick = (unsigned long)TickCount();
    gPendingHostOperationKind = operationKind;
    if (SendFrame(operationKind, requestId, payload, payloadLength) != noErr)
    {
        gPendingHostOperation = false;
        SetText(form, "ActionStatus", "Could not send the REMOTE file operation.");
        return 0;
    }
    SetText(form, "ActionStatus", "Waiting for the REMOTE file operation result.");
    return 1;
}

static short EncodeMacNameForHost(
    const unsigned char *macRomanName,
    unsigned char *utf8Name,
    unsigned char *utf8Length)
{
    if (!IsValidMacRomanName(macRomanName) ||
        !MacRomanToUtf8(
            macRomanName,
            utf8Name,
            RETROLINK_MAC_MAX_UTF8_NAME_BYTES,
            utf8Length))
    {
        return 0;
    }
    return 1;
}

static short GetSelectedMacEntry(
    RetroLinkForm form,
    CInfoPBRec *catalogInfo,
    unsigned char *entryName)
{
    short row = -1;
    unsigned char kind;
    unsigned char entryIndex;
    long directoryId;
    OSErr error;

    if (gMacUiVolumeIndex < 0 ||
        gMacUiVolumeIndex >= gMacVolumeCount ||
        MacStudioGetListBoxSelection(form, "MacFiles", &row) != noErr ||
        row < 0 || row >= RETROLINK_MAX_LIST_ROWS)
    {
        return 0;
    }
    kind = gMacUiVisibleKinds[row];
    entryIndex = gMacUiVisibleEntries[row];
    if ((kind != 0 && kind != 1) ||
        entryIndex >= gMacUiPage.entryCount)
    {
        return 0;
    }
    error = ResolveMacDirectory(
        gMacUiVolumeIndex,
        gMacUiPathCount,
        (const unsigned char (*)[32])gMacUiPathComponents,
        &directoryId);
    if (error != noErr)
        return 0;
    error = GetMacCatalogEntry(
        gMacUiVolumeIndex,
        directoryId,
        gMacUiPage.cursor + entryIndex + 1,
        catalogInfo,
        entryName);
    return error == noErr;
}

static short GetSelectedHostEntry(
    RetroLinkForm form,
    unsigned char *entryIndex)
{
    short row = -1;
    unsigned char kind;

    if (!gConnected ||
        MacStudioGetListBoxSelection(form, "HostFiles", &row) != noErr ||
        row < 0 || row >= RETROLINK_MAX_LIST_ROWS)
    {
        return 0;
    }
    kind = gHostVisibleKinds[row];
    *entryIndex = gHostVisibleEntries[row];
    return (kind == 0 || kind == 1) && *entryIndex < gHostEntryCount;
}

static void CreateMacFolder(RetroLinkForm form)
{
    static const unsigned char prompt[] = {
        20, 'N', 'e', 'w', ' ', 'M', 'a', 'c', ' ', 'f', 'o', 'l', 'd',
        'e', 'r', ' ', 'n', 'a', 'm', 'e', ':'
    };
    unsigned char name[256];
    long directoryId;
    FSSpec directorySpec;
    OSErr error;

    if (gMacUiVolumeIndex < 0 || gMacUiVolumeIndex >= gMacVolumeCount)
    {
        SetText(form, "ActionStatus", "Open a LOCAL volume before creating a folder.");
        return;
    }
    if (gConnected &&
        memcmp(gActiveEndpointId, gLocalEndpointId, 16) != 0)
    {
        SetText(form, "ActionStatus", "Take Control before changing LOCAL files.");
        return;
    }
    if (!PromptForMacName(form, prompt, NULL, name))
        return;
    if (!IsValidMacRomanName(name))
    {
        SetText(form, "ActionStatus", "Mac names must be 1-31 MacRoman bytes and cannot contain a colon.");
        return;
    }
    error = ResolveMacDirectory(
        gMacUiVolumeIndex,
        gMacUiPathCount,
        (const unsigned char (*)[32])gMacUiPathComponents,
        &directoryId);
    if (error == noErr)
    {
        memset(&directorySpec, 0, sizeof(directorySpec));
        directorySpec.vRefNum = gMacVolumes[gMacUiVolumeIndex].volumeRefNum;
        directorySpec.parID = directoryId;
        memcpy(directorySpec.name, name, name[0] + 1);
        error = FSpDirCreate(&directorySpec, smSystemScript, &directoryId);
    }
    if (error == noErr)
    {
        RefreshMacFilesList(form, 0);
        SetText(form, "ActionStatus", "LOCAL folder created.");
    }
    else
    {
        SetText(form, "ActionStatus", "Could not create the LOCAL folder.");
    }
}

static void RenameMacEntry(RetroLinkForm form)
{
    static const unsigned char prompt[] = {
        9, 'N', 'e', 'w', ' ', 'n', 'a', 'm', 'e', ':'
    };
    unsigned char name[256];
    unsigned char entryName[32];
    CInfoPBRec catalogInfo;
    FSSpec fileSpec;
    long parentDirectoryId;
    OSErr error;

    if (gConnected &&
        memcmp(gActiveEndpointId, gLocalEndpointId, 16) != 0)
    {
        SetText(form, "ActionStatus", "Take Control before changing LOCAL files.");
        return;
    }
    if (!GetSelectedMacEntry(form, &catalogInfo, entryName))
    {
        SetText(form, "ActionStatus", "Select a LOCAL file or folder to rename.");
        return;
    }
    parentDirectoryId = catalogInfo.hFileInfo.ioDirID;
    if (!PromptForMacName(form, prompt, entryName, name))
        return;
    if (!IsValidMacRomanName(name))
    {
        SetText(form, "ActionStatus", "Mac names must be 1-31 MacRoman bytes and cannot contain a colon.");
        return;
    }
    memset(&fileSpec, 0, sizeof(fileSpec));
    fileSpec.vRefNum = gMacVolumes[gMacUiVolumeIndex].volumeRefNum;
    fileSpec.parID = parentDirectoryId;
    memcpy(fileSpec.name, entryName, entryName[0] + 1);
    error = FSpRename(&fileSpec, (ConstStr63Param)name);
    if (error == noErr)
    {
        RefreshMacFilesList(form, 0);
        SetText(form, "ActionStatus", "LOCAL entry renamed.");
    }
    else
    {
        SetText(form, "ActionStatus", "Could not rename the LOCAL entry.");
    }
}

static void DeleteMacEntry(RetroLinkForm form)
{
    static const unsigned char prompt[] = {
        23, 'T', 'y', 'p', 'e', ' ', 'D', 'E', 'L', 'E', 'T', 'E',
        ' ', 't', 'o', ' ', 'c', 'o', 'n', 'f', 'i', 'r', 'm', ':'
    };
    static const unsigned char confirmation[] = {6, 'D', 'E', 'L', 'E', 'T', 'E'};
    unsigned char name[256];
    unsigned char entryName[32];
    unsigned char childName[32];
    CInfoPBRec catalogInfo;
    FSSpec fileSpec;
    long parentDirectoryId;
    long objectDirectoryId;
    OSErr error;

    if (gConnected &&
        memcmp(gActiveEndpointId, gLocalEndpointId, 16) != 0)
    {
        SetText(form, "ActionStatus", "Take Control before changing LOCAL files.");
        return;
    }
    if (!GetSelectedMacEntry(form, &catalogInfo, entryName))
    {
        SetText(form, "ActionStatus", "Select a LOCAL file or folder to delete.");
        return;
    }
    parentDirectoryId = catalogInfo.hFileInfo.ioDirID;
    if (!PromptForMacName(form, prompt, NULL, name))
        return;
    if (name[0] != confirmation[0] ||
        memcmp(name + 1, confirmation + 1, confirmation[0]) != 0)
    {
        SetText(form, "ActionStatus", "Delete cancelled; confirmation did not match.");
        return;
    }
    if ((catalogInfo.hFileInfo.ioFlAttrib & RETROLINK_MAC_DIRECTORY_FLAG) != 0)
    {
        objectDirectoryId = catalogInfo.dirInfo.ioDrDirID;
        error = GetMacCatalogEntry(
            gMacUiVolumeIndex,
            objectDirectoryId,
            1,
            &catalogInfo,
            childName);
        if (error == noErr)
        {
            SetText(form, "ActionStatus", "The LOCAL directory is not empty.");
            return;
        }
        if (error != fnfErr)
        {
            SetText(form, "ActionStatus", "Could not check whether the LOCAL directory is empty.");
            return;
        }
    }
    memset(&fileSpec, 0, sizeof(fileSpec));
    fileSpec.vRefNum = gMacVolumes[gMacUiVolumeIndex].volumeRefNum;
    fileSpec.parID = parentDirectoryId;
    memcpy(fileSpec.name, entryName, entryName[0] + 1);
    error = FSpDelete(&fileSpec);
    if (error == noErr)
    {
        RefreshMacFilesList(form, 0);
        SetText(form, "ActionStatus", "LOCAL entry deleted.");
    }
    else
    {
        SetText(form, "ActionStatus", "Could not delete the LOCAL entry.");
    }
}

static void SendHostFileOperation(
    RetroLinkForm form,
    unsigned char operationKind,
    const unsigned char *name,
    unsigned char nameLength)
{
    unsigned char payload[RETROLINK_MAX_PAYLOAD];
    unsigned short offset = 20;
    unsigned char selectedIndex;

    if (memcmp(gActiveEndpointId, gLocalEndpointId, 16) != 0)
    {
        SetText(form, "ActionStatus", "Take Control before changing REMOTE files.");
        return;
    }
    if (operationKind != 0x12 &&
        !GetSelectedHostEntry(form, &selectedIndex))
    {
        SetText(form, "ActionStatus", "Select a REMOTE file or folder first.");
        return;
    }

    WriteBigEndian32(payload, gRoleEpoch);
    memcpy(payload + 4, gLocalEndpointId, 16);
    if (operationKind == 0x12)
    {
        if (!AppendHostPathRef(payload, &offset, NULL, 0))
        {
            SetText(form, "ActionStatus", "The REMOTE path is too long.");
            return;
        }
        payload[offset++] = nameLength;
        memcpy(payload + offset, name, nameLength);
        offset = (unsigned short)(offset + nameLength);
    }
    else if (operationKind == 0x13)
    {
        if (!AppendHostPathRef(
                payload,
                &offset,
                gHostEntryNames[selectedIndex],
                gHostEntryNameLengths[selectedIndex]) ||
            !AppendHostPathRef(payload, &offset, NULL, 0))
        {
            SetText(form, "ActionStatus", "The REMOTE rename path is too long.");
            return;
        }
        payload[offset++] = nameLength;
        memcpy(payload + offset, name, nameLength);
        offset = (unsigned short)(offset + nameLength);
    }
    else if (operationKind == 0x14)
    {
        if (!AppendHostPathRef(
                payload,
                &offset,
                gHostEntryNames[selectedIndex],
                gHostEntryNameLengths[selectedIndex]) ||
            offset >= RETROLINK_MAX_PAYLOAD)
        {
            SetText(form, "ActionStatus", "The REMOTE delete path is too long.");
            return;
        }
        payload[offset++] = 0;
    }
    if (!SendHostOperation(form, operationKind, payload, offset))
        return;
}

static void CreateHostFolder(RetroLinkForm form)
{
    static const unsigned char prompt[] = {
        24, 'N', 'e', 'w', ' ', 'W', 'i', 'n', 'd', 'o', 'w', 's',
        ' ', 'f', 'o', 'l', 'd', 'e', 'r', ' ', 'n', 'a', 'm', 'e', ':'
    };
    unsigned char macRomanName[256];
    unsigned char utf8Name[RETROLINK_MAC_MAX_UTF8_NAME_BYTES];
    unsigned char utf8Length;

    if (!gConnected)
    {
        SetText(form, "ActionStatus", "Connect the REMOTE RetroLink peer before changing files.");
        return;
    }
    if (!PromptForMacName(form, prompt, NULL, macRomanName))
        return;
    if (!EncodeMacNameForHost(macRomanName, utf8Name, &utf8Length))
    {
        SetText(form, "ActionStatus", "The new name cannot be represented as a valid LOCAL/REMOTE name.");
        return;
    }
    SendHostFileOperation(form, 0x12, utf8Name, utf8Length);
}

static void RenameHostEntry(RetroLinkForm form)
{
    static const unsigned char prompt[] = {
        9, 'N', 'e', 'w', ' ', 'n', 'a', 'm', 'e', ':'
    };
    unsigned char macRomanName[256];
    unsigned char utf8Name[RETROLINK_MAC_MAX_UTF8_NAME_BYTES];
    unsigned char utf8Length;
    unsigned char selectedIndex;
    unsigned char initialName[256];
    unsigned char initialLength;

    if (!GetSelectedHostEntry(form, &selectedIndex))
    {
        SetText(form, "ActionStatus", "Select a REMOTE file or folder to rename.");
        return;
    }
    initialLength = gHostEntryNameLengths[selectedIndex];
    if (initialLength > RETROLINK_MAC_MAX_NAME_BYTES ||
        !Utf8ToMacRoman(
            gHostEntryNames[selectedIndex],
            initialLength,
            initialName) ||
        !PromptForMacName(form, prompt, initialName, macRomanName))
    {
        return;
    }
    if (!EncodeMacNameForHost(macRomanName, utf8Name, &utf8Length))
    {
        SetText(form, "ActionStatus", "The new name cannot be represented as a valid LOCAL/REMOTE name.");
        return;
    }
    SendHostFileOperation(form, 0x13, utf8Name, utf8Length);
}

static void DeleteHostEntry(RetroLinkForm form)
{
    static const unsigned char prompt[] = {
        23, 'T', 'y', 'p', 'e', ' ', 'D', 'E', 'L', 'E', 'T', 'E',
        ' ', 't', 'o', ' ', 'c', 'o', 'n', 'f', 'i', 'r', 'm', ':'
    };
    static const unsigned char confirmation[] = {6, 'D', 'E', 'L', 'E', 'T', 'E'};
    unsigned char answer[256];
    unsigned char selectedIndex;

    if (!GetSelectedHostEntry(form, &selectedIndex))
    {
        SetText(form, "ActionStatus", "Select a REMOTE file or folder to delete.");
        return;
    }
    if (!PromptForMacName(form, prompt, NULL, answer))
        return;
    if (answer[0] != confirmation[0] ||
        memcmp(answer + 1, confirmation + 1, confirmation[0]) != 0)
    {
        SetText(form, "ActionStatus", "Delete cancelled; confirmation did not match.");
        return;
    }
    SendHostFileOperation(form, 0x14, NULL, 0);
}

void CreateMacFolderClick(RetroLinkForm form, short item)
{
    (void)item;
    if (gActiveFilePane == 1)
        CreateHostFolder(form);
    else
        CreateMacFolder(form);
}

void RenameMacEntryClick(RetroLinkForm form, short item)
{
    (void)item;
    if (gActiveFilePane == 1)
        RenameHostEntry(form);
    else
        RenameMacEntry(form);
}

void DeleteMacEntryClick(RetroLinkForm form, short item)
{
    (void)item;
    if (gActiveFilePane == 1)
        DeleteHostEntry(form);
    else
        DeleteMacEntry(form);
}

static void SendTransferReady(
    unsigned long requestId,
    unsigned short resultCode,
    unsigned long long length,
    unsigned long crc32)
{
    unsigned char payload[17];
    WriteBigEndian16(payload, resultCode);
    WriteBigEndian64(payload + 2, length);
    WriteBigEndian32(payload + 10, crc32);
    payload[14] = 1;
    WriteBigEndian16(payload + 15, 128);
    SendFrame(RETROLINK_TRANSFER_READY, requestId, payload, sizeof(payload));
}

static void SendTransferComplete(
    unsigned long requestId,
    unsigned short resultCode,
    unsigned long length,
    unsigned long crc32)
{
    unsigned char payload[14];
    WriteBigEndian16(payload, resultCode);
    WriteBigEndian64(payload + 2, length);
    WriteBigEndian32(payload + 10, crc32);
    SendFrame(RETROLINK_TRANSFER_COMPLETE, requestId, payload, sizeof(payload));
}

static void CreateTransferTemporaryName(
    unsigned short sequence,
    unsigned char *name)
{
    static const unsigned char hexDigits[] = "0123456789ABCDEF";
    name[0] = 11;
    name[1] = '~';
    name[2] = 'R';
    name[3] = 'L';
    name[4] = hexDigits[(sequence >> 12) & 0x0F];
    name[5] = hexDigits[(sequence >> 8) & 0x0F];
    name[6] = hexDigits[(sequence >> 4) & 0x0F];
    name[7] = hexDigits[sequence & 0x0F];
    name[8] = '.';
    name[9] = 'T';
    name[10] = 'M';
    name[11] = 'P';
}

static OSErr CreateTransferTemporaryFile(
    short volumeRefNum,
    long directoryId,
    FSSpec *temporarySpec)
{
    short attempt;

    for (attempt = 0; attempt < 16; attempt++)
    {
        unsigned char temporaryName[12];
        OSErr error;
        gTransferTemporaryCounter++;
        CreateTransferTemporaryName(gTransferTemporaryCounter, temporaryName);
        temporarySpec->vRefNum = volumeRefNum;
        temporarySpec->parID = directoryId;
        memcpy(
            temporarySpec->name,
            temporaryName,
            temporaryName[0] + 1);
        error = FSpCreate(temporarySpec, '????', '????', smSystemScript);
        if (error == noErr)
            return noErr;
        if (error != dupFNErr)
            return error;
    }
    return dupFNErr;
}

static void HandleCopyPrepare(
    RetroLinkForm form,
    const unsigned char *payload,
    unsigned short payloadLength,
    unsigned long requestId)
{
    unsigned short offset = 36;
    unsigned short resultCode = 1;
    unsigned short nameLength;
    unsigned long roleEpoch;
    unsigned long long streamLength;
    unsigned long expectedCrc;
    unsigned char overwrite;
    unsigned char transferMode;
    unsigned char transferFormat;
    unsigned char destinationName[64];
    unsigned char pathComponents[RETROLINK_MAC_MAX_PATH_COMPONENTS][32];
    short destinationVolumeIndex = -1;
    short destinationPathCount = 0;
    long destinationDirectoryId = 0;
    long allocationLength = 0;
    long receivedLength = 0;
    long writeCount;
    Ptr transferBuffer = NULL;
    short fileRefNum = 0;
    OSErr error = paramErr;
    FSSpec temporarySpec;
    CInfoPBRec catalogInfo;

    if (payloadLength < 36)
    {
        SendTransferReady(requestId, resultCode, 0, 0);
        return;
    }

    roleEpoch = ReadBigEndian32(payload);
    if (roleEpoch != gRoleEpoch ||
        memcmp(payload + 4, gHostEndpointId, 16) != 0 ||
        memcmp(payload + 20, gHostEndpointId, 16) != 0 ||
        memcmp(gActiveEndpointId, gHostEndpointId, 16) != 0 ||
        !SkipTransferPath(payload, payloadLength, &offset) ||
        payloadLength - offset < 16 ||
        memcmp(payload + offset, gLocalEndpointId, 16) != 0)
    {
        SendTransferReady(requestId, 2, 0, 0);
        SetText(form, "ActionStatus", "The copy request is not authorized by the active REMOTE endpoint.");
        return;
    }
    offset = (unsigned short)(offset + 16);
    if (!ReadTransferMacPath(
            payload,
            payloadLength,
            &offset,
            &destinationVolumeIndex,
            &destinationPathCount,
            pathComponents) ||
        payloadLength - offset < 1)
    {
        SendTransferReady(requestId, 1, 0, 0);
        SetText(form, "ActionStatus", "The requested LOCAL destination path is invalid.");
        return;
    }

    nameLength = payload[offset++];
    if (nameLength == 0 || payloadLength - offset != nameLength + 15 ||
        !IsValidUtf8Name(payload + offset, (unsigned char)nameLength, 1) ||
        !Utf8ToMacRoman(payload + offset, (unsigned char)nameLength, destinationName) ||
        destinationName[0] == 0 ||
        destinationName[0] > RETROLINK_MAC_MAX_NAME_BYTES ||
        (destinationName[0] == 1 && destinationName[1] == '.') ||
        (destinationName[0] == 2 &&
         destinationName[1] == '.' && destinationName[2] == '.'))
    {
        SendTransferReady(requestId, 1, 0, 0);
        SetText(form, "ActionStatus", "The requested LOCAL file name is invalid or too long.");
        return;
    }
    offset = (unsigned short)(offset + nameLength);
    streamLength = ReadBigEndian64(payload + offset);
    expectedCrc = ReadBigEndian32(payload + offset + 8);
    transferFormat = payload[offset + 12];
    overwrite = payload[offset + 13];
    transferMode = payload[offset + 14];
    if (streamLength > RETROLINK_TRANSFER_MAX_BYTES ||
        transferFormat != 0 || overwrite != 0 || transferMode != 1)
    {
        SendTransferReady(requestId, 1, streamLength, expectedCrc);
        SetText(form, "ActionStatus", "The transfer exceeds the LOCAL limit or uses an unsupported mode.");
        return;
    }

    error = ResolveMacDirectory(
        destinationVolumeIndex,
        destinationPathCount,
        pathComponents,
        &destinationDirectoryId);
    if (error != noErr)
    {
        SendTransferReady(requestId, (unsigned short)error, streamLength, expectedCrc);
        SetText(form, "ActionStatus", "The destination LOCAL folder is unavailable.");
        return;
    }

    memset(&catalogInfo, 0, sizeof(catalogInfo));
    catalogInfo.hFileInfo.ioNamePtr = (StringPtr)destinationName;
    catalogInfo.hFileInfo.ioVRefNum =
        gMacVolumes[destinationVolumeIndex].volumeRefNum;
    catalogInfo.hFileInfo.ioFDirIndex = 0;
    catalogInfo.hFileInfo.ioDirID = destinationDirectoryId;
    error = PBGetCatInfoSync(&catalogInfo);
    if (error == noErr)
    {
        SendTransferReady(requestId, (unsigned short)dupFNErr, streamLength, expectedCrc);
        SetText(form, "ActionStatus", "The LOCAL destination already exists; overwrite is disabled.");
        return;
    }
    if (error != fnfErr)
    {
        SendTransferReady(requestId, (unsigned short)error, streamLength, expectedCrc);
        SetText(form, "ActionStatus", "Could not check the LOCAL destination.");
        return;
    }

    error = CreateTransferTemporaryFile(
        gMacVolumes[destinationVolumeIndex].volumeRefNum,
        destinationDirectoryId,
        &temporarySpec);
    if (error != noErr)
    {
        SendTransferReady(requestId, (unsigned short)error, streamLength, expectedCrc);
        SetText(form, "ActionStatus", "Could not create a temporary LOCAL file.");
        return;
    }
    allocationLength = (long)(((streamLength + 127) / 128) * 128);
    if (allocationLength == 0)
        allocationLength = 1;
    transferBuffer = NewPtr(allocationLength);
    if (transferBuffer == NULL || MemError() != noErr)
    {
        FSpDelete(&temporarySpec);
        SendTransferReady(requestId, (unsigned short)memFullErr, streamLength, expectedCrc);
        SetText(form, "ActionStatus", "Not enough LOCAL memory to receive the file.");
        return;
    }

    SendTransferReady(requestId, 0, streamLength, expectedCrc);
    SetText(form, "ActionStatus", "Receiving file data using XMODEM-CRC...");
    if (streamLength != 0)
    {
        error = MacStudioProtocolReceiveExact(
            "ProtocolTransfer1",
            (unsigned char *)transferBuffer,
            allocationLength,
            (long)streamLength,
            &receivedLength);
        if (error != noErr || receivedLength != (long)streamLength)
            goto copy_receive_failed;
    }
    if (CalculateCrc32((unsigned char *)transferBuffer, (long)streamLength) != expectedCrc)
    {
        error = ioErr;
        goto copy_receive_failed;
    }

    error = FSpOpenDF(&temporarySpec, fsWrPerm, &fileRefNum);
    if (error != noErr)
        goto copy_receive_failed;
    if (streamLength > 0)
    {
        writeCount = (long)streamLength;
        error = FSWrite(
            fileRefNum,
            &writeCount,
            (Ptr)transferBuffer);
        if (error == noErr && writeCount != (long)streamLength)
            error = ioErr;
    }
    if (error == noErr)
        error = FSClose(fileRefNum);
    else
        FSClose(fileRefNum);
    fileRefNum = 0;
    if (error != noErr)
        goto copy_receive_failed;

    error = FSpRename(&temporarySpec, (ConstStr63Param)destinationName);
    if (error != noErr)
        goto copy_receive_failed;
    memcpy(
        temporarySpec.name,
        destinationName,
        destinationName[0] + 1);
    error = FlushVol(0, temporarySpec.vRefNum);
    if (error != noErr)
        goto copy_receive_failed;

    DisposePtr(transferBuffer);
    SendTransferComplete(
        requestId,
        0,
        (unsigned long)streamLength,
        expectedCrc);
    SetText(form, "ActionStatus", "File transferred and verified on LOCAL.");
    RefreshMacFilesList(form, 0);
    return;

copy_receive_failed:
    if (fileRefNum != 0)
        FSClose(fileRefNum);
    if (transferBuffer != NULL)
        DisposePtr(transferBuffer);
    FSpDelete(&temporarySpec);
    SendTransferComplete(
        requestId,
        error == noErr ? (unsigned short)ioErr : (unsigned short)error,
        (unsigned long)streamLength,
        expectedCrc);
    SetText(form, "ActionStatus", "Transfer failed; the temporary LOCAL file was removed.");
}

static short IsDos83FileName(const unsigned char *name, unsigned char length)
{
    unsigned char index;
    unsigned char baseLength = 0;
    unsigned char extensionLength = 0;
    short inExtension = 0;

    if (name == NULL || length == 0)
        return 0;
    for (index = 0; index < length; index++)
    {
        unsigned char value = name[index];
        if (value == '.')
        {
            if (inExtension || baseLength == 0 || index + 1 == length)
                return 0;
            inExtension = 1;
            continue;
        }
        if (!((value >= 'A' && value <= 'Z') ||
              (value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') ||
              value == '_' || value == '-'))
            return 0;
        if (inExtension)
            extensionLength++;
        else
            baseLength++;
    }
    return baseLength <= 8 && extensionLength <= 3;
}

static void ReleaseOutboundTransfer(void)
{
    if (gOutboundTransferBuffer != NULL)
        DisposePtr(gOutboundTransferBuffer);
    gOutboundTransferBuffer = NULL;
    gOutboundTransferPending = false;
    gOutboundTransferReady = false;
    gOutboundTransferSent = false;
    gOutboundTransferRequestId = 0;
    gOutboundTransferLength = 0;
    gOutboundTransferCrc = 0;
}

static void FailOutboundTransfer(RetroLinkForm form, const char *message)
{
    ReleaseOutboundTransfer();
    MacStudioSetControlValue(form, "TransferProgressBar", 0);
    SetText(form, "ActionStatus", message);
}

static short AppendMacCopyPath(
    unsigned char *payload,
    unsigned short *offset,
    short volumeIndex,
    short pathCount,
    const unsigned char pathComponents[][32])
{
    short index;
    unsigned short pathLength = 5;
    unsigned char utf8Name[RETROLINK_MAC_MAX_UTF8_NAME_BYTES];
    unsigned char utf8Length;

    if (volumeIndex < 0 || volumeIndex >= gMacVolumeCount ||
        pathCount < 0 || pathCount > RETROLINK_MAC_MAX_PATH_COMPONENTS)
        return 0;
    for (index = 0; index < pathCount; index++)
    {
        if (pathComponents[index][0] == 0 ||
            !MacRomanToUtf8(
                pathComponents[index],
                utf8Name,
                sizeof(utf8Name),
                &utf8Length))
            return 0;
        pathLength = (unsigned short)(pathLength + 1 + utf8Length);
    }
    if (pathLength > RETROLINK_MAC_MAX_PATH_BYTES ||
        *offset + pathLength > RETROLINK_MAX_PAYLOAD)
        return 0;

    WriteBigEndian32(payload + *offset, gMacVolumes[volumeIndex].providerId);
    *offset = (unsigned short)(*offset + 4);
    payload[(*offset)++] = (unsigned char)pathCount;
    for (index = 0; index < pathCount; index++)
    {
        if (!MacRomanToUtf8(
                pathComponents[index],
                utf8Name,
                sizeof(utf8Name),
                &utf8Length))
            return 0;
        payload[(*offset)++] = utf8Length;
        memcpy(payload + *offset, utf8Name, utf8Length);
        *offset = (unsigned short)(*offset + utf8Length);
    }
    return 1;
}

static short AppendHostCopyPath(
    unsigned char *payload,
    unsigned short *offset)
{
    short index;
    unsigned short pathLength = 5;

    if (gHostPathCount < 0 || gHostPathCount > RETROLINK_MAC_MAX_PATH_COMPONENTS)
        return 0;
    for (index = 0; index < gHostPathCount; index++)
    {
        unsigned char length = gHostPathLengths[index];
        if (length == 0 ||
            !IsValidUtf8Name(gHostPathComponents[index], length, 0))
            return 0;
        pathLength = (unsigned short)(pathLength + 1 + length);
    }
    if (pathLength > RETROLINK_MAC_MAX_PATH_BYTES ||
        *offset + pathLength > RETROLINK_MAX_PAYLOAD)
        return 0;

    WriteBigEndian32(payload + *offset, RETROLINK_WINDOWS_SHARE_PROVIDER);
    *offset = (unsigned short)(*offset + 4);
    payload[(*offset)++] = (unsigned char)gHostPathCount;
    for (index = 0; index < gHostPathCount; index++)
    {
        unsigned char length = gHostPathLengths[index];
        payload[(*offset)++] = length;
        memcpy(payload + *offset, gHostPathComponents[index], length);
        *offset = (unsigned short)(*offset + length);
    }
    return 1;
}

static void HandleTransferReady(
    RetroLinkForm form,
    const unsigned char *payload,
    unsigned short payloadLength,
    unsigned long requestId)
{
    unsigned short resultCode;
    OSErr error;

    if (!gOutboundTransferPending ||
        requestId != gOutboundTransferRequestId)
        return;
    if (payloadLength != 17)
    {
        FailOutboundTransfer(form, "The REMOTE transfer response was malformed.");
        return;
    }
    resultCode = ReadBigEndian16(payload);
    if (resultCode != 0 ||
        ReadBigEndian32(payload + 2) != 0 ||
        ReadBigEndian32(payload + 6) != gOutboundTransferLength ||
        ReadBigEndian32(payload + 10) != gOutboundTransferCrc ||
        payload[14] != 1 ||
        ReadBigEndian16(payload + 15) != 128)
    {
        FailOutboundTransfer(form, "The REMOTE peer rejected or mismatched the file transfer.");
        return;
    }

    gOutboundTransferReady = true;
    SetText(form, "ActionStatus", "REMOTE is ready; sending the LOCAL file using XMODEM-CRC...");
    if (gOutboundTransferLength != 0)
    {
        error = MacStudioProtocolSend(
            "ProtocolTransfer1",
            (const unsigned char *)gOutboundTransferBuffer,
            (long)gOutboundTransferLength);
        if (error != noErr)
        {
            FailOutboundTransfer(form, "The Mac could not complete the XMODEM-CRC send.");
            return;
        }
    }
    if (gOutboundTransferBuffer != NULL)
        DisposePtr(gOutboundTransferBuffer);
    gOutboundTransferBuffer = NULL;
    gOutboundTransferSent = true;
    SetText(form, "ActionStatus", "Data sent; waiting for REMOTE verification.");
}

static void HandleTransferComplete(
    RetroLinkForm form,
    const unsigned char *payload,
    unsigned short payloadLength,
    unsigned long requestId)
{
    unsigned short resultCode;

    if (!gOutboundTransferPending ||
        requestId != gOutboundTransferRequestId)
        return;
    if (payloadLength != 14)
    {
        FailOutboundTransfer(form, "The REMOTE completion response was malformed.");
        return;
    }
    resultCode = ReadBigEndian16(payload);
    if (resultCode != 0 ||
        ReadBigEndian32(payload + 2) != 0 ||
        ReadBigEndian32(payload + 6) != gOutboundTransferLength ||
        ReadBigEndian32(payload + 10) != gOutboundTransferCrc)
    {
        FailOutboundTransfer(form, "REMOTE could not verify or save the transferred file.");
        return;
    }

    ReleaseOutboundTransfer();
    MacStudioSetControlValue(form, "TransferProgressBar", 100);
    if (gConnected &&
        memcmp(gActiveEndpointId, gLocalEndpointId, 16) == 0)
        RequestHostList(form, 0);
    SetText(form, "ActionStatus", "REMOTE verified and saved the LOCAL file.");
}

static void WaitForOutboundTransfer(RetroLinkForm form)
{
    unsigned long phaseStart = TickCount();
    long finalTicks = 0;
    short completionPhase = 0;

    while (gOutboundTransferPending)
    {
        SerialDataAvailable(form, 0);
        if (!gOutboundTransferPending)
            break;
        if (gOutboundTransferSent && !completionPhase)
        {
            completionPhase = 1;
            phaseStart = TickCount();
        }
        if (TickCount() - phaseStart >= 600)
        {
            FailOutboundTransfer(
                form,
                completionPhase
                    ? "Timeout: REMOTE did not confirm the transferred file in 10 seconds."
                    : "Timeout: REMOTE did not prepare the transfer in 10 seconds.");
            break;
        }
        Delay(1, &finalTicks);
    }
}

void CopyToDosClick(RetroLinkForm form, short item)
{
    unsigned char payload[RETROLINK_MAX_PAYLOAD];
    unsigned short offset = 36;
    unsigned char utf8NameLength;
    unsigned long fileLength;
    unsigned long crc32;
    long directoryId;
    unsigned long requestId;
    long readCount;
    long allocationLength;
    short row = -1;
    short volumeIndex = gMacUiVolumeIndex;
    short fileRefNum = 0;
    short result;
    unsigned char kind;
    unsigned char entryIndex;
    CInfoPBRec catalogInfo;
    unsigned char entryName[32];
    FSSpec fileSpec;
    Ptr transferBuffer = NULL;
    const RetroLinkMacEntry *entry;
    OSErr error;

    (void)item;
    if (!gConnected ||
        memcmp(gActiveEndpointId, gLocalEndpointId, 16) != 0)
    {
        SetText(form, "ActionStatus", "Take Control before copying a LOCAL file to REMOTE.");
        return;
    }
    if (gOutboundTransferPending)
    {
        SetText(form, "ActionStatus", "A LOCAL-to-REMOTE transfer is already in progress.");
        return;
    }
    if (volumeIndex < 0 || volumeIndex >= gMacVolumeCount)
    {
        SetText(form, "ActionStatus", "Open a LOCAL volume and select a file to copy.");
        return;
    }
    result = MacStudioGetListBoxSelection(form, "MacFiles", &row);
    if (result != noErr || row < 0 || row >= RETROLINK_MAX_LIST_ROWS)
    {
        SetText(form, "ActionStatus", "Select a LOCAL file before copying to REMOTE.");
        return;
    }
    kind = gMacUiVisibleKinds[row];
    entryIndex = gMacUiVisibleEntries[row];
    if (kind != 0 || entryIndex >= gMacUiPage.entryCount)
    {
        SetText(form, "ActionStatus", "Only a selected LOCAL file can be copied to REMOTE.");
        return;
    }
    entry = &gMacUiPage.entries[entryIndex];
    utf8NameLength = entry->utf8NameLength;
    if (!IsDos83FileName(entry->utf8Name, utf8NameLength))
    {
        SetText(form, "ActionStatus", "The REMOTE destination requires an 8.3 ASCII file name.");
        return;
    }
    if (entry->dataForkSize > RETROLINK_TRANSFER_MAX_BYTES)
    {
        SetText(form, "ActionStatus", "The LOCAL file exceeds the 512 KiB REMOTE transfer limit.");
        return;
    }
    error = ResolveMacDirectory(
        volumeIndex,
        gMacUiPathCount,
        gMacUiPathComponents,
        &directoryId);
    if (error != noErr)
    {
        SetText(form, "ActionStatus", "The selected LOCAL folder is unavailable.");
        return;
    }
    result = GetMacCatalogEntry(
        volumeIndex,
        directoryId,
        gMacUiPage.cursor + entryIndex + 1,
        &catalogInfo,
        entryName);
    if (result != noErr ||
        (catalogInfo.hFileInfo.ioFlAttrib & RETROLINK_MAC_DIRECTORY_FLAG) != 0)
    {
        SetText(form, "ActionStatus", "The selected LOCAL file is no longer available.");
        return;
    }

    fileLength = (unsigned long)catalogInfo.hFileInfo.ioFlLgLen;
    if (fileLength > RETROLINK_TRANSFER_MAX_BYTES ||
        fileLength != entry->dataForkSize)
    {
        SetText(form, "ActionStatus", "The LOCAL file changed or exceeds the REMOTE transfer limit.");
        return;
    }
    if (fileLength != 0)
    {
        allocationLength = (long)fileLength;
        transferBuffer = NewPtr(allocationLength);
        if (transferBuffer == NULL || MemError() != noErr)
        {
            SetText(form, "ActionStatus", "Not enough LOCAL memory to prepare the transfer.");
            return;
        }
    }

    memset(&fileSpec, 0, sizeof(fileSpec));
    fileSpec.vRefNum = gMacVolumes[volumeIndex].volumeRefNum;
    fileSpec.parID = (long)directoryId;
    memcpy(fileSpec.name, entryName, entryName[0] + 1);
    error = FSpOpenDF(&fileSpec, fsRdPerm, &fileRefNum);
    if (error == noErr && fileLength != 0)
    {
        readCount = (long)fileLength;
        error = FSRead(fileRefNum, &readCount, (Ptr)transferBuffer);
        if (error == noErr && readCount != (long)fileLength)
            error = ioErr;
    }
    if (fileRefNum != 0)
    {
        OSErr closeError = FSClose(fileRefNum);
        if (error == noErr)
            error = closeError;
        fileRefNum = 0;
    }
    if (error != noErr)
    {
        if (transferBuffer != NULL)
            DisposePtr(transferBuffer);
        SetText(form, "ActionStatus", "Could not read the selected LOCAL data fork.");
        return;
    }

    crc32 = CalculateCrc32((const unsigned char *)transferBuffer, (long)fileLength);
    WriteBigEndian32(payload, gRoleEpoch);
    memcpy(payload + 4, gLocalEndpointId, 16);
    memcpy(payload + 20, gLocalEndpointId, 16);
    if (!AppendMacCopyPath(
            payload,
            &offset,
            volumeIndex,
            gMacUiPathCount,
            gMacUiPathComponents))
    {
        if (transferBuffer != NULL)
            DisposePtr(transferBuffer);
        SetText(form, "ActionStatus", "The selected LOCAL path cannot be represented in RetroLink.");
        return;
    }
    memcpy(payload + offset, gHostEndpointId, 16);
    offset = (unsigned short)(offset + 16);
    if (!AppendHostCopyPath(payload, &offset) ||
        offset + 1 + utf8NameLength + 15 > RETROLINK_MAX_PAYLOAD)
    {
        if (transferBuffer != NULL)
            DisposePtr(transferBuffer);
        SetText(form, "ActionStatus", "The REMOTE destination path is too long for RetroLink.");
        return;
    }
    payload[offset++] = utf8NameLength;
    memcpy(payload + offset, entry->utf8Name, utf8NameLength);
    offset = (unsigned short)(offset + utf8NameLength);
    WriteBigEndian64(payload + offset, fileLength);
    WriteBigEndian32(payload + offset + 8, crc32);
    payload[offset + 12] = 0;
    payload[offset + 13] = 0;
    payload[offset + 14] = 1;
    offset = (unsigned short)(offset + 15);

    requestId = gNextRequestId++;
    if (requestId == 0)
        requestId = gNextRequestId++;
    if (requestId == 0)
        requestId = 1;
    gOutboundTransferPending = true;
    gOutboundTransferReady = false;
    gOutboundTransferSent = false;
    gOutboundTransferRequestId = requestId;
    gOutboundTransferLength = fileLength;
    gOutboundTransferCrc = crc32;
    gOutboundTransferBuffer = transferBuffer;
    MacStudioSetControlValue(form, "TransferProgressBar", 0);
    if (SendFrame(RETROLINK_COPY_PREPARE, requestId, payload, offset) != noErr)
    {
        FailOutboundTransfer(form, "Could not send COPY_PREPARE to REMOTE.");
        return;
    }
    SetText(form, "ActionStatus", "Waiting for REMOTE to prepare the destination...");
    WaitForOutboundTransfer(form);
}

static void HandleRoleState(RetroLinkForm form, const unsigned char *payload, unsigned short length)
{
    unsigned long roleEpoch;

    if (length != 21)
        return;

    roleEpoch = ReadBigEndian32(payload);
    if (roleEpoch < gRoleEpoch)
        return;
    gRoleEpoch = roleEpoch;
    memcpy(gActiveEndpointId, payload + 4, 16);
    UpdateRoleText(form);
    SetText(form, "ActionStatus", "Active endpoint changed.");
    if (gPendingHostList &&
        memcmp(gActiveEndpointId, gLocalEndpointId, 16) == 0)
    {
        gPendingHostList = false;
        gHostPathCount = 0;
        RequestHostList(form, 0);
    }
    if (gPendingMacFilesClick &&
        memcmp(gActiveEndpointId, gLocalEndpointId, 16) == 0)
        HandleMacFilesClick(form);
}

static void HandleHostOperationResult(
    RetroLinkForm form,
    const unsigned char *payload,
    unsigned short payloadLength,
    unsigned long requestId)
{
    unsigned short resultCode;
    unsigned char operationKind;
    unsigned char nameLength;

    if (!gPendingHostOperation ||
        requestId != gPendingHostOperationRequestId)
        return;
    gPendingHostOperation = false;
    if (payloadLength < 4)
    {
        SetText(form, "ActionStatus", "The REMOTE operation result was truncated.");
        return;
    }

    resultCode = ReadBigEndian16(payload);
    operationKind = payload[2];
    nameLength = payload[3];
    if (payloadLength != 4 + nameLength ||
        operationKind != gPendingHostOperationKind ||
        (nameLength != 0 &&
         !IsValidUtf8Name(payload + 4, nameLength, 0)))
    {
        SetText(form, "ActionStatus", "The REMOTE operation result was invalid.");
        return;
    }
    if (resultCode != 0)
    {
        SetText(form, "ActionStatus", "The REMOTE file operation failed.");
        return;
    }

    SetText(form, "ActionStatus", "REMOTE file operation completed.");
    RequestHostList(form, 0);
}

static void HandleFrame(
    RetroLinkForm form,
    const unsigned char *frame,
    unsigned short payloadLength)
{
    unsigned char messageType = frame[5];
    const unsigned char *payload = frame + RETROLINK_HEADER_SIZE;

    if (messageType == 0x01)
    {
        if (payloadLength != 29 ||
            payload[0] > 1 ||
            payload[1] < 1 ||
            payload[0] > payload[1])
        {
            SetText(form, "ConnectionStatus", "Unsupported or malformed HELLO request.");
            return;
        }

        memcpy(gSessionId, frame + 8, 16);
        SendHelloAck(form, payload);
    }
    else if (!gConnected || memcmp(frame + 8, gSessionId, 16) != 0)
        return;
    else if (messageType == 0x04)
        HandleRoleState(form, payload, payloadLength);
    else if (messageType == 0x10)
        HandleMacListRequest(
            form,
            payload,
            payloadLength,
            ReadBigEndian32(frame + 24));
    else if (messageType == 0x11)
    {
        if (memcmp(gActiveEndpointId, gLocalEndpointId, 16) == 0)
            HandleHostListPage(
                form,
                payload,
                payloadLength,
                ReadBigEndian32(frame + 24));
    }
    else if (messageType == 0x15)
        HandleHostOperationResult(
            form,
            payload,
            payloadLength,
            ReadBigEndian32(frame + 24));
    else if (messageType == 0x12 ||
             messageType == 0x13 ||
             messageType == 0x14)
        HandleFileOperationRequest(
            form,
            payload,
            payloadLength,
            ReadBigEndian32(frame + 24),
            messageType);
    else if (messageType == RETROLINK_COPY_PREPARE)
        HandleCopyPrepare(
            form,
            payload,
            payloadLength,
            ReadBigEndian32(frame + 24));
    else if (messageType == RETROLINK_TRANSFER_READY)
        HandleTransferReady(
            form,
            payload,
            payloadLength,
            ReadBigEndian32(frame + 24));
    else if (messageType == RETROLINK_TRANSFER_COMPLETE)
        HandleTransferComplete(
            form,
            payload,
            payloadLength,
            ReadBigEndian32(frame + 24));
    else if (messageType == 0x7F)
    {
        if (gOutboundTransferPending &&
            ReadBigEndian32(frame + 24) == gOutboundTransferRequestId)
            FailOutboundTransfer(form, "The REMOTE peer rejected the file transfer.");
        else
            SetText(form, "ActionStatus", "The remote RetroLink peer reported a protocol error.");
    }
}

static void DropFramePrefix(long count)
{
    if (count <= 0)
        return;
    gFrameLength -= count;
    if (gFrameLength > 0)
        memmove(gFrameBuffer, gFrameBuffer + count, (size_t)gFrameLength);
}

static void ProcessFrames(RetroLinkForm form)
{
    long magicOffset;
    long index;
    long expectedLength;
    unsigned short payloadLength;
    unsigned short expectedCrc;

    while (gFrameLength >= 4)
    {
        magicOffset = -1;
        for (index = 0; index <= gFrameLength - 4; index++)
        {
            if (gFrameBuffer[index] == 'R' &&
                gFrameBuffer[index + 1] == 'L' &&
                gFrameBuffer[index + 2] == 'N' &&
                gFrameBuffer[index + 3] == 'K')
            {
                magicOffset = index;
                break;
            }
        }

        if (magicOffset < 0)
        {
            if (gFrameLength > 3)
                DropFramePrefix(gFrameLength - 3);
            return;
        }
        if (magicOffset > 0)
            DropFramePrefix(magicOffset);
        if (gFrameLength < RETROLINK_HEADER_SIZE)
            return;

        payloadLength = ReadBigEndian16(gFrameBuffer + 28);
        if (gFrameBuffer[4] != 1 ||
            gFrameBuffer[6] != 0 ||
            gFrameBuffer[7] != 0 ||
            payloadLength > RETROLINK_MAX_PAYLOAD)
        {
            DropFramePrefix(1);
            continue;
        }

        expectedLength = RETROLINK_HEADER_SIZE + payloadLength + 2;
        if (gFrameLength < expectedLength)
            return;

        expectedCrc = ReadBigEndian16(gFrameBuffer + expectedLength - 2);
        if (expectedCrc != CalculateCrc16(gFrameBuffer, expectedLength - 2))
        {
            DropFramePrefix(1);
            continue;
        }

        HandleFrame(form, gFrameBuffer, payloadLength);
        DropFramePrefix(expectedLength);
    }
}

static void FeedSerialByte(RetroLinkForm form, unsigned char value)
{
    if (gFrameLength >= RETROLINK_MAX_FRAME)
        DropFramePrefix(1);
    gFrameBuffer[gFrameLength++] = value;
    ProcessFrames(form);
}

void RetroLinkInitialize(RetroLinkForm form)
{
    short result;

    InitializeEndpointId();
    MacStudioSetControlValue(form, "SerialPortSelector", 1);
    MacStudioSetControlValue(form, "SerialBaudSelector", 8);
    result = EnumerateMacVolumes();
    if (result == noErr)
        RefreshMacFilesList(form, 0);
    else
    {
        SetMacListMessage(form, "Volume scan failed");
        SetText(form, "ActionStatus", "Could not enumerate mounted LOCAL volumes.");
    }
    result = MacStudioSerialOpenConfigured("SerialPort1", 2, 9600);
    if (result == noErr)
    {
        gSerialPortOpen = true;
        SetText(form, "SerialCloseButton", "Close");
        SetText(form, "ConnectionStatus", "Serial B ready at 9600 baud. Waiting for REMOTE...");
    }
    else
    {
        gSerialPortOpen = false;
        SetText(form, "SerialCloseButton", "Open");
        SetText(form, "ConnectionStatus", "Could not open Serial B. Check the port settings.");
    }
    UpdateRoleText(form);
}

void SerialSettingsClick(RetroLinkForm form, short item)
{
    static const long baudRates[] = {
        300, 600, 1200, 1800, 2400, 3600, 4800,
        7200, 9600, 14400, 19200, 28800, 38400, 57600
    };
    short portSelection = 0;
    short baudSelection = 8;
    short result;
    (void)item;

    if (MacStudioGetControlValue(form, "SerialPortSelector", &portSelection) != noErr ||
        MacStudioGetControlValue(form, "SerialBaudSelector", &baudSelection) != noErr ||
        portSelection < 0 || portSelection > 1 ||
        baudSelection < 0 || baudSelection >= 14)
    {
        SetText(form, "ActionStatus", "Select a valid serial port and baud rate.");
        return;
    }

    gConnected = false;
    gFrameLength = 0;
    result = MacStudioSerialOpenConfigured(
        "SerialPort1",
        (short)(portSelection + 1),
        baudRates[baudSelection]);
    if (result == noErr)
    {
        gSerialPortOpen = true;
        SetText(form, "SerialCloseButton", "Close");
        SetText(form, "ConnectionStatus", "Serial settings applied. Reconnect REMOTE RetroLink.");
        SetText(form, "ActionStatus", "Waiting for a new REMOTE session.");
    }
    else
    {
        gSerialPortOpen = false;
        SetText(form, "SerialCloseButton", "Open");
        SetText(form, "ConnectionStatus", "Could not apply serial settings. Check the selected port and rate.");
        SetText(form, "ActionStatus", "The serial driver rejected the selected configuration.");
    }
    UpdateRoleText(form);
}

void SerialCloseClick(RetroLinkForm form, short item)
{
    short result;
    (void)item;
    if (!gSerialPortOpen)
    {
        SerialSettingsClick(form, item);
        return;
    }
    if (gOutboundTransferPending)
        FailOutboundTransfer(form, "The serial connection closed during file transfer.");
    gConnected = false;
    gFrameLength = 0;
    result = MacStudioSerialClose("SerialPort1");
    gSerialPortOpen = false;
    SetText(form, "SerialCloseButton", "Open");
    if (result == noErr)
    {
        SetText(form, "ConnectionStatus", "Serial port closed.");
        SetText(form, "ActionStatus", "Select a port and baud, then choose Open.");
    }
    else
    {
        SetText(form, "ConnectionStatus", "Could not close the serial port.");
        SetText(form, "ActionStatus", "Serial driver error while closing.");
    }
    UpdateRoleText(form);
}

void RefreshMacClick(RetroLinkForm form, short item)
{
    short result;
    (void)item;
    result = EnumerateMacVolumes();
    if (result == noErr)
    {
        gMacUiVolumeIndex = -1;
        gMacUiPathCount = 0;
        RefreshMacFilesList(form, 0);
        SetText(form, "ActionStatus", "LOCAL volumes refreshed.");
    }
    else
    {
        SetMacListMessage(form, "Volume scan failed");
        SetText(form, "ActionStatus", "Could not refresh LOCAL volumes.");
    }
}

void RefreshHostClick(RetroLinkForm form, short item)
{
    (void)item;
    if (!gConnected)
    {
        SetText(form, "ActionStatus", "Connect REMOTE RetroLink before refreshing remote files.");
        return;
    }
    if (memcmp(gActiveEndpointId, gLocalEndpointId, 16) != 0)
    {
        gPendingHostList = true;
        RequestMacControl(form);
        return;
    }
    gHostPathCount = 0;
    RequestHostList(form, 0);
}

void TakeControlClick(RetroLinkForm form, short item)
{
    (void)item;
    RequestMacControl(form);
}

void HostFilesClick(RetroLinkForm form, short item)
{
    (void)item;
    HandleHostFilesClick(form);
}

void SerialDataAvailable(RetroLinkForm form, short item)
{
    unsigned char data[128];
    char errorMessage[80];
    long readCount = 0;
    long readCapacity = sizeof(data);
    long index;
    short result;

    (void)item;
    if (gOutboundTransferPending && !gOutboundTransferReady)
        readCapacity = 1;
    result = MacStudioSerialRead(
        "SerialPort1",
        data,
        readCapacity,
        &readCount);
    if (result != noErr)
    {
        sprintf(
            errorMessage,
            "Serial read failed (error %d). Check the endpoint.",
            (int)result);
        SetText(form, "ConnectionStatus", errorMessage);
        return;
    }

    for (index = 0; index < readCount; index++)
        FeedSerialByte(form, data[index]);
    if (gPendingHostOperation &&
        (unsigned long)(TickCount() - gPendingHostOperationTick) >= 600)
    {
        gPendingHostOperation = false;
        SetText(form, "ActionStatus", "Timeout: REMOTE did not complete the file operation in 10 seconds.");
    }
}

void SerialError(RetroLinkForm form, short item)
{
    char errorMessage[80];
    short error;
    (void)item;
    error = MacStudioGetLastSerialError("SerialPort1");
    sprintf(
        errorMessage,
        "Serial I/O error (%d). Check endpoint and baud rate.",
        (int)error);
    SetText(form, "ConnectionStatus", errorMessage);
}

void MacStudioUserControlClicked(
    RetroLinkForm form,
    const char *controlId,
    short item)
{
    (void)item;
    if (strcmp(controlId, "HostFiles") == 0)
        HandleHostFilesClick(form);
    else if (strcmp(controlId, "MacFiles") == 0)
        HandleMacFilesClick(form);
    else
    {
        (void)form;
        (void)controlId;
    }
}
