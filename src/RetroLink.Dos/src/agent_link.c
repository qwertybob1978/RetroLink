#include <string.h>

#include "agent_link.h"

#define AGENT_HELLO 0x01
#define AGENT_HELLO_ACK 0x02
#define AGENT_DEBUG_RECORD 0x30
#define AGENT_DEBUG_ACK 0x31
#define AGENT_DISCONNECT 0x7E
#define AGENT_ERROR 0x7F
#define AGENT_MAX_DEBUG_LENGTH 512

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

static rl_u32 read_u32(const rl_u8 *value)
{
    return ((rl_u32)value[0] << 24) |
        ((rl_u32)value[1] << 16) |
        ((rl_u32)value[2] << 8) |
        (rl_u32)value[3];
}

static int send_frame(AgentLink *link, RlFrame *frame)
{
    rl_u8 encoded[RL_FRAME_BUFFER_SIZE];
    rl_u16 length;
    rl_u16 index;

    memcpy(frame->session_id, link->session_id, RL_SESSION_ID_SIZE);
    length = rl_encode_agent_frame(frame, encoded);
    if (length == 0)
        return 0;
    for (index = 0; index < length; ++index)
    {
        if (!dos_uart_write(link->uart, encoded[index], 10))
            return 0;
    }
    return 1;
}

static void send_hello_ack(AgentLink *link)
{
    RlFrame response;
    memset(&response, 0, sizeof(response));
    response.message_type = AGENT_HELLO_ACK;
    response.payload_length = 20;
    response.payload[0] = 1;
    response.payload[1] = 1;
    response.payload[2] = 0;
    response.payload[3] = 0;
    response.payload[4] = 6;
    response.payload[5] = 22;
    write_u16(response.payload + 6, 0x8088);
    write_u32(response.payload + 8, 0);
    write_u32(response.payload + 12, 0);
    write_u32(response.payload + 16, 0x0000000FUL);
    if (send_frame(link, &response))
    {
        link->connected = 1;
        link->pending_sent = 0;
        agent_link_log(link, 0, "DOSBox 8088 debug monitor connected");
    }
}

static void send_error(AgentLink *link, rl_u8 rejected_type)
{
    static const char message[] = "DOS debug monitor does not support this Agent command.";
    RlFrame response;
    rl_u16 message_length;

    message_length = (rl_u16)(sizeof(message) - 1);
    memset(&response, 0, sizeof(response));
    response.message_type = AGENT_ERROR;
    write_u16(response.payload, 1);
    response.payload[2] = rejected_type;
    response.payload[3] = (rl_u8)message_length;
    memcpy(response.payload + 4, message, message_length);
    response.payload_length = (rl_u16)(message_length + 4);
    send_frame(link, &response);
}

static void send_debug_record(AgentLink *link)
{
    RlFrame frame;
    if (!link->connected || !link->pending_record || link->pending_sent)
        return;

    memset(&frame, 0, sizeof(frame));
    frame.message_type = AGENT_DEBUG_RECORD;
    write_u32(frame.payload, link->pending_sequence);
    frame.payload[4] = link->pending_severity;
    write_u16(frame.payload + 5, link->pending_length);
    if (link->pending_length != 0)
    {
        memcpy(
            frame.payload + 7,
            link->pending_message,
            link->pending_length);
    }
    frame.payload_length = (rl_u16)(7 + link->pending_length);
    if (send_frame(link, &frame))
        link->pending_sent = 1;
}

static void handle_frame(AgentLink *link, const RlFrame *frame)
{
    if (frame->message_type == AGENT_HELLO)
    {
        if (frame->payload_length != 4 || frame->payload[0] != 1)
            return;
        memcpy(link->session_id, frame->session_id, RL_SESSION_ID_SIZE);
        link->connected = 0;
        link->pending_sent = 0;
        send_hello_ack(link);
        return;
    }
    if (!link->connected ||
        memcmp(link->session_id, frame->session_id, RL_SESSION_ID_SIZE) != 0)
    {
        return;
    }
    if (frame->message_type == AGENT_DEBUG_ACK)
    {
        if (frame->payload_length == 5 &&
            read_u32(frame->payload) == link->pending_sequence)
        {
            link->pending_record = 0;
            link->pending_sent = 0;
            link->pending_length = 0;
        }
        return;
    }
    if (frame->message_type == AGENT_DISCONNECT)
    {
        link->connected = 0;
        link->pending_sent = 0;
        return;
    }
    send_error(link, frame->message_type);
}

void agent_link_init(AgentLink *link, DosUart *uart)
{
    memset(link, 0, sizeof(*link));
    link->uart = uart;
    link->next_sequence = 1;
}

void agent_link_poll(AgentLink *link)
{
    rl_u8 value;
    RlFrame frame;

    if (link == 0 || link->uart == 0)
        return;
    while (dos_uart_try_read(link->uart, &value))
    {
        if (rl_decode_agent_byte(&link->decoder, value, &frame))
            handle_frame(link, &frame);
    }
    send_debug_record(link);
}

void agent_link_log(AgentLink *link, rl_u8 severity, const char *message)
{
    unsigned int length;
    unsigned int index;

    if (link == 0 || message == 0)
        return;
    length = (unsigned int)strlen(message);
    if (length > AGENT_MAX_DEBUG_LENGTH)
        length = AGENT_MAX_DEBUG_LENGTH;
    for (index = 0; index < length; ++index)
    {
        unsigned char value = (unsigned char)message[index];
        if (value != '\t' && (value < 0x20 || value > 0x7E))
            return;
    }
    if (link->pending_record)
    {
        if (link->pending_sent)
            return;
    }
    link->pending_sequence = link->next_sequence++;
    if (link->next_sequence == 0)
        link->next_sequence = 1;
    link->pending_severity = severity;
    link->pending_length = (rl_u16)length;
    if (length != 0)
        memcpy(link->pending_message, message, length);
    link->pending_record = 1;
    link->pending_sent = 0;
    send_debug_record(link);
}
