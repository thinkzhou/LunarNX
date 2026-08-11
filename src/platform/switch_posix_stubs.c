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
    static uint64_t entropy_call_counter = 0;
    (void)data;
    if (output == NULL || output_len == NULL) return -1;

    randomGet(output, len);
    // Ryujinx can replay libnx's randomGet sequence on every NRO launch. Mix
    // in a per-boot timing salt so independently-created Chiaki DRBG contexts
    // do not reuse PSN session identities. On hardware, XORing this into the
    // kernel CSPRNG output preserves the CSPRNG's security.
    uint64_t state = armGetSystemTick() ^
        (__atomic_add_fetch(&entropy_call_counter, 1, __ATOMIC_RELAXED) *
         UINT64_C(0x9e3779b97f4a7c15));
    for (size_t offset = 0; offset < len; ++offset) {
        if ((offset & 7u) == 0) {
            state += UINT64_C(0x9e3779b97f4a7c15);
            uint64_t mixed = state;
            mixed = (mixed ^ (mixed >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
            mixed = (mixed ^ (mixed >> 27)) * UINT64_C(0x94d049bb133111eb);
            state = mixed ^ (mixed >> 31);
        }
        output[offset] ^= (unsigned char)(state >> ((offset & 7u) * 8u));
    }
    *output_len = len;
    return 0;
}
