// self_test.h — comprehensive end-to-end test suite.
//
// Walks every public surface of every module, exercising real code paths
// with synthetic input where physical interaction would normally be
// required. Each step logs a PASS / FAIL line; the runner prints a final
// summary with counts.
//
// Triggered from the serial CLI via `selftest`. Safe to run any time —
// snapshots and restores volume / state / settings so the cube is left in
// the same state it started in.

#ifndef DASH_SELF_TEST_H
#define DASH_SELF_TEST_H

namespace dash {

void runSelfTest();

}  // namespace dash

#endif
