#include <conio.h>
#include <direct.h>
#include <dos.h>
#include <errno.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "agent_link.h"
#include "dos_serial.h"
#include "retro_protocol.h"

#define RL_HELLO 0x01
#define RL_HELLO_ACK 0x02
#define RL_ROLE_REQUEST 0x03
#define RL_ROLE_STATE 0x04
#define RL_LIST_REQUEST 0x10
#define RL_LIST_PAGE 0x11
#define RL_MAKE_DIRECTORY 0x12
#define RL_RENAME 0x13
#define RL_DELETE 0x14
#define RL_OPERATION_RESULT 0x15
#define RL_COPY_PREPARE 0x20
#define RL_TRANSFER_READY 0x21
#define RL_TRANSFER_COMPLETE 0x22
#define RL_ERROR 0x7F
#define RL_MAX_TRANSFER_BYTES 524288UL
#define DOS_FILE_ATTRIBUTES 0x3FUL
#define DOS_DIRECTORY_ATTRIBUTE 0x10
#define DOS_READ_ONLY_ATTRIBUTE 0x01
#define DOS_HIDDEN_ATTRIBUTE 0x02
#define DOS_SYSTEM_ATTRIBUTE 0x04
#define DOS_ARCHIVE_ATTRIBUTE 0x20
#define XMODEM_SOH 0x01
#define XMODEM_EOT 0x04
#define XMODEM_ACK 0x06
#define XMODEM_NAK 0x15
#define XMODEM_CRC_REQUEST 0x43
#define XMODEM_BLOCK_BYTES 128
#define UI_VISIBLE_ROWS 16

typedef struct
{
    RlDirectoryEntry entry;
    rl_u8 has_parent;
} LocalEntry;

static DosUart gDebugUart;
static DosUart gLinkUart;
static AgentLink gAgent;
static RlFrameDecoder gDecoder;
static RlFrame gFrame;
static RlListPage gDecodedPage;
static RlPeerInfo gPeer;
static RlDirectoryEntry gLocalEntries[RL_MAX_LIST_ENTRIES];
static RlDirectoryEntry gRemoteEntries[RL_MAX_LIST_ENTRIES + 1];
static rl_u8 gLocalMarked[RL_MAX_LIST_ENTRIES];
static rl_u8 gEndpointId[RL_SESSION_ID_SIZE];
static rl_u8 gSessionId[RL_SESSION_ID_SIZE];
static rl_u8 gActiveEndpointId[RL_SESSION_ID_SIZE];
static rl_u32 gRoleEpoch;
static rl_u32 gNextRequestId;
static rl_u32 gPendingListRequest;
static rl_u32 gPendingTransferRequest;
static rl_u32 gPendingOperationRequest;
static rl_u8 gPendingOperationKind;
static rl_u8 gOperationReady;
static rl_u16 gOperationResultCode;
static rl_u8 gPeerReady;
static rl_u8 gHelloAckSeen;
static rl_u8 gHelloAckSessionMismatch;
static rl_u8 gHelloAckInvalid;
static rl_u8 gLastFrameType;
static rl_u8 gRoleChanged;
static rl_u8 gListReady;
static rl_u8 gTransferReady;
static rl_u8 gTransferComplete;
static rl_u8 gActivePanel;
static rl_u8 gLocalDepth;
static rl_u8 gRemoteDepth;
static rl_u8 gLocalCount;
static rl_u8 gRemoteCount;
static rl_u8 gLocalSelection;
static rl_u8 gRemoteSelection;
static rl_u8 gRemoteRootIndex;
static rl_u8 gRemoteAtVolumes;
static rl_u8 gConnected;
static rl_u8 gTransferActive;
static rl_u8 gProgressActive;
static rl_u8 gProgressPercent;
static rl_u32 gBaudRate = 9600;
static unsigned long gReceivedByteCount;
static unsigned long gDecodedFrameCount;
static char gLocalComponents[RL_MAX_PATH_COMPONENTS][RL_MAX_NAME_BYTES + 1];
static char gRemoteComponents[RL_MAX_PATH_COMPONENTS][RL_MAX_NAME_BYTES + 1];
static char gStatus[76];
static rl_u8 gPayload[RL_MAX_PAYLOAD];
static rl_u8 gEncoded[RL_FRAME_BUFFER_SIZE];
static rl_u8 gXmodemPacket[XMODEM_BLOCK_BYTES + 5];
static char gWorkingPath[160];
static char gSavedPath[160];

static void handle_incoming_copy_prepare(const RlFrame *frame);
static void go_parent(void);
static void reload_local_entries(void);
static void draw_ui(void);

static void prepend_remote_parent(void)
{
    unsigned int index;

    for (index = gRemoteCount; index > 0; --index)
        gRemoteEntries[index] = gRemoteEntries[index - 1];
    memset(&gRemoteEntries[0], 0, sizeof(gRemoteEntries[0]));
    strcpy(gRemoteEntries[0].name, "..");
    gRemoteEntries[0].kind = 1;
    gRemoteEntries[0].attributes = DOS_DIRECTORY_ATTRIBUTE;
    ++gRemoteCount;
}

static int is_remote_parent(const RlDirectoryEntry *entry)
{
    return entry != 0 && entry->kind == 1 &&
        strcmp(entry->name, "..") == 0;
}

static void console_gotoxy(int column, int row)
{
    union REGS registers;

    memset(&registers, 0, sizeof(registers));
    registers.h.ah = 0x02;
    registers.h.bh = 0;
    registers.h.dh = (unsigned char)(row - 1);
    registers.h.dl = (unsigned char)(column - 1);
    int86(0x10, &registers, &registers);
}

static void console_clear(void)
{
    union REGS registers;

    memset(&registers, 0, sizeof(registers));
    registers.h.ah = 0x06;
    registers.h.al = 0;
    registers.h.bh = 0x07;
    registers.h.ch = 0;
    registers.h.cl = 0;
    registers.h.dh = 24;
    registers.h.dl = 79;
    int86(0x10, &registers, &registers);
    console_gotoxy(1, 1);
}

static rl_u16 read_u16(const rl_u8 *value)
{
    return (rl_u16)(((rl_u16)value[0] << 8) | value[1]);
}

static rl_u32 read_u32(const rl_u8 *value)
{
    return ((rl_u32)value[0] << 24) |
        ((rl_u32)value[1] << 16) |
        ((rl_u32)value[2] << 8) |
        (rl_u32)value[3];
}

static void write_u16(rl_u8 *destination, rl_u16 value)
{
    destination[0] = (rl_u8)(value >> 8);
    destination[1] = (rl_u8)value;
}

static void write_u32(rl_u8 *destination, rl_u32 value)
{
    destination[0] = (rl_u8)(value >> 24);
    destination[1] = (rl_u8)(value >> 16);
    destination[2] = (rl_u8)(value >> 8);
    destination[3] = (rl_u8)value;
}

static rl_u32 elapsed_ticks(rl_u32 start, rl_u32 current)
{
    if (current >= start)
        return current - start;
    return 0x001800B0UL - start + current;
}

static void set_status(const char *text)
{
    unsigned int length;
    if (text == 0)
        return;
    length = (unsigned int)strlen(text);
    if (length >= sizeof(gStatus))
        length = sizeof(gStatus) - 1;
    memcpy(gStatus, text, length);
    gStatus[length] = '\0';
    console_gotoxy(1, 25);
    cprintf("%-79.79s", gStatus);
}

static int safe_path_component(const char *component)
{
    unsigned int index;
    unsigned int length;
    if (component == 0)
        return 0;
    length = (unsigned int)strlen(component);
    if (length == 0 || length > RL_MAX_NAME_BYTES ||
        strcmp(component, ".") == 0 ||
        strcmp(component, "..") == 0)
    {
        return 0;
    }
    for (index = 0; index < length; ++index)
    {
        unsigned char value = (unsigned char)component[index];
        if (value < 0x20 || value > 0x7E ||
            value == ':' || value == '/' || value == '\\')
        {
            return 0;
        }
    }
    return 1;
}
static void set_progress(rl_u8 percent)
{
    char bar[51];
    unsigned int filled;
    unsigned int index;

    if (percent > 100)
        percent = 100;
    gProgressActive = 1;
    gProgressPercent = percent;
    filled = percent / 2;
    for (index = 0; index < 50; ++index)
        bar[index] = index < filled ? '#' : '-';
    bar[50] = '\0';
    console_gotoxy(1, 23);
    cprintf("Transfer [%s] %3u%%", bar, percent);
}

static int send_link_frame(
    rl_u8 message_type,
    rl_u32 request_id,
    const rl_u8 *payload,
    rl_u16 payload_length)
{
    rl_u16 length;
    rl_u16 index;

    memset(&gFrame, 0, sizeof(gFrame));
    gFrame.message_type = message_type;
    memcpy(gFrame.session_id, gSessionId, RL_SESSION_ID_SIZE);
    gFrame.request_id = request_id;
    gFrame.payload_length = payload_length;
    if (payload_length != 0)
        memcpy(gFrame.payload, payload, payload_length);
    length = rl_encode_frame(&gFrame, gEncoded);
    if (length == 0)
        return 0;
    for (index = 0; index < length; ++index)
    {
        if (!dos_uart_write(&gLinkUart, gEncoded[index], 10))
            return 0;
    }
    return 1;
}

static void make_random_ids(void)
{
    rl_u8 index;
    unsigned int seed;
    seed = (unsigned int)dos_clock_ticks() ^ (unsigned int)time(0);
    srand(seed);
    for (index = 0; index < RL_SESSION_ID_SIZE; ++index)
    {
        gEndpointId[index] = (rl_u8)rand();
        gSessionId[index] = (rl_u8)rand();
    }
    gEndpointId[0] |= 0x80;
    gSessionId[0] |= 0x40;
}

static void send_role_state(void)
{
    write_u32(gPayload, gRoleEpoch);
    memcpy(gPayload + 4, gActiveEndpointId, RL_SESSION_ID_SIZE);
    gPayload[20] = 0;
    send_link_frame(RL_ROLE_STATE, 0, gPayload, 21);
}

static int build_dos_path(
    char *destination,
    unsigned int capacity,
    const char components[RL_MAX_PATH_COMPONENTS][RL_MAX_NAME_BYTES + 1],
    rl_u8 component_count)
{
    rl_u8 index;
    unsigned int length;

    if (destination == 0 || capacity < 4 ||
        component_count > RL_MAX_PATH_COMPONENTS)
    {
        return 0;
    }
    strcpy(destination, "C:\\");
    length = 3;
    for (index = 0; index < component_count; ++index)
    {
        unsigned int component_length;
        if (!safe_path_component(components[index]))
            return 0;
        component_length = (unsigned int)strlen(components[index]);
        if (length + component_length + 2 >= capacity)
            return 0;
        memcpy(destination + length, components[index], component_length);
        length += component_length;
        destination[length++] = '\\';
        destination[length] = '\0';
    }
    if (length > 3 && destination[length - 1] == '\\')
        destination[length - 1] = '\0';
    return 1;
}

