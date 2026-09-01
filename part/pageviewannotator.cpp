/*
    SPDX-FileCopyrightText: 2005 Enrico Ros <eros.kde@email.it>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "pageviewannotator.h"

// qt / kde includes
#include <KConfigGroup>
#include <KLocalizedString>
#include <KMessageBox>
#include <KSharedConfig>
#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QInputDialog>
#include <QImageReader>
#include <QList>
#include <QLoggingCategory>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QVariant>

#include <KUser>
#include <QDebug>
#include <QMenu>
#include <QSizeF>

// system includes
#include <QKeyEvent>
#include <QStandardPaths>
#include <QTabletEvent>
#include <cmath>
#include <math.h>
#include <memory>

// local includes
#include "annotationactionhandler.h"
#include "core/annotations.h"
#include "core/area.h"
#include "core/document.h"
#include "core/latexnotegeometry.h"
#include "core/page.h"
#include "core/signatureutils.h"
#include "core/utils.h"
#include "editannottooldialog.h"
#include "gui/debug_ui.h"
#include "gui/guiutils.h"
#include "latexnoteutils.h"
#include "pageview.h"
#include "settings.h"
#include "signaturepartutils.h"
#include "templatenoteutils.h"

/** @short PickPointEngine */
class PickPointEngine : public AnnotatorEngine
{
public:
    explicit PickPointEngine(PageView *pageView, const QDomElement &engineElement)
        : AnnotatorEngine(engineElement)
        , clicked(false)
        , xscale(1.0)
        , yscale(1.0)
        , hoverIconName {engineElement.attribute(QStringLiteral("hoverIcon"))}
        , iconName {m_annotElement.attribute(QStringLiteral("icon"))}
        , stampImagePath {m_annotElement.attribute(QStringLiteral("imagePath"))}
        , latexAppearancePdfFileName {m_annotElement.attribute(QStringLiteral("latexAppearancePdfFileName"))}
        , latexStamp(m_annotElement.attribute(QStringLiteral("okularLatex")).toInt() != 0)
        , boxedLatexStamp(m_annotElement.attribute(QStringLiteral("latexBoxed")).toInt() != 0)
        , fixedAspectRatio(0.0)
        , hasIntrinsicStampSizeInPoints(false)
        , hasIntrinsicStampSize(false)
        , intrinsicStampDpiX(0.0)
        , intrinsicStampDpiY(0.0)
        , pagewidth(1.0)
        , pageheight(1.0)
        , pageView(pageView)
    {
        const QString stampAppearanceSource = stampImagePath.isEmpty() ? iconName : stampImagePath;
        // parse engine specific attributes
        if (m_annotElement.attribute(QStringLiteral("type")) == QLatin1String("Stamp") && !stampAppearanceSource.simplified().isEmpty()) {
            hoverIconName = stampAppearanceSource;
        }
        center = QVariant(engineElement.attribute(QStringLiteral("center"))).toBool();
        bool ok = true;
        size = engineElement.attribute(QStringLiteral("size"), QStringLiteral("32")).toInt(&ok);
        if (!ok) {
            size = 32;
        }
        m_block = QVariant(engineElement.attribute(QStringLiteral("block"))).toBool();

        // create engine objects
        if (!hoverIconName.simplified().isEmpty()) {
            pixmap = GuiUtils::stampPixmap(hoverIconName, QSize(size, size));
        }
        if (m_annotElement.attribute(QStringLiteral("type")) == QLatin1String("Stamp") && QFile::exists(stampAppearanceSource)) {
            QImageReader reader(stampAppearanceSource);
            const QImage image = reader.read();
            if (!image.isNull()) {
                intrinsicStampSize = image.size();
                if (image.dotsPerMeterX() > 0) {
                    intrinsicStampDpiX = image.dotsPerMeterX() * 0.0254;
                }
                if (image.dotsPerMeterY() > 0) {
                    intrinsicStampDpiY = image.dotsPerMeterY() * 0.0254;
                }
            }
            if (intrinsicStampSize.isEmpty()) {
                intrinsicStampSize = reader.size();
            }
            hasIntrinsicStampSize = !intrinsicStampSize.isEmpty();
        }
        if (m_annotElement.attribute(QStringLiteral("type")) == QLatin1String("Stamp") && latexStamp && !latexAppearancePdfFileName.isEmpty()) {
            QSizeF latexSizePoints = GuiUtils::pdfPageSizeInPoints(latexAppearancePdfFileName);
            bool ok = false;
            const double layoutWidth = m_annotElement.attribute(QStringLiteral("latexLayoutWidth")).toDouble(&ok);
            bool paddingOk = false;
            const double configuredPadding = m_annotElement.attribute(QStringLiteral("latexPadding")).toDouble(&paddingOk);
            const double padding = paddingOk && configuredPadding >= 0.0 ? configuredPadding : Okular::LatexNoteGeometry::defaultPaddingPoints();
            if (ok && layoutWidth > 0.0) {
                latexSizePoints = LatexNoteUtils::visualSizeForLatexTextAnnotation(latexSizePoints, layoutWidth, padding);
            } else {
                latexSizePoints = LatexNoteUtils::visualSizeForLatexTextAnnotation(latexSizePoints, 0.0, padding);
            }
            if (latexSizePoints.isValid() && !latexSizePoints.isEmpty()) {
                intrinsicStampSizeInPoints = latexSizePoints;
                hasIntrinsicStampSizeInPoints = true;
            }
        }
        if (m_annotElement.attribute(QStringLiteral("type")) == QLatin1String("Stamp") && !latexStamp && hasIntrinsicStampSizeInPoints && intrinsicStampSizeInPoints.height() > 0.0) {
            fixedAspectRatio = intrinsicStampSizeInPoints.width() / intrinsicStampSizeInPoints.height();
        } else if (m_annotElement.attribute(QStringLiteral("type")) == QLatin1String("Stamp") && !latexStamp && !pixmap.isNull() && pixmap.height() > 0) {
            fixedAspectRatio = double(pixmap.width()) / pixmap.height();
        }
    }

    QRect event(EventType type, Button button, Modifiers modifiers, double nX, double nY, double xScale, double yScale, const Okular::Page *page) override
    {
        xscale = xScale;
        yscale = yScale;
        const QSizeF dpi = Okular::Utils::realDpi(nullptr);
        const double dpiX = dpi.width() > 0.0 && std::isfinite(dpi.width()) ? dpi.width() : 72.0;
        const double dpiY = dpi.height() > 0.0 && std::isfinite(dpi.height()) ? dpi.height() : 72.0;
        pagewidth = page->width() * 72.0 / dpiX;
        pageheight = page->height() * 72.0 / dpiY;
        // only proceed if pressing left button
        if (button != Left) {
            return QRect();
        }

        // start operation on click
        if (type == Press && clicked == false) {
            clicked = true;
            startpoint.x = nX;
            startpoint.y = nY;
        }
        // repaint if moving while pressing
        else if (type == Move && clicked == true) {
        }
        // operation finished on release
        else if (type == Release && clicked == true) {
            m_creationCompleted = true;
        } else {
            return QRect();
        }

        if (std::isfinite(fixedAspectRatio) && fixedAspectRatio > 0.0 && xScale > 0.0 && yScale > 0.0) {
            const double width = qAbs(nX - startpoint.x) * xScale;
            const double height = qAbs(nY - startpoint.y) * yScale;
            if (width > 0.0 && height > 0.0) {
                if (width / height > fixedAspectRatio) {
                    const double direction = nX >= startpoint.x ? 1.0 : -1.0;
                    nX = startpoint.x + direction * (height * fixedAspectRatio) / xScale;
                } else {
                    const double direction = nY >= startpoint.y ? 1.0 : -1.0;
                    nY = startpoint.y + direction * (width / fixedAspectRatio) / yScale;
                }
            }
        }
        // Constrain to 1:1 form factor (e.g. circle or square)
        else if (modifiers.constrainRatioAndAngle) {
            double side = qMin(qAbs(nX - startpoint.x) * xScale, qAbs(nY - startpoint.y) * yScale);
            nX = qBound(startpoint.x - side / xScale, nX, startpoint.x + side / xScale);
            nY = qBound(startpoint.y - side / yScale, nY, startpoint.y + side / yScale);
        }
        // update variables and extents (zoom invariant rect)
        point.x = nX;
        point.y = nY;
        if (center) {
            rect.left = nX - (size / (xScale * 2.0));
            rect.top = nY - (size / (yScale * 2.0));
        } else {
            rect.left = nX;
            rect.top = nY;
        }
        rect.right = rect.left + size;
        rect.bottom = rect.top + size;
        QRect boundrect = rect.geometry((int)xScale, (int)yScale).adjusted(0, 0, 1, 1);
        if (m_block) {
            const Okular::NormalizedRect tmprect(qMin(startpoint.x, point.x), qMin(startpoint.y, point.y), qMax(startpoint.x, point.x), qMax(startpoint.y, point.y));
            boundrect |= tmprect.geometry((int)xScale, (int)yScale).adjusted(0, 0, 1, 1);
        } else if (isCallout()) {
            const Okular::NormalizedRect tmprect(qMin(startpoint.x, point.x), qMin(startpoint.y, point.y), qMax(startpoint.x, point.x), qMax(startpoint.y, point.y));
            boundrect |= tmprect.geometry((int)xScale, (int)yScale).adjusted(-4, -4, 4, 4);
        }
        return boundrect;
    }

    void paint(QPainter *painter, double xScale, double yScale, const QRect & /*clipRect*/) override
    {
        if (clicked) {
            if (m_block) {
                const Okular::NormalizedRect tmprect(qMin(startpoint.x, point.x), qMin(startpoint.y, point.y), qMax(startpoint.x, point.x), qMax(startpoint.y, point.y));
                const QRect realrect = tmprect.geometry((int)xScale, (int)yScale);
                const QPen origpen = painter->pen();
                QPen pen = painter->pen();
                // Draw rectangle in 3 colored dashed line
                const QColor baseColor = QApplication::palette().base().color();
                const QColor textColor = QApplication::palette().text().color();
                const QColor highlightColor = QApplication::palette().highlight().color();
                const qreal penWidth = pen.widthF();
                pen.setDashPattern({penWidth * 5, penWidth * 10} /* { dash, empty space } */);
                pen.setColor(baseColor);
                painter->setPen(pen);
                // Use drawLine instead of drawRect to avoid shimering on resize
                painter->drawLine(realrect.topLeft(), realrect.topRight());
                painter->drawLine(realrect.topLeft(), realrect.bottomLeft());
                painter->drawLine(realrect.topRight(), realrect.bottomRight());
                painter->drawLine(realrect.bottomLeft(), realrect.bottomRight());

                pen.setDashOffset(penWidth * 5);
                pen.setColor(textColor);
                painter->setPen(pen);
                painter->drawLine(realrect.topLeft(), realrect.topRight());
                painter->drawLine(realrect.topLeft(), realrect.bottomLeft());
                painter->drawLine(realrect.topRight(), realrect.bottomRight());
                painter->drawLine(realrect.bottomLeft(), realrect.bottomRight());

                pen.setDashOffset(penWidth * 10);
                pen.setColor(highlightColor);
                painter->setPen(pen);
                painter->drawLine(realrect.topLeft(), realrect.topRight());
                painter->drawLine(realrect.topLeft(), realrect.bottomLeft());
                painter->drawLine(realrect.topRight(), realrect.bottomRight());
                painter->drawLine(realrect.bottomLeft(), realrect.bottomRight());

                // and draw the internal 3x3 grid
                pen.setStyle(Qt::DashLine);
                pen.setColor(highlightColor);
                painter->setPen(pen);
                const QPoint rectTopLeft = realrect.topLeft();
                const QPoint rectBottomRight = realrect.bottomRight();
                const int yHorizontal1 = rectTopLeft.y() + (realrect.height() + 2) / 3;
                const int yHorizontal2 = rectTopLeft.y() + (realrect.height() + 2) / 3 * 2;
                painter->drawLine(rectTopLeft.x(), yHorizontal1, rectBottomRight.x(), yHorizontal1);
                painter->drawLine(rectTopLeft.x(), yHorizontal2, rectBottomRight.x(), yHorizontal2);
                const int xVertical1 = rectTopLeft.x() + (realrect.width() + 2) / 3;
                const int xVertical2 = rectTopLeft.x() + (realrect.width() + 2) / 3 * 2;
                painter->drawLine(xVertical1, rectTopLeft.y(), xVertical1, rectBottomRight.y());
                painter->drawLine(xVertical2, rectTopLeft.y(), xVertical2, rectBottomRight.y());
                painter->setPen(origpen);
            }
            if (!pixmap.isNull()) {
                if (m_block) {
                    const Okular::NormalizedRect tmprect(qMin(startpoint.x, point.x), qMin(startpoint.y, point.y), qMax(startpoint.x, point.x), qMax(startpoint.y, point.y));
                    painter->drawPixmap(tmprect.geometry((int)xScale, (int)yScale), pixmap);
                } else {
                    painter->drawPixmap(QPointF(rect.left * xScale, rect.top * yScale), pixmap);
                }
            } else if (isCallout()) {
                const QPen origpen = painter->pen();
                QPen pen = painter->pen();
                pen.setColor(m_annotElement.hasAttribute(QStringLiteral("borderColor")) ? QColor(m_annotElement.attribute(QStringLiteral("borderColor"))) : m_engineColor);
                painter->setPen(pen);
                painter->drawLine(QPointF(startpoint.x * xScale, startpoint.y * yScale), QPointF(point.x * xScale, point.y * yScale));

                const Okular::NormalizedRect previewRect = calloutBoxRect(QString());
                if (m_annotElement.hasAttribute(QStringLiteral("color"))) {
                    painter->setBrush(QColor(m_annotElement.attribute(QStringLiteral("color"))));
                }
                painter->drawRect(previewRect.geometry((int)xScale, (int)yScale));
                painter->setBrush(Qt::NoBrush);
                painter->setPen(origpen);
            }
        }
    }

