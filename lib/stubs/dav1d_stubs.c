void dav1d_data_unref(void* d) {}
void dav1d_flush(void* d) {}
void dav1d_close(void* c) {}
void dav1d_open(void** c, void* s) {}
void dav1d_send_data(void* c, void* d) {}
void dav1d_get_picture(void* c, void* p) {}
void dav1d_data_wrap(void* d, void* b, unsigned s, void* f, void* u) {}
void dav1d_data_wrap_user_data(void* d, void* b, void* u) {}
void dav1d_get_event_flags(void* c, unsigned* f) {}
void dav1d_picture_unref(void* p) {}
void dav1d_parse_sequence_header(void* p, void* d, void* b, unsigned s) {}
const char* dav1d_version(void) { return "0.0.0"; }
void dav1d_default_settings(void* s) {}
int dav1d_get_frame_delay(void* c) { return 8; }
