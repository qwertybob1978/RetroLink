#ifndef RETROLINK_DOS_AGENT_LINK_H
#define RETROLINK_DOS_AGENT_LINK_H

#include "dos_serial.h"

typedef struct
{
    DosUart *uart;
    RlFrameDecoder decoder;
    rl_u8 session_id[RL_SESSION_ID_SIZE];
    rl_u32 next_sequence;
    rl_u32 pending_sequence;
    rl_u16 pending_length;
    rl_u8 pending_severity;
    rl_u8 connected;
    rl_u8 pending_record;
    rl_u8 pending_sent;
    rl_u8 pending_message[512];
} AgentLink;

void agent_link_init(AgentLink *link, DosUart *uart);
void agent_link_poll(AgentLink *link);
void agent_link_log(AgentLink *link, rl_u8 severity, const char *message);

#endif
