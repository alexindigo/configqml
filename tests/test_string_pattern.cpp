// SPDX-License-Identifier: LGPL-3.0-or-later
#include "compatfixture.h"
#include "configenvironment.h"
#include "configoverlayeditor.h"
#include "configstorehandle.h"
#include <QTemporaryDir>
using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;
namespace {
D word() {
    auto d = D::leaf(T::String, QString("OK"));
    d.stringPattern = "[A-Z]{2}";
    return d;
}
ConfigStoreOptions options(const QString &file = {}) {
    ConfigStoreOptions o;
    o.autoSave = false;
    o.schema = ConfigSchema(D::group({{"settings", D::group({{"word", word()}})}}));
    o.layers = {{"user",
                 ConfigLayerKind::File,
                 ConfigAdmissionPolicy::FieldFallback,
                 {{"user", {"settings"}, {}}},
                 true}};
    if (!file.isEmpty())
        o.jsonFiles = {{"user", "user", file}};
    return o;
}
} // namespace
class StringPatternTest : public QObject {
    Q_OBJECT
private slots:
    void typeConstructorSourceCompatibility() {
        using Type = ConfigDescriptor::Type;
        const auto descriptor = ConfigDescriptor{Type::String};
        QCOMPARE(descriptor.type, Type::String);
        QVERIFY(descriptor.validate(QString("unchanged")).ok());
    }
    void nestedDescriptorAssignmentOwnsMetadata() {
        auto record = D::leaf(T::Record, QVariantMap{{"label", "OK"}});
        record.fields = {{"label", word()}};
        auto list = D::list(record, {});
        ConfigSchema schema(D::group({{"rows", list}}));
        QVERIFY(schema.valid());
        // The schema copy owns compiled pattern metadata. Selecting an lvalue
        // child must not destroy that source halfway through assignment.
        auto selected = *schema.descriptor({"rows"});
        selected = selected.element.first();
        QCOMPARE(selected.type, T::Record);
        selected = selected.fields["label"];
        QCOMPARE(selected.stringPattern, QString("[A-Z]{2}"));
        QVERIFY(selected.validate(QString("UP")).ok());
        QCOMPARE(selected.validate(QString("bad")).errors.first().code, QString("string-pattern"));
        selected = selected;
        QVERIFY(selected.validate(QString("OK")).ok());
        selected = std::move(selected);
        QVERIFY(selected.validate(QString("HI")).ok());
    }
    void schemaOwnershipAndAnchoring() {
        auto d = word();
        d.stringPattern = "(";
        ConfigSchema invalid(D::group({{"word", d}}));
        QVERIFY(!invalid.valid());
        QCOMPARE(invalid.errors().first().code, QString("invalid-string-pattern"));
        d = D::leaf(T::Boolean, false);
        d.stringPattern = "false";
        ConfigSchema wrong(D::group({{"word", d}}));
        QVERIFY(!wrong.valid());
        QCOMPARE(wrong.errors().first().code, QString("string-pattern-type"));
        auto badDefault = word();
        badDefault.defaultValue = QString("bad");
        ConfigSchema defaults(D::group({{"word", badDefault}}));
        QVERIFY(!defaults.valid());
        QCOMPARE(defaults.errors().first().code, QString("string-pattern"));
        QString storage = "[A-Z]{2}";
        d = word();
        d.stringPattern = QString::fromRawData(storage.constData(), storage.size());
        ConfigSchema frozen(D::group({{"word", d}}));
        QVERIFY(frozen.valid());
        storage[1] = 'a';
        QCOMPARE(frozen.descriptor({"word"})->stringPattern, QString("[A-Z]{2}"));
        QVERIFY(frozen.validate({"word"}, QString("UP")).ok());
        for (const auto &text : {QString("bad"), QString("OK\n"), QString("prefixOK")})
            QCOMPARE(frozen.validate({"word"}, text).errors.first().code,
                     QString("string-pattern"));
        d.stringPattern = "(?i)ok";
        QVERIFY(d.validate(QString("oK")).ok());
        d.nullable = true;
        QVERIFY(d.validate(QVariant::fromValue(nullptr)).ok());
        d.stringPattern.clear();
        QVERIFY(d.validate(QString("anything\n")).ok());
    }
    void rawEnvironmentNestedAndSensitive() {
        QTemporaryDir dir;
        const auto file = dir.filePath("settings.json");
        const QByteArray bytes("{\"word\":\"bad\",\"unknown\":0}");
        compat::put(file, bytes);
        ConfigStore store(options(file));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(store.value({"settings", "word"}).toString(), QString("OK"));
        QCOMPARE(compat::request(store, [&] { return store.save(); }).status,
                 QString("nothing_to_save"));
        QCOMPARE(compat::bytes(file), bytes);
        const auto schema = options().schema;
        QVERIFY(!mapEnvironment(schema, {{"WORD", {"settings", "word"}}}, {{"WORD", "bad"}}).ok());
        QVERIFY(mapEnvironment(schema, {{"WORD", {"settings", "word"}}}, {{"WORD", "UP"}}).ok());
        auto nested = D::leaf(T::Map, QVariantMap{});
        nested.element = {word()};
        nested.sensitive = true;
        ConfigSchema sensitive(D::group({{"tokens", nested}}));
        const auto rejected = sensitive.validate({"tokens"}, QVariantMap{{"opaque-secret", "bad"}});
        QCOMPARE(rejected.errors.first().code, QString("string-pattern"));
        QCOMPARE(rejected.errors.first().path, ConfigPath({"tokens", "<redacted>"}));
        auto record = D::leaf(T::Record, QVariantMap{{"label", "OK"}});
        record.fields = {{"label", word()}};
        ConfigSchema lists(D::group({{"rows", D::list(record, {})}}));
        QCOMPARE(lists.validate({"rows"}, QVariantList{QVariantMap{{"label", "bad"}}})
                     .errors.first()
                     .code,
                 QString("string-pattern"));
        auto strictOptions = options(file);
        strictOptions.layers[0].policy = ConfigAdmissionPolicy::RejectInput;
        ConfigStore strict(strictOptions);
        QTRY_COMPARE(strict.status(), ConfigStore::Status::Error);
        QCOMPARE(compat::bytes(file), bytes);
    }
    void allPersistentProducersShareAdmission() {
        QTemporaryDir dir;
        const auto file = dir.filePath("settings.json");
        compat::put(file, "{\"word\":\"OK\"}");
        ConfigStore store(options(file));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        const auto inputCount = store.inputs().size();
        QVERIFY(!store.setUser({"settings", "word"}, QString("bad")).accepted);
        QVERIFY(!store.scope({"settings"})->setUser({"word"}, QString("bad")).accepted);
        auto *draft = store.draft({{"settings"}});
        QVERIFY(!draft->setValue({"settings", "word"}, QString("bad")).accepted);
        QCOMPARE(store.inputs().size(), inputCount);
        ConfigRetainedGroupOptions group;
        group.requestKey = "bad";
        group.paths = {{"settings"}};
        group.edits = {{{"settings", "word"}, true, QString("bad")}};
        QCOMPARE(compat::request(store, [&] { return store.retainGroup(group); }).error.code,
                 QString("string-pattern"));
        const auto profile = dir.filePath("bad-profile.json");
        compat::put(profile, "{\"format\":1,\"name\":\"bad\",\"entries\":[{\"path\":[\"settings\","
                             "\"word\"],\"present\":true,\"value\":\"bad\"}]}");
        QCOMPARE(compat::request(store, [&] { return store.importProfile(profile); }).status,
                 QString("failed"));
        QCOMPARE(compat::bytes(file), QByteArray("{\"word\":\"OK\"}"));
        QVERIFY(draft->setValue({"settings", "word"}, QString("UP")).accepted);
        QCOMPARE(compat::request(store, [&] { return draft->apply(); }).status, QString("saved"));
        ConfigStore restarted(options(file));
        QTRY_COMPARE(restarted.status(), ConfigStore::Status::Ready);
        QCOMPARE(restarted.value({"settings", "word"}).toString(), QString("UP"));
        group.requestKey = "valid";
        group.edits[0].value = QString("HI");
        const auto retained = compat::request(store, [&] { return store.retainGroup(group); });
        QCOMPARE(retained.status, QString("retained"));
        QCOMPARE(
            compat::request(store, [&] { return store.saveGroup(retained.groupToken); }).status,
            QString("saved"));
        compat::put(profile, "{\"format\":1,\"name\":\"valid\",\"entries\":[{\"path\":["
                             "\"settings\",\"word\"],\"present\":true,\"value\":\"OK\"}]}");
        QCOMPARE(compat::request(store, [&] { return store.importProfile(profile); }).status,
                 QString("saved"));
        QCOMPARE(store.value({"settings", "word"}).toString(), QString("OK"));
    }
    void listAndOverlayProducers() {
        auto row = D::leaf(T::Record, QVariantMap{{"id", "x"}, {"label", "OK"}});
        row.fields = {{"id", D::leaf(T::String, QString("x"))}, {"label", word()}};
        ConfigStoreOptions mainOptions;
        mainOptions.autoSave = false;
        mainOptions.schema = ConfigSchema(
            D::group({{"rows", D::list(row, {QVariantMap{{"id", "x"}, {"label", "OK"}}})}}));
        mainOptions.layers = {{"user",
                               ConfigLayerKind::File,
                               ConfigAdmissionPolicy::FieldFallback,
                               {{"main", {}, {}}},
                               true}};
        ConfigRecordListPolicySpec policy;
        policy.policyId = "rows";
        policy.path = {"rows"};
        policy.keyField = "id";
        mainOptions.recordListPolicies = {policy};
        ConfigStore base(options()), main(mainOptions);
        QTRY_COMPARE(base.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(main.status(), ConfigStore::Status::Ready);
        ConfigRecordListMetadata metadata;
        metadata.ready = true;
        metadata.entries = {{"x", true, false, true, {{"label", "bad"}}}};
        QCOMPARE(
            compat::request(main, [&] { return main.updateRecordListPolicy("rows", metadata); })
                .error.code,
            QString("list-policy-metadata"));
        ConfigRecordListEditOptions edit;
        edit.generation = main.info({"rows"}).publicationGeneration;
        edit.edits = {{ConfigRecordListEdit::Op::Set, 0, "label", QString("bad")}};
        QCOMPARE(
            compat::request(main, [&] { return main.editRecordList("rows", edit); }).error.code,
            QString("string-pattern"));
        edit.generation = main.info({"rows"}).publicationGeneration;
        edit.edits = {
            {ConfigRecordListEdit::Op::Insert, 1, {}, QVariantMap{{"id", "y"}, {"label", "bad"}}}};
        QCOMPARE(
            compat::request(main, [&] { return main.editRecordList("rows", edit); }).error.code,
            QString("string-pattern"));
        ConfigOverlayOptions selection;
        selection.base = base.scope({"settings"}, false);
        selection.ownerPath = {"rows"};
        selection.ownerGeneration = main.info({"rows"}).publicationGeneration;
        selection.entryPath = {{ConfigValueSelector::Kind::Index, {}, 0}};
        selection.identityPath = {{ConfigValueSelector::Kind::Key, "id"}};
        selection.identityValue = QString("x");
        selection.overrideKey = "instanceSettings";
        ConfigOverlayEditor *editor = nullptr;
        connect(&main, &ConfigStore::overlayOpened, this,
                [&](quint64, ConfigOverlayEditor *opened) { editor = opened; });
        QCOMPARE(compat::request(main, [&] { return main.openOverlay(selection); }).status,
                 QString("opened"));
        QVERIFY(editor);
        QVERIFY(!editor->setValue({"word"}, QString("bad")).accepted);
        QVERIFY(!editor->dirty());
        QVERIFY(editor->setValue({"word"}, QString("UP")).accepted);
        QCOMPARE(compat::request(main, [&] { return editor->apply(); }).status, QString("saved"));
        QCOMPARE(editor->value({"word"}).toString(), QString("UP"));
        QCOMPARE(base.value({"settings", "word"}).toString(), QString("OK"));
        QCOMPARE(compat::request(main, [&] { return editor->resetOverrides(); }).status,
                 QString("saved"));
        QCOMPARE(editor->value({"word"}).toString(), QString("OK"));
    }
};
QTEST_GUILESS_MAIN(StringPatternTest)
#include "test_string_pattern.moc"
