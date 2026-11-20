// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "configdraft.h"
#include "configjsoncodec.h"
#include "confignumber.h"
#include "configoverlayeditor.h"
#include "configstorehandle.h"
#include <QSaveFile>
#include <QTemporaryDir>
#include <atomic>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
namespace {
QByteArray original() {
    return R"({"todos":[],"pages":[],"current_page_id":"","count":0,"completedCount":0,"exportEmptySections":false,"nullable":null,"map":{},"nested":{"n":1,"opaque":{"wide":18446744073709551615,"tiny":1e-400,"items":[false,{},[]]}},"group":{"equal":0,"other":0,"opaque":{"keep":true}},"opaque":{"__proto__":{"keep":false},"wide":18446744073709551615}})";
}
ConfigStoreOptions baseOptions(const QString &file) {
    auto nested = D::leaf(T::Record, QVariantMap{{"n", 1}});
    nested.fields = {{"n", D::leaf(T::SafeInteger, 1)}};
    auto nullable = D::leaf(T::String, QVariant::fromValue(nullptr));
    nullable.nullable = true;
    auto map = D::leaf(T::Map, QVariantMap{});
    map.element = {D::leaf(T::SafeInteger, 0)};
    ConfigStoreOptions o;
    o.autoSave = false;
    o.schema = ConfigSchema(D::group(
        {{"settings", D::group({{"todos", D::list(D::leaf(T::String, QString()), {})},
                                {"pages", D::list(D::leaf(T::String, QString()), {})},
                                {"current_page_id", D::leaf(T::String, QString())},
                                {"count", D::leaf(T::SafeInteger, 0)},
                                {"completedCount", D::leaf(T::SafeInteger, 0)},
                                {"exportEmptySections", D::leaf(T::Boolean, false)},
                                {"nullable", nullable},
                                {"map", map},
                                {"nested", nested},
                                {"group", D::group({{"equal", D::leaf(T::SafeInteger, 0)},
                                                    {"other", D::leaf(T::SafeInteger, 0)}})}})}}));
    o.layers = {{"user",
                 ConfigLayerKind::File,
                 ConfigAdmissionPolicy::FieldFallback,
                 {{"base", {"settings"}, {}}},
                 true}};
    o.jsonFiles = {{"user", "base", file}};
    return o;
}
ConfigStoreOptions ownerOptions(const QString &file) {
    auto row = D::leaf(T::Record, QVariantMap{{"id", QString()}});
    row.fields = {{"id", D::leaf(T::String, QString())}};
    ConfigStoreOptions o;
    o.autoSave = false;
    o.schema = ConfigSchema(
        D::group({{"rows", D::list(row, {})}, {"unrelated", D::leaf(T::SafeInteger, 0)}}));
    o.layers = {{"user",
                 ConfigLayerKind::File,
                 ConfigAdmissionPolicy::FieldFallback,
                 {{"owner", {}, {}}},
                 true}};
    o.jsonFiles = {{"user", "owner", file}};
    return o;
}
QVariantMap raw(const QString &file) {
    return ConfigJsonCodec().decode(compat::bytes(file), "fixture").raw;
}
QVariantMap overrides(const QString &file) {
    return raw(file).value("rows").toList().first().toMap().value("instanceSettings").toMap();
}
QByteArray bytes(const QVariant &value) {
    const auto encoded = encodeJsonValue(value);
    if (!encoded.ok())
        qFatal("fixture encoding");
    return encoded.bytes;
}
QByteArray untouched(QVariantMap value, const QList<ConfigPath> &edits = {}) {
    for (const auto &path : edits)
        removeValue(value, path);
    return bytes(value);
}
ConfigOverlayEditor *open(ConfigStore &owner, ConfigStoreHandle *base,
                          ConfigOverlayMode mode = ConfigOverlayMode::Editor, int index = 0) {
    ConfigOverlayOptions o;
    o.mode = mode;
    o.base = base;
    o.ownerPath = {"rows"};
    o.ownerGeneration = owner.info(o.ownerPath).publicationGeneration;
    o.entryPath = {{ConfigValueSelector::Kind::Index, {}, index}};
    o.identityPath = {{ConfigValueSelector::Kind::Key, "id"}};
    o.identityValue = QString(index ? "sibling" : "todo");
    o.overrideKey = "instanceSettings";
    ConfigOverlayEditor *editor = nullptr;
    const auto connection =
        QObject::connect(&owner, &ConfigStore::overlayOpened, &owner,
                         [&](quint64, ConfigOverlayEditor *created) { editor = created; });
    const auto result = compat::request(owner, [&] { return owner.openOverlay(o); });
    QObject::disconnect(connection);
    if (result.status != "opened" || !editor)
        qFatal("fixture open: %s", qPrintable(result.error.code));
    return editor;
}
void seed(const QString &base, const QString &owner) {
    compat::put(base, original());
    compat::put(owner, "{\"rows\":[{\"id\":\"todo\",\"rowOpaque\":true,\"instanceSettings\":" +
                           original() + "},{\"id\":\"sibling\",\"instanceSettings\":" + original() +
                           "}],\"unrelated\":0,\"rootOpaque\":18446744073709551615}");
}
} // namespace
class OverlayPreserveTest : public QObject {
    Q_OBJECT
private slots:
    void explicitIntent_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("preview");
        for (auto mode : {ConfigOverlayMode::Editor, ConfigOverlayMode::Live})
            for (bool preview : {false, true}) {
                if (preview && mode == ConfigOverlayMode::Editor)
                    continue;
                QTest::newRow(qPrintable(QString("mode-%1-preview-%2").arg(int(mode)).arg(preview)))
                    << int(mode) << preview;
            }
    }
    void explicitIntent() {
        QFETCH(int, mode);
        QFETCH(bool, preview);
        QTemporaryDir dir;
        const auto baseFile = dir.filePath("base.json"), ownerFile = dir.filePath("owner.json");
        seed(baseFile, ownerFile);
        ConfigStore base(baseOptions(baseFile)), owner(ownerOptions(ownerFile));
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(owner.status(), ConfigStore::Status::Ready);
        auto *draft = preview ? base.draft({{"settings"}}) : nullptr;
        auto *scope = draft ? draft->previewScope({"settings"}) : base.scope({"settings"});
        auto *editor = open(owner, scope, ConfigOverlayMode(mode));
        const auto before = overrides(ownerFile);
        const auto baseBefore = compat::bytes(baseFile);
        const auto ownerBefore = compat::bytes(ownerFile);
        const auto siblingBefore = bytes(raw(ownerFile).value("rows").toList()[1]);
        const auto rootBefore = bytes(raw(ownerFile).value("rootOpaque"));
        auto publish = [&] {
            return compat::request(owner, [&] {
                return ConfigOverlayMode(mode) == ConfigOverlayMode::Live ? editor->save()
                                                                          : editor->apply();
            });
        };
        QVERIFY(editor->setValue({"exportEmptySections"}, true).accepted);
        // Setter/preview never silently publishes, even for a live namespace.
        QCOMPARE(compat::bytes(ownerFile), ownerBefore);
        QCOMPARE(publish().status, QString("saved"));
        QCOMPARE(untouched(overrides(ownerFile), {{"exportEmptySections"}}),
                 untouched(before, {{"exportEmptySections"}}));
        QCOMPARE(compat::bytes(baseFile), baseBefore);
        QCOMPARE(bytes(raw(ownerFile).value("rows").toList()[1]), siblingBefore);
        QCOMPARE(bytes(raw(ownerFile).value("rootOpaque")), rootBefore);
        QVERIFY(overrides(ownerFile).value("exportEmptySections").toBool());
        const auto savedBytes = compat::bytes(ownerFile);
        const auto noEdit = publish();
        QVERIFY(noEdit.status == "saved" || noEdit.status == "nothing_to_save");
        QCOMPARE(compat::bytes(ownerFile), savedBytes);
        // Equal omission is intent on this field only, never a map-wide cleanup.
        QVERIFY(editor->setValue({"count"}, 0).accepted);
        QVERIFY(publish().status != "failed");
        QVERIFY(!overrides(ownerFile).contains("count"));
        QCOMPARE(untouched(overrides(ownerFile), {{"exportEmptySections"}, {"count"}}),
                 untouched(before, {{"exportEmptySections"}, {"count"}}));
        QVERIFY(editor->unset({"current_page_id"}));
        QVERIFY(publish().status != "failed");
        QVERIFY(!overrides(ownerFile).contains("current_page_id"));
        const QList<ConfigPath> touched{{"exportEmptySections"}, {"count"}, {"current_page_id"}};
        QCOMPARE(untouched(overrides(ownerFile), touched), untouched(before, touched));
        // Namespace edits must not materialize inherited siblings or remove equal siblings.
        QVERIFY(editor->setValue({"group", "other"}, 3).accepted);
        QVERIFY(publish().status != "failed");
        auto touchedGroup = touched;
        touchedGroup.append(ConfigPath{"group", "other"});
        QCOMPARE(untouched(overrides(ownerFile), touchedGroup), untouched(before, touchedGroup));
        QVERIFY(editor->setValue({"group", "other"}, 0).accepted);
        QVERIFY(publish().status != "failed");
        QCOMPARE(untouched(overrides(ownerFile), touchedGroup), untouched(before, touchedGroup));
        QVERIFY(!overrides(ownerFile).value("group").toMap().contains("other"));
        if (ConfigOverlayMode(mode) == ConfigOverlayMode::Editor) {
            const auto persisted = compat::bytes(ownerFile);
            QVERIFY(editor->setValue({"pages"}, QVariantList{QString("discarded")}).accepted);
            QCOMPARE(compat::bytes(ownerFile), persisted);
            QCOMPARE(compat::request(owner, [&] { return editor->discard(); }).status,
                     QString("discarded"));
            QCOMPARE(compat::bytes(ownerFile), persisted);
        } else {
            const auto persisted = compat::bytes(ownerFile);
            QCOMPARE(compat::request(owner, [&] { return editor->flush(); }).status,
                     QString("nothing_to_save"));
            QCOMPARE(compat::bytes(ownerFile), persisted);
        }
        // A real consumer sees the old own empty list after the base changes later.
        auto *view = open(owner, base.scope({"settings"}), ConfigOverlayMode::View);
        auto *inherited = open(owner, base.scope({"settings"}), ConfigOverlayMode::View, 1);
        const auto persisted = compat::bytes(ownerFile);
        QVERIFY(!view->setValue({"count"}, 99).accepted);
        QVERIFY(!view->unset({"count"}));
        for (const auto operation :
             {&ConfigOverlayEditor::apply, &ConfigOverlayEditor::save, &ConfigOverlayEditor::flush,
              &ConfigOverlayEditor::discard, &ConfigOverlayEditor::resetOverrides}) {
            QCOMPARE(
                compat::request(owner, [&] { return (view->*operation)(nullptr, {}); }).error.code,
                QString("overlay-readonly"));
            QCOMPARE(compat::bytes(ownerFile), persisted);
        }
        QVERIFY(
            base.setUser({"settings", "todos"}, QVariantList{QString("new shared task")}).accepted);
        QCOMPARE(compat::request(base, [&] { return base.save(); }).status, QString("saved"));
        QCOMPARE(compat::request(base, [&] { return base.reload(); }).status, QString("reloaded"));
        QTRY_COMPARE(view->value({"todos"}).toList(), QVariantList{});
        QTRY_COMPARE(inherited->value({"todos"}).toList(), QVariantList{});
        QCOMPARE(compat::bytes(ownerFile), persisted);
        QCOMPARE(untouched(overrides(ownerFile), touchedGroup), untouched(before, touchedGroup));
        ConfigStore cold(ownerOptions(ownerFile));
        QTRY_COMPARE(cold.status(), ConfigStore::Status::Ready);
        QCOMPARE(open(cold, base.scope({"settings"}))->value({"todos"}).toList(), QVariantList{});
        // Whole-overlay Reset intentionally owns all selected overrides, not its sibling row.
        auto *reset = open(owner, base.scope({"settings"}));
        QCOMPARE(compat::request(owner, [&] { return reset->resetOverrides(); }).status,
                 QString("saved"));
        QVERIFY(!raw(ownerFile).value("rows").toList()[0].toMap().contains("instanceSettings"));
        QCOMPARE(bytes(raw(ownerFile).value("rows").toList()[1]), siblingBefore);
        QCOMPARE(bytes(raw(ownerFile).value("rootOpaque")), rootBefore);
        auto *afterReset = open(owner, base.scope({"settings"}), ConfigOverlayMode::View);
        QCOMPARE(afterReset->value({"todos"}).toList(), QVariantList{QString("new shared task")});
    }
    void draftsAndScopedReset() {
        QTemporaryDir dir;
        const auto baseFile = dir.filePath("base.json"), ownerFile = dir.filePath("owner.json");
        seed(baseFile, ownerFile);
        ConfigStore base(baseOptions(baseFile)), owner(ownerOptions(ownerFile));
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(owner.status(), ConfigStore::Status::Ready);
        const auto originalBase = raw(baseFile);
        const auto originalOwner = compat::bytes(ownerFile);
        auto *draft = base.draft({{"settings"}});
        auto *live = open(owner, draft->previewScope({"settings"}), ConfigOverlayMode::Live);
        QVERIFY(draft->setValue({"settings", "exportEmptySections"}, true).accepted);
        QCOMPARE(draft->previewValue({"settings", "exportEmptySections"}).toBool(), true);
        QCOMPARE(untouched(raw(baseFile)), untouched(originalBase));
        QCOMPARE(compat::bytes(ownerFile), originalOwner);
        QVERIFY(draft->discard());
        QCOMPARE(untouched(raw(baseFile)), untouched(originalBase));
        QCOMPARE(compat::bytes(ownerFile), originalOwner);
        QTRY_COMPARE(live->error(), QString("overlay-owner"));
        QVERIFY(draft->setValue({"settings", "exportEmptySections"}, true).accepted);
        QCOMPARE(compat::request(base, [&] { return draft->apply(); }).status, QString("saved"));
        QCOMPARE(untouched(raw(baseFile), {{"exportEmptySections"}}),
                 untouched(originalBase, {{"exportEmptySections"}}));
        QCOMPARE(compat::bytes(ownerFile), originalOwner);
        QCOMPARE(compat::request(base, [&] { return base.flush(); }).status,
                 QString("nothing_to_save"));
        QVERIFY(base.reset({"settings", "exportEmptySections"}, "user").accepted);
        QCOMPARE(compat::request(base, [&] { return base.save(); }).status, QString("saved"));
        QVERIFY(!raw(baseFile).contains("exportEmptySections"));
        QCOMPARE(untouched(raw(baseFile), {{"exportEmptySections"}}),
                 untouched(originalBase, {{"exportEmptySections"}}));
        QCOMPARE(compat::bytes(ownerFile), originalOwner);
        // External refresh changes only the authored foreign key. No draft/preview writeback.
        auto *foreignDraft = base.draft({{"settings"}});
        auto external = raw(baseFile);
        external["exportEmptySections"] = false;
        QSaveFile replacement(baseFile);
        QVERIFY(replacement.open(QIODevice::WriteOnly));
        QVERIFY(replacement.write(bytes(external)) > 0);
        QVERIFY(replacement.commit());
        QCOMPARE(compat::request(base, [&] { return base.reload(); }).status, QString("reloaded"));
        QVERIFY(foreignDraft->conflicted());
        QCOMPARE(untouched(raw(baseFile), {{"exportEmptySections"}}),
                 untouched(originalBase, {{"exportEmptySections"}}));
        QCOMPARE(compat::bytes(ownerFile), originalOwner);
        auto *ownerDraft = owner.draft({{"unrelated"}});
        QVERIFY(ownerDraft->setValue({"unrelated"}, 5).accepted);
        QCOMPARE(compat::bytes(ownerFile), originalOwner);
        QCOMPARE(compat::request(owner, [&] { return ownerDraft->apply(); }).status,
                 QString("saved"));
        QCOMPARE(bytes(overrides(ownerFile)), bytes(decodeJsonValue(original()).value));
        const auto ownerAfterDraft = compat::bytes(ownerFile);
        QCOMPARE(compat::request(owner, [&] { return owner.flush(); }).status,
                 QString("nothing_to_save"));
        QCOMPARE(compat::bytes(ownerFile), ownerAfterDraft);
        QVERIFY(owner.reset({"unrelated"}, "user").accepted);
        QCOMPARE(compat::request(owner, [&] { return owner.save(); }).status, QString("saved"));
        QCOMPARE(bytes(overrides(ownerFile)), bytes(decodeJsonValue(original()).value));
    }
    void refreshPreservesStoredChoices() {
        QTemporaryDir dir;
        const auto baseFile = dir.filePath("base.json"), ownerFile = dir.filePath("owner.json");
        seed(baseFile, ownerFile);
        ConfigStore base(baseOptions(baseFile)), owner(ownerOptions(ownerFile));
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(owner.status(), ConfigStore::Status::Ready);
        auto *draft = base.draft({{"settings"}});
        auto *live = open(owner, draft->previewScope({"settings"}), ConfigOverlayMode::Live);
        const auto before = bytes(overrides(ownerFile));
        auto foreign = raw(ownerFile);
        foreign["unrelated"] = 5;
        QSaveFile replacement(ownerFile);
        QVERIFY(replacement.open(QIODevice::WriteOnly));
        QVERIFY(replacement.write(bytes(foreign)) > 0);
        QVERIFY(replacement.commit());
        QCOMPARE(compat::request(owner, [&] { return owner.reload(); }).status,
                 QString("reloaded"));
        QCOMPARE(bytes(overrides(ownerFile)), before);
        const auto afterForeign = compat::bytes(ownerFile);
        QVERIFY(
            draft->setValue({"settings", "todos"}, QVariantList{QString("preview task")}).accepted);
        // Acquiring another view settles the worker preview, not a persistence intent.
        auto *view = open(owner, draft->previewScope({"settings"}), ConfigOverlayMode::View);
        QCOMPARE(view->value({"todos"}).toList(), QVariantList{});
        QCOMPARE(compat::request(owner, [&] { return live->save(); }).status,
                 QString("nothing_to_save"));
        QCOMPARE(compat::bytes(ownerFile), afterForeign);
        QCOMPARE(bytes(overrides(ownerFile)), before);
        QVERIFY(live->setValue({"exportEmptySections"}, true).accepted);
        QCOMPARE(compat::request(owner, [&] { return live->flush(); }).status, QString("saved"));
        QCOMPARE(untouched(overrides(ownerFile), {{"exportEmptySections"}}),
                 untouched(decodeJsonValue(original()).value.toMap(), {{"exportEmptySections"}}));
    }
    void inheritedAbsenceIsNotAnEdit_data() {
        QTest::addColumn<int>("mode");
        for (auto mode : {ConfigOverlayMode::Editor, ConfigOverlayMode::Live})
            QTest::newRow(qPrintable(QString::number(int(mode)))) << int(mode);
    }
    void inheritedAbsenceIsNotAnEdit() {
        QFETCH(int, mode);
        QTemporaryDir dir;
        const auto baseFile = dir.filePath("base.json"), ownerFile = dir.filePath("owner.json");
        seed(baseFile, ownerFile);
        compat::put(ownerFile,
                    R"({"rows":[{"id":"todo","instanceSettings":{"opaque":{"keep":true}}}]})");
        ConfigStore base(baseOptions(baseFile)), owner(ownerOptions(ownerFile));
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(owner.status(), ConfigStore::Status::Ready);
        auto *editor = open(owner, base.scope({"settings"}), ConfigOverlayMode(mode));
        const auto before = bytes(overrides(ownerFile));
        QVERIFY(editor->unset({"group", "other"}));
        const auto unsetResult = compat::request(owner, [&] {
            return ConfigOverlayMode(mode) == ConfigOverlayMode::Live ? editor->flush()
                                                                      : editor->apply();
        });
        QVERIFY(unsetResult.status != "failed");
        QCOMPARE(bytes(overrides(ownerFile)), before);
        QVERIFY(editor->setValue({"exportEmptySections"}, true).accepted);
        const auto result = compat::request(owner, [&] {
            return ConfigOverlayMode(mode) == ConfigOverlayMode::Live ? editor->save()
                                                                      : editor->apply();
        });
        QCOMPARE(result.status, QString("saved"));
        QCOMPARE(untouched(overrides(ownerFile), {{"exportEmptySections"}}), before);
        QVERIFY(!overrides(ownerFile).contains("group"));
    }
    void failedApplyRetry() {
        QTemporaryDir dir;
        const auto baseFile = dir.filePath("base.json"), ownerFile = dir.filePath("owner.json");
        seed(baseFile, ownerFile);
        auto options = ownerOptions(ownerFile);
        auto once = std::make_shared<std::atomic_bool>(true);
        options.fileFault = [once](const QString &phase, const QString &) {
            return phase == "write" && once->exchange(false) ? QString("fixture-write") : QString();
        };
        ConfigStore base(baseOptions(baseFile)), owner(options);
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(owner.status(), ConfigStore::Status::Ready);
        const auto before = overrides(ownerFile);
        const auto beforeBytes = compat::bytes(ownerFile);
        auto *editor = open(owner, base.scope({"settings"}));
        QVERIFY(editor->setValue({"exportEmptySections"}, true).accepted);
        QCOMPARE(compat::request(owner, [&] { return editor->apply(); }).error.code,
                 QString("fixture-write"));
        QCOMPARE(compat::bytes(ownerFile), beforeBytes);
        const auto revision = owner.info({"rows"}).intentRevision;
        QCOMPARE(compat::request(owner, [&] { return editor->apply(); }).status, QString("saved"));
        QCOMPARE(owner.info({"rows"}).intentRevision, revision);
        QCOMPARE(untouched(overrides(ownerFile), {{"exportEmptySections"}}),
                 untouched(before, {{"exportEmptySections"}}));
    }
};
QTEST_GUILESS_MAIN(OverlayPreserveTest)
#include "test_overlay_preserve.moc"
