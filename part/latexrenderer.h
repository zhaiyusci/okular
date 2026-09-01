/*
    SPDX-FileCopyrightText: 2004 Duncan Mac-Vicar Prett <duncan@kde.org>
    SPDX-FileCopyrightText: 2004-2005 Olivier Goffart <ogoffart@kde.org>
    SPDX-FileCopyrightText: 2011 Niels Ole Salscheider
    <niels_ole@salscheider-online.de>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef LATEXRENDERER_H
#define LATEXRENDERER_H

#include <QList>
#include <QString>
#include <QStringList>

class QString;
class QColor;

namespace GuiUtils
{
enum class LatexRenderWarningType { None, CompileError, BackendLimitation, CompileFallback };

struct LatexRenderWarning {
    LatexRenderWarningType type = LatexRenderWarningType::None;
    QString message;
    double severity = 0.0;
    bool isValid() const
    {
        return type != LatexRenderWarningType::None && !message.isEmpty();
    }
};

struct StemTeXStatus {
    bool supported = false;
    bool initializing = false;
    bool ready = false;
    bool primaryReady = false;
    int spareReady = 0;
    int spareTarget = 0;
    bool spareRebuilding = false;
    int rendererStatus = 0;
    int renderStage = 0;
    bool asyncRunning = false;
    bool asyncPending = false;
    quint64 runningJobId = 0;
    quint64 pendingJobId = 0;
    int lastError = 0;
    QString note;
};

struct StemTeXProfile {
    QString id;
    QString name;
    QString path;
    bool userManaged = false;
};

class LatexRenderer
{
public:
    enum Error { NoError, LatexFailed };

    LatexRenderer();
    ~LatexRenderer();

    LatexRenderer(const LatexRenderer &) = delete;
    LatexRenderer &operator=(const LatexRenderer &) = delete;

    Error renderLatexInHtml(QString &html, const QColor &textColor, int fontSize, int resolution, QString &latexOutput);
    Error renderLatexToImage(const QString &latexFormula, const QColor &textColor, int fontSize, int resolution, QString &fileName, QString &latexOutput);
    Error renderLatexToPdf(const QString &latexFormula, const QColor &textColor, QString &pdfFileName, QString &latexOutput, double maxWidth = 0.0, double fontSize = 0.0);
    QString lastBackendName() const;
    LatexRenderWarning lastWarning() const;
    QString lastWarningMessage() const;
    static bool mightContainLatex(const QString &text);
    static QString compactErrorMessage(const QString &latexOutput);
    static QList<StemTeXProfile> stemTeXProfiles();
    static QString stemTeXRuntimeRoot();
    static QString stemTeXUserProfilesRoot();
    static QString stemTeXProfileCreatorExecutable();
    static QString defaultStemTeXTexmfRoot();
    static void prewarmStemTeX();
    static void restartStemTeX();
    static StemTeXStatus stemTeXStatus();

private:
    QStringList m_fileList;
    QString m_lastBackendName;
    LatexRenderWarning m_lastWarning;
};

}

#endif // LATEXRENDERER_H
