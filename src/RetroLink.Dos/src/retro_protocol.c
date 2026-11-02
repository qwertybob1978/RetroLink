#include <string.h>

#include "retro_protocol.h"

#define RL_TRANSFER_FORMAT_RAW 0
#define RL_TRANSFER_MODE_XMODEM_CRC 1
#define RL_TRANSFER_BLOCK_SIZE 128
#define RL_LIST_PAGE_HEADER_SIZE 11
#define RL_LIST_ENTRY_FIXED_SIZE 20

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

static int valid_component(const char *component, int allow_slash)
{
    unsigned int length;
    unsigned int index;

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
        if (value < 0x20 || value > 0x7E || value == ':' ||
            value == '\\' || (!allow_slash && value == '/'))
        {
            return 0;
        }
    }
    return 1;
}

static rl_u16 write_path(
    rl_u8 *payload,
    rl_u16 offset,
    rl_u32 provider_id,
    const char *const *components,
    rl_u8 component_count,
    int allow_slash)
{
    rl_u8 index;

    if (provider_id == 0 || component_count > RL_MAX_PATH_COMPONENTS ||
        (component_count != 0 && components == 0))
        return 0;
    write_u32(payload + offset, provider_id);
    payload[offset + 4] = component_count;
    offset = (rl_u16)(offset + 5);
    for (index = 0; index < component_count; ++index)
    {
        rl_u8 length;
        if (!valid_component(components[index], allow_slash))
            return 0;
        length = (rl_u8)strlen(components[index]);
        payload[offset++] = length;
        memcpy(payload + offset, components[index], length);
        offset = (rl_u16)(offset + length);
        if (offset > RL_MAX_PAYLOAD)
            return 0;
    }
    return offset;
}

static int read_path(
    const rl_u8 *payload,
    rl_u16 length,
    rl_u16 *offset,
    rl_u32 *provider_id,
    char components[RL_MAX_PATH_COMPONENTS][RL_MAX_NAME_BYTES + 1],
    rl_u8 *component_count,
    int allow_slash)
{
    rl_u8 count;
    rl_u8 index;

    if (*offset > length || length - *offset < 5)
        return 0;
    *provider_id = read_u32(payload + *offset);
    count = payload[*offset + 4];
    *offset = (rl_u16)(*offset + 5);
    if (*provider_id == 0 || count > RL_MAX_PATH_COMPONENTS)
        return 0;
    if (allow_slash < 0)
        allow_slash = *provider_id != RL_DOS_PROVIDER_ID;

    for (index = 0; index < count; ++index)
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
        if (!valid_component(components[index], allow_slash))
            return 0;
        *offset = (rl_u16)(*offset + name_length);
    }
    *component_count = count;
    return 1;
}

int rl_parse_hello_ack(const rl_u8 *payload, rl_u16 length, RlPeerInfo *peer)
{
    rl_u16 offset;
    rl_u8 root_index;

    if (payload == 0 || peer == 0 || length < 37 ||
        payload[0] != 1 || payload[36] > RL_MAX_ROOTS)
    {
        return 0;
    }

    memset(peer, 0, sizeof(*peer));
    peer->version = payload[0];
    peer->app_major = payload[1];
    peer->app_minor = payload[2];
    peer->app_patch = payload[3];
    peer->system_major = payload[4];
    peer->system_minor = payload[5];
    peer->machine_type = read_u16(payload + 6);
    memcpy(peer->endpoint_id, payload + 8, RL_SESSION_ID_SIZE);
    peer->capabilities = read_u32(payload + 24);
    peer->baud_rate = read_u32(payload + 28);
    peer->free_bytes = read_u32(payload + 32);
    peer->root_count = payload[36];
    offset = 37;

    for (root_index = 0; root_index < peer->root_count; ++root_index)
    {
        rl_u8 name_length;
        RlRoot *root = &peer->roots[root_index];
        if (length - offset < 5)
            return 0;
        root->provider_id = read_u32(payload + offset);
        name_length = payload[offset + 4];
        offset = (rl_u16)(offset + 5);
        if (root->provider_id == 0 || name_length > 64 ||
            length - offset < (rl_u16)(name_length + 1))
        {
            return 0;
        }
        memcpy(root->name, payload + offset, name_length);
        root->name[name_length] = '\0';
        offset = (rl_u16)(offset + name_length);
        root->read_only = payload[offset++];
        if (root->read_only > 1)
            return 0;
    }
    return offset == length;
}

