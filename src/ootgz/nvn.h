/* On-screen drawing through the game's own NVN (see nvn.c). */
#ifndef NVN_H
#define NVN_H

#include <stdbool.h>
#include <stdint.h>

#include "nvnapi.h"

/* Point main's nvnBootstrapLoader import at our wrapper (module init); returns 0 or a message. */
const char *nvn_hook_game(uintptr_t main_base);

/* Frame advance (game thread). nvn_freeze(true) from the frame it pauses on (the one it lets through
 * first) until it runs again (false): the game's picture is kept meanwhile. nvn_present_held, in the hold
 * loop after gfx_frame: presents that picture with the overlay on it; false if it couldn't (nothing shows,
 * as before). */
void nvn_freeze(bool paused);
bool nvn_present_held(void);
/* remote texdump (debugging): copy that texture at the next present to sloperinaGZ/dump/texture.bin */
void nvn_texdump(uintptr_t texture, int bytes_per_pixel);
void nvn_texdump_write(void);
/* a texture's size and NVN format (0s if it can't be read) */
/* debugging: hudtest (nvn.c) */
extern volatile bool nvn_hud_test;
extern volatile uintptr_t nvn_hud_test_target;
extern volatile int nvn_hud_test_bind;
extern volatile uintptr_t nvn_hud_test_cmd;
void nvn_cmd_begun(void *cmd);
void nvn_cmd_ended(void *cmd);
int nvn_hud_binds(uintptr_t *cmds, int *binds, int max);
int nvn_hud_bind_log(char *lines, int len);
void nvn_after_set_rt(void *cmd, const nvn_texture_t *target, int n_colors, const nvn_texture_t *depth);
void nvn_texture_info(uintptr_t texture, int *w, int *h, int *format); /* game thread: writes what the render thread copied */
extern volatile uint32_t nvn_held_presents; /* frames presented while held */

#endif
