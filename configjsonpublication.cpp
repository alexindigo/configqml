// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configjsonpublication_p.h"
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>

namespace configqml {
int openPublicationLock(int directory, const QString &name) {
    if (directory < 0)
        return -1;
    const auto fd =
        ::openat(directory, QFile::encodeName(QFileInfo(name).fileName() + ".lock").constData(),
                 O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    struct stat state;
    if (::fstat(fd, &state) == 0 && S_ISREG(state.st_mode) && state.st_nlink == 1) {
        QElapsedTimer timer;
        timer.start();
        do {
            if (::flock(fd, LOCK_EX | LOCK_NB) == 0)
                return fd;
            if (errno != EWOULDBLOCK && errno != EINTR)
                break;
            QThread::msleep(2);
        } while (timer.elapsed() < 2000);
    }
    ::close(fd);
    return -1;
}
QByteArray fileIdentityStamp(const struct stat &s) {
    return QByteArray::number(s.st_dev) + ':' + QByteArray::number(s.st_ino) + ':' +
           QByteArray::number(s.st_size) + ':' + QByteArray::number(s.st_mtim.tv_sec) + ':' +
           QByteArray::number(s.st_mtim.tv_nsec) + ':' + QByteArray::number(s.st_ctim.tv_sec) +
           ':' + QByteArray::number(s.st_ctim.tv_nsec);
}
QByteArray fileIdentityStampAt(int directory, const QString &name) {
    struct stat state;
    if (::fstatat(directory, QFile::encodeName(name).constData(), &state, AT_SYMLINK_NOFOLLOW) < 0)
        return errno == ENOENT ? QByteArray("missing") : QByteArray("stat-error");
    return fileIdentityStamp(state);
}
ConfigError checkFileIdentityAt(int directory, const QString &name, const QString &id, bool writing,
                                const QString &diagnosticFile) {
    struct stat state;
    if (::fstatat(directory, QFile::encodeName(name).constData(), &state, AT_SYMLINK_NOFOLLOW) ==
        0) {
        if (!S_ISREG(state.st_mode))
            return {"unsupported-file-identity", id, "path", diagnosticFile};
        if (writing && state.st_nlink != 1)
            return {"hard-linked-target", id, "publish", diagnosticFile};
    } else if (errno != ENOENT)
        return {"stat-failed", id, "path", diagnosticFile};
    return {};
}
ConfigJsonBackend::PublicationResult
ConfigJsonBackend::publishFile(int directory, const QString &id, const QString &name,
                               const QByteArray &bytes, const PublicationOptions &options) {
    using Mode = PublicationOptions::Mode;
    const auto basename = QFileInfo(name).fileName();
    const auto finalBytes = QFile::encodeName(basename);
    struct Stage {
        int directory;
        ConfigFileDescriptor file;
        QByteArray name;
        explicit Stage(int directory) : directory(directory) {}
        ~Stage() {
            if (!name.isEmpty())
                ::unlinkat(directory, name.constData(), 0);
        }
    } stage(directory);
    PublicationResult result;
    const auto phase = [&](const QString &value) {
        return options.phase ? options.phase(value) : value;
    };
    const auto error = [&](const QString &code, const QString &step, const QString &file) {
        ConfigError value{code, id, phase(step), file};
        return options.translateError ? options.translateError(value) : value;
    };
    const auto checkpoint = [&](const QString &step, const QString &file) {
        const auto injected = fault(phase(step), file);
        return injected.isEmpty() ? ConfigError{} : error(injected, step, file);
    };
    const auto cleanup = [&]() {
        if (stage.name.isEmpty())
            return ConfigError{};
        const auto path = QFileInfo(name).absolutePath() + '/' + QFile::decodeName(stage.name);
        auto failure = checkpoint("cleanup", path);
        if (failure.empty() && ::unlinkat(directory, stage.name.constData(), 0) < 0)
            failure = error("stage-cleanup-failed", "cleanup", path);
        // An explicitly reported cleanup failure retains private staging as evidence.
        stage.name.clear();
        return failure;
    };
    const auto fail = [&](ConfigError failure) {
        result.error = std::move(failure);
        const auto cleanupError = cleanup();
        if (!cleanupError.empty())
            result.error = cleanupError;
        return result;
    };
    if (options.mode != Mode::Remove) {
        auto failure = checkpoint("create", name);
        if (!failure.empty())
            return fail(failure);
        const auto temporary = QFile::encodeName(
            '.' + basename + '.' + QUuid::createUuid().toString(QUuid::Id128) + ".stage");
        stage.file.fd = ::openat(directory, temporary.constData(),
                                 O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (stage.file.fd < 0)
            return fail(error("stage-open-failed", "create", name));
        stage.name = temporary;
        if (!options.preservePermissions && ::fchmod(stage.file.fd, 0600) < 0)
            return fail(error("permissions-failed", "permissions", name));
        failure = checkpoint("created",
                             QFileInfo(name).absolutePath() + '/' + QFile::decodeName(temporary));
        if (!failure.empty())
            return fail(failure);
        struct stat existing;
        if (options.preservePermissions &&
            ::fstatat(directory, finalBytes.constData(), &existing, AT_SYMLINK_NOFOLLOW) == 0 &&
            ::fchmod(stage.file.fd, existing.st_mode & 0777) < 0)
            return fail(error("permissions-failed", "permissions", name));
        for (qsizetype offset = 0; offset < bytes.size();) {
            failure = checkpoint("write", name);
            if (!failure.empty())
                return fail(failure);
            const auto written = ::write(stage.file.fd, bytes.constData() + offset,
                                         std::min<qsizetype>(16384, bytes.size() - offset));
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                return fail(error("write-failed", "write", name));
            offset += written;
        }
        // write(2) is unbuffered; the flush checkpoint precedes the file fsync.
        failure = checkpoint("flush", name);
        if (!failure.empty())
            return fail(failure);
        failure = checkpoint("file-sync", name);
        if (!failure.empty())
            return fail(failure);
        if (::fsync(stage.file.fd) < 0)
            return fail(error("file-sync-failed", "file-sync", name));
    }
    auto failure = checkpoint("publish", name);
    if (!failure.empty())
        return fail(failure);
    if (options.validate) {
        failure = options.validate();
        if (!failure.empty())
            return fail(failure);
    }
    if (options.mode != Mode::KeepExisting) {
        failure = checkFileIdentityAt(directory, basename, id, true, name);
        if (!failure.empty())
            return fail(failure);
    }
    failure = checkpoint("publish-checked", name);
    if (!failure.empty())
        return fail(failure);
    // All effects below are relative to the same opened parent. O_NOFOLLOW on
    // that parent's open does not anchor components ABOVE it to a trusted root;
    // their mutation during resolution/open remains the backend's existing limit.
    if (options.mode == Mode::Remove) {
        if (::unlinkat(directory, finalBytes.constData(), 0) < 0)
            return fail(error("empty-delete-target", "publish", name));
    } else if (options.mode == Mode::KeepExisting) {
        if (::linkat(directory, stage.name.constData(), directory, finalBytes.constData(), 0) < 0) {
            if (errno != EEXIST)
                return fail(error("rename-failed", "publish", name));
            result.existing = true;
            result.error = cleanup();
            return result;
        }
    } else {
        if (::renameat(directory, stage.name.constData(), directory, finalBytes.constData()) < 0)
            return fail(error("rename-failed", "publish", name));
        stage.name.clear();
    }
    result.published = true;
    if (options.published) {
        struct stat state;
        options.published(options.mode != Mode::Remove && ::fstat(stage.file.fd, &state) == 0
                              ? fileIdentityStamp(state)
                              : QByteArray());
    }
    failure = cleanup();
    if (!failure.empty())
        return fail(failure);
    failure = checkpoint("directory-sync", name);
    if (!failure.empty())
        return fail(failure);
    if (::fsync(directory) < 0)
        return fail(error("directory-sync-failed", "directory-sync", name));
    return result;
}
} // namespace configqml
