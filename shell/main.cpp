/*
    SPDX-FileCopyrightText: 2002 Wilco Greven <greven@kde.org>
    SPDX-FileCopyrightText: 2003 Christophe Devriese <Christophe.Devriese@student.kuleuven.ac.be>
    SPDX-FileCopyrightText: 2003 Laurent Montel <montel@kde.org>
    SPDX-FileCopyrightText: 2003-2007 Albert Astals Cid <aacid@kde.org>
    SPDX-FileCopyrightText: 2004 Andy Goossens <andygoossens@telenet.be>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "shell.h"

#include "aboutdata.h"
#include "okular_main.h"
#include "shellutils.h"
#include <KAboutData>
#include <KCrash>
#include <KIconTheme>
#include <KLocalizedString>
#include <KMessageBox>
#include <KWindowSystem>
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QLibraryInfo>
#include <QLocale>
#include <QObject>
#include <QStringList>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QTranslator>
#include <QtGlobal>

#include <cstdio>
#include <cstdlib>
#include <exception>

#define HAVE_STYLE_MANAGER __has_include(<KStyleManager>)
#if HAVE_STYLE_MANAGER
#include <KStyleManager>
#endif

/**
 * Event handler for macOS file opening via QFileOpenEvent.
 * Should not do anything on Windows/Linux as QFileOpenEvent is never fired there.
 */

class MengsheeApplication final : public QApplication
{
public:
    using QApplication::QApplication;

protected:
    bool notify(QObject *receiver, QEvent *event) override
    {
        try {
            return QApplication::notify(receiver, event);
        } catch (const std::exception &exception) {
            reportUnhandledException(exception.what());
        } catch (...) {
            reportUnhandledException(nullptr);
        }
        return false;
    }

private:
    void reportUnhandledException(const char *what) noexcept
    {
        try {
            const QString details = what && *what ? QString::fromLocal8Bit(what) : i18n("Unknown C++ exception");
            qCritical().noquote() << "Mengshee caught an unhandled exception in a Qt event:" << details;
            if (m_exceptionDialogPending || m_exceptionDialogVisible) {
                return;
            }

            m_exceptionDialogPending = true;
            QTimer::singleShot(0, this, [this, details] {
                m_exceptionDialogPending = false;
                m_exceptionDialogVisible = true;
                KMessageBox::error(QApplication::activeWindow(),
                                   i18n("Mengshee stopped an operation after an unexpected internal error. The application is still running, but you should save your work to a new file before continuing.\n\n%1", details));
                m_exceptionDialogVisible = false;
            });
        } catch (...) {
            std::fputs("Mengshee caught an unhandled exception while reporting another exception.\n", stderr);
        }
    }

    bool m_exceptionDialogPending = false;
    bool m_exceptionDialogVisible = false;
};

class FileOpenEventHandler : public QObject
{
    Q_OBJECT
protected:
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (event->type() == QEvent::FileOpen) {
            auto *foe = static_cast<QFileOpenEvent *>(event);
            // Find existing Shell window
            const auto widgets = QApplication::topLevelWidgets();
            for (QWidget *widget : widgets) {
                if (Shell *shell = qobject_cast<Shell *>(widget)) {
                    QString serializedOptions = ShellUtils::serializeOptions(false, false, false, false, false, QString(), QString(), QString());
                    shell->openDocument(foe->url(), serializedOptions);
                    shell->raise();
                    shell->activateWindow();
                    return true;
                }
            }
            return false;
        }
        return QObject::eventFilter(obj, event);
    }
};

