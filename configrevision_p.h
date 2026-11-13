// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configbackend.h"
#include <QCryptographicHash>

namespace configqml {
struct ConfigFileDescriptor;
inline QByteArray compactIdentity(const QByteArray &identity) {
    return QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex();
}
struct ConfigRevisionWitness {
    QString target;
    QByteArray stamp, digest, parents;
    // Pending preimages keep only their own newly-created parent identities alive.
    QList<std::shared_ptr<ConfigFileDescriptor>> ownedParents = {};
    QByteArray ownedParentBase;
};
ConfigRevision fileRevision(bool exists, const QString &target, const QByteArray &stamp,
                            const QByteArray &digest, const QByteArray &parents);
} // namespace configqml
