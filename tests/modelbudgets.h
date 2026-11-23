// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
// Instrumented/debug lanes retain all functional assertions, never latency gates.
// Caller-thread library samples gate CPU AND CPU + blocked at 16 ms. Each sample
// reports wall/CPU/own-thread run-queue/blocked plus voluntary/involuntary switches.
// Only schedstat run-queue wait is excluded; sleeping, locks, worker and IO waits
// still count. The CPU gate always applies. The CPU + blocked gate needs enabled
// kernel.sched_schedstats; where it cannot be enabled (unprivileged builds such
// as AUR check()) each sample prints CALLER_WORK_BLOCKED_NOT_MEASURED with its
// reason. CI enables schedstats so every PR runs both gates.
#if defined(CONFIGQML_TIMING_GATES) && CONFIGQML_TIMING_GATES
constexpr bool enforceModelBudgets = true;
#else
constexpr bool enforceModelBudgets = false;
#endif
