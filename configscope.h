// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configlayers.h"
#include <memory>

namespace configqml {
// Host-created handle. Opaque keys are appended as single segments, never parsed.
class ConfigScope {
public:
    ConfigScope(std::shared_ptr<ConfigLayers> owner, ConfigPath prefix = {}, bool writable = true);
    bool attached() const { return bool(m_owner); }
    void detach() { m_owner.reset(); }
    ConfigScope scope(const ConfigPath &relative, bool writable = true) const;
    std::optional<QVariant> value(const ConfigPath &relative) const;
    ConfigInfo info(const ConfigPath &relative) const;
    ConfigWriteResult setUser(const ConfigPath &relative, const QVariant &value);
    ConfigWriteResult reset(const ConfigPath &relative, const QString &targetLayer);
    ConfigWriteResult setRuntime(const ConfigPath &relative, const QVariant &value);
    ConfigWriteResult clearRuntime(const ConfigPath &relative);

private:
    ConfigWriteResult denied(const ConfigPath &relative) const;
    std::shared_ptr<ConfigLayers> m_owner;
    ConfigPath m_prefix;
    bool m_writable = false;
    ConfigDiagnosticBuilder m_diagnostics;
};

// Storage adapters can use these reversible, canonical single-component names.
std::optional<QString> encodeScopeKey(const QString &key);
std::optional<QString> decodeScopeKey(const QString &name);
} // namespace configqml
