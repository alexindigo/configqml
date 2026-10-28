// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configstore.h"
#include <QUrl>
#include <limits>
inline configqml::ConfigStoreOptions
fileOptions(const QString &path,
            configqml::ConfigWatchMode mode = configqml::ConfigWatchMode::StartupSnapshot) {
    using namespace configqml;
    using D = ConfigDescriptor;
    using T = D::Type;
    ConfigStoreOptions o;
    auto nullable = D::leaf(T::String, QString("default"));
    nullable.nullable = true;
    o.schema =
        ConfigSchema(D::group({{"a", D::leaf(T::SafeInteger, 0)},
                               {"b", D::leaf(T::SafeInteger, 0)},
                               {"wide", D::leaf(T::Integer, qint64(0))},
                               {"unsigned", D::leaf(T::UnsignedInteger, quint64(0))},
                               {"url", D::leaf(T::Url, QUrl("https://example.test/default"))},
                               {"nullable", nullable},
                               {"items", D::list(D::leaf(T::SafeInteger, 0), {})}}));
    o.layers = {{"user",
                 ConfigLayerKind::File,
                 ConfigAdmissionPolicy::FieldFallback,
                 {{"json", {}, {}}},
                 true}};
    o.jsonFiles = {{"user", "json", path, true, mode}};
    o.autoSave = false;
    return o;
}
