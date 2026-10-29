// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configqmlstore.h"
#include "configqmlmodel.h"
#include "storeprobe.h"
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QQuickItem>
#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QTest>
#include <QElapsedTimer>
#include "modelbudgets.h"
using namespace configqml;

class QmlModelsTest : public QObject {
    Q_OBJECT
    static QVariantMap row(QString key, QString title, int n) {
        return {{"key", key},
                {"title", title},
                {"wide", QString("18446744073709551615")},
                {"nested", QVariantMap{{"n", n}, {"items", QVariantList{1, 2}}}}};
    }
private slots:
    void borrowedModelKeys() {
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQml
            import ConfigQml 1.0
            ConfigStore {
                schema: ConfigSchema {
                    ConfigValue { key: "rows"; valueType: ConfigValue.StringList; defaultValue: ["rows-value"] }
                    ConfigValue { key: "tags"; valueType: ConfigValue.StringList; defaultValue: ["tags-value"] }
                    ConfigValue { key: "more"; valueType: ConfigValue.StringList; defaultValue: ["more-value"] }
                    ConfigValue { key: "maps"; valueType: ConfigValue.Map; defaultValue: ({id: "maps-value"}); ConfigValue { valueType: ConfigValue.String } }
                    ConfigValue { key: "taps"; valueType: ConfigValue.Map; defaultValue: ({id: "taps-value"}); ConfigValue { valueType: ConfigValue.String } }
                }
            }
        )",
                          QUrl("file:///borrowed-model.qml"));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *store = qobject_cast<ConfigQmlStore *>(root.get());
        QVERIFY(store);
        QTRY_VERIFY(store->ready());
        QChar key[] = {QChar('r'), QChar('o'), QChar('w'), QChar('s')};
        auto *rows = store->listModel({QString::fromRawData(key, 4)});
        key[0] = QChar('t');
        key[1] = QChar('a');
        key[2] = QChar('g');
        auto *tags = store->listModel({"tags"});
        QVERIFY(rows != tags);
        QCOMPARE(tags->data(tags->index(0, 0), Qt::UserRole).toString(), QString("tags-value"));
        {
            QString transient = "more";
            QVERIFY(
                store->listModel({QString::fromRawData(transient.constData(), transient.size())}));
        }
        QCOMPARE(store->listModel({"more"})
                     ->data(store->listModel({"more"})->index(0, 0), Qt::UserRole)
                     .toString(),
                 QString("more-value"));
        key[0] = QChar('m');
        key[1] = QChar('a');
        key[2] = QChar('p');
        auto *maps = store->keyedModel({QString::fromRawData(key, 4)});
        key[0] = QChar('t');
        auto *taps = store->keyedModel({"taps"});
        QVERIFY(maps != taps);
        QCOMPARE(taps->data(taps->index(0, 0), Qt::UserRole).toString(), QString("taps-value"));
        ConfigQmlSchema schema;
        ConfigQmlValue leaf;
        key[0] = QChar('r');
        key[1] = QChar('o');
        key[2] = QChar('w');
        leaf.key = QString::fromRawData(key, 4);
        leaf.defaultValue = QString("frozen-name");
        auto children = schema.children();
        children.append(&children, &leaf);
        ConfigQmlStore manual;
        manual.setSchema(&schema);
        manual.componentComplete();
        QTRY_VERIFY(manual.ready());
        key[0] = QChar('t');
        key[1] = QChar('a');
        key[2] = QChar('g');
        QCOMPARE(manual.values()->property("rows").toString(), QString("frozen-name"));
    }
    void consumers() {
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QML_MODEL_FIXTURE));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *store = root->property("store").value<ConfigQmlStore *>();
        QVERIFY(store);
        QTRY_VERIFY(store->ready());
        auto *model = store->listModel({"rows"});
        QAbstractItemModelTester tester(model,
                                        QAbstractItemModelTester::FailureReportingMode::Fatal);
        QQuickWindow window;
        auto *item = qobject_cast<QQuickItem *>(root.get());
        QVERIFY(item);
        item->setParentItem(window.contentItem());
        window.resize(400, 500);
        window.show();
        auto *view = root->property("list").value<QObject *>();
        auto *repeater = root->property("repeater").value<QObject *>();
        QVERIFY(view && repeater);
        QTRY_COMPARE(view->property("count").toInt(), 3);
        QTRY_COMPARE(repeater->property("count").toInt(), 3);
        auto rowAt = [&](int index) {
            QVariant result;
            QMetaObject::invokeMethod(root.get(), "rowAt", Q_RETURN_ARG(QVariant, result),
                                      Q_ARG(int, index));
            return result.value<QJSValue>().isObject()
                       ? result.value<QJSValue>().toVariant().toMap()
                       : result.toMap();
        };
        QTRY_COMPARE(rowAt(1).value("text").toString(), QString("b:B"));
        const auto serialB = rowAt(1).value("serial");
        QMetaObject::invokeMethod(root.get(), "choose", Q_ARG(int, 1));
        QMetaObject::invokeMethod(root.get(), "retain");
        QString result;
        QVERIFY(
            QMetaObject::invokeMethod(root.get(), "protectedWrite", Q_RETURN_ARG(QString, result)));
        QVERIFY2(result != "no-error", qPrintable(result));
        QVERIFY(
            QMetaObject::invokeMethod(root.get(), "directWrite", Q_RETURN_ARG(QString, result)));
        QVERIFY2(result.contains("read-only-model"), qPrintable(result));
        QMetaObject::invokeMethod(root.get(), "roleWrite");
        QVERIFY2(root->property("writeError").toString().contains("read-only-model"),
                 qPrintable(root->property("writeError").toString()));
        QCOMPARE(store->core()->inputs().size(), 0);
        QVERIFY(QMetaObject::invokeMethod(root.get(), "nestedWide", Q_RETURN_ARG(QString, result)));
        QCOMPARE(result, QString("string:18446744073709551615"));
        QVERIFY(QMetaObject::invokeMethod(root.get(), "monitorAt", Q_RETURN_ARG(QString, result),
                                          Q_ARG(int, 1)));
        QCOMPARE(result, QString("DP.1/a_..:offline"));
        QVERIFY(QMetaObject::invokeMethod(root.get(), "numberAt", Q_RETURN_ARG(QString, result),
                                          Q_ARG(int, 0)));
        QCOMPARE(result, QString("string:18446744073709551615"));
        QVariant replacement =
            QVariantList{row("c", "C", 3), row("a", "A", 1), row("b", "NEW", 77)};
        QMetaObject::invokeMethod(root.get(), "replaceRows", Q_ARG(QVariant, replacement));
        QTRY_COMPARE(rowAt(2).value("text").toString(), QString("b:NEW"));
        QCOMPARE(rowAt(2).value("serial"), serialB);
        QTRY_COMPARE(view->property("currentIndex").toInt(), 2);
        QTRY_VERIFY(view->property("currentItem").value<QObject *>());
        auto *current = view->property("currentItem").value<QObject *>();
        QTRY_COMPARE(current->property("text").toString(), QString("b:NEW:77"));
        QCOMPARE(current->property("exact").toString(), QString("18446744073709551615"));
        const auto retained = root->property("retained").value<ConfigNode>();
        QVERIFY(retained.valid());
        QCOMPARE(retained.get("n").value().toLongLong(), 2);
        QCOMPARE(retained.get("items").at(0).value().toLongLong(), 3);
        replacement = QVariantList{row("b", "replaced", 88), row("x", "X", 1), row("y", "Y", 2)};
        QMetaObject::invokeMethod(root.get(), "replaceRows", Q_ARG(QVariant, replacement));
        QTRY_COMPARE(rowAt(0).value("text").toString(), QString("b:replaced"));
        QCOMPARE(rowAt(0).value("serial"), serialB);
        QTRY_COMPARE(rowAt(2).value("text").toString(), QString("y:Y"));
        replacement = QVariantList{row("b", "small", 8)};
        QMetaObject::invokeMethod(root.get(), "replaceRows", Q_ARG(QVariant, replacement));
        QTRY_COMPARE(repeater->property("count").toInt(), 1);
        QTRY_COMPARE(rowAt(0).value("text").toString(), QString("b:small"));
        QCOMPARE(rowAt(0).value("serial"), serialB);
        QTRY_COMPARE(view->property("currentIndex").toInt(), 0);
        auto *keyed = store->keyedModel({"monitors"});
        QCOMPARE(keyed->rowCount(), 2);
        QCOMPARE(keyed->data(keyed->index(1, 0), Qt::UserRole + 1).toString(),
                 QString("DP.1/a_.."));
        item->setParentItem(nullptr);
    }
    void thousandRowReorder() {
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QML_MODEL_FIXTURE));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *store = root->property("store").value<ConfigQmlStore *>();
        QTRY_VERIFY(store->ready());
        QQuickWindow window;
        auto *item = qobject_cast<QQuickItem *>(root.get());
        item->setParentItem(window.contentItem());
        window.resize(400, 500);
        window.show();
        auto *repeater = root->property("repeater").value<QObject *>();
        QElapsedTimer initial;
        initial.start();
        QMetaObject::invokeMethod(root.get(), "populate", Q_ARG(int, 1000));
        const auto initialIngress = initial.nsecsElapsed();
        QTRY_COMPARE(repeater->property("count").toInt(), 1000);
        qInfo("BUDGET QML write rows=1000 ingress_us=%lld publication_library_us=%lld",
              initialIngress / 1000,
              ConfigStoreTestAccess::publicationLibraryNanos(*store->core()) / 1000);
        if (enforceModelBudgets)
            QVERIFY(initialIngress +
                        ConfigStoreTestAccess::publicationLibraryNanos(*store->core()) <
                    16000000);
        QTest::qWait(100); // Let ListView finish its asynchronous offscreen cache population.
        const auto created = root->property("created").toInt();
        const auto destroyed = root->property("destroyed").toInt();
        auto *model = store->listModel({"rows"});
        QSignalSpy reset(model, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(model, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(model, &QAbstractItemModel::rowsRemoved);
        QElapsedTimer timer;
        timer.start();
        QMetaObject::invokeMethod(root.get(), "rotateRows");
        const auto ingressNs = timer.nsecsElapsed();
        const auto ingressMillis = timer.elapsed();
        QTRY_COMPARE(model->data(model->index(0, 0), Qt::UserRole + 1).toString(),
                     QString("row-1"));
        QTRY_COMPARE(repeater->property("count").toInt(), 1000);
        QCOMPARE(root->property("created").toInt(), created);
        QCOMPARE(root->property("destroyed").toInt(), destroyed);
        QCOMPARE(reset.size(), 0);
        QCOMPARE(inserted.size(), 0);
        QCOMPARE(removed.size(), 0);
        QVariant first;
        QMetaObject::invokeMethod(root.get(), "rowAt", Q_RETURN_ARG(QVariant, first),
                                  Q_ARG(int, 0));
        const auto firstMap = first.metaType() == QMetaType::fromType<QJSValue>()
                                  ? first.value<QJSValue>().toVariant().toMap()
                                  : first.toMap();
        QCOMPARE(firstMap.value("key").toString(), QString("row-1"));
        qInfo("1000-row small rotate: consumer row-1 in %lld ms; ingress %lld ms; worst GUI "
              "publication %lld us; zero recreated delegates",
              timer.elapsed(), ingressMillis,
              ConfigStoreTestAccess::worstPublicationNanos(*store->core()) / 1000);
        if (enforceModelBudgets)
            QVERIFY(ingressNs + ConfigStoreTestAccess::publicationLibraryNanos(*store->core()) <
                    16000000);
        timer.restart();
        QMetaObject::invokeMethod(root.get(), "reverseRows");
        const auto reverseIngress = timer.nsecsElapsed();
        QTRY_COMPARE(model->data(model->index(0, 0), Qt::UserRole + 1).toString(),
                     QString("row-0"));
        QCOMPARE(reset.size(), 1);
        QCOMPARE(inserted.size(), 0);
        QCOMPARE(removed.size(), 0);
        auto *native = qobject_cast<ConfigListModel *>(model);
        QCOMPARE(native->indexOfRow("row-999"), 1);
        QMetaObject::invokeMethod(root.get(), "chooseKey", Q_ARG(QString, QString("row-999")));
        auto *view = root->property("list").value<QObject *>();
        QTRY_COMPARE(view->property("currentIndex").toInt(), 1);
        QTRY_VERIFY(view->property("currentItem").value<QObject *>());
        QCOMPARE(view->property("currentItem").value<QObject *>()->property("rowId").toString(),
                 QString("row-999"));
        const auto apply = ConfigStoreTestAccess::publicationLibraryNanos(*store->core());
        const auto viewNs = ConfigStoreTestAccess::modelNotificationNanos(*native);
        const auto planning = ConfigStoreTestAccess::planningNanos(*store->core());
        if (enforceModelBudgets) {
            QVERIFY(reverseIngress + apply < 16000000);
            QVERIFY(planning < 50000000);
        }
        QTest::qWait(100);
        ConfigStoreTestAccess::plainReset(*store->core(), *native);
        QCOMPARE(reset.size(), 2);
        qInfo(
            "BUDGET QML reverse rows=1000 ingress_us=%lld apply_library_us=%lld worker_us=%lld "
            "view_us=%lld plain_reset_view_us=%lld enforce=%d; same single-reset notification path",
            reverseIngress / 1000, apply / 1000, planning / 1000, viewNs / 1000,
            ConfigStoreTestAccess::modelNotificationNanos(*native) / 1000, enforceModelBudgets);
        item->setParentItem(nullptr);
    }
    void listViewResetComparison() {
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QML_MODEL_FIXTURE));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        root->setProperty("useRepeater", false);
        auto *store = root->property("store").value<ConfigQmlStore *>();
        QTRY_VERIFY(store->ready());
        QQuickWindow window;
        auto *item = qobject_cast<QQuickItem *>(root.get());
        item->setParentItem(window.contentItem());
        window.resize(400, 500);
        window.show();
        QMetaObject::invokeMethod(root.get(), "populate", Q_ARG(int, 1000));
        auto *model = qobject_cast<ConfigListModel *>(store->listModel({"rows"}));
        QTRY_COMPARE(model->rowCount(), 1000);
        QTest::qWait(100);
        QSignalSpy resets(model, &QAbstractItemModel::modelReset);
        QSignalSpy moves(model, &QAbstractItemModel::rowsMoved);
        QSignalSpy inserts(model, &QAbstractItemModel::rowsInserted);
        QSignalSpy removes(model, &QAbstractItemModel::rowsRemoved);
        qint64 plain = 0, hybrid = 0;
        for (int iteration = 0; iteration < 3; ++iteration) {
            ConfigStoreTestAccess::plainReset(*store->core(), *model);
            plain += ConfigStoreTestAccess::modelNotificationNanos(*model);
            QTest::qWait(100);
            const auto next = model->data(model->index(0), Qt::UserRole + 1).toString() == "row-0"
                                  ? QString("row-999")
                                  : QString("row-0");
            QMetaObject::invokeMethod(root.get(), "reverseRows");
            QTRY_COMPARE(model->data(model->index(0), Qt::UserRole + 1).toString(), next);
            hybrid += ConfigStoreTestAccess::modelNotificationNanos(*model);
            QTest::qWait(100);
        }
        QCOMPARE(resets.size(), 6);
        QCOMPARE(moves.size(), 0);
        QCOMPARE(inserts.size(), 0);
        QCOMPARE(removes.size(), 0);
        qInfo("VIEW ListView rows=1000 hybrid_reset_mean_us=%lld plain_reset_mean_us=%lld; "
              "identical single reset primitive (no extra row notifications)",
              hybrid / 3000, plain / 3000);
        item->setParentItem(nullptr);
    }
    void qmlIngressMargin() {
        QQmlEngine engine;
        engine.addImportPath(QML_IMPORT_ROOT);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QML_MODEL_FIXTURE));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *store = root->property("store").value<ConfigQmlStore *>();
        QTRY_VERIFY(store->ready());
        QQuickWindow window;
        auto *item = qobject_cast<QQuickItem *>(root.get());
        item->setParentItem(window.contentItem());
        window.resize(400, 500);
        window.show();
        QMetaObject::invokeMethod(root.get(), "populate", Q_ARG(int, 1000));
        auto *model = store->listModel({"rows"});
        QTRY_COMPARE(model->rowCount(), 1000);
        QTest::qWait(100);
        qint64 maximum = 0;
        for (int iteration = 0; iteration < 4; ++iteration) {
            QElapsedTimer timer;
            timer.start();
            QVERIFY(QMetaObject::invokeMethod(root.get(), "rotateRows"));
            const auto ingress = timer.nsecsElapsed();
            QTRY_COMPARE(model->data(model->index(0, 0), Qt::UserRole + 1).toString(),
                         QString("row-%1").arg(iteration + 1));
            const auto publication = ConfigStoreTestAccess::publicationLibraryNanos(*store->core());
            maximum = qMax(maximum, ingress + publication);
            qInfo("BUDGET combined-view rotate iteration=%d ingress_us=%lld "
                  "publication_library_us=%lld sum_us=%lld enforce=%d",
                  iteration, ingress / 1000, publication / 1000, (ingress + publication) / 1000,
                  enforceModelBudgets);
            if (enforceModelBudgets)
                QVERIFY(ingress + publication < 16000000);
        }
        QElapsedTimer timer;
        timer.start();
        QVERIFY(QMetaObject::invokeMethod(root.get(), "insertRow"));
        const auto ingress = timer.nsecsElapsed();
        QTRY_COMPARE(model->rowCount(), 1001);
        const auto publication = ConfigStoreTestAccess::publicationLibraryNanos(*store->core());
        QCOMPARE(qobject_cast<ConfigListModel *>(model)->indexOfRow("inserted"), 500);
        qInfo("BUDGET combined-view insert ingress_us=%lld publication_library_us=%lld "
              "max_rotation_sum_us=%lld enforce=%d",
              ingress / 1000, publication / 1000, maximum / 1000, enforceModelBudgets);
        if (enforceModelBudgets)
            QVERIFY(ingress + publication < 16000000);
        item->setParentItem(nullptr);
    }
};
QTEST_MAIN(QmlModelsTest)
#include "test_qml_models.moc"
