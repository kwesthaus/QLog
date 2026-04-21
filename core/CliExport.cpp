#include "core/CliExport.h"

#include <QDate>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QScopedPointer>
#include <QTextStream>

#include "core/QSOFilterManager.h"
#include "data/Data.h"
#include "data/StationProfile.h"
#include "logformat/LogFormat.h"

namespace
{
const QString kCommandName = QStringLiteral("export");

struct CliExportOptions
{
    bool useStdout = false;
    QString outputPath;
    QString maximumQSOIDFilePath;
    QDate startDate;
    QDate endDate;
    qint64 minimumQSOID = -1;
    QString stationProfileName;
    QString myCallsign;
    QString myGridsquare;
    QString userFilter;
    QString exportType = QStringLiteral("generic");
    QString qslSendVia;
    bool includeQslNo = false;
    bool includeQslIgnore = false;
    bool includeQslAlreadySent = false;
};

struct CliExportResult
{
    long count = -1;
    qint64 maximumQSOID = -1;
};

QString normalizeQslSendVia(const QString &rawValue)
{
    const QString value = rawValue.trimmed().toLower();

    if ( value == QLatin1String("b") || value == QLatin1String("bureau") )
        return QStringLiteral("B");

    if ( value == QLatin1String("d") || value == QLatin1String("direct") )
        return QStringLiteral("D");

    if ( value == QLatin1String("e") || value == QLatin1String("electronic") )
        return QStringLiteral("E");

    if ( value == QLatin1String("blank") || value == QLatin1String("none") )
        return QStringLiteral(" ");

    return {};
}

bool parseOptions(const QCommandLineParser &parser, CliExportOptions &options, QString &errorMessage)
{
    const QStringList positionalArguments = parser.positionalArguments();
    if ( positionalArguments.count() > 1 && positionalArguments.first() == kCommandName )
    {
        errorMessage = QObject::tr("Unexpected positional arguments after 'export'.");
        return false;
    }

    options.useStdout = parser.isSet(QStringLiteral("stdout"));
    options.outputPath = parser.value(QStringLiteral("output"));

    if ( options.useStdout == !options.outputPath.isEmpty() )
    {
        errorMessage = QObject::tr("Use exactly one of --output or --stdout.");
        return false;
    }

    const QString startDateText = parser.value(QStringLiteral("date-from"));
    const QString endDateText = parser.value(QStringLiteral("date-to"));
    const QString minimumQSOIDText = parser.value(QStringLiteral("min-qso-id")).trimmed();
    options.maximumQSOIDFilePath = parser.value(QStringLiteral("max-qso-id-file"));

    if ( startDateText.isEmpty() != endDateText.isEmpty() )
    {
        errorMessage = QObject::tr("Use both --date-from and --date-to together.");
        return false;
    }

    if ( !startDateText.isEmpty() )
    {
        options.startDate = QDate::fromString(startDateText, Qt::ISODate);
        options.endDate = QDate::fromString(endDateText, Qt::ISODate);

        if ( !options.startDate.isValid() || !options.endDate.isValid() )
        {
            errorMessage = QObject::tr("Dates must use ISO format YYYY-MM-DD.");
            return false;
        }

        if ( options.startDate > options.endDate )
        {
            errorMessage = QObject::tr("--date-from must not be after --date-to.");
            return false;
        }
    }

    if ( !minimumQSOIDText.isEmpty() )
    {
        bool isValidMinimumQSOID = false;
        options.minimumQSOID = minimumQSOIDText.toLongLong(&isValidMinimumQSOID);

        if ( !isValidMinimumQSOID || options.minimumQSOID < 0 )
        {
            errorMessage = QObject::tr("--min-qso-id must be a non-negative integer.");
            return false;
        }
    }

    options.stationProfileName = parser.value(QStringLiteral("station-profile"));
    options.myCallsign = parser.value(QStringLiteral("my-callsign"));
    options.myGridsquare = parser.value(QStringLiteral("my-grid"));
    options.userFilter = parser.value(QStringLiteral("user-filter"));

    const bool hasQslFilterOptions = parser.isSet(QStringLiteral("qsl-send-via"))
                                  || parser.isSet(QStringLiteral("qsl-include-no"))
                                  || parser.isSet(QStringLiteral("qsl-include-invalid"))
                                  || parser.isSet(QStringLiteral("qsl-include-sent"));

    const QString exportTypeValue = parser.value(QStringLiteral("type")).trimmed().toLower();
    if ( !exportTypeValue.isEmpty() )
    {
        if ( exportTypeValue != QLatin1String("generic") && exportTypeValue != QLatin1String("qsl") )
        {
            errorMessage = QObject::tr("--type must be either 'generic' or 'qsl'.");
            return false;
        }

        options.exportType = exportTypeValue;
    }
    else if ( hasQslFilterOptions )
    {
        options.exportType = QStringLiteral("qsl");
    }

    if ( parser.isSet(QStringLiteral("qsl-send-via")) )
    {
        options.qslSendVia = normalizeQslSendVia(parser.value(QStringLiteral("qsl-send-via")));
        if ( options.qslSendVia.isNull() )
        {
            errorMessage = QObject::tr("--qsl-send-via must be one of: B, D, E, bureau, direct, electronic, blank.");
            return false;
        }
    }

    options.includeQslNo = parser.isSet(QStringLiteral("qsl-include-no"));
    options.includeQslIgnore = parser.isSet(QStringLiteral("qsl-include-invalid"));
    options.includeQslAlreadySent = parser.isSet(QStringLiteral("qsl-include-sent"));

    if ( options.exportType != QLatin1String("qsl") && hasQslFilterOptions )
    {
        errorMessage = QObject::tr("QSL-specific filter options require --type qsl.");
        return false;
    }

    return true;
}

bool validateOptions(const CliExportOptions &options, QString &errorMessage)
{
    if ( !options.maximumQSOIDFilePath.isEmpty() && options.minimumQSOID < 0 )
    {
        errorMessage = QObject::tr("--max-qso-id-file requires --min-qso-id.");
        return false;
    }

    if ( !options.stationProfileName.isEmpty() )
    {
        const QStringList names = StationProfilesManager::instance()->profileNameList();
        if ( !names.contains(options.stationProfileName) )
        {
            errorMessage = QObject::tr("Unknown station profile '%1'.").arg(options.stationProfileName);
            return false;
        }
    }

    if ( !options.userFilter.isEmpty() )
    {
        const QStringList filters = QSOFilterManager::instance()->getFilterList();
        if ( !filters.contains(options.userFilter) )
        {
            errorMessage = QObject::tr("Unknown saved user filter '%1'.").arg(options.userFilter);
            return false;
        }
    }

    return true;
}

void applyFilters(LogFormat *format, const CliExportOptions &options)
{
    if ( format == nullptr )
        return;

    if ( options.startDate.isValid() )
        format->setFilterDateRange(options.startDate, options.endDate);

    if ( options.minimumQSOID >= 0 )
        format->setFilterMinimumQSOID(options.minimumQSOID);

    if ( !options.stationProfileName.isEmpty() )
        format->setFilterStationProfile(StationProfilesManager::instance()->getProfile(options.stationProfileName));

    if ( !options.myCallsign.isEmpty() )
        format->setFilterMyCallsign(options.myCallsign);

    if ( !options.myGridsquare.isEmpty() )
        format->setFilterMyGridsquare(options.myGridsquare);

    if ( !options.userFilter.isEmpty() )
        format->setUserFilter(options.userFilter);

    if ( options.exportType == QLatin1String("qsl") )
    {
        format->setFilterSentPaperQSL(options.includeQslNo,
                                      options.includeQslIgnore,
                                      options.includeQslAlreadySent);

        if ( !options.qslSendVia.isEmpty() )
            format->setFilterSendVia(options.qslSendVia);
    }
}

bool queryMaximumQSOID(LogFormat *format, qint64 &maximumQSOID, QString &errorMessage)
{
    if ( format == nullptr )
    {
        errorMessage = QObject::tr("Failed to initialize ADIF export.");
        return false;
    }

    QSqlQuery query;
    const QString queryStatement = QStringLiteral("SELECT MAX(id) FROM contacts WHERE %1")
                                       .arg(format->getWhereClause());

    if ( !query.prepare(queryStatement) )
    {
        errorMessage = QObject::tr("Cannot prepare maximum QSO ID query: %1")
                           .arg(query.lastError().text());
        return false;
    }

    format->bindWhereClause(query);

    if ( !query.exec() )
    {
        errorMessage = QObject::tr("Cannot execute maximum QSO ID query: %1")
                           .arg(query.lastError().text());
        return false;
    }

    if ( !query.next() || query.value(0).isNull() )
    {
        maximumQSOID = -1;
        return true;
    }

    maximumQSOID = query.value(0).toLongLong();
    return true;
}

bool writeMaximumQSOIDFile(const CliExportOptions &options, qint64 maximumQSOID, QString &errorMessage)
{
    if ( options.maximumQSOIDFilePath.isEmpty() )
        return true;

    QFile outputFile(options.maximumQSOIDFilePath);
    if ( !outputFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate) )
    {
        errorMessage = QObject::tr("Cannot write maximum QSO ID to '%1'.")
                           .arg(QFileInfo(outputFile).filePath());
        return false;
    }

