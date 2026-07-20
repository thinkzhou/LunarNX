#include "stun.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

static void write_u64(uint8_t out[8], uint64_t value) {
  for (int i = 0; i < 8; i++) {
    out[i] = (uint8_t)(value >> (56 - i * 8));
  }
}

static StunMessage parse_message(const StunMessage* source) {
  StunMessage parsed;
  memset(&parsed, 0, sizeof(parsed));
  memcpy(parsed.buf, source->buf, source->size);
  parsed.size = source->size;
  stun_parse_msg_buf(&parsed);
  return parsed;
}

int main(void) {
  StunMessage request;
  memset(&request, 0, sizeof(request));
  stun_msg_create(&request, STUN_CLASS_REQUEST | STUN_METHOD_BINDING);

  uint32_t priority = htonl(1845501695u);
  uint8_t tie_breaker[8];
  write_u64(tie_breaker, UINT64_C(0x0123456789abcdef));
  stun_msg_write_attr(&request,
                      STUN_ATTR_TYPE_PRIORITY,
                      sizeof(priority),
                      (char*)&priority);
  stun_msg_write_attr(&request, STUN_ATTR_TYPE_USE_CANDIDATE, 0, NULL);
  stun_msg_write_attr(&request,
                      STUN_ATTR_TYPE_ICE_CONTROLLING,
                      sizeof(tie_breaker),
                      (char*)tie_breaker);

  StunMessage parsed = parse_message(&request);
  require(parsed.stunclass == STUN_CLASS_REQUEST,
          "Binding request class should parse");
  require(parsed.stunmethod == STUN_METHOD_BINDING,
          "Binding request method should parse");
  require(parsed.priority == 1845501695u,
          "PRIORITY should parse in host byte order");
  require(parsed.use_candidate == 1,
          "USE-CANDIDATE should be retained");
  require(parsed.ice_controlling == UINT64_C(0x0123456789abcdef),
          "ICE-CONTROLLING tie-breaker should parse");
  require(parsed.ice_controlled == 0,
          "absent ICE-CONTROLLED should remain zero");

  StunMessage error;
  memset(&error, 0, sizeof(error));
  stun_msg_create(&error, STUN_CLASS_ERROR | STUN_METHOD_BINDING);
  uint8_t error_value[4] = {0, 0, 4, 87};
  stun_msg_write_attr(&error,
                      STUN_ATTR_TYPE_ERROR_CODE,
                      sizeof(error_value),
                      (char*)error_value);
  parsed = parse_message(&error);
  require(parsed.stunclass == STUN_CLASS_ERROR,
          "Binding error class should parse");
  require(parsed.error_code == 487,
          "role-conflict error code should parse");

  puts("STUN ICE attribute tests passed");
  return 0;
}
