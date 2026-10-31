// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
// Instrumented/debug lanes retain all functional assertions, never wall-clock gates.
#if defined(CONFIGQML_TIMING_GATES) && CONFIGQML_TIMING_GATES
constexpr bool enforceModelBudgets = true;
#else
constexpr bool enforceModelBudgets = false;
#endif
