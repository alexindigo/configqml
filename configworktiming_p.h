// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include <QObject>
#include <QList>
#include <QPointer>
#include <optional>
#include <memory>

namespace configqml {
// Private, opt-in measurement. Released owners acquire no data members and
// ordinary consumers never read scheduler accounting or emit timing samples.
struct ConfigThreadWorkSample {
    qint64 wall = 0, cpu = 0, runqueue = 0, voluntary = 0, involuntary = 0;
    bool valid = false;
    qint64 blocked() const { return qMax(qint64(0), wall - cpu - runqueue); }
    qint64 charged() const { return cpu + blocked(); }
    bool within(qint64 budget) const { return valid && cpu < budget && charged() < budget; }
    void report(const char *label) const;
    ConfigThreadWorkSample &operator+=(const ConfigThreadWorkSample &other);
};
ConfigThreadWorkSample operator+(ConfigThreadWorkSample a, const ConfigThreadWorkSample &b);
ConfigThreadWorkSample operator-(const ConfigThreadWorkSample &a, const ConfigThreadWorkSample &b);

class ConfigThreadWorkTimer {
public:
    ConfigThreadWorkTimer();
    ConfigThreadWorkSample finish() const;

private:
    struct Stamp {
        qint64 wall = 0, cpu = 0, runqueue = 0, voluntary = 0, involuntary = 0, tid = 0;
        bool valid = false;
    };
    static Stamp capture();
    Stamp m_start;
};

struct ConfigWorkFrame {
    ConfigThreadWorkSample notifications{0, 0, 0, 0, 0, true};
    QList<ConfigThreadWorkSample> samples;
};
class ConfigWorkTiming final : public QObject {
public:
    static ConfigWorkTiming *get(QObject *owner);
    static void enable(QObject *owner);
    ConfigThreadWorkSample lastModel, worst;
    bool validMeasurements = true;
    QList<std::shared_ptr<ConfigWorkFrame>> frames;
    explicit ConfigWorkTiming(QObject *owner) : QObject(owner) {}
};

class ConfigWorkPublication {
public:
    ConfigWorkPublication(QObject *owner, bool models);
    ~ConfigWorkPublication();

private:
    QPointer<ConfigWorkTiming> m_state;
    std::shared_ptr<ConfigWorkFrame> m_frame;
    std::optional<ConfigThreadWorkTimer> m_timer;
    bool m_models;
};
} // namespace configqml
