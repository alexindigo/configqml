// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configjsonbackend.h"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
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
} // namespace
ConfigJsonBackend::ConfigJsonBackend(const QList<ConfigJsonFile> &files, FaultHook hook)
    : m_files(files), m_hook(std::move(hook)) {
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
    Target result;
    QString configured;
    for (const auto &file : m_files)
        if (file.sourceId == id)
            configured = file.path;
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
    struct stat s;
    if (::lstat(QFile::encodeName(name).constData(), &s) == 0) {
        if (!S_ISREG(s.st_mode))
            return {"unsupported-file-identity", id, "path", name};
        if (writing && s.st_nlink != 1)
            return {"hard-linked-target", id, "publish", name};
    } else if (errno != ENOENT)
        return {"stat-failed", id, "path", name};
    for (const auto &other : m_files) {
        if (other.sourceId != id && resolve(other.sourceId).path == name)
            return {"duplicate-file-identity", id, "path", name};
    }
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
            r.revision = {false, QFile::encodeName(name).toHex() + "|missing|"};
        } else
            r.error = {"open-failed", id, "read", name};
        return r;
    }
    struct stat before, after;
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
    if (::fstat(fd.fd, &after) < 0 || stamp(before) != stamp(after) ||
        stamp(name) != stamp(after) || resolve(id).path != name) {
        r.error = {"concurrent-read", id, "read", name};
        return r;
    }
    r.success = true;
    r.bytes = bytes;
    r.revision = {true, QFile::encodeName(name).toHex() + '|' + stamp(after) + '|' +
                            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()};
    return r;
}
void ConfigJsonBackend::confirm(const QString &id, const ConfigRevision &revision) {
    const auto fields = revision.token.split('|');
    if (fields.size() == 3) {
        const auto target = QFile::decodeName(QByteArray::fromHex(fields[0]));
        m_sourceTargets[ownString(id)] = target;
        m_confirmed[ownString(target)] = fields[1];
    }
}
bool ConfigJsonBackend::changed(const QString &id) const {
    const auto target = resolve(id);
    return !target.error.empty() || m_sourceTargets.value(id) != target.path ||
           !m_confirmed.contains(target.path) ||
           stamp(target.path) != m_confirmed.value(target.path);
}
ConfigBackendResult ConfigJsonBackend::publish(const ConfigBackendRequest &request, int directory,
                                               const QString &name) {
    const auto id = request.sourceId;
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
        r.error = {"revision-conflict", id, "preflight", name};
        return r;
    }
    const auto originalStamp = stamp(name);
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
        !checkTarget(id, name, true).empty())
        return fail("revision-conflict", "preflight");
    if (::renameat(directory, temporaryBytes.constData(), directory,
                   QFile::encodeName(QFileInfo(name).fileName()).constData()) < 0)
        return fail("rename-failed", "rename");
    if (::fsync(directory) < 0)
        return fail("directory-sync-failed", "sync", true);
    injected = fault("readback", name);
    if (!injected.isEmpty())
        return fail(injected, "readback", true);
    r = read(id, name);
    r.operation = ConfigBackendRequest::Operation::Publish;
    if (!r.success || r.bytes != request.bytes)
        return fail("readback-mismatch", "readback", true);
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
ConfigJsonTransaction ConfigJsonBackend::reconcile(const QString &sourceId,
                                                   const QVariantMap &baseline,
                                                   const QList<ConfigFileEdit> &edits) {
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
    if (error.empty())
        error = checkTarget(id, name, true);
    auto fail = [&](ConfigError e) {
        r.success = false;
        r.error = std::move(e);
        return t;
    };
    if (!error.empty())
        return fail(error);
    if (!QDir().mkpath(QFileInfo(name).absolutePath()))
        return fail({"mkdir-failed", id, "stage", name});
    if (resolve(id).path != name)
        return fail({"target-changed", id, "preflight", name});
    Fd dir(::open(QFile::encodeName(QFileInfo(name).absolutePath()).constData(),
                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    Fd lock(openLock(dir.fd, name));
    if (lock.fd < 0)
        return fail({"lock-failed", id, "lock", name});
    r = read(id, name);
    r.operation = ConfigBackendRequest::Operation::Publish;
    if (!r.success)
        return t;
    ConfigJsonCodec codec;
    if (r.revision.exists) {
        auto parsed = codec.decode(r.bytes, id);
        if (!parsed.ok()) {
            parsed.error.file = name;
            return fail(parsed.error);
        }
        t.raw = parsed.raw;
    }
    for (const auto &edit : edits) {
        const auto fresh = valueAt(t.raw, edit.path);
        if (edit.ifMissing && fresh)
            continue;
        if (edit.ifMissing)
            for (qsizetype i = 1; i < edit.path.size(); ++i) {
                const auto ancestor = valueAt(t.raw, edit.path.mid(0, i));
                if (ancestor && ancestor->metaType().id() != QMetaType::QVariantMap)
                    return fail({"path-conflict", id, "reconcile", name, edit.path});
            }
        if (!edit.ifMissing && (!same(valueAt(baseline, edit.path), fresh) ||
                                !compatibleAncestors(baseline, t.raw, edit.path)))
            return fail({"path-conflict", id, "reconcile", name, edit.path});
    }
    bool applied = false;
    for (const auto &edit : edits) {
        if (edit.ifMissing && valueAt(t.raw, edit.path))
            continue;
        applied = true;
        if (edit.value)
            putValue(t.raw, edit.path, *edit.value);
        else if (edit.path.isEmpty())
            t.raw.clear();
        else
            removeValue(t.raw, edit.path);
    }
    if (!applied)
        return t;
    const auto encoded = codec.encode(t.raw, id);
    if (!encoded.ok())
        return fail(encoded.error);
    r = publish({ConfigBackendRequest::Operation::Publish, id, encoded.bytes, r.revision}, dir.fd,
                name);
    t.published =
        r.success || r.publicationState == ConfigBackendResult::PublicationState::Uncertain;
    if (r.success)
        t.raw = codec.decode(r.bytes, id).raw;
    return t;
}
} // namespace configqml