    void addInPlaceTextAnnotation(Okular::Annotation *&ann, const QString &summary, const QString &content, Okular::TextAnnotation::InplaceIntent inplaceIntent)
    {
        Okular::TextAnnotation *ta = new Okular::TextAnnotation();
        ann = ta;
        ta->setFlags(ta->flags() | Okular::Annotation::FixedRotation);
        ta->setContents(m_annotElement.hasAttribute(QStringLiteral("contents")) ? m_annotElement.attribute(QStringLiteral("contents")) : content);
        ta->setTextType(Okular::TextAnnotation::InPlace);
        ta->setInplaceIntent(inplaceIntent);
        ta->setTextFontName(QStringLiteral("Helvetica"));
        if (m_annotElement.hasAttribute(QStringLiteral("templateNoteData"))) {
            ta->setTemplateNoteData(m_annotElement.attribute(QStringLiteral("templateNoteData")));
        }
        // set alignment
        if (m_annotElement.hasAttribute(QStringLiteral("align"))) {
            ta->setInplaceAlignment(m_annotElement.attribute(QStringLiteral("align")).toInt());
        }
        // set font
        if (m_annotElement.hasAttribute(QStringLiteral("font"))) {
            QFont f;
            // Workaround broken old code that saved fonts incorrectly with extra backslashes
            QString fontString = m_annotElement.attribute(QStringLiteral("font"));
            if (fontString.count(QStringLiteral("\\\\,")) > 9) {
                fontString.replace(QStringLiteral("\\\\,"), QStringLiteral(","));
            }
            f.fromString(fontString);
            if (f.pointSizeF() > 0) {
                ta->setTextFontPointSize(f.pointSizeF());
            }
        } else if (m_annotElement.hasAttribute(QStringLiteral("fontName"))) {
            ta->setTextFontName(m_annotElement.attribute(QStringLiteral("fontName")));
        }
        if (m_annotElement.hasAttribute(QStringLiteral("fontSize"))) {
            ta->setTextFontPointSize(m_annotElement.attribute(QStringLiteral("fontSize")).toDouble());
        }
        // set font color
        if (m_annotElement.hasAttribute(QStringLiteral("textColor"))) {
            ta->setTextColor(QColor(m_annotElement.attribute(QStringLiteral("textColor"))));
        } else if (inplaceIntent != Okular::TextAnnotation::TypeWriter) {
            ta->setTextColor(Qt::black);
        }
        if (m_annotElement.hasAttribute(QStringLiteral("borderColor"))) {
            ta->setInplaceBorderColor(QColor(m_annotElement.attribute(QStringLiteral("borderColor"))));
        } else if (m_engineColor.isValid()) {
            ta->setInplaceBorderColor(m_engineColor);
        }
        // set width
        if (m_annotElement.hasAttribute(QStringLiteral("width"))) {
            ta->style().setWidth(m_annotElement.attribute(QStringLiteral("width")).toDouble());
        }
        // set boundary
        rect.left = qMin(startpoint.x, point.x);
        rect.top = qMin(startpoint.y, point.y);
        rect.right = qMax(startpoint.x, point.x);
        rect.bottom = qMax(startpoint.y, point.y);
        qCDebug(OkularUiDebug).nospace() << "xyScale=" << xscale << "," << yscale;
        static const int padding = 2;
        const QFontMetricsF mf(ta->textFont());
        const QRectF rcf =
            mf.boundingRect(Okular::NormalizedRect(rect.left, rect.top, 1.0, 1.0).geometry((int)pagewidth, (int)pageheight).adjusted(padding, padding, -padding, -padding), Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap, ta->contents());
        rect.right = qMax(rect.right, rect.left + (rcf.width() + padding * 2) / pagewidth);
        rect.bottom = qMax(rect.bottom, rect.top + (rcf.height() + padding * 2) / pageheight);
        ta->window().setSummary(summary);
    }

    void addCalloutAnnotation(Okular::Annotation *&ann, const QString &content)
    {
        addInPlaceTextAnnotation(ann, i18n("Callout"), content, Okular::TextAnnotation::Callout);

        Okular::TextAnnotation *ta = static_cast<Okular::TextAnnotation *>(ann);
        rect = calloutBoxRect(content);
        const Okular::NormalizedPoint connection = calloutConnectionPoint(rect);
        Okular::NormalizedPoint elbow((startpoint.x + connection.x) / 2.0, connection.y);
        if (qAbs(startpoint.x - connection.x) < 0.02) {
            elbow.x = connection.x;
            elbow.y = (startpoint.y + connection.y) / 2.0;
        }
        ta->setInplaceCallout(startpoint, 0);
        ta->setInplaceCallout(elbow, 1);
        ta->setInplaceCallout(connection, 2);
    }

    QList<Okular::Annotation *> end() override
    {
        // find out annotation's description node
        if (m_annotElement.isNull()) {
            m_creationCompleted = false;
            clicked = false;
            return QList<Okular::Annotation *>();
        }

        // find out annotation's type
        Okular::Annotation *ann = nullptr;
        const QString typeString = m_annotElement.attribute(QStringLiteral("type"));
        // create InPlace TextAnnotation from path
        if (typeString == QLatin1String("FreeText")) {
            addInPlaceTextAnnotation(ann, i18n("Inline Note"), QString(), Okular::TextAnnotation::Unknown);
        } else if (typeString == QLatin1String("Callout")) {
            if (m_annotElement.hasAttribute(QStringLiteral("contents"))) {
                addCalloutAnnotation(ann, QString());
            } else {
                bool resok;
                const QString content = QInputDialog::getMultiLineText(pageView, i18n("New Callout Note"), i18n("Text of the new note:"), QString(), &resok);
                if (resok) {
                    addCalloutAnnotation(ann, content);
                }
            }
        } else if (typeString == QLatin1String("Typewriter")) {
            if (m_annotElement.hasAttribute(QStringLiteral("contents"))) {
                addInPlaceTextAnnotation(ann, i18n("Typewriter"), QString(), Okular::TextAnnotation::TypeWriter);
            } else {
                bool resok;
                const QString content = QInputDialog::getMultiLineText(pageView, i18n("New Text Note"), i18n("Text of the new note:"), QString(), &resok);
                if (resok) {
                    addInPlaceTextAnnotation(ann, i18n("Typewriter"), content, Okular::TextAnnotation::TypeWriter);
                }
            }
        } else if (typeString == QLatin1String("Text")) {
            Okular::TextAnnotation *ta = new Okular::TextAnnotation();
            ann = ta;
            ta->setTextType(Okular::TextAnnotation::Linked);
            ta->setTextIcon(iconName);
            // ta->window.flags &= ~(Okular::Annotation::Hidden);
            const double iconhei = 0.03;
            rect.left = point.x;
            rect.top = point.y;
            rect.right = rect.left + iconhei;
            rect.bottom = rect.top + iconhei * xscale / yscale;
            ta->window().setSummary(i18n("Pop-up Note"));
        }
        // create StampAnnotation from path
        else if (typeString == QLatin1String("Stamp")) {
            Okular::StampAnnotation *sa = new Okular::StampAnnotation();
            ann = sa;
            sa->setStampIconName(iconName);
            if (!stampImagePath.isEmpty()) {
                sa->setStampImagePath(stampImagePath);
            }
            if (m_annotElement.hasAttribute(QStringLiteral("contents"))) {
                sa->setContents(m_annotElement.attribute(QStringLiteral("contents")));
            }
            if (latexStamp) {
                sa->setOkularLatex(true);
                sa->setFlags(sa->flags() | Okular::Annotation::FixedRotation);
                const bool latexCallout = m_annotElement.attribute(QStringLiteral("latexCallout")).toInt() != 0;
                sa->setLatexNoteType(latexCallout ? Okular::Annotation::LatexNoteCallout : (boxedLatexStamp ? Okular::Annotation::LatexNoteBoxed : Okular::Annotation::LatexNotePlain));
                sa->setLatexAppearancePdfFileName(latexAppearancePdfFileName);
                QColor textColor = m_annotElement.hasAttribute(QStringLiteral("textColor")) ? QColor(m_annotElement.attribute(QStringLiteral("textColor"))) : Qt::black;
                if (!textColor.isValid() || textColor.alpha() == 0) {
                    textColor = Qt::black;
                }
                QColor fillColor = boxedLatexStamp ? (m_annotElement.hasAttribute(QStringLiteral("color")) ? QColor(m_annotElement.attribute(QStringLiteral("color"))) : Qt::yellow) : Qt::transparent;
                if (boxedLatexStamp && (!fillColor.isValid() || fillColor.alpha() == 0)) {
                    fillColor = Qt::yellow;
                }
                QColor borderColor = boxedLatexStamp ? (m_annotElement.hasAttribute(QStringLiteral("borderColor")) ? QColor(m_annotElement.attribute(QStringLiteral("borderColor"))) : textColor) : Qt::transparent;
                if (boxedLatexStamp && (!borderColor.isValid() || borderColor.alpha() == 0)) {
                    borderColor = textColor;
                }
                sa->setLatexTextColor(textColor);
                sa->setLatexFillColor(fillColor);
                sa->setLatexBorderColor(borderColor);
                bool ok = false;
                const double layoutWidth = m_annotElement.attribute(QStringLiteral("latexLayoutWidth")).toDouble(&ok);
                if (ok && layoutWidth > 0.0) {
                    sa->setLatexLayoutWidth(layoutWidth);
                } else if (m_annotElement.attribute(QStringLiteral("latexDefaultLayoutWidth")) == QLatin1String("page-third") && pagewidth > 0.0) {
                    sa->setLatexLayoutWidth(pagewidth / 3.0);
                }
                const double padding = m_annotElement.attribute(QStringLiteral("latexPadding")).toDouble(&ok);
                if (ok && padding >= 0.0) {
                    sa->setLatexPadding(padding);
                }
                const double fontSize = m_annotElement.attribute(QStringLiteral("latexFontSize")).toDouble(&ok);
                if (ok && fontSize > 0.0) {
                    sa->setLatexFontSize(fontSize);
                }
                sa->style().setWidth(boxedLatexStamp ? m_annotElement.attribute(QStringLiteral("width"), QStringLiteral("1")).toDouble() : 0.0);
                sa->style().setColor(textColor);
            }
            // set boundary
            rect.left = qMin(startpoint.x, point.x);
            rect.top = qMin(startpoint.y, point.y);
            rect.right = qMax(startpoint.x, point.x);
            rect.bottom = qMax(startpoint.y, point.y);
            const QRectF rcf = rect.geometry((int)xscale, (int)yscale);
            const int ml = (rcf.bottomRight() - rcf.topLeft()).toPoint().manhattanLength();
            if (latexStamp && !latexAppearancePdfFileName.isEmpty() && ml <= QApplication::startDragDistance()) {
                const QSizeF pdfSizePoints = GuiUtils::pdfPageSizeInPoints(latexAppearancePdfFileName);
                const QSizeF visualSizePoints = LatexNoteUtils::visualSizeForLatexTextAnnotation(pdfSizePoints, sa->latexLayoutWidth(), LatexNoteUtils::paddingForLatexAnnotation(sa));
                if (visualSizePoints.isValid() && !visualSizePoints.isEmpty() && pagewidth > 0.0 && pageheight > 0.0) {
                    rect.right = rect.left + visualSizePoints.width() / pagewidth;
                    rect.bottom = rect.top + visualSizePoints.height() / pageheight;
                }
            } else if (ml <= QApplication::startDragDistance()) {
                double stampxscale = xscale > 0.0 ? pixmap.width() / xscale : 0.0;
                double stampyscale = yscale > 0.0 ? pixmap.height() / yscale : 0.0;
                if (hasIntrinsicStampSizeInPoints && pagewidth > 0.0 && pageheight > 0.0) {
                    stampxscale = intrinsicStampSizeInPoints.width() / pagewidth;
                    stampyscale = intrinsicStampSizeInPoints.height() / pageheight;
                    if (stampxscale > 1.0 || stampyscale > 1.0) {
                        const double shrink = qMin(1.0 / stampxscale, 1.0 / stampyscale);
                        stampxscale *= shrink;
                        stampyscale *= shrink;
                    }
                } else if (hasIntrinsicStampSize && pagewidth > 0.0 && pageheight > 0.0) {
                    if (intrinsicStampDpiX > 0.0 && intrinsicStampDpiY > 0.0) {
                        constexpr double pdfDpi = 72.0;
                        stampxscale = intrinsicStampSize.width() * pdfDpi / intrinsicStampDpiX / pagewidth;
                        stampyscale = intrinsicStampSize.height() * pdfDpi / intrinsicStampDpiY / pageheight;
                    } else {
                        stampxscale = intrinsicStampSize.width() / pagewidth;
                        stampyscale = intrinsicStampSize.height() / pageheight;
                    }
                    if (stampxscale > 1.0 || stampyscale > 1.0) {
                        const double shrink = qMin(1.0 / stampxscale, 1.0 / stampyscale);
                        stampxscale *= shrink;
                        stampyscale *= shrink;
                    }
                }
                if (!std::isfinite(stampxscale) || stampxscale <= 0.0) {
                    stampxscale = 0.05;
                }
                if (!std::isfinite(stampyscale) || stampyscale <= 0.0) {
                    stampyscale = 0.05;
                }
                if (center) {
                    rect.left = point.x - stampxscale / 2;
                    rect.top = point.y - stampyscale / 2;
                } else {
                    rect.left = point.x;
                    rect.top = point.y;
                }
                rect.right = rect.left + stampxscale;
                rect.bottom = rect.top + stampyscale;
            }
            if (latexStamp && ml > QApplication::startDragDistance() && pagewidth > 0.0 && !sa->contents().trimmed().isEmpty()) {
                const double layoutWidth = LatexNoteUtils::layoutWidthForLatexTextVisibleWidth(rect.width() * pagewidth, LatexNoteUtils::paddingForLatexAnnotation(sa));
                if (layoutWidth > 0.0) {
                    sa->setLatexLayoutWidth(layoutWidth);
                    const LatexNoteUtils::RenderResult rendered =
                        LatexNoteUtils::renderAppearancePdf(sa->contents(), sa->latexTextColor(), layoutWidth, sa->isLatexCallout(), LatexNoteUtils::fontSizeForLatexAnnotation(sa));
                    if (rendered.ok) {
                        latexAppearancePdfFileName = rendered.pdfFileName;
                        sa->setLatexAppearancePdfFileName(latexAppearancePdfFileName);
                        LatexNoteUtils::showRenderWarning(pageView, rendered.warning);
                    }
                }
            }
            if (latexStamp && sa->isLatexCallout()) {
                double boxWidth = rect.width();
                double boxHeight = rect.height();
                if (!std::isfinite(boxWidth) || boxWidth <= 0.0) {
                    boxWidth = 0.16;
                }
                if (!std::isfinite(boxHeight) || boxHeight <= 0.0) {
                    boxHeight = 0.08;
                }
                const bool clickPlacement = ml <= QApplication::startDragDistance();
                double left = clickPlacement ? startpoint.x + qMax(0.04, qMin(0.12, boxWidth * 0.7)) : point.x;
                double top = clickPlacement ? startpoint.y + qMax(0.04, qMin(0.12, boxHeight * 0.9)) : point.y;
                left = qBound(0.0, left, qMax(0.0, 1.0 - boxWidth));
                top = qBound(0.0, top, qMax(0.0, 1.0 - boxHeight));
                rect = Okular::NormalizedRect(left, top, left + boxWidth, top + boxHeight);
            }
        }
        // create GeomAnnotation
        else if (typeString == QLatin1String("GeomSquare") || typeString == QLatin1String("GeomCircle")) {
            Okular::GeomAnnotation *ga = new Okular::GeomAnnotation();
            ann = ga;
            // set the type
            if (typeString == QLatin1String("GeomSquare")) {
                ga->setGeometricalType(Okular::GeomAnnotation::InscribedSquare);
            } else {
                ga->setGeometricalType(Okular::GeomAnnotation::InscribedCircle);
            }
            if (m_annotElement.hasAttribute(QStringLiteral("width"))) {
                ann->style().setWidth(m_annotElement.attribute(QStringLiteral("width")).toDouble());
            }
            if (m_annotElement.hasAttribute(QStringLiteral("innerColor"))) {
                ga->setGeometricalInnerColor(QColor(m_annotElement.attribute(QStringLiteral("innerColor"))));
            }
            // set boundary
            rect.left = qMin(startpoint.x, point.x);
            rect.top = qMin(startpoint.y, point.y);
            rect.right = qMax(startpoint.x, point.x);
            rect.bottom = qMax(startpoint.y, point.y);
        }

        m_creationCompleted = false;
        clicked = false;

        // safety check
        if (!ann) {
            return QList<Okular::Annotation *>();
        }

        // set common attributes
        ann->style().setColor(m_annotElement.hasAttribute(QStringLiteral("color")) ? m_annotElement.attribute(QStringLiteral("color")) : m_engineColor);
        if (m_annotElement.hasAttribute(QStringLiteral("opacity"))) {
            ann->style().setOpacity(m_annotElement.attribute(QStringLiteral("opacity"), QStringLiteral("1.0")).toDouble());
        }

        // set the bounding rectangle, and make sure that the newly created
        // annotation lies within the page by translating it if necessary
        if (rect.right > 1) {
            rect.left -= rect.right - 1;
            rect.right = 1;
        }
        if (rect.bottom > 1) {
            rect.top -= rect.bottom - 1;
            rect.bottom = 1;
        }
        ann->setBoundingRectangle(rect);
        if (latexStamp && ann->isLatexCallout()) {
            const Okular::NormalizedPoint connection = calloutConnectionPoint(rect);
            Okular::NormalizedPoint elbow((startpoint.x + connection.x) / 2.0, connection.y);
            if (qAbs(startpoint.x - connection.x) < 0.02) {
                elbow.x = connection.x;
                elbow.y = (startpoint.y + connection.y) / 2.0;
            }
            ann->setLatexCalloutPoint(startpoint, 0);
            ann->setLatexCalloutPoint(elbow, 1);
            ann->setLatexCalloutPoint(connection, 2);
        }

        // return annotation
        return QList<Okular::Annotation *>() << ann;
    }

protected:
    bool clicked;
    bool m_block;
    double xscale, yscale;
    Okular::NormalizedRect rect;
    Okular::NormalizedPoint startpoint;
    Okular::NormalizedPoint point;

private:
    QPixmap pixmap;
    QString hoverIconName, iconName, stampImagePath, latexAppearancePdfFileName;
    bool latexStamp;
    bool boxedLatexStamp;
    int size;
    double fixedAspectRatio;
    bool hasIntrinsicStampSizeInPoints;
    QSizeF intrinsicStampSizeInPoints;
    bool hasIntrinsicStampSize;
    QSize intrinsicStampSize;
    double intrinsicStampDpiX;
    double intrinsicStampDpiY;
    double pagewidth, pageheight;
    bool center;
    PageView *pageView = nullptr;