rl_u16 rl_make_list_request(
    rl_u8 *payload,
    rl_u32 role_epoch,
    const rl_u8 active_endpoint[RL_SESSION_ID_SIZE],
    rl_u32 provider_id,
    const char *const *components,
    rl_u8 component_count,
    rl_u32 cursor,
    rl_u16 entry_limit)
{
    rl_u16 offset;

    if (payload == 0 || active_endpoint == 0 ||
        entry_limit == 0 || entry_limit > RL_MAX_LIST_ENTRIES)
    {
        return 0;
    }
    write_u32(payload, role_epoch);
    memcpy(payload + 4, active_endpoint, RL_SESSION_ID_SIZE);
    offset = write_path(
        payload,
        4 + RL_SESSION_ID_SIZE,
        provider_id,
        components,
        component_count,
        provider_id != RL_DOS_PROVIDER_ID);
    if (offset == 0 || offset + 6 > RL_MAX_PAYLOAD)
        return 0;
    write_u32(payload + offset, cursor);
    write_u16(payload + offset + 4, entry_limit);
    return (rl_u16)(offset + 6);
}

int rl_parse_list_page(const rl_u8 *payload, rl_u16 length, RlListPage *page)
{
    rl_u16 offset;
    rl_u8 index;

    if (payload == 0 || page == 0 ||
        length < RL_LIST_PAGE_HEADER_SIZE || length > RL_MAX_PAYLOAD)
    {
        return 0;
    }
    page->result_code = read_u16(payload);
    page->cursor = read_u32(payload + 2);
    page->next_cursor = read_u32(payload + 6);
    page->entry_count = payload[10];
    if (page->entry_count > RL_MAX_LIST_ENTRIES ||
        (page->result_code != 0 && page->entry_count != 0))
    {
        return 0;
    }
    offset = RL_LIST_PAGE_HEADER_SIZE;

    for (index = 0; index < page->entry_count; ++index)
    {
        rl_u8 name_length;
        rl_u32 high_size;
        RlDirectoryEntry *entry = &page->entries[index];
        if (length - offset < 4)
            return 0;
        entry->kind = payload[offset];
        entry->attributes = read_u16(payload + offset + 1);
        name_length = payload[offset + 3];
        offset = (rl_u16)(offset + 4);
        if ((entry->kind != 0 && entry->kind != 1) ||
            name_length == 0 || name_length > RL_MAX_NAME_BYTES ||
            length - offset < (rl_u16)(name_length + 16))
        {
            return 0;
        }
        memcpy(entry->name, payload + offset, name_length);
        entry->name[name_length] = '\0';
        offset = (rl_u16)(offset + name_length);
        high_size = read_u32(payload + offset);
        entry->data_size = read_u32(payload + offset + 4);
        if (high_size != 0)
            entry->data_size = 0xFFFFFFFFUL;
        entry->resource_size = read_u32(payload + offset + 8);
        entry->modified_time = read_u32(payload + offset + 12);
        offset = (rl_u16)(offset + 16);
    }
    return offset == length;
}

