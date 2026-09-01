/*
    SPDX-FileCopyrightText: 2017 Tobias Deiminger <haxtibal@t-online.de>
    SPDX-FileCopyrightText: 2004-2005 Enrico Ros <eros.kde@email.it>
    SPDX-FileCopyrightText: 2004-2006 Albert Astals Cid <aacid@kde.org>

    Work sponsored by the LiMux project of the city of Munich:
    SPDX-FileCopyrightText: 2017 Klarälvdalens Datakonsult AB a KDAB Group company <info@kdab.com>

    With portions of code from kpdf/kpdf_pagewidget.cc by:
    SPDX-FileCopyrightText: 2002 Wilco Greven <greven@kde.org>
    SPDX-FileCopyrightText: 2003 Christophe Devriese <Christophe.Devriese@student.kuleuven.ac.be>
    SPDX-FileCopyrightText: 2003 Laurent Montel <montel@kde.org>
    SPDX-FileCopyrightText: 2003 Dirk Mueller <mueller@kde.org>
    SPDX-FileCopyrightText: 2004 James Ots <kde@jamesots.com>
    SPDX-FileCopyrightText: 2011 Jiri Baum - NICTA <jiri@baum.com.au>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "pageviewmouseannotation.h"

#include <cmath>
#include <exception>
#include <thread>

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMetaObject>
#include <QPointer>
#include <QPolygon>
#include <QStandardPaths>
#include <QTextStream>
#include <qevent.h>
#include <qpainter.h>
#include <qtooltip.h>

#include <KLocalizedString>

#include "core/document.h"
#include "core/page.h"
#include "gui/debug_ui.h"
#include "gui/guiutils.h"
#include "latexnoteutils.h"
#include "pageview.h"
#include "videowidget.h"

static const int handleSize = 10;
static const int handleSizeHalf = handleSize / 2;
static const int latexWarningMarkerSize = 14;

static bool isStampAnnotation(const Okular::Annotation *annotation)
{
    return annotation && annotation->subType() == Okular::Annotation::AStamp;
}

static Okular::TextAnnotation *calloutTextAnnotation(Okular::Annotation *annotation)
{
    if (!annotation || annotation->subType() != Okular::Annotation::AText) {
        return nullptr;
    }

    auto *textAnnotation = static_cast<Okular::TextAnnotation *>(annotation);
    if (textAnnotation->textType() != Okular::TextAnnotation::InPlace || textAnnotation->inplaceIntent() != Okular::TextAnnotation::Callout) {
        return nullptr;
    }

    return textAnnotation;
}

static const Okular::TextAnnotation *calloutTextAnnotation(const Okular::Annotation *annotation)
{
    return calloutTextAnnotation(const_cast<Okular::Annotation *>(annotation));
}

static bool isLatexCalloutStampAnnotation(const Okular::Annotation *annotation)
{
    return annotation && annotation->subType() == Okular::Annotation::AStamp && annotation->isOkularLatex() && annotation->isLatexCallout();
}

static QString annotationInteractionLogPath()
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

static void logLatexCalloutInteraction(const char *operation, const Okular::Annotation *annotation, const QStringList &details = QStringList())
{
    if (!OkularUiDebug().isDebugEnabled() || !isLatexCalloutStampAnnotation(annotation)) {
        return;
    }

    const Okular::NormalizedRect rect = annotation->boundingRectangle();
    QStringList fields = {QStringLiteral("LaTeX callout interaction; operation: %1").arg(QLatin1String(operation)),
                          QStringLiteral("rect: %1,%2,%3,%4").arg(rect.left).arg(rect.top).arg(rect.right).arg(rect.bottom),
                          QStringLiteral("flags: %1").arg(annotation->flags()),
                          QStringLiteral("p0: %1,%2").arg(annotation->latexCalloutPoint(0).x).arg(annotation->latexCalloutPoint(0).y),
                          QStringLiteral("p1: %1,%2").arg(annotation->latexCalloutPoint(1).x).arg(annotation->latexCalloutPoint(1).y),
                          QStringLiteral("p2: %1,%2").arg(annotation->latexCalloutPoint(2).x).arg(annotation->latexCalloutPoint(2).y),
                          QStringLiteral("tp0: %1,%2").arg(annotation->transformedLatexCalloutPoint(0).x).arg(annotation->transformedLatexCalloutPoint(0).y),
                          QStringLiteral("tp1: %1,%2").arg(annotation->transformedLatexCalloutPoint(1).x).arg(annotation->transformedLatexCalloutPoint(1).y),
                          QStringLiteral("tp2: %1,%2").arg(annotation->transformedLatexCalloutPoint(2).x).arg(annotation->transformedLatexCalloutPoint(2).y)};
    fields << details;
    const QString message = fields.join(QStringLiteral("; "));
    qCDebug(OkularUiDebug).noquote() << message;

    QFile logFile(annotationInteractionLogPath());
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&logFile);
        stream << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << " " << message << '\n';
    }
}

static Okular::Annotation *calloutAnnotation(Okular::Annotation *annotation)
{
    if (calloutTextAnnotation(annotation) || isLatexCalloutStampAnnotation(annotation)) {
        return annotation;
    }
    return nullptr;
}

static const Okular::Annotation *calloutAnnotation(const Okular::Annotation *annotation)
{
    return calloutAnnotation(const_cast<Okular::Annotation *>(annotation));
}

static Okular::NormalizedRect latexCalloutBoxRectangle(const Okular::Annotation *annotation)
{
    return annotation ? annotation->boundingRectangle() : Okular::NormalizedRect();
}

static bool isCalloutHandle(MouseAnnotation::ResizeHandle handle)
{
    return handle == MouseAnnotation::RH_CalloutTip || handle == MouseAnnotation::RH_CalloutKnee || handle == MouseAnnotation::RH_CalloutAnchor;
}

static bool isLinePointHandle(MouseAnnotation::ResizeHandle handle)
{
    return handle == MouseAnnotation::RH_LinePoint;
}

static Okular::LineAnnotation *lineAnnotation(Okular::Annotation *annotation)
{
    if (!annotation || annotation->subType() != Okular::Annotation::ALine) {
        return nullptr;
    }
    return static_cast<Okular::LineAnnotation *>(annotation);
}

static const Okular::LineAnnotation *lineAnnotation(const Okular::Annotation *annotation)
{
    return lineAnnotation(const_cast<Okular::Annotation *>(annotation));
}

static bool hasFinitePoint(const Okular::NormalizedPoint &point);

static bool hasUsableLinePoints(const Okular::Annotation *annotation)
{
    const Okular::LineAnnotation *lineAnn = lineAnnotation(annotation);
    if (!lineAnn || lineAnn->linePoints().count() < 2 || lineAnn->transformedLinePoints().count() != lineAnn->linePoints().count()) {
        return false;
    }

    for (const Okular::NormalizedPoint &point : lineAnn->linePoints()) {
        if (!hasFinitePoint(point)) {
            return false;
        }
    }
    for (const Okular::NormalizedPoint &point : lineAnn->transformedLinePoints()) {
        if (!hasFinitePoint(point)) {
            return false;
        }
    }
    return true;
}

static Okular::NormalizedRect lineBoundingRect(const QList<Okular::NormalizedPoint> &points)
{
    if (points.isEmpty()) {
        return {};
    }

    double left = points.first().x;
    double right = points.first().x;
    double top = points.first().y;
    double bottom = points.first().y;
    for (const Okular::NormalizedPoint &point : points) {
        left = qMin(left, point.x);
        right = qMax(right, point.x);
        top = qMin(top, point.y);
        bottom = qMax(bottom, point.y);
    }
    return Okular::NormalizedRect(left, top, right, bottom);
}

static int calloutIndexForHandle(MouseAnnotation::ResizeHandle handle)
{
    if (handle == MouseAnnotation::RH_CalloutTip) {
        return 0;
    }
    if (handle == MouseAnnotation::RH_CalloutKnee) {
        return 1;
    }
    if (handle == MouseAnnotation::RH_CalloutAnchor) {
        return 2;
    }
    return -1;
}

static MouseAnnotation::ResizeHandle calloutHandleForIndex(int index)
{
    switch (index) {
    case 0:
        return MouseAnnotation::RH_CalloutTip;
    case 1:
        return MouseAnnotation::RH_CalloutKnee;
    case 2:
        return MouseAnnotation::RH_CalloutAnchor;
    default:
        return MouseAnnotation::RH_None;
    }
}

static bool hasFinitePoint(const Okular::NormalizedPoint &point)
{
    return std::isfinite(point.x) && std::isfinite(point.y);
}

static Okular::NormalizedPoint calloutPoint(const Okular::Annotation *annotation, int index, bool transformed)
{
    if (const auto *textAnnotation = calloutTextAnnotation(annotation)) {
        return transformed ? textAnnotation->transformedInplaceCallout(index) : textAnnotation->inplaceCallout(index);
    }
    if (isLatexCalloutStampAnnotation(annotation)) {
        return transformed ? annotation->transformedLatexCalloutPoint(index) : annotation->latexCalloutPoint(index);
    }
    return {};
}

static void setCalloutPoint(Okular::Annotation *annotation, const Okular::NormalizedPoint &point, int index)
{
    if (auto *textAnnotation = calloutTextAnnotation(annotation)) {
        textAnnotation->setInplaceCallout(point, index);
    } else if (isLatexCalloutStampAnnotation(annotation)) {
        annotation->setLatexCalloutPoint(point, index);
    }
}

static bool hasUsableCalloutPoints(const Okular::Annotation *annotation)
{
    if (!annotation) {
        return false;
    }

    bool hasNonZeroPoint = false;
    for (int i = 0; i < 3; ++i) {
        const Okular::NormalizedPoint point = calloutPoint(annotation, i, false);
        const Okular::NormalizedPoint transformedPoint = calloutPoint(annotation, i, true);
        if (!hasFinitePoint(point) || !hasFinitePoint(transformedPoint)) {
            return false;
        }
        hasNonZeroPoint = hasNonZeroPoint || point.x != 0.0 || point.y != 0.0;
    }
    return hasNonZeroPoint;
}

static Okular::NormalizedPoint boundToCalloutBoxEdge(const Okular::NormalizedPoint &point, const Okular::NormalizedRect &box)
{
    const double x = qBound(box.left, point.x, box.right);
    const double y = qBound(box.top, point.y, box.bottom);

    const double leftDistance = qAbs(x - box.left);
    const double rightDistance = qAbs(box.right - x);
    const double topDistance = qAbs(y - box.top);
    const double bottomDistance = qAbs(box.bottom - y);
    const double edgeDistance = qMin(qMin(leftDistance, rightDistance), qMin(topDistance, bottomDistance));

    if (edgeDistance == leftDistance) {
        return Okular::NormalizedPoint(box.left, y);
    }
    if (edgeDistance == rightDistance) {
        return Okular::NormalizedPoint(box.right, y);
    }
    if (edgeDistance == topDistance) {
        return Okular::NormalizedPoint(x, box.top);
    }
    return Okular::NormalizedPoint(x, box.bottom);
}

static bool pointMoved(const Okular::NormalizedPoint &a, const Okular::NormalizedPoint &b)
{
    constexpr double minDelta = 1e-8;
    return qAbs(a.x - b.x) >= minDelta || qAbs(a.y - b.y) >= minDelta;
}

static bool isFiniteRect(const Okular::NormalizedRect &rect)
{
    return std::isfinite(rect.left) && std::isfinite(rect.top) && std::isfinite(rect.right) && std::isfinite(rect.bottom);
}

static bool isUsableRect(const Okular::NormalizedRect &rect)
{
    return isFiniteRect(rect) && rect.right > rect.left && rect.bottom > rect.top;
}

static bool isZeroDelta(const Okular::NormalizedPoint &delta)
{
    constexpr double minDelta = 1e-8;
    return qAbs(delta.x) < minDelta && qAbs(delta.y) < minDelta;
}

static Okular::NormalizedRect toNormalizedRect(const QRectF &rect)
{
    return Okular::NormalizedRect(rect.left(), rect.top(), rect.right(), rect.bottom());
}

static QRectF toRectF(const Okular::NormalizedRect &rect)
{
    return QRectF(QPointF(rect.left, rect.top), QPointF(rect.right, rect.bottom)).normalized();
}

static bool handleAdjustsHorizontally(MouseAnnotation::ResizeHandle rotatedHandle)
{
    return rotatedHandle & (MouseAnnotation::RH_Left | MouseAnnotation::RH_Right);
}

static bool handleAdjustsLayoutWidth(MouseAnnotation::ResizeHandle rotatedHandle)
{
    return handleAdjustsHorizontally(rotatedHandle);
}

static QRectF latexControlRectAfterResize(const Okular::NormalizedRect &baseLayoutRect, MouseAnnotation::ResizeHandle rotatedHandle, const QPointF &delta1, const QPointF &delta2)
{
    const QRectF baseRect = toRectF(baseLayoutRect);
    if (baseRect.width() <= 0.0 || baseRect.height() <= 0.0) {
        return {};
    }

    Q_UNUSED(rotatedHandle);
    return QRectF(baseRect.left() + delta1.x(), baseRect.top() + delta1.y(), baseRect.width() + delta2.x() - delta1.x(), baseRect.height() + delta2.y() - delta1.y());
}

static MouseAnnotation::ResizeHandle rotateHandleForPage(MouseAnnotation::ResizeHandle handle, Okular::Rotation rotation)
{
    unsigned int rotatedHandle = 0;
    switch (rotation) {
    case Okular::Rotation90:
        rotatedHandle = (handle << 3 | handle >> (4 - 3)) & MouseAnnotation::RH_AllHandles;
        break;
    case Okular::Rotation180:
        rotatedHandle = (handle << 2 | handle >> (4 - 2)) & MouseAnnotation::RH_AllHandles;
        break;
    case Okular::Rotation270:
        rotatedHandle = (handle << 1 | handle >> (4 - 1)) & MouseAnnotation::RH_AllHandles;
        break;
    case Okular::Rotation0:
    default:
        rotatedHandle = handle;
        break;
    }
    return static_cast<MouseAnnotation::ResizeHandle>(rotatedHandle);
}

static void fitRectInsidePage(QRectF &rect)
{
    constexpr double minSize = 1e-6;
    if (!std::isfinite(rect.left()) || !std::isfinite(rect.top()) || !std::isfinite(rect.right()) || !std::isfinite(rect.bottom()) || rect.width() <= minSize || rect.height() <= minSize) {
        rect = QRectF();
        return;
    }

    if (rect.width() > 1.0 || rect.height() > 1.0) {
        const double scale = qMin(1.0 / rect.width(), 1.0 / rect.height());
        const QSizeF scaledSize(rect.width() * scale, rect.height() * scale);
        rect = QRectF(rect.center() - QPointF(scaledSize.width() / 2.0, scaledSize.height() / 2.0), scaledSize);
    }

    if (rect.left() < 0.0) {
        rect.translate(-rect.left(), 0.0);
    }
    if (rect.top() < 0.0) {
        rect.translate(0.0, -rect.top());
    }
    if (rect.right() > 1.0) {
        rect.translate(1.0 - rect.right(), 0.0);
    }
    if (rect.bottom() > 1.0) {
        rect.translate(0.0, 1.0 - rect.bottom());
    }
}

static Okular::NormalizedRect adjustedRectForResize(const Okular::NormalizedRect &sourceRect, const QPointF &delta1, const QPointF &delta2)
{
    QRectF adjustedRect(sourceRect.left + delta1.x(), sourceRect.top + delta1.y(), sourceRect.width() + delta2.x() - delta1.x(), sourceRect.height() + delta2.y() - delta1.y());
    fitRectInsidePage(adjustedRect);
    const Okular::NormalizedRect normalizedRect = toNormalizedRect(adjustedRect);
    return isUsableRect(normalizedRect) ? normalizedRect : Okular::NormalizedRect();
}

static double stampImageAspectRatio(const Okular::Annotation *annotation)
{
    if (!isStampAnnotation(annotation)) {
        return 0.0;
    }

    const auto *stamp = static_cast<const Okular::StampAnnotation *>(annotation);
    const QString iconName = stamp->stampImagePath().isEmpty() ? stamp->stampIconName() : stamp->stampImagePath();
    if (QFile::exists(iconName)) {
        QImageReader reader(iconName);
        QSize imageSize = reader.size();
        if (imageSize.isEmpty()) {
            imageSize = reader.read().size();
        }
        if (imageSize.width() > 0 && imageSize.height() > 0) {
            return double(imageSize.width()) / imageSize.height();
        }
    }

    return 0.0;
}

static bool latexTextAnnotationBoxed(const Okular::TextAnnotation *annotation)
{
    return annotation && annotation->inplaceIntent() != Okular::TextAnnotation::TypeWriter;
}

static bool latexStampAnnotationBoxed(const Okular::StampAnnotation *annotation)
{
    return annotation && annotation->latexNoteType() != Okular::Annotation::LatexNotePlain;
}

static QColor fillColorForLatexTextAnnotation(const Okular::TextAnnotation *annotation, bool boxed)
{
    if (!boxed) {
        return Qt::transparent;
    }

    QColor fillColor = annotation ? annotation->style().color() : QColor();
    if (!fillColor.isValid() || fillColor.alpha() == 0) {
        fillColor = Qt::yellow;
    }
    return fillColor;
}

static QColor borderColorForLatexTextAnnotation(const Okular::TextAnnotation *annotation, bool boxed)
{
    if (!boxed) {
        return Qt::transparent;
    }

    QColor borderColor = annotation ? annotation->inplaceBorderColor() : QColor();
    if (!borderColor.isValid() || borderColor.alpha() == 0) {
        borderColor = LatexNoteUtils::colorForLatexAnnotation(annotation);
    }
    return borderColor;
}

static QColor fillColorForLatexStampAnnotation(const Okular::StampAnnotation *annotation, bool boxed)
{
    if (!boxed) {
        return Qt::transparent;
    }

    QColor fillColor = annotation ? annotation->latexFillColor() : QColor();
    if (!fillColor.isValid() || fillColor.alpha() == 0) {
        fillColor = Qt::yellow;
    }
    return fillColor;
}

static QColor borderColorForLatexStampAnnotation(const Okular::StampAnnotation *annotation, bool boxed)
{
    if (!boxed) {
        return Qt::transparent;
    }

    QColor borderColor = annotation ? annotation->latexBorderColor() : QColor();
    if (!borderColor.isValid() || borderColor.alpha() == 0) {
        borderColor = LatexNoteUtils::colorForLatexAnnotation(annotation);
    }
    return borderColor;
}

static double latexAnnotationLayoutWidth(const Okular::Annotation *annotation, const Okular::Page *page)
{
    if (const auto *textAnnotation = LatexNoteUtils::annotationAsLatexTextAnnotation(annotation)) {
        return LatexNoteUtils::layoutWidthForLatexTextAnnotation(textAnnotation, page);
    }

    if (const auto *stampAnnotation = LatexNoteUtils::annotationAsLatexStampAnnotation(annotation)) {
        const double layoutWidthPoints = stampAnnotation->latexLayoutWidth();
        if (std::isfinite(layoutWidthPoints) && layoutWidthPoints > 0.0) {
            return layoutWidthPoints;
        }
        return LatexNoteUtils::layoutWidthForLatexTextVisibleWidth(LatexNoteUtils::rectWidthInPoints(stampAnnotation->boundingRectangle(), page), LatexNoteUtils::paddingForLatexAnnotation(stampAnnotation));
    }

    return 0.0;
}

static QRect controlGeometry(const AnnotationDescription &ad)
{
    return Okular::AnnotationUtils::annotationGeometry(ad.annotation, ad.pageViewItem->uncroppedWidth(), ad.pageViewItem->uncroppedHeight());
}

static QRect geometryForBoundingRect(const AnnotationDescription &ad, Okular::NormalizedRect rect)
{
    if (!ad.isValid() || !ad.pageViewItem || !ad.pageViewItem->page()) {
        return {};
    }

    QTransform matrix;
    matrix.rotate(int(ad.pageViewItem->page()->rotation()) * 90);
    switch (ad.pageViewItem->page()->rotation()) {
    case Okular::Rotation90:
        matrix.translate(0, -1);
        break;
    case Okular::Rotation180:
        matrix.translate(-1, -1);
        break;
    case Okular::Rotation270:
        matrix.translate(-1, 0);
        break;
    case Okular::Rotation0:
    default:
        break;
    }
    rect.transform(matrix);
    return rect.geometry(ad.pageViewItem->uncroppedWidth(), ad.pageViewItem->uncroppedHeight());
}

static GuiUtils::LatexRenderWarning compileErrorWarningForLatexNote(const LatexNoteUtils::RenderResult &rendered)
{
    GuiUtils::LatexRenderWarning warning;
    warning.type = GuiUtils::LatexRenderWarningType::CompileError;
    warning.message = rendered.errorMessage.trimmed().isEmpty() ? i18n("LaTeX rendering failed. The previous appearance is still shown.") : rendered.errorMessage;
    return warning;
}

struct LatexResizeUpdate {
    int pageNumber = -1;
    QString annotationUniqueName;
    QString contents;
    QColor textColor;
    Okular::NormalizedRect resizedRect;
    MouseAnnotation::ResizeHandle handle = MouseAnnotation::RH_None;
    MouseAnnotation::ResizeHandle rotatedHandle = MouseAnnotation::RH_None;
    bool adjustsLayoutWidth = false;
    bool needsRender = false;
    bool callout = false;
    double visibleWidthPoints = 0.0;
    double layoutWidthPoints = 0.0;
    double fontSizePoints = 0.0;
    QString pdfFileName;
    QSizeF pdfSize;
};

static bool
prepareLatexResizeUpdate(Okular::Document *document, int pageNumber, Okular::Annotation *annotation, const Okular::NormalizedRect &resizedRect, MouseAnnotation::ResizeHandle handle, Okular::Rotation rotation, LatexResizeUpdate *update)
{
    if (!document || !update || !LatexNoteUtils::annotationIsLatex(annotation) || annotation->contents().trimmed().isEmpty()) {
        return false;
    }

    const Okular::Page *page = document->page(pageNumber);
    if (!page || !isUsableRect(resizedRect) || annotation->uniqueName().isEmpty()) {
        return false;
    }

    update->pageNumber = pageNumber;
    update->annotationUniqueName = annotation->uniqueName();
    update->contents = annotation->contents();
    update->textColor = LatexNoteUtils::colorForLatexAnnotation(annotation);
    update->resizedRect = resizedRect;
    update->handle = handle;
    update->rotatedHandle = rotateHandleForPage(handle, rotation);
    update->adjustsLayoutWidth = handleAdjustsLayoutWidth(update->rotatedHandle);
    update->visibleWidthPoints = LatexNoteUtils::rectWidthInPoints(resizedRect, page);
    update->pdfFileName = annotation->latexAppearancePdfFileName();
    update->pdfSize = GuiUtils::pdfPageSizeInPoints(update->pdfFileName);
    update->layoutWidthPoints = latexAnnotationLayoutWidth(annotation, page);
    update->fontSizePoints = LatexNoteUtils::fontSizeForLatexAnnotation(annotation);
    update->callout = annotation->isLatexCallout();

    if (!std::isfinite(update->layoutWidthPoints) || update->layoutWidthPoints <= 0.0) {
        update->layoutWidthPoints = LatexNoteUtils::layoutWidthForLatexTextVisibleWidth(LatexNoteUtils::rectWidthInPoints(annotation->boundingRectangle(), page), LatexNoteUtils::paddingForLatexAnnotation(annotation));
    }
    if (!std::isfinite(update->layoutWidthPoints) || update->layoutWidthPoints <= 0.0) {
        return false;
    }

    if (update->adjustsLayoutWidth) {
        update->layoutWidthPoints = LatexNoteUtils::layoutWidthForLatexTextVisibleWidth(update->visibleWidthPoints, LatexNoteUtils::paddingForLatexAnnotation(annotation));
        update->needsRender = true;
    } else if (!update->pdfSize.isValid() || update->pdfSize.isEmpty() || update->pdfFileName.isEmpty()) {
        update->needsRender = true;
    }

    if (!std::isfinite(update->layoutWidthPoints) || update->layoutWidthPoints <= 0.0) {
        return false;
    }

    qCDebug(OkularUiDebug) << "Finalizing LaTeX note resize; handle:" << int(update->handle) << "rotated handle:" << int(update->rotatedHandle) << "adjusts layout width:" << update->adjustsLayoutWidth
                           << "visible width:" << update->visibleWidthPoints << "async render:" << update->needsRender;
    return true;
}

static bool applyLatexResizeUpdate(Okular::Document *document, const LatexResizeUpdate &update, const LatexNoteUtils::RenderResult *rendered, GuiUtils::LatexRenderWarning *warning)
{
    if (!document) {
        return false;
    }

    const Okular::Page *page = document->page(update.pageNumber);
    Okular::Annotation *annotation = page ? page->annotation(update.annotationUniqueName) : nullptr;
    if (!page || !LatexNoteUtils::annotationIsLatex(annotation) || annotation->contents() != update.contents || qAbs(LatexNoteUtils::fontSizeForLatexAnnotation(annotation) - update.fontSizePoints) > 1e-6) {
        return false;
    }

    GuiUtils::LatexRenderWarning renderWarning;
    QString pdfFileName = update.pdfFileName;
    QSizeF pdfSize = update.pdfSize;
    if (rendered) {
        if (rendered->ok && rendered->pdfSizePoints.isValid() && !rendered->pdfSizePoints.isEmpty() && !rendered->pdfFileName.isEmpty()) {
            pdfFileName = rendered->pdfFileName;
            pdfSize = rendered->pdfSizePoints;
            renderWarning = rendered->warning;
            qCDebug(OkularUiDebug) << "LaTeX note resize render produced PDF; path:" << pdfFileName << "size:" << pdfSize;
        } else {
            qCWarning(OkularUiDebug) << "LaTeX note resize render failed; keeping the previous appearance:" << rendered->errorMessage;
            if (warning) {
                *warning = compileErrorWarningForLatexNote(*rendered);
            }
        }
    }
    if (!pdfSize.isValid() || pdfSize.isEmpty() || pdfFileName.isEmpty()) {
        qCWarning(OkularUiDebug) << "LaTeX note resize has no reusable appearance PDF; committing the fixed frame anyway";
    }

    const Okular::NormalizedRect updatedRect = update.resizedRect;
    if (!isUsableRect(updatedRect)) {
        qCWarning(OkularUiDebug) << "LaTeX note resize produced an invalid annotation rectangle";
        return false;
    }

    if (warning && !warning->isValid()) {
        *warning = renderWarning;
    }

    document->prepareToModifyAnnotationProperties(annotation);
    if (auto *textAnnotation = LatexNoteUtils::annotationAsLatexTextAnnotation(annotation)) {
        const bool boxed = latexTextAnnotationBoxed(textAnnotation);
        const Okular::TextAnnotation::InplaceIntent targetIntent =
            textAnnotation->inplaceIntent() == Okular::TextAnnotation::Callout ? Okular::TextAnnotation::Callout : (boxed ? Okular::TextAnnotation::Unknown : Okular::TextAnnotation::TypeWriter);
        const double targetBorderWidth = boxed ? qMax(0.0, textAnnotation->style().width()) : 0.0;
        textAnnotation->setTextColor(update.textColor);
        textAnnotation->setInplaceBorderColor(borderColorForLatexTextAnnotation(textAnnotation, boxed));
        textAnnotation->setInplaceIntent(targetIntent);
        textAnnotation->style().setColor(fillColorForLatexTextAnnotation(textAnnotation, boxed));
        textAnnotation->style().setWidth(targetBorderWidth);
    } else if (auto *stampAnnotation = LatexNoteUtils::annotationAsLatexStampAnnotation(annotation)) {
        const bool boxed = stampAnnotation->latexNoteType() != Okular::Annotation::LatexNotePlain;
        const QColor fillColor = fillColorForLatexStampAnnotation(stampAnnotation, boxed);
        stampAnnotation->setStampIconName(QStringLiteral("latex-notes"));
        stampAnnotation->setStampImagePath(QString());
        stampAnnotation->setLatexNoteType(stampAnnotation->isLatexCallout() ? Okular::Annotation::LatexNoteCallout : (boxed ? Okular::Annotation::LatexNoteBoxed : Okular::Annotation::LatexNotePlain));
        stampAnnotation->setLatexTextColor(update.textColor);
        stampAnnotation->setLatexFillColor(fillColor);
        stampAnnotation->setLatexBorderColor(borderColorForLatexStampAnnotation(stampAnnotation, boxed));
        stampAnnotation->style().setWidth(boxed ? qMax(0.0, stampAnnotation->style().width()) : 0.0);
    }
    annotation->setOkularLatex(true);
    annotation->setFlags(annotation->flags() | Okular::Annotation::FixedRotation);
    annotation->setLatexAppearancePdfFileName(pdfFileName);
    annotation->setLatexLayoutWidth(update.layoutWidthPoints);
    annotation->setBoundingRectangle(updatedRect);
    annotation->setModificationDate(QDateTime::currentDateTime());
    qCDebug(OkularUiDebug) << "Writing LaTeX note resize result to annotation; appearance PDF:" << pdfFileName << "layout width:" << update.layoutWidthPoints << "pdf size:" << pdfSize
                            << "fixed rect:" << updatedRect.left << updatedRect.top << updatedRect.right << updatedRect.bottom;
    document->modifyPageAnnotationProperties(update.pageNumber, annotation);
    qCDebug(OkularUiDebug) << "Updated LaTeX note geometry; layout width:" << update.layoutWidthPoints << "rect:" << updatedRect.left << updatedRect.top << updatedRect.right << updatedRect.bottom;
    return true;
}

bool MouseAnnotation::updateLatexNoteAfterResizeAsync(const AnnotationDescription &ad, const Okular::NormalizedRect &resizedRect, ResizeHandle handle, Okular::Rotation rotation)
{
    LatexResizeUpdate update;
    if (!prepareLatexResizeUpdate(m_document, ad.pageNumber, ad.annotation, resizedRect, handle, rotation, &update)) {
        return false;
    }

    const quint64 requestId = ++m_latexResizeRequestId;
    auto finishResize = [this, requestId](const LatexResizeUpdate &resizeUpdate, const LatexNoteUtils::RenderResult *rendered) -> bool {
        if (requestId != m_latexResizeRequestId) {
            return true;
        }

        const bool clearingPendingPreview = m_hasPendingLatexResizePreview && m_pendingLatexResizeAnnotationUniqueName == resizeUpdate.annotationUniqueName;
        if (clearingPendingPreview && m_focusedAnnotation.isValid() && m_focusedAnnotation.pageNumber == resizeUpdate.pageNumber && m_focusedAnnotation.annotation &&
            m_focusedAnnotation.annotation->uniqueName() == resizeUpdate.annotationUniqueName) {
            updateViewport(m_focusedAnnotation);
        }
        if (clearingPendingPreview) {
            m_hasPendingLatexResizePreview = false;
            m_pendingLatexResizeAnnotationUniqueName.clear();
        }

        GuiUtils::LatexRenderWarning warning;
        const bool ok = applyLatexResizeUpdate(m_document, resizeUpdate, rendered, &warning);
        if (m_focusedAnnotation.isValid() && m_focusedAnnotation.pageNumber == resizeUpdate.pageNumber && m_focusedAnnotation.annotation && m_focusedAnnotation.annotation->uniqueName() == resizeUpdate.annotationUniqueName) {
            if (const Okular::Page *page = m_document->page(resizeUpdate.pageNumber)) {
                if (Okular::Annotation *currentAnnotation = page->annotation(resizeUpdate.annotationUniqueName)) {
                    m_focusedAnnotation.annotation = currentAnnotation;
                }
            }

            if (!ok) {
                if (warning.isValid()) {
                    setLatexRenderWarning(m_focusedAnnotation, warning);
                } else {
                    updateViewport(m_focusedAnnotation);
                }
                return false;
            }

            updateViewport(m_focusedAnnotation);
            if (warning.isValid()) {
                setLatexRenderWarning(m_focusedAnnotation, warning);
            } else {
                clearLatexRenderWarning();
            }
            updateViewport(m_focusedAnnotation);
        }
        return ok;
    };

    if (!update.needsRender) {
        return finishResize(update, nullptr);
    }

    m_pendingLatexResizeAnnotationUniqueName = update.annotationUniqueName;
    m_pendingLatexResizePreviewRect = update.resizedRect;
    m_hasPendingLatexResizePreview = true;
    updateViewport(ad);

    QPointer<MouseAnnotation> self(this);
    QPointer<QCoreApplication> applicationGuard(QCoreApplication::instance());
    if (!applicationGuard) {
        m_hasPendingLatexResizePreview = false;
        m_pendingLatexResizeAnnotationUniqueName.clear();
        return false;
    }
    try {
        std::thread([applicationGuard, self, requestId, update, finishResize]() mutable noexcept {
            try {
                LatexNoteUtils::RenderResult rendered;
                try {
                    rendered = LatexNoteUtils::renderAppearancePdf(update.contents, update.textColor, update.layoutWidthPoints, update.callout, update.fontSizePoints);
                } catch (const std::exception &exception) {
                    rendered.errorMessage = i18n("LaTeX rendering failed unexpectedly: %1", QString::fromLocal8Bit(exception.what()));
                    qCCritical(OkularUiDebug) << rendered.errorMessage;
                } catch (...) {
                    rendered.errorMessage = i18n("LaTeX rendering failed because of an unknown internal error.");
                    qCCritical(OkularUiDebug) << rendered.errorMessage;
                }

                if (!applicationGuard) {
                    return;
                }
                const bool queued = QMetaObject::invokeMethod(
                    applicationGuard.data(),
                    [self, requestId, update, rendered, finishResize]() mutable {
                        if (!self || requestId != self->m_latexResizeRequestId) {
                            return;
                        }
                        finishResize(update, &rendered);
                    },
                    Qt::QueuedConnection);
                if (!queued) {
                    qCWarning(OkularUiDebug) << "Could not queue the LaTeX resize result";
                }
            } catch (const std::exception &exception) {
                qCCritical(OkularUiDebug) << "Unhandled exception in LaTeX resize worker:" << exception.what();
            } catch (...) {
                qCCritical(OkularUiDebug) << "Unknown exception in LaTeX resize worker";
            }
        }).detach();
    } catch (const std::exception &exception) {
        m_hasPendingLatexResizePreview = false;
        m_pendingLatexResizeAnnotationUniqueName.clear();
        GuiUtils::LatexRenderWarning warning;
        warning.type = GuiUtils::LatexRenderWarningType::CompileError;
        warning.message = i18n("Could not start the LaTeX rendering worker: %1", QString::fromLocal8Bit(exception.what()));
        warning.severity = 1.0;
        setLatexRenderWarning(ad, warning);
        return false;
    } catch (...) {
        m_hasPendingLatexResizePreview = false;
        m_pendingLatexResizeAnnotationUniqueName.clear();
        GuiUtils::LatexRenderWarning warning;
        warning.type = GuiUtils::LatexRenderWarningType::CompileError;
        warning.message = i18n("Could not start the LaTeX rendering worker because of an unknown internal error.");
        warning.severity = 1.0;
        setLatexRenderWarning(ad, warning);
        return false;
    }
    return true;
}

static double stampNormalizedAspectRatio(const AnnotationDescription &ad)
{
    const double imageAspectRatio = stampImageAspectRatio(ad.annotation);
    if (std::isfinite(imageAspectRatio) && imageAspectRatio > 0.0 && ad.pageViewItem && ad.pageViewItem->page()) {
        const Okular::Page *page = ad.pageViewItem->page();
        if (page->width() > 0.0 && page->height() > 0.0) {
            return imageAspectRatio * page->height() / page->width();
        }
    }

    if (std::isfinite(imageAspectRatio) && imageAspectRatio > 0.0 && ad.pageViewItem) {
        const QRect &pageRect = ad.pageViewItem->uncroppedGeometry();
        if (pageRect.width() > 0 && pageRect.height() > 0) {
            return imageAspectRatio * double(pageRect.height()) / pageRect.width();
        }
    }

    if (!ad.annotation) {
        return 0.0;
    }
    const Okular::NormalizedRect rect = ad.annotation->boundingRectangle();
    if (rect.height() > 0.0) {
        return rect.width() / rect.height();
    }
    return 0.0;
}

bool AnnotationDescription::isValid() const
{
    return (annotation != nullptr);
}

bool AnnotationDescription::isContainedInPage(const Okular::Document *document, int pageNumber) const
{
    if (AnnotationDescription::pageNumber == pageNumber) {
        /* Don't access page via pageViewItem here. pageViewItem might have been deleted. */
        const Okular::Page *page = document->page(pageNumber);
        if (page != nullptr) {
            if (page->annotations().contains(annotation)) {
                return true;
            }
        }
    }
    return false;
}