static void copyLegacyDataTree(const QString &sourcePath, const QString &destinationPath)
{
    if (!QDir(sourcePath).exists()) {
        return;
    }

    QDir().mkpath(destinationPath);
    QDirIterator iterator(sourcePath, QDir::AllEntries | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    const QDir sourceDirectory(sourcePath);
    while (iterator.hasNext()) {
        const QString sourceEntry = iterator.next();
        const QString relativePath = sourceDirectory.relativeFilePath(sourceEntry);
        const QString destinationEntry = QDir(destinationPath).filePath(relativePath);
        if (iterator.fileInfo().isDir()) {
            QDir().mkpath(destinationEntry);
        } else if (!QFile::exists(destinationEntry)) {
            QDir().mkpath(QFileInfo(destinationEntry).absolutePath());
            QFile::copy(sourceEntry, destinationEntry);
        }
    }
}

static void migrateLegacyApplicationData()
{
    const QString configLocation = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    const QList<QPair<QString, QString>> configFiles = {
        {QStringLiteral("scholiarc"), QStringLiteral("mengsheerc")},
        {QStringLiteral("scholia-generator-popplerrc"), QStringLiteral("mengshee-generator-popplerrc")},
        {QStringLiteral("okular-generator-ghostscriptrc"), QStringLiteral("mengshee-generator-ghostscriptrc")},
    };
    for (const auto &[legacyName, currentName] : configFiles) {
        const QString legacyPath = QDir(configLocation).filePath(legacyName);
        const QString currentPath = QDir(configLocation).filePath(currentName);
        if (QFile::exists(legacyPath) && !QFile::exists(currentPath)) {
            QDir().mkpath(configLocation);
            QFile::copy(legacyPath, currentPath);
        }
    }

    const QString dataLocation = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    copyLegacyDataTree(QDir(dataLocation).filePath(QStringLiteral("scholia")), QDir(dataLocation).filePath(QStringLiteral("mengshee")));
}

static int runMengsheeApplication(int argc, char **argv)
{
    /**
     * trigger initialisation of proper icon theme
     */
#if KICONTHEMES_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    KIconTheme::initTheme();
#endif

    QCoreApplication::setAttribute(Qt::AA_CompressTabletEvents);

    MengsheeApplication app(argc, argv);
    migrateLegacyApplicationData();

    QTranslator qtTranslator;
    if (qtTranslator.load(QLocale(), QStringLiteral("qt"), QStringLiteral("_"), QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        app.installTranslator(&qtTranslator);
    }
    QTranslator qtBaseTranslator;
    if (qtBaseTranslator.load(QLocale(), QStringLiteral("qtbase"), QStringLiteral("_"), QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        app.installTranslator(&qtBaseTranslator);
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QString prefixDir = QDir(appDir).absoluteFilePath(QStringLiteral(".."));
    const QStringList mengsheePluginPaths = {
        QDir(appDir).absoluteFilePath(QStringLiteral("plugins")),
        QDir(prefixDir).absoluteFilePath(QStringLiteral("plugins")),
        QDir(prefixDir).absoluteFilePath(QStringLiteral("lib/plugins")),
    };
    QStringList libraryPaths;
    for (const QString &pluginPath : mengsheePluginPaths) {
        if (QDir(pluginPath).exists()) {
            libraryPaths.append(pluginPath);
        }
    }
    for (const QString &pluginPath : QCoreApplication::libraryPaths()) {
        if (!libraryPaths.contains(pluginPath)) {
            libraryPaths.append(pluginPath);
        }
    }
    QCoreApplication::setLibraryPaths(libraryPaths);
    if (qEnvironmentVariableIsSet("MENGSHEE_DEBUG_PLUGIN_PATHS")) {
        qDebug() << "Mengshee plugin search paths:" << QCoreApplication::libraryPaths();
    }

    const QString bundledLocaleDir = QDir(appDir).absoluteFilePath(QStringLiteral("data/locale"));
    if (QDir(bundledLocaleDir).exists()) {
        const QByteArray domains[] = {
            QByteArrayLiteral("okular"),
            QByteArrayLiteral("okular_poppler"),
            QByteArrayLiteral("kcolorscheme6"),
            QByteArrayLiteral("kconfigwidgets6"),
            QByteArrayLiteral("ki18n6"),
            QByteArrayLiteral("kiconthemes6"),
            QByteArrayLiteral("kio6"),
            QByteArrayLiteral("kparts6"),
            QByteArrayLiteral("kservice6"),
            QByteArrayLiteral("ktextwidgets6"),
            QByteArrayLiteral("kxmlgui6"),
        };
        for (const QByteArray &domain : domains) {
            KLocalizedString::addDomainLocaleDir(domain, bundledLocaleDir);
        }
    }

    /**
     * Install event filter to handle macOS file opening.
     * This enables double-click and "Open With" on macOS.
     */
    FileOpenEventHandler eventHandler;
    app.installEventFilter(&eventHandler);
    KLocalizedString::setApplicationDomain("okular");

#if HAVE_STYLE_MANAGER
    /**
     * trigger initialisation of proper application style
     */
    KStyleManager::initStyle();
#else
    /**
     * For Windows and macOS: use Breeze if available
     * Of all tested styles that works the best for us
     */
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    QApplication::setStyle(QStringLiteral("breeze"));
#endif
#endif

    KAboutData aboutData = okularAboutData();
    KAboutData::setApplicationData(aboutData);
    // set icon for shells which do not use desktop file metadata
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("mengshee"), QIcon::fromTheme(QStringLiteral("okular"))));

    KCrash::initialize();

    QCommandLineParser parser;
    // The KDE4 version accepted flags such as -unique with a single dash -> preserve compatibility
    parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
    aboutData.setupCommandLine(&parser);

    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("p") << QStringLiteral("page"), i18n("Page of the document to be shown"), QStringLiteral("number")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("presentation"), i18n("Start the document in presentation mode")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("print"), i18n("Start with print dialog")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("print-and-exit"), i18n("Start with print dialog and exit after printing")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("unique"), i18n("\"Unique instance\" control")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("noraise"), i18n("Not raise window")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("find"), i18n("Find a string on the text"), QStringLiteral("string")));
    parser.addOption(QCommandLineOption(QStringList() << QStringLiteral("editor-cmd"), i18n("Sets the external editor command"), QStringLiteral("string")));
    QCommandLineOption newProcessOption(QStringList() << QStringLiteral("new-process"), i18n("Open documents in a separate process"));
    newProcessOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(newProcessOption);
    parser.addPositionalArgument(QStringLiteral("urls"), i18n("Documents to open. Specify '-' to read from stdin."));

    parser.process(app);
    aboutData.processCommandLine(&parser);

    // see if we are starting with session management
    if (app.isSessionRestored()) {
        kRestoreMainWindows<Shell>();
    } else {
        // no session.. just start up normally
        QStringList paths;
        for (int i = 0; i < parser.positionalArguments().count(); ++i) {
            paths << parser.positionalArguments().at(i);
        }
        Okular::Status status = Okular::main(paths, ShellUtils::serializeOptions(parser), parser.isSet(newProcessOption));
        switch (status) {
        case Okular::Error:
            return -1;
        case Okular::AttachedOtherProcess:
            return 0;
        case Okular::Success:
            // Do nothing
            break;
        }
    }

    return app.exec();
}

int main(int argc, char **argv)
{
    try {
        return runMengsheeApplication(argc, argv);
    } catch (const std::exception &exception) {
        std::fprintf(stderr, "Mengshee stopped after an unhandled startup exception: %s\n", exception.what());
    } catch (...) {
        std::fputs("Mengshee stopped after an unknown startup exception.\n", stderr);
    }
    return EXIT_FAILURE;
}
#include "main.moc"
/* kate: replace-tabs on; indent-width 4; */
