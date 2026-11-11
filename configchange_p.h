// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configconsumer_p.h"
#include "configvalue.h"
#include <functional>
#include <memory>
#include <QProperty>

namespace configqml {
// Internal preparation, never a public signal or a connection-order convention.
// Invalidation callbacks may only update native caches. Property notification is
// a separate, guarded consumer-reentry stage after every cache is current.
class ConfigChangePublisher {
public:
    using Callback = std::function<void(const QList<ConfigPath> &)>;
    void add(QObject *context, Callback invalidate, Callback notify = {}, Callback bindings = {}) {
        m_entries.removeIf([](const Entry &entry) { return !entry.context; });
        m_entries.append({context, std::make_shared<bool>(true), std::move(invalidate),
                          std::move(notify), std::move(bindings)});
    }
    void remove(QObject *context) {
        for (const auto &entry : m_entries)
            if (entry.context == context)
                *entry.active = false;
        m_entries.removeIf(
            [context](const Entry &entry) { return !entry.context || entry.context == context; });
    }
    struct Change {
        ConfigChangePublisher *publisher;
        QList<ConfigPath> paths;
    };
    static bool prepare(QObject *owner, const QList<Change> &changes) {
        struct Delivery {
            Entry entry;
            QList<ConfigPath> paths;
        };
        QList<Delivery> deliveries;
        for (const auto &change : changes) {
            change.publisher->m_entries.removeIf([](const Entry &entry) { return !entry.context; });
            for (const auto &entry : change.publisher->m_entries)
                deliveries.append({entry, change.paths});
        }
        ConfigConsumerGuard boundary(owner);
        for (const auto &delivery : deliveries)
            if (delivery.entry.context && *delivery.entry.active && delivery.entry.invalidate)
                delivery.entry.invalidate(delivery.paths);
        {
            QScopedPropertyUpdateGroup group;
            for (const auto &delivery : deliveries) {
                if (delivery.entry.context && *delivery.entry.active && delivery.entry.bindings &&
                    !boundary.invoke([&] { delivery.entry.bindings(delivery.paths); }))
                    return false;
            }
        }
        for (const auto &delivery : deliveries)
            if (delivery.entry.context && *delivery.entry.active && delivery.entry.notify &&
                !boundary.invoke([&] { delivery.entry.notify(delivery.paths); }))
                return false;
        return boundary.alive();
    }

private:
    struct Entry {
        QPointer<QObject> context;
        std::shared_ptr<bool> active;
        Callback invalidate, notify, bindings;
    };
    QList<Entry> m_entries;
};
} // namespace configqml
