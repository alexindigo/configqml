// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configjsonbackend.h"
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace configqml {
namespace {
// Own only an exclusively created, unpublished staging name. Final evidence is
// never cleanup-owned, including after a failed directory durability barrier.
struct BackupStage {
    int directory = -1;
    int file = -1;
    QByteArray name;
    ~BackupStage() {
        if (!name.isEmpty())
            ::unlinkat(directory, name.constData(), 0);
        if (file >= 0)
            ::close(file);
        if (directory >= 0)
            ::close(directory);
    }
};
} // namespace

ConfigError ConfigJsonBackend::preserveBackup(const QString &sourceId, int version,
                                              const QByteArray &original) {
    const auto target = resolve(sourceId);
    if (!target.error.empty())
        return target.error;
    const auto finalName =
        QFileInfo(target.path).fileName() + QString(".configqml-v%1.bak").arg(version);
    const auto parent = QFileInfo(target.path).absolutePath();
    const auto finalPath = parent + '/' + finalName;
    const auto finalBytes = QFile::encodeName(finalName);
    BackupStage owned;
    owned.directory = ::open(QFile::encodeName(parent).constData(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    const auto cleanupStage = [&](ConfigError error = {}) {
        if (!owned.name.isEmpty()) {
            const auto stagePath = parent + '/' + QFile::decodeName(owned.name);
            const auto cleanup = fault("backup-cleanup", stagePath);
            if (!cleanup.isEmpty() || ::unlinkat(owned.directory, owned.name.constData(), 0) < 0)
                error = {cleanup.isEmpty() ? QString("migration-backup-cleanup") : cleanup,
                         sourceId, "backup-cleanup", stagePath};
            // A failed cleanup remains private staging, never final-name evidence.
            owned.name.clear();
        }
        return error;
    };
    const auto fail = [&](const QString &code, const QString &phase) {
        return cleanupStage({code, sourceId, phase, finalPath});
    };
    const auto barrier = [&](int fd, const QString &phase) {
        const auto injected = fault(phase, finalPath);
        if (!injected.isEmpty())
            return fail(injected, phase);
        return ::fsync(fd) == 0 ? ConfigError{} : fail("migration-backup-sync", phase);
    };
    const auto reuse = [&]() -> ConfigError {
        const auto fd = ::openat(owned.directory, finalBytes.constData(),
                                 O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0)
            return fail("migration-backup-conflict", "backup-validate");
        QFile file;
        if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
            ::close(fd);
            return fail("migration-backup-conflict", "backup-validate");
        }
        struct stat state{};
        if (::fstat(fd, &state) < 0 || !S_ISREG(state.st_mode) || (state.st_mode & 0777) != 0600 ||
            file.readAll() != original || file.error() != QFileDevice::NoError)
            return fail("migration-backup-conflict", "backup-validate");
        auto error = barrier(fd, "backup-file-sync");
        return error.empty() ? barrier(owned.directory, "backup-directory-sync") : error;
    };
    if (QThread::currentThread() != m_owner || owned.directory < 0)
        return fail("migration-backup", "backup-directory");
    struct stat existing{};
    if (::fstatat(owned.directory, finalBytes.constData(), &existing, AT_SYMLINK_NOFOLLOW) == 0)
        return reuse();
    if (errno != ENOENT)
        return fail("migration-backup", "backup-validate");
    const auto stageName = QFile::encodeName('.' + finalName + '.' +
                                             QUuid::createUuid().toString(QUuid::Id128) + ".stage");
    auto injected = fault("backup-create", finalPath);
    if (!injected.isEmpty())
        return fail(injected, "backup-create");
    owned.file = ::openat(owned.directory, stageName.constData(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (owned.file < 0)
        return fail("migration-backup", "backup-create");
    owned.name = stageName;
    injected = fault("backup-created", parent + '/' + QFile::decodeName(stageName));
    if (!injected.isEmpty())
        return fail(injected, "backup-created");
    QFile file;
    if (!file.open(owned.file, QIODevice::WriteOnly, QFileDevice::DontCloseHandle))
        return fail("migration-backup", "backup-open");
    for (qsizetype offset = 0; offset < original.size();) {
        injected = fault("backup-write", finalPath);
        if (!injected.isEmpty())
            return fail(injected, "backup-write");
        const auto written = file.write(original.constData() + offset,
                                        std::min<qsizetype>(16384, original.size() - offset));
        if (written <= 0)
            return fail("migration-backup", "backup-write");
        offset += written;
    }
    injected = fault("backup-flush", finalPath);
    if (!injected.isEmpty() || !file.flush())
        return fail(injected.isEmpty() ? QString("migration-backup") : injected, "backup-flush");
    auto error = barrier(owned.file, "backup-file-sync");
    if (!error.empty())
        return error;
    injected = fault("backup-publish", finalPath);
    if (!injected.isEmpty())
        return fail(injected, "backup-publish");
    // linkat is an atomic no-replace publication of the complete synced inode.
    if (::linkat(owned.directory, stageName.constData(), owned.directory, finalBytes.constData(),
                 0) < 0) {
        if (errno != EEXIST)
            return fail("migration-backup", "backup-publish");
        error = cleanupStage(); // Remove only our staging before validating competing evidence.
        return error.empty() ? reuse() : error;
    }
    error = cleanupStage();
    return error.empty() ? barrier(owned.directory, "backup-directory-sync") : error;
}
} // namespace configqml
