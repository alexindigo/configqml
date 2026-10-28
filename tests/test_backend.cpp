// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configmemorybackend.h"
#include "configcodec.h"
#include "configlayers.h"
#include "numericfixtures.h"
#include <QTest>
#include <thread>
using namespace configqml;
using Op = ConfigBackendRequest::Operation;
using Fault = ConfigMemoryBackend::Fault;
static ConfigBackendResult read(ConfigBackend &backend, const QString &id) {
    return *backend.take(backend.submit({Op::Read, id, {}, {}}));
}
class BackendTest : public QObject {
    Q_OBJECT
private slots:
    void losslessCodecReadback() {
        class NumericCodec : public ConfigCodec {
        public:
            ConfigCodecCapabilities capabilities() const override {
                return {true, true, true, false, true, true, false, true};
            }
            ConfigDecodeResult decode(const QByteArray &bytes, const QString &source) override {
                auto raw = decodeJsonValue(bytes);
                return raw.ok() && raw.value.metaType().id() == QMetaType::QVariantMap
                           ? ConfigDecodeResult{raw.value.toMap(), {}}
                           : ConfigDecodeResult{{}, {"decode", source, "decode"}};
            }
            ConfigEncodeResult encode(const QVariantMap &raw, const QString &source) override {
                auto result = encodeJsonValue(raw);
                return result.ok() ? ConfigEncodeResult{result.bytes, {}}
                                   : ConfigEncodeResult{{}, {"encode", source, "encode"}};
            }
        } codec;
        for (const auto &row : numericFixtures()) {
            const QByteArray original = "{\"n\":" + row.token.toLatin1() + "}";
            auto decoded = codec.decode(original, "source");
            QVERIFY(decoded.ok());
            auto encoded = codec.encode(decoded.raw, "source");
            QVERIFY(encoded.ok());
            QCOMPARE(encoded.bytes, original);
            ConfigMemoryBackend memory;
            auto write = memory.take(memory.submit({Op::Publish, "source", encoded.bytes, {}}));
            QVERIFY(write && write->success);
            auto captured = read(memory, "source");
            QCOMPARE(captured.bytes, original);
            auto reopened = codec.decode(captured.bytes, "source");
            QVERIFY(reopened.ok());
            using D = ConfigDescriptor;
            auto number = D::leaf(row.unsignedType ? D::Type::UnsignedInteger : D::Type::Integer,
                                  row.unsignedType ? QVariant::fromValue(quint64(0))
                                                   : QVariant::fromValue(qint64(0)));
            ConfigLayers engine(ConfigSchema(D::group({{"n", number}})),
                                {{"user",
                                  ConfigLayerKind::File,
                                  ConfigAdmissionPolicy::FieldFallback,
                                  {{"source", {}, reopened.raw}},
                                  true}});
            if (row.expected.isValid())
                QCOMPARE(*engine.value({"n"}), row.expected);
            else
                QCOMPARE(engine.diagnostics("user", "source").first().code, row.error);
        }
    }
    void publicationAndConflict() {
        ConfigMemoryBackend memory;
        ConfigBackend &backend = memory;
        QVERIFY(backend.capabilities().conditionalPublish);
        QVERIFY(!backend.capabilities().watch);
        auto initial = read(backend, "user");
        QVERIFY(initial.success);
        QVERIFY(!initial.revision.exists);
        auto id = backend.submit({Op::Publish, "user", "{}", initial.revision});
        auto publication = backend.take(id);
        QVERIFY(publication && publication->success);
        QCOMPARE(publication->bytes, QByteArray("{}"));
        QCOMPARE(read(backend, "user").bytes, QByteArray("{}"));
        QVERIFY(!backend.take(id));
        auto stale = backend.take(backend.submit({Op::Publish, "user", "bad", initial.revision}));
        QVERIFY(stale && !stale->success);
        QCOMPARE(stale->error.code, QString("revision-conflict"));
        QCOMPARE(read(backend, "user").bytes, QByteArray("{}"));
    }
    void deterministicDelayAndOwnership() {
        ConfigMemoryBackend memory;
        QVERIFY(memory.schedule({true, Fault::None}));
        char bytes[] = "original";
        const auto id = memory.submit({Op::Publish, "user", QByteArray::fromRawData(bytes, 8), {}});
        bytes[0] = 'X';
        QVERIFY(!memory.take(id));
        QCOMPARE(memory.pending(), QList<quint64>{id});
        QVERIFY(memory.release(id));
        QVERIFY(!memory.release(id));
        auto result = memory.take(id);
        QVERIFY(result && result->success);
        QCOMPARE(read(memory, "user").bytes, QByteArray("original"));
        QVERIFY(memory.schedule({true, Fault::None}));
        auto r1 = memory.submit({Op::Read, "user", {}, {}});
        QVERIFY(memory.schedule({true, Fault::None}));
        auto r2 = memory.submit({Op::Read, "user", {}, {}});
        QVERIFY(memory.release(r2));
        QVERIFY(memory.take(r2));
        QVERIFY(!memory.take(r1));
        QVERIFY(memory.release(r1));
        QVERIFY(memory.take(r1));
    }
    void faultsRetainTruth() {
        for (const auto fault :
             {Fault::BeforeOperation, Fault::AfterPublication, Fault::Readback}) {
            ConfigMemoryBackend memory;
            QVERIFY(memory.seed("user", "before"));
            const auto before = read(memory, "user");
            QVERIFY(memory.schedule({false, fault}));
            auto result =
                memory.take(memory.submit({Op::Publish, "user", "after", before.revision}));
            QVERIFY(result && !result->success);
            QVERIFY(!result->error.empty());
            const auto after = read(memory, "user");
            QCOMPARE(after.bytes,
                     fault == Fault::BeforeOperation ? QByteArray("before") : QByteArray("after"));
            QCOMPARE(result->publicationState,
                     fault == Fault::BeforeOperation
                         ? ConfigBackendResult::PublicationState::Known
                         : ConfigBackendResult::PublicationState::Uncertain);
        }
        ConfigMemoryBackend a, b;
        QVERIFY(a.seed("user", "private"));
        QVERIFY(!read(b, "user").revision.exists);
        QVERIFY(a.seed("user", {}, false));
        QVERIFY(!read(a, "user").revision.exists);
    }
    void affinity() {
        ConfigMemoryBackend memory;
        quint64 request = 1;
        bool seeded = true;
        std::thread foreign([&] {
            request = memory.submit({Op::Read, "user", {}, {}});
            seeded = memory.seed("user", "foreign");
        });
        foreign.join();
        QCOMPARE(request, quint64(0));
        QVERIFY(!seeded);
        QVERIFY(!read(memory, "user").revision.exists);
    }
};
QTEST_GUILESS_MAIN(BackendTest)
#include "test_backend.moc"