void AnnotationDescription::invalidate()
{
    annotation = nullptr;
    pageViewItem = nullptr;
    pageNumber = -1;
}

AnnotationDescription::AnnotationDescription(PageViewItem *newPageViewItem, const QPoint eventPos)
{
    const Okular::AnnotationObjectRect *annObjRect = nullptr;
    if (newPageViewItem) {
        const QRect &uncroppedPage = newPageViewItem->uncroppedGeometry();
        /* find out normalized mouse coords inside current item (nX and nY will be in the range of 0..1). */
        const double nX = newPageViewItem->absToPageX(eventPos.x());
        const double nY = newPageViewItem->absToPageY(eventPos.y());
        annObjRect = static_cast<const Okular::AnnotationObjectRect *>(newPageViewItem->page()->objectRect(Okular::ObjectRect::OAnnotation, nX, nY, uncroppedPage.width(), uncroppedPage.height()));
    }

    if (annObjRect) {
        annotation = annObjRect->annotation();
        pageViewItem = newPageViewItem;
        pageNumber = pageViewItem->pageNumber();
    } else {
        invalidate();
    }
}

MouseAnnotation::MouseAnnotation(PageView *parent, Okular::Document *document)
    : QObject(parent)
    , m_document(document)
    , m_pageView(parent)
    , m_state(StateInactive)
    , m_handle(RH_None)
    , m_hasOriginalBoundingRect(false)
    , m_hasPreviewBoundingRect(false)
    , m_hasOriginalCalloutGeometry(false)
    , m_hasOriginalLineGeometry(false)
    , m_linePointHandleIndex(-1)
    , m_hasLatexResizeLayoutRect(false)
    , m_latexRenderWarningAnnotation(nullptr)
    , m_latexResizeRequestId(0)
    , m_hasPendingLatexResizePreview(false)
{
    m_resizeHandleList << RH_Left << RH_Right << RH_Top << RH_Bottom << RH_TopLeft << RH_TopRight << RH_BottomLeft << RH_BottomRight;
}

