// SPDX-License-Identifier: LGPL-3.0-or-later
#include "modelbudgets.h"
#include <cstdio>
int work();
static_assert(enforceModelBudgets == bool(EXPECTED_TIMING), "Wrong compiled timing policy");
int main() {
    std::printf("compiled timing gates=%d expected=%d\n", enforceModelBudgets, EXPECTED_TIMING);
    return work() == 7 ? 0 : 1;
}
