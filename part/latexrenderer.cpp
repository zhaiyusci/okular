/*
    SPDX-FileCopyrightText: 2004 Duncan Mac-Vicar Prett <duncan@kde.org>
    SPDX-FileCopyrightText: 2004-2005 Olivier Goffart <ogoffart@kde.org>
    SPDX-FileCopyrightText: 2011 Niels Ole Salscheider
    <niels_ole@salscheider-online.de>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "latexrenderer.h"

#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <QDebug>

#include <KLocalizedString>

#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLibrary>
#include <QList>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include "gui/debug_ui.h"
#include "settings.h"

namespace GuiUtils
{
namespace
{
QString texInvocationLogPath()
{
    QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (logDir.isEmpty()) {
        logDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    }
    if (logDir.isEmpty()) {
        logDir = QDir::tempPath();
    }
    QDir().mkpath(logDir);
    return QDir(logDir).filePath(QStringLiteral("mengshee-tex-debug.log"));
}

void logTexInvocation(const char *operation, const QString &backend, const QString &reason, const QStringList &details = QStringList())
{
    if (!OkularUiDebug().isDebugEnabled()) {
        return;
    }

    QStringList fields = {QStringLiteral("Invoking TeX; operation: %1").arg(QLatin1String(operation)), QStringLiteral("backend: %1").arg(backend), QStringLiteral("reason: %1").arg(reason)};
    for (const QString &detail : details) {
        fields << detail;
    }
    const QString message = fields.join(QStringLiteral("; "));
    qCDebug(OkularUiDebug).noquote() << message;

    QFile logFile(texInvocationLogPath());
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&logFile);
        stream << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << " " << message << '\n';
    }
}

#ifdef Q_OS_WIN
struct StemTeXConfig {
    const char *repo_root_utf8;
    const char *runtime_root_utf8;
    const char *texmf_root_utf8;
    const char *profile_root_utf8;
    const char *state_root_utf8;
    const char *renders_root_utf8;
    int request_timeout_ms;
    int xdvipdfmx_timeout_ms;
    double min_width_pt;
    double max_width_pt;
    double default_width_pt;
    int spare_worker_count;
    int auto_restart;
    int delete_intermediates;
    const char *worker_template_utf8;
};

struct StemTeXRenderResult {
    char *request_id_utf8;
    char *pdf_path_utf8;
    char *summary_json_utf8;
    int outcome_code;
    int issue_flags;
    char *outcome_message_utf8;
};

static constexpr int StemTeXRenderOutcomeOk = 0;

struct StemTeXEngineSnapshot {
    int status;
    int stage;
    int primary_ready;
    int spare_ready;
    int spare_target;
    int spare_rebuilding;
    int async_running;
    int async_pending;
    uint64_t running_job_id;
    uint64_t pending_job_id;
    int last_error;
};

struct StemTeXProfileInfo {
    QString id;
    QString name;
    QString path;
    bool userManaged = false;
};

struct StemtexApi {
    using RenderCallback = void (*)(uint64_t, int, const StemTeXRenderResult *, int, const char *, void *);
    using Create = void *(*)(const StemTeXConfig *, int *, char **);
    using Render = int (*)(void *, const char *, double, StemTeXRenderResult *, int *, char **);
    using RenderAsync = int (*)(void *, const char *, double, uint64_t *, RenderCallback, void *, int *, char **);
    using RenderAsyncWithFontSize = int (*)(void *, const char *, double, double, uint64_t *, RenderCallback, void *, int *, char **);
    using EngineSnapshot = int (*)(void *, StemTeXEngineSnapshot *);
    using ProfileInfo = char *(*)(const char *, int *, char **);
    using ValidateConfig = int (*)(const StemTeXConfig *, int *, char **);
    using FreeResult = void (*)(StemTeXRenderResult *);
    using FreeString = void (*)(char *);
    using Destroy = void (*)(void *);

    QLibrary library;
    Create create = nullptr;
    Render render = nullptr;
    RenderAsync renderAsync = nullptr;
    RenderAsyncWithFontSize renderAsyncWithFontSize = nullptr;
    EngineSnapshot engineSnapshot = nullptr;
    ProfileInfo profileInfo = nullptr;
    ValidateConfig validateConfig = nullptr;
    FreeResult freeResult = nullptr;
    FreeString freeString = nullptr;
    Destroy destroy = nullptr;

    explicit StemtexApi(const QString &dllPath)
        : library(dllPath)
    {
    }

    bool load(QString *error)
    {
        if (!library.load()) {
            if (error) {
                *error = library.errorString();
            }
            return false;
        }

        create = reinterpret_cast<Create>(library.resolve("stemtex_renderer_create"));
        render = reinterpret_cast<Render>(library.resolve("stemtex_renderer_render"));
        renderAsync = reinterpret_cast<RenderAsync>(library.resolve("stemtex_renderer_render_async"));
        renderAsyncWithFontSize = reinterpret_cast<RenderAsyncWithFontSize>(library.resolve("stemtex_renderer_render_async_with_font_size"));
        engineSnapshot = reinterpret_cast<EngineSnapshot>(library.resolve("stemtex_renderer_engine_snapshot"));
        profileInfo = reinterpret_cast<ProfileInfo>(library.resolve("stemtex_renderer_profile_info_json"));
        validateConfig = reinterpret_cast<ValidateConfig>(library.resolve("stemtex_renderer_validate_config"));
        freeResult = reinterpret_cast<FreeResult>(library.resolve("stemtex_renderer_free_result"));
        freeString = reinterpret_cast<FreeString>(library.resolve("stemtex_renderer_free_string"));
        destroy = reinterpret_cast<Destroy>(library.resolve("stemtex_renderer_destroy"));
        const bool ok = create && render && renderAsync && engineSnapshot && profileInfo && validateConfig && freeResult && freeString && destroy;
        if (!ok && error) {
            *error = QStringLiteral("stemtex-renderer.dll does not export the expected renderer ABI.");
        }
        return ok;
    }
};

class StemtexRendererSession
{
public:
    ~StemtexRendererSession()
    {
        if (m_renderer && m_api.destroy) {
            try {
                m_api.destroy(m_renderer);
            } catch (const std::exception &exception) {
                qCCritical(OkularUiDebug) << "StemTeX renderer destruction failed:" << exception.what();
            } catch (...) {
                qCCritical(OkularUiDebug) << "StemTeX renderer destruction failed with an unknown exception";
            }
        }
    }

    static void prewarm()
    {
        (void)sharedSession(false, false, nullptr);
    }

    static void reset()
    {
        std::shared_ptr<StemtexRendererSession> oldSession;
        SharedState &state = sharedState();
        {
            std::lock_guard<std::mutex> guard(state.mutex);
            ++state.generation;
            oldSession = std::move(state.session);
            state.initializing = false;
            state.attempted = false;
            state.lastError.clear();
        }
        state.condition.notify_all();
    }

    static std::shared_ptr<StemtexRendererSession> instance(QString *error, bool waitForInitialization = false)
    {
        return sharedSession(true, waitForInitialization, error);
    }

    static StemTeXStatus sharedStatus()
    {
        StemTeXStatus status;
        status.supported = true;
        SharedState &state = sharedState();
        std::shared_ptr<StemtexRendererSession> session;
        {
            std::lock_guard<std::mutex> guard(state.mutex);
            session = state.session;
            status.initializing = state.initializing;
            if (!state.lastError.isEmpty()) {
                status.note = state.lastError;
            } else if (state.initializing) {
                status.note = i18n("StemTeX renderer is starting.");
            } else if (!state.attempted) {
                status.note = i18n("StemTeX renderer has not started yet.");
            }
        }

        if (session) {
            return session->status();
        }
        return status;
    }

    static QList<StemTeXProfile> availableProfileRecords()
    {
        const QString runtime = runtimeRoot();
        const QString dllPath = rendererDllPath(runtime);
        if (!QFileInfo::exists(dllPath)) {
            return {};
        }

        configureRendererDllSearch(runtime);
        StemtexApi api(dllPath);
        QString error;
        if (!api.load(&error)) {
            return {};
        }

        QList<StemTeXProfile> records;
        QString lastError;
        const QList<StemTeXProfileInfo> profiles = availableProfiles(api, &lastError);
        for (const StemTeXProfileInfo &profile : profiles) {
            records << StemTeXProfile {profile.id, profile.name, profile.path, profile.userManaged};
        }
        return records;
    }

    static QString defaultTexmfRoot()
    {
        return runtimeRoot();
    }

    static QString resolvedRuntimeRoot()
    {
        return runtimeRoot();
    }

    static QString managedUserProfilesRoot()
    {
        return userProfilesRoot();
    }

    LatexRenderer::Error render(const QString &latexSource, const QColor &textColor, double maxWidth, double fontSize, QString &pdfFileName, QString &latexOutput, QStringList &fileList, LatexRenderWarning *warning)
    {
        if (!m_renderer) {
            latexOutput = i18n("StemTeX renderer is not initialized.");
            return LatexRenderer::LatexFailed;
        }

        const double widthPt = std::isfinite(maxWidth) && maxWidth > 0.0 ? maxWidth : 360.0;
        const QColor effectiveColor = textColor.isValid() ? textColor : QColor(Qt::black);
        const QString coloredSource = QStringLiteral("{\\color[rgb]{%1,%2,%3}\n%4\n\\par}").arg(effectiveColor.redF(), 0, 'f', 6).arg(effectiveColor.greenF(), 0, 'f', 6).arg(effectiveColor.blueF(), 0, 'f', 6).arg(latexSource);
        const QByteArray snippet = coloredSource.toUtf8();

        struct RenderState {
            std::mutex mutex;
            std::condition_variable condition;
            bool done = false;
            int ok = 0;
            int errorCode = 0;
            uint64_t jobId = 0;
            uint64_t callbackJobId = 0;
            QString errorText;
            QString sourcePdfFile;
            QString summary;
            int outcomeCode = StemTeXRenderOutcomeOk;
            QString outcomeMessage;
        };
        auto state = std::make_shared<RenderState>();

        const auto callback = [](uint64_t jobId, int ok, const StemTeXRenderResult *result, int errorCode, const char *error, void *userData) noexcept {
            auto *state = static_cast<RenderState *>(userData);
            if (!state) {
                return;
            }

            try {
                QString errorText = error ? QString::fromUtf8(error) : QString();
                QString sourcePdfFile = result && result->pdf_path_utf8 ? QString::fromUtf8(result->pdf_path_utf8) : QString();
                QString summary = result && result->summary_json_utf8 ? QString::fromUtf8(result->summary_json_utf8) : QString();
                QString outcomeMessage = result && result->outcome_message_utf8 ? QString::fromUtf8(result->outcome_message_utf8) : QString();

                std::lock_guard<std::mutex> guard(state->mutex);
                state->callbackJobId = jobId;
                state->ok = ok;
                state->errorCode = errorCode;
                state->errorText = std::move(errorText);
                state->sourcePdfFile = std::move(sourcePdfFile);
                state->summary = std::move(summary);
                state->outcomeCode = result ? result->outcome_code : errorCode;
                state->outcomeMessage = std::move(outcomeMessage);
                state->done = true;
                state->condition.notify_all();
            } catch (const std::exception &exception) {
                try {
                    std::lock_guard<std::mutex> guard(state->mutex);
                    state->callbackJobId = jobId;
                    state->ok = 0;
                    state->errorCode = errorCode;
                    state->errorText = QStringLiteral("StemTeX callback failed while receiving the render result.");
                    state->done = true;
                    state->condition.notify_all();
                    qCCritical(OkularUiDebug) << "StemTeX render callback failed:" << exception.what();
                } catch (...) {
                    qCCritical(OkularUiDebug) << "StemTeX render callback could not report its failure";
                }
            } catch (...) {
                try {
                    std::lock_guard<std::mutex> guard(state->mutex);
                    state->callbackJobId = jobId;
                    state->ok = 0;
                    state->errorCode = errorCode;
                    state->errorText = QStringLiteral("StemTeX callback failed with an unknown exception.");
                    state->done = true;
                    state->condition.notify_all();
                } catch (...) {
                    qCCritical(OkularUiDebug) << "StemTeX render callback could not report an unknown failure";
                }
            }
        };

        int submitErrorCode = 0;
        char *submitError = nullptr;
        uint64_t jobId = 0;
        int submitted = 0;
        try {
            if (std::isfinite(fontSize) && fontSize > 0.0) {
                if (!m_api.renderAsyncWithFontSize) {
                    latexOutput = i18n("The installed StemTeX renderer does not support configurable font sizes.");
                    return LatexRenderer::LatexFailed;
                }
                submitted = m_api.renderAsyncWithFontSize(m_renderer, snippet.constData(), widthPt, fontSize, &jobId, callback, state.get(), &submitErrorCode, &submitError);
            } else {
                submitted = m_api.renderAsync(m_renderer, snippet.constData(), widthPt, &jobId, callback, state.get(), &submitErrorCode, &submitError);
            }
        } catch (const std::exception &exception) {
            latexOutput = i18n("StemTeX rendering failed unexpectedly: %1", QString::fromLocal8Bit(exception.what()));
            return LatexRenderer::LatexFailed;
        } catch (...) {
            latexOutput = i18n("StemTeX rendering failed because of an unknown internal error.");
            return LatexRenderer::LatexFailed;
        }
        {
            std::lock_guard<std::mutex> guard(state->mutex);
            state->jobId = jobId;
        }
        if (!submitted) {
            const QString errorText = submitError ? QString::fromUtf8(submitError) : QString();
            if (submitError) {
                m_api.freeString(submitError);
            }
            latexOutput = i18n("StemTeX rendering failed: %1", errorText.isEmpty() ? QString::number(submitErrorCode) : errorText);
            return LatexRenderer::LatexFailed;
        }
        if (submitError) {
            m_api.freeString(submitError);
        }

        {
            std::unique_lock<std::mutex> guard(state->mutex);
            state->condition.wait(guard, [&state]() { return state->done; });
        }

        int ok = 0;
        int errorCode = 0;
        uint64_t callbackJobId = 0;
        QString errorText;
        QString sourcePdfFile;
        QString summary;
        int outcomeCode = StemTeXRenderOutcomeOk;
        QString outcomeMessage;
        {
            std::lock_guard<std::mutex> guard(state->mutex);
            ok = state->ok;
            errorCode = state->errorCode;
            callbackJobId = state->callbackJobId;
            errorText = state->errorText;
            sourcePdfFile = state->sourcePdfFile;
            summary = state->summary;
            outcomeCode = state->outcomeCode;
            outcomeMessage = state->outcomeMessage;
        }

        if (callbackJobId != jobId) {
            latexOutput = i18n("StemTeX rendering failed: stale async job result.");
            return LatexRenderer::LatexFailed;
        }
        if (!ok || sourcePdfFile.isEmpty() || !QFileInfo::exists(sourcePdfFile)) {
            latexOutput = i18n("StemTeX rendering failed: %1", errorText.isEmpty() ? QString::number(errorCode) : errorText);
            return LatexRenderer::LatexFailed;
        }

        latexOutput = summary;
        pdfFileName = sourcePdfFile;
        fileList << sourcePdfFile;
        if (warning && outcomeCode != StemTeXRenderOutcomeOk) {
            warning->type = LatexRenderWarningType::CompileError;
            warning->message = outcomeMessage.trimmed();
            if (warning->message.isEmpty()) {
                warning->message = i18n("StemTeX returned code %1. The rendered PDF is still shown.", outcomeCode);
            }
            warning->severity = 1.0;
        }
        qCDebug(OkularUiDebug) << "StemTeX render finished; PDF:" << pdfFileName << "outcome:" << outcomeCode << "message:" << outcomeMessage << "summary:" << summary;
        return LatexRenderer::NoError;
    }

private:
    struct SharedState {
        std::mutex mutex;
        std::condition_variable condition;
        std::shared_ptr<StemtexRendererSession> session;
        uint64_t generation = 0;
        bool initializing = false;
        bool attempted = false;
        QString lastError;
    };

    StemtexRendererSession(QString runtimeRoot, QString dllPath)
        : m_runtimeRoot(std::move(runtimeRoot))
        , m_dllPath(std::move(dllPath))
        , m_api(m_dllPath)
    {
    }

    static std::shared_ptr<StemtexRendererSession> sharedSession(bool reportStarting, bool waitForInitialization, QString *error)
    {
        SharedState &state = sharedState();
        bool startInitialization = false;
        uint64_t generation = 0;

        {
            std::unique_lock<std::mutex> guard(state.mutex);
            if (state.session) {
                return state.session;
            }
            if (!state.initializing && !state.attempted) {
                state.initializing = true;
                startInitialization = true;
                generation = state.generation;
            } else if (state.initializing && !waitForInitialization) {
                if (error && reportStarting) {
                    *error = i18n("StemTeX renderer is still starting.");
                }
                return nullptr;
            } else if (state.attempted) {
                if (error) {
                    *error = state.lastError;
                }
                return nullptr;
            }
        }

        if (startInitialization) {
            try {
                std::thread([generation]() noexcept {
                    try {
                        QString initError;
                        std::shared_ptr<StemtexRendererSession> created;
                        bool ok = false;
                        try {
                            const QString runtime = runtimeRoot();
                            created = std::shared_ptr<StemtexRendererSession>(new StemtexRendererSession(runtime, rendererDllPath(runtime)));
                            ok = created->initialize(&initError);
                        } catch (const std::exception &exception) {
                            initError = i18n("StemTeX renderer initialization failed unexpectedly: %1", QString::fromLocal8Bit(exception.what()));
                            qCCritical(OkularUiDebug) << initError;
                        } catch (...) {
                            initError = i18n("StemTeX renderer initialization failed because of an unknown internal error.");
                            qCCritical(OkularUiDebug) << initError;
                        }

                        SharedState &state = sharedState();
                        {
                            std::lock_guard<std::mutex> guard(state.mutex);
                            if (generation == state.generation) {
                                if (ok) {
                                    state.session = std::move(created);
                                    state.lastError.clear();
                                } else {
                                    state.lastError = initError;
                                }
                                state.initializing = false;
                                state.attempted = true;
                            }
                        }
                        state.condition.notify_all();
                    } catch (const std::exception &exception) {
                        qCCritical(OkularUiDebug) << "StemTeX initialization worker failed:" << exception.what();
                        try {
                            SharedState &state = sharedState();
                            {
                                std::lock_guard<std::mutex> guard(state.mutex);
                                if (generation == state.generation) {
                                    state.initializing = false;
                                    state.attempted = true;
                                    state.lastError = QStringLiteral("StemTeX renderer initialization failed unexpectedly.");
                                }
                            }
                            state.condition.notify_all();
                        } catch (...) {
                        }
                    } catch (...) {
                        qCCritical(OkularUiDebug) << "StemTeX initialization worker failed with an unknown exception";
                        try {
                            SharedState &state = sharedState();
                            {
                                std::lock_guard<std::mutex> guard(state.mutex);
                                if (generation == state.generation) {
                                    state.initializing = false;
                                    state.attempted = true;
                                    state.lastError = QStringLiteral("StemTeX renderer initialization failed with an unknown error.");
                                }
                            }
                            state.condition.notify_all();
                        } catch (...) {
                        }
                    }
                }).detach();
            } catch (const std::exception &exception) {
                std::lock_guard<std::mutex> guard(state.mutex);
                state.initializing = false;
                state.attempted = true;
                state.lastError = i18n("Could not start the StemTeX initialization worker: %1", QString::fromLocal8Bit(exception.what()));
                state.condition.notify_all();
            } catch (...) {
                std::lock_guard<std::mutex> guard(state.mutex);
                state.initializing = false;
                state.attempted = true;
                state.lastError = i18n("Could not start the StemTeX initialization worker because of an unknown internal error.");
                state.condition.notify_all();
            }
        }

        if (waitForInitialization) {
            std::unique_lock<std::mutex> guard(state.mutex);
            state.condition.wait(guard, [&state]() { return !state.initializing; });
            if (state.session) {
                return state.session;
            }
            if (error) {
                *error = state.lastError;
            }
            return nullptr;
        }

        if (error && reportStarting) {
            *error = i18n("StemTeX renderer is starting.");
        }
        return nullptr;
    }

    static SharedState &sharedState()
    {
        // Initialization runs on a detached worker. Keep its synchronization
        // state alive until process termination to avoid static-destruction races.
        static SharedState *const state = new SharedState;
        return *state;
    }

    static QString environmentPath(const char *name)
    {
        const QString value = QString::fromLocal8Bit(qgetenv(name)).trimmed();
        if (value.isEmpty()) {
            return {};
        }
        return QDir::cleanPath(QDir(value).absolutePath());
    }

    static QString normalizeRuntimeRoot(const QString &path)
    {
        QDir dir(QDir::cleanPath(QDir(path).absolutePath()));
        const QString nestedRuntime = dir.filePath(QStringLiteral("runtime"));
        if (QFileInfo::exists(QDir(nestedRuntime).filePath(QStringLiteral("bin/windows/xetexdaemon.exe")))) {
            return QDir::cleanPath(nestedRuntime);
        }
        return QDir::cleanPath(dir.absolutePath());
    }

    static QString runtimeRoot()
    {
        const QString envRuntime = environmentPath("MENGSHEE_STEMTEX_RUNTIME_ROOT");
        if (!envRuntime.isEmpty()) {
            return normalizeRuntimeRoot(envRuntime);
        }

        return normalizeRuntimeRoot(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../StemTeX/runtime")));
    }

    static QString missingRuntimeComponent(const QString &runtimeRoot)
    {
        const QDir runtimeDir(runtimeRoot);
        for (const QString &relativePath : {
                 QStringLiteral("run-xelatexdaemon.bat"),
                 QStringLiteral("bin/windows/stemtex-worker-host.exe"),
                 QStringLiteral("bin/windows/xetexdaemon.exe"),
                 QStringLiteral("bin/windows/xdvipdfmxdaemon.exe"),
                 QStringLiteral("bin/windows/dvipdfmxdaemon.dll"),
                 QStringLiteral("bin/windows/dvisvgmdaemon.exe"),
                 QStringLiteral("bin/windows/dvisvgmdaemon.dll"),
             }) {
            if (!QFileInfo::exists(runtimeDir.filePath(relativePath))) {
                return relativePath;
            }
        }
        return {};
    }

    static QString texmfRoot(const QString &runtimeRoot)
    {
        const QString envTexmf = environmentPath("MENGSHEE_STEMTEX_TEXMF_ROOT");
        if (!envTexmf.isEmpty()) {
            return envTexmf;
        }
        const QString configuredTexmf = Okular::Settings::latexStemtexTexmfRoot().trimmed();
        return configuredTexmf.isEmpty() ? runtimeRoot : QDir::cleanPath(QDir(configuredTexmf).absolutePath());
    }

    static QString bundledProfilesRoot()
    {
        const QString envProfiles = environmentPath("MENGSHEE_STEMTEX_PROFILES_ROOT");
        if (!envProfiles.isEmpty()) {
            return envProfiles;
        }
        return QDir::cleanPath(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../StemTeX/gui/profiles")));
    }

    static QString writableStemTeXRoot()
    {
        QString data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (data.isEmpty()) {
            data = QDir(QDir::tempPath()).filePath(QStringLiteral("mengshee"));
        }
        return QDir::cleanPath(QDir(data).filePath(QStringLiteral("StemTeX")));
    }

    static QString userProfilesRoot()
    {
        const QString envProfiles = environmentPath("MENGSHEE_STEMTEX_USER_PROFILES_ROOT");
        if (!envProfiles.isEmpty()) {
            return envProfiles;
        }
        return QDir(writableStemTeXRoot()).filePath(QStringLiteral("profiles"));
    }

    static QString writableStemTeXTempRoot()
    {
        return QDir(writableStemTeXRoot()).filePath(QStringLiteral("temporary"));
    }

    static bool readProfileInfo(const StemtexApi &api, const QString &profileRoot, const QString &idPrefix, bool userManaged, StemTeXProfileInfo *profileInfo, QString *error)
    {
        if (profileRoot.isEmpty()) {
            if (error) {
                *error = i18n("StemTeX profile path is empty.");
            }
            return false;
        }

        QByteArray profile = QDir::cleanPath(profileRoot).toUtf8();
        int errorCode = 0;
        char *errorUtf8 = nullptr;
        char *json = api.profileInfo(profile.constData(), &errorCode, &errorUtf8);
        const QString errorText = errorUtf8 ? QString::fromUtf8(errorUtf8) : QString();
        QByteArray jsonBytes;
        if (json) {
            jsonBytes = QByteArray(json);
            api.freeString(json);
        }
        if (errorUtf8) {
            api.freeString(errorUtf8);
        }
        if (!json) {
            if (error) {
                *error = i18n("StemTeX profile is not valid: %1", errorText.isEmpty() ? QString::number(errorCode) : errorText);
            }
            return false;
        }

        QJsonParseError parseError {};
        const QJsonDocument document = QJsonDocument::fromJson(jsonBytes, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            if (error) {
                *error = i18n("StemTeX profile metadata is not valid JSON: %1", parseError.errorString());
            }
            return false;
        }
        if (!document.object().value(QLatin1String("valid")).toBool()) {
            if (error) {
                *error = i18n("StemTeX profile is not valid: %1", QDir::toNativeSeparators(profileRoot));
            }
            return false;
        }
        const QJsonObject object = document.object();
        QString path = object.value(QLatin1String("path")).toString();
        if (path.isEmpty()) {
            path = QDir::cleanPath(profileRoot);
        }
        QString name = object.value(QLatin1String("name")).toString();
        if (name.isEmpty()) {
            name = QFileInfo(path).fileName();
        }
        if (profileInfo) {
            profileInfo->id = idPrefix + QLatin1Char(':') + QFileInfo(path).fileName();
            profileInfo->name = name;
            profileInfo->path = QDir::cleanPath(path);
            profileInfo->userManaged = userManaged;
        }
        return true;
    }

    static QList<StemTeXProfileInfo> availableProfiles(const StemtexApi &api, QString *lastError)
    {
        QList<StemTeXProfileInfo> profiles;
        QSet<QString> seenPaths;
        QStringList missingRoots;
        const auto appendProfiles = [&](const QString &root, const QString &idPrefix, bool userManaged) {
            const QDir profilesDir(root);
            if (!profilesDir.exists()) {
                missingRoots << QDir::toNativeSeparators(root);
                return;
            }

            const QFileInfoList candidates = profilesDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const QFileInfo &candidate : candidates) {
                const QString path = QDir::cleanPath(candidate.absoluteFilePath());
                if (seenPaths.contains(path)) {
                    continue;
                }
                StemTeXProfileInfo profile;
                if (readProfileInfo(api, path, idPrefix, userManaged, &profile, lastError)) {
                    profiles << profile;
                    seenPaths.insert(path);
                }
            }
        };

        appendProfiles(bundledProfilesRoot(), QStringLiteral("bundled"), false);
        appendProfiles(userProfilesRoot(), QStringLiteral("user"), true);
        if (profiles.isEmpty() && lastError && !missingRoots.isEmpty()) {
            *lastError = i18n("StemTeX profile directories were not found: %1", missingRoots.join(QStringLiteral(", ")));
        }
        return profiles;
    }

    static QString selectProfileRoot(const StemtexApi &api, QString *error)
    {
        const QString explicitProfileRoot = environmentPath("MENGSHEE_STEMTEX_PROFILE_ROOT");
        if (!explicitProfileRoot.isEmpty()) {
            StemTeXProfileInfo explicitProfile;
            return readProfileInfo(api, explicitProfileRoot, QStringLiteral("external"), false, &explicitProfile, error) ? explicitProfile.path : QString();
        }

        QStringList preferredNames;
        const QString envProfileName = QString::fromLocal8Bit(qgetenv("MENGSHEE_STEMTEX_PROFILE_NAME")).trimmed();
        const QString configuredProfileName = Okular::Settings::latexStemtexProfileName().trimmed();
        if (!envProfileName.isEmpty()) {
            preferredNames << envProfileName;
        } else if (!configuredProfileName.isEmpty()) {
            preferredNames << configuredProfileName;
        }
        preferredNames << QStringLiteral("bundled:unicodemath") << QStringLiteral("unicodemath");
        preferredNames.removeDuplicates();

        QString lastError;
        const QList<StemTeXProfileInfo> profiles = availableProfiles(api, &lastError);
        for (const QString &name : preferredNames) {
            for (const StemTeXProfileInfo &profile : profiles) {
                if (profile.id.compare(name, Qt::CaseInsensitive) == 0 || profile.name.compare(name, Qt::CaseInsensitive) == 0 || QFileInfo(profile.path).fileName().compare(name, Qt::CaseInsensitive) == 0) {
                    return profile.path;
                }
            }
        }
        if (!profiles.isEmpty()) {
            return profiles.first().path;
        }

        if (error) {
            *error = lastError.isEmpty() ? i18n("No valid bundled or user StemTeX profile was found.") : lastError;
        }
        return {};
    }

    static QString rendererDllPath(const QString &runtimeRoot)
    {
        const QString envDll = QString::fromLocal8Bit(qgetenv("STEMTEX_RENDERER_DLL")).trimmed();
        if (!envDll.isEmpty()) {
            return QDir::cleanPath(envDll);
        }
        return QDir(runtimeRoot).filePath(QStringLiteral("bin/sdk/stemtex-renderer.dll"));
    }

    static void configureRendererDllSearch(const QString &runtimeRoot)
    {
        const QString appDir = QCoreApplication::applicationDirPath();
        const QString sdkDir = QDir(runtimeRoot).filePath(QStringLiteral("bin/sdk"));
        const std::wstring appDirWide = QDir::toNativeSeparators(QDir::cleanPath(appDir)).toStdWString();
        const std::wstring sdkDirWide = QDir::toNativeSeparators(QDir::cleanPath(sdkDir)).toStdWString();
        SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
        AddDllDirectory(appDirWide.c_str());
        AddDllDirectory(sdkDirWide.c_str());
    }

    bool initialize(QString *error)
    {
        if (!QFileInfo::exists(m_dllPath)) {
            if (error) {
                *error = i18n("StemTeX renderer DLL was not found: %1", QDir::toNativeSeparators(m_dllPath));
            }
            return false;
        }
        const QString missingComponent = missingRuntimeComponent(m_runtimeRoot);
        if (!missingComponent.isEmpty()) {
            if (error) {
                *error = i18n("StemTeX runtime is incomplete: missing %1 under %2", QDir::toNativeSeparators(missingComponent), QDir::toNativeSeparators(m_runtimeRoot));
            }
            return false;
        }
        configureRendererDllSearch(m_runtimeRoot);
        if (!m_api.load(error)) {
            return false;
        }

        QString profileError;
        const QString profile = selectProfileRoot(m_api, &profileError);
        if (profile.isEmpty()) {
            if (error) {
                *error = profileError;
            }
            return false;
        }

        const QByteArray runtime = QDir::cleanPath(m_runtimeRoot).toUtf8();
        const QString texmfRootPath = texmfRoot(m_runtimeRoot);
        const QByteArray texmf = QDir::cleanPath(texmfRootPath).toUtf8();
        const QByteArray profileRoot = QDir::cleanPath(profile).toUtf8();
        const QString tempRoot = writableStemTeXTempRoot();
        const QByteArray stateRoot = QDir(tempRoot).filePath(QStringLiteral("state")).toUtf8();
        const QByteArray rendersRoot = QDir(tempRoot).filePath(QStringLiteral("renders")).toUtf8();
        StemTeXConfig config {};
        config.repo_root_utf8 = runtime.constData();
        config.runtime_root_utf8 = runtime.constData();
        config.texmf_root_utf8 = texmf.constData();
        config.profile_root_utf8 = profileRoot.constData();
        config.state_root_utf8 = stateRoot.constData();
        config.renders_root_utf8 = rendersRoot.constData();
        config.request_timeout_ms = 90000;
        config.xdvipdfmx_timeout_ms = 90000;
        config.spare_worker_count = 0;
        config.delete_intermediates = 1;

        int errorCode = 0;
        char *errorUtf8 = nullptr;
        char *diagnosticsUtf8 = nullptr;
        if (!m_api.validateConfig(&config, &errorCode, &diagnosticsUtf8)) {
            const QString diagnostics = diagnosticsUtf8 ? QString::fromUtf8(diagnosticsUtf8).trimmed() : QString();
            if (diagnosticsUtf8) {
                m_api.freeString(diagnosticsUtf8);
            }
            if (error) {
                *error = i18n("StemTeX renderer configuration is invalid: %1", diagnostics.isEmpty() ? QString::number(errorCode) : diagnostics);
            }
            return false;
        }
        if (diagnosticsUtf8) {
            m_api.freeString(diagnosticsUtf8);
        }

        m_renderer = m_api.create(&config, &errorCode, &errorUtf8);
        const QString errorText = errorUtf8 ? QString::fromUtf8(errorUtf8) : QString();
        if (errorUtf8) {
            m_api.freeString(errorUtf8);
        }
        if (!m_renderer) {
            if (error) {
                *error = i18n("StemTeX renderer failed to start: %1", errorText.isEmpty() ? QString::number(errorCode) : errorText);
            }
            return false;
        }

        qCDebug(OkularUiDebug) << "StemTeX renderer initialized; runtime:" << m_runtimeRoot << "texmf:" << texmfRootPath << "profile:" << profile << "dll:" << m_dllPath;
        return true;
    }

    StemTeXStatus status()
    {
        StemTeXStatus status;
        status.supported = true;
        if (!m_renderer) {
            status.note = i18n("StemTeX renderer is not initialized.");
            return status;
        }

        StemTeXEngineSnapshot snapshot {};
        int snapshotAvailable = 0;
        try {
            snapshotAvailable = m_api.engineSnapshot(m_renderer, &snapshot);
        } catch (const std::exception &exception) {
            status.note = i18n("StemTeX renderer status failed unexpectedly: %1", QString::fromLocal8Bit(exception.what()));
            qCCritical(OkularUiDebug) << status.note;
            return status;
        } catch (...) {
            status.note = i18n("StemTeX renderer status failed because of an unknown internal error.");
            qCCritical(OkularUiDebug) << status.note;
            return status;
        }
        if (!snapshotAvailable) {
            status.ready = true;
            status.note = i18n("StemTeX renderer status is unavailable.");
            return status;
        }

        status.ready = true;
        status.rendererStatus = snapshot.status;
        status.renderStage = snapshot.stage;
        status.primaryReady = snapshot.primary_ready != 0;
        status.spareReady = qMax(0, snapshot.spare_ready);
        status.spareTarget = qMax(0, snapshot.spare_target);
        status.spareRebuilding = snapshot.spare_rebuilding != 0;
        status.asyncRunning = snapshot.async_running != 0;
        status.asyncPending = snapshot.async_pending != 0;
        status.runningJobId = snapshot.running_job_id;
        status.pendingJobId = snapshot.pending_job_id;
        status.lastError = snapshot.last_error;
        return status;
    }

    QString m_runtimeRoot;
    QString m_dllPath;
    StemtexApi m_api;
    void *m_renderer = nullptr;
};
#endif
LatexRenderWarning latexWarningMessage(const QString &latexOutput)
{
    Q_UNUSED(latexOutput);
    return {};
}

}

LatexRenderer::LatexRenderer()
{
}

LatexRenderer::~LatexRenderer()
{
    for (const QString &file : std::as_const(m_fileList)) {
        QFile::remove(file);
    }
}

QString LatexRenderer::lastBackendName() const
{
    return m_lastBackendName;
}

LatexRenderWarning LatexRenderer::lastWarning() const
{
    return m_lastWarning;
}

QString LatexRenderer::lastWarningMessage() const
{
    return m_lastWarning.message;
}

LatexRenderer::Error LatexRenderer::renderLatexInHtml(QString &html, const QColor &textColor, int fontSize, int resolution, QString &latexOutput)
{
    Q_UNUSED(textColor);
    Q_UNUSED(fontSize);
    Q_UNUSED(resolution);

    m_lastBackendName.clear();
    m_lastWarning = {};

    if (!html.contains(QStringLiteral("$$"))) {
        return NoError;
    }

    latexOutput = i18n("Legacy popup formula rendering has been removed. Use %1 LaTeX notes backed by StemTeX.", QStringLiteral("Mengshee"));
    return LatexFailed;
}

bool LatexRenderer::mightContainLatex(const QString &text)
{
    Q_UNUSED(text);
    return false;
}

QString LatexRenderer::compactErrorMessage(const QString &latexOutput)
{
    const QStringList lines = latexOutput.split(QLatin1Char('\n'));
    QString message;
    QString context;
    bool foundErrorLine = false;

    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        if (!foundErrorLine && trimmed.startsWith(QLatin1Char('!'))) {
            message = trimmed.mid(1).trimmed();
            foundErrorLine = true;
            continue;
        }
        if (foundErrorLine && trimmed.startsWith(QLatin1String("l."))) {
            context = trimmed;
            break;
        }
        if (message.isEmpty()) {
            message = trimmed;
        }
    }

    if (message.isEmpty()) {
        message = i18n("Unknown LaTeX error.");
    }
    if (!context.isEmpty()) {
        message = i18nc("LaTeX error with compiler context", "%1 (%2)", message, context);
    }

    message = message.simplified();
    constexpr int maxLength = 180;
    if (message.size() > maxLength) {
        message = message.left(maxLength - 3) + QStringLiteral("...");
    }
    return message;
}

void LatexRenderer::prewarmStemTeX()
{
#ifdef Q_OS_WIN
    StemtexRendererSession::prewarm();
#endif
}

QList<StemTeXProfile> LatexRenderer::stemTeXProfiles()
{
#ifdef Q_OS_WIN
    return StemtexRendererSession::availableProfileRecords();
#else
    return {};
#endif
}

QString LatexRenderer::stemTeXRuntimeRoot()
{
#ifdef Q_OS_WIN
    return StemtexRendererSession::resolvedRuntimeRoot();
#else
    return {};
#endif
}

QString LatexRenderer::stemTeXUserProfilesRoot()
{
#ifdef Q_OS_WIN
    return StemtexRendererSession::managedUserProfilesRoot();
#else
    return {};
#endif
}

QString LatexRenderer::stemTeXProfileCreatorExecutable()
{
#ifdef Q_OS_WIN
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("stemtex-profile-creator.exe"));
#else
    return {};
#endif
}

QString LatexRenderer::defaultStemTeXTexmfRoot()
{
#ifdef Q_OS_WIN
    return StemtexRendererSession::defaultTexmfRoot();
#else
    return {};
#endif
}

void LatexRenderer::restartStemTeX()
{
#ifdef Q_OS_WIN
    StemtexRendererSession::reset();
    StemtexRendererSession::prewarm();
#endif
}

StemTeXStatus LatexRenderer::stemTeXStatus()
{
#ifdef Q_OS_WIN
    return StemtexRendererSession::sharedStatus();
#else
    StemTeXStatus status;
    status.note = i18n("StemTeX renderer is only available on Windows.");
    return status;
#endif
}

LatexRenderer::Error LatexRenderer::renderLatexToImage(const QString &latexFormula, const QColor &textColor, int fontSize, int resolution, QString &fileName, QString &latexOutput)
{
    Q_UNUSED(latexFormula);
    Q_UNUSED(textColor);
    Q_UNUSED(fontSize);
    Q_UNUSED(resolution);

    m_lastBackendName.clear();
    m_lastWarning = {};
    fileName.clear();
    latexOutput = i18n("Legacy LaTeX image rendering has been removed. Use %1 LaTeX notes backed by StemTeX.", QStringLiteral("Mengshee"));
    return LatexFailed;
}

LatexRenderer::Error LatexRenderer::renderLatexToPdf(const QString &latexFormula, const QColor &textColor, QString &pdfFileName, QString &latexOutput, double maxWidth, double fontSize)
{
    m_lastBackendName.clear();
    m_lastWarning = {};

    QString formula = latexFormula.trimmed();
    if (formula.isEmpty()) {
        pdfFileName.clear();
        return LatexFailed;
    }
#ifdef Q_OS_WIN
    logTexInvocation("stemtex-render",
                     QStringLiteral("stemtex"),
                     QStringLiteral("configured-stemtex"),
                     {QStringLiteral("max width: %1").arg(maxWidth), QStringLiteral("font size: %1").arg(fontSize), QStringLiteral("source length: %1").arg(formula.size())});
    QString stemtexError;
    const bool waitForStemTeXStartup = QCoreApplication::instance() && QThread::currentThread() != QCoreApplication::instance()->thread();
    const std::shared_ptr<StemtexRendererSession> session = StemtexRendererSession::instance(&stemtexError, waitForStemTeXStartup);
    if (!session) {
        pdfFileName.clear();
        latexOutput = stemtexError.isEmpty() ? i18n("StemTeX renderer is not available.") : stemtexError;
        return LatexFailed;
    }

    const Error stemtexErrorCode = session->render(formula, textColor, maxWidth, fontSize, pdfFileName, latexOutput, m_fileList, &m_lastWarning);
    if (stemtexErrorCode == NoError) {
        m_lastBackendName = QStringLiteral("stemtex");
    }
    return stemtexErrorCode;
#else
    pdfFileName.clear();
    latexOutput = i18n("StemTeX rendering is only available on Windows.");
    return LatexFailed;
#endif
}

}