    QTextStream outputStream(&outputFile);
    if ( maximumQSOID >= 0 )
        outputStream << maximumQSOID << Qt::endl;

    outputStream.flush();
    outputFile.close();
    return true;
}

CliExportResult runExport(QTextStream &stream, const CliExportOptions &options, QString &errorMessage)
{
    CliExportResult result;

    QScopedPointer<LogFormat> format(LogFormat::open(QStringLiteral("adi"), stream));
    if ( !format )
    {
        errorMessage = QObject::tr("Failed to initialize ADIF export.");
        return result;
    }

    applyFilters(format.data(), options);
    result.count = format->runExport();

    if ( result.count < 0 )
        return result;

    if ( !queryMaximumQSOID(format.data(), result.maximumQSOID, errorMessage) )
        result.count = -1;

    if ( result.count >= 0 && !writeMaximumQSOIDFile(options, result.maximumQSOID, errorMessage) )
        result.count = -1;

    return result;
}
}

const QString &CliExport::commandName()
{
    return kCommandName;
}

void CliExport::addOptions(QCommandLineParser &parser)
{
    parser.addPositionalArgument(QStringLiteral("command"),
                                 QObject::tr("Command to run. Use 'export' for command-line ADIF export."),
                                 QStringLiteral("[command]"));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("output"),
                                        QObject::tr("Write ADIF export to <path>."),
                                        QObject::tr("path")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("stdout"),
                                        QObject::tr("Write ADIF export to standard output.")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("date-from"),
                                        QObject::tr("Filter export start date (YYYY-MM-DD)."),
                                        QObject::tr("date")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("date-to"),
                                        QObject::tr("Filter export end date (YYYY-MM-DD)."),
                                        QObject::tr("date")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("min-qso-id"),
                                        QObject::tr("Export only contacts with QSO ID greater than <id>."),
                                        QObject::tr("id")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("max-qso-id-file"),
                                        QObject::tr("Write the maximum exported QSO ID to <path>. Requires --min-qso-id."),
                                        QObject::tr("path")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("station-profile"),
                                        QObject::tr("Export only contacts matching station profile <name>."),
                                        QObject::tr("name")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("my-callsign"),
                                        QObject::tr("Export only contacts matching station callsign <callsign>."),
                                        QObject::tr("callsign")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("my-grid"),
                                        QObject::tr("Export only contacts matching my gridsquare <grid>."),
                                        QObject::tr("grid")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("user-filter"),
                                        QObject::tr("Apply saved user-defined filter <name>."),
                                        QObject::tr("name")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("type"),
                                        QObject::tr("Use export filter type <generic|qsl>."),
                                        QObject::tr("type")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("qsl-send-via"),
                                        QObject::tr("For QSL exports, filter by send-via value <B|D|E|blank>."),
                                        QObject::tr("value")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("qsl-include-no"),
                                        QObject::tr("For QSL exports, include records with qsl_sent = N.")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("qsl-include-invalid"),
                                        QObject::tr("For QSL exports, include records with qsl_sent = I.")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("qsl-include-sent"),
                                        QObject::tr("For QSL exports, include records with qsl_sent = Y.")));
}