static void fill_component_pointers(
    const char components[RL_MAX_PATH_COMPONENTS][RL_MAX_NAME_BYTES + 1],
    rl_u8 count,
    const char *pointers[RL_MAX_PATH_COMPONENTS])
{
    rl_u8 index;
    for (index = 0; index < count; ++index)
        pointers[index] = components[index];
}

static int path_component_from_payload(
    const rl_u8 *payload,
    rl_u16 length,
    rl_u16 *offset,
    char components[RL_MAX_PATH_COMPONENTS][RL_MAX_NAME_BYTES + 1],
    rl_u8 *count)
{
    rl_u8 index;
    if (*offset > length || length - *offset < 5)
        return 0;
    *count = payload[*offset + 4];
    if (*count > RL_MAX_PATH_COMPONENTS)
        return 0;
    *offset = (rl_u16)(*offset + 5);
    for (index = 0; index < *count; ++index)
    {
        rl_u8 name_length;
        if (*offset >= length)
            return 0;
        name_length = payload[(*offset)++];
        if (name_length == 0 || name_length > RL_MAX_NAME_BYTES ||
            length - *offset < name_length)
        {
            return 0;
        }
        memcpy(components[index], payload + *offset, name_length);
        components[index][name_length] = '\0';
        if (!safe_path_component(components[index]))
            return 0;
        *offset = (rl_u16)(*offset + name_length);
    }
    return 1;
}