MouseAnnotation::~MouseAnnotation()
{
}

void MouseAnnotation::routeMousePressEvent(PageViewItem *pageViewItem, const QPoint eventPos)
{
    PageViewItem *interactionPageItem = pageViewItem;
    if (!interactionPageItem && m_focusedAnnotation.isValid()) {
        interactionPageItem = m_focusedAnnotation.pageViewItem;
    }

    /* Is there a selected annotation? */
    if (m_focusedAnnotation.isValid() && interactionPageItem) {
        m_mousePosition = eventPos - interactionPageItem->uncroppedGeometry().topLeft();
        m_handle = getHandleAt(m_mousePosition, m_focusedAnnotation);
        if (hasLatexRenderWarning(m_focusedAnnotation) && getLatexWarningMarkerRect(m_focusedAnnotation).contains(m_mousePosition)) {
            return;
        }
        if (m_handle != RH_None) {
            /* Returning here means, the selection-rectangle gets control, unconditionally.
             * Even if it overlaps with another annotation. */
            return;
        }
    }

    AnnotationDescription ad(pageViewItem, eventPos);
    /* qDebug() << "routeMousePressEvent: eventPos = " << eventPos; */
    if (ad.isValid()) {
        if (ad.annotation->subType() == Okular::Annotation::AMovie || ad.annotation->subType() == Okular::Annotation::AScreen || ad.annotation->subType() == Okular::Annotation::AFileAttachment ||
            ad.annotation->subType() == Okular::Annotation::ARichMedia) {
            /* qDebug() << "routeMousePressEvent: trigger action for AMovie/AScreen/AFileAttachment"; */
            processAction(ad);
        } else {
            /* qDebug() << "routeMousePressEvent: select for modification"; */
            m_mousePosition = eventPos - pageViewItem->uncroppedGeometry().topLeft();
            m_handle = getHandleAt(m_mousePosition, ad);
            if (m_handle != RH_None) {
                setState(StateFocused, ad);
            }
        }
    } else {
        /* qDebug() << "routeMousePressEvent: no annotation under mouse, enter StateInactive"; */
        setState(StateInactive, ad);
    }
}

