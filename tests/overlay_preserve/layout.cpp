// SPDX-License-Identifier: LGPL-3.0-or-later
#include <configdraft.h>
#include <configoverlayeditor.h>
#include <configqmlstore.h>
#include <configstorehandle.h>
#include <cstdio>
using namespace configqml;
template <typename T> void layout(const char *name) {
    std::printf("%s %zu %zu\n", name, sizeof(T), alignof(T));
}
int main() {
    layout<ConfigDraft>("ConfigDraft");
    layout<ConfigStore>("ConfigStore");
    layout<ConfigQmlStore>("ConfigQmlStore");
    layout<ConfigStoreHandle>("ConfigStoreHandle");
    layout<ConfigOverlayEditor>("ConfigOverlayEditor");
    layout<ConfigStoreOptions>("ConfigStoreOptions");
    layout<ConfigResult>("ConfigResult");
}
