/* Ignore voids: immune to void planes, out-of-bounds ground-map voids, abyss collision floors,
 * deep lava, quicksand and crushers (patch stubs gated by LOCK_BIT_NO_VOID in lock_slot). */
#ifndef NOVOID_H
#define NOVOID_H

#include <stdbool.h>

bool novoid_available(void);
bool novoid_enabled(void);
void novoid_set(bool on);

#endif
