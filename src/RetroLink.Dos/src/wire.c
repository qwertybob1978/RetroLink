#include <string.h>

#include "wire.h"

static void rl_write_u16(rl_u8 *destination, rl_u16 value)
{
    destination[0] = (rl_u8)(value >> 8);
    destination[1] = (rl_u8)value;
}

static void rl_write_u32(rl_u8 *destination, rl_u32 value)
{
    destination[0] = (rl_u8)(value >> 24);
    destination[1] = (rl_u8)(value >> 16);
    destination[2] = (rl_u8)(value >> 8);
    destination[3] = (rl_u8)value;
}

static rl_u16 rl_read_u16(const rl_u8 *value)
{
    return (rl_u16)(((rl_u16)value[0] << 8) | value[1]);
}

static rl_u32 rl_read_u32(const rl_u8 *value)
{
    return ((rl_u32)value[0] << 24) |
        ((rl_u32)value[1] << 16) |
        ((rl_u32)value[2] << 8) |
        (rl_u32)value[3];
}

static void rl_drop_prefix(RlFrameDecoder *decoder, rl_u16 count)
{
    if (count >= decoder->length)
    {
        decoder->length = 0;
        return;
    }

    decoder->length = (rl_u16)(decoder->length - count);
    memmove(decoder->bytes, decoder->bytes + count, decoder->length);
}

static int rl_magic_offset(
    const RlFrameDecoder *decoder,
    const rl_u8 *magic,
    rl_u16 magic_length)
{
    rl_u16 index;
    if (decoder->length < magic_length)
        return -1;

    for (index = 0; index <= decoder->length - magic_length; ++index)
    {
        if (memcmp(decoder->bytes + index, magic, magic_length) == 0)
            return (int)index;
    }

    return -1;
}

static int rl_decode_byte(
    RlFrameDecoder *decoder,
    rl_u8 value,
    RlFrame *frame,
    const rl_u8 *magic,
    rl_u16 header_size,
    int agent)
{
    rl_u16 payload_length;
    rl_u16 frame_length;
    rl_u16 expected_crc;
    rl_u16 actual_crc;
    rl_u16 session_offset;
    int magic_offset;

    if (decoder->length >= RL_FRAME_BUFFER_SIZE)
        rl_drop_prefix(decoder, 1);
    decoder->bytes[decoder->length++] = value;

    for (;;)
    {
        magic_offset = rl_magic_offset(decoder, magic, 4);
        if (magic_offset < 0)
        {
            if (decoder->length > 3)
                rl_drop_prefix(decoder, (rl_u16)(decoder->length - 3));
            return 0;
        }
        if (magic_offset > 0)
            rl_drop_prefix(decoder, (rl_u16)magic_offset);
        if (decoder->length < header_size)
            return 0;

        payload_length = rl_read_u16(decoder->bytes + (agent ? 24 : 28));
        if (decoder->bytes[4] != 1 ||
            decoder->bytes[6] != 0 ||
            decoder->bytes[7] != 0 ||
            payload_length > RL_MAX_PAYLOAD)
        {
            rl_drop_prefix(decoder, 1);
            continue;
        }

        frame_length = (rl_u16)(header_size + payload_length + 2);
        if (decoder->length < frame_length)
            return 0;

        expected_crc = rl_read_u16(decoder->bytes + frame_length - 2);
        actual_crc = rl_crc16(decoder->bytes, (rl_u16)(frame_length - 2));
        if (expected_crc != actual_crc)
        {
            rl_drop_prefix(decoder, 1);
            continue;
        }

        frame->message_type = decoder->bytes[5];
        session_offset = agent ? 8 : 8;
        memcpy(frame->session_id, decoder->bytes + session_offset, RL_SESSION_ID_SIZE);
        frame->request_id = agent ? 0 : rl_read_u32(decoder->bytes + 24);
        frame->payload_length = payload_length;
        if (payload_length != 0)
        {
            memcpy(
                frame->payload,
                decoder->bytes + header_size,
                payload_length);
        }
        rl_drop_prefix(decoder, frame_length);
        return 1;
    }
}

