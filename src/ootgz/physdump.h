/* Remote "physdump <name>": the Havok worlds' bodies, raw, to the SD card (physdump.c). Debugging only. */
#ifndef PHYSDUMP_H
#define PHYSDUMP_H

#include <stdbool.h>
#include <stdint.h>

bool physdump_write(const char *name, int *records, int64_t *bytes);

#endif
