#ifndef QLOG_CORE_CLIEXPORT_H
#define QLOG_CORE_CLIEXPORT_H

#include <QCommandLineParser>

class CliExport
{
public:
    static const QString &commandName();
    static void addOptions(QCommandLineParser &parser);
    static bool isRequested(const QCommandLineParser &parser);
    static int run(const QCommandLineParser &parser);
};

#endif // QLOG_CORE_CLIEXPORT_H