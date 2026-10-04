#include "uuse/async.hpp"

/*
 * This translation unit intentionally has no ownership logic of its own.
 * Operation owns the intrusive IoHandle ABI so the frontend and actor use one
 * reference implementation rather than parallel lifetime rules.
 */
