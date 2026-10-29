// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlvalue.h"
#include "configprojection_p.h"
#include <QJSValue>
#include <QJSValueIterator>

namespace configqml {
namespace {
class QmlInput final : public ConfigValueInput {
public:
    QmlInput(const QVariant &input, QList<QJSValue> &ancestors)
        : m_value(input), m_ancestors(ancestors) {
        if (m_value.metaType() == QMetaType::fromType<QJSValue>()) {
            m_js = m_value.value<QJSValue>();
            if (m_js.isQObject() || m_js.isQMetaObject() || m_js.isCallable() || m_js.isDate() ||
                m_js.isRegExp() || m_js.isError())
                m_error = "engine-bound-value";
            if (m_js.isObject())
                for (const auto &ancestor : ancestors)
                    if (m_js.strictlyEquals(ancestor))
                        m_error = "cyclic-value";
            if (!m_error.isEmpty())
                return;
            if (m_js.isArray())
                m_kind = Kind::List;
            else if (m_js.isObject()) {
                // Native Qt value wrappers expose their QVariant directly; plain
                // JS records stay engine views until the Core owning traversal.
                auto native = m_js.toVariant(QJSValue::RetainJSObjects);
                if (native.metaType() != QMetaType::fromType<QJSValue>()) {
                    m_value = native;
                    m_js = {};
                } else
                    m_kind = Kind::Map;
            } else {
                m_value = m_js.toVariant();
                m_js = {};
            }
        }
        if (!m_js.isObject()) {
            const auto id = m_value.metaType().id();
            if (id == QMetaType::QVariantList || id == QMetaType::QStringList) {
                m_kind = Kind::List;
                m_list = m_value.toList();
            } else if (id == QMetaType::QVariantMap) {
                m_kind = Kind::Map;
                m_map = m_value.toMap();
            }
        }
    }
    Kind kind() const override { return m_kind; }
    QVariant scalar() const override { return m_value; }
    qsizetype count() const override {
        if (m_js.isArray())
            return m_js.property("length").toUInt();
        return m_kind == Kind::List ? m_list.size() : m_map.size();
    }
    QString error() const override { return m_error; }
    void visit(const Visitor &visitor) const override {
        if (m_js.isObject())
            m_ancestors.append(m_js);
        if (m_js.isArray()) {
            const quint32 length = m_js.property("length").toUInt();
            for (quint32 i = 0; i < length; ++i) {
                QmlInput child(QVariant::fromValue(m_js.property(i)), m_ancestors);
                visitor(QString::number(i), child);
            }
        } else if (m_js.isObject()) {
            QJSValueIterator it(m_js);
            while (it.hasNext()) {
                it.next();
                QmlInput child(QVariant::fromValue(it.value()), m_ancestors);
                visitor(it.name(), child);
            }
        } else if (m_kind == Kind::List) {
            for (int i = 0; i < m_list.size(); ++i) {
                QmlInput child(m_list[i], m_ancestors);
                visitor(QString::number(i), child);
            }
        } else {
            for (auto it = m_map.cbegin(); it != m_map.cend(); ++it) {
                QmlInput child(it.value(), m_ancestors);
                visitor(it.key(), child);
            }
        }
        if (m_js.isObject())
            m_ancestors.removeLast();
    }

private:
    QVariant m_value;
    QJSValue m_js;
    Kind m_kind = Kind::Scalar;
    QVariantList m_list;
    QVariantMap m_map;
    QString m_error;
    QList<QJSValue> &m_ancestors;
};
} // namespace
ConfigValueResult captureQmlValue(const QVariant &value, const ConfigPath &path,
                                  const ConfigDiagnosticBuilder &diagnostics) {
    QList<QJSValue> ancestors;
    QmlInput input(value, ancestors);
    return captureValue(input, path, diagnostics);
}
ConfigAdmission admitQmlInput(const ConfigSchema &schema, const ConfigPath &path,
                              const QVariant &value) {
    const auto ownedPath = ownPath(path);
    QList<QJSValue> ancestors;
    QmlInput input(value, ancestors);
    return schema.admit(ownedPath, input, ConfigDescriptor::Format::Qml);
}
ConfigValueResult admitQmlValue(const ConfigDescriptor &d, const QVariant &input,
                                const ConfigPath &path) {
    QList<QJSValue> ancestors;
    QmlInput reader(input, ancestors);
    return d.validate(reader, path, ConfigDescriptor::Format::Qml);
}
QVariant qmlValue(const ConfigDescriptor &d, const QVariant &v) {
    return collectionPresentation(d, v);
}
QVariantMap qmlResult(const ConfigResult &input, const ConfigDiagnosticBuilder &diagnostics) {
    const auto r = diagnostics.sanitize(input);
    QVariantMap covered;
    for (auto it = r.coveredRevisions.cbegin(); it != r.coveredRevisions.cend(); ++it)
        covered[it.key()] = QString::fromLatin1(it->token);
    QVariantList publications;
    QVariantList masks;
    QVariantList coverage, evidence;
    for (const auto &item : r.coverage)
        coverage.append(QVariantMap{{"path", item.path},
                                    {"sourceId", item.sourceId},
                                    {"inputThrough", QString::number(item.inputThrough)},
                                    {"intentRevision", QString::number(item.intentRevision)}});
    for (const auto &item : r.evidence)
        evidence.append(QVariantMap{{"evidenceId", item.evidenceId},
                                    {"directory", item.directory},
                                    {"sourceId", item.sourceId},
                                    {"phase", item.phase},
                                    {"role", item.role},
                                    {"observationOrder", QString::number(item.observationOrder)}});
    for (auto it = r.masks.cbegin(); it != r.masks.cend(); ++it)
        masks.append(QVariantMap{{"path", it.key()}, {"maskedBy", it.value()}});
    for (const auto &p : r.publications)
        publications.append(QVariantMap{{"sourceId", p.sourceId},
                                        {"success", p.success},
                                        {"revision", QString::fromLatin1(p.revision.token)},
                                        {"publicationState", p.state},
                                        {"file", p.error.file},
                                        {"phase", p.error.operation},
                                        {"error", p.error.code}});
    return {{"requestId", QString::number(r.requestId)},
            {"storeId", r.storeId},
            {"operation", r.operation},
            {"status", r.status},
            {"inputPrefix", QString::number(r.inputPrefix)},
            {"coveredRevisions", covered},
            {"publications", publications},
            {"publicationState", r.publicationState},
            {"error", QVariantMap{{"code", r.error.code},
                                  {"sourceId", r.error.sourceId},
                                  {"file", r.error.file},
                                  {"path", r.error.path},
                                  {"line", r.error.line},
                                  {"operation", r.error.operation}}},
            {"maskedBy", r.maskedBy},
            {"masks", masks},
            {"coverage", coverage},
            {"evidence", evidence},
            {"policyId", r.policyId},
            {"metadataRevision", QString::number(r.metadataRevision)},
            {"groupToken", r.groupToken},
            {"requestKey", r.requestKey},
            {"capturePrefix", QString::number(r.capturePrefix)},
            {"dependencies", r.dependencies},
            {"attemptId", r.attemptId},
            {"attemptNumber", QString::number(r.attemptNumber)},
            {"unresolvedGroups", r.unresolvedGroups},
            {"excludedGroups", r.excludedGroups},
            {"description", r.description()}};
}
QVariantMap qmlGroupInfo(const ConfigGroupInfo &info, const ConfigDiagnosticBuilder &diagnostics) {
    if (!info.error.empty())
        return {{"error", QVariantMap{{"code", info.error.code}}}};
    QVariantList coverage, attempts;
    for (const auto &item : info.coverage)
        coverage.append(QVariantMap{{"path", item.path},
                                    {"sourceId", item.sourceId},
                                    {"inputThrough", QString::number(item.inputThrough)},
                                    {"intentRevision", QString::number(item.intentRevision)}});
    for (const auto &attempt : info.attempts) {
        const auto error = diagnostics.sanitize(attempt.error);
        attempts.append(QVariantMap{{"attemptId", attempt.attemptId},
                                    {"attemptNumber", QString::number(attempt.attemptNumber)},
                                    {"requestId", QString::number(attempt.requestId)},
                                    {"status", attempt.status},
                                    {"publicationState", attempt.publicationState},
                                    {"error", QVariantMap{{"code", error.code},
                                                          {"sourceId", error.sourceId},
                                                          {"operation", error.operation},
                                                          {"file", error.file},
                                                          {"path", error.path},
                                                          {"line", error.line}}}});
    }
    return {{"groupToken", info.groupToken}, {"requestKey", info.requestKey},
            {"state", info.state},           {"capturePrefix", QString::number(info.capturePrefix)},
            {"coverage", coverage},          {"dependencies", info.dependencies},
            {"attempts", attempts}};
}
} // namespace configqml
