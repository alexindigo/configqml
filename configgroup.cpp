// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configworker_p.h"
#include "configshutdown_p.h"
#include "configconsumer_p.h"
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QDir>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace configqml {
quint64 ConfigStore::applyGroup(const QMap<ConfigPath, std::optional<QVariant>> &edits,
                                const QMap<ConfigPath, ConfigPathState> &fences, QObject *context,
                                Callback callback) {
    const auto id = registerRequest(context, std::move(callback));
    ConfigInput input;
    input.kind = ConfigInput::Kind::Group;
    input.sequence = m_sequence + 1;
    input.edits = edits; // Private callers supply already-owned, schema-checked values.
    input.fences = fences;
    for (auto it = edits.cbegin(); it != edits.cend(); ++it)
        input.pathGenerations[it.key()] = m_cache.information.value(it.key()).pathGeneration + 1;
    if (!m_accepting || m_status != Status::Ready ||
        !m_owner->dispatch({id, input.sequence, "apply", input})) {
        ConfigResult result;
        result.requestId = id;
        result.storeId = m_storeId;
        result.operation = "apply";
        result.status = "failed";
        result.error = {"store-closed", {}, "apply"};
        QMetaObject::invokeMethod(
            this, [this, result] { receiveResult(result); }, Qt::QueuedConnection);
        return id;
    }
    m_sequence = input.sequence;
    for (auto it = input.pathGenerations.cbegin(); it != input.pathGenerations.cend(); ++it) {
        auto &state = m_cache.information[it.key()];
        state.latestInput = input.sequence;
        state.pathGeneration = it.value();
    }
    m_inputs.append(input);
    return id;
}
quint64 ConfigStore::profileRequest(const QString &operation, const QString &name,
                                    const QString &file, const QList<ConfigPath> &paths,
                                    QObject *context, Callback callback) {
    const auto id = registerRequest(context, std::move(callback));
    ConfigCommand command{id, m_sequence + (operation == "importProfile" ? 1 : 0), operation, {}};
    command.profileName = ownString(name);
    command.file = ownString(file);
    for (const auto &path : paths)
        command.selection.append(ownPath(path));
    if (!m_accepting || m_status != Status::Ready || !m_owner->dispatch(command)) {
        ConfigResult result;
        result.requestId = id;
        result.storeId = m_storeId;
        result.operation = operation;
        result.status = "failed";
        result.error = {"not-ready", {}, operation};
        QMetaObject::invokeMethod(
            this, [this, result] { receiveResult(result); }, Qt::QueuedConnection);
    } else if (operation == "importProfile")
        m_sequence = command.prefix;
    return id;
}
quint64 ConfigStore::exportProfile(const QString &name, const QString &file,
                                   const QList<ConfigPath> &paths, QObject *context,
                                   Callback callback) {
    return profileRequest("exportProfile", name, file, paths, context, std::move(callback));
}
quint64 ConfigStore::importProfile(const QString &file, const QList<ConfigPath> &paths,
                                   QObject *context, Callback callback) {
    return profileRequest("importProfile", {}, file, paths, context, std::move(callback));
}
void ConfigWorker::processGroup(const ConfigCommand &command) {
    const auto &input = *command.input;
    QString error;
    if (!input.fences.isEmpty() && !m_files.isEmpty() &&
        !reloadFiles(ConfigCommand{0, m_prefix, "reload", {}}))
        error = "draft-refresh";
    for (auto it = input.fences.cbegin(); it != input.fences.cend(); ++it)
        if (m_pathIntents.value(it.key()) != it->intentRevision ||
            m_contentRevisions.value(it.key()) != it->contentRevision ||
            m_pathGenerations.value(it.key()) != it->pathGeneration)
            error = "draft-conflict";
    coverInput(command); // Rejection also settles its path generations.
    auto candidate = *m_layers;
    const auto before = *m_layers;
    for (auto it = input.edits.cbegin(); error.isEmpty() && it != input.edits.cend(); ++it) {
        error = quarantined(it.key());
        if (!error.isEmpty())
            break;
        ConfigInput leaf;
        leaf.kind = it.value() ? ConfigInput::Kind::User : ConfigInput::Kind::Reset;
        leaf.path = it.key();
        leaf.value = it.value().value_or(QVariant());
        leaf.sequence = input.sequence;
        if (m_options.admission)
            error = m_options.admission(leaf);
        if (!error.isEmpty())
            break;
    }
    if (error.isEmpty()) {
        const auto outcome = candidate.applyEdits(input.edits);
        if (!outcome.accepted)
            error = outcome.errors.isEmpty() ? QString("admission") : outcome.errors.first().code;
    }
    if (!error.isEmpty()) {
        auto result = resultFor(command);
        result.status = "failed";
        result.error = {error, {}, "apply"};
        result.rejectedPathGenerations = input.pathGenerations;
        post(publish(result));
        return;
    }
    *m_layers = std::move(candidate);
    acceptIntent(input);
    recordPersistent(input, before);
    beginSave(command);
}
static std::optional<QVariant> portableValue(const ConfigDescriptor &descriptor,
                                             const QVariant &raw) {
    using T = ConfigDescriptor::Type;
    if (descriptor.sensitive || descriptor.forbidden || descriptor.machineLocal ||
        descriptor.type == T::SecretRef)
        return {};
    QVariant projected = raw;
    // Raw data is not admitted data. Check container shape before traversing and
    // whitelist record members, then admit every projected value through its descriptor.
    if (!isNull(raw) && descriptor.type == T::Record) {
        if (raw.metaType().id() != QMetaType::QVariantMap)
            return {};
        QVariantMap result;
        const auto map = raw.toMap();
        for (auto it = descriptor.fields.cbegin(); it != descriptor.fields.cend(); ++it)
            if (map.contains(it.key())) {
                const auto child = portableValue(it.value(), map.value(it.key()));
                if (!child)
                    return {};
                result[it.key()] = *child;
            }
        projected = result;
    } else if (!isNull(raw) && descriptor.type == T::List) {
        if (raw.metaType().id() != QMetaType::QVariantList || descriptor.element.size() != 1)
            return {};
        QVariantList result;
        for (const auto &item : raw.toList()) {
            const auto child = portableValue(descriptor.element.first(), item);
            if (!child)
                return {};
            result.append(*child);
        }
        projected = result;
    } else if (!isNull(raw) && descriptor.type == T::Map) {
        if (raw.metaType().id() != QMetaType::QVariantMap || descriptor.element.size() != 1)
            return {};
        auto result = raw.toMap();
        for (auto &item : result) {
            const auto child = portableValue(descriptor.element.first(), item);
            if (!child)
                return {};
            item = *child;
        }
        projected = result;
    }
    if (!descriptor.validate(jsonSettingValue(projected, descriptor)).ok())
        return {};
    return projected; // Retain the raw numeric/URL representation, not normalized admission.
}
static QString readProfile(const QString &path, qint64 limit, QByteArray &bytes) {
    if (limit <= 0)
        return "profile-size-limit";
    struct stat state;
    const auto name = QFile::encodeName(path);
    if (::stat(name.constData(), &state) < 0)
        return "profile-read";
    if (!S_ISREG(state.st_mode))
        return "profile-file-type";
    // Nonblocking open prevents a replacement FIFO from blocking before fstat.
    const auto fd = ::open(name.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return "profile-read";
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return "profile-read";
    }
    if (::fstat(fd, &state) < 0)
        return "profile-read";
    if (!S_ISREG(state.st_mode))
        return "profile-file-type";
    if (state.st_size > limit)
        return "profile-too-large";
    char buffer[65536];
    for (;;) {
        const auto remaining = limit - bytes.size();
        // At the boundary probe one byte without growing the admitted buffer.
        const auto count =
            file.read(buffer, qMin<qint64>(sizeof(buffer), remaining ? remaining : 1));
        if (count < 0)
            return "profile-read";
        if (count == 0)
            return {};
        if (count > remaining)
            return "profile-too-large";
        bytes.append(buffer, count);
    }
}
void ConfigWorker::profile(const ConfigCommand &command) {
    auto result = resultFor(command);
    const auto fail = [&](const QString &code) {
        if (command.operation == "importProfile") {
            auto covered = command;
            ConfigInput input;
            input.sequence = command.prefix;
            covered.input = input;
            coverInput(covered);
        }
        result.status = "failed";
        result.error = {code, {}, command.operation, command.file};
        post(publish(result));
    };
    if (!QFileInfo(command.file).isAbsolute() || QDir::cleanPath(command.file) != command.file) {
        fail("profile-path");
        return;
    }
    if (command.operation == "exportProfile")
        for (const auto &file : m_options.jsonFiles) {
            const QFileInfo destination(command.file);
            const QFileInfo source(m_json->path(file.sourceId));
            if (destination.absoluteFilePath() == source.absoluteFilePath() ||
                (!destination.canonicalFilePath().isEmpty() &&
                 destination.canonicalFilePath() == source.canonicalFilePath())) {
                fail("profile-is-config-source");
                return;
            }
        }
    for (const auto &prefix : command.selection)
        if (!m_options.schema.descriptor(prefix)) {
            fail("undeclared-path");
            return;
        }
    QSet<ConfigPath> selected;
    for (const auto &path : m_options.schema.leaves()) {
        bool included = command.selection.isEmpty();
        for (const auto &prefix : command.selection)
            included |= startsWith(path, prefix);
        if (included && m_options.schema.portable(path) && m_layers->info(path).route.valid())
            selected.insert(path);
    }
    ConfigJsonCodec codec;
    if (command.operation == "exportProfile") {
        if (command.profileName.isEmpty()) {
            fail("profile-name");
            return;
        }
        QVariantList entries;
        for (const auto &path : m_options.schema.leaves()) {
            if (!selected.contains(path))
                continue;
            const auto route = m_layers->info(path).route;
            const auto source = m_layers->source(route.layerId, route.sourceId);
            const auto raw = valueAt(source->raw, path.mid(route.prefix.size()));
            QVariantMap entry{{"path", path}, {"present", bool(raw)}};
            if (raw) {
                const auto value = portableValue(*m_options.schema.descriptor(path), *raw);
                if (!value) {
                    fail("profile-admission");
                    return;
                }
                entry["value"] = *value;
            }
            entries.append(entry);
        }
        const auto bytes = codec.encode(
            {{"format", 1}, {"name", command.profileName}, {"entries", entries}}, "profile");
        QSaveFile file(command.file);
        file.setDirectWriteFallback(false);
        if (!bytes.ok() || !file.open(QIODevice::WriteOnly) ||
            !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
            file.write(bytes.bytes) != bytes.bytes.size() || !file.commit()) {
            fail("profile-export");
            return;
        }
        result.status = "exported";
        post(publish(result));
        return;
    }
    QByteArray profileBytes;
    const auto readError = readProfile(command.file, m_options.profileMaxBytes, profileBytes);
    if (!readError.isEmpty()) {
        fail(readError);
        return;
    }
    const auto decoded = codec.decode(profileBytes, "profile");
    const auto format = decoded.raw.value("format");
    if (!decoded.ok() || encodeJsonValue(format).bytes != "1" ||
        decoded.raw.value("name").metaType().id() != QMetaType::QString ||
        decoded.raw.value("name").toString().isEmpty() ||
        decoded.raw.value("entries").metaType().id() != QMetaType::QVariantList) {
        fail("profile-format");
        return;
    }
    ConfigInput input;
    input.kind = ConfigInput::Kind::Group;
    input.sequence = command.prefix;
    QSet<ConfigPath> seen;
    for (const auto &item : decoded.raw.value("entries").toList()) {
        const auto entry = item.toMap();
        ConfigPath path;
        const auto rawPath = entry.value("path");
        if (rawPath.metaType().id() != QMetaType::QVariantList) {
            fail("profile-entry");
            return;
        }
        for (const auto &segment : rawPath.toList()) {
            if (segment.metaType().id() != QMetaType::QString) {
                fail("profile-entry");
                return;
            }
            path.append(segment.toString());
        }
        if (seen.contains(path) || !m_options.schema.portable(path) ||
            entry.value("present").metaType().id() != QMetaType::Bool) {
            fail("profile-entry");
            return;
        }
        seen.insert(path);
        if (!selected.contains(path))
            continue;
        if (!entry.value("present").toBool())
            input.edits[path] = std::nullopt;
        else {
            const auto descriptor = *m_options.schema.descriptor(path);
            const auto raw = entry.value("value");
            const auto checked = m_options.schema.validate(path, jsonSettingValue(raw, descriptor));
            const auto portable = portableValue(descriptor, raw);
            if (!checked.ok() || !portable || *portable != raw) {
                fail("profile-admission");
                return;
            }
            input.edits[path] = checked.value;
        }
    }
    auto apply = command;
    apply.input = input;
    processGroup(apply);
}
} // namespace configqml
