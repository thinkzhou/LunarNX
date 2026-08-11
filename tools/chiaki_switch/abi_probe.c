#include <chiaki/session.h>

#include <stddef.h>

size_t lunarnx_consumer_chiaki_session_size(void)
{
    return sizeof(ChiakiSession);
}

size_t lunarnx_consumer_chiaki_session_log_offset(void)
{
    return offsetof(ChiakiSession, log);
}

size_t lunarnx_consumer_chiaki_session_holepunch_offset(void)
{
    return offsetof(ChiakiSession, holepunch_session);
}

size_t lunarnx_consumer_chiaki_stream_connection_size(void)
{
    return sizeof(ChiakiStreamConnection);
}

size_t lunarnx_consumer_chiaki_controller_state_offset(void)
{
    return offsetof(ChiakiSession, controller_state);
}

