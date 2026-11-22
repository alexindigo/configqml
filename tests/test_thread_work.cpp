// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configworktiming_p.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDebug>
#include <QProcess>
#include <QTest>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <cerrno>

using namespace configqml;
namespace {
constexpr qint64 budget = 16000000;
void burnCpu(qint64 ns) {
    timespec start{}, next{};
    ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start);
    do {
        ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &next);
    } while ((next.tv_sec - start.tv_sec) * 1000000000LL + next.tv_nsec - start.tv_nsec < ns);
}
int schedulingChild(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_param priority{};
    priority.sched_priority = 1;
    if (::sched_setaffinity(0, sizeof(set), &set) || ::sched_setscheduler(0, SCHED_FIFO, &priority))
        return 3;
    if (::write(STDOUT_FILENO, "ready\n", 6) != 6)
        return 4;
    char command;
    if (::read(STDIN_FILENO, &command, 1) != 1)
        return 5;
    burnCpu(80000000);
    return 0;
}
int control(const QString &kind, const QString &gate) {
    QProcess child;
    cpu_set_t original;
    if (kind == "schedule") {
        if (::sched_getaffinity(0, sizeof(original), &original))
            return 6;
        int cpu = 0;
        while (cpu < CPU_SETSIZE && !CPU_ISSET(cpu, &original))
            ++cpu;
        if (cpu == CPU_SETSIZE)
            return 7;
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (::sched_setaffinity(0, sizeof(set), &set))
            return 8;
        child.start("sudo", {"-n", QCoreApplication::applicationFilePath(), "--schedule-child",
                             QString::number(cpu)});
        if (!child.waitForReadyRead(5000) || child.readLine() != "ready\n")
            return 9;
    }
    ConfigThreadWorkTimer timer;
    if (kind == "cpu")
        burnCpu(40000000);
    else if (kind == "block") {
        timespec remaining{0, 40000000};
        while (::nanosleep(&remaining, &remaining) && errno == EINTR) {
        }
    } else if (kind == "schedule") {
        if (child.write("x", 1) != 1 || (child.bytesToWrite() && !child.waitForBytesWritten(5000)))
            return 10;
    } else
        return 11;
    const auto sample = timer.finish();
    sample.report(qPrintable(kind + "-" + gate));
    bool permitted = gate == "legacy-wall" ? sample.wall < budget
                     : gate == "cpu-only"  ? sample.valid && sample.cpu < budget
                                           : sample.within(budget);
    if (kind == "schedule") {
        const bool finished = child.waitForFinished(5000);
        const int restore = ::sched_setaffinity(0, sizeof(original), &original);
        if (!finished || child.exitStatus() != QProcess::NormalExit || child.exitCode() ||
            restore || sample.runqueue < 32000000 || sample.wall < 32000000)
            return 12;
    }
    return permitted ? 0 : 2;
}
} // namespace
class ThreadWorkTest : public QObject {
    Q_OBJECT
private slots:
    void optionalAndNestedPublicationStorage() {
        QObject owner;
        QVERIFY(!ConfigWorkTiming::get(&owner));
        {
            const ConfigWorkPublication unmeasured(&owner, true);
        }
        QVERIFY(!ConfigWorkTiming::get(&owner));
        ConfigWorkTiming::enable(&owner);
        auto *state = ConfigWorkTiming::get(&owner);
        QVERIFY(state);
        {
            const ConfigWorkPublication outer(&owner, true);
            QCOMPARE(state->frames.size(), 1);
            const auto frame = state->frames.last();
            {
                const ConfigWorkPublication inner(&owner, false);
                QCOMPARE(state->frames.size(), 2);
                QVERIFY(state->frames.last() != frame);
            }
            QCOMPARE(state->frames.size(), 1);
            QCOMPARE(state->frames.last(), frame);
        }
        QVERIFY(state->frames.isEmpty());
        QVERIFY(state->lastModel.valid);
        QVERIFY(!ConfigThreadWorkSample{}.within(budget));
    }
    void realWorkAndScheduling_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("gate");
        QTest::addColumn<int>("exit");
        QTest::newRow("cpu-rejected") << QString("cpu") << QString("charged") << 2;
        QTest::newRow("blocked-rejected") << QString("block") << QString("charged") << 2;
        QTest::newRow("scheduling-excluded") << QString("schedule") << QString("charged") << 0;
        QTest::newRow("legacy-rejects-scheduling-control")
            << QString("schedule") << QString("legacy-wall") << 2;
        QTest::newRow("cpu-only-hides-blocking-control")
            << QString("block") << QString("cpu-only") << 0;
    }
    void realWorkAndScheduling() {
        QFETCH(QString, kind);
        QFETCH(QString, gate);
        QFETCH(int, exit);
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--control", kind, gate});
        QVERIFY(child.waitForFinished(10000));
        const auto output = child.readAllStandardError() + child.readAllStandardOutput();
        qInfo().noquote() << output;
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), exit);
        QVERIFY(output.contains("CALLER_WORK"));
        QVERIFY(output.contains("valid=1"));
        QVERIFY(output.contains("runqueue_ns="));
        QVERIFY(output.contains("blocked_ns="));
        QVERIFY(output.contains("voluntary="));
        QVERIFY(output.contains("involuntary="));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() == 3 && arguments[1] == "--schedule-child")
        return schedulingChild(arguments[2].toInt());
    if (arguments.size() == 4 && arguments[1] == "--control")
        return control(arguments[2], arguments[3]);
    ThreadWorkTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_thread_work.moc"
