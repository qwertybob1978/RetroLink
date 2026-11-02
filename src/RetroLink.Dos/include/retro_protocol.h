#ifndef RETROLINK_DOS_PROTOCOL_H
#define RETROLINK_DOS_PROTOCOL_H

#include "wire.h"

#define RL_DOS_PROVIDER_ID 0x80000001UL
#define RL_MAX_PATH_COMPONENTS 16
#define RL_MAX_NAME_BYTES 63
#define RL_MAX_ROOTS 8
#define RL_MAX_LIST_ENTRIES 32
#define RL_LIST_COMPLETE_CURSOR 0xFFFFFFFFUL

typedef struct
{
    rl_u32 provider_id;
    char name[65];
    rl_u8 read_only;
} RlRoot;

typedef struct
{
    rl_u8 kind;
    rl_u16 attributes;
    char name[RL_MAX_NAME_BYTES + 1];
    rl_u32 data_size;
    rl_u32 resource_size;
    rl_u32 modified_time;
} RlDirectoryEntry;

typedef struct
{
    rl_u8 version;
    rl_u8 app_major;
    rl_u8 app_minor;
    rl_u8 app_patch;
    rl_u8 system_major;
    rl_u8 system_minor;
    rl_u16 machine_type;
    rl_u8 endpoint_id[RL_SESSION_ID_SIZE];
    rl_u32 capabilities;
    rl_u32 baud_rate;
    rl_u32 free_bytes;
    rl_u8 root_count;
    RlRoot roots[RL_MAX_ROOTS];
} RlPeerInfo;

typedef struct
{
    rl_u16 result_code;
    rl_u32 cursor;
    rl_u32 next_cursor;
    rl_u8 entry_count;
    RlDirectoryEntry entries[RL_MAX_LIST_ENTRIES];
} RlListPage;

int rl_parse_hello_ack(const rl_u8 *payload, rl_u16 length, RlPeerInfo *peer);
rl_u16 rl_make_list_request(
    rl_u8 *payload,
    rl_u32 role_epoch,
    const rl_u8 active_endpoint[RL_SESSION_ID_SIZE],
    rl_u32 provider_id,
    const char *const *components,
    rl_u8 component_count,
    rl_u32 cursor,
    rl_u16 entry_limit);
int rl_parse_list_page(const rl_u8 *payload, rl_u16 length, RlListPage *page);
rl_u16 rl_make_list_page(
    rl_u8 *payload,
    rl_u16 result_code,
    rl_u32 cursor,
    rl_u32 next_cursor,
    const RlDirectoryEntry *entries,
    rl_u8 entry_count);
rl_u16 rl_make_role_request(
    rl_u8 *payload,
    const rl_u8 requester_endpoint[RL_SESSION_ID_SIZE],
    rl_u32 expected_epoch,
    rl_u8 reason);
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
    rl_u8 overwrite);
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
    rl_u8 *overwrite);

#endif
