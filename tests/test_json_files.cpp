// SPDX-License-Identifier: LGPL-3.0-or-later
#include "fileoptions.h"
#include "storeprobe.h"
#include "modelbudgets.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <sys/stat.h>
#include <unistd.h>
#include <QJsonDocument>
#include <QJsonObject>
using namespace configqml;
using Probe = ConfigStoreTestAccess;
using D = ConfigDescriptor;
using T = D::Type;
static bool writeFile(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
static QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
class JsonFilesTest : public QObject {
    Q_OBJECT
private slots:
    void borrowedBackendKeys() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto canonical = dir.filePath("config.json");
        QVERIFY(writeFile(canonical, "{}"));
        QString mutablePath = canonical;
        auto *pathData = mutablePath.data();
        QChar id[] = {QChar('f'), QChar('i'), QChar('l'), QChar('e')};
        ConfigJsonBackend backend({{"user", QString::fromRawData(id, 4),
                                    QString::fromRawData(pathData, mutablePath.size())}});
        id[0] = QChar('m');
        pathData[1] = QChar('x');
        QCOMPARE(backend.path("file"), canonical);
        id[0] = QChar('f');
        ConfigBackendRequest request;
        request.sourceId = QString::fromRawData(id, 4);
        auto sequence = backend.submit(request);
        id[0] = QChar('m');
        auto result = backend.take(sequence);
        QVERIFY(result && result->success);
        QCOMPARE(result->sourceId, QString("file"));
        QCOMPARE(backend.reads("file"), quint64(1));
        QCOMPARE(backend.reads("mile"), quint64(0));
        id[0] = QChar('f');
        backend.confirm(QString::fromRawData(id, 4), result->revision);
        id[0] = QChar('m');
        QVERIFY(!backend.changed("file"));
    }
    void codec() {
        ConfigJsonCodec codec;
        auto input =
            QByteArray("{\"z\":18446744073709551615,\"a\":{\"y\":1.0000000000000001,\"x\":null}}");
        auto decoded = codec.decode(input, "file");
        QVERIFY(decoded.ok());
        QCOMPARE(codec.encode(decoded.raw, "file").bytes.trimmed(), input);
        decoded.raw["new"] = qint64(-9223372036854775807LL - 1);
        const auto encoded = codec.encode(decoded.raw, "file");
        QVERIFY(encoded.ok());
        QVERIFY(encoded.bytes.contains("18446744073709551615"));
        QVERIFY(encoded.bytes.contains("1.0000000000000001"));
        QVERIFY(encoded.bytes.contains("-9223372036854775808"));
        auto bad = codec.decode("{\n\"a\": [1, }", "file");
        QVERIFY(!bad.ok());
        QCOMPARE(bad.error.line, 2);
        QCOMPARE(bad.error.path, (ConfigPath{"a", "1"}));
        QVERIFY(!codec.decode("{\"x\":1,\"x\":2}", "file").ok());
        QVERIFY(!codec.decode("[]", "file").ok());
    }
    void wideCodecOrdering() {
        constexpr int count = 50000;
        QVariantMap raw;
        QByteArray reverse = "{";
        for (int i = count - 1; i >= 0; --i) {
            const auto key = QString("key-%1").arg(i, 5, 10, QChar('0'));
            raw.insert(key, true);
            if (i != count - 1)
                reverse += ',';
            reverse += encodeJsonValue(key).bytes + ":true";
        }
        reverse += '}';
        ConfigJsonCodec fresh;
        ConfigJsonCodec preserved;
        const auto decoded = preserved.decode(reverse, "wide");
        QVERIFY(decoded.ok());
        QElapsedTimer timer;
        timer.start();
        const auto sorted = fresh.encode(raw, "wide");
        const auto retained = preserved.encode(decoded.raw, "wide");
        const auto elapsed = timer.elapsed();
        QVERIFY(sorted.ok());
        QVERIFY(retained.ok());
        QCOMPARE(sorted.bytes, encodeJsonValue(raw).bytes + '\n');
        QCOMPARE(retained.bytes, reverse + '\n');
        qInfo("50000-key fresh and preserved encoding: %lldms enforce=%d", elapsed,
              enforceModelBudgets);
        if (enforceModelBudgets)
            QVERIFY2(elapsed < 2000, "Wide object ordering must not scan the key list per member");
        // Removed members disappear; new keys append after surviving original keys.
        auto changed = decoded.raw;
        changed.remove("key-49999");
        changed.insert("appended", false);
        const auto edited = preserved.encode(changed, "wide");
        QVERIFY(edited.ok());
        QVERIFY(edited.bytes.startsWith("{\"key-49998\":true,"));
        QVERIFY(edited.bytes.endsWith(",\"appended\":false}\n"));
    }
    void restartUnknownMaskedAndExact() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("config.json");
        QVERIFY(writeFile(path, "{\"unknown\":{\"wide\":1.0000000000000001},\"a\":4}"));
        auto o = fileOptions(path);
        o.layers.append({"runtime",
                         ConfigLayerKind::Runtime,
                         ConfigAdmissionPolicy::FieldFallback,
                         {{"runtime", {}, {{"a", 9}}}},
                         false});
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy done(&store, &ConfigStore::completed);
        store.setUser({"a"}, 0);
        store.setUser({"wide"}, std::numeric_limits<qint64>::min());
        store.setUser({"unsigned"}, std::numeric_limits<quint64>::max());
        store.setUser({"url"}, QUrl("https://example.test/a%2Fb"));
        store.setUser({"nullable"}, QVariant::fromValue(nullptr));
        store.setUser({"items"}, QVariantList{});
        store.close();
        QTRY_COMPARE(store.status(), ConfigStore::Status::Closed);
        QCOMPARE(done.size(), 7);
        const auto result = qvariant_cast<ConfigResult>(done.last()[0]);
        QCOMPARE(result.status, QString("saved"));
        QCOMPARE(result.inputPrefix, quint64(6));
        QCOMPARE(store.value({"a"}).toLongLong(), 9);
        QVERIFY(readFile(path).contains("1.0000000000000001"));
        ConfigStore reopened(fileOptions(path));
        QTRY_COMPARE(reopened.status(), ConfigStore::Status::Ready);
        QCOMPARE(reopened.value({"a"}).toLongLong(), 0);
        QCOMPARE(reopened.value({"wide"}).toLongLong(), std::numeric_limits<qint64>::min());
        QCOMPARE(reopened.value({"unsigned"}).toULongLong(), std::numeric_limits<quint64>::max());
        QCOMPARE(reopened.value({"url"}).toUrl(), QUrl("https://example.test/a%2Fb"));
        QVERIFY(isNull(reopened.value({"nullable"})));
        QCOMPARE(reopened.value({"items"}).toList().size(), 0);
    }
    void watchReplaceDeleteRecreateAndParse() {
        QTemporaryDir dir;
        const auto path = dir.filePath("config.json");
        QVERIFY(writeFile(path, "{\"a\":1}"));
        ConfigStore store(fileOptions(path, ConfigWatchMode::Live));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(store.value({"a"}).toLongLong(), 1);
        QSignalSpy errors(&store, &ConfigStore::sourceError);
        QVERIFY(writeFile(path, "{\n\"a\": }"));
        QTRY_COMPARE(errors.size(), 1);
        const auto error = qvariant_cast<ConfigError>(errors[0][0]);
        QCOMPARE(error.file, path);
        QCOMPARE(error.line, 2);
        QCOMPARE(store.value({"a"}).toLongLong(), 1);
        QVERIFY(writeFile(path, "{\"a\":2,\"b\":3}"));
        bool coherent = false;
        connect(&store, &ConfigStore::valuesChanged, this, [&](const auto &) {
            if (store.value({"a"}).toLongLong() == 2)
                coherent = store.value({"b"}).toLongLong() == 3;
        });
        QTRY_COMPARE(store.value({"a"}).toLongLong(), 2);
        QVERIFY(coherent);
        QVERIFY(QFile::remove(path));
        QTRY_COMPARE(store.value({"a"}).toLongLong(), 0);
        QVERIFY(writeFile(path, "{\"a\":7}"));
        QTRY_COMPARE(store.value({"a"}).toLongLong(), 7);
        QCOMPARE(store.inputs().size(), 0);
    }
    void modesAndFreshConditional() {
        QTemporaryDir dir;
        const auto path = dir.filePath("config.json");
        QVERIFY(writeFile(path, "{\"a\":1}"));
        ConfigStore off(fileOptions(path, ConfigWatchMode::Off));
        ConfigStore snapshot(fileOptions(path));
        QTRY_COMPARE(off.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(snapshot.status(), ConfigStore::Status::Ready);
        QCOMPARE(off.value({"a"}).toLongLong(), 0);
        QCOMPARE(snapshot.value({"a"}).toLongLong(), 1);
        QVERIFY(writeFile(path, "{\"a\":2}"));
        QTest::qWait(60);
        QCOMPARE(snapshot.value({"a"}).toLongLong(), 1);
        off.reload();
        QTRY_COMPARE(off.value({"a"}).toLongLong(), 2);
        // Raw-invalid is present: initializing must not replace it, even if it
        // arrived after this writer's startup snapshot.
        QVERIFY(writeFile(path, "{\"a\":\"invalid\",\"b\":null}"));
        snapshot.initializeIfMissing({"a"}, 8);
        snapshot.initializeIfMissing({"b"}, 9);
        snapshot.initializeIfMissing({"wide"}, qint64(42));
        QSignalSpy done(&snapshot, &ConfigStore::completed);
        snapshot.close();
        QTRY_COMPARE(snapshot.status(), ConfigStore::Status::Closed);
        QCOMPARE(qvariant_cast<ConfigResult>(done.last()[0]).status, QString("saved"));
        ConfigJsonCodec codec;
        const auto raw = codec.decode(readFile(path), "json").raw;
        QCOMPARE(raw["a"].toString(), QString("invalid"));
        QVERIFY(isNull(raw["b"]));
        QCOMPARE(raw["wide"].value<ConfigNumber>().integer(false).value.toLongLong(), 42);
    }
    void disjointMergeConflictAndRetainedIntent() {
        QTemporaryDir dir;
        const auto path = dir.filePath("config.json");
        QVERIFY(writeFile(path, "{\"a\":1,\"b\":1}"));
        ConfigStore first(fileOptions(path)), second(fileOptions(path));
        QTRY_COMPARE(first.status(), ConfigStore::Status::Ready);
        QTRY_COMPARE(second.status(), ConfigStore::Status::Ready);
        first.setUser({"a"}, 2);
        first.close();
        QTRY_COMPARE(first.status(), ConfigStore::Status::Closed);
        second.setUser({"b"}, 3);
        QSignalSpy done(&second, &ConfigStore::completed);
        second.save();
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(qvariant_cast<ConfigResult>(done.last()[0]).status, QString("saved"));
        QCOMPARE(second.value({"a"}).toLongLong(), 2);
        second.setUser({"a"}, 5);
        QVERIFY(writeFile(path, "{\"a\":4,\"b\":3}"));
        second.save();
        QTRY_COMPARE(done.size(), 4);
        const auto failed = qvariant_cast<ConfigResult>(done.last()[0]);
        QCOMPARE(failed.status, QString("failed"));
        QCOMPARE(failed.error.code, QString("path-conflict"));
        QCOMPARE(failed.error.file, path);
        QCOMPARE(second.value({"a"}).toLongLong(), 5);
        second.flush();
        QTRY_COMPARE(done.size(), 5);
        QCOMPARE(qvariant_cast<ConfigResult>(done.last()[0]).status, QString("failed"));
        QVERIFY(readFile(path).contains("\"a\":4"));
    }
    void faultPhases_data() {
        QTest::addColumn<QString>("phase");
        for (auto phase : {"write", "rename", "readback"})
            QTest::newRow(phase) << QString(phase);
    }
    void faultPhases() {
        QFETCH(QString, phase);
        QTemporaryDir dir;
        const auto path = dir.filePath("config.json");
        const QByteArray original = "{\"a\":1}";
        QVERIFY(writeFile(path, original));
        auto o = fileOptions(path);
        o.fileFault = [phase](const auto &at, const auto &) {
            return at == phase ? QString("injected-") + phase : QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy done(&store, &ConfigStore::completed);
        store.setUser({"a"}, 2);
        store.save();
        QTRY_COMPARE(done.size(), 2);
        const auto result = qvariant_cast<ConfigResult>(done.last()[0]);
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.operation, phase);
        QCOMPARE(result.error.file, path);
        QCOMPARE(result.publicationState,
                 phase == "readback" ? QString("uncertain") : QString("known"));
        QCOMPARE(store.value({"a"}).toLongLong(), 2);
        if (phase != "readback")
            QCOMPARE(readFile(path), original);
        else
            QVERIFY(readFile(path).contains("\"a\":2"));
        QCOMPARE(
            QDir(dir.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size(),
            2);
    }
    void noncooperativeInterleavings() {
        QTemporaryDir dir;
        const auto path = dir.filePath("config.json");
        QVERIFY(writeFile(path, "{\"a\":1}"));
        auto o = fileOptions(path, ConfigWatchMode::Live);
        o.fileFault = [](const auto &phase, const auto &file) {
            if (phase == "readback")
                writeFile(file, "{\"a\":8}");
            return QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy done(&store, &ConfigStore::completed);
        store.setUser({"a"}, 2);
        store.save();
        QTRY_COMPARE(done.size(), 2);
        auto result = qvariant_cast<ConfigResult>(done.last()[0]);
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.publicationState, QString("uncertain"));
        QVERIFY(readFile(path).contains("\"a\":8"));
        // A subsequent foreign edit is never suppressed by a time window.
        QVERIFY(writeFile(path, "{\"a\":9,\"b\":7}"));
        QTRY_COMPARE(store.value({"b"}).toLongLong(), 7);
        QCOMPARE(store.value({"a"}).toLongLong(), 2); // Pending intent retained.
    }
    void twoProcesses_data() {
        QTest::addColumn<bool>("overlap");
        QTest::addColumn<bool>("heldTeardown");
        QTest::newRow("disjoint") << false << false;
        QTest::newRow("overlapping") << true << false;
        QTest::newRow("overlapping-held-teardown") << true << true;
    }
    void twoProcesses() {
        QFETCH(bool, overlap);
        QFETCH(bool, heldTeardown);
        QTemporaryDir dir;
        const auto path = dir.filePath("config.json");
        QVERIFY(writeFile(path, "{}"));
        QProcess a, b;
        const auto program = QCoreApplication::applicationDirPath() + "/file_writer";
        a.start(program, {path, "a", "1", heldTeardown ? "manual-held-teardown" : "manual"});
        b.start(program, {path, overlap ? "a" : "b", "2", "manual"});
        QVERIFY(a.waitForReadyRead(5000));
        QVERIFY(b.waitForReadyRead(5000));
        QVERIFY(a.readAllStandardOutput().contains("READY"));
        QVERIFY(b.readAllStandardOutput().contains("READY"));
        a.write("GO\n");
        if (heldTeardown)
            QVERIFY(a.waitForFinished(7000));
        b.write("GO\n");
        if (!heldTeardown)
            QVERIFY(a.waitForFinished(7000));
        QVERIFY(b.waitForFinished(7000));
        const auto aOut = a.readAllStandardOutput(), bOut = b.readAllStandardOutput();
        const auto aErr = a.readAllStandardError(), bErr = b.readAllStandardError();
        qInfo("writer A exitStatus=%d exitCode=%d stdout=%s stderr=%s", int(a.exitStatus()),
              a.exitCode(), aOut.constData(), aErr.constData());
        qInfo("writer B exitStatus=%d exitCode=%d stdout=%s stderr=%s", int(b.exitStatus()),
              b.exitCode(), bOut.constData(), bErr.constData());
        qInfo("final file=%s", readFile(path).constData());
        QCOMPARE(a.exitStatus(), QProcess::NormalExit);
        QCOMPARE(b.exitStatus(), QProcess::NormalExit);
        QVERIFY(aErr.isEmpty());
        QVERIFY(bErr.isEmpty());
        const auto aReceipt = QJsonDocument::fromJson(aOut.left(aOut.indexOf('\n'))).object();
        const auto bReceipt = QJsonDocument::fromJson(bOut.left(bOut.indexOf('\n'))).object();
        if (overlap) {
            QCOMPARE(a.exitCode() + b.exitCode(), 3);
            QVERIFY((a.exitCode() == 0 && b.exitCode() == 3) ||
                    (a.exitCode() == 3 && b.exitCode() == 0));
            QCOMPARE((a.exitCode() == 3 ? aReceipt : bReceipt).value("error").toString(),
                     QString("path-conflict"));
            QCOMPARE((a.exitCode() == 0 ? aReceipt : bReceipt).value("status").toString(),
                     QString("saved"));
            ConfigStore reopened(fileOptions(path));
            QTRY_COMPARE(reopened.status(), ConfigStore::Status::Ready);
            QCOMPARE(reopened.value({"a"}).toLongLong(), a.exitCode() == 0 ? 1 : 2);
        } else {
            QCOMPARE(a.exitCode(), 0);
            QCOMPARE(b.exitCode(), 0);
            ConfigStore reopened(fileOptions(path));
            QTRY_COMPARE(reopened.status(), ConfigStore::Status::Ready);
            QCOMPARE(reopened.value({"a"}).toLongLong(), 1);
            QCOMPARE(reopened.value({"b"}).toLongLong(), 2);
        }
    }
    void autoSaveAndQuit() {
        QTemporaryDir dir;
        const auto path = dir.filePath("config.json");
        auto o = fileOptions(path);
        o.autoSave = true;
        o.autoSaveInterval = 40;
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy autoSaved(&store, &ConfigStore::autoSaved);
        for (int i = 1; i <= 10; ++i)
            store.setUser({"a"}, i);
        QCOMPARE(store.value({"a"}).toLongLong(), 10);
        QTRY_COMPARE(autoSaved.size(), 1);
        const auto result = qvariant_cast<ConfigResult>(autoSaved[0][0]);
        QCOMPARE(result.status, QString("saved"));
        QCOMPARE(result.inputPrefix, quint64(10));
        ConfigStore reopened(fileOptions(path));
        QTRY_COMPARE(reopened.status(), ConfigStore::Status::Ready);
        QCOMPARE(reopened.value({"a"}).toLongLong(), 10);
        const auto quitPath = dir.filePath("quit.json");
        QProcess child;
        child.start(QCoreApplication::applicationDirPath() + "/file_writer",
                    {quitPath, "b", "77", "quit"});
        QVERIFY(child.waitForFinished(7000));
        QCOMPARE(child.exitCode(), 0);
        ConfigStore quit(fileOptions(quitPath));
        QTRY_COMPARE(quit.status(), ConfigStore::Status::Ready);
        QCOMPARE(quit.value({"b"}).toLongLong(), 77);
    }
    void shardsNoEchoFanoutAndGuiBurst() {
        QTemporaryDir dir;
        ConfigStoreOptions o;
        QMap<QString, D> sections;
        ConfigLayer layer{
            "user", ConfigLayerKind::File, ConfigAdmissionPolicy::FieldFallback, {}, true};
        for (int i = 0; i < 29; ++i) {
            const auto key = QString("s%1").arg(i);
            sections[key] = D::group({{"a", D::leaf(T::SafeInteger, 0)}});
            layer.sources.append({key, {key}, {}});
            o.jsonFiles.append(
                {"user", key, dir.filePath(key + ".json"), true, ConfigWatchMode::Live});
            QVERIFY(writeFile(dir.filePath(key + ".json"), "{\"a\":0}"));
        }
        o.schema = ConfigSchema(D::group(sections));
        o.layers = {layer};
        o.autoSave = false;
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy done(&store, &ConfigStore::completed);
        QElapsedTimer timer;
        qint64 worst = 0;
        for (int i = 1; i <= 100; ++i) {
            timer.start();
            store.setUser({"s0", "a"}, i);
            store.save();
            worst = qMax(worst, timer.nsecsElapsed());
        }
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 200, 15000);
        QTest::qWait(100); // Deliver actual file/directory hints.
        bool observed = false;
        quint64 targetReads = 0, otherReads = 0;
        Probe::run(
            store,
            [&](auto &w) {
                targetReads = w.jsonBackend()->reads("s0");
                for (int i = 1; i < 29; ++i)
                    otherReads += w.jsonBackend()->reads(QString("s%1").arg(i));
            },
            this, [&] { observed = true; });
        QTRY_VERIFY(observed);
        QCOMPARE(otherReads, quint64(28));   // Exactly one startup read per unchanged section.
        QCOMPARE(targetReads, quint64(301)); // Startup + 100 fresh + 100 preflight + 100 readback.
        QCOMPARE(store.inputs().size(), 100);
        for (int i = 1; i < 29; ++i)
            QCOMPARE(readFile(dir.filePath(QString("s%1.json").arg(i))), QByteArray("{\"a\":0}"));
        const auto publication = Probe::worstPublicationNanos(store);
        qInfo("JSON burst GUI ingress+save max us %.3f; publication max us %.3f enforce=%d",
              worst / 1000.0, publication / 1000.0, enforceModelBudgets);
        if (enforceModelBudgets) {
            QVERIFY(worst < 16000000);
            QVERIFY(publication < 16000000);
        }
        // External reload reaches the consumer and leaves all other sections alone.
        QVERIFY(writeFile(dir.filePath("s1.json"), "{\"a\":42}"));
        QTRY_COMPARE(store.value({"s1", "a"}).toLongLong(), 42);
        QCOMPARE(store.value({"s0", "a"}).toLongLong(), 100);
    }
    void fileIdentityAndReadOnly() {
        QTemporaryDir dir;
        const auto target = dir.filePath("target.json"), link = dir.filePath("link.json");
        QVERIFY(writeFile(target, "{\"a\":4}"));
        QVERIFY(::symlink(QFile::encodeName(target).constData(),
                          QFile::encodeName(link).constData()) == 0);
        auto o = fileOptions(target);
        o.layers[0].writable = false;
        o.autoSave = true;
        ConfigStore readonly(o);
        QTRY_COMPARE(readonly.status(), ConfigStore::Status::Ready);
        QVERIFY(!readonly.setUser({"a"}, 7).accepted);
        readonly.close();
        QTRY_COMPARE(readonly.status(), ConfigStore::Status::Closed);
        QCOMPARE(readFile(target), QByteArray("{\"a\":4}"));
        QVERIFY(!QFileInfo::exists(target + ".lock"));
    }
    void symlinkedPaths_data() {
        QTest::addColumn<bool>("directoryLink");
        QTest::newRow("file") << false;
        QTest::newRow("parent-directory") << true;
    }
    void symlinkedPaths() {
        QFETCH(bool, directoryLink);
        QTemporaryDir dir;
        const auto one = dir.filePath("one"), two = dir.filePath("two");
        QVERIFY(QDir().mkpath(one));
        QVERIFY(QDir().mkpath(two));
        const auto first = one + "/config.json", second = two + "/config.json";
        QVERIFY(writeFile(first, "{\"a\":1}"));
        QVERIFY(writeFile(second, "{\"a\":7}"));
        const auto link = dir.filePath("configured");
        auto makeLink = [&](const QString &destination, const QString &at) {
            return ::symlink(QFile::encodeName(destination).constData(),
                             QFile::encodeName(at).constData()) == 0;
        };
        QVERIFY(makeLink(directoryLink ? "one" : "one/config.json", link));
        const auto configured = directoryLink ? link + "/config.json" : link;
        auto options = fileOptions(configured, ConfigWatchMode::Live);
        QString published;
        options.fileFault = [&](const auto &phase, const auto &target) {
            if (phase == "write")
                published = target;
            return QString();
        };
        ConfigStore store(options);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(store.value({"a"}).toLongLong(), 1);
        QSignalSpy done(&store, &ConfigStore::completed);
        store.setUser({"a"}, 2);
        store.save();
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(qvariant_cast<ConfigResult>(done.last()[0]).status, QString("saved"));
        QCOMPARE(published, first);
        QVERIFY(QFileInfo(link).isSymLink());
        QVERIFY(readFile(first).contains("\"a\":2"));
        QVERIFY(QFileInfo::exists(first + ".lock"));
        QVERIFY(!QFileInfo::exists(dir.filePath("configured.lock")));
        QTest::qWait(100);
        bool observed = false;
        quint64 reads = 0;
        Probe::run(
            store, [&](auto &w) { reads = w.jsonBackend()->reads("json"); }, this,
            [&] { observed = true; });
        QTRY_VERIFY(observed);
        QCOMPARE(reads, quint64(4)); // No own-write echo read through either alias form.
        QVERIFY(writeFile(first, "{\"a\":4}"));
        QTRY_COMPARE(store.value({"a"}).toLongLong(), 4);
        const auto replacement = dir.filePath("replacement");
        QVERIFY(makeLink(directoryLink ? "two" : "two/config.json", replacement));
        QVERIFY(::rename(QFile::encodeName(replacement).constData(),
                         QFile::encodeName(link).constData()) == 0);
        QTRY_COMPARE(store.value({"a"}).toLongLong(), 7);
        QVERIFY(writeFile(first, "{\"a\":8}"));
        QVERIFY(writeFile(second, "{\"a\":9}"));
        QTRY_COMPARE(store.value({"a"}).toLongLong(), 9);
        store.setUser({"a"}, 10);
        store.close();
        QTRY_COMPARE(store.status(), ConfigStore::Status::Closed);
        QCOMPARE(published, second);
        QVERIFY(QFileInfo(link).isSymLink());
        QCOMPARE(readFile(first), QByteArray("{\"a\":8}"));
        QVERIFY(readFile(second).contains("\"a\":10"));
    }
    void danglingSymlinkAndDuplicateTargets() {
        QTemporaryDir dir;
        const auto link = dir.filePath("link.json"), target = dir.filePath("new/deep/config.json");
        QVERIFY(::symlink("new/deep/config.json", QFile::encodeName(link).constData()) == 0);
        ConfigStore store(fileOptions(link));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        store.setUser({"a"}, 11);
        store.close();
        QTRY_COMPARE(store.status(), ConfigStore::Status::Closed);
        QVERIFY(QFileInfo(link).isSymLink());
        QVERIFY(readFile(target).contains("\"a\":11"));
        auto options = fileOptions(link);
        options.layers.prepend({"system",
                                ConfigLayerKind::File,
                                ConfigAdmissionPolicy::FieldFallback,
                                {{"alias", {}, {}}},
                                false});
        options.jsonFiles.prepend({"system", "alias", target});
        ConfigStore duplicate(options);
        QTRY_COMPARE(duplicate.status(), ConfigStore::Status::Error);
        QCOMPARE(duplicate.error(), QString("duplicate-file-identity"));
    }
    void hardLinkRefusesPublication() {
        QTemporaryDir dir;
        const auto target = dir.filePath("target.json"), alias = dir.filePath("hard.json");
        QVERIFY(writeFile(target, "{\"a\":4}"));
        QVERIFY(::link(QFile::encodeName(target).constData(),
                       QFile::encodeName(alias).constData()) == 0);
        ConfigStore store(fileOptions(alias));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        store.setUser({"a"}, 12);
        QSignalSpy done(&store, &ConfigStore::completed);
        store.close();
        QTRY_COMPARE(store.status(), ConfigStore::Status::Closed);
        const auto result = qvariant_cast<ConfigResult>(done.last()[0]);
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.error.code, QString("hard-linked-target"));
        QCOMPARE(result.error.file, alias);
        QCOMPARE(readFile(target), QByteArray("{\"a\":4}"));
        QCOMPARE(readFile(alias), QByteArray("{\"a\":4}"));
    }
    void shutdownSettlement_data() {
        QTest::addColumn<QString>("mode");
        for (const auto *mode :
             {"ready", "closing", "closed", "closing-manual", "closing-fault", "destroy-closing"})
            QTest::newRow(mode) << QString("shutdown-") + mode;
    }
    void processOwnedPersistence_data() {
        QTest::addColumn<QString>("mode");
        for (const auto *mode :
             {"drain-delete", "delete-autosave", "return-main", "return-live", "multiple",
              "delete-manual-save", "delete-manual-flush", "delete-manual-close"})
            QTest::newRow(mode) << QString("lifetime-") + mode;
    }
    void processShutdownTimeout() {
        QTemporaryDir dir;
        QProcess child;
        QElapsedTimer timer;
        timer.start();
        child.start(QCoreApplication::applicationDirPath() + "/file_writer",
                    {dir.filePath("unused.json"), "a", "77", "unsettled"});
        QVERIFY(child.waitForFinished(7000));
        const auto error = child.readAllStandardError();
        qInfo("unsettled exitStatus=%d exitCode=%d elapsedMs=%lld stderr=%s",
              int(child.exitStatus()), child.exitCode(), static_cast<long long>(timer.elapsed()),
              error.constData());
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
        QCOMPARE(error.count("shutdown-timeout"), 1);
        QVERIFY(error.contains("store=unsettled-test prefix=1"));
        QVERIFY(timer.elapsed() >= 4900);
        QVERIFY(timer.elapsed() <
                7000); // The post-routine must not start a second five-second wait.
        QVERIFY(!error.contains("Sanitizer"));
    }
    void processOwnedPersistence() {
        QFETCH(QString, mode);
        QTemporaryDir dir;
        const auto path = dir.filePath("config.json");
        QVERIFY(writeFile(path, "{}"));
        QProcess child;
        child.start(QCoreApplication::applicationDirPath() + "/file_writer",
                    {path, "a", "77", mode});
        QVERIFY(child.waitForFinished(7000));
        const auto out = child.readAllStandardOutput(), err = child.readAllStandardError();
        qInfo("owned lifetime mode=%s exitStatus=%d exitCode=%d stdout=%s stderr=%s",
              qPrintable(mode), int(child.exitStatus()), child.exitCode(), out.constData(),
              err.constData());
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
        QVERIFY(err.isEmpty());
        if (mode == "lifetime-drain-delete")
            QVERIFY(out.contains("DELETE_IN_DRAIN status=3"));
        if (mode.startsWith("lifetime-return")) {
            QVERIFY(out.contains("RETURN_WITHOUT_EXEC"));
            QVERIFY(!out.contains("QUIT_SIGNAL"));
        } else {
            QVERIFY(out.contains("QUIT_SIGNAL"));
            QVERIFY(out.contains("HEARTBEATS "));
        }
        const int count = mode == "lifetime-multiple" ? 3 : 1;
        for (int i = 0; i < count; ++i) {
            ConfigStore reopened(fileOptions(i ? path + QString(".%1").arg(i) : path));
            QTRY_COMPARE(reopened.status(), ConfigStore::Status::Ready);
            QCOMPARE(reopened.value({"a"}).toLongLong(), 77 + i);
        }
    }
    void shutdownSettlement() {
        QFETCH(QString, mode);
        QTemporaryDir dir;
        const auto path = dir.filePath("config.json");
        QVERIFY(writeFile(path, "{}"));
        QProcess child;
        child.start(QCoreApplication::applicationDirPath() + "/file_writer",
                    {path, "a", "77", mode});
        QVERIFY(child.waitForFinished(7000));
        const auto out = child.readAllStandardOutput(), err = child.readAllStandardError();
        qInfo("shutdown mode=%s exitStatus=%d exitCode=%d stdout=%s stderr=%s", qPrintable(mode),
              int(child.exitStatus()), child.exitCode(), out.constData(), err.constData());
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
        QVERIFY(err.isEmpty());
        const auto result = QJsonDocument::fromJson(out.trimmed()).object();
        QVERIFY(result.value("elapsedMs").toInt() >= 180);
        QVERIFY(result.value("elapsedMs").toInt() < 5000);
        QVERIFY(result.value("heartbeats").toInt() > 5);
        if (mode == "shutdown-destroy-closing")
            QCOMPARE(result.value("closes").toInt(), 0); // Destroyed consumer cancels callbacks.
        else {
            QCOMPARE(result.value("closes").toInt(), 1);
            QCOMPARE(result.value("prefix").toString(), QString("1"));
            QCOMPARE(result.value("status").toString(), mode.endsWith("fault") ? QString("failed")
                                                        : mode == "shutdown-ready"
                                                            ? QString("nothing_to_save")
                                                            : QString("saved"));
            if (mode == "shutdown-ready")
                QCOMPARE(result.value("saved").toString(), QString("saved"));
        }
        if (mode.endsWith("fault")) {
            QCOMPARE(readFile(path), QByteArray("{}"));
            QCOMPARE(result.value("error").toString(), QString("shutdown-write-fault"));
        } else {
            ConfigStore reopened(fileOptions(path));
            QTRY_COMPARE(reopened.status(), ConfigStore::Status::Ready);
            QCOMPARE(reopened.value({"a"}).toLongLong(), 77);
        }
    }
    void partialShardsAndRootReset() {
        QTemporaryDir dir;
        ConfigStoreOptions o;
        o.schema = ConfigSchema(D::group({{"x", D::group({{"a", D::leaf(T::SafeInteger, 0)}})},
                                          {"y", D::group({{"a", D::leaf(T::SafeInteger, 0)}})}}));
        o.layers = {{"user",
                     ConfigLayerKind::File,
                     ConfigAdmissionPolicy::FieldFallback,
                     {{"x", {"x"}, {}}, {"y", {"y"}, {}}},
                     true}};
        o.jsonFiles = {{"user", "x", dir.filePath("x.json")},
                       {"user", "y", dir.filePath("y.json")}};
        o.autoSave = false;
        o.fileFault = [](const auto &phase, const auto &file) {
            return phase == "write" && file.endsWith("y.json") ? QString("disk-full") : QString();
        };
        ConfigStore partial(o);
        QTRY_COMPARE(partial.status(), ConfigStore::Status::Ready);
        QSignalSpy done(&partial, &ConfigStore::completed);
        partial.setUser({"x", "a"}, 1);
        partial.setUser({"y", "a"}, 2);
        partial.save();
        QTRY_COMPARE(done.size(), 3);
        const auto result = qvariant_cast<ConfigResult>(done.last()[0]);
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.publicationState, QString("partial"));
        QCOMPARE(result.coveredRevisions.size(), 1);
        QCOMPARE(result.publications.size(), 2);
        QCOMPARE(result.error.code, QString("disk-full"));
        o.fileFault = {};
        QVERIFY(writeFile(dir.filePath("y.json"), "{\"a\":3,\"unknown\":4}"));
        ConfigStore reset(o);
        QTRY_COMPARE(reset.status(), ConfigStore::Status::Ready);
        reset.reset({}, "user");
        reset.close();
        QTRY_COMPARE(reset.status(), ConfigStore::Status::Closed);
        QCOMPARE(readFile(dir.filePath("x.json")).trimmed(), QByteArray("{}"));
        QCOMPARE(readFile(dir.filePath("y.json")).trimmed(), QByteArray("{}"));
    }
    void conditionalNoopAndNamespaceConflict() {
        QTemporaryDir dir;
        const auto path = dir.filePath("config.json");
        const QByteArray bytes = "{ \"a\" : false }";
        QVERIFY(writeFile(path, bytes));
        ConfigStore store(fileOptions(path));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        store.initializeIfMissing({"a"}, 1);
        QSignalSpy done(&store, &ConfigStore::completed);
        store.save();
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(qvariant_cast<ConfigResult>(done.last()[0]).status, QString("nothing_to_save"));
        QCOMPARE(readFile(path), bytes); // Present invalid raw value was not rewritten.
        ConfigJsonBackend backend({{"user", "json", path}});
        const QVariantMap base{{"group", QVariantMap{}}};
        QVERIFY(writeFile(path, "{\"group\":\"foreign\"}"));
        auto conflict = backend.reconcile("json", base, {{{"group", "a"}, QVariant(1), false}});
        QVERIFY(!conflict.outcome.success);
        QCOMPARE(conflict.outcome.error.code, QString("path-conflict"));
        auto conditional = backend.reconcile("json", base, {{{"group", "a"}, QVariant(1), true}});
        QVERIFY(!conditional.outcome.success);
        QVERIFY(readFile(path).contains("foreign"));
    }
    void readonlyLayersPoliciesAndPermissions() {
        QTemporaryDir dir;
        const auto system = dir.filePath("system.json"), user = dir.filePath("user.json");
        QVERIFY(writeFile(system, "{\"a\":2,\"b\":3}"));
        QVERIFY(writeFile(user, "{\"a\":4}"));
        QVERIFY(::chmod(QFile::encodeName(user).constData(), 0640) == 0);
        auto o = fileOptions(user);
        o.layers.prepend({"system",
                          ConfigLayerKind::File,
                          ConfigAdmissionPolicy::RejectInput,
                          {{"system-json", {}, {}}},
                          false});
        o.jsonFiles.prepend({"system", "system-json", system, false, ConfigWatchMode::Live});
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QCOMPARE(store.value({"a"}).toLongLong(), 4);
        QCOMPARE(store.value({"b"}).toLongLong(), 3);
        QVERIFY(writeFile(system, "{\"a\":7,\"b\":\"invalid\"}"));
        QSignalSpy errors(&store, &ConfigStore::sourceError);
        QTest::qWait(100);
        QCOMPARE(store.value({"b"}).toLongLong(), 3); // Whole source projection rejected.
        QCOMPARE(errors.size(), 1);
        store.setUser({"a"}, 9);
        store.close();
        QTRY_COMPARE(store.status(), ConfigStore::Status::Closed);
        struct stat st;
        QVERIFY(::stat(QFile::encodeName(user).constData(), &st) == 0);
        QCOMPARE(st.st_mode & 0777, mode_t(0640));
        QVERIFY(!QFileInfo::exists(system + ".lock"));
        ConfigStore invalid(o);
        QTRY_COMPARE(invalid.status(), ConfigStore::Status::Error);
        const auto missing = dir.filePath("missing.json");
        auto required = fileOptions(missing);
        required.jsonFiles[0].optional = false;
        ConfigStore absent(required);
        QTRY_COMPARE(absent.status(), ConfigStore::Status::Error);
    }
    void writerPreflightAndIdentityReplacement() {
        QTemporaryDir dir;
        const auto path = dir.filePath("config.json");
        QVERIFY(writeFile(path, "{\"a\":1}"));
        auto o = fileOptions(path);
        o.fileFault = [](const auto &phase, const auto &file) {
            if (phase == "rename")
                writeFile(file, "{\"a\":99}");
            return QString();
        };
        ConfigStore store(o);
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QSignalSpy done(&store, &ConfigStore::completed);
        store.setUser({"a"}, 2);
        store.save();
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(qvariant_cast<ConfigResult>(done.last()[0]).error.code,
                 QString("revision-conflict"));
        QVERIFY(readFile(path).contains("99"));
        // Same bytes / different inode is an external identity, not own echo.
        ConfigStore live(fileOptions(path, ConfigWatchMode::Live));
        QTRY_COMPARE(live.status(), ConfigStore::Status::Ready);
        bool observed = false;
        quint64 before = 0, after = 0;
        Probe::run(
            live, [&](auto &w) { before = w.jsonBackend()->reads("json"); }, this,
            [&] { observed = true; });
        QTRY_VERIFY(observed);
        QVERIFY(writeFile(path, readFile(path)));
        QTest::qWait(100);
        observed = false;
        Probe::run(
            live, [&](auto &w) { after = w.jsonBackend()->reads("json"); }, this,
            [&] { observed = true; });
        QTRY_VERIFY(observed);
        QCOMPARE(after, before + 1);
        QCOMPARE(live.inputs().size(), 0);
    }
    void unwritableDirectoryRetainsOriginal() {
        QTemporaryDir dir;
        const auto path = dir.filePath("config.json");
        const QByteArray original = "{\"a\":1}";
        QVERIFY(writeFile(path, original));
        ConfigStore store(fileOptions(path));
        QTRY_COMPARE(store.status(), ConfigStore::Status::Ready);
        QVERIFY(::chmod(QFile::encodeName(dir.path()).constData(), 0500) == 0);
        QSignalSpy done(&store, &ConfigStore::completed);
        store.setUser({"a"}, 2);
        store.save();
        QTRY_COMPARE(done.size(), 2);
        const auto result = qvariant_cast<ConfigResult>(done.last()[0]);
        QVERIFY(::chmod(QFile::encodeName(dir.path()).constData(), 0700) == 0);
        QCOMPARE(result.status, QString("failed"));
        QCOMPARE(result.publicationState, QString("known"));
        QCOMPARE(readFile(path), original);
        QCOMPARE(store.value({"a"}).toLongLong(), 2);
    }
};
QTEST_GUILESS_MAIN(JsonFilesTest)
#include "test_json_files.moc"
