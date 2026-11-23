// SPDX-License-Identifier: LGPL-3.0-or-later
#include <configqmlstore.h>
#include <configfacade.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QTimer>
#include <cstdio>

// Freeze a real compiled moc consumer derived from the released QML facade.
// Existing inherited meta-method/property indices and its own dispatch survive.
class OldFacade final : public configqml::ConfigQmlStore {
    Q_OBJECT
    Q_PROPERTY(int marker READ marker WRITE setMarker NOTIFY markerChanged)
public:
    int marker() const { return m_marker; }
    void setMarker(int value) {
        m_marker = value;
        emit markerChanged();
    }
    Q_INVOKABLE int echo(int value) const { return value + marker(); }
signals:
    void markerChanged();

private:
    int m_marker = 0;
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    OldFacade facade;
    auto *schema = new configqml::ConfigQmlSchema(&facade);
    facade.setSchema(schema);
    facade.componentComplete();
    if (!facade.ready()) {
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&facade, &configqml::ConfigQmlStore::statusChanged, &loop,
                         &QEventLoop::quit);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        deadline.start(10000);
        loop.exec();
    }
    if (!facade.ready() || !facade.setProperty("marker", 17) || facade.marker() != 17)
        return 1;
    int echoed = 0;
    if (!QMetaObject::invokeMethod(&facade, "echo", Q_RETURN_ARG(int, echoed), Q_ARG(int, 4)) ||
        echoed != 21)
        return 2;
    const auto &meta = configqml::ConfigQmlStore::staticMetaObject;
    if (meta.indexOfMethod("saveSelected(QVariantList,QJSValue)") < 0 ||
        meta.indexOfProperty("values") < 0)
        return 3;
    std::puts("OLD_METAOBJECT_OK");
    return 0;
}
#include "metaobject.moc"