    bool isCallout() const
    {
        return m_annotElement.attribute(QStringLiteral("type")) == QLatin1String("Callout");
    }

    Okular::NormalizedRect calloutBoxRect(const QString &content) const
    {
        double boxWidth = 0.24;
        double boxHeight = 0.10;

        if (m_annotElement.attribute(QStringLiteral("okularLatex")).toInt() != 0 && !latexAppearancePdfFileName.isEmpty() && pagewidth > 0.0 && pageheight > 0.0) {
            const QSizeF pdfSizePoints = GuiUtils::pdfPageSizeInPoints(latexAppearancePdfFileName);
            bool ok = false;
            const double configuredPadding = m_annotElement.attribute(QStringLiteral("latexPadding")).toDouble(&ok);
            const double padding = ok && configuredPadding >= 0.0 ? configuredPadding : Okular::LatexNoteGeometry::defaultPaddingPoints();
            const QSizeF visualSizePoints = LatexNoteUtils::visualSizeForLatexTextAnnotation(pdfSizePoints, 0.0, padding);
            if (visualSizePoints.isValid() && !visualSizePoints.isEmpty()) {
                boxWidth = qBound(0.01, qMax(boxWidth, visualSizePoints.width() / pagewidth), 1.0);
                boxHeight = qBound(0.01, qMax(boxHeight, visualSizePoints.height() / pageheight), 1.0);
            }
        } else if (!content.isEmpty() && pagewidth > 0.0 && pageheight > 0.0) {
            QFont font;
            if (m_annotElement.hasAttribute(QStringLiteral("font"))) {
                font.fromString(m_annotElement.attribute(QStringLiteral("font")));
            }
            const QFontMetricsF fm(font);
            constexpr int padding = 6;
            const QRectF textRect = fm.boundingRect(QRectF(0, 0, pagewidth * boxWidth - padding * 2, pageheight), Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap, content);
            boxHeight = qMax(boxHeight, (textRect.height() + padding * 2) / pageheight);
        }

        double left = point.x;
        if (point.x < startpoint.x) {
            left -= boxWidth;
        }
        double top = point.y - boxHeight / 2.0;

        left = qBound(0.0, left, qMax(0.0, 1.0 - boxWidth));
        top = qBound(0.0, top, qMax(0.0, 1.0 - boxHeight));
        return Okular::NormalizedRect(left, top, left + boxWidth, top + boxHeight);
    }

    Okular::NormalizedPoint calloutConnectionPoint(const Okular::NormalizedRect &boxRect) const
    {
        const double connectX = startpoint.x < (boxRect.left + boxRect.right) / 2.0 ? boxRect.left : boxRect.right;
        return Okular::NormalizedPoint(connectX, qBound(boxRect.top, startpoint.y, boxRect.bottom));
    }
};

class PickPointEngineSignature : public PickPointEngine
{
#if HAVE_NEW_SIGNATURE_API
public:
    explicit PickPointEngineSignature(SignaturePartUtils::SigningInformation *info)
        : PickPointEngine(nullptr, {})
        , m_page(nullptr)
        , m_aborted(false)
        , m_signingInformation(info)
    {
        m_block = true;
    }

    QRect event(EventType type, Button button, Modifiers modifiers, double nX, double nY, double xScale, double yScale, const Okular::Page *page) override
    {
        m_page = page;
        return PickPointEngine::event(type, button, modifiers, nX, nY, xScale, yScale, page);
    }

    QList<Okular::Annotation *> end() override
    {
        rect.left = qMin(startpoint.x, point.x);
        rect.top = qMin(startpoint.y, point.y);
        rect.right = qMax(startpoint.x, point.x);
        rect.bottom = qMax(startpoint.y, point.y);

        clicked = false;

        // find out annotation's type
        Okular::SignatureAnnotation *ann = new Okular::SignatureAnnotation();
        ann->setFlags(ann->flags() | Okular::Annotation::FixedRotation);

        const QString certSubjectCommonName = m_signingInformation->certificate->subjectInfo(Okular::CertificateInfo::CommonName, Okular::CertificateInfo::EmptyString::TranslatedNotAvailable);
        const QString datetime = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss t"));
        const QString signatureText = i18n("Signed by: %1\n\nDate: %2", certSubjectCommonName, datetime);

        m_creationCompleted = false;
        clicked = false;
        // safety check
        if (!ann) {
            return QList<Okular::Annotation *>();
        }

        ann->setLeftText(certSubjectCommonName);
        ann->setText(signatureText);
        ann->setImagePath(m_signingInformation->backgroundImagePath);

#if HAVE_AUTOMATIC_SIGNATURE_FONT_SIZE
        // 0 means "Chose an appropriate size"
        ann->setLeftFontSize(0);
        ann->setFontSize(0);
#endif

        // set the bounding rectangle, and make sure that the newly created
        // annotation lies within the page by translating it if necessary
        if (rect.right > 1) {
            rect.left -= rect.right - 1;
            rect.right = 1;
        }
        if (rect.bottom > 1) {
            rect.top -= rect.bottom - 1;
            rect.bottom = 1;
        }
        ann->setBoundingRectangle(rect);

        return QList<Okular::Annotation *>() << ann;
    }

    bool isAccepted() const
    {
        return !m_aborted && !m_signingInformation->certificate->nickName().isEmpty();
    }

    bool isAborted() const
    {
        return m_aborted;
    }

private:
    const Okular::Page *m_page;

    bool m_aborted;
    SignaturePartUtils::SigningInformation *m_signingInformation;
#else
public:
    PickPointEngineSignature(Okular::Document *document, PageView *pageView, SignaturePartUtils::SigningInformation *info)
        : PickPointEngine(pageView, {})
        , m_document(document)
        , m_page(nullptr)
        , m_pageView(pageView)
        , m_startOver(false)
        , m_aborted(false)
        , m_signingInformation(info)
    {
        m_block = true;
    }

    QRect event(EventType type, Button button, Modifiers modifiers, double nX, double nY, double xScale, double yScale, const Okular::Page *page) override
    {
        m_page = page;
        return PickPointEngine::event(type, button, modifiers, nX, nY, xScale, yScale, page);
    }

    QList<Okular::Annotation *> end() override
    {
        m_startOver = false;
        rect.left = qMin(startpoint.x, point.x);
        rect.top = qMin(startpoint.y, point.y);
        rect.right = qMax(startpoint.x, point.x);
        rect.bottom = qMax(startpoint.y, point.y);

        // FIXME this is a bit arbitrary, try to figure out a better rule, potentially based in cm and not pixels?
        if (rect.width() * m_page->width() < 100 || rect.height() * m_page->height() < 100) {
            const KMessageBox::ButtonCode answer = KMessageBox::questionTwoActions(
                m_pageView,
                xi18nc("@info", "A signature of this size may be too small to read. If you would like to create a potentially more readable signature, press <interface>Start over</interface> and draw a bigger rectangle."),
                QString(),
                KGuiItem(i18nc("@action:button", "Start Over")),
                KGuiItem(i18nc("@action:button", "Sign")),
                QStringLiteral("TooSmallDigitalSignatureQuestion"));
            if (answer == KMessageBox::PrimaryAction) {
                m_startOver = true;
                return {};
            }
        }
        m_creationCompleted = false;
        clicked = false;

        return {};
    }

    bool isAccepted() const
    {
        return !m_aborted && !m_signingInformation->certificate->nickName().isEmpty();
    }

    bool userWantsToStartOver() const
    {
        return m_startOver;
    }

    bool isAborted() const
    {
        return m_aborted;
    }

    std::pair<Okular::SigningResult, QString> sign(const QString &newFilePath)
    {
        Okular::NewSignatureData data;
        data.setCertNickname(m_signingInformation->certificate->nickName());
        data.setCertSubjectCommonName(m_signingInformation->certificate->subjectInfo(Okular::CertificateInfo::CommonName, Okular::CertificateInfo::EmptyString::TranslatedNotAvailable));
        data.setPassword(m_signingInformation->certificatePassword);
        data.setDocumentPassword(m_signingInformation->documentPassword);
        data.setPage(m_page->number());
        data.setBoundingRectangle(rect);
        data.setReason(m_signingInformation->reason);
        data.setLocation(m_signingInformation->location);
        data.setBackgroundImagePath(m_signingInformation->backgroundImagePath);
        return m_document->sign(data, newFilePath);
    }

    SignaturePartUtils::SigningInformation *signingInformation() const
    {
        return m_signingInformation;
    }

private:
    Okular::Document *m_document;
    const Okular::Page *m_page;
    PageView *m_pageView;

    bool m_startOver;
    bool m_aborted;
    SignaturePartUtils::SigningInformation *m_signingInformation;
#endif
};

/** @short PolyLineEngine */
class PolyLineEngine : public AnnotatorEngine
{
public:
    explicit PolyLineEngine(const QDomElement &engineElement)
        : AnnotatorEngine(engineElement)
        , last(false)
    {
        // parse engine specific attributes
        m_block = engineElement.attribute(QStringLiteral("block")) == QLatin1String("true");
        bool ok = true;
        // numofpoints represents the max number of points for the current
        // polygon/polyline, with a pair of exceptions:
        // -1 means: the polyline must close on the first point (polygon)
        // 0 means: construct as many points as you want, right-click
        //   to construct the last point
        numofpoints = engineElement.attribute(QStringLiteral("points")).toInt(&ok);
        if (!ok) {
            numofpoints = -1;
        }
    }

    static Okular::NormalizedPoint constrainAngle(const Okular::NormalizedPoint &p1, double x, double y, double xScale, double yScale, double angleIncrement)
    {
        // given the normalized point (x, y), return the closest point such that the line segment from p1 forms an angle
        // with the horizontal axis which is an integer multiple of angleIncrement on a reference area of size xScale x yScale
        double dist = sqrt(p1.distanceSqr(x, y, xScale, yScale));
        double angle = atan2((y - p1.y) * yScale, (x - p1.x) * xScale);
        double constrainedAngle = round(angle / angleIncrement) * angleIncrement;
        double offset = dist * sin(angle - constrainedAngle);
        x += offset * sin(constrainedAngle) / xScale;
        y -= offset * cos(constrainedAngle) / yScale;
        return Okular::NormalizedPoint(x, y);
    }

    QRect event(EventType type, Button button, Modifiers modifiers, double nX, double nY, double xScale, double yScale, const Okular::Page * /*page*/) override
    {
        // only proceed if pressing left button
        //            if ( button != Left )
        //                return rect;

        // Constrain to 15° steps, except first point of course.
        if (modifiers.constrainRatioAndAngle && !points.isEmpty()) {
            const Okular::NormalizedPoint constrainedPoint = constrainAngle(points.constLast(), nX, nY, xScale, yScale, M_PI / 12.);
            nX = constrainedPoint.x;
            nY = constrainedPoint.y;
        }
        // process button press
        if (type == Press) {
            newPoint.x = nX;
            newPoint.y = nY;
            if (button == Right) {
                last = true;
            }
        }
        // move the second point
        else if (type == Move) {
            movingpoint.x = nX;
            movingpoint.y = nY;
            const QRect oldmovingrect = movingrect;
            movingrect = rect | QRect((int)(movingpoint.x * xScale), (int)(movingpoint.y * yScale), 1, 1);
            return oldmovingrect | movingrect;
        } else if (type == Release) {
            const Okular::NormalizedPoint tmppoint(nX, nY);
            if (fabs(tmppoint.x - newPoint.x) + fabs(tmppoint.y - newPoint.y) > 1e-2) {
                return rect;
            }

            if (numofpoints == -1 && points.count() > 1 && (fabs(points[0].x - newPoint.x) + fabs(points[0].y - newPoint.y) < 1e-2)) {
                last = true;
            } else {
                points.append(newPoint);
                rect |= QRect((int)(newPoint.x * xScale), (int)(newPoint.y * yScale), 1, 1);
            }
            // end creation if we have constructed the last point of enough points
            if (last || points.count() == numofpoints) {
                m_creationCompleted = true;
                last = false;
                normRect = Okular::NormalizedRect(rect, xScale, yScale);
            }
        }

        return rect;
    }

