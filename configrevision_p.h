// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include "configbackend.h"
#include <QCryptographicHash>

namespace configqml {
inline QByteArray compactIdentity(const QByteArray &identity) {
    return QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex();
}
struct ConfigRevisionWitness {
    QString target;
    QByteArray stamp, digest, parents;
};
ConfigRevision fileRevision(bool exists, const QString &target, const QByteArray &stamp,
                            const QByteArray &digest, const QByteArray &parents);
} // namespace configqml