static int safe_dos_83_name(const char *name)
{
    unsigned int length;
    unsigned int index;
    unsigned int base_length;
    unsigned int extension_length;
    int found_dot;

    if (name == 0 || !safe_path_component(name))
        return 0;
    length = (unsigned int)strlen(name);
    if (length > 12)
        return 0;
    base_length = 0;
    extension_length = 0;
    found_dot = 0;
    for (index = 0; index < length; ++index)
    {
        unsigned char value = (unsigned char)name[index];
        if (value == '.')
        {
            if (found_dot || index == 0 || index + 1 == length)
                return 0;
            found_dot = 1;
            continue;
        }
        if (!((value >= 'A' && value <= 'Z') ||
              (value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') ||
              value == '_' || value == '-' || value == '$' || value == '~'))
        {
            return 0;
        }
        if (found_dot)
            ++extension_length;
        else
            ++base_length;
    }
    return base_length >= 1 && base_length <= 8 && extension_length <= 3;
}

static int read_operation_name(
    const rl_u8 *payload,
    rl_u16 length,
    rl_u16 *offset,
    char name[RL_MAX_NAME_BYTES + 1])
{
    rl_u8 name_length;

    if (*offset >= length)
        return 0;
    name_length = payload[(*offset)++];
    if (name_length == 0 || name_length > 12 ||
        length - *offset < name_length)
    {
        return 0;
    }
    memcpy(name, payload + *offset, name_length);
    name[name_length] = '\0';
    *offset = (rl_u16)(*offset + name_length);
    return safe_dos_83_name(name);
}

static int append_dos_path(
    char *path,
    unsigned int capacity,
    const char *name)
{
    unsigned int path_length;
    unsigned int name_length;
    if (path == 0 || name == 0)
        return 0;
    path_length = (unsigned int)strlen(path);
    name_length = (unsigned int)strlen(name);
    if (path_length + name_length + 2 >= capacity)
        return 0;
    if (path_length != 0 && path[path_length - 1] != '\\')
        path[path_length++] = '\\';
    memcpy(path + path_length, name, name_length);
    path[path_length + name_length] = '\0';
    return 1;
}

static int get_dos_attributes(const char *path, unsigned int *attributes)
{
    struct find_t file_info;
    if (path == 0 || attributes == 0 ||
        _dos_findfirst(path, DOS_FILE_ATTRIBUTES, &file_info) != 0)
    {
        return 0;
    }
    *attributes = file_info.attrib;
    return 1;
}

static int is_dos_directory_empty(const char *path)
{
    struct find_t file_info;
    unsigned short find_result;
    char pattern[168];

    if (strlen(path) + 6 >= sizeof(pattern))
        return 0;
    strcpy(pattern, path);
    if (pattern[strlen(pattern) - 1] != '\\')
        strcat(pattern, "\\");
    strcat(pattern, "*.*");
    find_result = _dos_findfirst(pattern, DOS_FILE_ATTRIBUTES, &file_info);
    while (find_result == 0)
    {
        if (strcmp(file_info.name, ".") != 0 &&
            strcmp(file_info.name, "..") != 0)
        {
            return 0;
        }
        find_result = _dos_findnext(&file_info);
    }
    return 1;
}

static void send_operation_result(
    rl_u32 request_id,
    rl_u8 operation_kind,
    rl_u16 result_code,
    const char *canonical_name)
{
    rl_u8 name_length;
    name_length = canonical_name == 0
        ? 0 : (rl_u8)strlen(canonical_name);
    if (name_length > 12)
    {
        result_code = 8;
        name_length = 0;
    }
    write_u16(gPayload, result_code);
    gPayload[2] = operation_kind;
    gPayload[3] = name_length;
    if (name_length != 0)
        memcpy(gPayload + 4, canonical_name, name_length);
    send_link_frame(
        RL_OPERATION_RESULT,
        request_id,
        gPayload,
        (rl_u16)(4 + name_length));
}

static void handle_operation_result(const RlFrame *frame)
{
    rl_u8 name_length;

    if (gPendingOperationRequest == 0 ||
        frame->request_id != gPendingOperationRequest)
        return;
    if (frame->payload_length < 4)
    {
        gOperationResultCode = 2;
        gOperationReady = 1;
        return;
    }
    name_length = frame->payload[3];
    if (frame->payload_length != (rl_u16)(4 + name_length) ||
        frame->payload[2] != gPendingOperationKind)
    {
        gOperationResultCode = 2;
        gOperationReady = 1;
        return;
    }
    gOperationResultCode = read_u16(frame->payload);
    gOperationReady = 1;
}

static rl_u16 dos_errno_result(int error)
{
    if (error == ENOENT)
        return 1;
    if (error == EACCES || error == EPERM)
        return 3;
    if (error == EEXIST)
        return 4;
    return 8;
}

static void handle_operation_request(const RlFrame *frame)
{
    rl_u8 operation_kind;
    rl_u32 role_epoch;
    rl_u8 active_endpoint[RL_SESSION_ID_SIZE];
    rl_u16 offset;
    rl_u32 provider_id;
    rl_u8 component_count;
    rl_u8 destination_count;
    char components[RL_MAX_PATH_COMPONENTS][RL_MAX_NAME_BYTES + 1];
    char destination_components[RL_MAX_PATH_COMPONENTS][RL_MAX_NAME_BYTES + 1];
    char parent_path[160];
    char target_path[160];
    char destination_parent[160];
    char destination_path[160];
    char source_parent_components[RL_MAX_PATH_COMPONENTS][RL_MAX_NAME_BYTES + 1];
    char source_path[160];
    char name[RL_MAX_NAME_BYTES + 1];
    const char *source_name;
    unsigned int attributes;
    rl_u16 result_code;
    rl_u8 index;
    int operation_result;

    operation_kind = frame->message_type;
    if (frame->payload_length < 20)
    {
        send_operation_result(frame->request_id, operation_kind, 2, 0);
        return;
    }
    role_epoch = read_u32(frame->payload);
    memcpy(active_endpoint, frame->payload + 4, RL_SESSION_ID_SIZE);
    if (role_epoch != gRoleEpoch ||
        memcmp(active_endpoint, gActiveEndpointId, RL_SESSION_ID_SIZE) != 0 ||
        memcmp(active_endpoint, gPeer.endpoint_id, RL_SESSION_ID_SIZE) != 0)
    {
        send_operation_result(frame->request_id, operation_kind, 3, 0);
        return;
    }

    offset = 20;
    if (frame->payload_length - offset < 4)
    {
        send_operation_result(frame->request_id, operation_kind, 2, 0);
        return;
    }
    provider_id = read_u32(frame->payload + offset);
    if (provider_id != RL_DOS_PROVIDER_ID ||
        !path_component_from_payload(
            frame->payload,
            frame->payload_length,
            &offset,
            components,
            &component_count))
    {
        send_operation_result(
            frame->request_id,
            operation_kind,
            provider_id == RL_DOS_PROVIDER_ID ? 2 : 7,
            0);
        return;
    }

    result_code = 2;
    if (operation_kind == RL_MAKE_DIRECTORY)
    {
        if (!read_operation_name(
                frame->payload,
                frame->payload_length,
                &offset,
                name) ||
            offset != frame->payload_length ||
            !build_dos_path(
                parent_path,
                sizeof(parent_path),
                components,
                component_count))
        {
            send_operation_result(frame->request_id, operation_kind, 2, 0);
            return;
        }
        strcpy(target_path, parent_path);
        if (!append_dos_path(target_path, sizeof(target_path), name))
        {
            send_operation_result(frame->request_id, operation_kind, 2, 0);
            return;
        }
        if (get_dos_attributes(target_path, &attributes))
            result_code = 4;
        else
        {
            errno = 0;
            operation_result = _mkdir(target_path);
            result_code = operation_result == 0 ? 0 : dos_errno_result(errno);
        }
        send_operation_result(
            frame->request_id,
            operation_kind,
            result_code,
            result_code == 0 ? name : 0);
    }
    else if (operation_kind == RL_RENAME)
    {
        if (!build_dos_path(parent_path, sizeof(parent_path), components, component_count) ||
            frame->payload_length - offset < 4)
        {
            send_operation_result(frame->request_id, operation_kind, 2, 0);
            return;
        }
        provider_id = read_u32(frame->payload + offset);
        if (provider_id != RL_DOS_PROVIDER_ID ||
            !path_component_from_payload(
                frame->payload,
                frame->payload_length,
                &offset,
                destination_components,
                &destination_count))
        {
            send_operation_result(
                frame->request_id,
                operation_kind,
                provider_id == RL_DOS_PROVIDER_ID ? 2 : 7,
                0);
            return;
        }
        if (component_count == 0 ||
            !read_operation_name(
                frame->payload,
                frame->payload_length,
                &offset,
                name) ||
            offset != frame->payload_length)
        {
            send_operation_result(frame->request_id, operation_kind, 2, 0);
            return;
        }
        for (index = 0; index + 1 < component_count; ++index)
            strcpy(source_parent_components[index], components[index]);
        source_name = components[component_count - 1];
        if (!build_dos_path(
                parent_path,
                sizeof(parent_path),
                source_parent_components,
                (rl_u8)(component_count - 1)))
        {
            send_operation_result(frame->request_id, operation_kind, 2, 0);
            return;
        }
        strcpy(source_path, parent_path);
        if (!append_dos_path(source_path, sizeof(source_path), source_name) ||
            !build_dos_path(
                destination_parent,
                sizeof(destination_parent),
                destination_components,
                destination_count))
        {
            send_operation_result(frame->request_id, operation_kind, 2, 0);
            return;
        }
        strcpy(destination_path, destination_parent);
        if (!append_dos_path(destination_path, sizeof(destination_path), name))
        {
            send_operation_result(frame->request_id, operation_kind, 2, 0);
            return;
        }
        if (!get_dos_attributes(source_path, &attributes))
            result_code = 1;
        else if (get_dos_attributes(destination_path, &attributes))
            result_code = 4;
        else
        {
            errno = 0;
            operation_result = rename(source_path, destination_path);
            result_code = operation_result == 0 ? 0 : dos_errno_result(errno);
        }
        send_operation_result(
            frame->request_id,
            operation_kind,
            result_code,
            result_code == 0 ? name : 0);
    }
    else if (operation_kind == RL_DELETE)
    {
        if (component_count == 0 ||
            frame->payload_length - offset != 1 ||
            frame->payload[offset] != 0)
        {
            send_operation_result(frame->request_id, operation_kind, 2, 0);
            return;
        }
        if (!build_dos_path(
                parent_path,
                sizeof(parent_path),
                components,
                (rl_u8)(component_count - 1)))
        {
            send_operation_result(frame->request_id, operation_kind, 2, 0);
            return;
        }
        strcpy(target_path, parent_path);
        source_name = components[component_count - 1];
        if (!append_dos_path(target_path, sizeof(target_path), source_name) ||
            !get_dos_attributes(target_path, &attributes))
        {
            result_code = 1;
        }
        else if ((attributes & DOS_DIRECTORY_ATTRIBUTE) != 0)
        {
            if (!is_dos_directory_empty(target_path))
                result_code = 5;
            else
            {
                errno = 0;
                operation_result = _rmdir(target_path);
                result_code = operation_result == 0 ? 0 : dos_errno_result(errno);
            }
        }
        else
        {
            errno = 0;
            operation_result = remove(target_path);
            result_code = operation_result == 0 ? 0 : dos_errno_result(errno);
        }
        send_operation_result(frame->request_id, operation_kind, result_code, 0);
    }
    else
    {
        send_operation_result(frame->request_id, operation_kind, 7, 0);
        return;
    }

    if (result_code == 0)
    {
        set_status("REMOTE completed a LOCAL file operation.");
        reload_local_entries();
    }
    else
    {
        set_status("REMOTE LOCAL file operation failed.");
    }
}

static int enumerate_directory(
    const char *path,
    rl_u32 cursor,
    rl_u8 maximum_entries,
    RlDirectoryEntry entries[RL_MAX_LIST_ENTRIES],
    rl_u8 *entry_count,
    rl_u32 *next_cursor,
    int include_parent)
{
    struct find_t file_info;
    unsigned short find_result;
    unsigned long seen;
    unsigned int attributes;
    int more_entries;
    rl_u8 count;

    if (path == 0 || entry_count == 0 || next_cursor == 0 ||
        maximum_entries == 0 || maximum_entries > RL_MAX_LIST_ENTRIES)
        return 0;
    if (getcwd(gSavedPath, sizeof(gSavedPath)) == 0)
        return 0;
    if (chdir(path) != 0)
        return 0;

    count = 0;
    seen = 0;
    more_entries = 0;
    if (include_parent && cursor == 0 && strcmp(path, "C:\\") != 0)
    {
        strcpy(entries[count].name, "..");
        entries[count].kind = 1;
        entries[count].attributes = DOS_DIRECTORY_ATTRIBUTE;
        entries[count].data_size = 0;
        entries[count].resource_size = 0;
        entries[count].modified_time = 0;
        ++count;
    }

    attributes = DOS_FILE_ATTRIBUTES;
    find_result = _dos_findfirst("*.*", attributes, &file_info);
    while (find_result == 0)
    {
        if ((file_info.attrib & 0x08) == 0 &&
            strcmp(file_info.name, ".") != 0 &&
            strcmp(file_info.name, "..") != 0 &&
            safe_path_component(file_info.name))
        {
            if (seen >= cursor)
            {
                if (count < maximum_entries)
                {
                    RlDirectoryEntry *entry = &entries[count++];
                    unsigned int name_length;
                    name_length = (unsigned int)strlen(file_info.name);
                    if (name_length > RL_MAX_NAME_BYTES)
                        name_length = RL_MAX_NAME_BYTES;
                    memcpy(entry->name, file_info.name, name_length);
                    entry->name[name_length] = '\0';
                    entry->kind = (file_info.attrib & DOS_DIRECTORY_ATTRIBUTE) != 0
                        ? 1 : 0;
                    entry->attributes = file_info.attrib;
                    entry->data_size = file_info.size;
                    entry->resource_size = 0;
                    entry->modified_time = 0;
                }
                else
                {
                    more_entries = 1;
                    break;
                }
            }
            ++seen;
        }
        find_result = _dos_findnext(&file_info);
    }
    if (more_entries)
        *next_cursor = cursor + count;
    else
        *next_cursor = RL_LIST_COMPLETE_CURSOR;
    *entry_count = count;

    if (chdir(gSavedPath) != 0)
        return 0;
    return 1;
}

static void reload_local_entries(void)
{
    rl_u32 next_cursor;
    rl_u8 count;

    memset(gLocalMarked, 0, sizeof(gLocalMarked));
    if (!build_dos_path(
            gWorkingPath,
            sizeof(gWorkingPath),
            gLocalComponents,
            gLocalDepth))
    {
        gLocalCount = 0;
        return;
    }
    if (!enumerate_directory(
            gWorkingPath,
            0,
            RL_MAX_LIST_ENTRIES,
            gLocalEntries,
            &count,
            &next_cursor,
            1))
    {
        gLocalCount = 0;
        set_status("Cannot read the selected LOCAL directory.");
        return;
    }
    gLocalCount = count;
    if (gLocalSelection >= gLocalCount)
        gLocalSelection = gLocalCount == 0 ? 0 : (rl_u8)(gLocalCount - 1);
}

static rl_u8 collect_marked_files(rl_u8 *indices)
{
    rl_u8 count;
    rl_u8 index;

    count = 0;
    for (index = 0; index < gLocalCount; ++index)
    {
        if (gLocalMarked[index] && gLocalEntries[index].kind == 0)
            indices[count++] = index;
    }
    return count;
}

static void toggle_local_mark(void)
{
    char status[76];
    rl_u8 indices[RL_MAX_LIST_ENTRIES];
    rl_u8 marked_count;

    if (gActivePanel != 0 || gLocalSelection >= gLocalCount)
    {
        set_status("Select a file in the LOCAL pane before marking it.");
        return;
    }
    if (gLocalEntries[gLocalSelection].kind != 0)
    {
        set_status("Only files can be selected for transfer.");
        return;
    }

    gLocalMarked[gLocalSelection] = (rl_u8)!gLocalMarked[gLocalSelection];
    marked_count = collect_marked_files(indices);
    sprintf(status, "%u LOCAL file%s selected for transfer.",
        (unsigned int)marked_count,
        marked_count == 1 ? "" : "s");
    set_status(status);
}

static void handle_list_request(const RlFrame *frame)
{
    rl_u16 offset;
    rl_u32 epoch;
    rl_u32 provider_id;
    rl_u32 cursor;
    rl_u16 limit;
    rl_u8 active_endpoint[RL_SESSION_ID_SIZE];
    rl_u8 component_count;
    char components[RL_MAX_PATH_COMPONENTS][RL_MAX_NAME_BYTES + 1];
    char path[160];
    RlDirectoryEntry entries[RL_MAX_LIST_ENTRIES];
    rl_u8 entry_count;
    rl_u32 next_cursor;
    rl_u16 page_length;
    rl_u8 requested_limit;
    int status;

    if (frame->payload_length < 4 + RL_SESSION_ID_SIZE + 5 + 6)
        return;
    epoch = read_u32(frame->payload);
    memcpy(active_endpoint, frame->payload + 4, RL_SESSION_ID_SIZE);
    offset = 4 + RL_SESSION_ID_SIZE;
    provider_id = read_u32(frame->payload + offset);
    memset(components, 0, sizeof(components));
    if (!path_component_from_payload(
            frame->payload,
            frame->payload_length,
            &offset,
            components,
            &component_count) ||
        frame->payload_length - offset != 6)
    {
        return;
    }
    cursor = read_u32(frame->payload + offset);
    limit = read_u16(frame->payload + offset + 4);
    requested_limit = (rl_u8)limit;
    if (epoch != gRoleEpoch ||
        memcmp(active_endpoint, gActiveEndpointId, RL_SESSION_ID_SIZE) != 0 ||
        memcmp(active_endpoint, gPeer.endpoint_id, RL_SESSION_ID_SIZE) != 0 ||
        provider_id != RL_DOS_PROVIDER_ID ||
        limit == 0 || limit > RL_MAX_LIST_ENTRIES ||
        !build_dos_path(path, sizeof(path), components, component_count))
    {
        page_length = rl_make_list_page(
            gPayload,
            1,
            cursor,
            RL_LIST_COMPLETE_CURSOR,
            0,
            0);
        if (page_length != 0)
            send_link_frame(RL_LIST_PAGE, frame->request_id, gPayload, page_length);
        return;
    }

    status = enumerate_directory(
        path,
        cursor,
        requested_limit,
        entries,
        &entry_count,
        &next_cursor,
        0);
    page_length = rl_make_list_page(
        gPayload,
        status ? 0 : 1,
        cursor,
        status ? next_cursor : RL_LIST_COMPLETE_CURSOR,
        status ? entries : 0,
        status ? entry_count : 0);
    while (status && page_length == 0 && entry_count > 0)
    {
        --entry_count;
        next_cursor = cursor + entry_count;
        page_length = rl_make_list_page(
            gPayload,
            0,
            cursor,
            next_cursor,
            entries,
            entry_count);
    }
    if (page_length != 0)
        send_link_frame(RL_LIST_PAGE, frame->request_id, gPayload, page_length);
}

static void handle_role_request(const RlFrame *frame)
{
    rl_u32 expected_epoch;
    if (frame->payload_length != 21 ||
        memcmp(frame->payload, gPeer.endpoint_id, RL_SESSION_ID_SIZE) != 0)
    {
        return;
    }
    expected_epoch = read_u32(frame->payload + RL_SESSION_ID_SIZE);
    if (expected_epoch != gRoleEpoch || frame->payload[20] > 2 || gTransferActive)
    {
        send_role_state();
        return;
    }
    ++gRoleEpoch;
    memcpy(gActiveEndpointId, gPeer.endpoint_id, RL_SESSION_ID_SIZE);
    send_role_state();
    gRoleChanged = 1;
    set_status("REMOTE requested control and is now ACTIVE.");
}

static void handle_frame(const RlFrame *frame)
{
    if (frame->message_type == RL_HELLO_ACK)
    {
        gHelloAckSeen = 1;
        if (memcmp(frame->session_id, gSessionId, RL_SESSION_ID_SIZE) != 0)
        {
            gHelloAckSessionMismatch = 1;
            return;
        }
        if (rl_parse_hello_ack(frame->payload, frame->payload_length, &gPeer))
        {
            gPeerReady = 1;
            return;
        }
        gHelloAckInvalid = 1;
        return;
    }
    if (!gConnected ||
        memcmp(frame->session_id, gSessionId, RL_SESSION_ID_SIZE) != 0)
    {
        return;
    }
    switch (frame->message_type)
    {
    case RL_ROLE_STATE:
        if (frame->payload_length == 21)
        {
            gRoleEpoch = read_u32(frame->payload);
            memcpy(gActiveEndpointId, frame->payload + 4, RL_SESSION_ID_SIZE);
            gRoleChanged = 1;
        }
        break;
    case RL_ROLE_REQUEST:
        handle_role_request(frame);
        break;
    case RL_LIST_REQUEST:
        handle_list_request(frame);
        break;
    case RL_MAKE_DIRECTORY:
    case RL_RENAME:
    case RL_DELETE:
        handle_operation_request(frame);
        break;
    case RL_OPERATION_RESULT:
        handle_operation_result(frame);
        break;
    case RL_LIST_PAGE:
        if (frame->request_id == gPendingListRequest)
        {
            if (rl_parse_list_page(
                    frame->payload,
                    frame->payload_length,
                    &gDecodedPage))
            {
                gRemoteCount = gDecodedPage.entry_count;
                memcpy(
                    gRemoteEntries,
                    gDecodedPage.entries,
                    gRemoteCount * sizeof(gRemoteEntries[0]));
                if (gDecodedPage.result_code == 0 &&
                    gDecodedPage.cursor == 0 &&
                    !gRemoteAtVolumes)
                {
                    prepend_remote_parent();
                }
                gRemoteSelection = 0;
                gListReady = 1;
                if (gDecodedPage.result_code != 0)
                    set_status("REMOTE rejected the directory listing request.");
            }
            else
            {
                memset(&gDecodedPage, 0, sizeof(gDecodedPage));
                gDecodedPage.result_code = 8;
                gRemoteCount = 0;
                gListReady = 1;
                set_status("REMOTE returned an invalid directory page.");
            }
        }
        else if (gPendingListRequest != 0)
            set_status("Ignored a stale REMOTE directory page.");
        break;
    case RL_TRANSFER_READY:
        if (frame->request_id == gPendingTransferRequest &&
            frame->payload_length == 17)
        {
            memcpy(gPayload, frame->payload, 17);
            gTransferReady = 1;
        }
        break;
    case RL_TRANSFER_COMPLETE:
        if (frame->request_id == gPendingTransferRequest &&
            frame->payload_length == 14)
        {
            memcpy(gPayload, frame->payload, 14);
            gTransferComplete = 1;
        }
        break;
    case RL_COPY_PREPARE:
        handle_incoming_copy_prepare(frame);
        break;
    case RL_ERROR:
        if (frame->payload_length >= 4 &&
            frame->request_id == gPendingListRequest &&
            frame->payload[2] == RL_LIST_REQUEST)
        {
            memset(&gDecodedPage, 0, sizeof(gDecodedPage));
            gDecodedPage.result_code = 8;
            gListReady = 1;
            set_status("REMOTE rejected the directory listing request.");
        }
        else if (frame->payload_length >= 4 &&
                 frame->request_id == gPendingOperationRequest &&
                 frame->payload[2] == gPendingOperationKind)
        {
            gOperationResultCode = 8;
            gOperationReady = 1;
            set_status("REMOTE rejected the filesystem operation.");
        }
        else if (frame->payload_length >= 4)
        {
            set_status("REMOTE returned a RetroLink protocol error.");
        }
        break;
    default:
        break;
    }
}

static void retrolink_poll(void)
{
    rl_u8 value;
    while (dos_uart_try_read(&gLinkUart, &value))
    {
        ++gReceivedByteCount;
        if (rl_decode_frame_byte(&gDecoder, value, &gFrame))
        {
            ++gDecodedFrameCount;
            gLastFrameType = gFrame.message_type;
            handle_frame(&gFrame);
            if (gTransferActive && gTransferReady)
                break;
        }
    }
}

static void pump(void)
{
    agent_link_poll(&gAgent);
    retrolink_poll();
}

static int wait_for_flag(volatile rl_u8 *flag, rl_u16 timeout_seconds)
{
    rl_u32 start;
    rl_u32 timeout_ticks;
    start = dos_clock_ticks();
    timeout_ticks = (rl_u32)timeout_seconds * 18UL;
    while (*flag == 0)
    {
        pump();
        if (elapsed_ticks(start, dos_clock_ticks()) >= timeout_ticks)
            return 0;
        dos_idle();
    }
    return 1;
}

static int connect_peer(void)
{
    rl_u8 hello[29];
    rl_u16 encoded_length;
    rl_u32 capabilities;

    make_random_ids();
    memset(&gDecoder, 0, sizeof(gDecoder));
    memset(&gPeer, 0, sizeof(gPeer));
    gConnected = 0;
    gRemoteAtVolumes = 0;
    gRemoteCount = 0;
    gRemoteDepth = 0;
    gReceivedByteCount = 0;
    gDecodedFrameCount = 0;
    gHelloAckSeen = 0;
    gHelloAckSessionMismatch = 0;
    gHelloAckInvalid = 0;
    gLastFrameType = 0;
    memset(hello, 0, sizeof(hello));
    hello[0] = 1;
    hello[1] = 1;
    hello[2] = 1;
    memcpy(hello + 5, gEndpointId, RL_SESSION_ID_SIZE);
    capabilities = 0x00000051UL;
    write_u32(hello + 21, capabilities);
    write_u32(hello + 25, gBaudRate);
    gNextRequestId = 1;
    gPeerReady = 0;
    memcpy(gActiveEndpointId, gEndpointId, RL_SESSION_ID_SIZE);
    gFrame.message_type = RL_HELLO;
    memcpy(gFrame.session_id, gSessionId, RL_SESSION_ID_SIZE);
    gFrame.request_id = gNextRequestId++;
    gFrame.payload_length = sizeof(hello);
    memcpy(gFrame.payload, hello, sizeof(hello));
    encoded_length = rl_encode_frame(&gFrame, gEncoded);
    if (encoded_length == 0)
        return 0;
    {
        rl_u16 index;
        for (index = 0; index < encoded_length; ++index)
        {
            if (!dos_uart_write(&gLinkUart, gEncoded[index], 10))
                return 0;
        }
    }

    if (!wait_for_flag(&gPeerReady, 10))
    {
        char diagnostic[76];
        const char *ackStatus = gHelloAckSessionMismatch
            ? "session"
            : gHelloAckInvalid
                ? "invalid"
                : gHelloAckSeen
                    ? "ok"
                    : "none";
        sprintf(
            diagnostic,
            "Timeout RX=%lu frames=%lu type=%02X HELLO_ACK=%s",
            gReceivedByteCount,
            gDecodedFrameCount,
            gLastFrameType,
            ackStatus);
        set_status(diagnostic);
        return 0;
    }
    gConnected = 1;
    gRoleEpoch = 0;
    send_role_state();
    return 1;
}

static int ensure_active(void)
{
    rl_u16 request_length;
    if (memcmp(gActiveEndpointId, gEndpointId, RL_SESSION_ID_SIZE) == 0)
        return 1;

    gRoleChanged = 0;
    request_length = rl_make_role_request(
        gPayload,
        gEndpointId,
        gRoleEpoch,
        1);
    if (request_length == 0 ||
        !send_link_frame(RL_ROLE_REQUEST, gNextRequestId++, gPayload, request_length))
    {
        set_status("Could not request RetroLink control.");
        return 0;
    }
    if (!wait_for_flag(&gRoleChanged, 10) ||
        memcmp(gActiveEndpointId, gEndpointId, RL_SESSION_ID_SIZE) != 0)
    {
        set_status("Timeout: REMOTE did not hand over RetroLink control.");
        return 0;
    }
    return 1;
}

static int request_remote_list(void)
{
    const char *components[RL_MAX_PATH_COMPONENTS];
    rl_u16 payload_length;
    rl_u32 request_id;

    if (!gConnected || gPeer.root_count == 0)
    {
        set_status("REMOTE did not advertise a mounted volume.");
        return 0;
    }
    if (gRemoteAtVolumes)
    {
        set_status("Select a REMOTE volume before opening a directory.");
        return 0;
    }
    if (!ensure_active())
        return 0;

    fill_component_pointers(
        gRemoteComponents,
        gRemoteDepth,
        components);
    payload_length = rl_make_list_request(
        gPayload,
        gRoleEpoch,
        gEndpointId,
        gPeer.roots[gRemoteRootIndex].provider_id,
        components,
        gRemoteDepth,
        0,
        RL_MAX_LIST_ENTRIES);
    if (payload_length == 0)
    {
        set_status("The REMOTE directory path is too long for RetroLink.");
        return 0;
    }

    gListReady = 0;
    request_id = gNextRequestId++;
    gPendingListRequest = request_id;
    if (!send_link_frame(RL_LIST_REQUEST, request_id, gPayload, payload_length))
    {
        set_status("Could not send LIST_REQUEST to REMOTE.");
        return 0;
    }
    set_status("Reading the REMOTE directory...");
    if (!wait_for_flag(&gListReady, 10))
    {
        set_status("Timeout: REMOTE did not return a directory page.");
        return 0;
    }
    if (gDecodedPage.result_code != 0)
    {
        set_status("REMOTE rejected the directory listing request.");
        return 0;
    }
    set_status("REMOTE directory loaded.");
    return 1;
}

static void show_remote_volumes(void)
{
    rl_u8 index;

    gRemoteAtVolumes = 1;
    gRemoteDepth = 0;
    gRemoteRootIndex = 0;
    gRemoteCount = gPeer.root_count;
    if (gRemoteCount > RL_MAX_ROOTS)
        gRemoteCount = RL_MAX_ROOTS;
    for (index = 0; index < gRemoteCount; ++index)
    {
        unsigned int name_length;
        RlDirectoryEntry *entry = &gRemoteEntries[index];

        memset(entry, 0, sizeof(*entry));
        name_length = (unsigned int)strlen(gPeer.roots[index].name);
        if (name_length > RL_MAX_NAME_BYTES)
            name_length = RL_MAX_NAME_BYTES;
        memcpy(entry->name, gPeer.roots[index].name, name_length);
        entry->name[name_length] = '\0';
        entry->kind = 1;
        entry->attributes = DOS_DIRECTORY_ATTRIBUTE;
    }
    gRemoteSelection = 0;
}

static int move_remote_parent(void)
{
    if (gRemoteAtVolumes)
        return 0;
    if (gRemoteDepth != 0)
        --gRemoteDepth;
    else
        show_remote_volumes();
    gRemoteSelection = 0;
    return 1;
}

static int wait_link_byte(rl_u8 *value, rl_u16 timeout_seconds)
{
    rl_u32 start;
    rl_u32 timeout_ticks;

    if (value == 0)
        return 0;
    start = dos_clock_ticks();
    timeout_ticks = (rl_u32)timeout_seconds * 18UL;
    for (;;)
    {
        agent_link_poll(&gAgent);
        if (dos_uart_try_read(&gLinkUart, value))
            return 1;
        if (elapsed_ticks(start, dos_clock_ticks()) >= timeout_ticks)
            return 0;
        dos_idle();
    }
}

static int send_xmodem_file(FILE *file, rl_u32 length)
{
    rl_u8 response;
    rl_u8 block_number;
    rl_u32 sent;
    unsigned int attempts;
    unsigned int index;
    size_t read_count;
    rl_u16 crc;
    char status[76];

    if (length == 0)
        return 1;
    if (!wait_link_byte(&response, 10) || response != XMODEM_CRC_REQUEST)
        return 0;

    block_number = 1;
    sent = 0;
    while (sent < length)
    {
        rl_u32 remaining;
        rl_u16 actual_count;
        remaining = length - sent;
        actual_count = remaining > XMODEM_BLOCK_BYTES
            ? XMODEM_BLOCK_BYTES : (rl_u16)remaining;
        memset(gXmodemPacket + 3, 0x1A, XMODEM_BLOCK_BYTES);
        read_count = fread(
            gXmodemPacket + 3,
            1,
            actual_count,
            file);
        if (read_count != actual_count)
            return 0;
        gXmodemPacket[0] = XMODEM_SOH;
        gXmodemPacket[1] = block_number;
        gXmodemPacket[2] = (rl_u8)~block_number;
        crc = rl_crc16_xmodem(gXmodemPacket + 3, XMODEM_BLOCK_BYTES);
        gXmodemPacket[3 + XMODEM_BLOCK_BYTES] = (rl_u8)(crc >> 8);
        gXmodemPacket[4 + XMODEM_BLOCK_BYTES] = (rl_u8)crc;

        for (attempts = 0; attempts < 3; ++attempts)
        {
            for (index = 0; index < sizeof(gXmodemPacket); ++index)
            {
                if (!dos_uart_write(&gLinkUart, gXmodemPacket[index], 10))
                    return 0;
            }
            if (!wait_link_byte(&response, 10))
                continue;
            if (response == XMODEM_ACK)
                break;
            if (response != XMODEM_NAK)
                return 0;
        }
        if (attempts == 3)
            return 0;

        sent += actual_count;
        ++block_number;
        sprintf(
            status,
            "Sending to REMOTE: %lu of %lu bytes (%lu%%)",
            sent,
            length,
            (sent * 100UL) / length);
        set_progress((rl_u8)((sent * 100UL) / length));
        set_status(status);
    }

    for (attempts = 0; attempts < 3; ++attempts)
    {
        if (!dos_uart_write(&gLinkUart, XMODEM_EOT, 10))
            return 0;
        if (wait_link_byte(&response, 10) && response == XMODEM_ACK)
            return 1;
    }
    return 0;
}

static int compute_file_crc(FILE *file, rl_u32 *length, rl_u32 *crc32)
{
    rl_u8 buffer[128];
    size_t count;
    rl_u32 state;
    long file_length;

    if (fseek(file, 0L, SEEK_END) != 0)
        return 0;
    file_length = ftell(file);
    if (file_length < 0 || (unsigned long)file_length > RL_MAX_TRANSFER_BYTES)
        return 0;
    if (fseek(file, 0L, SEEK_SET) != 0)
        return 0;

    state = 0xFFFFFFFFUL;
    *length = (rl_u32)file_length;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0)
    {
        state = rl_crc32_update(state, buffer, (rl_u16)count);
    }
    if (ferror(file) || fseek(file, 0L, SEEK_SET) != 0)
        return 0;
    *crc32 = rl_crc32_finish(state);
    return 1;
}

