/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef BBGPU_H
#define BBGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} BbGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void bbgpu_register_kernel(void);
/* Start networking after game metadata, guest TLS and linked modules are ready. */
void bbgpu_start_online(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int bbgpu_init(const BbGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t bbgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled.
 * The context is the handler's ucontext_t (Linux) or the vectored exception handler's
 * EXCEPTION_POINTERS (Windows). */
int bbgpu_handle_fault(void *ucontext, void *address);
/* BB_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault
 * (same context as bbgpu_handle_fault). */
void bbgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
/* Party co-op (Chalice glyph pick list): called when an ImeDialog of max_length characters opens and no
 * preset answered. Returns the box's pick list, one "value<TAB>label" line per choice, or NULL for the
 * plain keyboard box. Also where the test aids queue their keys. */
const char *bbgpu_ime_choices(uint32_t max_length);
/* The ImeDialog of max_length characters ended: the text it was confirmed with (cancelled != 0: none). */
void bbgpu_ime_closed(uint32_t max_length, const char *text_utf8, int cancelled);
/* Like bbgpu_text_input_begin with a pick list (bbgpu_ime_choices' lines). */
int bbgpu_text_input_begin_list(const char *initial_utf8, const char *prompt_utf8, const char *choices);
/* Test aid: keys the next text box takes as soon as it opens (bbgpu_text_input_test_type's keys plus
 * 0x01 for Up and 0x02 for Down). */
int bbgpu_text_input_test_queue(const char *keys);
int bbgpu_text_input_poll(char *out_utf8, uint64_t size);
/* 1 while the in-game settings menu or the map is open: the game's pad input is held neutral. */
int bbgpu_overlay_captures_input(void);
/* The map (gpu/shim/map/map_page.h): 1 when baked maps are there; toggle opens or closes the map
 * view (the pad reader, on the map button). */
int bbgpu_map_available(void);
void bbgpu_map_toggle(void);
int bbgpu_window_in_background(void);
/* Patches the loaded image before the game runs (image still writable): libGnm entry hooks. */
void bbgpu_patch_image(unsigned char *image, uint64_t size);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned bbgpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif
int bbgpu_ping_combo_enabled(void);
void bbgpu_ping_request(void);
/* I5 gather the party (gpu/shim/party/party_gather.h): 1 while this game hosts a running party
 * with gathering on; the pad reader calls request on touchpad held + L3. `source` names the
 * combo in the log. */
int bbgpu_gather_combo_enabled(void);
void bbgpu_gather_request(const char *source);
const char *bbgpu_ime_preset(uint32_t max_length);
#ifdef __cplusplus
}
#endif