    void paint(QPainter *painter, double xScale, double yScale, const QRect & /*clipRect*/) override
    {
        if (points.count() < 1) {
            return;
        }

        if (m_block && points.count() == 2) {
            const Okular::NormalizedPoint first = points[0];
            const Okular::NormalizedPoint second = points[1];
            // draw a semitransparent block around the 2 points
            painter->setPen(m_engineColor);
            painter->setBrush(QBrush(m_engineColor.lighter(), Qt::Dense4Pattern));
            painter->drawRect((int)(first.x * (double)xScale), (int)(first.y * (double)yScale), (int)((second.x - first.x) * (double)xScale), (int)((second.y - first.y) * (double)yScale));
        } else {
            // draw a polyline that connects the constructed points
            painter->setPen(QPen(m_engineColor, 2));
            for (int i = 1; i < points.count(); ++i) {
                painter->drawLine((int)(points[i - 1].x * (double)xScale), (int)(points[i - 1].y * (double)yScale), (int)(points[i].x * (double)xScale), (int)(points[i].y * (double)yScale));
            }
            painter->drawLine((int)(points.last().x * (double)xScale), (int)(points.last().y * (double)yScale), (int)(movingpoint.x * (double)xScale), (int)(movingpoint.y * (double)yScale));
        }
    }

    QList<Okular::Annotation *> end() override
    {
        m_creationCompleted = false;

        // find out annotation's description node
        if (m_annotElement.isNull()) {
            return QList<Okular::Annotation *>();
        }

        // find out annotation's type
        Okular::Annotation *ann = nullptr;
        const QString typeString = m_annotElement.attribute(QStringLiteral("type"));

        // create LineAnnotation from path
        if (typeString == QLatin1String("Line") || typeString == QLatin1String("Polyline") || typeString == QLatin1String("Polygon")) {
            if (points.count() < 2) {
                return QList<Okular::Annotation *>();
            }

            // add note
            Okular::LineAnnotation *la = new Okular::LineAnnotation();
            ann = la;

            la->setLinePoints(points);

            if (numofpoints == -1) {
                la->setLineClosed(true);
                if (m_annotElement.hasAttribute(QStringLiteral("innerColor"))) {
                    la->setLineInnerColor(QColor(m_annotElement.attribute(QStringLiteral("innerColor"))));
                }
            } else if (numofpoints == 2) {
                if (m_annotElement.hasAttribute(QStringLiteral("leadFwd"))) {
                    la->setLineLeadingForwardPoint(m_annotElement.attribute(QStringLiteral("leadFwd")).toDouble());
                }
                if (m_annotElement.hasAttribute(QStringLiteral("leadBack"))) {
                    la->setLineLeadingBackwardPoint(m_annotElement.attribute(QStringLiteral("leadBack")).toDouble());
                }
            }
            if (m_annotElement.hasAttribute(QStringLiteral("startStyle"))) {
                la->setLineStartStyle((Okular::LineAnnotation::TermStyle)m_annotElement.attribute(QStringLiteral("startStyle")).toInt());
            }
            if (m_annotElement.hasAttribute(QStringLiteral("endStyle"))) {
                la->setLineEndStyle((Okular::LineAnnotation::TermStyle)m_annotElement.attribute(QStringLiteral("endStyle")).toInt());
            }

            la->setBoundingRectangle(normRect);
        }

        // safety check
        if (!ann) {
            return QList<Okular::Annotation *>();
        }

        if (m_annotElement.hasAttribute(QStringLiteral("width"))) {
            ann->style().setWidth(m_annotElement.attribute(QStringLiteral("width")).toDouble());
        }

        // set common attributes
        ann->style().setColor(m_annotElement.hasAttribute(QStringLiteral("color")) ? m_annotElement.attribute(QStringLiteral("color")) : m_engineColor);
        if (m_annotElement.hasAttribute(QStringLiteral("opacity"))) {
            ann->style().setOpacity(m_annotElement.attribute(QStringLiteral("opacity"), QStringLiteral("1.0")).toDouble());
        }
        // return annotation

        return QList<Okular::Annotation *>() << ann;
    }

private:
    QList<Okular::NormalizedPoint> points;
    Okular::NormalizedPoint newPoint;
    Okular::NormalizedPoint movingpoint;
    QRect rect;
    QRect movingrect;
    Okular::NormalizedRect normRect;
    bool m_block;
    bool last;
    int numofpoints;
};

/** @short TextSelectorEngine */
class TextSelectorEngine : public AnnotatorEngine
{
public:
    TextSelectorEngine(const QDomElement &engineElement, PageView *pageView)
        : AnnotatorEngine(engineElement)
        , m_pageView(pageView)
    {
        // parse engine specific attributes
    }

    QRect event(EventType type, Button button, Modifiers /*modifiers*/, double nX, double nY, double xScale, double yScale, const Okular::Page * /*page*/) override
    {
        // only proceed if pressing left button
        if (button != Left) {
            return QRect();
        }

        if (type == Press) {
            lastPoint.x = nX;
            lastPoint.y = nY;
            const QRect oldrect = rect;
            rect = QRect();
            return oldrect;
        } else if (type == Move) {
            if (item()) {
                const QPoint selectionStart((int)(lastPoint.x * item()->uncroppedWidth()), (int)(lastPoint.y * item()->uncroppedHeight()));
                const QPoint selectionEnd((int)(nX * item()->uncroppedWidth()), (int)(nY * item()->uncroppedHeight()));
                selection.reset();
                std::unique_ptr<Okular::RegularAreaRect> newselection = m_pageView->textSelectionForItem(item(), selectionStart, selectionEnd);
                if (newselection && !newselection->isEmpty()) {
                    const QList<QRect> geom = newselection->geometry((int)xScale, (int)yScale);
                    QRect newrect;
                    for (const QRect &r : geom) {
                        if (newrect.isNull()) {
                            newrect = r;
                        } else {
                            newrect |= r;
                        }
                    }
                    rect = newrect;
                    selection = std::move(newselection);
                }
            }
        } else if (type == Release) {
            m_creationCompleted = true;
        }
        return rect;
    }

    void paint(QPainter *painter, double xScale, double yScale, const QRect & /*clipRect*/) override
    {
        if (selection) {
            painter->setPen(Qt::NoPen);
            QColor col = m_engineColor;
            col.setAlphaF(0.5);
            painter->setBrush(col);
            for (const Okular::NormalizedRect &r : std::as_const(*selection)) {
                painter->drawRect(r.geometry((int)xScale, (int)yScale));
            }
        }
    }

    QList<Okular::Annotation *> end() override
    {
        m_creationCompleted = false;

        // safety checks
        if (m_annotElement.isNull() || !selection) {
            return QList<Okular::Annotation *>();
        }

        // find out annotation's type
        Okular::Annotation *ann = nullptr;
        const QString typeString = m_annotElement.attribute(QStringLiteral("type"));

        Okular::HighlightAnnotation::HighlightType type = Okular::HighlightAnnotation::Highlight;
        bool typevalid = false;
        // create HighlightAnnotation's from the selected area
        if (typeString == QLatin1String("Highlight")) {
            type = Okular::HighlightAnnotation::Highlight;
            typevalid = true;
        } else if (typeString == QLatin1String("Squiggly")) {
            type = Okular::HighlightAnnotation::Squiggly;
            typevalid = true;
        } else if (typeString == QLatin1String("Underline")) {
            type = Okular::HighlightAnnotation::Underline;
            typevalid = true;
        } else if (typeString == QLatin1String("StrikeOut")) {
            type = Okular::HighlightAnnotation::StrikeOut;
            typevalid = true;
        }
        if (typevalid) {
            Okular::HighlightAnnotation *ha = new Okular::HighlightAnnotation();
            ha->setHighlightType(type);
            ha->setBoundingRectangle(Okular::NormalizedRect(rect, item()->uncroppedWidth(), item()->uncroppedHeight()));

            for (const Okular::NormalizedRect &r : std::as_const(*selection)) {
                Okular::HighlightAnnotation::Quad q;
                q.setCapStart(false);
                q.setCapEnd(false);
                q.setFeather(1.0);
                q.setPoint(Okular::NormalizedPoint(r.left, r.bottom), 0);
                q.setPoint(Okular::NormalizedPoint(r.right, r.bottom), 1);
                q.setPoint(Okular::NormalizedPoint(r.right, r.top), 2);
                q.setPoint(Okular::NormalizedPoint(r.left, r.top), 3);
                ha->highlightQuads().append(q);
            }
            ann = ha;
        }

        selection.reset();

        // safety check
        if (!ann) {
            return QList<Okular::Annotation *>();
        }

        // set common attributes
        ann->style().setColor(m_annotElement.hasAttribute(QStringLiteral("color")) ? m_annotElement.attribute(QStringLiteral("color")) : m_engineColor);
        if (m_annotElement.hasAttribute(QStringLiteral("opacity"))) {
            ann->style().setOpacity(m_annotElement.attribute(QStringLiteral("opacity"), QStringLiteral("1.0")).toDouble());
        }

        // return annotations
        return QList<Okular::Annotation *>() << ann;
    }

    QCursor cursor() const override
    {
        return Qt::IBeamCursor;
    }

private:
    // data
    PageView *m_pageView;
    // TODO: support more pages
    std::unique_ptr<Okular::RegularAreaRect> selection;
    Okular::NormalizedPoint lastPoint;
    QRect rect;
};

/** @short AnnotationTools*/
class AnnotationTools
{
public:
    AnnotationTools()
        : m_toolsCount(0)
    {
    }

    void setTools(const QStringList &tools)
    {
        // Populate m_toolsDefinition
        m_toolsCount = 0;
        m_toolsDefinition.clear();
        QDomElement root = m_toolsDefinition.createElement(QStringLiteral("root"));
        m_toolsDefinition.appendChild(root);
        for (const QString &toolXml : tools) {
            QDomDocument entryParser;
            if (entryParser.setContent(toolXml)) {
                root.appendChild(m_toolsDefinition.importNode(entryParser.documentElement(), true));
                m_toolsCount++;
            } else {
                qCWarning(OkularUiDebug) << "Skipping malformed tool XML in AnnotationTools setting";
            }
        }
    }

    QStringList toStringList()
    {
        QStringList tools;
        QDomElement toolElement = m_toolsDefinition.documentElement().firstChildElement();
        QString str;
        QTextStream stream(&str);
        while (!toolElement.isNull()) {
            str.clear();
            toolElement.save(stream, -1 /* indent disabled */);
            tools << str;
            toolElement = toolElement.nextSiblingElement();
        }
        return tools;
    }

    QDomElement tool(int toolId)
    {
        QDomElement toolElement = m_toolsDefinition.documentElement().firstChildElement();
        while (!toolElement.isNull() && toolElement.attribute(QStringLiteral("id")).toInt() != toolId) {
            toolElement = toolElement.nextSiblingElement();
        }
        return toolElement; // can return a null element
    }

    void appendTool(QDomElement toolElement)
    {
        toolElement = m_toolsDefinition.importNode(toolElement, true).toElement();
        toolElement.setAttribute(QStringLiteral("id"), nextToolId());
        m_toolsDefinition.documentElement().appendChild(toolElement);
        ++m_toolsCount;
    }

    bool ensureToolXml(const QString &type, const QString &name, const QString &toolXml)
    {
        if (findToolId(type, name) != -1) {
            return false;
        }

        QDomDocument entryParser;
        if (!entryParser.setContent(toolXml)) {
            qCWarning(OkularUiDebug) << "Skipping malformed default tool XML";
            return false;
        }

        QDomElement toolElement = entryParser.documentElement();
        if (toolElement.tagName() != QLatin1String("tool")) {
            qCWarning(OkularUiDebug) << "Skipping default annotation tool XML without a tool root";
            return false;
        }

        appendTool(toolElement);
        return true;
    }

    bool removeTool(const QString &type, const QString &name)
    {
        QDomElement toolElement = m_toolsDefinition.documentElement().firstChildElement();
        while (!toolElement.isNull()) {
            const QDomElement nextToolElement = toolElement.nextSiblingElement();
            if (toolElement.attribute(QStringLiteral("type")) == type && toolElement.attribute(QStringLiteral("name")) == name) {
                m_toolsDefinition.documentElement().removeChild(toolElement);
                --m_toolsCount;
                return true;
            }
            toolElement = nextToolElement;
        }
        return false;
    }

    bool normalizeCalloutTool()
    {
        bool changed = false;
        QDomElement toolElement = m_toolsDefinition.documentElement().firstChildElement();
        while (!toolElement.isNull()) {
            if (toolElement.attribute(QStringLiteral("type")) == QLatin1String("note-callout")) {
                QDomElement engineElement = toolElement.firstChildElement(QStringLiteral("engine"));
                QDomElement annotationElement = engineElement.firstChildElement(QStringLiteral("annotation"));
                if (!engineElement.isNull() && engineElement.attribute(QStringLiteral("color")) != QLatin1String("#000000")) {
                    engineElement.setAttribute(QStringLiteral("color"), QStringLiteral("#000000"));
                    changed = true;
                }
                if (!annotationElement.isNull()) {
                    const QColor fillColor(annotationElement.attribute(QStringLiteral("color")));
                    const QColor textColor(annotationElement.attribute(QStringLiteral("textColor")));
                    if (!annotationElement.hasAttribute(QStringLiteral("color")) || (fillColor == Qt::black && (!textColor.isValid() || textColor == Qt::black))) {
                        annotationElement.setAttribute(QStringLiteral("color"), QStringLiteral("#ffffffff"));
                        changed = true;
                    }
                    if (!annotationElement.hasAttribute(QStringLiteral("textColor"))) {
                        annotationElement.setAttribute(QStringLiteral("textColor"), QStringLiteral("#ff000000"));
                        changed = true;
                    }
                    if (!annotationElement.hasAttribute(QStringLiteral("borderColor"))) {
                        annotationElement.setAttribute(QStringLiteral("borderColor"), QStringLiteral("#ff000000"));
                        changed = true;
                    }
                    if (!annotationElement.hasAttribute(QStringLiteral("width"))) {
                        annotationElement.setAttribute(QStringLiteral("width"), QStringLiteral("1"));
                        changed = true;
                    }
                }
            } else if (toolElement.attribute(QStringLiteral("type")) == QLatin1String("note-inline")) {
                QDomElement engineElement = toolElement.firstChildElement(QStringLiteral("engine"));
                QDomElement annotationElement = engineElement.firstChildElement(QStringLiteral("annotation"));
                if (!annotationElement.isNull()) {
                    if (!annotationElement.hasAttribute(QStringLiteral("borderColor"))) {
                        annotationElement.setAttribute(QStringLiteral("borderColor"), engineElement.attribute(QStringLiteral("color"), QStringLiteral("#ffff0000")));
                        changed = true;
                    }
                    if (!annotationElement.hasAttribute(QStringLiteral("textColor"))) {
                        annotationElement.setAttribute(QStringLiteral("textColor"), QStringLiteral("#ff000000"));
                        changed = true;
                    }
                }
            }
            toolElement = toolElement.nextSiblingElement();
        }
        return changed;
    }

