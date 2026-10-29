// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configevidence_p.h"
#include "confignumber.h"
#include <QDir>
#include <QFile>
#include <QUuid>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
namespace configqml {
namespace {
bool artifact(int dir, const char *name, const QByteArray &bytes) {
    const auto fd = ::openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;
    qsizetype offset = 0;
    bool ok = true;
    while (offset < bytes.size()) {
        const auto written = ::write(fd, bytes.constData() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            ok = false;
            break;
        }
        offset += written;
    }
    ok = (::fsync(fd) == 0) && ok;
    ::close(fd);
    return ok;
}
} // namespace
std::optional<ConfigEvidenceReference> retainEvidence(const QString &root,
                                                      const ConfigEvidenceObservation &o) {
    if (!QDir::isAbsolutePath(root) || QDir::cleanPath(root) != root || !QDir().mkpath(root))
        return {};
    const auto parent = ::open(QFile::encodeName(root).constData(),
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0)
        return {};
    const auto id = QUuid::createUuid().toString(QUuid::Id128);
    const auto name = id.toLatin1();
    if (::mkdirat(parent, name.constData(), 0700) < 0) {
        ::close(parent);
        return {};
    }
    const auto directory =
        ::openat(parent, name.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    bool ok = directory >= 0;
    const auto encoded =
        encodeJsonValue(QVariantMap{{"evidenceId", id},
                                    {"storeId", o.storeId},
                                    {"requestId", o.requestId},
                                    {"groupToken", o.groupToken},
                                    {"attemptId", o.attemptId},
                                    {"sourceId", o.sourceId},
                                    {"phase", o.phase},
                                    {"observationOrder", QString::number(o.observationOrder)},
                                    {"presence", o.presence},
                                    {"bytesState", o.bytesState},
                                    {"revision", QString::fromLatin1(o.revision.token)},
                                    {"role", o.role}});
    if (ok && o.bytesState == "available")
        ok = artifact(directory, "bytes", o.bytes);
    if (ok)
        ok = encoded.ok() && artifact(directory, "metadata.json", encoded.bytes);
    if (ok)
        ok = ::fsync(directory) == 0;
    if (directory >= 0)
        ::close(directory);
    ok = (::fsync(parent) == 0) && ok;
    ::close(parent);
    if (!ok)
        return {}; // Incomplete evidence is retained, never overwritten or represented as complete.
    return ConfigEvidenceReference{id,      root + '/' + id, o.sourceId,
                                   o.phase, o.role,          o.observationOrder};
}
} // namespace configqml
