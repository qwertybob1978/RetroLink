#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "retro_protocol.h"
#include "wire.h"

static void test_crc_vectors(void)
{
    static const rl_u8 sample[] = "123456789";
    assert(rl_crc16(sample, 9) == 0x29B1);
    assert(rl_crc16_xmodem(sample, 9) == 0x31C3);
    assert(rl_crc32(sample, 9) == 0xCBF43926UL);
    assert(rl_crc32_finish(rl_crc32_update(
        rl_crc32_update(0xFFFFFFFFUL, sample, 4),
        sample + 4,
        5)) == 0xCBF43926UL);
}

static void test_retrolink_frame(void)
{
    RlFrame input;
    RlFrame output;
    RlFrameDecoder decoder;
    rl_u8 encoded[RL_FRAME_BUFFER_SIZE];
    rl_u16 encoded_length;
    rl_u16 index;
    int decoded;

    memset(&input, 0, sizeof(input));
    memset(&decoder, 0, sizeof(decoder));
    input.message_type = 0x11;
    input.request_id = 0x12345678UL;
    for (index = 0; index < RL_SESSION_ID_SIZE; ++index)
        input.session_id[index] = (rl_u8)index;
    input.payload_length = 3;
    input.payload[0] = 'R';
    input.payload[1] = 'L';
    input.payload[2] = 'K';

    encoded_length = rl_encode_frame(&input, encoded);
    assert(encoded_length == RL_FRAME_HEADER_SIZE + 5);
    assert(memcmp(encoded, "RLNK", 4) == 0);
    assert(encoded[5] == 0x11);
    assert(encoded[24] == 0x12 && encoded[27] == 0x78);
    assert(encoded[28] == 0 && encoded[29] == 3);

    decoded = 0;
    for (index = 0; index < encoded_length; ++index)
    {
        if (rl_decode_frame_byte(&decoder, encoded[index], &output))
            decoded++;
    }
    assert(decoded == 1);
    assert(output.message_type == input.message_type);
    assert(output.request_id == input.request_id);
    assert(output.payload_length == input.payload_length);
    assert(memcmp(output.session_id, input.session_id, RL_SESSION_ID_SIZE) == 0);
    assert(memcmp(output.payload, input.payload, input.payload_length) == 0);

    memset(&decoder, 0, sizeof(decoder));
    encoded[encoded_length - 1] ^= 0x01;
    decoded = 0;
    for (index = 0; index < encoded_length; ++index)
    {
        if (rl_decode_frame_byte(&decoder, encoded[index], &output))
            decoded++;
    }
    assert(decoded == 0);
    encoded[encoded_length - 1] ^= 0x01;
    for (index = 0; index < encoded_length; ++index)
    {
        if (rl_decode_frame_byte(&decoder, encoded[index], &output))
            decoded++;
    }
    assert(decoded == 1);
    assert(output.request_id == input.request_id);
}

static void test_agent_frame(void)
{
    RlFrame input;
    RlFrame output;
    RlFrameDecoder decoder;
    rl_u8 encoded[RL_FRAME_BUFFER_SIZE];
    rl_u16 encoded_length;
    rl_u16 index;
    int decoded;

    memset(&input, 0, sizeof(input));
    memset(&decoder, 0, sizeof(decoder));
    input.message_type = 0x02;
    memset(input.session_id, 0xA5, RL_SESSION_ID_SIZE);
    input.payload_length = 2;
    input.payload[0] = 1;
    input.payload[1] = 0;
    encoded_length = rl_encode_agent_frame(&input, encoded);
    assert(encoded_length == RL_AGENT_HEADER_SIZE + 4);
    assert(memcmp(encoded, "MSAG", 4) == 0);
    assert(encoded[24] == 0 && encoded[25] == 2);

    decoded = 0;
    for (index = 0; index < encoded_length; ++index)
    {
        if (rl_decode_agent_byte(&decoder, encoded[index], &output))
            decoded++;
    }
    assert(decoded == 1);
    assert(output.message_type == input.message_type);
    assert(output.payload_length == input.payload_length);
    assert(memcmp(output.session_id, input.session_id, RL_SESSION_ID_SIZE) == 0);
    assert(memcmp(output.payload, input.payload, input.payload_length) == 0);
}