rl_u16 rl_make_list_page(
    rl_u8 *payload,
    rl_u16 result_code,
    rl_u32 cursor,
    rl_u32 next_cursor,
    const RlDirectoryEntry *entries,
    rl_u8 entry_count)
{
    rl_u16 offset;
    rl_u8 index;

    if (payload == 0 || entry_count > RL_MAX_LIST_ENTRIES ||
        (result_code != 0 && entry_count != 0) ||
        (entry_count != 0 && entries == 0))
    {
        return 0;
    }
    write_u16(payload, result_code);
    write_u32(payload + 2, cursor);
    write_u32(payload + 6, next_cursor);
    payload[10] = entry_count;
    offset = RL_LIST_PAGE_HEADER_SIZE;
    for (index = 0; index < entry_count; ++index)
    {
        rl_u8 name_length;
        const RlDirectoryEntry *entry = &entries[index];
        if ((entry->kind != 0 && entry->kind != 1) ||
            !valid_component(entry->name, 0))
        {
            return 0;
        }
        name_length = (rl_u8)strlen(entry->name);
        if (offset + RL_LIST_ENTRY_FIXED_SIZE + name_length > RL_MAX_PAYLOAD)
            return 0;
        payload[offset] = entry->kind;
        write_u16(payload + offset + 1, entry->attributes);
        payload[offset + 3] = name_length;
        memcpy(payload + offset + 4, entry->name, name_length);
        offset = (rl_u16)(offset + 4 + name_length);
        write_u32(payload + offset, 0);
        write_u32(payload + offset + 4, entry->data_size);
        write_u32(payload + offset + 8, entry->resource_size);
        write_u32(payload + offset + 12, entry->modified_time);
        offset = (rl_u16)(offset + 16);
    }
    return offset;
}

rl_u16 rl_make_role_request(
    rl_u8 *payload,
    const rl_u8 requester_endpoint[RL_SESSION_ID_SIZE],
    rl_u32 expected_epoch,
    rl_u8 reason)
{
    if (payload == 0 || requester_endpoint == 0 || reason > 2)
        return 0;
    memcpy(payload, requester_endpoint, RL_SESSION_ID_SIZE);
    write_u32(payload + RL_SESSION_ID_SIZE, expected_epoch);
    payload[RL_SESSION_ID_SIZE + 4] = reason;
    return RL_SESSION_ID_SIZE + 5;
}

rl_u16 rl_make_copy_prepare(
    rl_u8 *payload,
    rl_u32 role_epoch,
    const rl_u8 active_endpoint[RL_SESSION_ID_SIZE],
    const rl_u8 source_endpoint[RL_SESSION_ID_SIZE],
    rl_u32 source_provider,
    const char *const *source_components,
    rl_u8 source_component_count,
    const rl_u8 destination_endpoint[RL_SESSION_ID_SIZE],
    rl_u32 destination_provider,
    const char *const *destination_components,
    rl_u8 destination_component_count,
    const char *destination_name,
    rl_u32 stream_length,
    rl_u32 stream_crc32,
    rl_u8 overwrite)
{
    rl_u16 offset;
    rl_u8 name_length;

    if (payload == 0 || active_endpoint == 0 || source_endpoint == 0 ||
        destination_endpoint == 0 ||
        !valid_component(
            destination_name,
            destination_provider != RL_DOS_PROVIDER_ID) ||
        stream_length > 0x7FFFFFFFUL || overwrite > 1)
    {
        return 0;
    }
    name_length = (rl_u8)strlen(destination_name);
    write_u32(payload, role_epoch);
    memcpy(payload + 4, active_endpoint, RL_SESSION_ID_SIZE);
    memcpy(payload + 4 + RL_SESSION_ID_SIZE, source_endpoint, RL_SESSION_ID_SIZE);
    offset = write_path(
        payload,
        4 + RL_SESSION_ID_SIZE * 2,
        source_provider,
        source_components,
        source_component_count,
        source_provider != RL_DOS_PROVIDER_ID);
    if (offset == 0)
        return 0;
    memcpy(payload + offset, destination_endpoint, RL_SESSION_ID_SIZE);
    offset = (rl_u16)(offset + RL_SESSION_ID_SIZE);
    offset = write_path(
        payload,
        offset,
        destination_provider,
        destination_components,
        destination_component_count,
        destination_provider != RL_DOS_PROVIDER_ID);
    if (offset == 0 || offset + name_length + 16 > RL_MAX_PAYLOAD ||
        !valid_component(
            destination_name,
            destination_provider != RL_DOS_PROVIDER_ID))
        return 0;
    payload[offset++] = name_length;
    memcpy(payload + offset, destination_name, name_length);
    offset = (rl_u16)(offset + name_length);
    memset(payload + offset, 0, 4);
    write_u32(payload + offset + 4, stream_length);
    write_u32(payload + offset + 8, stream_crc32);
    payload[offset + 12] = RL_TRANSFER_FORMAT_RAW;
    payload[offset + 13] = overwrite;
    payload[offset + 14] = RL_TRANSFER_MODE_XMODEM_CRC;
    return (rl_u16)(offset + 15);
}

