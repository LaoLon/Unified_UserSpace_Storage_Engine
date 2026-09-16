#ifndef HFS_OP_H
#define HFS_OP_H

#include <stdbool.h>
#include <stdint.h>

#include "hfs.h"

/* Initialize one operation before it is exposed to an executor. */
void hfsOpInit(HfsOp *op);
/* Acquire the executor reference and move a new operation to ready state. */
bool hfsOpBeginSubmit(HfsOp *op);
/* Record that dependency admission is currently blocking the operation. */
bool hfsOpMarkDependencyWait(HfsOp *op);
/* Return a dependency-waiting operation to the ready state. */
bool hfsOpMarkReady(HfsOp *op);
/* Record that the backend start callback has been accepted. */
bool hfsOpMarkSubmitted(HfsOp *op);
/* Publish normal completion and release the executor reference. */
void hfsOpComplete(HfsOp *op);
/* Publish submit failure and release the executor reference. */
void hfsOpSubmitFailed(HfsOp *op);
/* Release the synchronous caller reference after the result is consumed. */
void hfsOpReleaseCaller(HfsOp *op);
/* Read the current lifecycle state for diagnostics and future schedulers. */
HfsOpState hfsOpState(const HfsOp *op);
/* Read the number of live caller/executor references. */
uint32_t hfsOpReferences(const HfsOp *op);

#endif /* HFS_OP_H */
