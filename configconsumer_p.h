// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include <QPointer>
#include <QVarLengthArray>
#include <utility>

namespace configqml {
// One continuation boundary for synchronous consumer reentry. Keep this on the
// caller's stack, never in the emitter. After invoke() returns false, do not
// touch an emitter/dependency or continue its notification loop. Callback data
// must already be detached and signal arguments owned before invoking consumers.
class ConfigConsumerGuard {
public:
    explicit ConfigConsumerGuard(QObject *emitter, QObject *dependency = nullptr) {
        m_objects.append(QPointer<QObject>(emitter));
        if (dependency)
            m_objects.append(QPointer<QObject>(dependency));
    }
    bool alive() const {
        for (const auto &object : m_objects)
            if (!object)
                return false;
        return true;
    }
    template <typename Function> bool invoke(Function &&function) const {
        if (!alive())
            return false;
        std::forward<Function>(function)();
        return alive();
    }

private:
    QVarLengthArray<QPointer<QObject>, 2> m_objects;
};
} // namespace configqml