static int send_selected_file(void)
{
    const char *source_components[RL_MAX_PATH_COMPONENTS];
    const char *destination_components[RL_MAX_PATH_COMPONENTS];
    FILE *file;
    rl_u32 file_length;
    rl_u32 file_crc;
    rl_u32 request_id;
    rl_u16 payload_length;
    rl_u16 result_code;
    rl_u32 ready_length;
    rl_u32 ready_crc;
    char file_path[160];
    char status[76];
    RlDirectoryEntry *entry;

    if (gActivePanel != 0 || gLocalSelection >= gLocalCount)
    {
        set_status("F5 sends selected LOCAL files to REMOTE; choose the LOCAL pane.");
        return 0;
    }
    if (gRemoteAtVolumes)
    {
        set_status("Open a REMOTE volume before transferring a LOCAL file.");
        return 0;
    }
    entry = &gLocalEntries[gLocalSelection];
    if (entry->kind != 0 || !ensure_active())
    {
        set_status("Select a regular LOCAL file and ensure LOCAL is ACTIVE.");
        return 0;
    }
    if (!build_dos_path(
            file_path,
            sizeof(file_path),
            gLocalComponents,
            gLocalDepth))
    {
        set_status("The LOCAL source path is too long.");
        return 0;
    }
    if (strlen(file_path) + strlen(entry->name) + 2 >= sizeof(file_path))
    {
        set_status("The LOCAL source path is too long.");
        return 0;
    }
    if (strcmp(file_path, "C:\\") != 0)
        strcat(file_path, "\\");
    strcat(file_path, entry->name);
    file = fopen(file_path, "rb");
    if (file == 0)
    {
        set_status("Could not open the selected LOCAL file.");
        return 0;
    }
    if (!compute_file_crc(file, &file_length, &file_crc))
    {
        fclose(file);
        set_status("File is too large or could not be read (maximum 512 KiB).");
        return 0;
    }
    fill_component_pointers(
        gLocalComponents,
        gLocalDepth,
        source_components);
    fill_component_pointers(
        gRemoteComponents,
        gRemoteDepth,
        destination_components);
    payload_length = rl_make_copy_prepare(
        gPayload,
        gRoleEpoch,
        gEndpointId,
        gEndpointId,
        RL_DOS_PROVIDER_ID,
        source_components,
        gLocalDepth,
        gPeer.endpoint_id,
        gPeer.roots[gRemoteRootIndex].provider_id,
        destination_components,
        gRemoteDepth,
        entry->name,
        file_length,
        file_crc,
        0);
    if (payload_length == 0)
    {
        fclose(file);
        set_status("The file or destination path cannot be represented in RetroLink.");
        return 0;
    }

    gTransferReady = 0;
    gTransferComplete = 0;
    gTransferActive = 1;
    set_progress(0);
    request_id = gNextRequestId++;
    gPendingTransferRequest = request_id;
    if (!send_link_frame(RL_COPY_PREPARE, request_id, gPayload, payload_length))
    {
        gTransferActive = 0;
        fclose(file);
        set_status("Could not send COPY_PREPARE to REMOTE.");
        return 0;
    }
    set_status("Waiting for REMOTE to prepare the destination...");
    if (!wait_for_flag(&gTransferReady, 10))
    {
        gTransferActive = 0;
        fclose(file);
        set_status("Timeout: REMOTE did not prepare the transfer in 10 seconds.");
        return 0;
    }

    result_code = read_u16(gPayload);
    ready_length = read_u32(gPayload + 2);
    ready_crc = read_u32(gPayload + 10);
    if (result_code != 0 || read_u32(gPayload + 6) != file_length ||
        ready_length != 0 || ready_crc != file_crc ||
        gPayload[14] != 1 || read_u16(gPayload + 15) != XMODEM_BLOCK_BYTES)
    {
        gTransferActive = 0;
        fclose(file);
        set_status("REMOTE rejected the file size, CRC, or XMODEM mode.");
        return 0;
    }

    sprintf(status, "Starting XMODEM-CRC transfer of %lu bytes...", file_length);
    set_status(status);
    if (!send_xmodem_file(file, file_length))
    {
        gTransferActive = 0;
        fclose(file);
        gProgressActive = 0;
        set_status("XMODEM-CRC transfer failed or timed out.");
        return 0;
    }
    fclose(file);

    set_status("Data sent; waiting for REMOTE to verify and save the file.");
    if (!wait_for_flag(&gTransferComplete, 10))
    {
        gTransferActive = 0;
        gProgressActive = 0;
        set_status("Timeout: REMOTE did not confirm the completed transfer.");
        return 0;
    }
    result_code = read_u16(gPayload);
    if (result_code != 0 ||
        read_u32(gPayload + 2) != 0 ||
        read_u32(gPayload + 6) != file_length ||
        read_u32(gPayload + 10) != file_crc)
    {
        gTransferActive = 0;
        gProgressActive = 0;
        set_status("REMOTE reported a transfer verification failure.");
        return 0;
    }
    set_progress(100);
    gTransferActive = 0;
    agent_link_log(&gAgent, 0, "RetroLink file transfer completed");
    set_status("Transfer complete; REMOTE verified the file.");
    request_remote_list();
    return 1;
}

