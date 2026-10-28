// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
// Instrumented/debug lanes retain all functional assertions, never wall-clock gates.
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__) && !defined(CONFIGQML_INSTRUMENTED_BUILD)
constexpr bool enforceModelBudgets = true;
#else
constexpr bool enforceModelBudgets = false;
#endif