void MouseAnnotation::routeMouseReleaseEvent()
{
    if (isModified()) {
        /* qDebug() << "routeMouseReleaseEvent: finish command"; */
        finishCommand();
        setState(StateFocused, m_focusedAnnotation);
    }
    /*
    else
    {
        qCDebug(OkularUiDebug) << "routeMouseReleaseEvent: ignore";
    }
    */
}

void MouseAnnotation::routeMouseMoveEvent(PageViewItem *pageViewItem, const QPoint eventPos, bool leftButtonPressed)
{
    PageViewItem *interactionPageItem = pageViewItem;
    if (!interactionPageItem && m_focusedAnnotation.isValid()) {
        interactionPageItem = m_focusedAnnotation.pageViewItem;
    }

    if (!interactionPageItem) {
        /* qDebug() << "routeMouseMoveEvent: no pageViewItem provided, ignore"; */
        return;
    }

    if (leftButtonPressed) {
        if (isFocused()) {
            /* On first move event after annotation is selected, enter modification state */
            if (m_handle == RH_Content) {
                /* qDebug() << "routeMouseMoveEvent: handle " << m_handle << ", enter StateMoving"; */
                setState(StateMoving, m_focusedAnnotation);
            } else if (m_handle != RH_None) {
                /* qDebug() << "routeMouseMoveEvent: handle " << m_handle << ", enter StateResizing"; */
                setState(StateResizing, m_focusedAnnotation);
            }
        }

        if (isModified()) {
            /* qDebug() << "routeMouseMoveEvent: perform command, delta " << eventPos - m_mousePosition; */
            updateViewport(m_focusedAnnotation);
            performCommand(eventPos);
            m_mousePosition = eventPos - interactionPageItem->uncroppedGeometry().topLeft();
            updateViewport(m_focusedAnnotation);
        }
    } else {
        if (isFocused()) {
            /* qDebug() << "routeMouseMoveEvent: update cursor for focused annotation, new eventPos " << eventPos; */
            m_mousePosition = eventPos - interactionPageItem->uncroppedGeometry().topLeft();
            m_handle = getHandleAt(m_mousePosition, m_focusedAnnotation);
            m_pageView->updateCursor();
        }

        /* We get here quite frequently. */
        const AnnotationDescription ad(pageViewItem, eventPos);
        m_mousePosition = eventPos - interactionPageItem->uncroppedGeometry().topLeft();
        if (ad.isValid()) {
            if (!(m_mouseOverAnnotation == ad)) {
                /* qDebug() << "routeMouseMoveEvent: Annotation under mouse (subtype " << ad.annotation->subType() << ", flags " << ad.annotation->flags() << ")"; */
                m_mouseOverAnnotation = ad;
                m_pageView->updateCursor();
            }
        } else {
            if (!(m_mouseOverAnnotation == ad)) {
                /* qDebug() << "routeMouseMoveEvent: Annotation disappeared under mouse."; */
                m_mouseOverAnnotation.invalidate();
                m_pageView->updateCursor();
            }
        }
    }
}

void MouseAnnotation::routeKeyPressEvent(const QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_Escape:
        cancel();
        break;
    case Qt::Key_Delete:
        if (m_focusedAnnotation.isValid()) {
            AnnotationDescription adToBeDeleted = m_focusedAnnotation;
            cancel();
            m_document->removePageAnnotation(adToBeDeleted.pageNumber, adToBeDeleted.annotation);
        }
        break;
    }
}

void MouseAnnotation::routeTooltipEvent(const QHelpEvent *helpEvent)
{
    /* qDebug() << "MouseAnnotation::routeTooltipEvent, event " << helpEvent; */
    if (hasLatexRenderWarning(m_focusedAnnotation)) {
        const QRect markerRect = getLatexWarningMarkerRect(m_focusedAnnotation);
        const QRect markerViewportRect = viewportRectForPageRect(markerRect, m_focusedAnnotation);
        const QRect leftHandleViewportRect = viewportRectForPageRect(getHandleRect(RH_Left, m_focusedAnnotation), m_focusedAnnotation);
        const QRect rightHandleViewportRect = viewportRectForPageRect(getHandleRect(RH_Right, m_focusedAnnotation), m_focusedAnnotation);
        if (markerViewportRect.contains(helpEvent->pos()) || leftHandleViewportRect.contains(helpEvent->pos()) || rightHandleViewportRect.contains(helpEvent->pos())) {
            const QPoint tooltipPosition = m_pageView->viewport()->mapToGlobal(markerViewportRect.isValid() ? markerViewportRect.center() : helpEvent->pos());
            LatexNoteUtils::showRenderWarning(m_pageView->viewport(), m_latexRenderWarning, tooltipPosition);
            return;
        }
    }

    if (m_mouseOverAnnotation.isValid() && m_mouseOverAnnotation.annotation->subType() != Okular::Annotation::AWidget) {
        /* get boundingRect in uncropped page coordinates */
        QRect boundingRect = Okular::AnnotationUtils::annotationGeometry(m_mouseOverAnnotation.annotation, m_mouseOverAnnotation.pageViewItem->uncroppedWidth(), m_mouseOverAnnotation.pageViewItem->uncroppedHeight());

        /* uncropped page to content area */
        boundingRect.translate(m_mouseOverAnnotation.pageViewItem->uncroppedGeometry().topLeft());
        /* content area to viewport */
        boundingRect.translate(-m_pageView->contentAreaPosition());

        const QString tip = GuiUtils::prettyToolTip(m_mouseOverAnnotation.annotation);
        QToolTip::showText(helpEvent->globalPos(), tip, m_pageView->viewport(), boundingRect);
    }
}

