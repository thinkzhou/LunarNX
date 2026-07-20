#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../lib/libpeer/src/rtcp.h"

int main(void) {
  uint8_t packet[12];
  const uint8_t expected[] = {
      0x81, 0xce, 0x00, 0x02,
      0x01, 0x02, 0x03, 0x04,
      0x11, 0x22, 0x33, 0x44,
  };

  int ret = rtcp_get_pli(packet, sizeof(packet), 0x01020304, 0x11223344);
  if (ret != (int)sizeof(packet) || memcmp(packet, expected, sizeof(packet)) != 0) {
    fprintf(stderr, "FAIL: RTCP PLI wire format is invalid\n");
    for (size_t i = 0; i < sizeof(packet); i++) {
      fprintf(stderr, "%02x%s", packet[i], i + 1 == sizeof(packet) ? "\n" : " ");
    }
    return 1;
  }

  puts("RTCP PLI tests passed");
  return 0;
}
