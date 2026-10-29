// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configshutdown_p.h"
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QMutexLocker>
#include <cstdio>

namespace configqml {
void ConfigWorkerOwner::acknowledgeModels(const QMap<ConfigPath, quint64> &generations) {
    QMutexLocker lock(&m_mutex);
    if (m_terminalReceived || m_stopped || generations.isEmpty())
        return;
    auto *worker = m_worker;
    QMetaObject::invokeMethod(
        worker, [worker, generations] { worker->acknowledgeModels(generations); },
        Qt::QueuedConnection);
}
namespace {
bool persistence(const QString &operation) {
    return operation == "save" || operation == "flush" || operation == "close" ||
           operation == "autosave" || operation == "apply" || operation == "importProfile" ||
           operation == "saveGroup" || operation == "retryGroup";
}
class ShutdownRegistry {
public:
    static ShutdownRegistry &instance() {
        // Process lifetime, including a timed-out worker still finishing IO.
        static auto *registry = new ShutdownRegistry;
        return *registry;
    }
    void add(const std::shared_ptr<ConfigWorkerOwner> &owner) {
        bool exiting;
        {
            QMutexLocker lock(&m_mutex);
            auto *app = QCoreApplication::instance();
            if (m_app != app) {
                m_app = app;
                m_exiting = false;
                m_deadline.reset();
                m_reported.clear();
                QObject::connect(
                    app, &QCoreApplication::aboutToQuit, app, [] { instance().drain(); },
                    Qt::DirectConnection);
                qAddPostRoutine(&ShutdownRegistry::atExit);
            }
            m_workers.append(owner);
            exiting = m_exiting;
        }
        if (exiting)
            owner->settleForExit();
    }
    void retire(ConfigWorkerOwner *owner) {
        QMutexLocker lock(&m_mutex);
        for (auto it = m_workers.begin(); it != m_workers.end(); ++it)
            if (it->get() == owner) {
                m_reported.remove(owner);
                m_workers.erase(it);
                return;
            }
    }
    void drain() {
        QDeadlineTimer deadline;
        {
            QMutexLocker lock(&m_mutex);
            if (m_draining)
                return; // The outer registry drain retains the workers, not their consumers.
            m_draining = true;
            m_exiting = true;
            if (!m_deadline)
                m_deadline.emplace(5000);
            deadline = *m_deadline; // One application-wide budget, shared by quit and exit.
        }
        for (;;) {
            QList<std::shared_ptr<ConfigWorkerOwner>> workers;
            {
                QMutexLocker lock(&m_mutex);
                workers = m_workers;
            }
            if (workers.isEmpty())
                break;
            for (const auto &owner : workers) {
                owner->settleForExit();
                owner->pollJoin();
            }
            if (deadline.hasExpired()) {
                for (const auto &owner : workers) {
                    QMutexLocker lock(&m_mutex);
                    if (!owner->stopped() && !m_reported.contains(owner.get())) {
                        m_reported.insert(owner.get());
                        std::fprintf(stderr, "configqml: shutdown-timeout: %s\n",
                                     qPrintable(owner->description()));
                    }
                }
                break;
            }
            // Qt invokes post routines before destroying the application/event dispatcher.
            // This is also usable when main returns without ever entering exec().
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
        QMutexLocker lock(&m_mutex);
        m_draining = false;
    }

private:
    static void atExit() {
        auto &registry = instance();
        registry.drain();
        QMutexLocker lock(&registry.m_mutex);
        registry.m_app = nullptr;
    }
    QMutex m_mutex;
    QPointer<QCoreApplication> m_app;
    QList<std::shared_ptr<ConfigWorkerOwner>> m_workers;
    std::optional<QDeadlineTimer> m_deadline;
    QSet<ConfigWorkerOwner *> m_reported;
    bool m_exiting = false;
    bool m_draining = false;
};
} // namespace
ConfigWorkerOwner::ConfigWorkerOwner(ConfigStoreOptions options, const QString &error)
    : m_storeId(options.storeId), m_autoSave(options.autoSave) {
    auto *applicationThread = QCoreApplication::instance()->thread();
    m_thread = new QThread;
    m_thread->moveToThread(applicationThread);
    m_worker = new ConfigWorker(std::move(options), error);
    m_worker->moveToThread(m_thread);
    m_joinTimer = new QTimer(this);
    m_joinTimer->setInterval(1);
    connect(m_joinTimer, &QTimer::timeout, this, &ConfigWorkerOwner::pollJoin);
    connect(m_worker, &ConfigWorker::packetReady, this, &ConfigWorkerOwner::receive,
            Qt::QueuedConnection);
    connect(m_thread, &QThread::started, m_worker, &ConfigWorker::initialize);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, this, &ConfigWorkerOwner::pollJoin, Qt::QueuedConnection);
    moveToThread(applicationThread);
}
void ConfigWorkerOwner::start() {
    ShutdownRegistry::instance().add(shared_from_this());
    m_thread->start();
}
quint64 ConfigWorkerOwner::reserveRequest() {
    QMutexLocker lock(&m_mutex);
    return ++m_requestId;
}
bool ConfigWorkerOwner::accepting() const {
    QMutexLocker lock(&m_mutex);
    return !m_sealed;
}
bool ConfigWorkerOwner::dispatch(const ConfigCommand &command) {
    QMutexLocker lock(&m_mutex);
    if (m_sealed || !m_worker)
        return false;
    m_prefix = qMax(m_prefix, command.prefix);
    if (persistence(command.operation))
        ++m_pendingPersistence;
    if (command.operation == "close")
        m_terminalQueued = m_sealed = true;
    auto *worker = m_worker;
    QMetaObject::invokeMethod(
        worker, [worker, command] { worker->enqueue(command); }, Qt::QueuedConnection);
    return true;
}
void ConfigWorkerOwner::detach() { settle(true); }
void ConfigWorkerOwner::settleForExit() { settle(false); }
void ConfigWorkerOwner::settle(bool detached) {
    ConfigCommand command;
    bool notify = false;
    {
        QMutexLocker lock(&m_mutex);
        m_detached |= detached;
        if (m_terminalQueued || m_stopped)
            return;
        m_terminalQueued = m_sealed = true;
        const bool publish = m_autoSave || m_pendingPersistence > 0;
        command = {
            publish && !m_detached ? ++m_requestId : 0, m_prefix, publish ? "close" : "retire", {}};
        if (publish)
            ++m_pendingPersistence;
        notify = publish && !m_detached;
        auto *worker = m_worker;
        QMetaObject::invokeMethod(
            worker, [worker, command] { worker->enqueue(command); }, Qt::QueuedConnection);
    }
    if (notify)
        emit implicitClose(command);
}
void ConfigWorkerOwner::reclaim(std::shared_ptr<const ConfigSnapshot> snapshot) {
    QMutexLocker lock(&m_mutex);
    if (m_worker)
        QMetaObject::invokeMethod(
            m_worker, [snapshot = std::move(snapshot)] {}, Qt::QueuedConnection);
}
void ConfigWorkerOwner::receive(const ConfigWorkerPacket &packet) {
    auto keepAlive = shared_from_this();
    const bool terminal = packet.result && (packet.result->operation == "close" ||
                                            packet.result->operation == "retire");
    {
        QMutexLocker lock(&m_mutex);
        if (packet.result && persistence(packet.result->operation))
            --m_pendingPersistence;
        if (terminal) {
            m_terminalReceived = true;
            m_outcome = packet.result;
        }
    }
    if (terminal) {
        {
            QMutexLocker lock(&m_mutex);
            auto *worker = m_worker;
            m_worker = nullptr;
            QMetaObject::invokeMethod(worker, &ConfigWorker::abandon, Qt::QueuedConnection);
        }
        m_joinTimer->start();
    }
    // Retirement is owned before exposing arbitrary consumer reentry. Dispatch
    // and snapshot reclamation use this owner, never a possibly retired raw worker.
    if (!terminal || packet.result->operation == "close")
        emit packetReady(packet);
    if (terminal)
        pollJoin();
}
bool ConfigWorkerOwner::stopped() const {
    QMutexLocker lock(&m_mutex);
    return m_stopped;
}
QString ConfigWorkerOwner::description() const {
    QMutexLocker lock(&m_mutex);
    return QString("store=%1 prefix=%2 (publication/worker teardown unsettled)")
        .arg(m_storeId)
        .arg(m_prefix);
}
void ConfigWorkerOwner::pollJoin() {
    auto keepAlive = shared_from_this();
    std::optional<ConfigResult> failed;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_terminalReceived || m_stopped || !m_thread->wait(0))
            return;
        m_stopped = true;
        if (m_detached && m_outcome && m_outcome->operation == "close" &&
            m_outcome->status == "failed")
            failed = m_outcome;
    }
    m_joinTimer->stop();
    m_thread->deleteLater();
    if (failed)
        std::fprintf(
            stderr,
            "configqml: detached publication failed: store=%s prefix=%llu code=%s phase=%s\n",
            qPrintable(failed->storeId), static_cast<unsigned long long>(failed->inputPrefix),
            qPrintable(failed->error.code), qPrintable(failed->error.operation));
    emit joined();
    ShutdownRegistry::instance().retire(this);
}
} // namespace configqml