void MouseAnnotation::routePaint(QPainter *painter, const QRect paintRect)
{
    /* QPainter draws relative to the origin of uncropped viewport. */
    static const QColor borderColor = QColor::fromHsvF(0, 0, 1.0);
    static const QColor fillColor = QColor::fromHsvF(0, 0, 0.75, 0.66);

    if (!isFocused() && !isMoved() && !isResized()) {
        return;
    }
    /*
     * Get annotation bounding rectangle in uncropped page coordinates.
     * Distinction between AnnotationUtils::annotationGeometry() and AnnotationObjectRect::boundingRect() is,
     * that boundingRect would enlarge the QRect to a minimum size of 14 x 14.
     * This is useful for getting focus an a very small annotation,
     * but for drawing and modification we want the real size.
     */
    const bool isLatexNote = LatexNoteUtils::annotationIsLatex(m_focusedAnnotation.annotation);
    QRect selectionRect = controlGeometryForInteraction(m_focusedAnnotation);
    const bool drawingMovePreview = isMoved();
    bool drawingLatexResizePreview = false;
    if (isLatexNote && isResized() && m_hasLatexResizeLayoutRect) {
        selectionRect = controlGeometryForInteraction(m_focusedAnnotation);
        drawingLatexResizePreview = true;
    }

    const QRect fullBoundingRect = getFullBoundingRect(m_focusedAnnotation);
    if (!paintRect.intersects(fullBoundingRect.translated(m_focusedAnnotation.pageViewItem->uncroppedGeometry().topLeft()))) {
        /* Our selection rectangle is not in a region that needs to be (re-)drawn. */
        return;
    }

    painter->save();
    painter->setClipRect(paintRect, Qt::IntersectClip);
    painter->translate(m_focusedAnnotation.pageViewItem->uncroppedGeometry().topLeft());
    painter->setPen(QPen(fillColor, 2, (drawingMovePreview || drawingLatexResizePreview) ? Qt::DashLine : Qt::SolidLine, Qt::SquareCap, Qt::BevelJoin));
    painter->drawRect(selectionRect);
    if (drawingMovePreview) {
        painter->restore();
        return;
    }
    if (hasUsableLinePoints(m_focusedAnnotation.annotation)) {
        const auto *lineAnn = static_cast<const Okular::LineAnnotation *>(m_focusedAnnotation.annotation);
        const QList<Okular::NormalizedPoint> points = lineAnn->transformedLinePoints();
        Q_UNUSED(lineAnn);
        painter->setPen(borderColor);
        painter->setBrush(fillColor);
        for (int i = 0; i < points.count(); ++i) {
            painter->drawRect(getLinePointHandleRect(i, m_focusedAnnotation));
        }
    }
    if (m_focusedAnnotation.annotation->canBeResized()) {
        const Okular::Annotation *calloutAnn = calloutAnnotation(m_focusedAnnotation.annotation);
        if (hasUsableCalloutPoints(calloutAnn)) {
            painter->setPen(borderColor);
            painter->setBrush(fillColor);
            for (int i = 0; i < 3; ++i) {
                painter->drawRect(getHandleRect(calloutHandleForIndex(i), m_focusedAnnotation));
            }
        }

        for (const ResizeHandle &handle : std::as_const(m_resizeHandleList)) {
            const QRect rect = getHandleRect(handle, m_focusedAnnotation);
            painter->setPen(borderColor);
            painter->setBrush(fillColor);
            painter->drawRect(rect);
        }
        const QRect warningMarkerRect = getLatexWarningMarkerRect(m_focusedAnnotation);
        if (warningMarkerRect.isValid()) {
            const QPoint center = warningMarkerRect.center();
            QPolygon triangle;
            triangle << QPoint(center.x(), warningMarkerRect.top()) << QPoint(warningMarkerRect.right(), warningMarkerRect.bottom()) << QPoint(warningMarkerRect.left(), warningMarkerRect.bottom());
            painter->setPen(QPen(QColor(120, 53, 15), 1));
            painter->setBrush(QColor(245, 158, 11));
            painter->drawPolygon(triangle);
            painter->setPen(QPen(Qt::white, 1.4));
            painter->drawLine(center.x(), warningMarkerRect.top() + 4, center.x(), warningMarkerRect.bottom() - 4);
            painter->drawPoint(center.x(), warningMarkerRect.bottom() - 2);
        }
    }
    painter->restore();
}

Okular::Annotation *MouseAnnotation::annotation() const
{
    if (m_focusedAnnotation.isValid()) {
        return m_focusedAnnotation.annotation;
    }
    return nullptr;
}

int MouseAnnotation::pageNumber() const
{
    if (m_focusedAnnotation.isValid()) {
        return m_focusedAnnotation.pageNumber;
    }
    return -1;
}

bool MouseAnnotation::isActive() const
{
    return (m_state != StateInactive);
}

bool MouseAnnotation::isMouseOver() const
{
    return m_mouseOverAnnotation.isValid() || m_handle != RH_None || (hasLatexRenderWarning(m_focusedAnnotation) && getLatexWarningMarkerRect(m_focusedAnnotation).contains(m_mousePosition));
}

bool MouseAnnotation::isFocused() const
{
    return (m_state == StateFocused);
}

bool MouseAnnotation::isMoved() const
{
    return (m_state == StateMoving);
}

bool MouseAnnotation::isResized() const
{
    return (m_state == StateResizing);
}

bool MouseAnnotation::isModified() const
{
    return (m_state == StateMoving || m_state == StateResizing);
}

Qt::CursorShape MouseAnnotation::cursor() const
{
    if (m_handle != RH_None) {
        if (isCalloutHandle(m_handle) || isLinePointHandle(m_handle)) {
            return Qt::SizeAllCursor;
        }
        if (isMoved()) {
            return Qt::SizeAllCursor;
        } else if (isFocused() || isResized()) {
            switch (m_handle) {
            case RH_Top:
                return Qt::SizeVerCursor;
            case RH_TopRight:
                return Qt::SizeBDiagCursor;
            case RH_Right:
                return Qt::SizeHorCursor;
            case RH_BottomRight:
                return Qt::SizeFDiagCursor;
            case RH_Bottom:
                return Qt::SizeVerCursor;
            case RH_BottomLeft:
                return Qt::SizeBDiagCursor;
            case RH_Left:
                return Qt::SizeHorCursor;
            case RH_TopLeft:
                return Qt::SizeFDiagCursor;
            case RH_Content:
                return Qt::SizeAllCursor;
            default:
                return Qt::OpenHandCursor;
            }
        }
    } else if (m_mouseOverAnnotation.isValid()) {
        /* Mouse is over annotation, but the annotation is not yet selected. */
        if (m_mouseOverAnnotation.annotation->subType() == Okular::Annotation::AMovie) {
            return Qt::PointingHandCursor;
        } else if (m_mouseOverAnnotation.annotation->subType() == Okular::Annotation::ARichMedia) {
            return Qt::PointingHandCursor;
        } else if (m_mouseOverAnnotation.annotation->subType() == Okular::Annotation::AScreen) {
            if (GuiUtils::renditionMovieFromScreenAnnotation(static_cast<const Okular::ScreenAnnotation *>(m_mouseOverAnnotation.annotation)) != nullptr) {
                return Qt::PointingHandCursor;
            }
        } else if (m_mouseOverAnnotation.annotation->subType() == Okular::Annotation::AFileAttachment) {
            return Qt::PointingHandCursor;
        } else {
            return Qt::ArrowCursor;
        }
    }

    /* There's no none cursor, so we still have to return something. */
    return Qt::ArrowCursor;
}

void MouseAnnotation::notifyAnnotationChanged(int pageNumber)
{
    const AnnotationDescription emptyAd;

    if (m_focusedAnnotation.isValid() && !m_focusedAnnotation.isContainedInPage(m_document, pageNumber)) {
        setState(StateInactive, emptyAd);
    }

    if (m_mouseOverAnnotation.isValid() && !m_mouseOverAnnotation.isContainedInPage(m_document, pageNumber)) {
        m_mouseOverAnnotation = emptyAd;
        m_pageView->updateCursor();
    }
}

void MouseAnnotation::updateAnnotationPointers()
{
    if (m_focusedAnnotation.annotation) {
        const Okular::Page *page = m_document->page(m_focusedAnnotation.pageNumber);
        m_focusedAnnotation.annotation = page ? page->annotation(m_focusedAnnotation.annotation->uniqueName()) : nullptr;
    }

    if (m_mouseOverAnnotation.annotation) {
        const Okular::Page *page = m_document->page(m_mouseOverAnnotation.pageNumber);
        m_mouseOverAnnotation.annotation = page ? page->annotation(m_mouseOverAnnotation.annotation->uniqueName()) : nullptr;
    }
}

void MouseAnnotation::cancel()
{
    if (isActive()) {
        if (isModified()) {
            rollbackCommand();
        } else {
            finishCommand();
        }
        setState(StateInactive, m_focusedAnnotation);
    }
}

void MouseAnnotation::reset()
{
    cancel();
    m_focusedAnnotation.invalidate();
    m_mouseOverAnnotation.invalidate();
    clearLatexRenderWarning();
}

void MouseAnnotation::focusAnnotation(PageViewItem *pageViewItem, Okular::Annotation *annotation)
{
    if (!pageViewItem || !annotation) {
        return;
    }

    AnnotationDescription ad;
    ad.annotation = annotation;
    ad.pageViewItem = pageViewItem;
    ad.pageNumber = pageViewItem->pageNumber();
    setState(StateFocused, ad);
}

/* Handle state changes for the focused annotation. */
void MouseAnnotation::setState(MouseAnnotationState state, const AnnotationDescription &ad)
{
    /* qDebug() << "setState: requested " << state; */
    Okular::Annotation *previousFocusedAnnotation = m_focusedAnnotation.annotation;
    if (m_focusedAnnotation.isValid()) {
        /* If there was a annotation before, request also repaint for the previous area. */
        updateViewport(m_focusedAnnotation);
    }

    if (!ad.isValid()) {
        /* qDebug() << "No annotation provided, forcing state inactive." << state; */
        state = StateInactive;
    } else if ((state == StateMoving && !ad.annotation->canBeMoved()) || (state == StateResizing && !ad.annotation->canBeResized() && !isLinePointHandle(m_handle))) {
        /* qDebug() << "Annotation does not support requested state, forcing state selected." << state; */
        state = StateInactive;
    }

    switch (state) {
    case StateMoving:
        m_focusedAnnotation = ad;
        if (!m_hasOriginalBoundingRect) {
            m_originalBoundingRect = m_focusedAnnotation.annotation->boundingRectangle();
            m_hasOriginalBoundingRect = isUsableRect(m_originalBoundingRect);
            m_previewBoundingRect = m_originalBoundingRect;
            m_hasPreviewBoundingRect = m_hasOriginalBoundingRect;
        }
        m_focusedAnnotation.annotation->setFlags(m_focusedAnnotation.annotation->flags() | Okular::Annotation::BeingMoved);
        logLatexCalloutInteraction("enter-moving", m_focusedAnnotation.annotation);
        updateViewport(m_focusedAnnotation);
        break;
    case StateResizing:
        m_focusedAnnotation = ad;
        if (isCalloutHandle(m_handle) && !m_hasOriginalCalloutGeometry) {
            rememberOriginalCalloutGeometry(m_focusedAnnotation);
        }
        if (isLinePointHandle(m_handle) && !m_hasOriginalLineGeometry) {
            rememberOriginalLineGeometry(m_focusedAnnotation);
        }
        if (!m_hasOriginalBoundingRect) {
            m_originalBoundingRect = m_focusedAnnotation.annotation->boundingRectangle();
            m_hasOriginalBoundingRect = isUsableRect(m_originalBoundingRect);
            m_previewBoundingRect = m_originalBoundingRect;
            m_hasPreviewBoundingRect = m_hasOriginalBoundingRect;
        }
        if (LatexNoteUtils::annotationIsLatex(m_focusedAnnotation.annotation) && m_hasPendingLatexResizePreview && m_focusedAnnotation.annotation->uniqueName() == m_pendingLatexResizeAnnotationUniqueName) {
            m_latexResizeLayoutRect = m_pendingLatexResizePreviewRect;
            m_hasLatexResizeLayoutRect = true;
            m_previewBoundingRect = m_pendingLatexResizePreviewRect;
            m_hasPreviewBoundingRect = true;
        }
        m_focusedAnnotation.annotation->setFlags(m_focusedAnnotation.annotation->flags() | Okular::Annotation::BeingResized);
        logLatexCalloutInteraction("enter-resizing", m_focusedAnnotation.annotation, {QStringLiteral("handle: %1").arg(int(m_handle))});
        updateViewport(m_focusedAnnotation);
        break;
    case StateFocused:
        m_focusedAnnotation = ad;
        if (previousFocusedAnnotation && previousFocusedAnnotation != m_focusedAnnotation.annotation) {
            clearLatexRenderWarning();
            m_hasPendingLatexResizePreview = false;
            m_pendingLatexResizeAnnotationUniqueName.clear();
        }
        m_hasOriginalBoundingRect = false;
        m_hasPreviewBoundingRect = false;
        m_hasOriginalCalloutGeometry = false;
        m_hasOriginalLineGeometry = false;
        m_originalLinePoints.clear();
        m_linePointHandleIndex = -1;
        m_hasLatexResizeLayoutRect = false;
        m_focusedAnnotation.annotation->setFlags(m_focusedAnnotation.annotation->flags() & ~(Okular::Annotation::BeingMoved | Okular::Annotation::BeingResized));
        refreshLatexRenderWarning(m_focusedAnnotation);
        updateViewport(m_focusedAnnotation);
        break;
    case StateInactive:
    default:
        if (m_focusedAnnotation.isValid()) {
            m_focusedAnnotation.annotation->setFlags(m_focusedAnnotation.annotation->flags() & ~(Okular::Annotation::BeingMoved | Okular::Annotation::BeingResized));
        }
        m_focusedAnnotation.invalidate();
        m_handle = RH_None;
        m_hasOriginalBoundingRect = false;
        m_hasPreviewBoundingRect = false;
        m_hasOriginalCalloutGeometry = false;
        m_hasOriginalLineGeometry = false;
        m_originalLinePoints.clear();
        m_linePointHandleIndex = -1;
        m_hasLatexResizeLayoutRect = false;
        clearLatexRenderWarning();
        m_hasPendingLatexResizePreview = false;
        m_pendingLatexResizeAnnotationUniqueName.clear();
    }

    /* qDebug() << "setState: enter " << state; */
    m_state = state;
    m_pageView->updateCursor();
}

