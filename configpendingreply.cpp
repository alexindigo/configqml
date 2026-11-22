// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configpendingreply_p.h"
#include "configreadprojection_p.h"
#include "configobservation_p.h"
#include <QThread>

namespace configqml {
QString ConfigReplyError::code() const { return d ? d->code : QString(); }
QString ConfigReplyError::message() const { return d ? d->message : QString(); }
QString ConfigReplyError::phase() const { return d ? d->phase : QString(); }
bool ConfigReplyError::empty() const { return !d || d->code.isEmpty(); }
ConfigObservation ConfigReplyValue::generation() const {
    return d ? d->generation : ConfigObservation();
}
ConfigReadSnapshot ConfigReplyValue::snapshot() const {
    return d ? d->snapshot : ConfigReadSnapshot();
}
QList<ConfigReadResult> ConfigReplyValue::observations() const {
    return d ? d->observations : QList<ConfigReadResult>();
}
QList<ConfigPath> ConfigReplyValue::pending() const { return d ? d->pending : QList<ConfigPath>(); }
std::optional<ConfigResult> ConfigReplyValue::write() const { return d ? d->write : std::nullopt; }
ConfigPendingReply::ConfigPendingReply(QObject *parent)
    : QObject(parent), d(new ConfigPendingReplyState(this)) {}
ConfigPendingReply::~ConfigPendingReply() = default;
bool ConfigPendingReply::isFinished() const { return d->finished; }
bool ConfigPendingReply::isError() const { return !d->error.empty(); }
ConfigReplyError ConfigPendingReply::error() const { return d->error; }
ConfigReplyValue ConfigPendingReply::value() const { return d->value; }
ConfigPendingReplyState::ConfigPendingReplyState(ConfigPendingReply *reply) : q(reply) {}
ConfigPendingReply *ConfigPendingReplyState::refuse(const QString &code, const QString &phase,
                                                    QObject *parent) {
    auto *reply = new ConfigPendingReply(parent);
    reply->d->settle(code, phase);
    return reply;
}
ConfigPendingReply *ConfigPendingReplyState::read(ConfigReadProjection *initiator,
                                                  const ConfigObservation &after,
                                                  const QList<ConfigReadProjection *> &observe,
                                                  QObject *parent) {
    auto *reply = new ConfigPendingReply(parent);
    auto &state = *reply->d;
    state.views.append(initiator);
    for (auto *view : observe)
        state.views.append(view);
    for (const auto &view : state.views) {
        if (!view || view->thread() != reply->thread()) {
            state.settle("read-owner", "lifetime");
            return reply;
        }
    }
    // Capture and validate all exact view identities before dispatching any read.
    const auto origin = initiator ? initiator->observation() : ConfigObservation();
    for (qsizetype i = 0; i < observe.size(); ++i) {
        ConfigReadResultData empty;
        empty.status = "not_observed";
        state.captured.observations.append(ConfigObservationAccess::result(std::move(empty)));
    }
    for (auto *view : observe) {
        if (!view || !origin.valid() ||
            !ConfigObservationAccess::applicable(view->observation(), origin)) {
            state.settle("read-observation", "selection");
            return reply;
        }
    }
    for (const auto &view : state.views) {
        state.lifetimes.append(QObject::connect(view, &QObject::destroyed, reply, [reply] {
            reply->d->settle("read-owner", "lifetime");
        }));
    }
    QMetaObject::invokeMethod(
        reply, [reply, after] { reply->d->start(after); }, Qt::QueuedConnection);
    return reply;
}
void ConfigPendingReplyState::start(const ConfigObservation &after) {
    if (queued || finished)
        return;
    if (!views.first()) {
        settle("read-owner", "lifetime");
        return;
    }
    const QPointer<ConfigPendingReply> reply(q);
    views.first()->synchronize(after, q, [reply](const ConfigReadResult &result) {
        if (reply)
            reply->d->acquired(-1, result);
    });
}
void ConfigPendingReplyState::acquired(qsizetype index, const ConfigReadResult &result) {
    if (queued || finished)
        return;
    if (index < 0) {
        // A failed acquisition is last-good state, not a new initiating generation.
        if (result.status() == "observed") {
            captured.generation = result.observation();
            captured.snapshot = result.snapshot();
            captured.pending = result.pending();
        }
    } else
        captured.observations[index] = result;
    if (result.status() != "observed") {
        settle(result.error().code, "observation");
        return;
    }
    if (index >= 0) {
        if (--outstanding == 0)
            settle();
        return;
    }
    outstanding = views.size() - 1;
    if (!outstanding) {
        settle();
        return;
    }
    const QPointer<ConfigPendingReply> reply(q);
    for (qsizetype i = 1; i < views.size(); ++i) {
        if (!views[i]) {
            settle("read-owner", "lifetime");
            return;
        }
        views[i]->synchronize(captured.generation, q, [reply, i](const ConfigReadResult &read) {
            if (reply)
                reply->d->acquired(i - 1, read);
        });
    }
}
void ConfigPendingReplyState::settle(const QString &code, const QString &phase) {
    if (finished)
        return;
    // Lifetime loss can supersede a queued success, never a prior concrete error.
    if (!code.isEmpty() && error.empty())
        error.d = std::make_shared<const ConfigReplyErrorData>(ConfigReplyErrorData{
            ownString(code), QStringLiteral("configqml: ") + code, ownString(phase)});
    if (queued)
        return;
    queued = true;
    QMetaObject::invokeMethod(q, [reply = q] { reply->d->finish(); }, Qt::QueuedConnection);
}
void ConfigPendingReplyState::finish() {
    if (finished)
        return;
    if (error.empty())
        for (const auto &view : views)
            if (!ConfigReadProjectionState::available(view)) {
                settle("read-owner", "lifetime");
                break;
            }
    value.d = std::make_shared<const ConfigReplyValueData>(std::move(captured));
    for (const auto &connection : lifetimes)
        QObject::disconnect(connection);
    lifetimes.clear();
    views.clear();
    finished = true;
    emit q->finished(); // No continuation: the consumer may delete the reply here.
}
} // namespace configqml
