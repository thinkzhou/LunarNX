// Stubs for chiaki holepunch symbols that we don't use in LAN-only streaming.
// These prevent linker errors from libchiaki.a's remote/holepunch.c object.

#include <stddef.h>

// json-c stubs
void* json_pointer_get(void* obj, const char* path) { return NULL; }
int json_object_is_type(void* obj, int type) { return 0; }
const char* json_object_get_string(void* obj) { return NULL; }
void* json_tokener_parse(const char* str) { return NULL; }
const char* json_object_to_json_string_ext(void* obj, int flags) { return NULL; }
void* json_tokener_new(void) { return NULL; }
void* json_tokener_parse_ex(void* tok, const char* str, int len) { return NULL; }
int json_object_put(void* obj) { return 0; }
void json_tokener_free(void* tok) {}
int json_object_object_get_ex(void* obj, const char* key, void** value) { return 0; }

// more json-c stubs
int json_object_get_int(void* obj) { return 0; }
int json_object_object_length(void* obj) { return 0; }
int json_object_array_length(void* obj) { return 0; }
void* json_object_array_get_idx(void* obj, int idx) { return NULL; }

// libupnp stubs
int UPNP_DeletePortMapping(const char* addr, const char* port, const char* proto) { return -1; }
int UPNP_AddPortMapping(const char* addr, const char* ext_port, const char* proto,
                         const char* int_port, const char* int_client,
                         const char* desc, const char* duration) { return -1; }
int UPNP_GetExternalIPAddress(const char* addr, const char* proto, char* ext_ip) { return -1; }
char* strupnperror(int err) { return NULL; }

// curl WebSocket stubs (holepunch uses WS for PSN signaling)
int curl_ws_recv(void* curl, void* buffer, size_t buflen, const size_t** nread,
                 const void** ws_meta) { return 0; }
int curl_ws_send(void* curl, const void* buffer, size_t buflen,
                 size_t* sent, size_t fragsize, unsigned int flags) { return 0; }

// more json-c stubs
long long json_object_get_int64(void* obj) { return 0; }