/* Get the rectangular boundary of the given annotation, enlarged for space needed by resize handles.
 * Returns a QRect in page view item coordinates. */
QRect MouseAnnotation::getFullBoundingRect(const AnnotationDescription &ad) const
{
    QRect boundingRect;
    if (ad.isValid()) {
        boundingRect = Okular::AnnotationUtils::annotationGeometry(ad.annotation, ad.pageViewItem->uncroppedWidth(), ad.pageViewItem->uncroppedHeight());
        const QRect previewRect = controlGeometryForInteraction(ad);
        if (previewRect.isValid()) {
            boundingRect = boundingRect.united(previewRect);
        }
        boundingRect = boundingRect.adjusted(-handleSizeHalf, -handleSizeHalf, handleSizeHalf, handleSizeHalf);
        if (calloutAnnotation(ad.annotation)) {
            boundingRect = boundingRect.united(getCalloutLineRect(ad));
            for (int i = 0; i < 3; ++i) {
                boundingRect = boundingRect.united(getHandleRect(calloutHandleForIndex(i), ad));
            }
        }
        if (hasUsableLinePoints(ad.annotation)) {
            boundingRect = boundingRect.united(getLinePointsRect(ad));
            const auto *lineAnn = static_cast<const Okular::LineAnnotation *>(ad.annotation);
            for (int i = 0; i < lineAnn->transformedLinePoints().count(); ++i) {
                boundingRect = boundingRect.united(getLinePointHandleRect(i, ad));
            }
        }
        const QRect warningMarkerRect = getLatexWarningMarkerRect(ad);
        if (warningMarkerRect.isValid()) {
            boundingRect = boundingRect.united(warningMarkerRect);
        }
    }
    return boundingRect;
}

/* Apply the command determined by m_state to the currently focused annotation. */
void MouseAnnotation::performCommand(const QPoint newPos)
{
    const QRect &pageViewItemRect = m_focusedAnnotation.pageViewItem->uncroppedGeometry();
    QPointF mouseDelta(newPos - pageViewItemRect.topLeft() - m_mousePosition);
    QPointF normalizedRotatedMouseDelta(rotateInRect(QPointF(mouseDelta.x() / pageViewItemRect.width(), mouseDelta.y() / pageViewItemRect.height()), m_focusedAnnotation.pageViewItem->page()->rotation()));

    if (isMoved()) {
        Okular::NormalizedPoint delta(normalizedRotatedMouseDelta.x(), normalizedRotatedMouseDelta.y());
        const Okular::NormalizedRect annotRect = m_hasPreviewBoundingRect ? m_previewBoundingRect : m_focusedAnnotation.annotation->boundingRectangle();

        QRectF adjustedRect = toRectF(annotRect).translated(delta.x, delta.y);
        fitRectInsidePage(adjustedRect);
        const Okular::NormalizedRect normalizedRect = toNormalizedRect(adjustedRect);
        if (isUsableRect(normalizedRect)) {
            m_previewBoundingRect = normalizedRect;
            m_hasPreviewBoundingRect = true;
            logLatexCalloutInteraction(
                "move-preview",
                m_focusedAnnotation.annotation,
                {QStringLiteral("delta: %1,%2").arg(delta.x).arg(delta.y), QStringLiteral("preview: %1,%2,%3,%4").arg(normalizedRect.left).arg(normalizedRect.top).arg(normalizedRect.right).arg(normalizedRect.bottom)});
        }
        return;

    } else if (isResized()) {
        if (isLinePointHandle(m_handle)) {
            auto *lineAnn = lineAnnotation(m_focusedAnnotation.annotation);
            if (lineAnn && m_linePointHandleIndex >= 0) {
                QList<Okular::NormalizedPoint> points = lineAnn->linePoints();
                if (m_linePointHandleIndex < points.count()) {
                    Okular::NormalizedPoint point = points.at(m_linePointHandleIndex);
                    point.x = qBound(0.0, point.x + normalizedRotatedMouseDelta.x(), 1.0);
                    point.y = qBound(0.0, point.y + normalizedRotatedMouseDelta.y(), 1.0);
                    points[m_linePointHandleIndex] = point;
                    lineAnn->setLinePoints(points);
                    lineAnn->setBoundingRectangle(lineBoundingRect(points));
                }
            }
            return;
        }

        if (isCalloutHandle(m_handle)) {
            Okular::Annotation *calloutAnn = calloutAnnotation(m_focusedAnnotation.annotation);
            const int pointIndex = calloutIndexForHandle(m_handle);
            if (calloutAnn && pointIndex >= 0) {
                Okular::NormalizedPoint point = calloutPoint(calloutAnn, pointIndex, false);
                point.x += normalizedRotatedMouseDelta.x();
                point.y += normalizedRotatedMouseDelta.y();
                if (m_handle == RH_CalloutAnchor) {
                    point = boundToCalloutBoxEdge(point, latexCalloutBoxRectangle(calloutAnn));
                }
                setCalloutPoint(calloutAnn, point, pointIndex);
                logLatexCalloutInteraction("callout-point-preview",
                                           calloutAnn,
                                           {QStringLiteral("handle: %1").arg(int(m_handle)),
                                            QStringLiteral("index: %1").arg(pointIndex),
                                            QStringLiteral("delta: %1,%2").arg(normalizedRotatedMouseDelta.x()).arg(normalizedRotatedMouseDelta.y()),
                                            QStringLiteral("new point: %1,%2").arg(point.x).arg(point.y)});
            }
            return;
        }

        QPointF delta1, delta2;
        handleToAdjust(normalizedRotatedMouseDelta, delta1, delta2, m_handle, m_focusedAnnotation.pageViewItem->page()->rotation());
        const Okular::NormalizedRect annotRect = m_hasPreviewBoundingRect ? m_previewBoundingRect : m_focusedAnnotation.annotation->boundingRectangle();
        const ResizeHandle rotatedHandle = rotateHandle(m_handle, m_focusedAnnotation.pageViewItem->page()->rotation());

        if (LatexNoteUtils::annotationIsLatex(m_focusedAnnotation.annotation)) {
            const Okular::NormalizedRect baseLayoutRect = m_hasLatexResizeLayoutRect ? m_latexResizeLayoutRect : annotRect;
            QRectF adjustedLayoutRect = latexControlRectAfterResize(baseLayoutRect, rotatedHandle, delta1, delta2);
            if (adjustedLayoutRect.width() > 0.0 && adjustedLayoutRect.height() > 0.0) {
                fitRectInsidePage(adjustedLayoutRect);
                const Okular::NormalizedRect normalizedLayoutRect = toNormalizedRect(adjustedLayoutRect);
                if (isUsableRect(normalizedLayoutRect)) {
                    m_latexResizeLayoutRect = normalizedLayoutRect;
                    m_hasLatexResizeLayoutRect = true;
                    m_previewBoundingRect = normalizedLayoutRect;
                    m_hasPreviewBoundingRect = true;
                }
            }
            return;
        }

        if (isStampAnnotation(m_focusedAnnotation.annotation)) {
            const double aspectRatio = stampNormalizedAspectRatio(m_focusedAnnotation);
            if (std::isfinite(aspectRatio) && aspectRatio > 0.0) {
                QRectF adjustedRect(annotRect.left + delta1.x(), annotRect.top + delta1.y(), annotRect.width() + delta2.x() - delta1.x(), annotRect.height() + delta2.y() - delta1.y());
                if (adjustedRect.width() > 0.0 && adjustedRect.height() > 0.0) {
                    const bool adjustsLeft = rotatedHandle & RH_Left;
                    const bool adjustsRight = rotatedHandle & RH_Right;
                    const bool adjustsTop = rotatedHandle & RH_Top;
                    const bool adjustsBottom = rotatedHandle & RH_Bottom;

                    if ((adjustsLeft || adjustsRight) && !(adjustsTop || adjustsBottom)) {
                        const double height = adjustedRect.width() / aspectRatio;
                        const double centerY = annotRect.top + annotRect.height() / 2.0;
                        adjustedRect.setTop(centerY - height / 2.0);
                        adjustedRect.setBottom(centerY + height / 2.0);
                    } else if ((adjustsTop || adjustsBottom) && !(adjustsLeft || adjustsRight)) {
                        const double width = adjustedRect.height() * aspectRatio;
                        const double centerX = annotRect.left + annotRect.width() / 2.0;
                        adjustedRect.setLeft(centerX - width / 2.0);
                        adjustedRect.setRight(centerX + width / 2.0);
                    } else if (adjustedRect.width() / adjustedRect.height() > aspectRatio) {
                        const double width = adjustedRect.height() * aspectRatio;
                        if (adjustsLeft) {
                            adjustedRect.setLeft(adjustedRect.right() - width);
                        } else {
                            adjustedRect.setRight(adjustedRect.left() + width);
                        }
                    } else {
                        const double height = adjustedRect.width() / aspectRatio;
                        if (adjustsTop) {
                            adjustedRect.setTop(adjustedRect.bottom() - height);
                        } else {
                            adjustedRect.setBottom(adjustedRect.top() + height);
                        }
                    }

                    fitRectInsidePage(adjustedRect);
                    const Okular::NormalizedRect normalizedRect = toNormalizedRect(adjustedRect);
                    if (isUsableRect(normalizedRect)) {
                        m_previewBoundingRect = normalizedRect;
                        m_hasPreviewBoundingRect = true;
                    }
                    return;
                }
            }
        }

        const Okular::NormalizedRect normalizedRect = adjustedRectForResize(annotRect, delta1, delta2);
        if (isUsableRect(normalizedRect)) {
            m_previewBoundingRect = normalizedRect;
            m_hasPreviewBoundingRect = true;
        }
    }
}

