// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configrevision_p.h"
#include <QFile>

namespace configqml {
ConfigRevision fileRevision(bool exists, const QString &target, const QByteArray &stamp,
                            const QByteArray &digest, const QByteArray &parents) {
    const auto identity =
        QFile::encodeName(target).toHex() + '|' + stamp + '|' + digest + '|' + parents;
    return {exists, compactIdentity(identity),
            std::make_shared<const ConfigRevisionWitness>(ConfigRevisionWitness{
                QString(target.constData(), target.size()), stamp, digest, parents})};
}
QVariantMap ConfigRevision::details() const {
    if (!witness)
        return {};
    return {{"target", witness->target},
            {"stamp", QString::fromLatin1(witness->stamp)},
            {"digest", QString::fromLatin1(witness->digest)},
            {"parents", QString::fromLatin1(witness->parents)}};
}
} // namespace configqml
