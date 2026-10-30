// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configschema.h"
#include "configresult.h"

namespace configqml {
ConfigDiagnosticBuilder::ConfigDiagnosticBuilder(const ConfigDescriptor &root, ConfigPath base)
    : m_root(std::make_shared<const ConfigDescriptor>(root)), m_base(ownPath(base)) {}
ConfigDiagnosticBuilder ConfigDiagnosticBuilder::at(const ConfigPath &origin) const {
    auto result = *this;
    result.m_origin = ownPath(origin);
    return result;
}
ConfigPath ConfigDiagnosticBuilder::path(const ConfigPath &input) const {
    auto result = ownPath(m_origin + input);
    if (!m_root || !startsWith(result, m_base))
        return result;
    using T = ConfigDescriptor::Type;
    const ConfigDescriptor *node = m_root.get();
    bool sensitive = node->sensitive || node->type == T::SecretRef;
    for (int i = m_base.size(); i < result.size(); ++i) {
        bool content = true;
        const ConfigDescriptor *child = nullptr;
        if (node && (node->type == T::Group || node->type == T::Record)) {
            const auto field = node->fields.constFind(result[i]);
            if (field != node->fields.cend()) {
                content = false; // Authored names remain useful even under a sensitive group.
                child = &field.value();
            }
        } else if (node && (node->type == T::List || node->type == T::Map) &&
                   node->element.size() == 1)
            child = &node->element.first();
        sensitive |= child && (child->sensitive || child->type == T::SecretRef);
        if (sensitive && content)
            result[i] = QStringLiteral("<redacted>");
        node = child;
    }
    return result;
}
ConfigDiagnostic ConfigDiagnosticBuilder::make(const ConfigPath &input, const QString &code) const {
    return {path(input), ownString(code)};
}
ConfigValueResult ConfigDiagnosticBuilder::sanitize(ConfigValueResult result) const {
    auto absolute = *this;
    absolute.m_origin.clear();
    for (auto &diagnostic : result.errors)
        diagnostic = absolute.make(diagnostic.path, diagnostic.code);
    return result;
}
ConfigError ConfigDiagnosticBuilder::sanitize(ConfigError error) const {
    auto absolute = *this;
    absolute.m_origin.clear();
    error.path = absolute.path(error.path);
    return error;
}
ConfigResult ConfigDiagnosticBuilder::sanitize(ConfigResult result) const {
    result.error = sanitize(result.error);
    for (auto &publication : result.publications)
        publication.error = sanitize(publication.error);
    return result;
}
ConfigError ConfigDiagnosticBuilder::error(const ConfigDiagnostic &diagnostic,
                                           const QString &source, const QString &operation,
                                           const QString &file, qsizetype line) const {
    auto absolute = *this;
    absolute.m_origin.clear();
    const auto safe = absolute.make(diagnostic.path, diagnostic.code);
    return {safe.code, ownString(source), ownString(operation), ownString(file), safe.path, line};
}
} // namespace configqml