/* Finalize a command in progress for the currently focused annotation. */
void MouseAnnotation::finishCommand()
{
    /*
     * Note:
     * Translate-/resizePageAnnotation causes PopplerAnnotationProxy::notifyModification,
     * where modify flag needs to be already cleared. So it is important to call
     * setFlags before translatePageAnnotation-/adjustPageAnnotation.
     */
    if (m_hasOriginalCalloutGeometry) {
        Okular::Annotation *calloutAnn = calloutAnnotation(m_focusedAnnotation.annotation);
        const bool wasResized = isResized();
        Okular::NormalizedPoint finalCalloutPoints[3];
        bool moved = false;
        if (calloutAnn) {
            for (int i = 0; i < 3; ++i) {
                finalCalloutPoints[i] = calloutPoint(calloutAnn, i, false);
                moved = moved || pointMoved(finalCalloutPoints[i], m_originalCalloutPoints[i]);
            }
        }
        logLatexCalloutInteraction("finish-callout-before-restore", calloutAnn, {QStringLiteral("was resized: %1").arg(wasResized), QStringLiteral("moved: %1").arg(moved)});

        restoreOriginalCalloutGeometry(m_focusedAnnotation);
        m_focusedAnnotation.annotation->setFlags(m_focusedAnnotation.annotation->flags() & ~Okular::Annotation::BeingResized);

        if (calloutAnn && wasResized && moved) {
            m_document->prepareToModifyAnnotationProperties(calloutAnn);
            for (int i = 0; i < 3; ++i) {
                setCalloutPoint(calloutAnn, finalCalloutPoints[i], i);
            }
            calloutAnn->setModificationDate(QDateTime::currentDateTime());
            logLatexCalloutInteraction("finish-callout-before-modify", calloutAnn);
            m_document->modifyPageAnnotationProperties(m_focusedAnnotation.pageNumber, calloutAnn);
            logLatexCalloutInteraction("finish-callout-after-modify", calloutAnn);
        }

        m_hasOriginalCalloutGeometry = false;
        return;
    }

    if (m_hasOriginalLineGeometry) {
        auto *lineAnn = lineAnnotation(m_focusedAnnotation.annotation);
        const bool wasResized = isResized();
        QList<Okular::NormalizedPoint> finalLinePoints;
        Okular::NormalizedRect finalBoundingRect;
        bool moved = false;
        if (lineAnn) {
            finalLinePoints = lineAnn->linePoints();
            finalBoundingRect = lineAnn->boundingRectangle();
            if (finalLinePoints.count() == m_originalLinePoints.count()) {
                for (int i = 0; i < finalLinePoints.count(); ++i) {
                    moved = moved || pointMoved(finalLinePoints.at(i), m_originalLinePoints.at(i));
                }
            } else {
                moved = true;
            }
        }

        restoreOriginalLineGeometry(m_focusedAnnotation);
        m_focusedAnnotation.annotation->setFlags(m_focusedAnnotation.annotation->flags() & ~Okular::Annotation::BeingResized);

        if (lineAnn && wasResized && moved) {
            m_document->prepareToModifyAnnotationProperties(lineAnn);
            lineAnn->setLinePoints(finalLinePoints);
            lineAnn->setBoundingRectangle(finalBoundingRect);
            lineAnn->setModificationDate(QDateTime::currentDateTime());
            m_document->modifyPageAnnotationProperties(m_focusedAnnotation.pageNumber, lineAnn);
        }

        m_hasOriginalLineGeometry = false;
        m_originalLinePoints.clear();
        m_linePointHandleIndex = -1;
        m_hasOriginalBoundingRect = false;
        m_hasPreviewBoundingRect = false;
        return;
    }

    if (m_hasOriginalBoundingRect) {
        const bool wasMoved = isMoved();
        const bool wasResized = isResized();
        const Okular::NormalizedRect finalBoundingRect = m_hasPreviewBoundingRect ? m_previewBoundingRect : m_focusedAnnotation.annotation->boundingRectangle();
        m_focusedAnnotation.annotation->setBoundingRectangle(m_originalBoundingRect);
        m_focusedAnnotation.annotation->setFlags(m_focusedAnnotation.annotation->flags() & ~(Okular::Annotation::BeingMoved | Okular::Annotation::BeingResized));

        if (isUsableRect(finalBoundingRect) && isUsableRect(m_originalBoundingRect)) {
            if (wasMoved) {
                const Okular::NormalizedPoint delta(finalBoundingRect.left - m_originalBoundingRect.left, finalBoundingRect.top - m_originalBoundingRect.top);
                if (!isZeroDelta(delta)) {
                    logLatexCalloutInteraction("finish-move-before-translate",
                                               m_focusedAnnotation.annotation,
                                               {QStringLiteral("delta: %1,%2").arg(delta.x).arg(delta.y),
                                                QStringLiteral("final rect: %1,%2,%3,%4").arg(finalBoundingRect.left).arg(finalBoundingRect.top).arg(finalBoundingRect.right).arg(finalBoundingRect.bottom)});
                    m_document->translatePageAnnotation(m_focusedAnnotation.pageNumber, m_focusedAnnotation.annotation, delta);
                    logLatexCalloutInteraction("finish-move-after-translate", m_focusedAnnotation.annotation);
                }
            } else if (wasResized) {
                if (LatexNoteUtils::annotationIsLatex(m_focusedAnnotation.annotation)) {
                    const Okular::NormalizedRect latexResizeRect = m_hasLatexResizeLayoutRect ? m_latexResizeLayoutRect : finalBoundingRect;
                    if (!updateLatexNoteAfterResizeAsync(m_focusedAnnotation, latexResizeRect, m_handle, m_focusedAnnotation.pageViewItem->page()->rotation())) {
                        updateViewport(m_focusedAnnotation);
                    }
                    m_hasLatexResizeLayoutRect = false;
                    m_hasOriginalBoundingRect = false;
                    m_hasPreviewBoundingRect = false;
                    return;
                }
                const Okular::NormalizedPoint delta1(finalBoundingRect.left - m_originalBoundingRect.left, finalBoundingRect.top - m_originalBoundingRect.top);
                const Okular::NormalizedPoint delta2(finalBoundingRect.right - m_originalBoundingRect.right, finalBoundingRect.bottom - m_originalBoundingRect.bottom);
                if (!isZeroDelta(delta1) || !isZeroDelta(delta2)) {
                    m_document->adjustPageAnnotation(m_focusedAnnotation.pageNumber, m_focusedAnnotation.annotation, delta1, delta2);
                }
            }
        }

        m_hasOriginalBoundingRect = false;
        m_hasPreviewBoundingRect = false;
        m_hasLatexResizeLayoutRect = false;
        return;
    }

    if (isMoved()) {
        m_focusedAnnotation.annotation->setFlags(m_focusedAnnotation.annotation->flags() & ~Okular::Annotation::BeingMoved);
        m_document->translatePageAnnotation(m_focusedAnnotation.pageNumber, m_focusedAnnotation.annotation, Okular::NormalizedPoint(0.0, 0.0));
    } else if (isResized()) {
        m_focusedAnnotation.annotation->setFlags(m_focusedAnnotation.annotation->flags() & ~Okular::Annotation::BeingResized);
        m_document->adjustPageAnnotation(m_focusedAnnotation.pageNumber, m_focusedAnnotation.annotation, Okular::NormalizedPoint(0.0, 0.0), Okular::NormalizedPoint(0.0, 0.0));
    }
}

void MouseAnnotation::rollbackCommand()
{
    if (m_hasOriginalCalloutGeometry) {
        restoreOriginalCalloutGeometry(m_focusedAnnotation);
        m_focusedAnnotation.annotation->setFlags(m_focusedAnnotation.annotation->flags() & ~Okular::Annotation::BeingResized);
        m_hasOriginalCalloutGeometry = false;
        updateViewport(m_focusedAnnotation);
        return;
    }

    if (m_hasOriginalLineGeometry) {
        restoreOriginalLineGeometry(m_focusedAnnotation);
        m_focusedAnnotation.annotation->setFlags(m_focusedAnnotation.annotation->flags() & ~Okular::Annotation::BeingResized);
        m_hasOriginalLineGeometry = false;
        m_originalLinePoints.clear();
        m_linePointHandleIndex = -1;
        m_hasOriginalBoundingRect = false;
        m_hasPreviewBoundingRect = false;
        updateViewport(m_focusedAnnotation);
        return;
    }

    if (m_hasOriginalBoundingRect) {
        m_focusedAnnotation.annotation->setBoundingRectangle(m_originalBoundingRect);
        m_focusedAnnotation.annotation->setFlags(m_focusedAnnotation.annotation->flags() & ~(Okular::Annotation::BeingMoved | Okular::Annotation::BeingResized));
        m_hasOriginalBoundingRect = false;
        m_hasPreviewBoundingRect = false;
        m_hasLatexResizeLayoutRect = false;
        updateViewport(m_focusedAnnotation);
        return;
    }

    finishCommand();
}

/* Tell viewport widget that the rectangular of the given annotation needs to be repainted. */
void MouseAnnotation::updateViewport(const AnnotationDescription &ad) const
{
    const QRect &changedPageViewItemRect = getFullBoundingRect(ad);
    if (changedPageViewItemRect.isValid()) {
        m_pageView->viewport()->update(changedPageViewItemRect.translated(ad.pageViewItem->uncroppedGeometry().topLeft()).translated(-m_pageView->contentAreaPosition()));
    }
}

QRect MouseAnnotation::controlGeometryForInteraction(const AnnotationDescription &ad) const
{
    if (!ad.isValid()) {
        return {};
    }

    if (m_focusedAnnotation == ad) {
        if (LatexNoteUtils::annotationIsLatex(ad.annotation) && isResized() && m_hasLatexResizeLayoutRect) {
            return geometryForBoundingRect(ad, m_latexResizeLayoutRect);
        }
        if (LatexNoteUtils::annotationIsLatex(ad.annotation) && m_hasPendingLatexResizePreview && ad.annotation->uniqueName() == m_pendingLatexResizeAnnotationUniqueName) {
            return geometryForBoundingRect(ad, m_pendingLatexResizePreviewRect);
        }
        if (m_hasPreviewBoundingRect) {
            return geometryForBoundingRect(ad, m_previewBoundingRect);
        }
    }

    return controlGeometry(ad);
}

QRect MouseAnnotation::viewportRectForPageRect(const QRect &rect, const AnnotationDescription &ad) const
{
    if (!ad.isValid() || !rect.isValid()) {
        return {};
    }

    return rect.translated(ad.pageViewItem->uncroppedGeometry().topLeft()).translated(-m_pageView->contentAreaPosition());
}

QRect MouseAnnotation::getLatexWarningMarkerRect(const AnnotationDescription &ad) const
{
    if (!hasLatexRenderWarning(ad)) {
        return {};
    }

    const QRect rightHandle = getHandleRect(RH_Right, ad);
    const QPoint markerCenter(rightHandle.right() + latexWarningMarkerSize / 2 + 4, rightHandle.top() - latexWarningMarkerSize / 2);
    return QRect(markerCenter.x() - latexWarningMarkerSize / 2, markerCenter.y() - latexWarningMarkerSize / 2, latexWarningMarkerSize, latexWarningMarkerSize);
}

bool MouseAnnotation::hasLatexRenderWarning(const AnnotationDescription &ad) const
{
    return ad.isValid() && ad.annotation == m_latexRenderWarningAnnotation && m_latexRenderWarning.isValid();
}

void MouseAnnotation::refreshLatexRenderWarning(const AnnotationDescription &ad)
{
    if (!ad.isValid()) {
        clearLatexRenderWarning();
        return;
    }

    if (m_latexRenderWarningAnnotation && m_latexRenderWarningAnnotation != ad.annotation) {
        clearLatexRenderWarning();
        return;
    }

    if (hasLatexRenderWarning(ad)) {
        updateViewport(ad);
    }
}

void MouseAnnotation::setLatexRenderWarning(const AnnotationDescription &ad, const GuiUtils::LatexRenderWarning &warning)
{
    if (!ad.isValid() || !warning.isValid()) {
        clearLatexRenderWarning();
        return;
    }

    m_latexRenderWarningAnnotation = ad.annotation;
    m_latexRenderWarning = warning;
    updateViewport(ad);
}

void MouseAnnotation::clearLatexRenderWarning()
{
    m_latexRenderWarningAnnotation = nullptr;
    m_latexRenderWarning = {};
}

/* eventPos: Mouse position in uncropped page coordinates.
   ad: The annotation to get the handle for. */