    bool normalizeTextToolFonts()
    {
        bool changed = false;
        QDomElement toolElement = m_toolsDefinition.documentElement().firstChildElement();
        while (!toolElement.isNull()) {
            QDomElement engineElement = toolElement.firstChildElement(QStringLiteral("engine"));
            QDomElement annotationElement = engineElement.firstChildElement(QStringLiteral("annotation"));
            if (isTextAnnotationTool(annotationElement)) {
                if (annotationElement.hasAttribute(QStringLiteral("font"))) {
                    if (isLegacyDefaultToolFont(annotationElement.attribute(QStringLiteral("font")))) {
                        annotationElement.removeAttribute(QStringLiteral("font"));
                        annotationElement.setAttribute(QStringLiteral("fontName"), QStringLiteral("Helvetica"));
                        if (!annotationElement.hasAttribute(QStringLiteral("fontSize"))) {
                            annotationElement.setAttribute(QStringLiteral("fontSize"), QStringLiteral("10"));
                        }
                        changed = true;
                    }
                } else if (!annotationElement.hasAttribute(QStringLiteral("fontName"))) {
                    annotationElement.setAttribute(QStringLiteral("fontName"), QStringLiteral("Helvetica"));
                    annotationElement.setAttribute(QStringLiteral("fontSize"), QStringLiteral("10"));
                    changed = true;
                } else if (!annotationElement.hasAttribute(QStringLiteral("fontSize"))) {
                    annotationElement.setAttribute(QStringLiteral("fontSize"), QStringLiteral("10"));
                    changed = true;
                }
            }
            toolElement = toolElement.nextSiblingElement();
        }
        return changed;
    }

    bool updateTool(QDomElement newToolElement, int toolId)
    {
        QDomElement toolElement = tool(toolId);
        if (toolElement.isNull()) {
            return false;
        }
        newToolElement = m_toolsDefinition.importNode(newToolElement, true).toElement();
        newToolElement.setAttribute(QStringLiteral("id"), toolId);
        QDomNode oldTool = m_toolsDefinition.documentElement().replaceChild(newToolElement, toolElement);
        return !oldTool.isNull();
    }

    int findToolId(const QString &type, const QString &name = QString())
    {
        if (type.isEmpty()) {
            return -1;
        }

        const auto idForTool = [](const QDomElement &toolElement) {
            bool ok = false;
            const int toolId = toolElement.attribute(QStringLiteral("id")).toInt(&ok);
            return ok ? toolId : -1;
        };
        QDomElement fallbackToolElement;
        QDomElement toolElement = m_toolsDefinition.documentElement().firstChildElement();
        while (!toolElement.isNull()) {
            if (toolElement.attribute(QStringLiteral("type")) == type) {
                if (toolElement.attribute(QStringLiteral("name")) == name) {
                    return idForTool(toolElement);
                }
                if (fallbackToolElement.isNull() && (name.isEmpty() || !toolElement.hasAttribute(QStringLiteral("name")))) {
                    fallbackToolElement = toolElement;
                }
            }
            toolElement = toolElement.nextSiblingElement();
        }

        if (!fallbackToolElement.isNull()) {
            return idForTool(fallbackToolElement);
        }
        return -1;
    }

    bool hasRequiredBuiltinTools()
    {
        const QStringList requiredTypes = {
            QStringLiteral("highlight"),
            QStringLiteral("underline"),
            QStringLiteral("squiggly"),
            QStringLiteral("strikeout"),
            QStringLiteral("typewriter"),
            QStringLiteral("note-inline"),
            QStringLiteral("note-linked"),
            QStringLiteral("note-callout"),
            QStringLiteral("ink"),
            QStringLiteral("straight-line"),
            QStringLiteral("rectangle"),
            QStringLiteral("ellipse"),
            QStringLiteral("polygon"),
            QStringLiteral("stamp"),
        };

        for (const QString &type : requiredTypes) {
            if (findToolId(type) == -1) {
                return false;
            }
        }
        return true;
    }

    bool hasTransientSystemToolState() const
    {
        QDomElement toolElement = m_toolsDefinition.documentElement().firstChildElement();
        while (!toolElement.isNull()) {
            const QDomElement engineElement = toolElement.firstChildElement(QStringLiteral("engine"));
            const QDomElement annotationElement = engineElement.firstChildElement(QStringLiteral("annotation"));
            if (!annotationElement.isNull()) {
                if (annotationElement.attribute(QStringLiteral("okularLatex")).toInt() != 0 || annotationElement.hasAttribute(QStringLiteral("latexVariant"))
                    || annotationElement.hasAttribute(QStringLiteral("latexAppearancePdfFileName")) || annotationElement.hasAttribute(QStringLiteral("templateNoteData"))) {
                    return true;
                }
            }
            toolElement = toolElement.nextSiblingElement();
        }
        return false;
    }

private:
    static bool isTextAnnotationTool(const QDomElement &annotationElement)
    {
        const QString type = annotationElement.attribute(QStringLiteral("type"));
        return type == QLatin1String("Typewriter") || type == QLatin1String("FreeText") || type == QLatin1String("Callout");
    }

    static bool isLegacyDefaultToolFont(QString fontString)
    {
        if (fontString.count(QStringLiteral("\\\\,")) > 9) {
            fontString.replace(QStringLiteral("\\\\,"), QStringLiteral(","));
        }

        QFont font;
        font.fromString(fontString);
        const QString family = font.family();
        return family == QLatin1String("Noto Sans") || family == QLatin1String("Microsoft YaHei") || family == QLatin1String("Microsoft YaHei UI")
            || family.contains(QLatin1String("YaHei"), Qt::CaseInsensitive) || fontString.contains(QLatin1String("Noto Sans"), Qt::CaseInsensitive)
            || fontString.contains(QLatin1String("Microsoft YaHei"), Qt::CaseInsensitive) || fontString.contains(QLatin1String("YaHei"), Qt::CaseInsensitive);
    }

    int nextToolId() const
    {
        int highestId = 0;
        QDomElement toolElement = m_toolsDefinition.documentElement().firstChildElement();
        while (!toolElement.isNull()) {
            bool ok = false;
            const int toolId = toolElement.attribute(QStringLiteral("id")).toInt(&ok);
            if (ok) {
                highestId = qMax(highestId, toolId);
            }
            toolElement = toolElement.nextSiblingElement();
        }
        return highestId + 1;
    }

    QDomDocument m_toolsDefinition;
    int m_toolsCount;
};

static QString defaultCalloutToolXml()
{
    return QStringLiteral("<tool id=\"15\" type=\"note-callout\" name=\"Callout\">"
                          "<engine type=\"PickPoint\" color=\"#000000\">"
                          "<annotation type=\"Callout\" color=\"#ffffffff\" borderColor=\"#ff000000\" textColor=\"#ff000000\" width=\"1\" "
                          "fontName=\"Helvetica\" fontSize=\"10\"/>"
                          "</engine>"
                          "</tool>");
}

static QStringList defaultBuiltinAnnotationTools()
{
    QStringList tools;
    QFile infoFile(QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral("mengshee/tools.xml")));
    if (!infoFile.exists() || !infoFile.open(QIODevice::ReadOnly)) {
        qCWarning(OkularUiDebug) << "Unable to open default annotation tools XML definition";
        return tools;
    }

    QDomDocument doc;
    if (!doc.setContent(&infoFile)) {
        qCWarning(OkularUiDebug) << "Default annotation tools XML file seems to be damaged";
        return tools;
    }

    const QDomElement toolsDefinition = doc.elementsByTagName(QStringLiteral("annotatingTools")).item(0).toElement();
    QDomNode toolDescription = toolsDefinition.firstChild();
    while (toolDescription.isElement()) {
        const QDomElement toolElement = toolDescription.toElement();
        if (toolElement.tagName() == QLatin1String("tool")) {
            QDomDocument temp;
            temp.appendChild(temp.importNode(toolElement, true));
            tools << temp.toString(-1);
        }
        toolDescription = toolDescription.nextSibling();
    }

    return tools;
}

static void deleteBuiltinAnnotationToolsOverride()
{
    KConfigGroup reviewsGroup(KSharedConfig::openConfig(), QStringLiteral("Reviews"));
    reviewsGroup.deleteEntry(QStringLiteral("BuiltinAnnotationTools"));
    reviewsGroup.sync();
}

PageViewAnnotator::PageViewAnnotator(PageView *parent, Okular::Document *storage)
    : QObject(parent)
    , m_document(storage)
    , m_pageView(parent)
    , m_actionHandler(nullptr)
    , m_engine(nullptr)
    , m_builtinToolsDefinition(nullptr)
    , m_quickToolsDefinition(nullptr)
    , m_transientToolsDefinition(nullptr)
    , m_continuousMode(true)
    , m_constrainRatioAndAngle(false)
    , m_signatureMode(false)
    , m_lastToolsDefinition(nullptr)
    , m_lastToolId(-1)
    , m_lockedItem(nullptr)
{
    reparseConfig();
    reparseBuiltinToolsConfig();
    reparseQuickToolsConfig();
    connect(Okular::Settings::self(), &Okular::Settings::builtinAnnotationToolsChanged, this, &PageViewAnnotator::reparseBuiltinToolsConfig);
    connect(Okular::Settings::self(), &Okular::Settings::quickAnnotationToolsChanged, this, &PageViewAnnotator::reparseQuickToolsConfig, Qt::QueuedConnection);
}

void PageViewAnnotator::reparseConfig()
{
    m_continuousMode = Okular::Settings::annotationContinuousMode();

    if (Okular::Settings::identityAuthor().isEmpty()) {
        detachAnnotation();
    }
}

void PageViewAnnotator::reparseBuiltinToolsConfig()
{
    // Read tool list from configuration. It's a list of XML <tool></tool> elements
    if (!m_builtinToolsDefinition) {
        m_builtinToolsDefinition = new AnnotationTools();
    }
    m_builtinToolsDefinition->setTools(Okular::Settings::builtinAnnotationTools());
    if (!m_builtinToolsDefinition->hasRequiredBuiltinTools() || m_builtinToolsDefinition->hasTransientSystemToolState()) {
        const QStringList defaultTools = defaultBuiltinAnnotationTools();
        if (!defaultTools.isEmpty()) {
            qCWarning(OkularUiDebug) << "Ignoring incomplete builtin annotation tools override and restoring default tools";
            deleteBuiltinAnnotationToolsOverride();
            m_builtinToolsDefinition->setTools(defaultTools);
        }
    }
    const bool addedCalloutTool = m_builtinToolsDefinition->ensureToolXml(QStringLiteral("note-callout"), QString(), defaultCalloutToolXml());
    const bool removedLegacyLatexCalloutTool = m_builtinToolsDefinition->removeTool(QStringLiteral("note-callout"), QStringLiteral("LaTeX Callout"));
    const bool normalizedCalloutTool = m_builtinToolsDefinition->normalizeCalloutTool();
    const bool normalizedTextToolFonts = m_builtinToolsDefinition->normalizeTextToolFonts();
    if (addedCalloutTool || removedLegacyLatexCalloutTool || normalizedCalloutTool || normalizedTextToolFonts) {
        saveBuiltinAnnotationTools();
    }

    if (m_actionHandler) {
        m_actionHandler->reparseBuiltinToolsConfig();
    }
}

void PageViewAnnotator::reparseQuickToolsConfig()
{
    // Read tool list from configuration. It's a list of XML <tool></tool> elements
    if (!m_quickToolsDefinition) {
        m_quickToolsDefinition = new AnnotationTools();
    }
    m_quickToolsDefinition->setTools(Okular::Settings::quickAnnotationTools());
    if (m_quickToolsDefinition->normalizeTextToolFonts()) {
        Okular::Settings::setQuickAnnotationTools(m_quickToolsDefinition->toStringList());
        Okular::Settings::self()->save();
    }

    if (m_actionHandler) {
        m_actionHandler->reparseQuickToolsConfig();
    }
}

PageViewAnnotator::~PageViewAnnotator()
{
    delete m_engine;
    delete m_builtinToolsDefinition;
    delete m_quickToolsDefinition;
    delete m_transientToolsDefinition;
}

void PageViewAnnotator::setPageView(PageView *pageView)
{
    if (!pageView || pageView == m_pageView || annotating()) {
        return;
    }

    AnnotationTools *activeToolsDefinition = m_lastToolsDefinition;
    const int activeToolId = m_lastToolId;
    const bool restartActiveTool = m_engine && !m_signatureMode && activeToolsDefinition && activeToolId >= 0;

    // Signing keeps PageView-owned credentials and (with the legacy API) a
    // raw PageView pointer in its engine.  Cancelling it before moving focus
    // avoids routing the signing operation through a different or deleted
    // view.  A normal annotation tool can instead be recreated below.
    if (m_signatureMode || (m_engine && !restartActiveTool)) {
        detachAnnotation();
    }

    m_pageView = pageView;
    m_lastDrawnRect = QRect();
    m_lockedItem = nullptr;
    if (restartActiveTool) {
        selectTool(activeToolsDefinition, activeToolId, ShowTip::No);
    } else {
        m_pageView->updateCursor();
    }
}

PageView *PageViewAnnotator::pageView() const
{
    return m_pageView;
}

void PageViewAnnotator::setSignatureMode(bool enabled)
{
    m_signatureMode = enabled;
}

bool PageViewAnnotator::signatureMode() const
{
    return m_signatureMode;
}

void PageViewAnnotator::startSigning(SignaturePartUtils::SigningInformation *info)
{
    m_signatureMode = true;
#if HAVE_NEW_SIGNATURE_API
    m_engine = new PickPointEngineSignature(info);
#else
    m_engine = new PickPointEngineSignature(m_document, m_pageView, info);
#endif
}

bool PageViewAnnotator::active() const
{
    return (m_engine != nullptr) || m_signatureMode;
}

bool PageViewAnnotator::annotating() const
{
    return active() && m_lockedItem;
}

QCursor PageViewAnnotator::cursor() const
{
    return m_engine ? m_engine->cursor() : Qt::CrossCursor;
}