static void test_hello_ack(void)
{
    rl_u8 payload[47];
    RlPeerInfo peer;
    memset(payload, 0, sizeof(payload));
    payload[0] = 1;
    payload[1] = 2;
    payload[2] = 3;
    payload[3] = 4;
    payload[4] = 7;
    payload[5] = 1;
    payload[6] = 0x12;
    payload[7] = 0x34;
    payload[8] = 0xAA;
    payload[24] = 0;
    payload[25] = 0;
    payload[26] = 0;
    payload[27] = 0x41;
    payload[28] = 0;
    payload[29] = 0;
    payload[30] = 0x25;
    payload[31] = 0x80;
    payload[36] = 1;
    payload[37] = 0x01;
    payload[38] = 0x02;
    payload[39] = 0x03;
    payload[40] = 0x04;
    payload[41] = 4;
    memcpy(payload + 42, "Disk", 4);
    payload[46] = 0;

    assert(rl_parse_hello_ack(payload, sizeof(payload), &peer));
    assert(peer.app_major == 2 && peer.app_minor == 3);
    assert(peer.system_major == 7 && peer.system_minor == 1);
    assert(peer.machine_type == 0x1234);
    assert(peer.endpoint_id[0] == 0xAA);
    assert(peer.capabilities == 0x41);
    assert(peer.baud_rate == 9600);
    assert(peer.root_count == 1);
    assert(peer.roots[0].provider_id == 0x01020304UL);
    assert(strcmp(peer.roots[0].name, "Disk") == 0);
}

static void test_list_protocol(void)
{
    rl_u8 request[RL_MAX_PAYLOAD];
    rl_u8 page_bytes[RL_MAX_PAYLOAD];
    const char *path[] = { "Classic" };
    RlDirectoryEntry entry;
    RlListPage page;
    rl_u8 endpoint[RL_SESSION_ID_SIZE];
    rl_u16 request_length;
    rl_u16 page_length;
    rl_u16 index;

    for (index = 0; index < RL_SESSION_ID_SIZE; ++index)
        endpoint[index] = (rl_u8)(0x10 + index);
    request_length = rl_make_list_request(
        request,
        0x01020304UL,
        endpoint,
        0x11223344UL,
        path,
        1,
        0,
        RL_MAX_LIST_ENTRIES);
    assert(request_length == 4 + 16 + 5 + 1 + 7 + 6);
    assert(request[0] == 1 && request[3] == 4);
    assert(request[4] == 0x10 && request[19] == 0x1F);

    memset(&entry, 0, sizeof(entry));
    entry.kind = 0;
    entry.attributes = 0x20;
    strcpy(entry.name, "README.TXT");
    entry.data_size = 0x1234;
    entry.modified_time = 0x01020304UL;
    page_length = rl_make_list_page(
        page_bytes,
        0,
        2,
        RL_LIST_COMPLETE_CURSOR,
        &entry,
        1);
    assert(page_length != 0);
    assert(rl_parse_list_page(page_bytes, page_length, &page));
    assert(page.result_code == 0 && page.cursor == 2);
    assert(page.next_cursor == RL_LIST_COMPLETE_CURSOR);
    assert(page.entry_count == 1);
    assert(strcmp(page.entries[0].name, "README.TXT") == 0);
    assert(page.entries[0].data_size == 0x1234);
    assert(page.entries[0].modified_time == 0x01020304UL);
}

