// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configworktiming_p.h"
#include <QDebug>
#include <QVariant>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

namespace configqml {
namespace {
constexpr auto propertyName = "_configqml_work_timing";
qint64 nanos(const timespec &t) { return qint64(t.tv_sec) * 1000000000 + t.tv_nsec; }
bool accountingEnabled() {
    const int fd = ::open("/proc/sys/kernel/sched_schedstats", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    char value = 0;
    const auto count = ::read(fd, &value, 1);
    ::close(fd);
    return count == 1 && value == '1';
}
} // namespace
ConfigThreadWorkTimer::Stamp ConfigThreadWorkTimer::capture() {
    Stamp stamp;
    timespec wall{}, cpu{};
    rusage usage{};
    if (::getrusage(RUSAGE_THREAD, &usage) != 0 ||
        ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) != 0 ||
        ::clock_gettime(CLOCK_MONOTONIC, &wall) != 0)
        return stamp;
    stamp.wall = nanos(wall);
    stamp.cpu = nanos(cpu);
    stamp.voluntary = usage.ru_nvcsw;
    stamp.involuntary = usage.ru_nivcsw;
    stamp.tid = ::gettid();
    stamp.valid = true;
    char text[160]{};
    const int fd = ::open("/proc/thread-self/schedstat", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        stamp.unmeasured = "schedstat-unreadable";
        return stamp;
    }
    const auto count = ::read(fd, text, sizeof(text) - 1);
    ::close(fd);
    unsigned long long runtime = 0, wait = 0, slices = 0;
    if (count <= 0 || std::sscanf(text, "%llu %llu %llu", &runtime, &wait, &slices) != 3) {
        stamp.unmeasured = "schedstat-unreadable";
        return stamp;
    }
    stamp.runqueue = qint64(wait);
    stamp.runqueueValid = true;
    return stamp;
}
ConfigThreadWorkTimer::ConfigThreadWorkTimer() : m_start(capture()) {
    if (m_start.runqueueValid && !accountingEnabled()) {
        // Without schedstats the kernel does not account run-queue delay.
        m_start.runqueueValid = false;
        m_start.unmeasured = "kernel.sched_schedstats=0";
    }
}
ConfigThreadWorkSample ConfigThreadWorkTimer::finish() const {
    const auto end = capture();
    ConfigThreadWorkSample result;
    result.valid = m_start.valid && end.valid && end.tid == m_start.tid &&
                   end.wall >= m_start.wall && end.cpu >= m_start.cpu &&
                   end.voluntary >= m_start.voluntary && end.involuntary >= m_start.involuntary;
    result.runqueueMeasured = result.valid && m_start.runqueueValid && end.runqueueValid &&
                              end.runqueue >= m_start.runqueue;
    if (!result.runqueueMeasured)
        result.unmeasured = m_start.unmeasured ? m_start.unmeasured
                            : end.unmeasured   ? end.unmeasured
                            : !result.valid    ? "invalid-sample"
                                               : "schedstat-regressed";
    if (result.valid) {
        result.wall = end.wall - m_start.wall;
        result.cpu = end.cpu - m_start.cpu;
        result.runqueue = result.runqueueMeasured ? end.runqueue - m_start.runqueue : 0;
        result.voluntary = end.voluntary - m_start.voluntary;
        result.involuntary = end.involuntary - m_start.involuntary;
    }
    return result;
}
bool ConfigThreadWorkSample::within(qint64 budget) const {
    if (!valid || cpu >= budget)
        return false;
    if (runqueueMeasured)
        return charged() < budget;
    // Explicit, never silent: the CPU gate passed; the blocked gate was not run.
    qInfo("CALLER_WORK_BLOCKED_NOT_MEASURED budget_ns=%lld cpu_ns=%lld wall_ns=%lld reason=%s",
          budget, cpu, wall, unmeasured ? unmeasured : "unknown");
    return true;
}
void ConfigThreadWorkSample::report(const char *label) const {
    if (!runqueueMeasured) {
        qInfo("CALLER_WORK %s wall_ns=%lld cpu_ns=%lld runqueue_ns=unmeasured "
              "blocked_ns=unmeasured charged_ns=unmeasured voluntary=%lld involuntary=%lld "
              "valid=%d runqueue_unmeasured=%s",
              label, wall, cpu, voluntary, involuntary, valid, unmeasured ? unmeasured : "unknown");
        return;
    }
    qInfo("CALLER_WORK %s wall_ns=%lld cpu_ns=%lld runqueue_ns=%lld blocked_ns=%lld "
          "charged_ns=%lld voluntary=%lld involuntary=%lld valid=%d",
          label, wall, cpu, runqueue, blocked(), charged(), voluntary, involuntary, valid);
}
ConfigThreadWorkSample &ConfigThreadWorkSample::operator+=(const ConfigThreadWorkSample &other) {
    valid &= other.valid;
    if (runqueueMeasured && !other.runqueueMeasured)
        unmeasured = other.unmeasured;
    runqueueMeasured &= other.runqueueMeasured;
    wall += other.wall;
    cpu += other.cpu;
    runqueue += other.runqueue;
    voluntary += other.voluntary;
    involuntary += other.involuntary;
    return *this;
}
ConfigThreadWorkSample operator+(ConfigThreadWorkSample a, const ConfigThreadWorkSample &b) {
    return a += b;
}
ConfigThreadWorkSample operator-(const ConfigThreadWorkSample &a, const ConfigThreadWorkSample &b) {
    ConfigThreadWorkSample result{qMax(qint64(0), a.wall - b.wall),
                                  qMax(qint64(0), a.cpu - b.cpu),
                                  qMax(qint64(0), a.runqueue - b.runqueue),
                                  qMax(qint64(0), a.voluntary - b.voluntary),
                                  qMax(qint64(0), a.involuntary - b.involuntary),
                                  a.valid && b.valid};
    result.runqueueMeasured = a.runqueueMeasured && b.runqueueMeasured;
    result.unmeasured = !a.runqueueMeasured ? a.unmeasured : b.unmeasured;
    return result;
}
ConfigWorkTiming *ConfigWorkTiming::get(QObject *owner) {
    return owner ? static_cast<ConfigWorkTiming *>(owner->property(propertyName).value<QObject *>())
                 : nullptr;
}
void ConfigWorkTiming::enable(QObject *owner) {
    if (!get(owner))
        owner->setProperty(propertyName,
                           QVariant::fromValue<QObject *>(new ConfigWorkTiming(owner)));
}
ConfigWorkPublication::ConfigWorkPublication(QObject *owner, bool models)
    : ConfigWorkPublication(owner, 0, 0) {
    m_models = models;
}
ConfigWorkPublication::ConfigWorkPublication(QObject *owner, quint64 delivery, quint64 request)
    : m_state(ConfigWorkTiming::get(owner)), m_delivery(delivery), m_request(request) {
    if (m_state) {
        m_frame = std::make_shared<ConfigWorkFrame>();
        m_state->frames.append(m_frame);
        m_timer.emplace();
    }
}
ConfigWorkPublication::~ConfigWorkPublication() {
    if (!m_state)
        return;
    const auto sample = m_timer->finish() - m_frame->notifications;
    const auto notifications = m_frame->samples;
    Q_ASSERT(!m_state->frames.isEmpty() && m_state->frames.last() == m_frame);
    m_state->frames.removeLast();
    m_state->validMeasurements &= sample.valid;
    if (m_models) {
        // One request can install models before persistence and again at
        // settlement. Charge both, without replacing the earlier work.
        if (m_request && m_state->lastModelRequest == m_request)
            m_state->lastModel += sample;
        else if (m_delivery >= m_state->lastModelDelivery) {
            m_state->lastModel = sample;
            m_state->lastModelRequest = m_request;
        }
        m_state->lastModelDelivery = qMax(m_state->lastModelDelivery, m_delivery);
    }
    if (!m_state->worst.valid || sample.charged() > m_state->worst.charged())
        m_state->worst = sample;
    m_state->lastModel.valid &= m_state->validMeasurements;
    m_state->worst.valid &= m_state->validMeasurements;
    // Reporting is outside every measured interval, including notifications.
    for (const auto &notification : notifications)
        notification.report("model-notification");
    sample.report("publication-library");
}
} // namespace configqml
