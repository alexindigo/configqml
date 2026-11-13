// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configjsonbackend.h"
#include "configjsonpublication_p.h"
#include "configrevision_p.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace configqml {
namespace {
using Fd = ConfigFileDescriptor;
bool validPolicy(ConfigConflictPolicy policy) {
    return policy == ConfigConflictPolicy::PathIntent ||
           policy == ConfigConflictPolicy::StrictSourcePreimage ||
           policy == ConfigConflictPolicy::StrictPendingRebase;
}
QByteArray stamp(const struct stat &s) { return fileIdentityStamp(s); }
QByteArray stamp(const QString &path) { return fileIdentityStampAt(AT_FDCWD, path); }
QByteArray directoryIdentity(const QString &path, const struct stat &state) {
    return QFile::encodeName(path).toHex() + ':' + QByteArray::number(state.st_dev) + ':' +
           QByteArray::number(state.st_ino);
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
    return checkFileIdentityAt(AT_FDCWD, name, id, writing, name);
}
} // namespace
struct ConfigJsonBackend::DirectoryWitnesses {
    // Keep descriptors alive: a removed directory's inode cannot be recycled
    // into a false matching witness while sibling routes are being published.
    QMap<QByteArray, std::shared_ptr<Fd>> directories;
};
void ConfigJsonBackend::beginPublication() {
    Q_ASSERT(QThread::currentThread() == m_owner);
    m_saveDirectories = std::make_shared<DirectoryWitnesses>();
}
void ConfigJsonBackend::endPublication() { m_saveDirectories.reset(); }
ConfigError ConfigJsonBackend::prepareParent(const QString &id, const QString &name,
                                             DirectoryWitnesses &owned) {
    auto directory = std::make_shared<Fd>(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    QString path;
    const auto parts = QFileInfo(name).absolutePath().mid(1).split('/', Qt::SkipEmptyParts);
    for (const auto &part : parts) {
        path += '/' + part;
        const auto encoded = QFile::encodeName(part);
        bool created = false;
        struct stat state;
        if (::fstatat(directory->fd, encoded.constData(), &state, AT_SYMLINK_NOFOLLOW) < 0) {
            if (errno != ENOENT)
                return {"stat-failed", id, "stage", name};
            created = ::mkdirat(directory->fd, encoded.constData(), 0777) == 0;
            if (!created && errno != EEXIST)
                return {"mkdir-failed", id, "stage", name};
            if (::fstatat(directory->fd, encoded.constData(), &state, AT_SYMLINK_NOFOLLOW) < 0)
                return {"stat-failed", id, "stage", name};
        }
        const auto observed = state;
        auto child = std::make_shared<Fd>(::openat(
            directory->fd, encoded.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (child->fd < 0 || ::fstat(child->fd, &state) < 0 || state.st_dev != observed.st_dev ||
            state.st_ino != observed.st_ino)
            return {"target-changed", id, "preflight", name};
        if (created)
            owned.directories.insert(directoryIdentity(path, state), child);
        directory = std::move(child);
    }
    return {};
}
ConfigRevision ConfigJsonBackend::withOwnParents(const ConfigRevision &revision, const QString &id,
                                                 const DirectoryWitnesses &owned) const {
    auto result = revision;
    const auto witness = revision.witness;
    const auto target = resolve(id);
    if (!witness || !target.error.empty() || witness->target != target.path)
        return result;
    const auto original = witness->parents.split(';');
    auto parents = witness->ownedParents;
    QByteArray unowned;
    for (const auto &component : target.identity.split(';')) {
        if (component.isEmpty())
            continue;
        if (!original.contains(component) && owned.directories.contains(component)) {
            parents.append(owned.directories.value(component));
            continue;
        }
        unowned += component + ';';
    }
    if (unowned == witness->parents && target.identity != witness->parents) {
        result = fileRevision(revision.exists, witness->target, witness->stamp, witness->digest,
                              target.identity);
        auto retained = std::make_shared<ConfigRevisionWitness>(*result.witness);
        retained->ownedParents = std::move(parents);
        retained->ownedParentBase =
            witness->ownedParents.isEmpty() ? witness->parents : witness->ownedParentBase;
        result.witness = std::move(retained);
    }
    return result;
}
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
bool sameSourcePreimage(const ConfigSourcePreimage &a, const ConfigSourcePreimage &b) {
    const auto left = a.revision.witness, right = b.revision.witness;
    return left && right && left->target == right->target && left->parents == right->parents &&
           a.revision.exists == b.revision.exists && a.bytes == b.bytes;
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
                result.identity += directoryIdentity(candidate, s) + ';';
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
    for (const auto &file : m_files)
        if (file.sourceId == id && !validPolicy(file.conflictPolicy))
            return {"conflict-policy", id, "declaration", name};
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
    // Resolve/exclude before opening the parent; staging and commit then stay on
    // that descriptor. Components above the opened parent are not rooted traversal
    // and can race during resolution/open, just as for configured-file publication.
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
    Fd directory(::open(QFile::encodeName(QFileInfo(target.path).absolutePath()).constData(),
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    Fd lock(openPublicationLock(directory.fd, target.path));
    if (lock.fd < 0)
        return {"lock-failed", id, "exportProfile", configured};
    const auto basename = QFileInfo(target.path).fileName();
    const auto original = fileIdentityStampAt(directory.fd, basename);
    PublicationOptions options;
    options.preservePermissions = false;
    options.phase = [](const QString &step) { return "profile-" + step; };
    options.validate = [&]() {
        const auto failure = validate();
        if (!failure.empty())
            return failure;
        return fileIdentityStampAt(directory.fd, basename) == original
                   ? ConfigError{}
                   : ConfigError{"target-changed", id, "exportProfile", configured};
    };
    return publishFile(directory.fd, id, target.path, bytes, options).error;
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
            r.revision = fileRevision(false, name, "missing", {}, target.identity);
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
    r.revision = fileRevision(true, name, stamp(after), compactIdentity(bytes), target.identity);
    if (stamp(before) != stamp(after) || stamp(name) != stamp(after) || afterTarget.path != name ||
        afterTarget.identity != target.identity) {
        r.error = {"concurrent-read", id, "read", name};
        return r;
    }
    r.success = true;
    return r;
}
void ConfigJsonBackend::confirm(const QString &id, const ConfigRevision &revision) {
    if (const auto witness = revision.witness) {
        const auto target = witness->target;
        m_sourceTargets[ownString(id)] = target;
        m_confirmed[ownString(target)] = witness->stamp;
        m_confirmedChains[ownString(id)] = witness->parents;
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
        if (file.sourceId == id && file.conflictPolicy != ConfigConflictPolicy::PathIntent)
            conflictCode = "source-preimage-conflict";
    auto r = read(id, name); // Fresh authority inside the resolved target's lock.
    r.operation = ConfigBackendRequest::Operation::Publish;
    if (observations)
        observations->append({r, "preflight", "foreign"});
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
    PublicationOptions options;
    if (remove)
        options.mode = PublicationOptions::Mode::Remove;
    options.phase = [remove](const QString &step) {
        if (step == "publish")
            return QString(remove ? "delete" : "rename");
        if (step == "create" || step == "permissions")
            return QString("stage");
        return step;
    };
    options.translateError = [remove](ConfigError error) {
        if (error.operation == "directory-sync" || error.operation == "file-sync")
            error.operation = "sync";
        else if (remove && error.operation == "delete" && error.code != "empty-delete-target")
            error.operation = "preflight";
        return error;
    };
    options.validate = [&]() -> ConfigError {
        if (fileIdentityStampAt(directory, QFileInfo(name).fileName()) != originalStamp ||
            stamp(name) != originalStamp || resolve(id).path != name ||
            resolve(id).identity != originalChain || !checkTarget(id, name, true).empty())
            return {remove ? QString("empty-delete-target") : conflictCode, id, "preflight", name};
        return {};
    };
    options.published = [&](const QByteArray &published) {
        if (remove) {
            r.publicationWitness = fileRevision(false, name, "missing", {}, originalChain);
            auto absent = r;
            absent.revision = *r.publicationWitness;
            absent.bytes.clear();
            absent.observedPresence = false;
            absent.bytesAvailable = false;
            observe(absent, "delete", "published");
            return;
        }
        if (published.isEmpty())
            return;
        const auto effect =
            fileRevision(true, name, published, compactIdentity(request.bytes), originalChain);
        if (stamp(name) == published && resolve(id).path == name &&
            resolve(id).identity == originalChain)
            r.publicationWitness = effect;
        auto own = r;
        own.revision = effect;
        own.bytes = request.bytes;
        own.observedPresence = true;
        own.bytesAvailable = true;
        observe(own, "rename", "published");
    };
    const auto publication = publishFile(directory, id, name, request.bytes, options);
    if (!publication.error.empty()) {
        r.success = false;
        r.error = publication.error;
        if (publication.published)
            r.publicationState = ConfigBackendResult::PublicationState::Uncertain;
        return r;
    }
    const auto injected = fault("readback", name);
    const auto witness = r.publicationWitness;
    r = read(id, name);
    r.publicationWitness = witness;
    r.operation = ConfigBackendRequest::Operation::Publish;
    observe(r, "readback", r.success && witness && r.revision == *witness ? "readback" : "late");
    if (!injected.isEmpty() || !r.success || (!remove && r.bytes != request.bytes) || !witness ||
        r.revision != *witness) {
        r.success = false;
        r.error = {injected.isEmpty() ? QString("readback-mismatch") : injected, id, "readback",
                   name};
        r.publicationState = ConfigBackendResult::PublicationState::Uncertain;
        return r;
    }
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
            Fd lock(openPublicationLock(dir.fd, name));
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
ConfigJsonTransaction ConfigJsonBackend::reconcile(
    const QString &sourceId, const QVariantMap &baseline, const QList<ConfigFileEdit> &edits,
    const std::optional<ConfigSourcePreimage> &preimage, const Canonicalizer &canonicalize,
    const PendingAcceptor &acceptPending) {
    const auto id = ownString(sourceId);
    ConfigJsonTransaction t;
    auto &r = t.outcome;
    r.sourceId = id;
    r.operation = ConfigBackendRequest::Operation::Publish;
    const auto injected = fault("parent-preflight", path(id));
    if (!injected.isEmpty()) {
        r.error = {injected, id, "preflight", path(id)};
        return t;
    }
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
    const bool strict = file.conflictPolicy != ConfigConflictPolicy::PathIntent;
    const bool pendingRebase =
        file.conflictPolicy == ConfigConflictPolicy::StrictPendingRebase && bool(acceptPending);
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
    // Only recorded mkdir successes from this save may extend a captured chain.
    // Existing/EEXIST directories, symlinks and replacements are never witnesses.
    const auto owned =
        m_saveDirectories ? m_saveDirectories : std::make_shared<DirectoryWitnesses>();
    auto expected = preimage ? withOwnParents(preimage->revision, id, *owned) : ConfigRevision{};
    if (strict && (!expected.witness || expected.witness->parents != target.identity ||
                   (expected.exists && !QFileInfo(QFileInfo(name).absolutePath()).exists())))
        return fail({"source-preimage-conflict", id, "preflight", name});
    const auto parentError = prepareParent(id, name, *owned);
    if (!parentError.empty())
        return fail(parentError);
    if (resolve(id).path != name)
        return fail({"target-changed", id, "preflight", name});
    expected = withOwnParents(expected, id, *owned);
    Fd dir(::open(QFile::encodeName(QFileInfo(name).absolutePath()).constData(),
                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    Fd lock(openPublicationLock(dir.fd, name));
    if (lock.fd < 0)
        return fail({"lock-failed", id, "lock", name});
    r = read(id, name);
    r.operation = ConfigBackendRequest::Operation::Publish;
    t.observations.append({r, "preflight", "foreign"});
    if (!r.success)
        return t;
    t.preflightRead = true;
    if (strict) {
        // A capture fences content and the resolved route, not a final file's
        // inode/timestamps. Exact restoration by an editor can resolve frozen
        // work without relaxing link/parent identity or late-race validation.
        const auto captured = expected.witness, fresh = r.revision.witness;
        const bool sameRoute = captured && fresh && captured->target == fresh->target &&
                               captured->parents == fresh->parents;
        if (pendingRebase ? !sameRoute
                          : !sameSourcePreimage({expected, preimage->bytes}, {r.revision, r.bytes}))
            return fail({"source-preimage-conflict", id, "preflight", name});
    }
    ConfigJsonCodec codec(m_diagnostics.value(id));
    if (r.revision.exists) {
        auto parsed = codec.decode(r.bytes, id);
        if (!parsed.ok()) {
            parsed.error.file = name;
            return fail(parsed.error);
        }
        t.raw = parsed.raw;
    }
    if (pendingRebase &&
        (r.revision.exists != preimage->revision.exists || r.bytes != preimage->bytes)) {
        const auto injected = fault("pending-accept", name);
        if (!injected.isEmpty())
            return fail({injected, id, "preflight", name});
        const auto acceptance = acceptPending(r);
        if (!acceptance.empty())
            return fail(acceptance);
        t.rebasedOnto = r.revision;
    }
    t.preflightBase = ConfigSourcePreimage{r.revision, r.bytes};
    const auto freshRaw = t.raw;
    QList<ConfigPath> overrideFences;
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
        if (pendingRebase) {
            // Conditions use fresh presence. Cleanup cannot destroy nonempty
            // foreign scaffolding; namespace reset fragments still fence the
            // entire namespace, including foreign additions and removals.
            if (edit.pruneEmpty && !edit.namespaceReset &&
                (!fresh || fresh->metaType().id() != QMetaType::QVariantMap ||
                 !fresh->toMap().isEmpty()))
                continue;
            if (!compatibleAncestors(baseline, freshRaw, edit.path) ||
                (edit.namespaceReset && r.revision.exists &&
                 !same(valueAt(baseline, fence), valueAt(freshRaw, fence))))
                return fail(m_diagnostics.value(id).error(
                    m_diagnostics.value(id).make(edit.path, "path-conflict"), id, "reconcile",
                    name));
            if (!edit.ifMissing && !edit.pruneEmpty && !edit.namespaceReset &&
                !same(valueAt(baseline, fence), valueAt(freshRaw, fence))) {
                if (!overrideFences.contains(fence))
                    overrideFences.append(fence);
            }
            continue;
        }
        if (!edit.ifMissing && (!same(valueAt(baseline, fence), valueAt(t.raw, fence)) ||
                                !compatibleAncestors(baseline, t.raw, fence)))
            return fail(m_diagnostics.value(id).error(
                m_diagnostics.value(id).make(edit.path, "path-conflict"), id, "reconcile", name));
    }
    bool applied = false;
    for (const auto &edit : edits)
        applied |= applyFileEdit(t.raw, edit);
    if (canonicalize) {
        const auto before = t.raw;
        const auto error = canonicalize(t.raw, r, t.warnings);
        if (!error.empty())
            return fail(error);
        applied |= before != t.raw;
    }
    if (!applied)
        return t;
    for (const auto &fence : overrideFences)
        if (!same(valueAt(freshRaw, fence), valueAt(t.raw, fence)))
            t.overriddenPaths.append(m_diagnostics.value(id).path(fence));
    auto choices = t.raw;
    choices.remove("_configqmlVersion");
    if (pendingRebase && t.rebasedOnto && !r.revision.exists && t.raw.isEmpty())
        return t; // A reset against a foreign deletion never recreates an empty file.
    const bool remove = file.deleteWhenEmpty && choices.isEmpty();
    if (remove && !r.revision.exists)
        return t;
    const auto encoded = codec.encode(t.raw, id);
    if (!encoded.ok())
        return fail(encoded.error);
    t.candidateBytes = remove ? QByteArray() : encoded.bytes;
    if (pendingRebase && t.rebasedOnto && !remove && r.revision.exists &&
        r.bytes == t.candidateBytes) {
        t.overriddenPaths.clear(); // Coverage without overwriting any foreign value.
        return t;
    }
    r = publish({ConfigBackendRequest::Operation::Publish, id, encoded.bytes, r.revision}, dir.fd,
                name, remove, &t.observations);
    t.published =
        r.success || r.publicationState == ConfigBackendResult::PublicationState::Uncertain;
    if (r.success)
        t.raw = r.revision.exists ? codec.decode(r.bytes, id).raw : QVariantMap{};
    return t;
}
} // namespace configqml
