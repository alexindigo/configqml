// SPDX-License-Identifier: LGPL-3.0-or-later
// Guest-only interposer for the ordinary shared-library qualification lane.
#include "configlayers.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

namespace {
std::atomic<unsigned long long> resolutions{0};
struct Report {
    ~Report() { std::fprintf(stderr, "SOURCE_CUT_RESOLUTIONS %llu\n", resolutions.load()); }
} report;
} // namespace

namespace configqml {
QVariantMap ConfigLayers::effective() const {
    using Original = QVariantMap (*)(const ConfigLayers *);
    static const auto original =
        reinterpret_cast<Original>(dlsym(RTLD_NEXT, "_ZNK9configqml12ConfigLayers9effectiveEv"));
    if (!original)
        std::abort();
    ++resolutions;
    return original(this);
}
} // namespace configqml
