// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configjsonbackend.h"
#include "configjsonpublication_p.h"
#include <QFile>
#include <QFileInfo>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace configqml {
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
    ConfigFileDescriptor directory(::open(QFile::encodeName(parent).constData(),
                                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    const auto fail = [&](const QString &code, const QString &phase) {
        return ConfigError{code, sourceId, phase, finalPath};
    };
    const auto barrier = [&](int fd, const QString &phase) {
        const auto injected = fault(phase, finalPath);
        if (!injected.isEmpty())
            return fail(injected, phase);
        return ::fsync(fd) == 0 ? ConfigError{} : fail("migration-backup-sync", phase);
    };
    const auto reuse = [&]() -> ConfigError {
        const auto fd = ::openat(directory.fd, finalBytes.constData(),
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
        return error.empty() ? barrier(directory.fd, "backup-directory-sync") : error;
    };
    if (QThread::currentThread() != m_owner || directory.fd < 0)
        return fail("migration-backup", "backup-directory");
    ConfigFileDescriptor lock(openPublicationLock(directory.fd, finalName));
    if (lock.fd < 0)
        return fail("migration-backup", "backup-lock");
    struct stat existing{};
    if (::fstatat(directory.fd, finalBytes.constData(), &existing, AT_SYMLINK_NOFOLLOW) == 0)
        return reuse();
    if (errno != ENOENT)
        return fail("migration-backup", "backup-validate");
    PublicationOptions options;
    options.mode = PublicationOptions::Mode::KeepExisting;
    options.preservePermissions = false;
    options.phase = [](const QString &step) { return "backup-" + step; };
    options.translateError = [](ConfigError error) {
        if (error.code == "stage-cleanup-failed")
            error.code = "migration-backup-cleanup";
        else if (error.code == "file-sync-failed" || error.code == "directory-sync-failed")
            error.code = "migration-backup-sync";
        else if (error.code == "stage-open-failed" || error.code == "permissions-failed" ||
                 error.code == "write-failed" || error.code == "rename-failed")
            error.code = "migration-backup";
        return error;
    };
    options.validate = [&]() {
        const auto current = resolve(sourceId);
        return current.error.empty() && current.path == target.path &&
                       current.identity == target.identity
                   ? ConfigError{}
                   : fail("target-changed", "backup-publish");
    };
    const auto publication = publishFile(directory.fd, sourceId, finalPath, original, options);
    if (!publication.error.empty())
        return publication.error;
    // A competing complete backup is validated only after our own stage is removed.
    return publication.existing ? reuse() : ConfigError{};
}
} // namespace configqml
