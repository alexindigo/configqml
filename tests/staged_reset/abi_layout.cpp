// SPDX-License-Identifier: LGPL-3.0-or-later
#include <configdraft.h>
#include <configqmlstore.h>
#include <configstorehandle.h>
#include <configoverlayeditor.h>
#include <iostream>
using namespace configqml;
template <typename T> void layout(const char *name) {
    std::cout << name << ' ' << sizeof(T) << ' ' << alignof(T) << '\n';
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
