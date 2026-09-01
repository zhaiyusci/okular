/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "latexnoteutils.h"

#include <cmath>
#include <exception>
#include <memory>
#include <thread>
#include <utility>

#include <KLocalizedString>
#include <KMessageBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolTip>
#include <QWidget>

#include "core/annotations.h"
#include "core/document.h"
#include "core/latexnotegeometry.h"
#include "core/page.h"
#include "core/utils.h"
#include "gui/debug_ui.h"
#include "gui/guiutils.h"
#include "latexrenderer.h"

namespace
{
LatexNoteUtils::RenderResult renderAppearancePdfNoThrow(const QString &latexInput, const QColor &textColor, double layoutWidthPoints, bool callout, double fontSizePoints) noexcept
{
    try {
        return LatexNoteUtils::renderAppearancePdf(latexInput, textColor, layoutWidthPoints, callout, fontSizePoints);
    } catch (const std::exception &exception) {
        LatexNoteUtils::RenderResult result;
        try {
            result.errorMessage = i18n("LaTeX rendering failed unexpectedly: %1", QString::fromLocal8Bit(exception.what()));
            qCCritical(OkularUiDebug) << result.errorMessage;
        } catch (...) {
        }
        return result;
    } catch (...) {
        LatexNoteUtils::RenderResult result;
        try {
            result.errorMessage = i18n("LaTeX rendering failed because of an unknown internal error.");
            qCCritical(OkularUiDebug) << result.errorMessage;
        } catch (...) {
        }
        return result;
    }
}

template<typename Worker> void startDetachedRenderWorker(QWidget *parent, Worker &&worker)
{
    try {
        std::thread(std::forward<Worker>(worker)).detach();
    } catch (const std::exception &exception) {
        KMessageBox::error(parent, i18n("Could not start the LaTeX rendering worker: %1", QString::fromLocal8Bit(exception.what())));
    } catch (...) {
        KMessageBox::error(parent, i18n("Could not start the LaTeX rendering worker because of an unknown internal error."));
    }
}

bool isFiniteUsableRect(const Okular::NormalizedRect &rect)
{
    return std::isfinite(rect.left) && std::isfinite(rect.top) && std::isfinite(rect.right) && std::isfinite(rect.bottom) && rect.width() > 0.0 && rect.height() > 0.0;
}

Okular::NormalizedRect fitRectInsidePage(Okular::NormalizedRect rect)
{
    if (rect.right > 1.0) {
        rect.left -= rect.right - 1.0;
        rect.right = 1.0;
    }
    if (rect.bottom > 1.0) {
        rect.top -= rect.bottom - 1.0;
        rect.bottom = 1.0;
    }
    if (rect.left < 0.0) {
        rect.right -= rect.left;
        rect.left = 0.0;
    }
    if (rect.top < 0.0) {
        rect.bottom -= rect.top;
        rect.top = 0.0;
    }
    rect.right = qBound(0.0, rect.right, 1.0);
    rect.bottom = qBound(0.0, rect.bottom, 1.0);
    return rect;
}

QString latexErrorMessage(GuiUtils::LatexRenderer::Error errorCode, const QString &latexOutput)
{
    switch (errorCode) {
    case GuiUtils::LatexRenderer::LatexFailed:
        return i18n("LaTeX rendering failed:\n%1", GuiUtils::LatexRenderer::compactErrorMessage(latexOutput));
    case GuiUtils::LatexRenderer::NoError:
        break;
    }
    return QString();
}

QString latexNoteBaseName(const QString &latexInput, const QColor &textColor, double layoutWidthPoints, double fontSizePoints, const QString &backendName)
{
    const bool fixedWidth = std::isfinite(layoutWidthPoints) && layoutWidthPoints > 0.0;
    const QString widthText = fixedWidth ? QString::number(layoutWidthPoints, 'f', 3) : QStringLiteral("0");
    const QString fontSizeText = std::isfinite(fontSizePoints) && fontSizePoints > 0.0 ? QString::number(fontSizePoints, 'f', 3) : QStringLiteral("0");
    const QString renderMode = fixedWidth ? QStringLiteral("fixed-width-content-v6") : QStringLiteral("natural-width-content-v6");
    const QString hashText = latexInput + QStringLiteral("|%1|%2|%3|%4|%5").arg(textColor.name(QColor::HexArgb), widthText, fontSizeText, renderMode, backendName);
    return QString::fromLatin1(QCryptographicHash::hash(hashText.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString latexTemporaryPath()
{
    QString tempPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (tempPath.isEmpty()) {
        tempPath = QDir(QDir::tempPath()).filePath(QStringLiteral("mengshee"));
    }
    tempPath = QDir(tempPath).filePath(QStringLiteral("temporary"));
    QDir().mkpath(tempPath);
    return QDir::cleanPath(tempPath);
}

QTemporaryDir *latexAppearanceSessionRoot()
{
    static const std::unique_ptr<QTemporaryDir> sessionRoot = []() {
        const QString baseLocation = latexTemporaryPath();
        auto tempDir = std::make_unique<QTemporaryDir>(QDir(baseLocation).filePath(QStringLiteral("mengshee-latex-appearances-XXXXXX")));
        tempDir->setAutoRemove(true);
        return tempDir;
    }();

    return sessionRoot.get();
}

QDir latexAppearanceSessionDir()
{
    QTemporaryDir *sessionRoot = latexAppearanceSessionRoot();
    if (!sessionRoot || !sessionRoot->isValid()) {
        return QDir(QDir(latexTemporaryPath()).filePath(QStringLiteral("mengshee-latex-appearances-unavailable/latex-notes")));
    }

    QDir rootDir(sessionRoot->path());
    rootDir.mkpath(QStringLiteral("latex-notes"));
    return QDir(rootDir.filePath(QStringLiteral("latex-notes")));
}

QSizeF pageSizeInPoints(const Okular::Page *page)
{
    if (!page || page->width() <= 0.0 || page->height() <= 0.0) {
        return {};
    }

    const QSizeF dpi = Okular::Utils::realDpi(nullptr);
    const double dpiX = dpi.width() > 0.0 && std::isfinite(dpi.width()) ? dpi.width() : 72.0;
    const double dpiY = dpi.height() > 0.0 && std::isfinite(dpi.height()) ? dpi.height() : 72.0;
    return QSizeF(page->width() * 72.0 / dpiX, page->height() * 72.0 / dpiY);
}
}

namespace LatexNoteUtils
{
const Okular::TextAnnotation *annotationAsLatexTextAnnotation(const Okular::Annotation *annotation)
{
    Q_UNUSED(annotation);
    return nullptr;
}

Okular::TextAnnotation *annotationAsLatexTextAnnotation(Okular::Annotation *annotation)
{
    return const_cast<Okular::TextAnnotation *>(annotationAsLatexTextAnnotation(static_cast<const Okular::Annotation *>(annotation)));
}

const Okular::StampAnnotation *annotationAsLatexStampAnnotation(const Okular::Annotation *annotation)
{
    if (!annotation || annotation->subType() != Okular::Annotation::AStamp || annotation->contents().trimmed().isEmpty() || !annotation->isOkularLatex()) {
        return nullptr;
    }

    const auto *stampAnnotation = static_cast<const Okular::StampAnnotation *>(annotation);
    if (!isFiniteUsableRect(stampAnnotation->boundingRectangle())) {
        return nullptr;
    }

    return stampAnnotation;
}

Okular::StampAnnotation *annotationAsLatexStampAnnotation(Okular::Annotation *annotation)
{
    return const_cast<Okular::StampAnnotation *>(annotationAsLatexStampAnnotation(static_cast<const Okular::Annotation *>(annotation)));
}

bool annotationIsLatex(const Okular::Annotation *annotation)
{
    return annotationAsLatexTextAnnotation(annotation) || annotationAsLatexStampAnnotation(annotation);
}

bool annotationIsLatex(Okular::Annotation *annotation)
{
    return annotationAsLatexTextAnnotation(annotation) || annotationAsLatexStampAnnotation(annotation);
}

QColor colorForLatexAnnotation(const Okular::Annotation *annotation)
{
    if (!annotation) {
        return Qt::black;
    }

    if (annotation->subType() == Okular::Annotation::AText) {
        const auto *textAnnotation = static_cast<const Okular::TextAnnotation *>(annotation);
        QColor textColor = textAnnotation->textColor();
        if (!textColor.isValid() || textColor.alpha() == 0) {
            textColor = Qt::black;
        }
        return textColor;
    }

    QColor textColor = annotation->latexTextColor();
    if (!textColor.isValid() || textColor.alpha() == 0) {
        textColor = Qt::black;
    }
    return textColor;
}

QString defaultLatexAppearancePdfFileName()
{
    QDir dataDir = latexAppearanceSessionDir();
    if (!dataDir.exists()) {
        qCWarning(OkularUiDebug) << "Could not create a temporary directory for the default LaTeX note appearance.";
        return QString();
    }

    const QString targetFileName = dataDir.filePath(QStringLiteral("latex-default-note.pdf"));
    if (QFile::exists(targetFileName)) {
        return targetFileName;
    }

    QFile resourceFile(QStringLiteral(":/mengshee/data/latex-default-note.pdf"));
    if (!resourceFile.exists()) {
        qCWarning(OkularUiDebug) << "Default LaTeX note appearance resource is missing.";
        return QString();
    }
    if (!resourceFile.copy(targetFileName)) {
        qCWarning(OkularUiDebug) << "Could not copy the default LaTeX note appearance to:" << targetFileName << "error:" << resourceFile.errorString();
        return QString();
    }

    return targetFileName;
}

double rectWidthInPoints(const Okular::NormalizedRect &rect, const Okular::Page *page)
{
    const double pageWidth = pageWidthInPoints(page);
    if (pageWidth <= 0.0 || !std::isfinite(rect.width()) || rect.width() <= 0.0) {
        return 0.0;
    }
    return qMax(1.0, rect.width() * pageWidth);
}

double pageWidthInPoints(const Okular::Page *page)
{
    return pageSizeInPoints(page).width();
}

double pageHeightInPoints(const Okular::Page *page)
{
    return pageSizeInPoints(page).height();
}

double rectHeightInPoints(const Okular::NormalizedRect &rect, const Okular::Page *page)
{
    const double pageHeight = pageHeightInPoints(page);
    if (pageHeight <= 0.0 || !std::isfinite(rect.height()) || rect.height() <= 0.0) {
        return 0.0;
    }

    return qMax(1.0, rect.height() * pageHeight);
}

double annotationWidthInPoints(const Okular::Annotation *annotation, const Okular::Page *page)
{
    return annotation ? rectWidthInPoints(annotation->boundingRectangle(), page) : 0.0;
}

double layoutWidthForLatexTextVisibleWidth(double visibleWidthPoints, double padding)
{
    return Okular::LatexNoteGeometry::layoutWidthForVisibleWidth(visibleWidthPoints, padding);
}

double layoutWidthForLatexFrame(const Okular::NormalizedRect &frame, const Okular::Page *page, double padding)
{
    return layoutWidthForLatexTextVisibleWidth(rectWidthInPoints(frame, page), padding);
}

double paddingForLatexAnnotation(const Okular::Annotation *annotation)
{
    if (!annotation || !std::isfinite(annotation->latexPadding()) || annotation->latexPadding() < 0.0) {
        return Okular::LatexNoteGeometry::defaultPaddingPoints();
    }
    return annotation->latexPadding();
}

double fontSizeForLatexAnnotation(const Okular::Annotation *annotation)
{
    if (!annotation || !std::isfinite(annotation->latexFontSize()) || annotation->latexFontSize() <= 0.0) {
        return 0.0;
    }
    return annotation->latexFontSize();
}

double layoutWidthForLatexTextAnnotation(const Okular::TextAnnotation *annotation, const Okular::Page *page)
{
    if (!annotation) {
        return 0.0;
    }

    const double storedWidth = annotation->latexLayoutWidth();
    if (std::isfinite(storedWidth) && storedWidth > 0.0) {
        return storedWidth;
    }

    const double visibleWidthPoints = annotationWidthInPoints(annotation, page);
    return layoutWidthForLatexTextVisibleWidth(visibleWidthPoints, paddingForLatexAnnotation(annotation));
}

QSizeF visualSizeForLatexTextAnnotation(const QSizeF &contentPdfSizePoints, double layoutWidthPoints, double padding)
{
    return Okular::LatexNoteGeometry::visualSizeForContent(contentPdfSizePoints, layoutWidthPoints, padding);
}

bool applyRenderedLatexTextAnnotationAppearance(QWidget *parent,
                                                Okular::Document *document,
                                                int pageNumber,
                                                Okular::TextAnnotation *textAnnotation,
                                                const QColor &textColor,
                                                const QColor &fillColor,
                                                const QColor &borderColor,
                                                double layoutWidthPoints,
                                                bool boxed,
                                                bool prepareModification,
                                                const RenderResult &rendered,
                                                bool showErrors)
{
    if (!document || pageNumber == -1 || !textAnnotation) {
        return false;
    }

    const Okular::Page *page = document->page(pageNumber);
    if (!page) {
        return false;
    }

    if (!rendered.ok) {
        if (showErrors) {
            KMessageBox::error(parent, rendered.errorMessage, i18n("LaTeX rendering failed"));
        } else {
            qCWarning(OkularUiDebug) << "LaTeX note async render failed:" << rendered.errorMessage;
        }
        return false;
    }

    if (!rendered.pdfSizePoints.isValid() || rendered.pdfSizePoints.isEmpty()) {
        if (showErrors) {
            KMessageBox::error(parent, i18n("Could not load the rendered LaTeX note PDF."), i18n("LaTeX rendering failed"));
        } else {
            qCWarning(OkularUiDebug) << "LaTeX note async render produced an invalid PDF size:" << rendered.pdfSizePoints;
        }
        return false;
    }

    const Okular::NormalizedRect updatedRect = textAnnotation->boundingRectangle();
    const Okular::TextAnnotation::InplaceIntent targetIntent =
        textAnnotation->inplaceIntent() == Okular::TextAnnotation::Callout ? Okular::TextAnnotation::Callout : (boxed ? Okular::TextAnnotation::Unknown : Okular::TextAnnotation::TypeWriter);
    const double targetBorderWidth = boxed ? qMax(0.0, textAnnotation->style().width()) : 0.0;
    const bool sameLayoutWidth = qAbs(textAnnotation->latexLayoutWidth() - layoutWidthPoints) < 1e-3;
    showRenderWarning(parent, rendered.warning);
    if (rendered.pdfFileName == textAnnotation->latexAppearancePdfFileName() && updatedRect == textAnnotation->boundingRectangle() && textAnnotation->textColor() == textColor && textAnnotation->style().color() == fillColor &&
        textAnnotation->inplaceBorderColor() == borderColor && textAnnotation->inplaceIntent() == targetIntent && qAbs(textAnnotation->style().width() - targetBorderWidth) < 1e-6 && sameLayoutWidth && textAnnotation->isOkularLatex()) {
        if (prepareModification) {
            return true;
        }
    }

    if (prepareModification) {
        document->prepareToModifyAnnotationProperties(textAnnotation);
    }
    textAnnotation->setOkularLatex(true);
    textAnnotation->setLatexAppearancePdfFileName(rendered.pdfFileName);
    textAnnotation->setLatexLayoutWidth(layoutWidthPoints);
    textAnnotation->setTextColor(textColor);
    textAnnotation->setInplaceBorderColor(borderColor);
    textAnnotation->setInplaceIntent(targetIntent);
    textAnnotation->style().setColor(fillColor);
    textAnnotation->style().setWidth(targetBorderWidth);
    textAnnotation->setBoundingRectangle(updatedRect);
    textAnnotation->setModificationDate(QDateTime::currentDateTime());
    qCDebug(OkularUiDebug) << "Updating LaTeX note appearance; source path:" << rendered.pdfFileName << "layout width:" << layoutWidthPoints << "pdf size:" << rendered.pdfSizePoints
                           << "rect:" << updatedRect.left << updatedRect.top << updatedRect.right << updatedRect.bottom;
    document->modifyPageAnnotationProperties(pageNumber, textAnnotation);
    return true;
}

bool applyRenderedLatexStampAnnotationAppearance(QWidget *parent,
                                                 Okular::Document *document,
                                                 int pageNumber,
                                                 Okular::StampAnnotation *stampAnnotation,
                                                 const QColor &textColor,
                                                 const QColor &fillColor,
                                                 const QColor &borderColor,
                                                 double layoutWidthPoints,
                                                 bool boxed,
                                                 bool prepareModification,
                                                 const RenderResult &rendered,
                                                 bool showErrors)
{
    if (!document || pageNumber == -1 || !stampAnnotation) {
        return false;
    }

    const Okular::Page *page = document->page(pageNumber);
    if (!page) {
        return false;
    }

    if (!rendered.ok) {
        if (showErrors) {
            KMessageBox::error(parent, rendered.errorMessage, i18n("LaTeX rendering failed"));
        } else {
            qCWarning(OkularUiDebug) << "LaTeX stamp async render failed:" << rendered.errorMessage;
        }
        return false;
    }

    if (!rendered.pdfSizePoints.isValid() || rendered.pdfSizePoints.isEmpty()) {
        if (showErrors) {
            KMessageBox::error(parent, i18n("Could not load the rendered LaTeX note PDF."), i18n("LaTeX rendering failed"));
        } else {
            qCWarning(OkularUiDebug) << "LaTeX stamp async render produced an invalid PDF size:" << rendered.pdfSizePoints;
        }
        return false;
    }

    const Okular::NormalizedRect updatedRect = stampAnnotation->boundingRectangle();
    const double targetBorderWidth = boxed ? qMax(0.0, stampAnnotation->style().width()) : 0.0;
    showRenderWarning(parent, rendered.warning);
    if (rendered.pdfFileName == stampAnnotation->latexAppearancePdfFileName() && updatedRect == stampAnnotation->boundingRectangle() && qAbs(stampAnnotation->latexLayoutWidth() - layoutWidthPoints) < 1e-3 &&
        stampAnnotation->isOkularLatex() && stampAnnotation->latexTextColor() == textColor && stampAnnotation->latexFillColor() == fillColor &&
        stampAnnotation->latexBorderColor() == borderColor && qAbs(stampAnnotation->style().width() - targetBorderWidth) < 1e-6) {
        if (prepareModification) {
            return true;
        }
    }

    if (prepareModification) {
        document->prepareToModifyAnnotationProperties(stampAnnotation);
    }
    stampAnnotation->setOkularLatex(true);
    stampAnnotation->setLatexNoteType(stampAnnotation->isLatexCallout() ? Okular::Annotation::LatexNoteCallout : (boxed ? Okular::Annotation::LatexNoteBoxed : Okular::Annotation::LatexNotePlain));
    stampAnnotation->setStampIconName(QStringLiteral("latex-notes"));
    stampAnnotation->setStampImagePath(QString());
    stampAnnotation->setLatexAppearancePdfFileName(rendered.pdfFileName);
    stampAnnotation->setLatexLayoutWidth(layoutWidthPoints);
    stampAnnotation->setLatexTextColor(textColor);
    stampAnnotation->setLatexFillColor(fillColor);
    stampAnnotation->setLatexBorderColor(borderColor);
    stampAnnotation->style().setWidth(targetBorderWidth);
    stampAnnotation->setBoundingRectangle(updatedRect);
    stampAnnotation->setModificationDate(QDateTime::currentDateTime());
    qCDebug(OkularUiDebug) << "Updating LaTeX stamp appearance; source path:" << rendered.pdfFileName << "layout width:" << layoutWidthPoints << "pdf size:" << rendered.pdfSizePoints
                           << "rect:" << updatedRect.left << updatedRect.top << updatedRect.right << updatedRect.bottom;
    document->modifyPageAnnotationProperties(pageNumber, stampAnnotation);
    return true;
}

RenderResult renderAppearancePdf(const QString &latexInput, const QColor &textColor, double layoutWidthPoints)
{
    return renderAppearancePdf(latexInput, textColor, layoutWidthPoints, false, 0.0);
}

RenderResult renderAppearancePdf(const QString &latexInput, const QColor &textColor, double layoutWidthPoints, bool callout)
{
    return renderAppearancePdf(latexInput, textColor, layoutWidthPoints, callout, 0.0);
}

RenderResult renderAppearancePdf(const QString &latexInput, const QColor &textColor, double layoutWidthPoints, bool callout, double fontSizePoints)
{
    Q_UNUSED(callout);
    RenderResult result;
    if (latexInput.trimmed().isEmpty()) {
        result.errorMessage = i18n("LaTeX source is empty.");
        return result;
    }

    GuiUtils::LatexRenderer renderer;
    QString latexOutput;
    QString temporaryPdfFile;
    const GuiUtils::LatexRenderer::Error errorCode = renderer.renderLatexToPdf(latexInput, textColor, temporaryPdfFile, latexOutput, layoutWidthPoints, fontSizePoints);
    if (errorCode != GuiUtils::LatexRenderer::NoError) {
        qCWarning(OkularUiDebug) << "LaTeX note PDF render failed; backend:" << renderer.lastBackendName() << "layout width:" << layoutWidthPoints << "font size:" << fontSizePoints << "error:" << errorCode
                                 << "message:" << latexErrorMessage(errorCode, latexOutput);
        result.errorMessage = latexErrorMessage(errorCode, latexOutput);
        return result;
    }

    const QFileInfo temporaryPdfInfo(temporaryPdfFile);
    qCDebug(OkularUiDebug) << "LaTeX note PDF render finished; backend:" << renderer.lastBackendName() << "layout width:" << layoutWidthPoints << "temporary PDF:" << temporaryPdfFile << "exists:" << temporaryPdfInfo.exists()
                           << "bytes:" << temporaryPdfInfo.size();

    result.pdfSizePoints = GuiUtils::pdfPageSizeInPoints(temporaryPdfFile);
    if (!result.pdfSizePoints.isValid() || result.pdfSizePoints.isEmpty()) {
        qCWarning(OkularUiDebug) << "LaTeX note rendered PDF has invalid size; temporary PDF:" << temporaryPdfFile << "size:" << result.pdfSizePoints;
        result.errorMessage = i18n("Could not load the rendered LaTeX note PDF.");
        return result;
    }
    qCDebug(OkularUiDebug) << "LaTeX note rendered PDF page size:" << result.pdfSizePoints;

    QDir dataDir = latexAppearanceSessionDir();
    if (!dataDir.exists()) {
        result.errorMessage = i18n("Could not create a temporary directory for LaTeX note appearances.");
        return result;
    }

    const QString noteBaseName = latexNoteBaseName(latexInput, textColor, layoutWidthPoints, fontSizePoints, renderer.lastBackendName());
    const QString appearancePdfFileName = dataDir.filePath(QStringLiteral("%1.pdf").arg(noteBaseName));
    if (QFile::exists(appearancePdfFileName)) {
        QFile::remove(appearancePdfFileName);
    }
    if (temporaryPdfFile.isEmpty() || (!QFile::rename(temporaryPdfFile, appearancePdfFileName) && !QFile::copy(temporaryPdfFile, appearancePdfFileName))) {
        qCWarning(OkularUiDebug) << "Could not move rendered LaTeX note PDF; from:" << temporaryPdfFile << "to:" << appearancePdfFileName;
        result.errorMessage = i18n("Could not save the rendered LaTeX note PDF.");
        return result;
    }

    if (!QFile::exists(appearancePdfFileName)) {
        qCWarning(OkularUiDebug) << "LaTeX note appearance PDF is missing; target:" << appearancePdfFileName;
        result.errorMessage = i18n("Could not save the rendered LaTeX note PDF.");
        return result;
    }

    result.ok = true;
    result.pdfFileName = appearancePdfFileName;
    result.warning = renderer.lastWarning();
    result.warningMessage = warningText(result.warning);
    const QFileInfo appearancePdfInfo(appearancePdfFileName);
    qCDebug(OkularUiDebug) << "LaTeX note appearance PDF ready; path:" << result.pdfFileName << "exists:" << appearancePdfInfo.exists() << "bytes:" << appearancePdfInfo.size() << "page size:" << result.pdfSizePoints
                           << "warning:" << result.warningMessage;
    return result;
}

bool updateLatexTextAnnotationAppearance(QWidget *parent,
                                         Okular::Document *document,
                                         int pageNumber,
                                         Okular::TextAnnotation *textAnnotation,
                                         const QColor &textColor,
                                         const QColor &fillColor,
                                         const QColor &borderColor,
                                         double layoutWidthPoints,
                                         bool boxed,
                                         bool prepareModification)
{
    if (!document || pageNumber == -1 || !textAnnotation) {
        return false;
    }

    const Okular::Page *page = document->page(pageNumber);
    if (!page) {
        return false;
    }
    if (!std::isfinite(layoutWidthPoints) || layoutWidthPoints < 0.0) {
        layoutWidthPoints = layoutWidthForLatexTextAnnotation(textAnnotation, page);
    }
    const RenderResult rendered = renderAppearancePdf(textAnnotation->contents(), textColor, layoutWidthPoints, false, fontSizeForLatexAnnotation(textAnnotation));
    return applyRenderedLatexTextAnnotationAppearance(parent, document, pageNumber, textAnnotation, textColor, fillColor, borderColor, layoutWidthPoints, boxed, prepareModification, rendered, true);
}

bool updateLatexStampAnnotationAppearance(QWidget *parent,
                                          Okular::Document *document,
                                          int pageNumber,
                                          Okular::StampAnnotation *stampAnnotation,
                                          const QColor &textColor,
                                          const QColor &fillColor,
                                          const QColor &borderColor,
                                          double layoutWidthPoints,
                                          bool boxed,
                                          bool prepareModification)
{
    if (!document || pageNumber == -1 || !stampAnnotation) {
        return false;
    }

    const Okular::Page *page = document->page(pageNumber);
    if (!page) {
        return false;
    }
    if (!std::isfinite(layoutWidthPoints) || layoutWidthPoints < 0.0) {
        const double storedWidth = stampAnnotation->latexLayoutWidth();
        layoutWidthPoints = std::isfinite(storedWidth) && storedWidth > 0.0 ? storedWidth : 0.0;
    }
    const RenderResult rendered = renderAppearancePdf(stampAnnotation->contents(), textColor, layoutWidthPoints, stampAnnotation->isLatexCallout(), fontSizeForLatexAnnotation(stampAnnotation));
    return applyRenderedLatexStampAnnotationAppearance(parent, document, pageNumber, stampAnnotation, textColor, fillColor, borderColor, layoutWidthPoints, boxed, prepareModification, rendered, true);
}

void updateLatexTextAnnotationAppearanceAsync(QWidget *parent,
                                              Okular::Document *document,
                                              int pageNumber,
                                              const QString &annotationUniqueName,
                                              const QString &latexInput,
                                              const QColor &textColor,
                                              const QColor &fillColor,
                                              const QColor &borderColor,
                                              double layoutWidthPoints,
                                              bool boxed)
{
    if (!document || pageNumber == -1 || annotationUniqueName.isEmpty() || latexInput.trimmed().isEmpty()) {
        return;
    }

    const Okular::Page *annotationPage = document->page(pageNumber);
    const double fontSizePoints = fontSizeForLatexAnnotation(annotationPage ? annotationPage->annotation(annotationUniqueName) : nullptr);
    QPointer<QWidget> parentGuard(parent);
    QPointer<Okular::Document> documentGuard(document);
    QPointer<QCoreApplication> applicationGuard(QCoreApplication::instance());
    if (!applicationGuard) {
        return;
    }
    startDetachedRenderWorker(parent, [applicationGuard, parentGuard, documentGuard, pageNumber, annotationUniqueName, latexInput, textColor, fillColor, borderColor, layoutWidthPoints, boxed, fontSizePoints]() mutable noexcept {
        try {
            RenderResult rendered = renderAppearancePdfNoThrow(latexInput, textColor, layoutWidthPoints, false, fontSizePoints);
            if (!applicationGuard) {
                return;
            }
            const bool queued = QMetaObject::invokeMethod(
                applicationGuard.data(),
                [parentGuard, documentGuard, pageNumber, annotationUniqueName, latexInput, textColor, fillColor, borderColor, layoutWidthPoints, boxed, fontSizePoints, rendered]() mutable {
                    if (!documentGuard) {
                        return;
                    }
                    const Okular::Page *page = documentGuard->page(pageNumber);
                    Okular::Annotation *annotation = page ? page->annotation(annotationUniqueName) : nullptr;
                    auto *textAnnotation = annotationAsLatexTextAnnotation(annotation);
                    if (!textAnnotation || textAnnotation->contents() != latexInput || qAbs(fontSizeForLatexAnnotation(textAnnotation) - fontSizePoints) > 1e-6) {
                        return;
                    }
                    applyRenderedLatexTextAnnotationAppearance(parentGuard.data(), documentGuard.data(), pageNumber, textAnnotation, textColor, fillColor, borderColor, layoutWidthPoints, boxed, true, rendered, false);
                },
                Qt::QueuedConnection);
            if (!queued) {
                qCWarning(OkularUiDebug) << "Could not queue the LaTeX text annotation update";
            }
        } catch (const std::exception &exception) {
            qCCritical(OkularUiDebug) << "Unhandled exception in LaTeX text annotation worker:" << exception.what();
        } catch (...) {
            qCCritical(OkularUiDebug) << "Unknown exception in LaTeX text annotation worker";
        }
    });
}

void updateLatexStampAnnotationAppearanceAsync(QWidget *parent,
                                               Okular::Document *document,
                                               int pageNumber,
                                               const QString &annotationUniqueName,
                                               const QString &latexInput,
                                               const QColor &textColor,
                                               const QColor &fillColor,
                                               const QColor &borderColor,
                                               double layoutWidthPoints,
                                               bool boxed)
{
    if (!document || pageNumber == -1 || annotationUniqueName.isEmpty() || latexInput.trimmed().isEmpty()) {
        return;
    }

    const Okular::Page *annotationPage = document->page(pageNumber);
    const double fontSizePoints = fontSizeForLatexAnnotation(annotationPage ? annotationPage->annotation(annotationUniqueName) : nullptr);
    QPointer<QWidget> parentGuard(parent);
    QPointer<Okular::Document> documentGuard(document);
    QPointer<QCoreApplication> applicationGuard(QCoreApplication::instance());
    if (!applicationGuard) {
        return;
    }
    startDetachedRenderWorker(parent, [applicationGuard, parentGuard, documentGuard, pageNumber, annotationUniqueName, latexInput, textColor, fillColor, borderColor, layoutWidthPoints, boxed, fontSizePoints]() mutable noexcept {
        try {
            RenderResult rendered = renderAppearancePdfNoThrow(latexInput, textColor, layoutWidthPoints, true, fontSizePoints);
            if (!applicationGuard) {
                return;
            }
            const bool queued = QMetaObject::invokeMethod(
                applicationGuard.data(),
                [parentGuard, documentGuard, pageNumber, annotationUniqueName, latexInput, textColor, fillColor, borderColor, layoutWidthPoints, boxed, fontSizePoints, rendered]() mutable {
                    if (!documentGuard) {
                        return;
                    }
                    const Okular::Page *page = documentGuard->page(pageNumber);
                    Okular::Annotation *annotation = page ? page->annotation(annotationUniqueName) : nullptr;
                    auto *stampAnnotation = annotationAsLatexStampAnnotation(annotation);
                    if (!stampAnnotation || stampAnnotation->contents() != latexInput || qAbs(fontSizeForLatexAnnotation(stampAnnotation) - fontSizePoints) > 1e-6) {
                        return;
                    }
                    applyRenderedLatexStampAnnotationAppearance(parentGuard.data(), documentGuard.data(), pageNumber, stampAnnotation, textColor, fillColor, borderColor, layoutWidthPoints, boxed, true, rendered, false);
                },
                Qt::QueuedConnection);
            if (!queued) {
                qCWarning(OkularUiDebug) << "Could not queue the LaTeX stamp annotation update";
            }
        } catch (const std::exception &exception) {
            qCCritical(OkularUiDebug) << "Unhandled exception in LaTeX stamp annotation worker:" << exception.what();
        } catch (...) {
            qCCritical(OkularUiDebug) << "Unknown exception in LaTeX stamp annotation worker";
        }
    });
}

QString warningText(const GuiUtils::LatexRenderWarning &warning)
{
    return warning.isValid() ? warning.message : QString();
}

void showRenderWarning(QWidget *parent, const QString &warningMessage)
{
    showRenderWarning(parent, warningMessage, QCursor::pos());
}

void showRenderWarning(QWidget *parent, const GuiUtils::LatexRenderWarning &warning)
{
    showRenderWarning(parent, warningText(warning));
}

void showRenderWarning(QWidget *parent, const QString &warningMessage, const QPoint &globalPosition)
{
    if (warningMessage.isEmpty()) {
        return;
    }

    QToolTip::showText(globalPosition, warningMessage, parent, QRect(), 7000);
}

void showRenderWarning(QWidget *parent, const GuiUtils::LatexRenderWarning &warning, const QPoint &globalPosition)
{
    showRenderWarning(parent, warningText(warning), globalPosition);
}
}
