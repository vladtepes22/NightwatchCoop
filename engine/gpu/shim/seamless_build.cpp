#ifndef BB_SEAMLESS_BUILD_ID
#error The seamless build must carry its source identity
#endif
extern "C" const char* bbport_build_id(void) { return BB_SEAMLESS_BUILD_ID; }