QRect PageViewAnnotator::performRouteMouseOrTabletEvent(const AnnotatorEngine::EventType eventType, const AnnotatorEngine::Button button, const AnnotatorEngine::Modifiers modifiers, const QPointF pos, PageViewItem *item)
{
    // creationCompleted is intended to be set by event(), handled subsequently by end(), and cleared within end().
    // If it's set here, we recursed for some reason (e.g., stacked event loop).
    // Just bail out, all we want to do is already on stack.
    if (m_engine && m_engine->creationCompleted()) {
        return QRect();
    }

    // if the right mouse button was pressed, we simply do nothing. In this way, we are still editing the annotation
    // and so this function will receive and process the right mouse button release event too. If we detach now the annotation tool,
    // the release event will be processed by the PageView class which would create the annotation property widget, and we do not want this.
    if (button == AnnotatorEngine::Right && eventType == AnnotatorEngine::Press) {
        return QRect();
    } else if (button == AnnotatorEngine::Right && eventType == AnnotatorEngine::Release) {
        detachAnnotation();
        return QRect();
    }

    // 1. lock engine to current item
    if (!m_lockedItem && eventType == AnnotatorEngine::Press) {
        m_lockedItem = item;
        m_engine->setItem(m_lockedItem);
    }
    if (!m_lockedItem) {
        return QRect();
    }

    // find out normalized mouse coords inside current item
    const QRect &itemRect = m_lockedItem->uncroppedGeometry();
    const QPointF eventPos = m_pageView->contentAreaPoint(pos);
    const double nX = qBound(0.0, m_lockedItem->absToPageX(eventPos.x()), 1.0);
    const double nY = qBound(0.0, m_lockedItem->absToPageY(eventPos.y()), 1.0);

    QRect modifiedRect;

    // 2. use engine to perform operations
    const QRect paintRect = m_engine->event(eventType, button, modifiers, nX, nY, itemRect.width(), itemRect.height(), m_lockedItem->page());

    // 3. update absolute extents rect and send paint event(s)
    if (paintRect.isValid()) {
        // 3.1. unite old and new painting regions
        QRegion compoundRegion(m_lastDrawnRect);
        m_lastDrawnRect = paintRect;
        m_lastDrawnRect.translate(itemRect.left(), itemRect.top());
        // 3.2. decompose paint region in rects and send paint events
        const QRegion rgn = compoundRegion.united(m_lastDrawnRect);
        const QPoint areaPos = m_pageView->contentAreaPosition();
        for (const QRect &r : rgn) {
            m_pageView->viewport()->update(r.translated(-areaPos));
        }
        modifiedRect = compoundRegion.boundingRect() | m_lastDrawnRect;
    }

    // 4. if engine has finished, apply Annotation to the page
    if (m_engine->creationCompleted()) {
        // apply engine data to the Annotation's and reset engine
        const QList<Okular::Annotation *> annotations = m_engine->end();
        bool detachAfterCreation = false;
        PageViewItem *createdStampPageItem = nullptr;
        Okular::Annotation *createdStampAnnotation = nullptr;
        // attach the newly filled annotations to the page
        for (Okular::Annotation *annotation : annotations) {
            if (!annotation) {
                continue;
            }

            if (annotation->subType() == Okular::Annotation::AStamp && !annotation->isOkularLatex()) {
                detachAfterCreation = true;
                createdStampPageItem = m_lockedItem;
                createdStampAnnotation = annotation;
            }
            if (auto *templateText = TemplateNoteUtils::annotationAsTemplateTextAnnotation(annotation)) {
                QString errorMessage;
                const QString expanded = TemplateNoteUtils::expandTemplate(m_document, m_lockedItem->pageNumber(), templateText, &errorMessage);
                if (errorMessage.isEmpty()) {
                    templateText->setContents(expanded);
                }
            }

            annotation->setCreationDate(QDateTime::currentDateTime());
            annotation->setModificationDate(QDateTime::currentDateTime());
            annotation->setAuthor(Okular::Settings::identityAuthor());
            m_document->addPageAnnotation(m_lockedItem->pageNumber(), annotation);

#if HAVE_NEW_SIGNATURE_API
            if (auto signatureAnnotation = dynamic_cast<Okular::SignatureAnnotation *>(annotation)) {
                m_pageView->startSigning(signatureAnnotation);
                // We cannot undo adding a signature annotation
                // clear the undo stack so we don't offer it to the user
                m_document->clearHistory();
            }
#endif

            if (annotation->openDialogAfterCreation()) {
                m_pageView->openAnnotationWindow(annotation, m_lockedItem->pageNumber());
            }
        }

        if (signatureMode()) {
            const auto signEngine = static_cast<PickPointEngineSignature *>(m_engine);

#if !HAVE_NEW_SIGNATURE_API
            if (signEngine->userWantsToStartOver()) {
                auto singingInfo = signEngine->signingInformation();
                delete m_engine;
                m_engine = new PickPointEngineSignature(m_document, m_pageView, singingInfo);
                return {};
            } else if (signEngine->isAccepted()) {
                const QString newFilePath = SignaturePartUtils::getFileNameForNewSignedFile(m_pageView, m_document);

                if (!newFilePath.isEmpty()) {
                    const std::pair<Okular::SigningResult, QString> result = static_cast<PickPointEngineSignature *>(m_engine)->sign(newFilePath);
                    switch (result.first) {
                    case Okular::SigningSuccess: {
                        Q_EMIT m_pageView->requestOpenNewlySignedFile(newFilePath, m_lockedItem->pageNumber() + 1);
                        break;
                    }
                    case Okular::FieldAlreadySigned: // We should not end up here
                    case Okular::KeyMissing:         // unless the user modified the key store after opening the dialog, this should not happen
                    case Okular::InternalSigningError:
                        KMessageBox::detailedError(m_pageView, errorString(result.first, static_cast<int>(result.first)), result.second);
                        break;
                    case Okular::GenericSigningError:
                        KMessageBox::detailedError(m_pageView, errorString(result.first, newFilePath), result.second);
                        break;
                    case Okular::UserCancelled:
                        break;
                    case Okular::BadPassphrase:
                        KMessageBox::detailedError(m_pageView, errorString(result.first, {}), result.second);
                        break;
                    case Okular::SignatureWriteFailed:
                        KMessageBox::detailedError(m_pageView, errorString(result.first, newFilePath), result.second);
                        break;
                    }
                }
                // Exit the signature mode.
                setSignatureMode(false);
                selectBuiltinTool(-1, ShowTip::No);
            }
#else
            if (signEngine->isAccepted()) {
                // Exit the signature mode.
                setSignatureMode(false);
                selectBuiltinTool(-1, ShowTip::No);
            }
#endif

            else if (signEngine->isAborted()) {
                // Exit the signature mode.
                setSignatureMode(false);
                selectBuiltinTool(-1, ShowTip::No);
            }
            m_continuousMode = false;
        }

        if (m_continuousMode && !detachAfterCreation && currentToolSupportsContinuousCreation()) {
            selectLastTool();
        } else {
            detachAnnotation();
        }

        if (createdStampPageItem && createdStampAnnotation) {
            Q_EMIT annotationCreated(createdStampPageItem, createdStampAnnotation);
        }
    }

    return modifiedRect;
}

QRect PageViewAnnotator::routeMouseEvent(QMouseEvent *e, PageViewItem *item)
{
    AnnotatorEngine::EventType eventType;
    AnnotatorEngine::Button button;
    AnnotatorEngine::Modifiers modifiers;

    // figure out the event type and button
    AnnotatorEngine::decodeEvent(e, &eventType, &button);

    // Constrain angle if action checked XOR shift button pressed.
    modifiers.constrainRatioAndAngle = (bool(constrainRatioAndAngleActive()) != bool(e->modifiers() & Qt::ShiftModifier));

    return performRouteMouseOrTabletEvent(eventType, button, modifiers, e->position(), item);
}

QRect PageViewAnnotator::routeTabletEvent(QTabletEvent *e, PageViewItem *item, const QPoint localOriginInGlobal)
{
    // Unlike routeMouseEvent, routeTabletEvent must explicitly ignore events it doesn't care about so that
    // the corresponding mouse event will later be delivered.
    if (!item) {
        e->ignore();
        return QRect();
    }

    AnnotatorEngine::EventType eventType;
    AnnotatorEngine::Button button;
    AnnotatorEngine::Modifiers modifiers;

    // figure out the event type and button
    AnnotatorEngine::decodeEvent(e, &eventType, &button);

    // Constrain angle if action checked XOR shift button pressed.
    modifiers.constrainRatioAndAngle = (bool(constrainRatioAndAngleActive()) != bool(e->modifiers() & Qt::ShiftModifier));

    const QPointF globalPosF = e->globalPosition();
    const QPointF localPosF = globalPosF - localOriginInGlobal;
    return performRouteMouseOrTabletEvent(eventType, button, modifiers, localPosF, item);
}

bool PageViewAnnotator::routeKeyEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        detachAnnotation();
        return true;
    }
    return false;
}

bool PageViewAnnotator::routePaints(const QRect wantedRect) const
{
    return m_engine && wantedRect.intersects(m_lastDrawnRect) && m_lockedItem;
}

void PageViewAnnotator::routePaint(QPainter *painter, const QRect paintRect)
{
    // if there's no locked item, then there's no decided place to draw on
    if (!m_lockedItem) {
        return;
    }

#ifndef NDEBUG
    // [DEBUG] draw the paint region if enabled
    if (Okular::Settings::debugDrawAnnotationRect()) {
        painter->drawRect(paintRect);
    }
#endif
    // move painter to current itemGeometry rect
    const QRect &itemRect = m_lockedItem->uncroppedGeometry();
    painter->save();
    painter->translate(itemRect.topLeft());
    // TODO: Clip annotation painting to cropped page.

    // transform cliprect from absolute to item relative coords
    QRect annotRect = paintRect.intersected(m_lastDrawnRect);
    annotRect.translate(-itemRect.topLeft());

    // use current engine for painting (in virtual page coordinates)
    m_engine->paint(painter, m_lockedItem->uncroppedWidth(), m_lockedItem->uncroppedHeight(), annotRect);
    painter->restore();
}

void PageViewAnnotator::selectBuiltinTool(int toolId, ShowTip showTip)
{
    selectTool(m_builtinToolsDefinition, toolId, showTip);
}

int PageViewAnnotator::selectBuiltinToolByType(const QString &toolType, const QString &toolName, ShowTip showTip)
{
    const int toolId = m_builtinToolsDefinition->findToolId(toolType, toolName);
    if (toolId == -1) {
        selectBuiltinTool(-1, ShowTip::No);
        m_pageView->displayMessage(i18nc("Annotation tool", "Annotation tool is not available"), QString(), PageViewMessage::Annotation);
        return -1;
    }
    selectBuiltinTool(toolId, showTip);
    return toolId;
}

void PageViewAnnotator::selectQuickTool(int toolId)
{
    selectTool(m_quickToolsDefinition, toolId, ShowTip::Yes);
}

void PageViewAnnotator::selectTool(AnnotationTools *toolsDefinition, int toolId, ShowTip showTip)
{
    // ask for Author's name if not already set
    if (toolId > 0 && !ensureAnnotationAuthor()) {
        detachAnnotation();
        return;
    }

    // terminate any previous operation
    if (m_engine) {
        delete m_engine;
        m_engine = nullptr;
    }
    m_lockedItem = nullptr;
    if (m_lastDrawnRect.isValid()) {
        m_pageView->viewport()->update(m_lastDrawnRect.translated(-m_pageView->contentAreaPosition()));
        m_lastDrawnRect = QRect();
    }

    // store current tool for later usage
    m_lastToolId = toolId;
    m_lastToolsDefinition = toolsDefinition;

    // handle tool deselection
    if (toolId == -1) {
        m_pageView->displayMessage(QString());
        m_pageView->updateCursor();
        Q_EMIT toolActive(false);
        return;
    }

    // for the selected tool create the Engine
    QDomElement toolElement = toolsDefinition->tool(toolId);
    if (!toolElement.isNull()) {
        // parse tool properties
        QDomElement engineElement = toolElement.firstChildElement(QStringLiteral("engine"));
        if (!engineElement.isNull()) {
            // create the AnnotatorEngine
            QString type = engineElement.attribute(QStringLiteral("type"));
            if (type == QLatin1String("SmoothLine")) {
                m_engine = new SmoothPathEngine(engineElement);
            } else if (type == QLatin1String("PickPoint")) {
                m_engine = new PickPointEngine(m_pageView, engineElement);
            } else if (type == QLatin1String("PolyLine")) {
                m_engine = new PolyLineEngine(engineElement);
            } else if (type == QLatin1String("TextSelector")) {
                m_engine = new TextSelectorEngine(engineElement, m_pageView);
            } else {
                qCWarning(OkularUiDebug).nospace() << "tools.xml: engine type:'" << type << "' is not defined!";
            }

            if (showTip == ShowTip::Yes) {
                // display the tooltip
                const QString annotType = toolElement.attribute(QStringLiteral("type"));
                QString tip;

                if (annotType == QLatin1String("ellipse")) {
                    tip = i18nc("Annotation tool", "Draw an ellipse (drag to select a zone)");
                } else if (annotType == QLatin1String("highlight")) {
                    tip = i18nc("Annotation tool", "Highlight text");
                } else if (annotType == QLatin1String("ink")) {
                    tip = i18nc("Annotation tool", "Draw a freehand line");
                } else if (annotType == QLatin1String("note-inline")) {
                    tip = i18nc("Annotation tool", "Inline Text Annotation (drag to select a zone)");
                } else if (annotType == QLatin1String("note-linked")) {
                    tip = i18nc("Annotation tool", "Put a pop-up note");
                } else if (annotType == QLatin1String("note-callout")) {
                    tip = i18nc("Annotation tool", "Callout Annotation (drag from the target to the text box)");
                } else if (annotType == QLatin1String("polygon")) {
                    tip = i18nc("Annotation tool", "Draw a polygon (click on the first point to close it)");
                } else if (annotType == QLatin1String("rectangle")) {
                    tip = i18nc("Annotation tool", "Draw a rectangle");
                } else if (annotType == QLatin1String("squiggly")) {
                    tip = i18nc("Annotation tool", "Squiggle text");
                } else if (annotType == QLatin1String("stamp")) {
                    tip = i18nc("Annotation tool", "Put a stamp symbol");
                } else if (annotType == QLatin1String("straight-line")) {
                    tip = i18nc("Annotation tool", "Draw a straight line");
                } else if (annotType == QLatin1String("strikeout")) {
                    tip = i18nc("Annotation tool", "Strike out text");
                } else if (annotType == QLatin1String("underline")) {
                    tip = i18nc("Annotation tool", "Underline text");
                } else if (annotType == QLatin1String("typewriter")) {
                    tip = i18nc("Annotation tool", "Typewriter Annotation (drag to select a zone)");
                }

                if (!tip.isEmpty()) {
                    m_pageView->displayMessage(tip, QString(), PageViewMessage::Annotation);
                }
            }
        }

        // consistency warning
        if (!m_engine) {
            qCWarning(OkularUiDebug) << "tools.xml: couldn't find good engine description. check xml.";
        }

        m_pageView->updateCursor();
    }

    Q_EMIT toolActive(true);
}

void PageViewAnnotator::selectLastTool()
{
    selectTool(m_lastToolsDefinition, m_lastToolId, ShowTip::No);
}

