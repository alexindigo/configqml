// SPDX-License-Identifier: LGPL-3.0-or-later
// Generated handle graph, uniform verbs and composed replies (addendum 02).
#include "compatfixture.h"
#include "confighandle.h"
#include "configdraft.h"
#include "configstorehandle.h"
#include "configpendingreply_p.h"
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace configqml;
using D = ConfigDescriptor;
using T = D::Type;

namespace {
std::unique_ptr<ConfigPendingReply> settle(ConfigPendingReply *reply) {
    std::unique_ptr<ConfigPendingReply> owned(reply);
    if (!reply)
        qFatal("null reply");
    QSignalSpy finished(reply, &ConfigPendingReply::finished);
    if (!reply->isFinished() && !finished.wait(10000))
        qFatal("reply timeout");
    return owned;
}
struct Fixture {
    QTemporaryDir directory;
    QString general, bar;
    ConfigStoreOptions options;
    std::unique_ptr<ConfigStore> store;
    std::shared_ptr<std::atomic_bool> fail = std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic_bool> holdWrite = std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<QSemaphore> writeHeld = std::make_shared<QSemaphore>();
    std::shared_ptr<QSemaphore> releaseWrite = std::make_shared<QSemaphore>();
    explicit Fixture(const QByteArray &generalBytes = "{\"scaleRatio\":2}",
                     const QByteArray &barBytes = "{\"position\":\"top\"}") {
        general = directory.filePath("general.json");
        bar = directory.filePath("bar.json");
        compat::put(general, generalBytes);
        compat::put(bar, barBytes);
        options.autoSave = false;
        auto optional = D::leaf(T::String, QVariant());
        optional.optional = true;
        optional.defaultValue.reset();
        options.schema = ConfigSchema(
            D::group({{"general", D::group({{"scaleRatio", D::leaf(T::Real, 1.0)},
                                            {"constructor", D::leaf(T::Boolean, false)},
                                            {"label", optional}})},
                      {"bar", D::group({{"position", D::leaf(T::String, "bottom")}})}}));
        options.layers = {{"user",
                           ConfigLayerKind::File,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"general", {"general"}, {}}, {"bar", {"bar"}, {}}},
                           true},
                          {"runtime",
                           ConfigLayerKind::Runtime,
                           ConfigAdmissionPolicy::FieldFallback,
                           {{"runtime", {}, {}}},
                           false}};
        options.jsonFiles = {{"user", "general", general}, {"user", "bar", bar}};
        options.fileFault = [fail = fail, hold = holdWrite, held = writeHeld,
                             release = releaseWrite](const QString &phase, const QString &) {
            if (phase == "write" && hold->exchange(false)) {
                held->release();
                release->acquire();
            }
            return fail->load() && phase == "write" ? QString("handle-write-fault") : QString();
        };
        store = std::make_unique<ConfigStore>(options);
    }
};
} // namespace