static int send_marked_files(void)
{
    rl_u8 marked_indices[RL_MAX_LIST_ENTRIES];
    rl_u8 marked_count;
    rl_u8 original_selection;
    rl_u8 position;
    char status[76];
    int sent_count;

    if (gActivePanel != 0)
    {
        set_status("Select files in the LOCAL pane before transferring.");
        return 0;
    }
    marked_count = collect_marked_files(marked_indices);
    if (marked_count == 0)
        return send_selected_file();

    original_selection = gLocalSelection;
    sent_count = 0;
    for (position = 0; position < marked_count; ++position)
    {
        gLocalSelection = marked_indices[position];
        sprintf(status, "Sending file %u of %u: %.12s",
            (unsigned int)(position + 1),
            (unsigned int)marked_count,
            gLocalEntries[gLocalSelection].name);
        set_status(status);
        draw_ui();
        if (!send_selected_file())
        {
            if (original_selection < gLocalCount)
                gLocalSelection = original_selection;
            draw_ui();
            return 0;
        }
        gLocalMarked[gLocalSelection] = 0;
        ++sent_count;
        draw_ui();
    }

    if (original_selection < gLocalCount)
        gLocalSelection = original_selection;
    draw_ui();
    sprintf(status, "Transferred %d selected LOCAL file%s to REMOTE.",
        sent_count,
        sent_count == 1 ? "" : "s");
    set_status(status);
    draw_ui();
    return 1;
}

