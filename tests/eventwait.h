// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include <QEventLoop>
#include <QPointer>

namespace configqml::test {
// Await an asynchronous outcome, not a filesystem-throughput promise. The test
// process/runner still bounds hangs; timing contracts have their own explicit
// budget assertions. Connect before checking to retain already-delivered events.
template <typename Sender, typename Signal, typename Predicate>
bool awaitEvent(Sender *sender, Signal signal, Predicate complete) {
    if (!sender)
        return false;
    QEventLoop loop;
    const QPointer<Sender> alive(sender);
    QObject::connect(sender, signal, &loop, [&] {
        if (complete())
            loop.quit();
    });
    QObject::connect(sender, &QObject::destroyed, &loop, &QEventLoop::quit);
    if (!complete())
        loop.exec();
    return alive && complete();
}
} // namespace configqml::test