class GenericHandlesTest : public QObject {
    Q_OBJECT
private slots:
    void cachedLookupRespectsSelection_data() {
        QTest::addColumn<bool>("rootFirst");
        QTest::addColumn<bool>("unionLookup");
        QTest::newRow("scope-root-first") << true << false;
        QTest::newRow("scope-selected-first") << false << false;
        QTest::newRow("union-root-first") << true << true;
        QTest::newRow("union-selected-first") << false << true;
    }
    void cachedLookupRespectsSelection() {
        QFETCH(bool, rootFirst);
        QFETCH(bool, unionLookup);
        Fixture f;
        QTRY_COMPARE(f.store->status(), ConfigStore::Status::Ready);
        auto *root = f.store->root();
        auto *narrow = root->select({{"general", "scaleRatio"}});
        QVERIFY(narrow->valid());
        auto lookup = [&](ConfigHandle *parent) {
            return unionLookup ? parent->select({{"general"}}) : parent->scope({"general"});
        };
        ConfigHandle *whole, *selected;
        if (rootFirst) {
            whole = lookup(root);
            selected = lookup(narrow);
        } else {
            selected = lookup(narrow);
            whole = lookup(root);
        }
        QVERIFY(whole->valid());
        QVERIFY(selected->valid());
        QVERIFY(whole != selected);
        if (!unionLookup) {
            QCOMPARE(whole->fields(), (QStringList{"constructor", "label", "scaleRatio"}));
            QCOMPARE(selected->fields(), (QStringList{"scaleRatio"}));
            QVERIFY(!selected->scope({"constructor"})->valid());
            QVERIFY(whole->scope({"constructor"})->valid());
        } else {
            QVERIFY(!selected->scope({"general", "constructor"})->valid());
            QVERIFY(whole->scope({"general", "constructor"})->valid());
        }
        const ConfigPath outside =
            unionLookup ? ConfigPath{"general", "constructor"} : ConfigPath{"constructor"};
        auto refused = settle(selected->set(QList<ConfigHandleEdit>{{outside, true}}));
        QCOMPARE(refused->error().code(), QString("outside-selection"));
        QCOMPARE(f.store->value({"general", "constructor"}).toBool(), false);
        const ConfigPath inside =
            unionLookup ? ConfigPath{"general", "scaleRatio"} : ConfigPath{"scaleRatio"};
        QVERIFY(!settle(selected->set(QList<ConfigHandleEdit>{{inside, 7.0}}))->isError());
        QCOMPARE(f.store->value({"general", "scaleRatio"}).toDouble(), 7.0);
        QCOMPARE(f.store->value({"general", "constructor"}).toBool(), false);
    }
    void invalidLookupCannotPoisonCache_data() { cachedLookupRespectsSelection_data(); }
    void invalidLookupCannotPoisonCache() {
        QFETCH(bool, rootFirst);
        QFETCH(bool, unionLookup);
        Fixture f;
        QTRY_COMPARE(f.store->status(), ConfigStore::Status::Ready);
        auto *root = f.store->root();
        auto *narrow = root->select({{"general", "scaleRatio"}});
        auto lookup = [&](ConfigHandle *parent) {
            const ConfigPath path{"general", "constructor"};
            return unionLookup ? parent->select({path}) : parent->scope(path);
        };
        ConfigHandle *valid, *invalid;
        if (rootFirst) {
            valid = lookup(root);
            invalid = lookup(narrow);
        } else {
            invalid = lookup(narrow);
            valid = lookup(root);
        }
        QVERIFY(valid->valid());
        QVERIFY(!invalid->valid());
        QVERIFY(!invalid->scope({})->valid());
        QVERIFY(!invalid->select({{"general"}})->valid());
        QVERIFY(settle(invalid->remove())->isError());
        QVERIFY(!settle(root->scope({"general", "constructor"})->set(true))->isError());
        QCOMPARE(f.store->value({"general", "constructor"}).toBool(), true);
        QVERIFY(!invalid->valid());
        // Valid permutations share a canonical selected identity; invalid
        // duplicates are checked even after that identity was cached.
        auto *unionA = root->select({{"bar"}, {"general"}});
        QCOMPARE(unionA, root->select({{"general"}, {"bar"}}));
        QVERIFY(!root->select({{"general"}, {"general"}})->valid());
    }
    void cachedLookupPreservesOpaqueSegments_data() {
        QTest::addColumn<bool>("pathBoundary");
        QTest::addColumn<bool>("singleFirst");
        QTest::newRow("segments-single-first") << true << true;
        QTest::newRow("segments-nested-first") << true << false;
        QTest::newRow("selection-single-first") << false << true;
        QTest::newRow("selection-union-first") << false << false;
    }
    void cachedLookupPreservesOpaqueSegments() {
        QFETCH(bool, pathBoundary);
        QFETCH(bool, singleFirst);
        const QString opaque = QString("a") + QChar(0x1f) + "b";
        const QString joined = QString("a") + QChar(0x1e) + "b";
        ConfigStoreOptions options;
        options.autoSave = false;
        options.schema =
            ConfigSchema(D::group({{opaque, D::leaf(T::SafeInteger, 2)},
                                   {joined, D::leaf(T::SafeInteger, 4)},
                                   {"a", D::group({{"b", D::leaf(T::SafeInteger, 3)}})},
                                   {"b", D::leaf(T::SafeInteger, 5)}}));
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        auto *root = store.root();
        auto first = [&] {
            return pathBoundary ? root->scope({opaque}) : root->select({{joined}});
        };
        auto second = [&] {
            return pathBoundary ? root->scope({"a", "b"}) : root->select({{"a"}, {"b"}});
        };
        ConfigHandle *one, *two;
        if (singleFirst) {
            one = first();
            two = second();
        } else {
            two = second();
            one = first();
        }
        QVERIFY(one->valid());
        QVERIFY(two->valid());
        QVERIFY(one != two);
        if (pathBoundary) {
            QCOMPARE(one->value().toInt(), 2);
            QCOMPARE(two->value().toInt(), 3);
        } else {
            QVERIFY(!one->scope({"a", "b"})->valid());
            QVERIFY(!two->scope({joined})->valid());
            QCOMPARE(one->scope({joined})->value().toInt(), 4);
            QCOMPARE(two->scope({"a", "b"})->value().toInt(), 3);
        }
    }
    void generatedLookupAndBindings() {
        Fixture f;
        QTRY_COMPARE(f.store->status(), ConfigStore::Status::Ready);
        auto *root = f.store->root();
        QVERIFY(root && root->valid());
        QCOMPARE(root, f.store->root());
        QCOMPARE(root->kind(), ConfigHandle::Kind::Live);
        QCOMPARE(root->scopes(), (QStringList{"bar", "general"}));
        auto *general = root->scope({"general"});
        QCOMPARE(general, root->scope({"general"}));
        QCOMPARE(general->fields(), (QStringList{"constructor", "label", "scaleRatio"}));
        auto *scale = general->scope({"scaleRatio"});
        QVERIFY(scale->isField());
        QCOMPARE(scale, root->scope({"general", "scaleRatio"}));
        QCOMPARE(scale->value().toDouble(), 2.0);
        // Optional, defaultless leaves are generated before any value exists.
        auto *label = general->scope({"label"});
        QVERIFY(label->valid());
        QVERIFY(!label->present());
        // A prototype-like schema key is ordinary data, never a verb or prototype.
        QVERIFY(general->scope({"constructor"})->isField());
        QCOMPARE(general->scope({"constructor"})->value().toBool(), false);
        // Invalid selection never widens to root; its verbs reply with errors.
        auto *missing = root->scope({"nope"});
        QVERIFY(!missing->valid());
        QVERIFY(missing->capabilities().isEmpty());
        auto refused = settle(missing->read());
        QVERIFY(refused->isError());
        QCOMPARE(refused->error().phase(), QString("lifetime"));
        auto *record = root->scope({"general", "scaleRatio", "x"});
        QVERIFY(!record->valid());

        QSignalSpy scaleChanged(scale, &ConfigHandle::valueChanged);
        QSignalSpy generalChanged(general, &ConfigHandle::valueChanged);
        QSignalSpy labelChanged(label, &ConfigHandle::valueChanged);
        QVERIFY(f.store->setUser({"general", "scaleRatio"}, 3.0).accepted);
        QTRY_COMPARE(scale->value().toDouble(), 3.0);
        QCOMPARE(scaleChanged.count(), 1);
        QCOMPARE(generalChanged.count(), 1);
        QCOMPARE(labelChanged.count(), 0);
        // Equal intent and its worker confirmation are not value transitions.
        auto equal = settle(scale->set(3.0));
        QVERIFY2(!equal->isError(), qPrintable(equal->error().code()));
        QCOMPARE(scaleChanged.count(), 1);
        // Absent -> present -> absent, exactly once each.
        auto present = settle(label->set("hello"));
        QVERIFY2(!present->isError(), qPrintable(present->error().code()));
        QVERIFY(label->present());
        QCOMPARE(labelChanged.count(), 1);
        auto absent = settle(label->remove());
        QVERIFY2(!absent->isError(), qPrintable(absent->error().code()));
        QVERIFY(!label->present());
        QCOMPARE(labelChanged.count(), 2);
        QCOMPARE(scaleChanged.count(), 1);
    }
    void liveVerbsComposeReplies() {
        Fixture f;
        QTRY_COMPARE(f.store->status(), ConfigStore::Status::Ready);
        auto *root = f.store->root();
        auto *scale = root->scope({"general", "scaleRatio"});
        QVERIFY(root->capabilities().contains("edit"));
        QVERIFY(!root->capabilities().contains("discard"));
        auto write = settle(scale->set(4.0));
        QVERIFY2(!write->isError(), qPrintable(write->error().code()));
        QVERIFY(write->value().write());
        QVERIFY(write->value().generation().valid());
        QCOMPARE(write->value().snapshot().selectedState({}).value().toDouble(), 4.0);
        // Checked refusals admit nothing and change no file.
        auto wrongType = settle(scale->set(QStringLiteral("big")));
        QVERIFY(wrongType->isError());
        QCOMPARE(wrongType->error().phase(), QString("candidate"));
        auto general = root->scope({"general"});
        auto outside = settle(general->set(QList<ConfigHandleEdit>{{{"position"}, "left"}}));
        QCOMPARE(outside->error().code(), QString("outside-selection"));
        auto duplicate = settle(
            general->set(QList<ConfigHandleEdit>{{{"scaleRatio"}, 5.0}, {{"scaleRatio"}, 6.0}}));
        QCOMPARE(duplicate->error().code(), QString("duplicate-path"));
        QCOMPARE(scale->value().toDouble(), 4.0);
        auto discard = settle(scale->discard());
        QCOMPARE(discard->error().code(), QString("unsupported-operation"));
        // Selected persistence: bar only, even with pending general work.
        auto bar = root->scope({"bar"});
        auto moved = settle(bar->scope({"position"})->set("left"));
        QVERIFY(!moved->isError());
        const auto before = compat::bytes(f.general);
        auto saved = settle(bar->apply());
        QVERIFY2(!saved->isError(), qPrintable(saved->error().code()));
        QVERIFY(saved->value().write());
        QCOMPARE(compat::bytes(f.general), before);
        QVERIFY(compat::bytes(f.bar).contains("left"));
        // An unknown token from another store fails rather than succeeding.
        Fixture other;
        QTRY_COMPARE(other.store->status(), ConfigStore::Status::Ready);
        auto foreign = settle(other.store->root()->read());
        auto unrelated = settle(scale->observe(foreign->value().generation()));
        QVERIFY(unrelated->isError());
        // Observation-only barrier without a token.
        auto barrier = settle(root->whenCurrent());
        QVERIFY2(!barrier->isError(), qPrintable(barrier->error().code()));
        QCOMPARE(barrier->value().snapshot().selectedState({"general", "scaleRatio"}).value(),
                 QVariant(4.0));
    }
    void editResetDiscardApply() {
        Fixture f;
        QTRY_COMPARE(f.store->status(), ConfigStore::Status::Ready);
        auto *root = f.store->root();
        auto *general = root->scope({"general"});
        auto opened = settle(general->edit());
        QVERIFY2(!opened->isError(), qPrintable(opened->error().code()));
        std::unique_ptr<ConfigHandle> edit(opened->value().handle());
        QVERIFY(edit);
        edit->setParent(nullptr);
        QCOMPARE(edit->kind(), ConfigHandle::Kind::Staged);
        QVERIFY(edit->capabilities().contains("discard"));
        QVERIFY(!edit->capabilities().contains("edit"));
        auto *scale = edit->scope({"scaleRatio"});
        QCOMPARE(scale->value().toDouble(), 2.0);
        const auto original = compat::bytes(f.general);
        // Staged Reset shows the checked inherited candidate; nothing is written.
        QSignalSpy staged(scale, &ConfigHandle::valueChanged);
        auto reset = settle(scale->remove());
        QVERIFY2(!reset->isError(), qPrintable(reset->error().code()));
        QVERIFY(!reset->value().write());
        QTRY_COMPARE(scale->value().toDouble(), 1.0);
        QCOMPARE(scale->state().pending(), ConfigSelectedState::Pending::Remove);
        QVERIFY(scale->state().targetPresent());
        QCOMPARE(compat::bytes(f.general), original);
        QCOMPARE(root->scope({"general", "scaleRatio"})->value().toDouble(), 2.0);
        auto discarded = settle(edit->discard());
        QVERIFY2(!discarded->isError(), qPrintable(discarded->error().code()));
        QTRY_COMPARE(scale->value().toDouble(), 2.0);
        QCOMPARE(scale->state().pending(), ConfigSelectedState::Pending::None);
        QCOMPARE(compat::bytes(f.general), original);
        QVERIFY(staged.count() >= 2);
        // Apply composes writer admission/publication with the observed view.
        auto view = root->scope({"general"});
        auto set = settle(scale->set(7.0));
        QVERIFY(!set->isError());
        QCOMPARE(scale->value().toDouble(), 7.0);
        QCOMPARE(view->scope({"scaleRatio"})->value().toDouble(), 2.0);
        // A narrower reader cannot certify the selected write: refused before
        // anything is admitted, and the edit stays staged.
        ConfigHandleOptions narrow;
        narrow.observe = {view->scope({"scaleRatio"})};
        auto insufficient = settle(edit->apply(narrow));
        QCOMPARE(insufficient->error().code(), QString("read-observation"));
        QCOMPARE(insufficient->error().phase(), QString("selection"));
        QVERIFY(!insufficient->value().write());
        QCOMPARE(compat::bytes(f.general), original);
        QCOMPARE(scale->state().pending(), ConfigSelectedState::Pending::Set);
        ConfigHandleOptions observe;
        observe.observe = {view};
        auto applied = settle(edit->apply(observe));
        QVERIFY2(!applied->isError(), qPrintable(applied->error().code()));
        QVERIFY(applied->value().write());
        QCOMPARE(applied->value().observations().size(), 1);
        QCOMPARE(applied->value()
                     .observations()
                     .first()
                     .snapshot()
                     .selectedState({"scaleRatio"})
                     .value(),
                 QVariant(7.0));
        QCOMPARE(view->scope({"scaleRatio"})->value().toDouble(), 7.0);
        QVERIFY(compat::bytes(f.general).contains("7"));
        // Closing the editor does not invalidate committed coverage.
        const auto generation = applied->value().generation();
        edit.reset();
        auto later = settle(view->observe(generation));
        QVERIFY2(!later->isError(), qPrintable(later->error().code()));
    }
    void retainedApplyOutlivesHeldEditor_data() {
        QTest::addColumn<bool>("duringNotification");
        QTest::newRow("held-write") << false;
        QTest::newRow("completion-notify") << true;
    }
    void retainedApplyOutlivesHeldEditor() {
        QFETCH(bool, duringNotification);
        Fixture f;
        const auto unblock = qScopeGuard([&] { f.releaseWrite->release(); });
        QTRY_COMPARE(f.store->status(), ConfigStore::Status::Ready);
        auto *reader = f.store->root()->scope({"general", "scaleRatio"});
        auto opened = settle(reader->edit());
        QVERIFY(!opened->isError());
        std::unique_ptr<ConfigHandle> edit(opened->value().handle());
        QVERIFY(edit);
        edit->setParent(nullptr);
        auto staged = settle(edit->set(7.0));
        QVERIFY(!staged->isError());

        ConfigHandleOptions options;
        options.observe = {reader};
        f.holdWrite->store(true);
        std::unique_ptr<ConfigPendingReply> reply(edit->apply(options));
        QSignalSpy finished(reply.get(), &ConfigPendingReply::finished);
        QTRY_VERIFY(f.writeHeld->available() == 1);
        if (duringNotification) {
            auto *draft = edit->findChild<ConfigDraft *>();
            QVERIFY(draft);
            connect(draft, &ConfigDraft::stateChanged, this, [&] {
                auto closing = std::move(edit);
                closing.reset();
                QCoreApplication::sendPostedEvents(reply.get(), QEvent::MetaCall);
                QCOMPARE(finished.count(), 0);
            });
        } else {
            edit.reset();
            // Still behind the explicit write gate: no terminal result yet.
            QCoreApplication::sendPostedEvents(reply.get(), QEvent::MetaCall);
            QVERIFY(!reply->isFinished());
            QCOMPARE(finished.count(), 0);
        }
        f.releaseWrite->release();

        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(!edit);
        QVERIFY2(!reply->isError(), qPrintable(reply->error().code()));
        QVERIFY(reply->value().write());
        QCOMPARE(reply->value().write()->status, QString("saved"));
        QVERIFY(reply->value().generation().valid());
        QVERIFY(!reply->value().snapshot().valid());
        QCOMPARE(reply->value().observations().size(), 1);
        QCOMPARE(reply->value().observations().first().snapshot().selectedState({}).value(),
                 QVariant(7.0));
        QVERIFY(compat::bytes(f.general).contains("7"));
        auto observed = settle(reader->observe(reply->value().generation()));
        QVERIFY2(!observed->isError(), qPrintable(observed->error().code()));
        QCOMPARE(observed->value().snapshot().selectedState({}).value(), QVariant(7.0));
    }
    void selectedApplyLeavesDisjointEdits() {
        Fixture f;
        QTRY_COMPARE(f.store->status(), ConfigStore::Status::Ready);
        auto opened = settle(f.store->root()->edit());
        std::unique_ptr<ConfigHandle> edit(opened->value().handle());
        QVERIFY(edit);
        edit->setParent(nullptr);
        auto *scale = edit->scope({"general", "scaleRatio"});
        auto *position = edit->scope({"bar", "position"});
        QVERIFY(!settle(scale->set(5.0))->isError());
        QVERIFY(!settle(position->set("right"))->isError());
        const auto general = compat::bytes(f.general);
        auto applied = settle(edit->scope({"bar"})->apply());
        QVERIFY2(!applied->isError(), qPrintable(applied->error().code()));
        QVERIFY(compat::bytes(f.bar).contains("right"));
        QCOMPARE(compat::bytes(f.general), general);
        QCOMPARE(scale->state().pending(), ConfigSelectedState::Pending::Set);
        QCOMPARE(scale->value().toDouble(), 5.0);
        QCOMPARE(position->state().pending(), ConfigSelectedState::Pending::None);
        // Selected Discard drops only its own unadmitted edits.
        auto discarded = settle(edit->scope({"general"})->discard());
        QVERIFY(!discarded->isError());
        QCOMPARE(scale->state().pending(), ConfigSelectedState::Pending::None);
        QCOMPARE(scale->value().toDouble(), 2.0);
        QCOMPARE(compat::bytes(f.general), general);
    }
    void savedButUnobservedRecoversByObservation() {
        Fixture f;
        QTRY_COMPARE(f.store->status(), ConfigStore::Status::Ready);
        auto *root = f.store->root();
        auto *scale = root->scope({"general", "scaleRatio"});
        // The whole-store reader covers the write but its own preflight also reads
        // bar, whose acquisition fails independently of the selected writer.
        auto *reader = root;
        compat::put(f.bar, "{");
        ConfigHandleOptions observe;
        observe.observe = {reader};
        auto written = settle(scale->set(8.0));
        QVERIFY(!written->isError());
        auto applied = settle(root->scope({"general"})->apply(observe));
        QVERIFY(applied->isFinished());
        QVERIFY(applied->isError());
        QCOMPARE(applied->error().phase(), QString("observation"));
        QVERIFY(applied->value().write());
        QVERIFY(applied->value().write()->status != "failed");
        QVERIFY(applied->value().generation().valid());
        QVERIFY(compat::bytes(f.general).contains("8"));
        QCOMPARE(reader->health(), QString("failed"));
        compat::put(f.bar, "{\"position\":\"top\"}");
        auto recovered = settle(reader->observe(applied->value().generation()));
        QVERIFY2(!recovered->isError(), qPrintable(recovered->error().code()));
        QCOMPARE(reader->health(), QString("ok"));
    }
    void readOnlyAndFrozenCapabilities() {
        Fixture f;
        QTRY_COMPARE(f.store->status(), ConfigStore::Status::Ready);
        std::unique_ptr<ConfigStoreHandle> scope(f.store->scope({"general"}, false));
        auto *root = scope->root();
        QCOMPARE(root->kind(), ConfigHandle::Kind::ReadOnly);
        QCOMPARE(root->capabilities(), (QStringList{"read", "observe", "whenCurrent"}));
        auto refused = settle(root->scope({"scaleRatio"})->set(9.0));
        QCOMPARE(refused->error().code(), QString("unsupported-operation"));
        auto refusedEdit = settle(root->edit());
        QCOMPARE(refusedEdit->error().code(), QString("unsupported-operation"));
        QCOMPARE(root->scope({"scaleRatio"})->path(), (ConfigPath{"scaleRatio"}));
        auto read = settle(root->read());
        QVERIFY2(!read->isError(), qPrintable(read->error().code()));
        QCOMPARE(read->value().snapshot().selectedState({"scaleRatio"}).value(), QVariant(2.0));
        // A frozen edit never thaws: no fresh barrier, only covered tokens.
        auto opened = settle(f.store->root()->edit());
        std::unique_ptr<ConfigHandle> edit(opened->value().handle());
        edit->setParent(nullptr);
        auto barrier = settle(edit->observe());
        QCOMPARE(barrier->error().code(), QString("unsupported-operation"));
        auto frozen = settle(edit->read());
        QVERIFY2(!frozen->isError(), qPrintable(frozen->error().code()));
        auto *preview = edit->preview();
        QVERIFY(preview);
        QCOMPARE(preview->kind(), ConfigHandle::Kind::ReadOnly);
        QVERIFY(!settle(edit->scope({"general", "scaleRatio"})->set(6.0))->isError());
        QTRY_COMPARE(preview->scope({"general", "scaleRatio"})->value().toDouble(), 6.0);
        auto live = settle(preview->read());
        QVERIFY2(!live->isError(), qPrintable(live->error().code()));
    }
    void abandonFailedApply() {
        Fixture f;
        QTRY_COMPARE(f.store->status(), ConfigStore::Status::Ready);
        auto *view = f.store->root()->scope({"general"});
        auto opened = settle(view->edit());
        std::unique_ptr<ConfigHandle> edit(opened->value().handle());
        QVERIFY(edit);
        edit->setParent(nullptr);
        QVERIFY(edit->capabilities().contains("abandon"));
        QVERIFY(!view->capabilities().contains("abandon"));
        QVERIFY(!settle(edit->scope({"scaleRatio"})->set(6.0))->isError());
        const auto original = compat::bytes(f.general);
        f.fail->store(true);
        auto failed = settle(edit->apply());
        QVERIFY(failed->isError());
        QCOMPARE(failed->error().phase(), QString("publication"));
        QVERIFY(failed->value().write());
        QCOMPARE(view->scope({"scaleRatio"})->value().toDouble(), 6.0); // Kept for Retry.
        QSignalSpy changed(view->scope({"scaleRatio"}), &ConfigHandle::valueChanged);
        ConfigHandleOptions observe;
        observe.observe = {view};
        auto abandoned = settle(edit->abandon(observe));
        QVERIFY2(!abandoned->isError(), qPrintable(abandoned->error().code()));
        QCOMPARE(abandoned->value().write()->status, QString("abandoned"));
        QCOMPARE(abandoned->value()
                     .observations()
                     .first()
                     .snapshot()
                     .selectedState({"scaleRatio"})
                     .value(),
                 QVariant(2.0));
        QCOMPARE(view->scope({"scaleRatio"})->value().toDouble(), 2.0);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(compat::bytes(f.general), original);
        QCOMPARE(edit->scope({"scaleRatio"})->state().pending(), ConfigSelectedState::Pending::Set);
        auto again = settle(edit->abandon());
        QCOMPARE(again->error().code(), QString("apply-not-pending"));
        QCOMPARE(again->error().phase(), QString("admission"));
        auto unsupported = settle(view->abandon());
        QCOMPARE(unsupported->error().code(), QString("unsupported-operation"));
    }
    void runtimeTargetHasNoDiskRoute() {
        Fixture f;
        QTRY_COMPARE(f.store->status(), ConfigStore::Status::Ready);
        auto *runtime = f.store->runtimeRoot();
        QVERIFY(runtime != f.store->root());
        QVERIFY(!runtime->capabilities().contains("edit"));
        const auto before = compat::bytes(f.general);
        auto set = settle(runtime->scope({"general", "scaleRatio"})->set(11.0));
        QVERIFY2(!set->isError(), qPrintable(set->error().code()));
        QCOMPARE(f.store->root()->scope({"general", "scaleRatio"})->value().toDouble(), 11.0);
        auto applied = settle(runtime->apply());
        QVERIFY2(!applied->isError(), qPrintable(applied->error().code()));
        QVERIFY(!applied->value().write());
        QCOMPARE(compat::bytes(f.general), before);
        auto cleared = settle(runtime->scope({"general", "scaleRatio"})->remove());
        QVERIFY(!cleared->isError());
        QCOMPARE(f.store->root()->scope({"general", "scaleRatio"})->value().toDouble(), 2.0);
    }
};

QTEST_MAIN(GenericHandlesTest)
#include "test_generic_primitives_handles.moc"