rl_u16 rl_crc16(const rl_u8 *data, rl_u16 length)
{
    rl_u16 crc = 0xFFFF;
    rl_u16 index;
    rl_u8 bit;

    for (index = 0; index < length; ++index)
    {
        crc ^= (rl_u16)((rl_u16)data[index] << 8);
        for (bit = 0; bit < 8; ++bit)
        {
            if ((crc & 0x8000) != 0)
                crc = (rl_u16)((crc << 1) ^ 0x1021);
            else
                crc = (rl_u16)(crc << 1);
        }
    }
    return crc;
}

rl_u16 rl_crc16_xmodem(const rl_u8 *data, rl_u16 length)
{
    rl_u16 crc = 0;
    rl_u16 index;
    rl_u8 bit;

    for (index = 0; index < length; ++index)
    {
        crc ^= (rl_u16)((rl_u16)data[index] << 8);
        for (bit = 0; bit < 8; ++bit)
        {
            if ((crc & 0x8000) != 0)
                crc = (rl_u16)((crc << 1) ^ 0x1021);
            else
                crc = (rl_u16)(crc << 1);
        }
    }
    return crc;
}

rl_u32 rl_crc32(const rl_u8 *data, rl_u16 length)
{
    return rl_crc32_finish(rl_crc32_update(0xFFFFFFFFUL, data, length));
}

rl_u32 rl_crc32_update(rl_u32 crc, const rl_u8 *data, rl_u16 length)
{
    rl_u16 index;
    rl_u8 bit;

    for (index = 0; index < length; ++index)
    {
        crc ^= data[index];
        for (bit = 0; bit < 8; ++bit)
        {
            if ((crc & 1) != 0)
                crc = (crc >> 1) ^ 0xEDB88320UL;
            else
                crc >>= 1;
        }
    }
    return crc;
}

rl_u32 rl_crc32_finish(rl_u32 state)
{
    return state ^ 0xFFFFFFFFUL;
}

static rl_u16 rl_encode(
    const RlFrame *frame,
    rl_u8 *output,
    const rl_u8 *magic,
    rl_u16 header_size,
    int agent)
{
    rl_u16 frame_length;
    rl_u16 crc;

    if (frame == 0 || output == 0 || frame->payload_length > RL_MAX_PAYLOAD)
        return 0;

    frame_length = (rl_u16)(header_size + frame->payload_length + 2);
    memcpy(output, magic, 4);
    output[4] = 1;
    output[5] = frame->message_type;
    output[6] = 0;
    output[7] = 0;
    memcpy(output + 8, frame->session_id, RL_SESSION_ID_SIZE);
    if (!agent)
        rl_write_u32(output + 24, frame->request_id);
    rl_write_u16(output + (agent ? 24 : 28), frame->payload_length);
    if (frame->payload_length != 0)
        memcpy(output + header_size, frame->payload, frame->payload_length);
    crc = rl_crc16(output, (rl_u16)(frame_length - 2));
    rl_write_u16(output + frame_length - 2, crc);
    return frame_length;
}

rl_u16 rl_encode_frame(const RlFrame *frame, rl_u8 *output)
{
    static const rl_u8 magic[4] = { 'R', 'L', 'N', 'K' };
    return rl_encode(frame, output, magic, RL_FRAME_HEADER_SIZE, 0);
}

rl_u16 rl_encode_agent_frame(const RlFrame *frame, rl_u8 *output)
{
    static const rl_u8 magic[4] = { 'M', 'S', 'A', 'G' };
    return rl_encode(frame, output, magic, RL_AGENT_HEADER_SIZE, 1);
}

int rl_decode_frame_byte(RlFrameDecoder *decoder, rl_u8 value, RlFrame *frame)
{
    static const rl_u8 magic[4] = { 'R', 'L', 'N', 'K' };
    return rl_decode_byte(
        decoder,
        value,
        frame,
        magic,
        RL_FRAME_HEADER_SIZE,
        0);
}

int rl_decode_agent_byte(RlFrameDecoder *decoder, rl_u8 value, RlFrame *frame)
{
    static const rl_u8 magic[4] = { 'M', 'S', 'A', 'G' };
    return rl_decode_byte(
        decoder,
        value,
        frame,
        magic,
        RL_AGENT_HEADER_SIZE,
        1);
}