bool CliExport::isRequested(const QCommandLineParser &parser)
{
    return parser.positionalArguments().value(0) == kCommandName;
}

int CliExport::run(const QCommandLineParser &parser)
{
    CliExportOptions options;
    QString errorMessage;

    if ( !parseOptions(parser, options, errorMessage) || !validateOptions(options, errorMessage) )
    {
        qCritical().noquote() << errorMessage;
        return 1;
    }

    Data::instance();

    CliExportResult result;
    if ( options.useStdout )
    {
        QTextStream outputStream(stdout, QIODevice::WriteOnly);
        result = runExport(outputStream, options, errorMessage);
        outputStream.flush();
    }
    else
    {
        QFile outputFile(options.outputPath);
        if ( !outputFile.open(QIODevice::WriteOnly | QIODevice::Text) )
        {
            qCritical().noquote() << QObject::tr("Cannot write to '%1'.").arg(QFileInfo(outputFile).filePath());
            return 1;
        }

        QTextStream outputStream(&outputFile);
        result = runExport(outputStream, options, errorMessage);
        outputStream.flush();
        outputFile.close();
    }

    if ( result.count < 0 )
    {
        qCritical().noquote() << errorMessage;
        return 1;
    }

    if ( result.maximumQSOID >= 0 )
    {
        qInfo().noquote() << QObject::tr("Exported %1 contact(s). Maximum exported QSO ID: %2.")
                                 .arg(result.count)
                                 .arg(result.maximumQSOID);
    }
    else
    {
        qInfo().noquote() << QObject::tr("Exported %1 contact(s). Maximum exported QSO ID: none.")
                                 .arg(result.count);
    }

    return 0;
}