/* On-screen drawing through the game's own NVN (see nvn.c). */
#ifndef NVN_H
#define NVN_H

#include <stdbool.h>
#include <stdint.h>

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
void nvn_texdump_write(void); /* game thread: writes what the render thread copied */
extern volatile uint32_t nvn_held_presents; /* frames presented while held */

#endif
