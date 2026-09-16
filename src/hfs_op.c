#include "hfs_op.h"

/* Move an operation between two lifecycle states with acquire/release order. */
static bool hfsOpTransition(HfsOp *op, HfsOpState expected,
				HfsOpState desired) {
	uint32_t expectedValue;

	if (op == NULL) {
		return false;
	}
	expectedValue = (uint32_t)expected;
	return atomic_compare_exchange_strong_explicit(&op->state,
		&expectedValue, (uint32_t)desired, memory_order_acq_rel,
		memory_order_acquire);
}

/* Initialize the caller-owned reference and an empty executor lifetime. */
void hfsOpInit(HfsOp *op) {
	if (op == NULL) {
		return;
	}
	atomic_init(&op->state, HFS_OP_NEW);
	atomic_init(&op->references, 1);
	atomic_init(&op->actorHeld, false);
}

/* Acquire the executor reference before admitting the operation. */
bool hfsOpBeginSubmit(HfsOp *op) {
	if (!hfsOpTransition(op, HFS_OP_NEW, HFS_OP_READY)) {
		return false;
	}
	atomic_fetch_add_explicit(&op->references, 1, memory_order_relaxed);
	atomic_store_explicit(&op->actorHeld, true, memory_order_release);
	return true;
}

/* Mark an operation as waiting for an inode or range dependency. */
bool hfsOpMarkDependencyWait(HfsOp *op) {
	return hfsOpTransition(op, HFS_OP_READY, HFS_OP_WAIT_DEP);
}

/* Mark an operation ready after its dependency has been admitted. */
bool hfsOpMarkReady(HfsOp *op) {
	return hfsOpTransition(op, HFS_OP_WAIT_DEP, HFS_OP_READY);
}

/* Mark an operation submitted once its executor accepts the backend callback. */
bool hfsOpMarkSubmitted(HfsOp *op) {
	return hfsOpTransition(op, HFS_OP_READY, HFS_OP_SUBMITTED);
}

/* Release the executor reference exactly once and preserve terminal state. */
void hfsOpComplete(HfsOp *op) {
	uint32_t state;

	if (op == NULL) {
		return;
	}
	state = atomic_load_explicit(&op->state, memory_order_acquire);
	for (;;) {
		if (state == HFS_OP_DONE || state == HFS_OP_ABORTED) {
			break;
		}
		if (atomic_compare_exchange_weak_explicit(&op->state, &state,
				(uint32_t)HFS_OP_DONE, memory_order_acq_rel,
				memory_order_acquire)) {
			break;
		}
	}
	if (atomic_exchange_explicit(&op->actorHeld, false,
			memory_order_acq_rel)) {
		hfsOpReleaseCaller(op);
	}
}

/* Abort an operation rejected before its backend callback is queued. */
void hfsOpSubmitFailed(HfsOp *op) {
	uint32_t state;

	if (op == NULL) {
		return;
	}
	state = atomic_load_explicit(&op->state, memory_order_acquire);
	while (state != HFS_OP_DONE && state != HFS_OP_ABORTED &&
		   !atomic_compare_exchange_weak_explicit(&op->state, &state,
				(uint32_t)HFS_OP_ABORTED, memory_order_acq_rel,
				memory_order_acquire)) {
		/* The compare-exchange updates state while another terminal path wins. */
	}
	if (atomic_exchange_explicit(&op->actorHeld, false,
			memory_order_acq_rel)) {
		hfsOpReleaseCaller(op);
	}
}

/* Decrement a caller or executor reference without allowing underflow. */
void hfsOpReleaseCaller(HfsOp *op) {
	uint32_t references;

	if (op == NULL) {
		return;
	}
	references = atomic_load_explicit(&op->references, memory_order_acquire);
	while (references != 0 &&
		   !atomic_compare_exchange_weak_explicit(&op->references,
			&references, references - 1, memory_order_acq_rel,
			memory_order_acquire)) {
		/* Retry with the value observed by the failed compare-exchange. */
	}
}

/* Return the current operation state for diagnostics. */
HfsOpState hfsOpState(const HfsOp *op) {
	if (op == NULL) {
		return HFS_OP_ABORTED;
	}
	return (HfsOpState)atomic_load_explicit(&op->state, memory_order_acquire);
}

/* Return the number of references still held by the operation. */
uint32_t hfsOpReferences(const HfsOp *op) {
	if (op == NULL) {
		return 0;
	}
	return atomic_load_explicit(&op->references, memory_order_acquire);
}
