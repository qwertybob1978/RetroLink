#ifndef RETROLINK_DOS_WIRE_H
#define RETROLINK_DOS_WIRE_H

typedef unsigned char rl_u8;
typedef unsigned short rl_u16;
typedef unsigned long rl_u32;

#define RL_SESSION_ID_SIZE 16
#define RL_MAX_PAYLOAD 1024
#define RL_FRAME_HEADER_SIZE 30
#define RL_AGENT_HEADER_SIZE 26
#define RL_FRAME_BUFFER_SIZE 1056

typedef struct
{
    rl_u8 message_type;
    rl_u8 session_id[RL_SESSION_ID_SIZE];
    rl_u32 request_id;
    rl_u16 payload_length;
    rl_u8 payload[RL_MAX_PAYLOAD];
} RlFrame;

typedef struct
{
    rl_u8 bytes[RL_FRAME_BUFFER_SIZE];
    rl_u16 length;
} RlFrameDecoder;

rl_u16 rl_crc16(const rl_u8 *data, rl_u16 length);
rl_u16 rl_crc16_xmodem(const rl_u8 *data, rl_u16 length);
rl_u32 rl_crc32(const rl_u8 *data, rl_u16 length);
rl_u32 rl_crc32_update(rl_u32 state, const rl_u8 *data, rl_u16 length);
rl_u32 rl_crc32_finish(rl_u32 state);
rl_u16 rl_encode_frame(const RlFrame *frame, rl_u8 *output);
rl_u16 rl_encode_agent_frame(const RlFrame *frame, rl_u8 *output);
int rl_decode_frame_byte(RlFrameDecoder *decoder, rl_u8 value, RlFrame *frame);
int rl_decode_agent_byte(RlFrameDecoder *decoder, rl_u8 value, RlFrame *frame);

#endif