static int receive_xmodem_file(
    FILE *file,
    rl_u32 expected_length,
    rl_u32 expected_crc,
    rl_u32 *actual_crc)
{
    rl_u8 response;
    rl_u8 expected_block;
    rl_u8 block;
    rl_u8 complement;
    rl_u16 received_crc;
    rl_u16 calculated_crc;
    rl_u32 received;
    rl_u32 crc_state;
    rl_u32 remaining;
    rl_u16 write_count;
    unsigned int bad_blocks;
    unsigned int index;
    char status[76];

    if (expected_length == 0)
    {
        *actual_crc = 0;
        return expected_crc == 0;
    }

    if (!dos_uart_write(&gLinkUart, XMODEM_CRC_REQUEST, 10))
        return 0;
    received = 0;
    crc_state = 0xFFFFFFFFUL;
    expected_block = 1;
    bad_blocks = 0;
    for (;;)
    {
        if (!wait_link_byte(&response, 10))
            return 0;
        if (response == XMODEM_EOT)
        {
            if (received == expected_length)
            {
                if (!dos_uart_write(&gLinkUart, XMODEM_ACK, 10))
                    return 0;
                break;
            }
            if (!dos_uart_write(&gLinkUart, XMODEM_NAK, 10))
                return 0;
            if (++bad_blocks >= 3)
                return 0;
            continue;
        }
        if (response == 0x18)
            return 0;
        if (response != XMODEM_SOH)
        {
            if (!dos_uart_write(&gLinkUart, XMODEM_NAK, 10))
                return 0;
            if (++bad_blocks >= 3)
                return 0;
            continue;
        }
        for (index = 1; index < sizeof(gXmodemPacket); ++index)
        {
            if (!wait_link_byte(&gXmodemPacket[index], 10))
                return 0;
        }
        block = gXmodemPacket[1];
        complement = gXmodemPacket[2];
        received_crc = (rl_u16)(
            ((rl_u16)gXmodemPacket[3 + XMODEM_BLOCK_BYTES] << 8) |
            gXmodemPacket[4 + XMODEM_BLOCK_BYTES]);
        calculated_crc = rl_crc16_xmodem(gXmodemPacket + 3, XMODEM_BLOCK_BYTES);
        if ((rl_u8)(block ^ complement) != 0xFF ||
            received_crc != calculated_crc)
        {
            if (!dos_uart_write(&gLinkUart, XMODEM_NAK, 10))
                return 0;
            if (++bad_blocks >= 3)
                return 0;
            continue;
        }
        if (block == (rl_u8)(expected_block - 1))
        {
            if (!dos_uart_write(&gLinkUart, XMODEM_ACK, 10))
                return 0;
            continue;
        }
        if (block != expected_block || received >= expected_length)
        {
            if (!dos_uart_write(&gLinkUart, XMODEM_NAK, 10))
                return 0;
            if (++bad_blocks >= 3)
                return 0;
            continue;
        }

        remaining = expected_length - received;
        write_count = remaining > XMODEM_BLOCK_BYTES
            ? XMODEM_BLOCK_BYTES : (rl_u16)remaining;
        if (fwrite(gXmodemPacket + 3, 1, write_count, file) != write_count)
            return 0;
        crc_state = rl_crc32_update(crc_state, gXmodemPacket + 3, write_count);
        received += write_count;
        ++expected_block;
        bad_blocks = 0;
        if (!dos_uart_write(&gLinkUart, XMODEM_ACK, 10))
            return 0;
        sprintf(
            status,
            "Receiving from REMOTE: %lu of %lu bytes (%lu%%)",
            received,
            expected_length,
            (received * 100UL) / expected_length);
        set_progress((rl_u8)((received * 100UL) / expected_length));
        set_status(status);
    }

    *actual_crc = rl_crc32_finish(crc_state);
    return received == expected_length && *actual_crc == expected_crc;
}

static void send_transfer_ready(
    const RlFrame *request,
    rl_u16 result_code,
    rl_u32 length,
    rl_u32 crc32)
{
    memset(gPayload, 0, 17);
    write_u16(gPayload, result_code);
    write_u32(gPayload + 6, length);
    write_u32(gPayload + 10, crc32);
    gPayload[14] = 1;
    write_u16(gPayload + 15, XMODEM_BLOCK_BYTES);
    send_link_frame(
        RL_TRANSFER_READY,
        request->request_id,
        gPayload,
        17);
}

static void send_transfer_complete(
    const RlFrame *request,
    rl_u16 result_code,
    rl_u32 length,
    rl_u32 crc32)
{
    memset(gPayload, 0, 14);
    write_u16(gPayload, result_code);
    write_u32(gPayload + 6, length);
    write_u32(gPayload + 10, crc32);
    send_link_frame(
        RL_TRANSFER_COMPLETE,
        request->request_id,
        gPayload,
        14);
}

static void handle_incoming_copy_prepare(const RlFrame *frame)
{
    rl_u32 role_epoch;
    rl_u32 source_provider;
    rl_u32 destination_provider;
    rl_u32 file_length;
    rl_u32 expected_crc;
    rl_u32 actual_crc;
    rl_u8 active_endpoint[RL_SESSION_ID_SIZE];
    rl_u8 source_endpoint[RL_SESSION_ID_SIZE];
    rl_u8 destination_endpoint[RL_SESSION_ID_SIZE];
    char source_components[RL_MAX_PATH_COMPONENTS][RL_MAX_NAME_BYTES + 1];
    char destination_components[RL_MAX_PATH_COMPONENTS][RL_MAX_NAME_BYTES + 1];
    rl_u8 source_count;
    rl_u8 destination_count;
    char destination_name[RL_MAX_NAME_BYTES + 1];
    rl_u8 overwrite;
    FILE *file;
    char destination_path[160];
    unsigned int name_length;
    int valid;
    int receive_ok;
    int close_result;

    valid = rl_parse_copy_prepare(
        frame->payload,
        frame->payload_length,
        &role_epoch,
        active_endpoint,
        source_endpoint,
        &source_provider,
        source_components,
        &source_count,
        destination_endpoint,
        &destination_provider,
        destination_components,
        &destination_count,
        destination_name,
        &file_length,
        &expected_crc,
        &overwrite);
    if (!valid ||
        role_epoch != gRoleEpoch ||
        memcmp(active_endpoint, gActiveEndpointId, RL_SESSION_ID_SIZE) != 0 ||
        memcmp(destination_endpoint, gEndpointId, RL_SESSION_ID_SIZE) != 0 ||
        destination_provider != RL_DOS_PROVIDER_ID ||
        file_length > RL_MAX_TRANSFER_BYTES)
    {
        send_transfer_ready(frame, 1, 0, 0);
        set_status("The REMOTE transfer request targets an unsupported LOCAL path.");
        return;
    }

    (void)source_provider;
    (void)source_components;
    (void)source_count;
    if (memcmp(source_endpoint, gPeer.endpoint_id, RL_SESSION_ID_SIZE) != 0 ||
        !build_dos_path(
            destination_path,
            sizeof(destination_path),
            destination_components,
            destination_count))
    {
        send_transfer_ready(frame, 1, 0, 0);
        set_status("The REMOTE transfer source or LOCAL destination is invalid.");
        return;
    }
    name_length = (unsigned int)strlen(destination_name);
    if (strlen(destination_path) + name_length + 2 >= sizeof(destination_path))
    {
        send_transfer_ready(frame, 1, 0, 0);
        set_status("The LOCAL destination path is too long.");
        return;
    }
    if (strcmp(destination_path, "C:\\") != 0)
        strcat(destination_path, "\\");
    strcat(destination_path, destination_name);
    if (!overwrite && access(destination_path, 0) == 0)
    {
        send_transfer_ready(frame, 1, 0, 0);
        set_status("The LOCAL destination file already exists.");
        return;
    }
    file = fopen(destination_path, "wb");
    if (file == 0)
    {
        send_transfer_ready(frame, 1, 0, 0);
        set_status("Could not create the LOCAL destination file.");
        return;
    }

    gTransferActive = 1;
    set_progress(0);
    send_transfer_ready(frame, 0, file_length, expected_crc);
    receive_ok = receive_xmodem_file(
        file,
        file_length,
        expected_crc,
        &actual_crc);
    close_result = fclose(file);
    if (receive_ok && close_result == 0)
    {
        send_transfer_complete(frame, 0, file_length, expected_crc);
        gTransferActive = 0;
        set_progress(100);
        agent_link_log(&gAgent, 0, "REMOTE-to-LOCAL file transfer completed");
        set_status("The REMOTE file was received and verified.");
        reload_local_entries();
        return;
    }

    remove(destination_path);
    send_transfer_complete(frame, 1, file_length, expected_crc);
    gTransferActive = 0;
    gProgressActive = 0;
    set_status("The REMOTE file failed XMODEM or CRC verification.");
}

static void draw_panel(
    int column,
    const char *title,
    const RlDirectoryEntry *entries,
    rl_u8 entry_count,
    rl_u8 selected,
    const rl_u8 *marked,
    int active)
{
    rl_u8 first;
    rl_u8 index;
    int row;

    first = selected >= UI_VISIBLE_ROWS
        ? (rl_u8)(selected - UI_VISIBLE_ROWS + 1) : 0;
    console_gotoxy(column, 4);
    cprintf("%c%-37.37s", active ? '>' : ' ', title);
    for (row = 0; row < UI_VISIBLE_ROWS; ++row)
    {
        index = (rl_u8)(first + row);
        console_gotoxy(column, 5 + row);
        if (index >= entry_count)
        {
            cprintf("%-38.38s", "");
        }
        else
        {
            const RlDirectoryEntry *entry = &entries[index];
            char kind;
            char selected_marker;
            char marked_marker;
            char name[14];
            unsigned int name_length;
            kind = entry->kind == 1 ? '<' : ' ';
            selected_marker = index == selected && active ? '>' : ' ';
            marked_marker = marked != 0 && marked[index] ? '*' : ' ';
            name_length = (unsigned int)strlen(entry->name);
            if (name_length > 12)
                name_length = 12;
            memcpy(name, entry->name, name_length);
            name[name_length] = '\0';
            cprintf(
                "%c%c%c%-12.12s %10lu",
                marked_marker,
                selected_marker,
                kind,
                name,
                entry->data_size);
        }
    }
}

static void draw_ui(void)
{
    char local_title[39];
    char remote_title[39];
    unsigned int length;
    rl_u8 index;

    console_clear();
    console_gotoxy(1, 1);
    cprintf("RetroLink DOS 8088 v0.1  |  %lu baud  |  %s",
        gBaudRate,
        gConnected ? "CONNECTED" : "DISCONNECTED");
    console_gotoxy(1, 2);
    cprintf("F3 Connect F5 Send Marked/Current Ins Select Tab Switch Enter Open F10 Exit");
    console_gotoxy(1, 21);
    cprintf("Backspace Up  F7 New Folder  F8 Delete  R Rename  F2 Baud");
    console_gotoxy(1, 3);
    cprintf("%-38.38s|%-38.38s", "LOCAL", "REMOTE");

    if (!build_dos_path(
            local_title,
            sizeof(local_title),
            gLocalComponents,
            gLocalDepth))
    {
        strcpy(local_title, "LOCAL path unavailable");
    }
    draw_panel(
        1,
        local_title,
        gLocalEntries,
        gLocalCount,
        gLocalSelection,
        gLocalMarked,
        gActivePanel == 0);

    if (gConnected && gPeer.root_count != 0)
    {
        if (gRemoteAtVolumes)
        {
            strcpy(remote_title, "REMOTE volumes");
        }
        else
        {
            strncpy(
                remote_title,
                gPeer.roots[gRemoteRootIndex].name,
                sizeof(remote_title) - 1);
            remote_title[sizeof(remote_title) - 1] = '\0';
            for (index = 0; index < gRemoteDepth; ++index)
            {
                length = (unsigned int)strlen(remote_title);
                if (length + strlen(gRemoteComponents[index]) + 2 <
                    sizeof(remote_title))
                {
                    strcat(remote_title, "\\");
                    strcat(remote_title, gRemoteComponents[index]);
                }
            }
        }
        draw_panel(
            42,
            remote_title,
            gRemoteEntries,
            gRemoteCount,
            gRemoteSelection,
            0,
            gActivePanel == 1);
    }
    else
    {
        draw_panel(
            42,
            "REMOTE (not connected)",
            gRemoteEntries,
            0,
            0,
            0,
            gActivePanel == 1);
    }

    console_gotoxy(40, 4);
    for (index = 0; index < 17; ++index)
    {
        console_gotoxy(40, 4 + index);
        cprintf("|");
    }
    console_gotoxy(1, 22);
    cprintf("Role: %s", memcmp(
        gActiveEndpointId,
        gEndpointId,
        RL_SESSION_ID_SIZE) == 0 ? "LOCAL ACTIVE" : "REMOTE ACTIVE");
    console_gotoxy(1, 23);
    if (gProgressActive)
    {
        set_progress(gProgressPercent);
    }
    else
    {
        cprintf("%-79.79s", "");
    }
    console_gotoxy(1, 25);
    cprintf("%-79.79s", gStatus);
}

