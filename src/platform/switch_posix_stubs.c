#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <switch.h>

struct sigaction;

int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact)
{
    (void)signum;
    (void)act;
    (void)oldact;
    return 0;
}

// The pinned mbedTLS 3 submodule has no Horizon OS entropy backend. Feed its
// hardware-entropy hook from libnx's kernel CSPRNG.
int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len,
                          size_t* output_len)
{
    (void)data;
    if (output == NULL || output_len == NULL) return -1;

    randomGet(output, len);
    *output_len = len;
    return 0;
}
