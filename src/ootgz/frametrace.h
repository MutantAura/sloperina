/* Remote "frametrace": one frame of the game's NVN command stream to the log (frametrace.c). Debugging only. */
#ifndef FRAMETRACE_H
#define FRAMETRACE_H

#include <stdbool.h>
#include <stdint.h>

#include "nvnapi.h"

/* nvn.c's wrap_proc: our version of a function the trace watches, else 0 */
void *frametrace_wrap(const char *name, void *proc, bool all); /* all: the draw calls etc. too (debugging) */
/* the colour render targets the game bound in its last whole frame, and how often (remote rtlist) */
int frametrace_targets(uintptr_t *tex, uint32_t *sets, int max);
/* nvn.c, at each of the game's presents: the texture presented */
void frametrace_present(getproc_t getproc, nvn_device_t *dev, const nvn_texture_t *presented);
/* remote: trace the next whole frame */
void frametrace_arm(void);
/* game thread: writes a finished trace to the SD card */
void frametrace_write(void);

#endif
