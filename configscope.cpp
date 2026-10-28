// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configscope.h"
#include <QRegularExpression>

namespace configqml {
ConfigScope::ConfigScope(std::shared_ptr<ConfigLayers> owner, ConfigPath prefix, bool writable)
    : m_owner(std::move(owner)), m_prefix(std::move(prefix)), m_writable(writable) {
    const auto d = m_owner ? m_owner->schema().descriptor(m_prefix) : std::nullopt;
    if (!m_owner || !m_owner->valid() || !d || d->type != ConfigDescriptor::Type::Group)
        m_owner.reset();
}
ConfigScope ConfigScope::scope(const ConfigPath &relative, bool writable) const {
    return ConfigScope(m_owner, m_prefix + relative, m_writable && writable);
}
std::optional<QVariant> ConfigScope::value(const ConfigPath &relative) const {
    return m_owner ? m_owner->value(m_prefix + relative) : std::nullopt;
}
ConfigInfo ConfigScope::info(const ConfigPath &relative) const {
    auto result = m_owner ? m_owner->info(m_prefix + relative) : ConfigInfo{};
    if (!m_writable)
        result.route = {};
    return result;
}
ConfigWriteResult ConfigScope::denied(const ConfigPath &relative) const {
    return {
        false, {{m_prefix + relative, m_owner ? "read-only-scope" : "detached-scope"}}, {}, {}, 0};
}
ConfigWriteResult ConfigScope::setUser(const ConfigPath &relative, const QVariant &value) {
    return m_owner && m_writable ? m_owner->setUser(m_prefix + relative, value) : denied(relative);
}
ConfigWriteResult ConfigScope::reset(const ConfigPath &relative, const QString &targetLayer) {
    return m_owner && m_writable ? m_owner->reset(m_prefix + relative, targetLayer)
                                 : denied(relative);
}
ConfigWriteResult ConfigScope::setRuntime(const ConfigPath &relative, const QVariant &value) {
    return m_owner && m_writable ? m_owner->setRuntime(m_prefix + relative, value)
                                 : denied(relative);
}
ConfigWriteResult ConfigScope::clearRuntime(const ConfigPath &relative) {
    return m_owner && m_writable ? m_owner->clearRuntime(m_prefix + relative) : denied(relative);
}
std::optional<QString> encodeScopeKey(const QString &key) {
    if (key.isEmpty() || QString::fromUtf8(key.toUtf8()) != key)
        return std::nullopt;
    return QStringLiteral("k-") + QString::fromLatin1(key.toUtf8().toHex());
}
std::optional<QString> decodeScopeKey(const QString &name) {
    static const QRegularExpression grammar(QStringLiteral("\\Ak-(?:[0-9a-f]{2})+\\z"));
    if (!grammar.match(name).hasMatch())
        return std::nullopt;
    auto key = QString::fromUtf8(QByteArray::fromHex(name.mid(2).toLatin1()));
    return encodeScopeKey(key) == name ? std::optional<QString>(key) : std::nullopt;
}
} // namespace configqml