static void open_local_selection(void)
{
    RlDirectoryEntry *entry;
    if (gLocalSelection >= gLocalCount)
        return;
    entry = &gLocalEntries[gLocalSelection];
    if (entry->kind != 1)
        return;
    if (strcmp(entry->name, "..") == 0)
    {
        if (gLocalDepth != 0)
            --gLocalDepth;
    }
    else
    {
        if (gLocalDepth >= RL_MAX_PATH_COMPONENTS)
        {
            set_status("The LOCAL path is already at maximum depth.");
            return;
        }
        strcpy(gLocalComponents[gLocalDepth++], entry->name);
    }
    gLocalSelection = 0;
    reload_local_entries();
    draw_ui();
}

static void open_remote_selection(void)
{
    RlDirectoryEntry *entry;
    if (!gConnected || gRemoteSelection >= gRemoteCount)
        return;
    entry = &gRemoteEntries[gRemoteSelection];
    if (gRemoteAtVolumes)
    {
        if (gRemoteSelection >= gPeer.root_count)
            return;
        gRemoteRootIndex = gRemoteSelection;
        gRemoteAtVolumes = 0;
        gRemoteDepth = 0;
        gRemoteSelection = 0;
        request_remote_list();
        draw_ui();
        return;
    }
    if (is_remote_parent(entry))
    {
        go_parent();
        return;
    }
    if (entry->kind != 1)
        return;
    if (gRemoteDepth >= RL_MAX_PATH_COMPONENTS)
    {
        set_status("The REMOTE path is already at maximum depth.");
        return;
    }
    strcpy(gRemoteComponents[gRemoteDepth++], entry->name);
    gRemoteSelection = 0;
    request_remote_list();
    draw_ui();
}

static void go_parent(void)
{
    if (gActivePanel == 0)
    {
        if (gLocalDepth != 0)
        {
            --gLocalDepth;
            gLocalSelection = 0;
            reload_local_entries();
        }
    }
    else if (gConnected && move_remote_parent())
    {
        if (!gRemoteAtVolumes)
            request_remote_list();
        else
            set_status("Select a REMOTE volume.");
    }
    draw_ui();
}

static int append_protocol_path(
    rl_u16 *offset,
    rl_u32 provider_id,
    const char *const *components,
    rl_u8 component_count)
{
    rl_u8 index;

    if (offset == 0 || component_count > RL_MAX_PATH_COMPONENTS ||
        (rl_u16)(*offset + 5) > RL_MAX_PAYLOAD)
        return 0;
    write_u32(gPayload + *offset, provider_id);
    *offset = (rl_u16)(*offset + 4);
    gPayload[(*offset)++] = component_count;
    for (index = 0; index < component_count; ++index)
    {
        unsigned int length;
        if (!safe_path_component(components[index]))
            return 0;
        length = (unsigned int)strlen(components[index]);
        if (length > RL_MAX_NAME_BYTES ||
            (rl_u16)(*offset + length + 1) > RL_MAX_PAYLOAD)
            return 0;
        gPayload[(*offset)++] = (rl_u8)length;
        memcpy(gPayload + *offset, components[index], length);
        *offset = (rl_u16)(*offset + length);
    }
    return 1;
}

static int prompt_dos_name(const char *prompt, char name[RL_MAX_NAME_BYTES + 1])
{
    int key;
    unsigned int length;

    console_gotoxy(1, 24);
    cprintf("%-79.79s", "");
    console_gotoxy(1, 24);
    cprintf("%s", prompt);
    length = 0;
    for (;;)
    {
        key = getch();
        if (key == 27)
        {
            name[0] = '\0';
            return 0;
        }
        if (key == 13)
            break;
        if (key == 8)
        {
            if (length != 0)
            {
                --length;
                name[length] = '\0';
                cprintf("\b \b");
            }
            continue;
        }
        if (key >= 0x20 && key <= 0x7E &&
            length < RL_MAX_NAME_BYTES)
        {
            name[length++] = (char)key;
            name[length] = '\0';
            putch(key);
        }
    }
    name[length] = '\0';
    if (!safe_dos_83_name(name))
    {
        set_status("Use a LOCAL 8.3 name with letters, digits, _ - $ or ~.");
        return 0;
    }
    return 1;
}

static int build_local_selected_path(
    char path[160],
    const RlDirectoryEntry *entry)
{
    if (entry == 0 ||
        !build_dos_path(path, 160, gLocalComponents, gLocalDepth))
        return 0;
    return append_dos_path(path, 160, entry->name);
}

static int perform_local_operation(rl_u8 operation_kind, const char *name)
{
    char path[160];
    unsigned int attributes;
    int result;
    RlDirectoryEntry *entry;

    if (operation_kind == RL_MAKE_DIRECTORY)
    {
        if (!build_dos_path(path, sizeof(path), gLocalComponents, gLocalDepth) ||
            !append_dos_path(path, sizeof(path), name))
            return 0;
        if (get_dos_attributes(path, &attributes))
        {
            set_status("That LOCAL name already exists.");
            return 0;
        }
        errno = 0;
        result = _mkdir(path);
    }
    else
    {
        if (gLocalSelection >= gLocalCount)
            return 0;
        entry = &gLocalEntries[gLocalSelection];
        if (strcmp(entry->name, "..") == 0)
        {
            set_status("Select a file or folder, not the parent entry.");
            return 0;
        }
        if (!build_local_selected_path(path, entry))
        {
            set_status("The selected LOCAL path is too long.");
            return 0;
        }
        if (operation_kind == RL_RENAME)
        {
            char destination[160];
            if (!build_dos_path(
                    destination,
                    sizeof(destination),
                    gLocalComponents,
                    gLocalDepth) ||
                !append_dos_path(destination, sizeof(destination), name))
            {
                set_status("The destination LOCAL path is too long.");
                return 0;
            }
            if (get_dos_attributes(destination, &attributes))
            {
                set_status("That LOCAL name already exists.");
                return 0;
            }
            errno = 0;
            result = rename(path, destination);
        }
        else
        {
            if (!get_dos_attributes(path, &attributes))
            {
                set_status("The selected LOCAL entry no longer exists.");
                return 0;
            }
            if ((attributes & DOS_DIRECTORY_ATTRIBUTE) != 0)
            {
                if (!is_dos_directory_empty(path))
                {
                    set_status("Only empty LOCAL folders can be deleted.");
                    return 0;
                }
                errno = 0;
                result = _rmdir(path);
            }
            else
            {
                errno = 0;
                result = remove(path);
            }
        }
    }
    if (result != 0)
    {
        set_status("LOCAL filesystem operation failed.");
        return 0;
    }
    reload_local_entries();
    set_status("LOCAL filesystem operation completed.");
    return 1;
}

static int perform_remote_operation(
    rl_u8 operation_kind,
    const char *name)
{
    const char *current_components[RL_MAX_PATH_COMPONENTS];
    const char *selected_components[RL_MAX_PATH_COMPONENTS];
    const RlDirectoryEntry *entry;
    rl_u16 offset;
    rl_u16 payload_length;
    rl_u32 request_id;
    rl_u8 index;

    if (!gConnected || gRemoteAtVolumes || !ensure_active())
    {
        set_status("Connect and open a REMOTE volume before changing it.");
        return 0;
    }
    fill_component_pointers(
        gRemoteComponents,
        gRemoteDepth,
        current_components);
    offset = 0;
    write_u32(gPayload, gRoleEpoch);
    memcpy(gPayload + 4, gEndpointId, RL_SESSION_ID_SIZE);
    offset = 20;

    if (operation_kind == RL_MAKE_DIRECTORY)
    {
        if (!append_protocol_path(
                &offset,
                gPeer.roots[gRemoteRootIndex].provider_id,
                current_components,
                gRemoteDepth))
            return 0;
    }
    else
    {
        if (gRemoteSelection >= gRemoteCount)
            return 0;
        entry = &gRemoteEntries[gRemoteSelection];
        if (is_remote_parent(entry))
        {
            set_status("Select a file or folder, not the parent entry.");
            return 0;
        }
        if (gRemoteDepth >= RL_MAX_PATH_COMPONENTS)
        {
            set_status("The selected REMOTE path exceeds the protocol limit.");
            return 0;
        }
        for (index = 0; index < gRemoteDepth; ++index)
            selected_components[index] = gRemoteComponents[index];
        selected_components[gRemoteDepth] = entry->name;
        if (operation_kind == RL_RENAME)
        {
            if (!append_protocol_path(
                    &offset,
                    gPeer.roots[gRemoteRootIndex].provider_id,
                    selected_components,
                    (rl_u8)(gRemoteDepth + 1)) ||
                !append_protocol_path(
                    &offset,
                    gPeer.roots[gRemoteRootIndex].provider_id,
                    current_components,
                    gRemoteDepth))
                return 0;
        }
        else if (!append_protocol_path(
                     &offset,
                     gPeer.roots[gRemoteRootIndex].provider_id,
                     selected_components,
                     (rl_u8)(gRemoteDepth + 1)))
            return 0;
    }

    if (operation_kind == RL_DELETE)
    {
        if ((rl_u16)(offset + 1) > RL_MAX_PAYLOAD)
            return 0;
        gPayload[offset++] = 0;
    }
    else
    {
        unsigned int name_length;
        if (!safe_dos_83_name(name))
            return 0;
        name_length = (unsigned int)strlen(name);
        if ((rl_u16)(offset + name_length + 1) > RL_MAX_PAYLOAD)
            return 0;
        gPayload[offset++] = (rl_u8)name_length;
        memcpy(gPayload + offset, name, name_length);
        offset = (rl_u16)(offset + name_length);
    }

    gOperationReady = 0;
    gOperationResultCode = 2;
    request_id = gNextRequestId++;
    gPendingOperationRequest = request_id;
    gPendingOperationKind = operation_kind;
    payload_length = offset;
    if (!send_link_frame(
            operation_kind,
            request_id,
            gPayload,
            payload_length))
    {
        gPendingOperationRequest = 0;
        set_status("Could not send the REMOTE filesystem operation.");
        return 0;
    }
    set_status("Waiting for the REMOTE filesystem operation...");
    if (!wait_for_flag(&gOperationReady, 10))
    {
        gPendingOperationRequest = 0;
        set_status("Timeout: REMOTE did not complete the filesystem operation.");
        return 0;
    }
    gPendingOperationRequest = 0;
    if (gOperationResultCode != 0)
    {
        set_status("The REMOTE filesystem operation failed.");
        return 0;
    }
    if (!request_remote_list())
        return 0;
    set_status("REMOTE filesystem operation completed.");
    return 1;
}