static void test_copy_prepare(void)
{
    rl_u8 payload[RL_MAX_PAYLOAD];
    rl_u8 active[RL_SESSION_ID_SIZE];
    rl_u8 source[RL_SESSION_ID_SIZE];
    rl_u8 destination[RL_SESSION_ID_SIZE];
    rl_u8 decoded_active[RL_SESSION_ID_SIZE];
    rl_u8 decoded_source[RL_SESSION_ID_SIZE];
    rl_u8 decoded_destination[RL_SESSION_ID_SIZE];
    char source_components[RL_MAX_PATH_COMPONENTS][RL_MAX_NAME_BYTES + 1];
    char destination_components[RL_MAX_PATH_COMPONENTS][RL_MAX_NAME_BYTES + 1];
    char decoded_name[RL_MAX_NAME_BYTES + 1];
    const char *source_path[] = { "DOS" };
    const char *destination_path[] = { "Text/Doc Readers" };
    rl_u8 source_count;
    rl_u8 destination_count;
    rl_u8 overwrite;
    rl_u32 epoch;
    rl_u32 source_provider;
    rl_u32 destination_provider;
    rl_u32 stream_length;
    rl_u32 stream_crc;
    rl_u16 length;
    rl_u8 index;

    for (index = 0; index < RL_SESSION_ID_SIZE; ++index)
    {
        active[index] = index;
        source[index] = (rl_u8)(index + 16);
        destination[index] = (rl_u8)(index + 32);
    }
    length = rl_make_copy_prepare(
        payload,
        5,
        active,
        source,
        RL_DOS_PROVIDER_ID,
        source_path,
        1,
        destination,
        0x01020304UL,
        destination_path,
        1,
        "TEST.TXT",
        129,
        0xAABBCCDDUL,
        0);
    assert(length != 0);
    assert(rl_parse_copy_prepare(
        payload,
        length,
        &epoch,
        decoded_active,
        decoded_source,
        &source_provider,
        source_components,
        &source_count,
        decoded_destination,
        &destination_provider,
        destination_components,
        &destination_count,
        decoded_name,
        &stream_length,
        &stream_crc,
        &overwrite));
    assert(epoch == 5);
    assert(memcmp(active, decoded_active, RL_SESSION_ID_SIZE) == 0);
    assert(memcmp(source, decoded_source, RL_SESSION_ID_SIZE) == 0);
    assert(memcmp(destination, decoded_destination, RL_SESSION_ID_SIZE) == 0);
    assert(source_provider == RL_DOS_PROVIDER_ID && source_count == 1);
    assert(strcmp(source_components[0], "DOS") == 0);
    assert(destination_provider == 0x01020304UL);
    assert(strcmp(destination_components[0], "Text/Doc Readers") == 0);
    assert(strcmp(decoded_name, "TEST.TXT") == 0);
    assert(stream_length == 129 && stream_crc == 0xAABBCCDDUL);
    assert(overwrite == 0);
}

static void test_rejects_unsafe_paths(void)
{
    rl_u8 payload[RL_MAX_PAYLOAD];
    rl_u8 endpoint[RL_SESSION_ID_SIZE];
    const char *unsafe_path[] = { ".." };
    const char *unsafe_name[] = { "A\\B" };
    rl_u16 length;
    memset(endpoint, 1, sizeof(endpoint));

    length = rl_make_list_request(
        payload,
        0,
        endpoint,
        RL_DOS_PROVIDER_ID,
        unsafe_path,
        1,
        0,
        RL_MAX_LIST_ENTRIES);
    assert(length == 0);
    length = rl_make_list_request(
        payload,
        0,
        endpoint,
        RL_DOS_PROVIDER_ID,
        unsafe_name,
        1,
        0,
        RL_MAX_LIST_ENTRIES);
    assert(length == 0);

    {
        const char *classic_mac_name[] = { "Text/Doc Readers" };
        length = rl_make_list_request(
            payload,
            0,
            endpoint,
            0x01020304UL,
            classic_mac_name,
            1,
            0,
            RL_MAX_LIST_ENTRIES);
        assert(length != 0);
    }
}

int main(void)
{
    test_crc_vectors();
    test_retrolink_frame();
    test_agent_frame();
    test_hello_ack();
    test_list_protocol();
    test_copy_prepare();
    test_rejects_unsafe_paths();
    puts("DOS protocol tests passed.");
    return 0;
}
