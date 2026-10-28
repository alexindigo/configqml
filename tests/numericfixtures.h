// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include <QVariant>
#include <QList>
#include <limits>
struct NumericFixture {
    QString token;
    bool unsignedType;
    QVariant expected; // Missing means integer admission must reject.
    QString error;
};
inline QList<NumericFixture> numericFixtures() {
    return {
        {"1.5", false, {}, "fractional-integer"},
        {"1.0000000000000001", false, {}, "fractional-integer"},
        {"9007199254740993", false, QVariant::fromValue(qint64(9007199254740993LL)), {}},
        {"9007199254740993.0", false, QVariant::fromValue(qint64(9007199254740993LL)), {}},
        {"9223372036854775807", false, QVariant::fromValue(std::numeric_limits<qint64>::max()), {}},
        {"9223372036854775808", false, {}, "integer-range"},
        {"-9223372036854775808",
         false,
         QVariant::fromValue(std::numeric_limits<qint64>::min()),
         {}},
        {"-9223372036854775809", false, {}, "integer-range"},
        {"9223372036854775808", true, QVariant::fromValue(quint64(1) << 63), {}},
        {"18446744073709551615",
         true,
         QVariant::fromValue(std::numeric_limits<quint64>::max()),
         {}},
        {"18446744073709551615.0",
         true,
         QVariant::fromValue(std::numeric_limits<quint64>::max()),
         {}},
        {"18446744073709551616", true, {}, "integer-range"},
        {"-1", true, {}, "integer-range"},
        {"-0", false, QVariant::fromValue(qint64(0)), {}},
        {"-0", true, QVariant::fromValue(quint64(0)), {}},
        {"1e3", false, QVariant::fromValue(qint64(1000)), {}},
        {"1.2300e2", false, QVariant::fromValue(qint64(123)), {}},
        {"1e-9999999999999999999999", false, {}, "fractional-integer"},
        {"1e9999999999999999999999", false, {}, "integer-range"},
        {"0e9999999999999999999999", false, QVariant::fromValue(qint64(0)), {}}};
}
