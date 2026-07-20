#include <switch.h>

struct mbedtls_timing_delay_context {
    uint64_t start_ticks;
    uint32_t int_ms;
    uint32_t fin_ms;
};

void mbedtls_timing_set_delay(void* data, unsigned int int_ms, unsigned int fin_ms) {
    struct mbedtls_timing_delay_context* ctx = 
        (struct mbedtls_timing_delay_context*)data;
    ctx->start_ticks = armGetSystemTick();
    ctx->int_ms = int_ms;
    ctx->fin_ms = fin_ms;
}

int mbedtls_timing_get_delay(void* data) {
    struct mbedtls_timing_delay_context* ctx = 
        (struct mbedtls_timing_delay_context*)data;
    
    uint64_t freq = armGetSystemTickFreq();
    uint64_t elapsed_ticks = armGetSystemTick() - ctx->start_ticks;
    uint64_t elapsed_ms = (elapsed_ticks * 1000) / freq;
    
    if (elapsed_ms >= ctx->fin_ms) return 2;  // final timeout
    if (elapsed_ms >= ctx->int_ms) return 1;   // intermediate
    return 0;                                    // still waiting
}