int PageViewAnnotator::selectStampTool(const QString &stampSymbol)
{
    const int stampToolId = m_builtinToolsDefinition->findToolId(QStringLiteral("stamp"));
    QDomElement toolElement = builtinTool(stampToolId);
    if (toolElement.isNull()) {
        return -1;
    }
    QDomElement engineElement = toolElement.firstChildElement(QStringLiteral("engine"));
    QDomElement annotationElement = engineElement.firstChildElement(QStringLiteral("annotation"));
    const bool imageNote = QFileInfo(stampSymbol).exists() && QFileInfo(stampSymbol).isFile();
    engineElement.setAttribute(QStringLiteral("hoverIcon"), stampSymbol);
    annotationElement.setAttribute(QStringLiteral("icon"), imageNote ? QStringLiteral("Image") : stampSymbol);
    if (imageNote) {
        annotationElement.setAttribute(QStringLiteral("imagePath"), stampSymbol);
    } else {
        annotationElement.removeAttribute(QStringLiteral("imagePath"));
    }
    annotationElement.removeAttribute(QStringLiteral("contents"));
    annotationElement.removeAttribute(QStringLiteral("okularLatex"));
    annotationElement.removeAttribute(QStringLiteral("latexVariant"));
    annotationElement.removeAttribute(QStringLiteral("latexBoxed"));
    annotationElement.removeAttribute(QStringLiteral("latexCallout"));
    annotationElement.removeAttribute(QStringLiteral("latexAppearancePdfFileName"));
    annotationElement.removeAttribute(QStringLiteral("latexPadding"));
    annotationElement.removeAttribute(QStringLiteral("latexFontSize"));
    annotationElement.removeAttribute(QStringLiteral("latexLayoutWidth"));
    annotationElement.removeAttribute(QStringLiteral("latexDefaultLayoutWidth"));
    annotationElement.removeAttribute(QStringLiteral("templateNoteData"));
    saveBuiltinAnnotationTools();
    selectBuiltinTool(stampToolId, ShowTip::Yes);
    return stampToolId;
}

int PageViewAnnotator::selectLatexStampTool(const QString &pdfAppearanceFile, const QString &contents, bool boxed, const QColor &textColor, const QColor &fillColor, const QColor &borderColor, bool callout)
{
    const QString toolType = QStringLiteral("stamp");
    if (!m_transientToolsDefinition) {
        m_transientToolsDefinition = new AnnotationTools();
    }
    m_transientToolsDefinition->setTools(m_builtinToolsDefinition->toStringList());

    const int toolId = m_transientToolsDefinition->findToolId(toolType);
    QDomElement toolElement = m_transientToolsDefinition->tool(toolId);
    if (toolElement.isNull()) {
        return -1;
    }

    QDomElement engineElement = toolElement.firstChildElement(QStringLiteral("engine"));
    QDomElement annotationElement = engineElement.firstChildElement(QStringLiteral("annotation"));
    engineElement.removeAttribute(QStringLiteral("hoverIcon"));
    annotationElement.setAttribute(QStringLiteral("type"), QStringLiteral("Stamp"));
    annotationElement.setAttribute(QStringLiteral("icon"), QStringLiteral("latex-notes"));
    annotationElement.removeAttribute(QStringLiteral("imagePath"));
    annotationElement.setAttribute(QStringLiteral("contents"), contents);
    annotationElement.setAttribute(QStringLiteral("okularLatex"), QStringLiteral("1"));
    annotationElement.setAttribute(QStringLiteral("latexVariant"), callout ? QStringLiteral("callout") : (boxed ? QStringLiteral("inline") : QStringLiteral("note")));
    annotationElement.setAttribute(QStringLiteral("latexBoxed"), (boxed || callout) ? QStringLiteral("1") : QStringLiteral("0"));
    annotationElement.setAttribute(QStringLiteral("latexCallout"), callout ? QStringLiteral("1") : QStringLiteral("0"));
    if (pdfAppearanceFile.isEmpty()) {
        annotationElement.removeAttribute(QStringLiteral("latexAppearancePdfFileName"));
    } else {
        annotationElement.setAttribute(QStringLiteral("latexAppearancePdfFileName"), pdfAppearanceFile);
    }
    annotationElement.setAttribute(QStringLiteral("latexPadding"), QStringLiteral("3"));
    annotationElement.setAttribute(QStringLiteral("latexFontSize"), QStringLiteral("0"));
    annotationElement.removeAttribute(QStringLiteral("latexLayoutWidth"));
    annotationElement.setAttribute(QStringLiteral("latexDefaultLayoutWidth"), QStringLiteral("page-third"));
    QColor targetTextColor = textColor;
    if (!targetTextColor.isValid() || targetTextColor.alpha() == 0) {
        targetTextColor = Qt::black;
    }
    annotationElement.setAttribute(QStringLiteral("textColor"), targetTextColor.name(QColor::HexArgb));
    if (boxed || callout) {
        annotationElement.setAttribute(QStringLiteral("width"), QStringLiteral("1"));
        QColor targetFillColor = fillColor;
        if (!targetFillColor.isValid() || targetFillColor.alpha() == 0) {
            targetFillColor = callout ? Qt::white : Qt::yellow;
        }
        QColor targetBorderColor = borderColor;
        if (!targetBorderColor.isValid() || targetBorderColor.alpha() == 0) {
            targetBorderColor = targetTextColor;
        }
        annotationElement.setAttribute(QStringLiteral("color"), targetFillColor.name(QColor::HexArgb));
        annotationElement.setAttribute(QStringLiteral("borderColor"), targetBorderColor.name(QColor::HexArgb));
    } else {
        annotationElement.setAttribute(QStringLiteral("width"), QStringLiteral("0"));
        annotationElement.setAttribute(QStringLiteral("color"), QStringLiteral("#00ffffff"));
        annotationElement.setAttribute(QStringLiteral("borderColor"), QStringLiteral("#00000000"));
    }
    selectTool(m_transientToolsDefinition, toolId, ShowTip::Yes);
    return toolId;
}

int PageViewAnnotator::selectTemplateTextTool(const QString &templateData, const QString &contents)
{
    const QString toolType = QStringLiteral("note-inline");
    if (!m_transientToolsDefinition) {
        m_transientToolsDefinition = new AnnotationTools();
    }
    m_transientToolsDefinition->setTools(m_builtinToolsDefinition->toStringList());

    const int toolId = m_transientToolsDefinition->findToolId(toolType);
    QDomElement toolElement = m_transientToolsDefinition->tool(toolId);
    if (toolElement.isNull()) {
        return -1;
    }

    QDomElement engineElement = toolElement.firstChildElement(QStringLiteral("engine"));
    QDomElement annotationElement = engineElement.firstChildElement(QStringLiteral("annotation"));
    engineElement.removeAttribute(QStringLiteral("hoverIcon"));
    annotationElement.setAttribute(QStringLiteral("type"), QStringLiteral("FreeText"));
    annotationElement.removeAttribute(QStringLiteral("icon"));
    annotationElement.removeAttribute(QStringLiteral("imagePath"));
    annotationElement.setAttribute(QStringLiteral("contents"), contents);
    annotationElement.setAttribute(QStringLiteral("templateNoteData"), templateData);
    annotationElement.setAttribute(QStringLiteral("fontName"), QStringLiteral("Helvetica"));
    annotationElement.setAttribute(QStringLiteral("fontSize"), QStringLiteral("9"));
    annotationElement.setAttribute(QStringLiteral("textColor"), QStringLiteral("#ff404040"));
    annotationElement.setAttribute(QStringLiteral("align"), QStringLiteral("1"));
    annotationElement.setAttribute(QStringLiteral("color"), QStringLiteral("#00000000"));
    annotationElement.setAttribute(QStringLiteral("borderColor"), QStringLiteral("#00000000"));
    annotationElement.setAttribute(QStringLiteral("width"), QStringLiteral("0"));
    annotationElement.removeAttribute(QStringLiteral("okularLatex"));
    annotationElement.removeAttribute(QStringLiteral("latexVariant"));
    annotationElement.removeAttribute(QStringLiteral("latexBoxed"));
    annotationElement.removeAttribute(QStringLiteral("latexCallout"));
    annotationElement.removeAttribute(QStringLiteral("latexAppearancePdfFileName"));
    annotationElement.removeAttribute(QStringLiteral("latexPadding"));
    annotationElement.removeAttribute(QStringLiteral("latexFontSize"));
    annotationElement.removeAttribute(QStringLiteral("latexLayoutWidth"));
    annotationElement.removeAttribute(QStringLiteral("latexDefaultLayoutWidth"));

    selectTool(m_transientToolsDefinition, toolId, ShowTip::Yes);
    return toolId;
}

void PageViewAnnotator::detachAnnotation()
{
    if (m_lastToolId == -1 && !m_engine && !signatureMode()) {
        return;
    }
    const bool wasSigning = signatureMode();
    selectBuiltinTool(-1, ShowTip::No);
    if (!wasSigning) {
        if (m_actionHandler) {
            m_actionHandler->deselectAllAnnotationActions();
        }
    } else {
        m_pageView->displayMessage(QString());
#if HAVE_NEW_SIGNATURE_API
        // The signature field is inserted before the user finishes signing.
        // Esc/right-click must remove that pending field as well, otherwise
        // it remains editable/savable although signing was cancelled.
        m_pageView->cancelSigning();
#endif
        setSignatureMode(false);
    }
}

bool PageViewAnnotator::applyTextMarkupAnnotationForSelection(const QString &toolType, const QString &toolName)
{
    const int toolId = m_builtinToolsDefinition->findToolId(toolType, toolName);
    if (toolId == -1) {
        return false;
    }
    return applyTextMarkupAnnotationForSelection(m_builtinToolsDefinition->tool(toolId));
}

bool PageViewAnnotator::applyQuickTextMarkupAnnotationForSelection(int toolId)
{
    return applyTextMarkupAnnotationForSelection(m_quickToolsDefinition->tool(toolId));
}

bool PageViewAnnotator::applyTextMarkupAnnotationForSelection(const QDomElement &toolElement)
{
    if (!m_pageView->hasTextSelection()) {
        return false;
    }

    const QDomElement engineElement = toolElement.firstChildElement(QStringLiteral("engine"));
    const QDomElement annotationElement = engineElement.firstChildElement(QStringLiteral("annotation"));
    if (engineElement.isNull() || annotationElement.isNull()) {
        return false;
    }

    const QString annotationType = annotationElement.attribute(QStringLiteral("type"));
    if (annotationType != QLatin1String("Highlight") && annotationType != QLatin1String("Underline") && annotationType != QLatin1String("Squiggly")
        && annotationType != QLatin1String("StrikeOut")) {
        return false;
    }

    if (!ensureAnnotationAuthor()) {
        return true;
    }

    const QColor engineColor = engineElement.hasAttribute(QStringLiteral("color")) ? QColor(engineElement.attribute(QStringLiteral("color"))) : QColor();
    m_pageView->addTextMarkupAnnotationForSelection(annotationElement, engineColor);
    return true;
}

bool PageViewAnnotator::ensureAnnotationAuthor()
{
    if (!Okular::Settings::identityAuthor().isEmpty()) {
        return true;
    }

    KUser currentUser;
    QString userName = currentUser.property(KUser::FullName).toString();
    if (userName.isEmpty()) {
        bool ok = false;
        userName = QInputDialog::getText(m_pageView, i18n("Author name"), i18n("Author name for the annotation:"), QLineEdit::Normal, QString(), &ok);
        if (!ok) {
            return false;
        }
    }

    Okular::Settings::setIdentityAuthor(userName);
    Okular::Settings::self()->save();
    return true;
}

QString PageViewAnnotator::defaultToolName(const QDomElement &toolElement)
{
    const QString annotType = toolElement.attribute(QStringLiteral("type"));

    if (annotType == QLatin1String("ellipse")) {
        return i18n("Ellipse");
    } else if (annotType == QLatin1String("highlight")) {
        return i18n("Highlighter");
    } else if (annotType == QLatin1String("ink")) {
        return i18n("Freehand Line");
    } else if (annotType == QLatin1String("note-inline")) {
        return i18n("Inline Note");
    } else if (annotType == QLatin1String("note-linked")) {
        return i18n("Pop-up Note");
    } else if (annotType == QLatin1String("note-callout")) {
        return i18n("Callout");
    } else if (annotType == QLatin1String("polygon")) {
        return i18n("Polygon");
    } else if (annotType == QLatin1String("rectangle")) {
        return i18n("Rectangle");
    } else if (annotType == QLatin1String("squiggly")) {
        return i18n("Squiggle");
    } else if (annotType == QLatin1String("stamp")) {
        return i18n("Stamp");
    } else if (annotType == QLatin1String("straight-line")) {
        return i18n("Straight Line");
    } else if (annotType == QLatin1String("strikeout")) {
        return i18n("Strike out");
    } else if (annotType == QLatin1String("underline")) {
        return i18n("Underline");
    } else if (annotType == QLatin1String("typewriter")) {
        return i18n("Typewriter");
    } else {
        return QString();
    }
}

