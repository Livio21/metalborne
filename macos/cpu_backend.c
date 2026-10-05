/* Diagnostic link target. Graphics and GPU-provided imports remain unavailable. */
#include "gpu/bbgpu.h"
#include <stdio.h>
void bbgpu_register_kernel(void) {}
int bbgpu_init(const BbGpuConfig *config) {
    (void)config;
    fputs("This CPU diagnostic has no GPU backend; pass --cpu-only.\n",stderr);
    return 1;
}
uintptr_t bbgpu_resolve(const char *nid) { (void)nid; return 0; }
int bbgpu_handle_fault(void *context,void *address) { (void)context; (void)address; return 0; }
void bbgpu_dump_guest_writes(void *context) { (void)context; }
int bbgpu_poll_events(void) { return 0; }
int bbgpu_read_host_input(BbHostInput *out) { (void)out; return 0; }
int bbgpu_text_input_begin(const char *initial,const char *prompt) { (void)initial; (void)prompt; return 0; }
int bbgpu_text_input_poll(char *out,uint64_t size) { (void)out; (void)size; return 2; }
int bbgpu_overlay_captures_input(void) { return 0; }
unsigned bbgpu_symbol_count(void) { return 0; }
int vulkan_smoke(void) { fputs("This CPU diagnostic has no Vulkan backend.\n",stderr); return 1; }
