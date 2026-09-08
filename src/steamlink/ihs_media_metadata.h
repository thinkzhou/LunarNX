#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Only valid synchronously inside the respective ihslib submit callback.
uint16_t LunarIHSAudioSequence(void);
uint32_t LunarIHSAudioTimestamp(void);
uint32_t LunarIHSVideoTimestamp(void);
#ifdef __cplusplus
}
#endif
