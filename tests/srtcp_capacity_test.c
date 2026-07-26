#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "dtls_srtp.h"

int main(void) {
  struct {
    uint8_t before[32];
    uint8_t packet[16];
    uint8_t after[32];
  } guarded;
  memset(&guarded, 0xa5, sizeof(guarded));

  DtlsSrtp dtls_srtp;
  memset(&dtls_srtp, 0, sizeof(dtls_srtp));
  int packet_len = (int)sizeof(guarded.packet);

  const int ret = dtls_srtp_encrypt_rctp_packet(
      &dtls_srtp, guarded.packet, &packet_len, sizeof(guarded.packet));
  assert(ret == srtp_err_status_bad_param);
  assert(packet_len == (int)sizeof(guarded.packet));

  for (size_t i = 0; i < sizeof(guarded.before); ++i) {
    assert(guarded.before[i] == 0xa5);
  }
  for (size_t i = 0; i < sizeof(guarded.after); ++i) {
    assert(guarded.after[i] == 0xa5);
  }

  return 0;
}