QPixmap PageViewAnnotator::makeToolPixmap(const QDomElement &toolElement)
{
    QPixmap pixmap(32 * qApp->devicePixelRatio(), 32 * qApp->devicePixelRatio());
    pixmap.setDevicePixelRatio(qApp->devicePixelRatio());
    pixmap.fill(Qt::transparent);
    const QString annotType = toolElement.attribute(QStringLiteral("type"));

    /* Parse color, innerColor and icon (if present) */
    QColor engineColor, innerColor, textColor, annotColor;
    QString icon;
    int endStyle = 5;
    QDomNodeList engineNodeList = toolElement.elementsByTagName(QStringLiteral("engine"));
    if (engineNodeList.size() > 0) {
        QDomElement engineEl = engineNodeList.item(0).toElement();
        if (!engineEl.isNull() && engineEl.hasAttribute(QStringLiteral("color"))) {
            engineColor = QColor(engineEl.attribute(QStringLiteral("color")));
        }
    }
    QDomNodeList annotationNodeList = toolElement.elementsByTagName(QStringLiteral("annotation"));
    if (annotationNodeList.size() > 0) {
        QDomElement annotationEl = annotationNodeList.item(0).toElement();
        if (!annotationEl.isNull()) {
            if (annotationEl.hasAttribute(QStringLiteral("color"))) {
                annotColor = annotationEl.attribute(QStringLiteral("color"));
            }
            if (annotationEl.hasAttribute(QStringLiteral("innerColor"))) {
                innerColor = QColor(annotationEl.attribute(QStringLiteral("innerColor")));
            }
            if (annotationEl.hasAttribute(QStringLiteral("textColor"))) {
                textColor = QColor(annotationEl.attribute(QStringLiteral("textColor")));
            }
            if (annotationEl.hasAttribute(QStringLiteral("borderColor"))) {
                engineColor = QColor(annotationEl.attribute(QStringLiteral("borderColor")));
            }
            if (annotationEl.hasAttribute(QStringLiteral("icon"))) {
                icon = annotationEl.attribute(QStringLiteral("icon"));
            }
            if (annotationEl.hasAttribute(QStringLiteral("endStyle"))) {
                endStyle = annotationEl.attribute(QStringLiteral("endStyle")).toInt();
            }
        }
    }
    if (!engineColor.isValid()) {
        engineColor = QColor(0, 0, 0);
    }
    if (!textColor.isValid()) {
        textColor = QColor(0, 0, 0);
    }

    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    const auto drawLetter = [&p](const QString &letter, const QColor &color) {
        QFont font = qApp->font();
        font.setBold(true);
        font.setPointSize(16);
        p.setFont(font);
        p.setPen(color);
        p.drawText(QRectF(0, 3, 32, 22), Qt::AlignCenter, letter);
    };
    const auto drawArrowHead = [&p](const QPointF &tip, const QPointF &left, const QPointF &right) {
        QPainterPath head;
        head.moveTo(tip);
        head.lineTo(left);
        head.moveTo(tip);
        head.lineTo(right);
        p.drawPath(head);
    };

    if (annotType == QLatin1String("ellipse")) {
        p.setBrush(Qt::NoBrush);
        if (innerColor.isValid()) {
            p.setBrush(innerColor);
        }
        p.setPen(QPen(engineColor, 2));
        p.drawEllipse(QRectF(4, 9, 24, 15));
    } else if (annotType == QLatin1String("highlight")) {
        QColor markerColor = engineColor;
        markerColor.setAlpha(190);
        p.setPen(Qt::NoPen);
        p.setBrush(markerColor);
        p.drawRoundedRect(QRectF(3, 10, 26, 11), 2.5, 2.5);
        p.drawRoundedRect(QRectF(6, 18, 20, 7), 2.0, 2.0);
        drawLetter(QStringLiteral("H"), QColor(45, 45, 45));
    } else if (annotType == QLatin1String("ink")) {
        QPainterPath path;
        path.moveTo(4, 22);
        path.cubicTo(9, 8, 16, 27, 21, 13);
        path.cubicTo(23, 8, 26, 9, 28, 12);
        p.setPen(QPen(engineColor, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
    } else if (annotType == QLatin1String("note-inline")) {
        p.setPen(QPen(engineColor, 1.7));
        p.setBrush(annotColor.isValid() ? annotColor : QColor(255, 255, 160, 180));
        p.drawRoundedRect(QRectF(5, 7, 22, 18), 2, 2);
        p.setPen(QPen(QColor(100, 100, 100), 1, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(9, 13), QPointF(23, 13));
        p.drawLine(QPointF(9, 18), QPointF(20, 18));
    } else if (annotType == QLatin1String("note-linked")) {
        p.setPen(QPen(engineColor, 1.7));
        p.setBrush(annotColor.isValid() ? annotColor : QColor(255, 255, 160, 220));
        QPainterPath note;
        note.addRoundedRect(QRectF(7, 6, 18, 20), 2, 2);
        p.drawPath(note);
        QPainterPath fold;
        fold.moveTo(20, 6);
        fold.lineTo(25, 11);
        fold.lineTo(20, 11);
        fold.closeSubpath();
        p.setBrush(QColor(255, 255, 255, 160));
        p.drawPath(fold);
    } else if (annotType == QLatin1String("note-callout")) {
        p.setBrush(Qt::NoBrush);
        if (annotColor.isValid()) {
            p.setBrush(annotColor);
        }
        p.setPen(QPen(engineColor, 2));
        p.drawLine(QPointF(4, 24), QPointF(12, 16));
        p.drawLine(QPointF(12, 16), QPointF(19, 16));
        p.drawRoundedRect(QRectF(18, 8, 11, 13), 1.5, 1.5);
    } else if (annotType == QLatin1String("polygon")) {
        QPainterPath path;
        path.moveTo(6, 20);
        path.lineTo(11, 8);
        path.lineTo(24, 9);
        path.lineTo(28, 20);
        path.lineTo(17, 26);
        path.closeSubpath();
        p.setBrush(Qt::NoBrush);
        if (innerColor.isValid()) {
            p.setBrush(innerColor);
        }
        p.setPen(QPen(engineColor, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
    } else if (annotType == QLatin1String("rectangle")) {
        p.setBrush(Qt::NoBrush);
        if (innerColor.isValid()) {
            p.setBrush(innerColor);
        }
        p.setPen(QPen(engineColor, 2));
        p.drawRect(QRectF(5, 8, 22, 17));
    } else if (annotType == QLatin1String("squiggly")) {
        drawLetter(QStringLiteral("W"), QColor(45, 45, 45));
        QPainterPath wave;
        wave.moveTo(4, 26);
        for (int x = 4; x <= 24; x += 4) {
            wave.quadTo(QPointF(x, 21), QPointF(x + 2, 26));
            wave.quadTo(QPointF(x + 4, 31), QPointF(x + 6, 26));
        }
        p.setPen(QPen(engineColor, 2.4, Qt::SolidLine, Qt::RoundCap));
        p.drawPath(wave);
    } else if (annotType == QLatin1String("stamp")) {
        p.save();
        p.translate(16, 16);
        p.rotate(-10);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(engineColor, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawRoundedRect(QRectF(-10, -7, 20, 14), 2, 2);
        p.drawLine(QPointF(-6, 1), QPointF(6, 1));
        p.restore();
    } else if (annotType == QLatin1String("straight-line")) {
        p.setPen(QPen(engineColor, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (endStyle == 3 || endStyle == 4 || endStyle == 7 || endStyle == 8) {
            p.drawLine(QPointF(6, 24), QPointF(25, 8));
            drawArrowHead(QPointF(25, 8), QPointF(24, 15), QPointF(18, 8));
        } else {
            p.drawLine(QPointF(5, 17), QPointF(27, 17));
        }
    } else if (annotType == QLatin1String("strikeout")) {
        drawLetter(QStringLiteral("X"), QColor(45, 45, 45));
        p.setPen(QPen(engineColor, 3.0, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(4, 16), QPointF(28, 16));
    } else if (annotType == QLatin1String("underline")) {
        drawLetter(QStringLiteral("U"), QColor(45, 45, 45));
        p.setPen(QPen(engineColor, 3.0, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(4, 27), QPointF(28, 27));
    } else if (annotType == QLatin1String("typewriter")) {
        QFont font = qApp->font();
        font.setBold(true);
        font.setPointSize(20);
        p.setFont(font);
        p.setPen(textColor);
        p.drawText(QRectF(0, 1, 32, 30), Qt::AlignCenter, QStringLiteral("T"));
    } else {
        /* Unrecognized annotation type -- It shouldn't happen */
        p.setPen(QPen(engineColor));
        p.drawText(QPoint(20, 31), QStringLiteral("?"));
    }

    return pixmap;
}

void PageViewAnnotator::setupActions(KActionCollection *ac)
{
    if (!m_actionHandler) {
        m_actionHandler = new AnnotationActionHandler(this, ac);
    }
}

void PageViewAnnotator::setupActionsPostGUIActivated()
{
    m_actionHandler->setupAnnotationToolBarVisibilityAction();
}

bool PageViewAnnotator::continuousMode()
{
    return m_continuousMode;
}

void PageViewAnnotator::setContinuousMode(bool enabled)
{
    m_continuousMode = enabled;
    Okular::Settings::setAnnotationContinuousMode(enabled);
    Okular::Settings::self()->save();
}

bool PageViewAnnotator::constrainRatioAndAngleActive()
{
    return m_constrainRatioAndAngle;
}

void PageViewAnnotator::setConstrainRatioAndAngle(bool enabled)
{
    m_constrainRatioAndAngle = enabled;
}

void PageViewAnnotator::setToolsEnabled(bool enabled)
{
    if (m_actionHandler) {
        m_actionHandler->setToolsEnabled(enabled);
    }
}

void PageViewAnnotator::setTextToolsEnabled(bool enabled)
{
    if (m_actionHandler) {
        m_actionHandler->setTextToolsEnabled(enabled);
    }
}

void PageViewAnnotator::saveBuiltinAnnotationTools()
{
    if (!m_builtinToolsDefinition || !m_builtinToolsDefinition->hasRequiredBuiltinTools()) {
        qCWarning(OkularUiDebug) << "Refusing to save incomplete builtin annotation tools";
        return;
    }
    if (m_builtinToolsDefinition->hasTransientSystemToolState()) {
        qCWarning(OkularUiDebug) << "Refusing to save transient system annotation tool state";
        return;
    }
    Okular::Settings::setBuiltinAnnotationTools(m_builtinToolsDefinition->toStringList());
    Okular::Settings::self()->save();
}

QDomElement PageViewAnnotator::builtinTool(int toolId)
{
    return m_builtinToolsDefinition->tool(toolId);
}

QDomElement PageViewAnnotator::quickTool(int toolId)
{
    return m_quickToolsDefinition->tool(toolId);
}

QDomElement PageViewAnnotator::currentEngineElement()
{
    AnnotationTools *toolsDefinition = m_lastToolsDefinition ? m_lastToolsDefinition : m_builtinToolsDefinition;
    return toolsDefinition->tool(m_lastToolId).firstChildElement(QStringLiteral("engine"));
}

QDomElement PageViewAnnotator::currentAnnotationElement()
{
    return currentEngineElement().firstChildElement(QStringLiteral("annotation"));
}

bool PageViewAnnotator::currentToolSupportsContinuousCreation()
{
    const QDomElement annotationElement = currentAnnotationElement();
    const QString annotationType = annotationElement.attribute(QStringLiteral("type"));
    if (annotationType == QLatin1String("FreeText") || annotationType == QLatin1String("Typewriter") || annotationType == QLatin1String("Callout") || annotationType == QLatin1String("Text")) {
        return false;
    }
    if (annotationElement.attribute(QStringLiteral("okularLatex")).toInt() != 0 || annotationElement.hasAttribute(QStringLiteral("templateNoteData"))) {
        return false;
    }
    return true;
}

void PageViewAnnotator::setAnnotationWidth(double width)
{
    currentAnnotationElement().setAttribute(QStringLiteral("width"), QString::number(width));
    saveBuiltinAnnotationTools();
    selectLastTool();
}

void PageViewAnnotator::setAnnotationColor(const QColor &color)
{
    currentEngineElement().setAttribute(QStringLiteral("color"), color.name(QColor::HexRgb));
    QDomElement annotationElement = currentAnnotationElement();
    QString annotType = annotationElement.attribute(QStringLiteral("type"));
    if (annotType == QLatin1String("Typewriter")) {
        annotationElement.setAttribute(QStringLiteral("textColor"), color.name(QColor::HexRgb));
    } else if (annotType == QLatin1String("FreeText") || annotType == QLatin1String("Callout")
               || (annotType == QLatin1String("Stamp") && annotationElement.attribute(QStringLiteral("okularLatex")).toInt() != 0)) {
        annotationElement.setAttribute(QStringLiteral("borderColor"), color.name(QColor::HexRgb));
    } else {
        annotationElement.setAttribute(QStringLiteral("color"), color.name(QColor::HexRgb));
    }
    saveBuiltinAnnotationTools();
    selectLastTool();
}

void PageViewAnnotator::setAnnotationInnerColor(const QColor &color)
{
    QDomElement annotationElement = currentAnnotationElement();
    const QString annotType = annotationElement.attribute(QStringLiteral("type"));
    if (annotType == QLatin1String("FreeText") || annotType == QLatin1String("Callout")
        || (annotType == QLatin1String("Stamp") && annotationElement.attribute(QStringLiteral("okularLatex")).toInt() != 0)) {
        annotationElement.setAttribute(QStringLiteral("color"), color.name(QColor::HexArgb));
        saveBuiltinAnnotationTools();
        selectLastTool();
        return;
    }
    if (color == Qt::transparent) {
        annotationElement.removeAttribute(QStringLiteral("innerColor"));
    } else {
        annotationElement.setAttribute(QStringLiteral("innerColor"), color.name(QColor::HexRgb));
    }
    saveBuiltinAnnotationTools();
    selectLastTool();
}

void PageViewAnnotator::setAnnotationTextColor(const QColor &color)
{
    currentAnnotationElement().setAttribute(QStringLiteral("textColor"), color.name(QColor::HexRgb));
    saveBuiltinAnnotationTools();
    selectLastTool();
}

void PageViewAnnotator::setAnnotationOpacity(double opacity)
{
    currentAnnotationElement().setAttribute(QStringLiteral("opacity"), QString::number(opacity));
    saveBuiltinAnnotationTools();
    selectLastTool();
}

void PageViewAnnotator::setAnnotationFont(const QFont &font)
{
    QDomElement annotationElement = currentAnnotationElement();
    annotationElement.removeAttribute(QStringLiteral("font"));
    annotationElement.setAttribute(QStringLiteral("fontName"), QStringLiteral("Helvetica"));
    annotationElement.setAttribute(QStringLiteral("fontSize"), QString::number(font.pointSizeF() > 0 ? font.pointSizeF() : 10.0));
    saveBuiltinAnnotationTools();
    selectLastTool();
}

void PageViewAnnotator::setAnnotationFontName(const QString &fontName, double pointSize)
{
    QDomElement annotationElement = currentAnnotationElement();
    annotationElement.removeAttribute(QStringLiteral("font"));
    annotationElement.setAttribute(QStringLiteral("fontName"), fontName);
    annotationElement.setAttribute(QStringLiteral("fontSize"), QString::number(pointSize));
    saveBuiltinAnnotationTools();
    selectLastTool();
}

void PageViewAnnotator::addToQuickAnnotations()
{
    QDomElement sourceToolElement = m_builtinToolsDefinition->tool(m_lastToolId);
    if (sourceToolElement.isNull()) {
        return;
    }

    // set custom name for quick annotation
    bool ok = false;
    QString itemText = QInputDialog::getText(m_pageView, i18n("Add favorite annotation"), i18n("Custom annotation name:"), QLineEdit::Normal, defaultToolName(sourceToolElement), &ok);
    if (!ok) {
        return;
    }

    QDomElement toolElement = sourceToolElement.cloneNode().toElement();
    // store name attribute only if the user specified a customized name
    if (!itemText.isEmpty()) {
        toolElement.setAttribute(QStringLiteral("name"), itemText);
    }
    m_quickToolsDefinition->appendTool(toolElement);
    Okular::Settings::setQuickAnnotationTools(m_quickToolsDefinition->toStringList());
    Okular::Settings::self()->save();
}

void PageViewAnnotator::slotAdvancedSettings()
{
    QDomElement toolElement = m_builtinToolsDefinition->tool(m_lastToolId);

    EditAnnotToolDialog t(nullptr, toolElement, true);
    if (t.exec() != QDialog::Accepted) {
        return;
    }

    QDomElement toolElementUpdated = t.toolXml().documentElement();
    int toolId = toolElement.attribute(QStringLiteral("id")).toInt();
    m_builtinToolsDefinition->updateTool(toolElementUpdated, toolId);
    saveBuiltinAnnotationTools();
    selectLastTool();
}

/* kate: replace-tabs on; indent-width 4; */
