// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once
#include <QString>
#include <exception>
namespace configqml {
struct ConfigProjectionFailure final : std::exception {
    explicit ConfigProjectionFailure(QString code) : code(std::move(code)) {}
    QString code;
};
} // namespace configqml