int rl_parse_copy_prepare(
    const rl_u8 *payload,
    rl_u16 length,
    rl_u32 *role_epoch,
    rl_u8 active_endpoint[RL_SESSION_ID_SIZE],
    rl_u8 source_endpoint[RL_SESSION_ID_SIZE],
    rl_u32 *source_provider,
    char source_components[RL_MAX_PATH_COMPONENTS][RL_MAX_NAME_BYTES + 1],
    rl_u8 *source_component_count,
    rl_u8 destination_endpoint[RL_SESSION_ID_SIZE],
    rl_u32 *destination_provider,
    char destination_components[RL_MAX_PATH_COMPONENTS][RL_MAX_NAME_BYTES + 1],
    rl_u8 *destination_component_count,
    char destination_name[RL_MAX_NAME_BYTES + 1],
    rl_u32 *stream_length,
    rl_u32 *stream_crc32,
    rl_u8 *overwrite)
{
    rl_u16 offset;
    rl_u8 name_length;
    rl_u32 high_length;

    if (payload == 0 || role_epoch == 0 || active_endpoint == 0 ||
        source_endpoint == 0 || source_provider == 0 ||
        source_components == 0 || source_component_count == 0 ||
        destination_endpoint == 0 || destination_provider == 0 ||
        destination_components == 0 || destination_component_count == 0 ||
        destination_name == 0 || stream_length == 0 ||
        stream_crc32 == 0 || overwrite == 0 ||
        length < 4 + RL_SESSION_ID_SIZE * 2)
    {
        return 0;
    }
    *role_epoch = read_u32(payload);
    memcpy(active_endpoint, payload + 4, RL_SESSION_ID_SIZE);
    memcpy(source_endpoint, payload + 4 + RL_SESSION_ID_SIZE, RL_SESSION_ID_SIZE);
    offset = 4 + RL_SESSION_ID_SIZE * 2;
    if (!read_path(
            payload,
            length,
            &offset,
            source_provider,
            source_components,
            source_component_count,
            -1) ||
        length - offset < RL_SESSION_ID_SIZE)
    {
        return 0;
    }
    memcpy(destination_endpoint, payload + offset, RL_SESSION_ID_SIZE);
    offset = (rl_u16)(offset + RL_SESSION_ID_SIZE);
    if (!read_path(
            payload,
            length,
            &offset,
            destination_provider,
            destination_components,
            destination_component_count,
            -1) ||
        offset >= length)
    {
        return 0;
    }
    name_length = payload[offset++];
    if (name_length == 0 || name_length > RL_MAX_NAME_BYTES ||
        length - offset != (rl_u16)(name_length + 15))
    {
        return 0;
    }
    memcpy(destination_name, payload + offset, name_length);
    destination_name[name_length] = '\0';
    if (!valid_component(
            destination_name,
            *destination_provider != RL_DOS_PROVIDER_ID))
        return 0;
    offset = (rl_u16)(offset + name_length);
    high_length = read_u32(payload + offset);
    *stream_length = read_u32(payload + offset + 4);
    *stream_crc32 = read_u32(payload + offset + 8);
    if (high_length != 0 || *stream_length > 0x7FFFFFFFUL ||
        payload[offset + 12] != RL_TRANSFER_FORMAT_RAW ||
        payload[offset + 14] != RL_TRANSFER_MODE_XMODEM_CRC ||
        payload[offset + 13] > 1)
    {
        return 0;
    }
    *overwrite = payload[offset + 13];
    return 1;
}
