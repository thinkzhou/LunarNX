#include "sdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

int main(void) {
  char sdp[CONFIG_SDP_BUFFER_SIZE];
  memset(sdp, 0, sizeof(sdp));

  sdp_create(sdp, 1, 1, 1);
  sdp_append_h264(sdp);
  sdp_append_opus(sdp);
  sdp_append_datachannel(sdp);

  char* video = strstr(sdp, "m=video");
  char* audio = strstr(sdp, "m=audio");
  char* application = strstr(sdp, "m=application");

  require(video != NULL, "offer should include video m-line");
  require(audio != NULL, "offer should include audio m-line");
  require(application != NULL, "offer should include datachannel m-line");
  require(video < audio && audio < application, "media sections should be ordered");

  size_t video_len = (size_t)(audio - video);
  require(memmem(video, video_len, "a=recvonly", strlen("a=recvonly")) != NULL,
          "video offer direction should match XStreaming recvonly");
  require(memmem(video, video_len, "a=sendrecv", strlen("a=sendrecv")) == NULL,
          "video offer should not advertise sendrecv");
  require(strstr(audio, "a=sendrecv") != NULL,
          "audio offer should keep sendrecv");

  printf("libpeer SDP offer tests passed\n");
  return 0;
}