MouseAnnotation::ResizeHandle MouseAnnotation::getHandleAt(const QPoint eventPos, const AnnotationDescription &ad) const
{
    ResizeHandle selected = RH_None;
    m_linePointHandleIndex = -1;

    if (hasUsableLinePoints(ad.annotation)) {
        const auto *lineAnn = static_cast<const Okular::LineAnnotation *>(ad.annotation);
        for (int i = 0; i < lineAnn->transformedLinePoints().count(); ++i) {
            if (getLinePointHandleRect(i, ad).contains(eventPos)) {
                m_linePointHandleIndex = i;
                return RH_LinePoint;
            }
        }
    }

    if (ad.annotation->canBeResized()) {
        if (calloutAnnotation(ad.annotation)) {
            for (int i = 0; i < 3; ++i) {
                const ResizeHandle handle = calloutHandleForIndex(i);
                const QRect rect = getHandleRect(handle, ad);
                if (rect.contains(eventPos)) {
                    return handle;
                }
            }
        }

        for (const ResizeHandle &handle : m_resizeHandleList) {
            const QRect rect = getHandleRect(handle, ad);
            if (rect.contains(eventPos)) {
                selected |= handle;
            }
        }

        /*
         * Handles may overlap when selection is very small.
         * Then it can happen that cursor is over more than one handles,
         * and therefore maybe more than two flags are set.
         * Favor one handle in that case.
         */
        if ((selected & RH_BottomRight) == RH_BottomRight) {
            return RH_BottomRight;
        }
        if ((selected & RH_TopRight) == RH_TopRight) {
            return RH_TopRight;
        }
        if ((selected & RH_TopLeft) == RH_TopLeft) {
            return RH_TopLeft;
        }
        if ((selected & RH_BottomLeft) == RH_BottomLeft) {
            return RH_BottomLeft;
        }
    }

    if (selected == RH_None && ad.annotation->canBeMoved()) {
        const QRect boundingRect = Okular::AnnotationUtils::annotationGeometry(ad.annotation, ad.pageViewItem->uncroppedWidth(), ad.pageViewItem->uncroppedHeight());
        if (boundingRect.contains(eventPos)) {
            return RH_Content;
        }
    }

    return selected;
}

/* Get the rectangle for a specified resizie handle. */
QRect MouseAnnotation::getHandleRect(ResizeHandle handle, const AnnotationDescription &ad) const
{
    if (isCalloutHandle(handle)) {
        const Okular::Annotation *calloutAnn = calloutAnnotation(ad.annotation);
        const int pointIndex = calloutIndexForHandle(handle);
        if (!hasUsableCalloutPoints(calloutAnn) || pointIndex < 0) {
            return {};
        }

        const Okular::NormalizedPoint point = calloutPoint(calloutAnn, pointIndex, true);
        const int left = qRound(point.x * ad.pageViewItem->uncroppedWidth()) - handleSizeHalf;
        const int top = qRound(point.y * ad.pageViewItem->uncroppedHeight()) - handleSizeHalf;
        return QRect(left, top, handleSize, handleSize);
    }

    QRect boundingRect = controlGeometryForInteraction(ad);
    int left, top;

    if (handle & RH_Top) {
        top = boundingRect.top() - handleSizeHalf;
    } else if (handle & RH_Bottom) {
        top = boundingRect.bottom() - handleSizeHalf;
    } else {
        top = boundingRect.top() + boundingRect.height() / 2 - handleSizeHalf;
    }

    if (handle & RH_Left) {
        left = boundingRect.left() - handleSizeHalf;
    } else if (handle & RH_Right) {
        left = boundingRect.right() - handleSizeHalf;
    } else {
        left = boundingRect.left() + boundingRect.width() / 2 - handleSizeHalf;
    }

    return QRect(left, top, handleSize, handleSize);
}

QRect MouseAnnotation::getLinePointHandleRect(int pointIndex, const AnnotationDescription &ad) const
{
    const auto *lineAnn = lineAnnotation(ad.annotation);
    if (!lineAnn || pointIndex < 0 || pointIndex >= lineAnn->transformedLinePoints().count()) {
        return {};
    }

    const Okular::NormalizedPoint point = lineAnn->transformedLinePoints().at(pointIndex);
    const int left = qRound(point.x * ad.pageViewItem->uncroppedWidth()) - handleSizeHalf;
    const int top = qRound(point.y * ad.pageViewItem->uncroppedHeight()) - handleSizeHalf;
    return QRect(left, top, handleSize, handleSize);
}

QRect MouseAnnotation::getLinePointsRect(const AnnotationDescription &ad) const
{
    const auto *lineAnn = lineAnnotation(ad.annotation);
    if (!lineAnn || lineAnn->transformedLinePoints().isEmpty()) {
        return {};
    }

    QRect lineRect;
    const QList<Okular::NormalizedPoint> points = lineAnn->transformedLinePoints();
    for (int i = 0; i < points.count(); ++i) {
        const Okular::NormalizedPoint point = points.at(i);
        const QPoint pagePoint(qRound(point.x * ad.pageViewItem->uncroppedWidth()), qRound(point.y * ad.pageViewItem->uncroppedHeight()));
        if (i == 0) {
            lineRect = QRect(pagePoint, QSize(1, 1));
        } else {
            lineRect = lineRect.united(QRect(pagePoint, QSize(1, 1)));
        }
    }

    return lineRect.adjusted(-handleSizeHalf, -handleSizeHalf, handleSizeHalf, handleSizeHalf);
}

QRect MouseAnnotation::getCalloutLineRect(const AnnotationDescription &ad) const
{
    const Okular::Annotation *calloutAnn = calloutAnnotation(ad.annotation);
    if (!hasUsableCalloutPoints(calloutAnn)) {
        return {};
    }

    QRect lineRect;
    for (int i = 0; i < 3; ++i) {
        const Okular::NormalizedPoint point = calloutPoint(calloutAnn, i, true);
        const QPoint pagePoint(qRound(point.x * ad.pageViewItem->uncroppedWidth()), qRound(point.y * ad.pageViewItem->uncroppedHeight()));
        if (i == 0) {
            lineRect = QRect(pagePoint, QSize(1, 1));
        } else {
            lineRect = lineRect.united(QRect(pagePoint, QSize(1, 1)));
        }
    }

    return lineRect.adjusted(-handleSizeHalf, -handleSizeHalf, handleSizeHalf, handleSizeHalf);
}

void MouseAnnotation::rememberOriginalCalloutGeometry(const AnnotationDescription &ad)
{
    const Okular::Annotation *calloutAnn = calloutAnnotation(ad.annotation);
    if (!hasUsableCalloutPoints(calloutAnn)) {
        m_hasOriginalCalloutGeometry = false;
        return;
    }

    for (int i = 0; i < 3; ++i) {
        m_originalCalloutPoints[i] = calloutPoint(calloutAnn, i, false);
    }
    m_originalCalloutBoundingRect = calloutAnn->boundingRectangle();
    m_hasOriginalCalloutGeometry = true;
}

void MouseAnnotation::restoreOriginalCalloutGeometry(const AnnotationDescription &ad)
{
    Okular::Annotation *calloutAnn = calloutAnnotation(ad.annotation);
    if (!calloutAnn || !m_hasOriginalCalloutGeometry) {
        return;
    }

    calloutAnn->setBoundingRectangle(m_originalCalloutBoundingRect);
    for (int i = 0; i < 3; ++i) {
        setCalloutPoint(calloutAnn, m_originalCalloutPoints[i], i);
    }
}

void MouseAnnotation::rememberOriginalLineGeometry(const AnnotationDescription &ad)
{
    const auto *lineAnn = lineAnnotation(ad.annotation);
    if (!hasUsableLinePoints(lineAnn)) {
        m_hasOriginalLineGeometry = false;
        return;
    }

    m_originalLinePoints = lineAnn->linePoints();
    m_originalLineBoundingRect = lineAnn->boundingRectangle();
    m_hasOriginalLineGeometry = true;
}

void MouseAnnotation::restoreOriginalLineGeometry(const AnnotationDescription &ad)
{
    auto *lineAnn = lineAnnotation(ad.annotation);
    if (!lineAnn || !m_hasOriginalLineGeometry) {
        return;
    }

    lineAnn->setLinePoints(m_originalLinePoints);
    lineAnn->setBoundingRectangle(m_originalLineBoundingRect);
}

/* Convert a resize handle delta into two adjust delta coordinates. */
void MouseAnnotation::handleToAdjust(const QPointF dIn, QPointF &dOut1, QPointF &dOut2, MouseAnnotation::ResizeHandle handle, Okular::Rotation rotation)
{
    const MouseAnnotation::ResizeHandle rotatedHandle = MouseAnnotation::rotateHandle(handle, rotation);
    dOut1.rx() = (rotatedHandle & MouseAnnotation::RH_Left) ? dIn.x() : 0;
    dOut1.ry() = (rotatedHandle & MouseAnnotation::RH_Top) ? dIn.y() : 0;
    dOut2.rx() = (rotatedHandle & MouseAnnotation::RH_Right) ? dIn.x() : 0;
    dOut2.ry() = (rotatedHandle & MouseAnnotation::RH_Bottom) ? dIn.y() : 0;
}

QPointF MouseAnnotation::rotateInRect(const QPointF rotated, Okular::Rotation rotation)
{
    QPointF ret;

    switch (rotation) {
    case Okular::Rotation90:
        ret = QPointF(rotated.y(), -rotated.x());
        break;
    case Okular::Rotation180:
        ret = QPointF(-rotated.x(), -rotated.y());
        break;
    case Okular::Rotation270:
        ret = QPointF(-rotated.y(), rotated.x());
        break;
    case Okular::Rotation0: /* no modifications */
    default:                /* other cases */
        ret = rotated;
    }

    return ret;
}

MouseAnnotation::ResizeHandle MouseAnnotation::rotateHandle(MouseAnnotation::ResizeHandle handle, Okular::Rotation rotation)
{
    unsigned int rotatedHandle = 0;
    switch (rotation) {
    case Okular::Rotation90:
        /* bit rotation: #1 => #4, #2 => #1, #3 => #2, #4 => #3 */
        rotatedHandle = (handle << 3 | handle >> (4 - 3)) & RH_AllHandles;
        break;
    case Okular::Rotation180:
        /* bit rotation: #1 => #3, #2 => #4, #3 => #1, #4 => #2 */
        rotatedHandle = (handle << 2 | handle >> (4 - 2)) & RH_AllHandles;
        break;
    case Okular::Rotation270:
        /* bit rotation: #1 => #2, #2 => #3, #3 => #4, #4 => #1 */
        rotatedHandle = (handle << 1 | handle >> (4 - 1)) & RH_AllHandles;
        break;
    case Okular::Rotation0: /* no modifications */
    default:                /* other cases */
        rotatedHandle = handle;
        break;
    }
    return (MouseAnnotation::ResizeHandle)rotatedHandle;
}

/* Start according action for AMovie/ARichMedia/AScreen/AFileAttachment.
 * It was formerly (before mouse annotation refactoring) called on mouse release event.
 * Now it's called on mouse press. Should we keep the former behavior? */
void MouseAnnotation::processAction(const AnnotationDescription &ad)
{
    if (ad.isValid()) {
        Okular::Annotation *ann = ad.annotation;
        PageViewItem *pageItem = ad.pageViewItem;

        if (ann->subType() == Okular::Annotation::AMovie) {
            VideoWidget *vw = pageItem->videoWidgets().value(static_cast<Okular::MovieAnnotation *>(ann)->movie());
            vw->show();
            vw->play();
        } else if (ann->subType() == Okular::Annotation::ARichMedia) {
            VideoWidget *vw = pageItem->videoWidgets().value(static_cast<Okular::RichMediaAnnotation *>(ann)->movie());
            vw->show();
            vw->play();
        } else if (ann->subType() == Okular::Annotation::AScreen) {
            m_pageView->processActionForView(static_cast<Okular::ScreenAnnotation *>(ann)->action());
        } else if (ann->subType() == Okular::Annotation::AFileAttachment) {
            const Okular::FileAttachmentAnnotation *fileAttachAnnot = static_cast<Okular::FileAttachmentAnnotation *>(ann);
            GuiUtils::saveEmbeddedFile(fileAttachAnnot->embeddedFile(), m_pageView);
        }
    }
}
