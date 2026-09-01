/*
    SPDX-FileCopyrightText: 2006 Pino Toscano <toscano.pino@tiscali.it>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "dlgannotations.h"

#include "latexrenderer.h"
#include "settings.h"
#include "widgetannottools.h"

#include <KLocalizedString>

#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSet>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QUrl>

namespace
{
constexpr int stemTeXProfileNameRole = Qt::UserRole + 1;
constexpr int stemTeXProfileDirectoryNameRole = Qt::UserRole + 2;

QString stemTeXLightHtml(bool ok)
{
    return QStringLiteral("<span style=\"color:%1;font-size:14px;\">&#9679;</span>").arg(ok ? QStringLiteral("#179c48") : QStringLiteral("#c62828"));
}

QString stemTeXStageText(const GuiUtils::StemTeXStatus &status)
{
    // Mirrors StemTeXRenderStage in stemtex_renderer.h.
    switch (status.renderStage) {
    case 1:
        return i18nc("@info Config dialog, annotations page, StemTeX engine status", "latest request queued");
    case 2:
        return i18nc("@info Config dialog, annotations page, StemTeX engine status", "XeTeX typesetting");
    case 3:
        return i18nc("@info Config dialog, annotations page, StemTeX engine status", "xdvipdfmx converting PDF");
    case 4:
        return i18nc("@info Config dialog, annotations page, StemTeX engine status", "worker rebuilding");
    case 5:
        return i18nc("@info Config dialog, annotations page, StemTeX engine status", "renderer stopping");
    case 0:
    default:
        return i18nc("@info Config dialog, annotations page, StemTeX engine status", "idle");
    }
}
}

DlgAnnotations::DlgAnnotations(QWidget *parent)
    : QWidget(parent)
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidgetResizable(true);
    mainLayout->addWidget(scrollArea);

    auto *content = new QWidget(scrollArea);
    scrollArea->setWidget(content);

    QFormLayout *layout = new QFormLayout(content);

    // BEGIN Annotation toolbar: Combo box to set the annotation toolbar associated to annotation action in tool menu
    QComboBox *primaryAnnotationToolBar = new QComboBox(this);
    primaryAnnotationToolBar->addItem(i18nc("item:inlistbox Config dialog, general page", "Full Annotation Toolbar"));
    primaryAnnotationToolBar->addItem(i18nc("item:inlistbox Config dialog, general page", "Quick Annotation Toolbar"));
    primaryAnnotationToolBar->setObjectName(QStringLiteral("kcfg_PrimaryAnnotationToolBar"));
    layout->addRow(i18nc("label:listbox Config dialog, general page", "Annotation toolbar:"), primaryAnnotationToolBar);
    // END Annotation toolbar

    // BEGIN Author row: Line edit to set the annotation’s default author value.
    QLineEdit *authorLineEdit = new QLineEdit(this);
    authorLineEdit->setObjectName(QStringLiteral("kcfg_IdentityAuthor"));
    layout->addRow(i18nc("@label:textbox Config dialog, annotations page", "Author:"), authorLineEdit);

    QLabel *authorInfoLabel = new QLabel(this);
    authorInfoLabel->setText(
        i18nc("@info Config dialog, annotations page", "<b>Note:</b> the information here is used only for annotations. The information is saved in annotated documents, and so will be transmitted together with the document."));
    authorInfoLabel->setWordWrap(true);
    layout->addRow(authorInfoLabel);
    // END Author row

    // Silly 1Em spacer:
    layout->addRow(new QLabel(this));

    QLabel *latexLabel = new QLabel(this);
    latexLabel->setText(i18nc("@label Config dialog, annotations page, heading line for LaTeX note settings", "<h3>LaTeX Notes</h3>"));
    layout->addRow(latexLabel);

    m_stemTeXProfileNameEdit = new QLineEdit(this);
    m_stemTeXProfileNameEdit->setObjectName(QStringLiteral("kcfg_LatexStemtexProfileName"));
    m_stemTeXProfileNameEdit->hide();
    m_stemTeXProfileNameEdit->setText(Okular::Settings::latexStemtexProfileName());

    auto *profileRow = new QWidget(this);
    auto *profileLayout = new QHBoxLayout(profileRow);
    profileLayout->setContentsMargins(0, 0, 0, 0);
    profileLayout->setSpacing(6);
    m_stemTeXProfileCombo = new QComboBox(this);
    reloadStemTeXProfiles();
    connect(m_stemTeXProfileCombo, &QComboBox::currentIndexChanged, this, [this]() {
        if (!m_stemTeXProfileNameEdit || !m_stemTeXProfileCombo) {
            return;
        }
        m_stemTeXProfileNameEdit->setText(m_stemTeXProfileCombo->currentData().toString());
    });
    connect(m_stemTeXProfileNameEdit, &QLineEdit::textChanged, this, &DlgAnnotations::syncStemTeXProfileCombo);
    syncStemTeXProfileCombo(m_stemTeXProfileNameEdit->text());
    m_stemTeXProfileCreatorButton = new QPushButton(i18nc("@action:button Config dialog, annotations page", "Create..."), profileRow);
    connect(m_stemTeXProfileCreatorButton, &QPushButton::clicked, this, &DlgAnnotations::launchStemTeXProfileCreator);
    auto *openProfilesButton = new QPushButton(i18nc("@action:button Config dialog, annotations page", "Open Folder"), profileRow);
    connect(openProfilesButton, &QPushButton::clicked, this, [this]() {
        const QString profilesRoot = GuiUtils::LatexRenderer::stemTeXUserProfilesRoot();
        QDir().mkpath(profilesRoot);
        QDesktopServices::openUrl(QUrl::fromLocalFile(profilesRoot));
    });
    const QString profilesRoot = GuiUtils::LatexRenderer::stemTeXUserProfilesRoot();
    m_stemTeXProfileCreatorButton->setToolTip(i18nc("@info:tooltip Config dialog, annotations page", "Create a user profile under %1", QDir::toNativeSeparators(profilesRoot)));
    openProfilesButton->setToolTip(i18nc("@info:tooltip Config dialog, annotations page", "Open the Mengshee user profile directory: %1", QDir::toNativeSeparators(profilesRoot)));
    profileLayout->addWidget(m_stemTeXProfileCombo, 1);
    profileLayout->addWidget(m_stemTeXProfileCreatorButton);
    profileLayout->addWidget(openProfilesButton);
    layout->addRow(i18nc("@label:listbox Config dialog, annotations page", "StemTeX profile:"), profileRow);

    auto *texmfRow = new QWidget(this);
    auto *texmfLayout = new QHBoxLayout(texmfRow);
    texmfLayout->setContentsMargins(0, 0, 0, 0);
    texmfLayout->setSpacing(6);
    m_stemTeXTexmfRootEdit = new QLineEdit(texmfRow);
    m_stemTeXTexmfRootEdit->setObjectName(QStringLiteral("kcfg_LatexStemtexTexmfRoot"));
    m_stemTeXTexmfRootEdit->setPlaceholderText(i18nc("@info:placeholder Config dialog, annotations page", "Leave empty to use bundled StemTeX TeX tree"));
    m_stemTeXTexmfRootEdit->setToolTip(i18nc("@info:tooltip Config dialog, annotations page", "Directory containing texmf-dist. Empty uses %1.", QDir::toNativeSeparators(GuiUtils::LatexRenderer::defaultStemTeXTexmfRoot())));
    auto *browseTexmf = new QPushButton(i18nc("@action:button Config dialog, annotations page", "Browse..."), texmfRow);
    connect(browseTexmf, &QPushButton::clicked, this, [this]() {
        const QString start = m_stemTeXTexmfRootEdit->text().trimmed().isEmpty() ? GuiUtils::LatexRenderer::defaultStemTeXTexmfRoot() : m_stemTeXTexmfRootEdit->text().trimmed();
        const QString selected = QFileDialog::getExistingDirectory(this, i18nc("@title:window Config dialog, annotations page", "Select TeXLive package/font tree"), start);
        if (!selected.isEmpty()) {
            m_stemTeXTexmfRootEdit->setText(QDir::toNativeSeparators(QDir::cleanPath(selected)));
        }
    });
    texmfLayout->addWidget(m_stemTeXTexmfRootEdit, 1);
    texmfLayout->addWidget(browseTexmf);
    layout->addRow(i18nc("@label:textbox Config dialog, annotations page", "StemTeX TeX tree:"), texmfRow);

    m_stemTeXStatusLabel = new QLabel(this);
    m_stemTeXStatusLabel->setTextFormat(Qt::RichText);
    layout->addRow(i18nc("@label Config dialog, annotations page", "StemTeX status:"), m_stemTeXStatusLabel);
    m_stemTeXStatusTimer = new QTimer(this);
    m_stemTeXStatusTimer->setInterval(500);
    connect(m_stemTeXStatusTimer, &QTimer::timeout, this, &DlgAnnotations::refreshStemTeXStatus);
    m_stemTeXStatusTimer->start();
    refreshStemTeXStatus();

    // Silly 1Em spacer:
    layout->addRow(new QLabel(this));

    // BEGIN Quick annotation tools section: WidgetAnnotTools manages tools.
    QLabel *toolsLabel = new QLabel(this);
    toolsLabel->setText(i18nc("@label Config dialog, annotations page, heading line for Quick Annotations tool manager", "<h3>Quick Annotation Tools</h3>"));
    layout->addRow(toolsLabel);

    WidgetAnnotTools *kcfg_QuickAnnotationTools = new WidgetAnnotTools(this);
    kcfg_QuickAnnotationTools->setObjectName(QStringLiteral("kcfg_QuickAnnotationTools"));
    layout->addRow(kcfg_QuickAnnotationTools);
    // END Quick annotation tools section
}

void DlgAnnotations::reloadStemTeXProfiles()
{
    if (!m_stemTeXProfileCombo) {
        return;
    }

    const QString currentProfile = m_stemTeXProfileNameEdit ? m_stemTeXProfileNameEdit->text().trimmed() : Okular::Settings::latexStemtexProfileName().trimmed();
    const QSignalBlocker blocker(m_stemTeXProfileCombo);
    m_stemTeXProfileCombo->clear();
    m_stemTeXProfileCombo->addItem(i18nc("@item:inlistbox Config dialog, annotations page", "Auto (recommended bundled profile)"), QString());

    const QList<GuiUtils::StemTeXProfile> profiles = GuiUtils::LatexRenderer::stemTeXProfiles();
    QStringList profileIds;
    QString resolvedCurrentProfile = currentProfile;
    for (const GuiUtils::StemTeXProfile &profile : profiles) {
        const QString label = profile.userManaged ? i18nc("@item:inlistbox Config dialog, annotations page", "%1 (user)", profile.name) : i18nc("@item:inlistbox Config dialog, annotations page", "%1 (bundled)", profile.name);
        m_stemTeXProfileCombo->addItem(label, profile.id);
        m_stemTeXProfileCombo->setItemData(m_stemTeXProfileCombo->count() - 1, QDir::toNativeSeparators(profile.path), Qt::ToolTipRole);
        m_stemTeXProfileCombo->setItemData(m_stemTeXProfileCombo->count() - 1, profile.name, stemTeXProfileNameRole);
        m_stemTeXProfileCombo->setItemData(m_stemTeXProfileCombo->count() - 1, QFileInfo(profile.path).fileName(), stemTeXProfileDirectoryNameRole);
        profileIds << profile.id;
        if (resolvedCurrentProfile == currentProfile && !currentProfile.contains(QLatin1Char(':')) && (profile.name.compare(currentProfile, Qt::CaseInsensitive) == 0 || QFileInfo(profile.path).fileName().compare(currentProfile, Qt::CaseInsensitive) == 0)) {
            resolvedCurrentProfile = profile.id;
        }
    }

    if (resolvedCurrentProfile != currentProfile && m_stemTeXProfileNameEdit) {
        m_stemTeXProfileNameEdit->setText(resolvedCurrentProfile);
    }
    if (!resolvedCurrentProfile.isEmpty() && !profileIds.contains(resolvedCurrentProfile)) {
        m_stemTeXProfileCombo->addItem(i18nc("@item:inlistbox Config dialog, annotations page", "Missing: %1", resolvedCurrentProfile), resolvedCurrentProfile);
    }

    m_stemTeXProfileCombo->setEnabled(!profiles.isEmpty());
    m_stemTeXProfileCombo->setToolTip(profiles.isEmpty() ? i18nc("@info:tooltip Config dialog, annotations page", "No bundled or Mengshee user StemTeX profiles were found.") : QString());
}

void DlgAnnotations::launchStemTeXProfileCreator()
{
    const QString executable = GuiUtils::LatexRenderer::stemTeXProfileCreatorExecutable();
    if (!QFileInfo::exists(executable)) {
        QMessageBox::warning(this, i18nc("@title:window", "StemTeX Profile Creator"), i18n("StemTeX Profile Creator was not found: %1", QDir::toNativeSeparators(executable)));
        return;
    }

    const QString profilesRoot = GuiUtils::LatexRenderer::stemTeXUserProfilesRoot();
    if (!QDir().mkpath(profilesRoot)) {
        QMessageBox::warning(this, i18nc("@title:window", "StemTeX Profile Creator"), i18n("Could not create the Mengshee profile directory: %1", QDir::toNativeSeparators(profilesRoot)));
        return;
    }

    QSet<QString> existingUserProfileIds;
    const QList<GuiUtils::StemTeXProfile> existingProfiles = GuiUtils::LatexRenderer::stemTeXProfiles();
    for (const GuiUtils::StemTeXProfile &profile : existingProfiles) {
        if (profile.userManaged) {
            existingUserProfileIds.insert(profile.id);
        }
    }

    const QString texmfRoot = m_stemTeXTexmfRootEdit && !m_stemTeXTexmfRootEdit->text().trimmed().isEmpty() ? QDir::cleanPath(QDir(m_stemTeXTexmfRootEdit->text().trimmed()).absolutePath()) : GuiUtils::LatexRenderer::defaultStemTeXTexmfRoot();
    if (!QFileInfo::exists(QDir(texmfRoot).filePath(QStringLiteral("texmf-dist/web2c")))) {
        QMessageBox::warning(this,
                             i18nc("@title:window", "StemTeX Profile Creator"),
                             i18n("The selected TeX tree is not available: %1\n\nInstall the Mengshee StemTeX support package or select a TeX Live tree before creating a profile.", QDir::toNativeSeparators(texmfRoot)));
        return;
    }
    auto *process = new QProcess(this);
    process->setProgram(executable);
    process->setArguments({QStringLiteral("--runtime"), GuiUtils::LatexRenderer::stemTeXRuntimeRoot(), QStringLiteral("--texmf"), texmfRoot, QStringLiteral("--profiles"), profilesRoot});
    process->setWorkingDirectory(QFileInfo(executable).absolutePath());
    m_stemTeXProfileCreatorButton->setEnabled(false);

    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            QMessageBox::warning(this, i18nc("@title:window", "StemTeX Profile Creator"), i18n("Could not start StemTeX Profile Creator: %1", process->errorString()));
            m_stemTeXProfileCreatorButton->setEnabled(true);
            process->deleteLater();
        }
    });
    connect(process, &QProcess::finished, this, [this, process, existingUserProfileIds](int, QProcess::ExitStatus exitStatus) {
        m_stemTeXProfileCreatorButton->setEnabled(true);
        if (exitStatus == QProcess::CrashExit) {
            QMessageBox::warning(this, i18nc("@title:window", "StemTeX Profile Creator"), i18n("StemTeX Profile Creator exited unexpectedly."));
        }

        QStringList newProfileIds;
        const QList<GuiUtils::StemTeXProfile> profiles = GuiUtils::LatexRenderer::stemTeXProfiles();
        for (const GuiUtils::StemTeXProfile &profile : profiles) {
            if (profile.userManaged && !existingUserProfileIds.contains(profile.id)) {
                newProfileIds << profile.id;
            }
        }
        reloadStemTeXProfiles();
        if (newProfileIds.size() == 1) {
            m_stemTeXProfileNameEdit->setText(newProfileIds.constFirst());
        } else {
            syncStemTeXProfileCombo(m_stemTeXProfileNameEdit->text());
        }
        process->deleteLater();
    });
    process->start();
}

void DlgAnnotations::syncStemTeXProfileCombo(const QString &profileName)
{
    if (!m_stemTeXProfileCombo) {
        return;
    }

    const QString target = profileName.trimmed();
    for (int i = 0; i < m_stemTeXProfileCombo->count(); ++i) {
        const QString profileId = m_stemTeXProfileCombo->itemData(i).toString();
        const bool legacyNameMatch = !target.contains(QLatin1Char(':')) && (m_stemTeXProfileCombo->itemData(i, stemTeXProfileNameRole).toString().compare(target, Qt::CaseInsensitive) == 0 || m_stemTeXProfileCombo->itemData(i, stemTeXProfileDirectoryNameRole).toString().compare(target, Qt::CaseInsensitive) == 0);
        if (profileId == target || legacyNameMatch) {
            if (legacyNameMatch && m_stemTeXProfileNameEdit && m_stemTeXProfileNameEdit->text() != profileId) {
                m_stemTeXProfileNameEdit->setText(profileId);
            }
            if (m_stemTeXProfileCombo->currentIndex() != i) {
                m_stemTeXProfileCombo->setCurrentIndex(i);
            }
            return;
        }
    }

    if (!target.isEmpty()) {
        m_stemTeXProfileCombo->addItem(i18nc("@item:inlistbox Config dialog, annotations page", "Missing: %1", target), target);
        m_stemTeXProfileCombo->setCurrentIndex(m_stemTeXProfileCombo->count() - 1);
    }
}

void DlgAnnotations::refreshStemTeXStatus()
{
    if (!m_stemTeXStatusLabel) {
        return;
    }

    const GuiUtils::StemTeXStatus status = GuiUtils::LatexRenderer::stemTeXStatus();
    QString text;
    if (!status.supported) {
        text = stemTeXLightHtml(false) + QStringLiteral("&nbsp;") + status.note.toHtmlEscaped();
        m_stemTeXStatusLabel->setText(text);
        return;
    }

    text += stemTeXLightHtml(status.ready && status.primaryReady);

    QString note = status.note;
    if (note.isEmpty() && status.ready) {
        note = stemTeXStageText(status);
    }
    if (status.asyncRunning) {
        note += i18nc("@info Config dialog, annotations page, StemTeX engine status", ", running job %1", QString::number(status.runningJobId));
    }
    if (status.asyncPending) {
        note += i18nc("@info Config dialog, annotations page, StemTeX engine status", ", pending job %1", QString::number(status.pendingJobId));
    }
    if (note.isEmpty() && status.initializing) {
        note = i18nc("@info Config dialog, annotations page, StemTeX engine status", "starting");
    } else if (note.isEmpty() && status.ready && status.primaryReady) {
        note = i18nc("@info Config dialog, annotations page, StemTeX engine status", "ready");
    }

    if (!note.isEmpty()) {
        text += QStringLiteral("&nbsp;");
        text += note.toHtmlEscaped();
    }
    m_stemTeXStatusLabel->setText(text);
}
