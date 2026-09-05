#ifndef HFS_DURABILITY_H
#define HFS_DURABILITY_H

#include <stdint.h>

/* Initialize the optional fsync ordering trace for one HFS process. */
void hfsDurabilityTraceInit(void);
/* Record one fsync phase without changing the caller-visible errno value. */
void hfsDurabilityTraceRecord(uint64_t inodeId, const char *phase, int result);
/* Write the optional trace after all HFS activity has stopped. */
void hfsDurabilityTraceFini(void);

#endif /* HFS_DURABILITY_H */
