// SPDX-License-Identifier: LGPL-3.0-or-later
#include "configvalue.h"
#include <QCoreApplication>
#include <cstring>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    return std::strcmp(configqml::version(), "0.1.0");
}