static void run_filesystem_operation(rl_u8 operation_kind)
{
    char name[RL_MAX_NAME_BYTES + 1];
    int accepted;

    name[0] = '\0';
    if (gActivePanel != 0 &&
        (!gConnected || gRemoteAtVolumes))
    {
        set_status("Connect and open a REMOTE volume before changing it.");
        draw_ui();
        return;
    }
    if (operation_kind != RL_DELETE &&
        !prompt_dos_name(
            operation_kind == RL_MAKE_DIRECTORY
                ? (gActivePanel == 0
                    ? "New LOCAL folder 8.3 name (Esc cancels): "
                    : "New REMOTE folder name (Esc cancels): ")
                : (gActivePanel == 0
                    ? "New LOCAL entry 8.3 name (Esc cancels): "
                    : "New REMOTE entry name (Esc cancels): "),
            name))
    {
        draw_ui();
        return;
    }
    if (operation_kind == RL_DELETE)
    {
        const RlDirectoryEntry *entry;
        if (gActivePanel == 0)
        {
            if (gLocalSelection >= gLocalCount)
            {
                draw_ui();
                return;
            }
            entry = &gLocalEntries[gLocalSelection];
        }
        else
        {
            if (gRemoteSelection >= gRemoteCount)
            {
                draw_ui();
                return;
            }
            entry = &gRemoteEntries[gRemoteSelection];
        }
        if (strcmp(entry->name, "..") == 0)
        {
            set_status("Select a file or folder, not the parent entry.");
            draw_ui();
            return;
        }
        console_gotoxy(1, 24);
        cprintf("%-79.79s", "");
        console_gotoxy(1, 24);
        cprintf("Delete %s? (Y/N): ", entry->name);
        accepted = getch();
        if (accepted != 'Y' && accepted != 'y')
        {
            set_status("Delete cancelled.");
            draw_ui();
            return;
        }
    }
    if (gActivePanel == 0)
        perform_local_operation(operation_kind, name);
    else
        perform_remote_operation(operation_kind, name);
    draw_ui();
}

static void change_baud_rate(void)
{
    if (gBaudRate == 9600)
        gBaudRate = 19200;
    else if (gBaudRate == 19200)
        gBaudRate = 38400;
    else if (gBaudRate == 38400)
        gBaudRate = 57600;
    else
        gBaudRate = 9600;

    dos_uart_open(&gDebugUart, 1, gBaudRate);
    dos_uart_open(&gLinkUart, 2, gBaudRate);
    set_status("Set REMOTE to the same baud rate, then reconnect with F3.");
    gConnected = 0;
    gPeerReady = 0;
    gListReady = 0;
    gRemoteCount = 0;
    gRemoteAtVolumes = 0;
    draw_ui();
}

static void handle_key(void)
{
    int key;
    int extended;
    rl_u8 *selection;
    rl_u8 count;

    key = getch();
    extended = 0;
    if (key == 0 || key == 0xE0)
    {
        key = getch();
        extended = 1;
    }
    if (extended)
    {
        if (key == 0x44)
            exit(0);
        if (key == 0x3C)
        {
            change_baud_rate();
            return;
        }
        if (key == 0x3D)
        {
            if (connect_peer())
            {
                if (gPeer.root_count != 0)
                {
                    show_remote_volumes();
                    set_status("Connected to REMOTE. Select a volume to browse.");
                }
                else
                    set_status("Connected, but REMOTE advertised no mounted volumes.");
            }
            draw_ui();
            return;
        }
        if (key == 0x3F)
        {
            send_marked_files();
            draw_ui();
            return;
        }
        if (key == 0x41)
        {
            run_filesystem_operation(RL_MAKE_DIRECTORY);
            return;
        }
        if (key == 0x42)
        {
            run_filesystem_operation(RL_DELETE);
            return;
        }
        if (key == 0x48)
        {
            selection = gActivePanel == 0 ? &gLocalSelection : &gRemoteSelection;
            count = gActivePanel == 0 ? gLocalCount : gRemoteCount;
            if (*selection != 0)
                --*selection;
            else if (count != 0)
                *selection = (rl_u8)(count - 1);
            draw_ui();
            return;
        }
        if (key == 0x50)
        {
            selection = gActivePanel == 0 ? &gLocalSelection : &gRemoteSelection;
            count = gActivePanel == 0 ? gLocalCount : gRemoteCount;
            if (count != 0)
                *selection = (rl_u8)((*selection + 1) % count);
            draw_ui();
            return;
        }
        if (key == 0x52)
        {
            toggle_local_mark();
            draw_ui();
            return;
        }
        if (key == 0x4B || key == 0x4D)
        {
            gActivePanel = (rl_u8)!gActivePanel;
            draw_ui();
            return;
        }
        return;
    }

    if (key == 9)
    {
        gActivePanel = (rl_u8)!gActivePanel;
        draw_ui();
    }
    else if (key == 13)
    {
        if (gActivePanel == 0)
            open_local_selection();
        else
            open_remote_selection();
    }
    else if (key == 8)
    {
        go_parent();
    }
    else if (key == 'r' || key == 'R')
    {
        run_filesystem_operation(RL_RENAME);
    }
}

static int remote_navigation_selftest(void)
{
    memset(&gPeer, 0, sizeof(gPeer));
    gPeer.root_count = 2;
    strcpy(gPeer.roots[0].name, "System");
    strcpy(gPeer.roots[1].name, "Work");
    show_remote_volumes();
    if (!gRemoteAtVolumes || gRemoteCount != 2 ||
        strcmp(gRemoteEntries[0].name, "System") != 0 ||
        strcmp(gRemoteEntries[1].name, "Work") != 0)
    {
        return 0;
    }

    gRemoteAtVolumes = 0;
    gRemoteDepth = 2;
    if (!move_remote_parent() || gRemoteAtVolumes || gRemoteDepth != 1 ||
        !move_remote_parent() || gRemoteAtVolumes || gRemoteDepth != 0 ||
        !move_remote_parent() || !gRemoteAtVolumes || gRemoteDepth != 0 ||
        move_remote_parent())
    {
        return 0;
    }

    gRemoteAtVolumes = 0;
    gRemoteDepth = 1;
    gRemoteCount = 2;
    memset(gRemoteEntries, 0, sizeof(gRemoteEntries));
    strcpy(gRemoteEntries[0].name, "Folder");
    strcpy(gRemoteEntries[1].name, "Readme");
    gRemoteEntries[0].kind = 1;
    gRemoteEntries[1].kind = 0;
    prepend_remote_parent();
    return gRemoteCount == 3 &&
        is_remote_parent(&gRemoteEntries[0]) &&
        strcmp(gRemoteEntries[1].name, "Folder") == 0 &&
        strcmp(gRemoteEntries[2].name, "Readme") == 0;
}

static int file_selection_selftest(void)
{
    rl_u8 indices[RL_MAX_LIST_ENTRIES];
    rl_u8 count;

    memset(gLocalEntries, 0, sizeof(gLocalEntries));
    memset(gLocalMarked, 0, sizeof(gLocalMarked));
    gLocalCount = 3;
    gLocalEntries[0].kind = 0;
    gLocalEntries[1].kind = 1;
    gLocalEntries[2].kind = 0;
    gLocalMarked[0] = 1;
    gLocalMarked[1] = 1;
    gLocalMarked[2] = 1;
    count = collect_marked_files(indices);
    if (count != 2 || indices[0] != 0 || indices[1] != 2)
        return 0;
    return 1;
}

static int run_selftest(void)
{
    static const rl_u8 sample[] = "123456789";
    RlFrame input;
    RlFrame output;
    RlFrameDecoder decoder;
    rl_u8 encoded[RL_FRAME_BUFFER_SIZE];
    rl_u16 encoded_length;
    rl_u16 index;
    int decoded;

    if (rl_crc16(sample, 9) != 0x29B1 ||
        rl_crc16_xmodem(sample, 9) != 0x31C3 ||
        rl_crc32(sample, 9) != 0xCBF43926UL)
    {
        printf("RETROLINK DOS SELFTEST FAIL: checksum vector\n");
        return 1;
    }
    if (!remote_navigation_selftest())
    {
        printf("RETROLINK DOS SELFTEST FAIL: remote parent navigation\n");
        return 1;
    }
    if (!file_selection_selftest())
    {
        printf("RETROLINK DOS SELFTEST FAIL: marked file selection\n");
        return 1;
    }
    memset(&input, 0, sizeof(input));
    memset(&decoder, 0, sizeof(decoder));
    input.message_type = RL_HELLO;
    input.request_id = 0x12345678UL;
    input.payload_length = 1;
    input.payload[0] = 1;
    for (index = 0; index < RL_SESSION_ID_SIZE; ++index)
        input.session_id[index] = (rl_u8)index;
    encoded_length = rl_encode_frame(&input, encoded);
    if (encoded_length == 0)
    {
        printf("RETROLINK DOS SELFTEST FAIL: frame encoding\n");
        return 1;
    }
    decoded = 0;
    for (index = 0; index < encoded_length; ++index)
    {
        if (rl_decode_frame_byte(&decoder, encoded[index], &output))
            decoded++;
    }
    if (decoded != 1 || output.message_type != input.message_type ||
        output.request_id != input.request_id ||
        output.payload_length != input.payload_length ||
        memcmp(output.session_id, input.session_id, RL_SESSION_ID_SIZE) != 0 ||
        output.payload[0] != input.payload[0])
    {
        printf("RETROLINK DOS SELFTEST FAIL: frame decoding\n");
        return 1;
    }
    printf("RETROLINK DOS SELFTEST PASS\n");
    return 0;
}

static int parse_baud_rate(const char *text, rl_u32 *baud)
{
    unsigned long value;
    if (text == 0 || baud == 0)
        return 0;
    value = strtoul(text, 0, 10);
    if (value != 9600 && value != 19200 &&
        value != 38400 && value != 57600)
    {
        return 0;
    }
    *baud = (rl_u32)value;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "/SELFTEST") == 0)
        return run_selftest();
    if (argc > 1 && !parse_baud_rate(argv[1], &gBaudRate))
    {
        printf("Usage: RETROLINK [9600|19200|38400|57600]\n");
        return 2;
    }
    if (!dos_uart_open(&gDebugUart, 1, gBaudRate))
    {
        printf("COM1 is unavailable for the IDE Agent debug link.\n");
        return 1;
    }
    if (!dos_uart_open(&gLinkUart, 2, gBaudRate))
    {
        printf("COM2 is unavailable for the RetroLink file link.\n");
        return 1;
    }

    agent_link_init(&gAgent, &gDebugUart);
    agent_link_log(&gAgent, 0, "DOSBox 8088 debug monitor started");
    memset(&gDecoder, 0, sizeof(gDecoder));
    gStatus[0] = '\0';
    reload_local_entries();
    console_clear();
    set_status("Connecting to REMOTE on COM2...");
    if (connect_peer())
    {
        if (gPeer.root_count != 0)
        {
            show_remote_volumes();
            set_status("Connected to REMOTE. Select a volume to browse.");
        }
        else
            set_status("Connected, but REMOTE advertised no mounted volumes.");
    }
    else if (gStatus[0] == '\0')
    {
        set_status("Press F3 to retry the REMOTE connection.");
    }
    draw_ui();

    for (;;)
    {
        pump();
        if (kbhit())
            handle_key();
        dos_idle();
    }
}
