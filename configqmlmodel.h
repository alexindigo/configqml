// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configlistmodel.h"
#include <QtQml/qqmlregistration.h>

namespace configqml {
struct ConfigNodeRegistration {
    Q_GADGET
    QML_FOREIGN(ConfigNode)
    QML_VALUE_TYPE(configNode)
    QML_UNCREATABLE("Collection nodes are immutable snapshots")
};
struct ConfigListModelRegistration {
    Q_GADGET
    QML_FOREIGN(ConfigListModel)
    QML_ANONYMOUS
};
} // namespace configqml
