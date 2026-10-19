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
/* Creates window, Vulkan device, presenter and GPU command processor. */
int bbgpu_init(const BbGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t bbgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int bbgpu_handle_fault(void *ucontext, void *address);
/* BB_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void bbgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int bbgpu_text_input_poll(char *out_utf8, uint64_t size);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int bbgpu_overlay_captures_input(void);
/* Window-thread snapshot: mouse velocity in pixels/second, SDL button bitmask.
 * Mouse input is zero while released, unfocused, in a text dialog or in the overlay. */
typedef struct { float x, y; uint32_t buttons; int focused; } BbMouseState;
void bbgpu_mouse_state(BbMouseState *state);
/* Patches the loaded image before the game runs (image still writable): libGnm entry hooks. */
void bbgpu_patch_image(unsigned char *image, uint64_t size);
/* 1 when bbgpu_patch_image installed the frame rate hooks (bbport_frame_rate.cpp); 0 when it did not
 * (BB_FRAME_HOOKS=0, or a site of the game's code differs): the frame rate patches are the way then. */
int bbgpu_frame_hooks_installed(void);
/* The GPU library's lines for the runtime's report at exit (runtime_report): what its hooks in the
 * game's code did (bbport_guest_guards.cpp: how often its guards acted). */
void bbgpu_exit_report(void);
/* Android performance hints (bbport_adpf.cpp): the calling thread is the game's main thread; and a thread
 * got `name` (the runtime's guest threads), which joins the hint session when it is a frame-critical one. */
void bbgpu_adpf_register_main(void);
void bbgpu_adpf_thread_named(const char *name);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned bbgpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
