// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configjsonbackend.h"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace configqml {
namespace {
struct Fd {
    int fd = -1;
    explicit Fd(int fd = -1) : fd(fd) {}
    ~Fd() {
        if (fd >= 0)
            ::close(fd);
    }
};
QByteArray stamp(const struct stat &s) {
    return QByteArray::number(s.st_dev) + ':' + QByteArray::number(s.st_ino) + ':' +
           QByteArray::number(s.st_size) + ':' + QByteArray::number(s.st_mtim.tv_sec) + ':' +
           QByteArray::number(s.st_mtim.tv_nsec) + ':' + QByteArray::number(s.st_ctim.tv_sec) +
           ':' + QByteArray::number(s.st_ctim.tv_nsec);
}
QByteArray stamp(const QString &path) {
    struct stat s;
    if (::lstat(QFile::encodeName(path).constData(), &s) < 0)
        return errno == ENOENT ? QByteArray("missing") : QByteArray("stat-error");
    return stamp(s);
}
bool locked(int fd) {
    QElapsedTimer timer;
    timer.start();
    do {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0)
            return true;
        if (errno != EWOULDBLOCK && errno != EINTR)
            return false;
        QThread::msleep(2);
    } while (timer.elapsed() < 2000);
    return false;
}
int openLock(int directory, const QString &name) {
    if (directory < 0)
        return -1;
    const auto fd =
        ::openat(directory, QFile::encodeName(QFileInfo(name).fileName() + ".lock").constData(),
                 O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat s;
    if (fd < 0)
        return -1;
    if (::fstat(fd, &s) < 0 || !S_ISREG(s.st_mode) || s.st_nlink != 1 || !locked(fd)) {
        ::close(fd);
        return -1;
    }
    return fd;
}
bool same(const std::optional<QVariant> &a, const std::optional<QVariant> &b) {
    if (bool(a) != bool(b))
        return false;
    if (!a)
        return true;
    // Compare lossless JSON representations, not QVariant's numeric coercions.
    return encodeJsonValue(*a).bytes == encodeJsonValue(*b).bytes;
}
bool compatibleAncestors(const QVariantMap &base, const QVariantMap &fresh, const ConfigPath &p) {
    for (qsizetype i = 1; i < p.size(); ++i) {
        const auto a = valueAt(base, p.mid(0, i));
        const auto b = valueAt(fresh, p.mid(0, i));
        if (b && b->metaType().id() != QMetaType::QVariantMap && !same(a, b))
            return false;
    }
    return true;
}
ConfigError checkFileIdentity(const QString &id, const QString &name, bool writing) {
    struct stat s;
    if (::lstat(QFile::encodeName(name).constData(), &s) == 0) {
        if (!S_ISREG(s.st_mode))
            return {"unsupported-file-identity", id, "path", name};
        if (writing && s.st_nlink != 1)
            return {"hard-linked-target", id, "publish", name};
    } else if (errno != ENOENT)
        return {"stat-failed", id, "path", name};
    return {};
}
} // namespace
bool applyFileEdit(QVariantMap &raw, const ConfigFileEdit &edit) {
    const auto current = valueAt(raw, edit.path);
    if (edit.ifMissing && current)
        return false;
    if (edit.pruneEmpty && (!current || current->metaType().id() != QMetaType::QVariantMap ||
                            !current->toMap().isEmpty()))
        return false;
    if (edit.value && edit.path.isEmpty())
        raw = edit.value->toMap();
    else if (edit.value)
        putValue(raw, edit.path, *edit.value);
    else if (edit.path.isEmpty())
        raw.clear();
    else
        removeValue(raw, edit.path);
    return true;
}
ConfigJsonBackend::ConfigJsonBackend(const QList<ConfigJsonFile> &files, FaultHook hook,
                                     const QMap<QString, ConfigDiagnosticBuilder> &diagnostics)
    : m_files(files), m_hook(std::move(hook)) {
    for (auto it = diagnostics.cbegin(); it != diagnostics.cend(); ++it)
        m_diagnostics.insert(ownString(it.key()), it.value());
    for (auto &file : m_files) {
        file.layerId = ownString(file.layerId);
        file.sourceId = ownString(file.sourceId);
        file.path = ownString(file.path);
        if (file.path.isEmpty())
            file.path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                        "/config.json";
    }
}
ConfigBackendCapabilities ConfigJsonBackend::capabilities() const {
    ConfigBackendCapabilities c;
    c.write = c.watch = c.conditionalPublish = true;
    return c;
}
QString ConfigJsonBackend::path(const QString &id) const { return resolve(id).path; }
ConfigJsonBackend::Target ConfigJsonBackend::resolve(const QString &id) const {
    QString configured;
    for (const auto &file : m_files)
        if (file.sourceId == id)
            configured = file.path;
    return resolvePath(configured, id);
}
ConfigJsonBackend::Target ConfigJsonBackend::resolvePath(const QString &configured,
                                                         const QString &id) {
    Target result;
    auto fail = [&](const QString &code) {
        result.error = {code, id, "path", configured};
        return result;
    };
    if (!QDir::isAbsolutePath(configured) || QDir::cleanPath(configured) != configured)
        return fail("absolute-clean-path-required");
    auto pending = configured.mid(1).split('/');
    QStringList parts;
    int followed = 0;
    while (!pending.isEmpty()) {
        const auto segment = pending.takeFirst();
        if (segment.isEmpty() || segment == ".")
            continue;
        if (segment == "..") {
            if (!parts.isEmpty())
                parts.removeLast();
            continue;
        }
        const auto candidate = "/" + (parts + QStringList{segment}).join('/');
        struct stat s;
        if (::lstat(QFile::encodeName(candidate).constData(), &s) == 0) {
            if (S_ISLNK(s.st_mode)) {
                result.identity += QFile::encodeName(candidate).toHex() + ':' + stamp(s) + ';';
                if (++followed > 40)
                    return fail("symlink-loop");
                result.links.append(candidate);
                QByteArray bytes(4096, '\0');
                const auto length = ::readlink(QFile::encodeName(candidate).constData(),
                                               bytes.data(), bytes.size());
                if (length < 0 || length == bytes.size())
                    return fail("symlink-read-failed");
                bytes.resize(length);
                const auto target = QFile::decodeName(bytes);
                if (QDir::isAbsolutePath(target))
                    parts.clear();
                pending = target.split('/') + pending;
                continue;
            }
            if (!pending.isEmpty() && !S_ISDIR(s.st_mode))
                return fail("not-a-directory");
            if (S_ISDIR(s.st_mode))
                result.identity += QFile::encodeName(candidate).toHex() + ':' +
                                   QByteArray::number(s.st_dev) + ':' +
                                   QByteArray::number(s.st_ino) + ';';
        } else if (errno != ENOENT)
            return fail("stat-failed");
        parts.append(segment);
    }
    result.path = "/" + parts.join('/');
    return result;
}
QStringList ConfigJsonBackend::watchPaths(const QString &id) const {
    const auto target = resolve(id);
    QStringList paths;
    if (!target.path.isEmpty())
        paths.append(target.path);
    for (const auto &link : target.links)
        paths.append(QFileInfo(link).absolutePath());
    return paths;
}
QString ConfigJsonBackend::fault(const QString &phase, const QString &target) const {
    return m_hook ? m_hook(phase, target) : QString();
}
ConfigError ConfigJsonBackend::checkTarget(const QString &id, const QString &name,
                                           bool writing) const {
    const auto identity = checkFileIdentity(id, name, writing);
    if (!identity.empty())
        return identity;
    for (const auto &other : m_files) {
        if (other.sourceId != id && resolve(other.sourceId).path == name)
            return {"duplicate-file-identity", id, "path", name};
    }
    return {};
}
ConfigError ConfigJsonBackend::exportProfile(const QString &configured, const QByteArray &bytes) {
    const QString id = "profile";
    if (QThread::currentThread() != m_owner)
        return {"wrong-thread", id, "exportProfile", configured};
    const auto target = resolvePath(configured, id);
    if (!target.error.empty())
        return target.error;
    // Resolution and exclusion belong to this storage owner. The writer receives
    // only the checked final target, never the original alias (including dangling links).
    const auto validate = [&]() -> ConfigError {
        const auto current = resolvePath(configured, id);
        if (!current.error.empty())
            return current.error;
        if (current.path != target.path || current.identity != target.identity)
            return {"target-changed", id, "exportProfile", configured};
        for (const auto &file : m_files) {
            const auto source = resolve(file.sourceId);
            if (!source.error.empty())
                return source.error;
            if (source.path == target.path)
                return {"profile-is-config-source", id, "exportProfile", configured};
        }
        return checkFileIdentity(id, target.path, true);
    };
    auto error = validate();
    if (!error.empty())
        return error;
    const auto original = stamp(target.path);
    QSaveFile file(target.path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(bytes) != bytes.size())
        return {"profile-export", id, "exportProfile", configured};
    error = validate();
    if (!error.empty())
        return error;
    if (stamp(target.path) != original)
        return {"target-changed", id, "exportProfile", configured};
    if (!file.commit())
        return {"profile-export", id, "exportProfile", configured};
    return {};
}
ConfigBackendResult ConfigJsonBackend::read(const QString &id, const QString &expectedTarget) {
    ConfigBackendResult r;
    r.sourceId = id;
    const auto target = resolve(id);
    const auto name = target.path;
    r.error = target.error;
    if (r.error.empty() && !expectedTarget.isEmpty() && name != expectedTarget)
        r.error = {"target-changed", id, "preflight", name};
    if (r.error.empty())
        r.error = checkTarget(id, name, false);
    if (!r.error.empty())
        return r;
    Fd fd(::open(QFile::encodeName(name).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd.fd < 0) {
        if (errno == ENOENT) {
            r.success = true;
            r.observedPresence = false;
            r.revision = {false, QFile::encodeName(name).toHex() + "|missing||" + target.identity};
        } else
            r.error = {"open-failed", id, "read", name};
        return r;
    }
    struct stat before, after;
    r.observedPresence = true;
    if (::fstat(fd.fd, &before) < 0 || !S_ISREG(before.st_mode)) {
        r.error = {"unsupported-file-identity", id, "read", name};
        return r;
    }
    ++m_reads[ownString(id)];
    QByteArray bytes;
    char buffer[16384];
    for (;;) {
        const auto n = ::read(fd.fd, buffer, sizeof(buffer));
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            r.error = {"read-failed", id, "read", name};
            return r;
        }
        bytes.append(buffer, n);
    }
    r.bytes = bytes;
    r.bytesAvailable = true;
    const auto afterTarget = resolve(id);
    if (::fstat(fd.fd, &after) < 0) {
        r.error = {"concurrent-read", id, "read", name};
        return r;
    }
    r.revision = {true, QFile::encodeName(name).toHex() + '|' + stamp(after) + '|' +
                            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() +
                            '|' + target.identity};
    if (stamp(before) != stamp(after) || stamp(name) != stamp(after) || afterTarget.path != name ||
        afterTarget.identity != target.identity) {
        r.error = {"concurrent-read", id, "read", name};
        return r;
    }
    r.success = true;
    return r;
}
void ConfigJsonBackend::confirm(const QString &id, const ConfigRevision &revision) {
    const auto fields = revision.token.split('|');
    if (fields.size() == 4) {
        const auto target = QFile::decodeName(QByteArray::fromHex(fields[0]));
        m_sourceTargets[ownString(id)] = target;
        m_confirmed[ownString(target)] = fields[1];
        m_confirmedChains[ownString(id)] = fields[3];
    }
}
bool ConfigJsonBackend::changed(const QString &id) const {
    const auto target = resolve(id);
    return !target.error.empty() || m_sourceTargets.value(id) != target.path ||
           m_confirmedChains.value(id) != target.identity || !m_confirmed.contains(target.path) ||
           stamp(target.path) != m_confirmed.value(target.path);
}
ConfigBackendResult
ConfigJsonBackend::publish(const ConfigBackendRequest &request, int directory, const QString &name,
                           bool remove, QList<ConfigJsonTransaction::Observation> *observations) {
    const auto id = request.sourceId;
    QString conflictCode = "revision-conflict";
    for (const auto &file : m_files)
        if (file.sourceId == id &&
            file.conflictPolicy == ConfigConflictPolicy::StrictSourcePreimage)
            conflictCode = "source-preimage-conflict";
    auto r = read(id, name); // Fresh authority inside the resolved target's lock.
    r.operation = ConfigBackendRequest::Operation::Publish;
    if (!r.success)
        return r;
    const auto identityError = checkTarget(id, name, true);
    if (!identityError.empty()) {
        r.success = false;
        r.error = identityError;
        return r;
    }
    if (r.revision != request.expected) {
        r.success = false;
        r.error = {conflictCode, id, "preflight", name};
        return r;
    }
    const auto originalStamp = stamp(name);
    const auto originalChain = resolve(id).identity;
    auto observe = [&](const ConfigBackendResult &value, const QString &phase,
                       const QString &role) {
        if (observations)
            observations->append({value, phase, role});
    };
    if (remove) {
        const auto injected = fault("delete", name);
        if (!injected.isEmpty() || stamp(name) != originalStamp ||
            resolve(id).identity != originalChain || resolve(id).path != name ||
            !checkTarget(id, name, true).empty()) {
            r.success = false;
            r.error = {injected.isEmpty() ? QString("empty-delete-target") : injected, id,
                       "preflight", name};
            return r;
        }
        if (::unlinkat(directory, QFile::encodeName(QFileInfo(name).fileName()).constData(), 0) <
            0) {
            r.success = false;
            r.error = {"empty-delete-target", id, "delete", name};
            return r;
        }
        r.publicationWitness =
            ConfigRevision{false, QFile::encodeName(name).toHex() + "|missing||" + originalChain};
        auto absent = r;
        absent.revision = *r.publicationWitness;
        absent.bytes.clear();
        absent.observedPresence = false;
        absent.bytesAvailable = false;
        observe(absent, "delete", "published");
        auto injectedAfter = fault("directory-sync", name);
        bool synced = ::fsync(directory) == 0;
        if (!synced || !injectedAfter.isEmpty()) {
            r.success = false;
            r.publicationState = ConfigBackendResult::PublicationState::Uncertain;
            r.error = {injectedAfter.isEmpty() ? QString("directory-sync-failed") : injectedAfter,
                       id, "sync", name};
            return r;
        }
        injectedAfter = fault("readback", name);
        const auto witness = r.publicationWitness;
        r = read(id, name);
        r.operation = ConfigBackendRequest::Operation::Publish;
        r.publicationWitness = witness;
        observe(r, "readback", r.revision == *witness && r.success ? "readback" : "late");
        if (!injectedAfter.isEmpty() || !r.success || r.revision != *witness) {
            r.success = false;
            r.publicationState = ConfigBackendResult::PublicationState::Uncertain;
            r.error = {injectedAfter.isEmpty() ? QString("readback-mismatch") : injectedAfter, id,
                       "readback", name};
            return r;
        }
        confirm(id, r.revision);
        return r;
    }
    const auto temporary =
        "." + QFileInfo(name).fileName() + "." + QUuid::createUuid().toString(QUuid::Id128);
    const auto temporaryBytes = QFile::encodeName(temporary);
    Fd fd(::openat(directory, temporaryBytes.constData(),
                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    auto fail = [&](const QString &code, const QString &phase, bool uncertain = false) {
        ::unlinkat(directory, temporaryBytes.constData(), 0);
        r.success = false;
        r.error = {code, id, phase, name};
        if (uncertain)
            r.publicationState = ConfigBackendResult::PublicationState::Uncertain;
        return r;
    };
    if (fd.fd < 0)
        return fail("stage-open-failed", "stage");
    struct stat existing;
    if (::lstat(QFile::encodeName(name).constData(), &existing) == 0 &&
        ::fchmod(fd.fd, existing.st_mode & 0777) < 0)
        return fail("permissions-failed", "stage");
    auto injected = fault("write", name);
    if (!injected.isEmpty())
        return fail(injected, "write");
    qsizetype offset = 0;
    while (offset < request.bytes.size()) {
        const auto n =
            ::write(fd.fd, request.bytes.constData() + offset, request.bytes.size() - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return fail("write-failed", "write");
        offset += n;
    }
    if (::fsync(fd.fd) < 0)
        return fail("file-sync-failed", "sync");
    injected = fault("rename", name);
    if (!injected.isEmpty())
        return fail(injected, "rename");
    if (stamp(name) != originalStamp || resolve(id).path != name ||
        resolve(id).identity != originalChain || !checkTarget(id, name, true).empty())
        return fail(conflictCode, "preflight");
    if (::renameat(directory, temporaryBytes.constData(), directory,
                   QFile::encodeName(QFileInfo(name).fileName()).constData()) < 0)
        return fail("rename-failed", "rename");
    struct stat published;
    if (::fstat(fd.fd, &published) == 0) {
        const ConfigRevision effect{
            true, QFile::encodeName(name).toHex() + '|' + stamp(published) + '|' +
                      QCryptographicHash::hash(request.bytes, QCryptographicHash::Sha256).toHex() +
                      '|' + originalChain};
        if (stamp(name) == stamp(published) && resolve(id).path == name &&
            resolve(id).identity == originalChain)
            r.publicationWitness = effect;
        auto own = r;
        own.revision = effect;
        own.bytes = request.bytes;
        own.observedPresence = true;
        own.bytesAvailable = true;
        observe(own, "rename", "published");
    }
    injected = fault("directory-sync", name);
    if (::fsync(directory) < 0 || !injected.isEmpty())
        return fail(injected.isEmpty() ? QString("directory-sync-failed") : injected, "sync", true);
    injected = fault("readback", name);
    const auto witness = r.publicationWitness;
    r = read(id, name);
    r.publicationWitness = witness;
    r.operation = ConfigBackendRequest::Operation::Publish;
    observe(r, "readback", r.success && witness && r.revision == *witness ? "readback" : "late");
    if (!injected.isEmpty() || !r.success || r.bytes != request.bytes || !witness ||
        r.revision != *witness)
        return fail(injected.isEmpty() ? QString("readback-mismatch") : injected, "readback", true);
    confirm(id, r.revision);
    return r;
}
quint64 ConfigJsonBackend::submit(const ConfigBackendRequest &input) {
    if (QThread::currentThread() != m_owner)
        return 0;
    auto request = input;
    request.sourceId = ownString(input.sourceId);
    request.bytes = QByteArray(input.bytes.constData(), input.bytes.size());
    request.expected.token =
        QByteArray(input.expected.token.constData(), input.expected.token.size());
    const auto sequence = ++m_next;
    ConfigBackendResult r;
    if (request.operation == ConfigBackendRequest::Operation::Read)
        r = read(request.sourceId);
    else {
        const auto target = resolve(request.sourceId);
        r.error = target.error;
        if (r.error.empty())
            r.error = checkTarget(request.sourceId, target.path, true);
        if (r.error.empty()) {
            const auto name = target.path;
            QDir().mkpath(QFileInfo(name).absolutePath());
            Fd dir(::open(QFile::encodeName(QFileInfo(name).absolutePath()).constData(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            Fd lock(openLock(dir.fd, name));
            if (lock.fd < 0) {
                r.success = false;
                r.error = {"lock-failed", request.sourceId, "lock", name};
            } else
                r = publish(request, dir.fd, name);
        }
    }
    r.requestId = sequence;
    r.sourceId = request.sourceId;
    r.operation = request.operation;
    m_results[sequence] = r;
    return sequence;
}
std::optional<ConfigBackendResult> ConfigJsonBackend::take(quint64 requestId) {
    if (QThread::currentThread() != m_owner || !m_results.contains(requestId))
        return {};
    return m_results.take(requestId);
}
ConfigJsonTransaction
ConfigJsonBackend::reconcile(const QString &sourceId, const QVariantMap &baseline,
                             const QList<ConfigFileEdit> &edits,
                             const std::optional<ConfigSourcePreimage> &preimage) {
    const auto id = ownString(sourceId);
    ConfigJsonTransaction t;
    auto &r = t.outcome;
    r.sourceId = id;
    r.operation = ConfigBackendRequest::Operation::Publish;
    const auto target = resolve(id);
    const auto name = target.path;
    if (QThread::currentThread() != m_owner) {
        r.error = {"wrong-thread", id, "reconcile", name};
        return t;
    }
    auto error = target.error;
    ConfigJsonFile file;
    for (const auto &candidate : m_files)
        if (candidate.sourceId == id)
            file = candidate;
    const bool strict = file.conflictPolicy == ConfigConflictPolicy::StrictSourcePreimage;
    if (error.empty())
        error = checkTarget(id, name, true);
    auto fail = [&](ConfigError e) {
        r.success = false;
        r.error = m_diagnostics.value(id).sanitize(std::move(e));
        if (t.observations.isEmpty())
            t.observations.append({r, r.error.operation, "foreign"});
        return t;
    };
    if (!error.empty())
        return fail(error);
    if (strict && !preimage)
        return fail({"source-preimage-conflict", id, "preflight", name});
    // Remember the chain before creating absent parent directories ourselves.
    const bool createdParents = !QFileInfo(QFileInfo(name).absolutePath()).exists();
    if (strict && createdParents &&
        (preimage->revision.exists ||
         preimage->revision.token.split('|').value(3) != target.identity))
        return fail({"source-preimage-conflict", id, "preflight", name});
    if (!QDir().mkpath(QFileInfo(name).absolutePath()))
        return fail({"mkdir-failed", id, "stage", name});
    if (resolve(id).path != name)
        return fail({"target-changed", id, "preflight", name});
    if (strict && createdParents && !resolve(id).identity.startsWith(target.identity))
        return fail({"source-preimage-conflict", id, "preflight", name});
    Fd dir(::open(QFile::encodeName(QFileInfo(name).absolutePath()).constData(),
                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    Fd lock(openLock(dir.fd, name));
    if (lock.fd < 0)
        return fail({"lock-failed", id, "lock", name});
    r = read(id, name);
    r.operation = ConfigBackendRequest::Operation::Publish;
    t.observations.append({r, "preflight", "foreign"});
    if (!r.success)
        return t;
    auto expected = preimage ? preimage->revision : ConfigRevision{};
    if (strict && createdParents) {
        const auto fields = expected.token.split('|');
        expected.token = fields.value(0) + '|' + fields.value(1) + '|' + fields.value(2) + '|' +
                         resolve(id).identity;
    }
    if (strict && (r.revision != expected || r.bytes != preimage->bytes))
        return fail({"source-preimage-conflict", id, "preflight", name});
    ConfigJsonCodec codec(m_diagnostics.value(id));
    if (r.revision.exists) {
        auto parsed = codec.decode(r.bytes, id);
        if (!parsed.ok()) {
            parsed.error.file = name;
            return fail(parsed.error);
        }
        t.raw = parsed.raw;
    }
    for (const auto &edit : edits) {
        if (edit.path.isEmpty() && edit.value &&
            edit.value->metaType().id() != QMetaType::QVariantMap)
            return fail({"root-edit-object", id, "reconcile", name});
        const auto fresh = valueAt(t.raw, edit.path);
        if (edit.ifMissing && fresh)
            continue;
        if (edit.ifMissing)
            for (qsizetype i = 1; i < edit.path.size(); ++i) {
                const auto ancestor = valueAt(t.raw, edit.path.mid(0, i));
                if (ancestor && ancestor->metaType().id() != QMetaType::QVariantMap)
                    return fail(m_diagnostics.value(id).error(
                        m_diagnostics.value(id).make(edit.path, "path-conflict"), id, "reconcile",
                        name));
            }
        const auto fence = edit.fencePath.value_or(edit.path);
        if (!edit.ifMissing && (!same(valueAt(baseline, fence), valueAt(t.raw, fence)) ||
                                !compatibleAncestors(baseline, t.raw, fence)))
            return fail(m_diagnostics.value(id).error(
                m_diagnostics.value(id).make(edit.path, "path-conflict"), id, "reconcile", name));
    }
    bool applied = false;
    for (const auto &edit : edits)
        applied |= applyFileEdit(t.raw, edit);
    if (!applied)
        return t;
    auto choices = t.raw;
    choices.remove("_configqmlVersion");
    const bool remove = file.deleteWhenEmpty && choices.isEmpty();
    if (remove && !r.revision.exists)
        return t;
    const auto encoded = codec.encode(t.raw, id);
    if (!encoded.ok())
        return fail(encoded.error);
    t.candidateBytes = remove ? QByteArray() : encoded.bytes;
    r = publish({ConfigBackendRequest::Operation::Publish, id, encoded.bytes, r.revision}, dir.fd,
                name, remove, &t.observations);
    t.published =
        r.success || r.publicationState == ConfigBackendResult::PublicationState::Uncertain;
    if (r.success)
        t.raw = r.revision.exists ? codec.decode(r.bytes, id).raw : QVariantMap{};
    return t;
}
} // namespace configqml
