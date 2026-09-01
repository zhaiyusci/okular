/*
    SPDX-FileCopyrightText: 2008 Pino Toscano <pino@kde.org>
    SPDX-FileCopyrightText: 2012 Guillermo A. Amaral B. <gamaral@kde.org>

    Work sponsored by the LiMux project of the city of Munich:
    SPDX-FileCopyrightText: 2017 Klarälvdalens Datakonsult AB a KDAB Group company <info@kdab.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "annots.h"

// qt/kde includes
#include <algorithm>
#include <cmath>
#include <limits>

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QStringList>
#include <QTemporaryFile>
#include <QVariant>

#include <core/annotations.h>
#include <core/area.h>
#include <core/latexnotegeometry.h>

#include "debug_pdf.h"
#include "generator_pdf.h"
#include "imagescaling.h"
#include "popplerembeddedfile.h"
#include "popplerversion.h"

Q_DECLARE_METATYPE(Poppler::Annotation *)

extern Okular::Sound *createSoundFromPopplerSound(const Poppler::SoundObject *popplerSound);
extern Okular::Movie *createMovieFromPopplerMovie(const Poppler::MovieObject *popplerMovie);
extern Okular::Movie *createMovieFromPopplerScreen(const Poppler::LinkRendition *popplerScreen);
extern QPair<Okular::Movie *, Okular::EmbeddedFile *> createMovieFromPopplerRichMedia(const Poppler::RichMediaAnnotation *popplerRichMedia);

struct SignatureImageHelper {
    SignatureImageHelper(std::unique_ptr<QTemporaryFile> &&tmpfile, const QString &imgpath)
        : imageFile(std::move(tmpfile))
        , imagePath(imgpath)
    {
    }
    std::unique_ptr<QTemporaryFile> imageFile;
    QString imagePath;
};

static void disposeAnnotation(const Okular::Annotation *ann)
{
    Poppler::Annotation *popplerAnn = qvariant_cast<Poppler::Annotation *>(ann->nativeId());
    delete popplerAnn;
}

static QPointF normPointToPointF(const Okular::NormalizedPoint &pt)
{
    return QPointF(pt.x, pt.y);
}

static QRectF normRectToRectF(const Okular::NormalizedRect &rect)
{
    return QRectF(QPointF(rect.left, rect.top), QPointF(rect.right, rect.bottom));
}

static Okular::NormalizedRect pageRectToNormRect(const QRectF &rect, const QSizeF &pageSize)
{
    if (pageSize.width() <= 0.0 || pageSize.height() <= 0.0) {
        return {};
    }

    const QRectF normalized = rect.normalized();
    return Okular::NormalizedRect(normalized.left() / pageSize.width(), 1.0 - normalized.bottom() / pageSize.height(), normalized.right() / pageSize.width(), 1.0 - normalized.top() / pageSize.height());
}

static QPointF normPointToPagePointF(const Okular::NormalizedPoint &point, const Poppler::Page *page)
{
    if (!page) {
        return normPointToPointF(point);
    }

    const QSizeF pageSize = page->pageSizeF();
    return QPointF(point.x * pageSize.width(), (1.0 - point.y) * pageSize.height());
}

static Okular::NormalizedPoint pagePointToNormPoint(const QPointF &point, const QSizeF &pageSize)
{
    if (pageSize.width() <= 0.0 || pageSize.height() <= 0.0) {
        return {};
    }

    return {point.x() / pageSize.width(), 1.0 - point.y() / pageSize.height()};
}

static QRectF normRectToPageRectF(const Okular::NormalizedRect &rect, const Poppler::Page *page)
{
    if (!page) {
        return normRectToRectF(rect);
    }

    const QSizeF pageSize = page->pageSizeF();
    return QRectF(QPointF(rect.left * pageSize.width(), (1.0 - rect.bottom) * pageSize.height()), QPointF(rect.right * pageSize.width(), (1.0 - rect.top) * pageSize.height())).normalized();
}

static void setCustomColorProperty(Poppler::Annotation *annotation, const QString &prefix, const QColor &color)
{
    if (!annotation || !color.isValid()) {
        return;
    }

    annotation->setCustomRealProperty(prefix + QStringLiteral("Red"), color.redF());
    annotation->setCustomRealProperty(prefix + QStringLiteral("Green"), color.greenF());
    annotation->setCustomRealProperty(prefix + QStringLiteral("Blue"), color.blueF());
    annotation->setCustomRealProperty(prefix + QStringLiteral("Alpha"), color.alphaF());
}

static QColor customColorProperty(const Poppler::Annotation *annotation, const QString &prefix, const QColor &fallback = {})
{
    if (!annotation) {
        return fallback;
    }

    const double alpha = annotation->customRealProperty(prefix + QStringLiteral("Alpha"), -1.0);
    if (alpha < 0.0) {
        return fallback;
    }

    QColor color;
    color.setRgbF(qBound(0.0, annotation->customRealProperty(prefix + QStringLiteral("Red"), 0.0), 1.0),
                  qBound(0.0, annotation->customRealProperty(prefix + QStringLiteral("Green"), 0.0), 1.0),
                  qBound(0.0, annotation->customRealProperty(prefix + QStringLiteral("Blue"), 0.0), 1.0),
                  qBound(0.0, alpha, 1.0));
    return color;
}

namespace
{
struct StampAppearanceTransfer {
    Poppler::Document *document = nullptr;
    std::unique_ptr<Poppler::AnnotationAppearance> appearance;
};

constexpr int LatexNoteDataVersion = 20260610;
const QString LatexNoteDataKey = QStringLiteral("LatexNoteData");
const QString TemplateNoteDataKey = QStringLiteral("TemplateNoteData");

const QStringList &pdfBase14FontNames()
{
    static const QStringList names {
        QStringLiteral("Times-Roman"),
        QStringLiteral("Times-Bold"),
        QStringLiteral("Times-Italic"),
        QStringLiteral("Times-BoldItalic"),
        QStringLiteral("Helvetica"),
        QStringLiteral("Helvetica-Bold"),
        QStringLiteral("Helvetica-Oblique"),
        QStringLiteral("Helvetica-BoldOblique"),
        QStringLiteral("Courier"),
        QStringLiteral("Courier-Bold"),
        QStringLiteral("Courier-Oblique"),
        QStringLiteral("Courier-BoldOblique"),
        QStringLiteral("Symbol"),
        QStringLiteral("ZapfDingbats"),
    };
    return names;
}

QString normalizedPdfBase14FontName(const QString &fontName)
{
    const QString name = fontName.startsWith(QLatin1Char('/')) ? fontName.mid(1) : fontName;
    if (pdfBase14FontNames().contains(name)) {
        return name;
    }

    static const QHash<QString, QString> aliases {
        {QStringLiteral("Helv"), QStringLiteral("Helvetica")},
        {QStringLiteral("HeBo"), QStringLiteral("Helvetica-Bold")},
        {QStringLiteral("HeOb"), QStringLiteral("Helvetica-Oblique")},
        {QStringLiteral("HeBO"), QStringLiteral("Helvetica-BoldOblique")},
        {QStringLiteral("Cour"), QStringLiteral("Courier")},
        {QStringLiteral("CoBo"), QStringLiteral("Courier-Bold")},
        {QStringLiteral("CoOb"), QStringLiteral("Courier-Oblique")},
        {QStringLiteral("CoBO"), QStringLiteral("Courier-BoldOblique")},
        {QStringLiteral("TiRo"), QStringLiteral("Times-Roman")},
        {QStringLiteral("TiBo"), QStringLiteral("Times-Bold")},
        {QStringLiteral("TiIt"), QStringLiteral("Times-Italic")},
        {QStringLiteral("TiBI"), QStringLiteral("Times-BoldItalic")},
        {QStringLiteral("Symb"), QStringLiteral("Symbol")},
        {QStringLiteral("ZaDb"), QStringLiteral("ZapfDingbats")},
    };
    return aliases.value(name, QStringLiteral("Helvetica"));
}

struct ParsedLatexNoteData {
    bool valid = false;
    QString type;
    double layoutWidthPoints = 0.0;
    double paddingPoints = Okular::LatexNoteGeometry::defaultPaddingPoints();
    double fontSizePoints = 0.0;
    QColor textColor = Qt::black;
    QColor fillColor;
    QColor borderColor;
    double borderWidthPoints = 0.0;
    bool hasBoxRectPoints = false;
    QRectF boxRectPoints;
    bool hasNormalizedCalloutPoints = false;
    Okular::NormalizedPoint normalizedCalloutPoints[3];
    bool hasPageCalloutPoints = false;
    QPointF pageCalloutPoints[3];
};

QString colorToLatexNoteJson(const QColor &color)
{
    if (!color.isValid()) {
        return QString();
    }

    return color.name(QColor::HexArgb);
}

QColor colorFromLatexNoteJson(const QJsonObject &object, const QString &key, const QColor &fallback = {})
{
    const QString value = object.value(key).toString();
    if (value.isEmpty()) {
        return fallback;
    }

    const QColor color(value);
    return color.isValid() ? color : fallback;
}

QJsonArray pointToJsonArray(double x, double y)
{
    QJsonArray array;
    array.append(x);
    array.append(y);
    return array;
}

QJsonArray rectToJsonArray(const QRectF &rect)
{
    QJsonArray array;
    array.append(rect.left());
    array.append(rect.top());
    array.append(rect.right());
    array.append(rect.bottom());
    return array;
}

bool readPointArray(const QJsonValue &value, double *x, double *y)
{
    if (!x || !y || !value.isArray()) {
        return false;
    }

    const QJsonArray array = value.toArray();
    if (array.size() != 2 || !array.at(0).isDouble() || !array.at(1).isDouble()) {
        return false;
    }

    *x = array.at(0).toDouble();
    *y = array.at(1).toDouble();
    return std::isfinite(*x) && std::isfinite(*y);
}

bool readThreePointArray(const QJsonObject &object, const QString &key, Okular::NormalizedPoint points[3])
{
    const QJsonValue value = object.value(key);
    if (!value.isArray()) {
        return false;
    }

    const QJsonArray array = value.toArray();
    if (array.size() != 3) {
        return false;
    }

    for (int i = 0; i < 3; ++i) {
        double x = 0.0;
        double y = 0.0;
        if (!readPointArray(array.at(i), &x, &y)) {
            return false;
        }
        points[i] = Okular::NormalizedPoint(x, y);
    }
    return true;
}

bool readThreePointArray(const QJsonObject &object, const QString &key, QPointF points[3])
{
    const QJsonValue value = object.value(key);
    if (!value.isArray()) {
        return false;
    }

    const QJsonArray array = value.toArray();
    if (array.size() != 3) {
        return false;
    }

    for (int i = 0; i < 3; ++i) {
        double x = 0.0;
        double y = 0.0;
        if (!readPointArray(array.at(i), &x, &y)) {
            return false;
        }
        points[i] = QPointF(x, y);
    }
    return true;
}

ParsedLatexNoteData parseLatexNoteData(const QString &json)
{
    ParsedLatexNoteData data;
    if (json.trimmed().isEmpty()) {
        return data;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return data;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt() != LatexNoteDataVersion) {
        return data;
    }

    const QString type = root.value(QStringLiteral("type")).toString();
    if (type != QLatin1String("plain") && type != QLatin1String("boxed") && type != QLatin1String("callout")) {
        return data;
    }

    data.valid = true;
    data.type = type;

    const QJsonObject layout = root.value(QStringLiteral("layout")).toObject();
    const double layoutWidth = layout.value(QStringLiteral("widthPt")).toDouble(0.0);
    if (std::isfinite(layoutWidth) && layoutWidth > 0.0) {
        data.layoutWidthPoints = layoutWidth;
    }
    const double padding = layout.value(QStringLiteral("paddingPt")).toDouble(Okular::LatexNoteGeometry::defaultPaddingPoints());
    if (std::isfinite(padding) && padding >= 0.0) {
        data.paddingPoints = padding;
    }

    const QJsonObject style = root.value(QStringLiteral("style")).toObject();
    data.textColor = colorFromLatexNoteJson(style, QStringLiteral("textColor"), Qt::black);
    const double fontSize = style.value(QStringLiteral("fontSizePt")).toDouble(0.0);
    if (std::isfinite(fontSize) && fontSize > 0.0) {
        data.fontSizePoints = fontSize;
    }
    data.fillColor = colorFromLatexNoteJson(style, QStringLiteral("fillColor"));
    data.borderColor = colorFromLatexNoteJson(style, QStringLiteral("borderColor"));
    const double borderWidth = style.value(QStringLiteral("borderWidthPt")).toDouble(type == QLatin1String("plain") ? 0.0 : 1.0);
    if (std::isfinite(borderWidth) && borderWidth >= 0.0) {
        data.borderWidthPoints = borderWidth;
    }

    const QJsonObject callout = root.value(QStringLiteral("callout")).toObject();
    const QJsonArray boxRect = callout.value(QStringLiteral("boxRectPt")).toArray();
    if (boxRect.size() == 4 && boxRect.at(0).isDouble() && boxRect.at(1).isDouble() && boxRect.at(2).isDouble() && boxRect.at(3).isDouble()) {
        const QRectF rect(QPointF(boxRect.at(0).toDouble(), boxRect.at(1).toDouble()), QPointF(boxRect.at(2).toDouble(), boxRect.at(3).toDouble()));
        if (rect.isValid()) {
            data.hasBoxRectPoints = true;
            data.boxRectPoints = rect.normalized();
        }
    }
    data.hasNormalizedCalloutPoints = readThreePointArray(callout, QStringLiteral("pointsNorm"), data.normalizedCalloutPoints);
    data.hasPageCalloutPoints = readThreePointArray(callout, QStringLiteral("pointsPt"), data.pageCalloutPoints);

    return data;
}

QString latexNoteDataForStampAnnotation(const Okular::StampAnnotation *annotation, const Poppler::Page *page)
{
    if (!annotation || !annotation->isOkularLatex()) {
        return QString();
    }

    const QString type =
        annotation->latexNoteType() == Okular::Annotation::LatexNoteCallout ? QStringLiteral("callout") : (annotation->latexNoteType() == Okular::Annotation::LatexNoteBoxed ? QStringLiteral("boxed") : QStringLiteral("plain"));

    QJsonObject layout;
    layout.insert(QStringLiteral("widthPt"), annotation->latexLayoutWidth());
    layout.insert(QStringLiteral("paddingPt"), annotation->latexPadding());

    QJsonObject style;
    style.insert(QStringLiteral("textColor"), colorToLatexNoteJson(annotation->latexTextColor().isValid() ? annotation->latexTextColor() : QColor(Qt::black)));
    style.insert(QStringLiteral("fontSizePt"), annotation->latexFontSize());
    if (type != QLatin1String("plain")) {
        style.insert(QStringLiteral("fillColor"), colorToLatexNoteJson(annotation->latexFillColor()));
        style.insert(QStringLiteral("borderColor"), colorToLatexNoteJson(annotation->latexBorderColor().isValid() ? annotation->latexBorderColor() : QColor(Qt::black)));
        style.insert(QStringLiteral("borderWidthPt"), annotation->style().width());
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), LatexNoteDataVersion);
    root.insert(QStringLiteral("type"), type);
    root.insert(QStringLiteral("layout"), layout);
    root.insert(QStringLiteral("style"), style);

    if (annotation->latexNoteType() == Okular::Annotation::LatexNoteCallout) {
        QJsonObject callout;
        callout.insert(QStringLiteral("boxRectPt"), rectToJsonArray(normRectToPageRectF(annotation->boundingRectangle(), page)));

        QJsonArray pointsNorm;
        QJsonArray pointsPt;
        for (int i = 0; i < 3; ++i) {
            const Okular::NormalizedPoint point = annotation->latexCalloutPoint(i);
            pointsNorm.append(pointToJsonArray(point.x, point.y));
            const QPointF pagePoint = normPointToPagePointF(point, page);
            pointsPt.append(pointToJsonArray(pagePoint.x(), pagePoint.y()));
        }
        callout.insert(QStringLiteral("pointsNorm"), pointsNorm);
        callout.insert(QStringLiteral("pointsPt"), pointsPt);
        root.insert(QStringLiteral("callout"), callout);
    }

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

Poppler::StampAnnotation::CustomPdfAppearanceOptions latexStampAppearanceOptions(const Okular::StampAnnotation *annotation, const Poppler::Page *page, QRectF *expandedBoundary = nullptr)
{
    Poppler::StampAnnotation::CustomPdfAppearanceOptions options;
    if (!annotation || !page) {
        return options;
    }

    const double padding = std::isfinite(annotation->latexPadding()) && annotation->latexPadding() >= 0.0 ? annotation->latexPadding() : Okular::LatexNoteGeometry::defaultPaddingPoints();
    const double contentInset = padding;
    const QRectF boxRectPoints = normRectToPageRectF(annotation->boundingRectangle(), page);
    QRectF appearanceRectPoints = boxRectPoints;
    const double borderWidth = annotation->style().width();

    if (annotation->latexNoteType() == Okular::Annotation::LatexNoteCallout) {
        const QPointF pointA = normPointToPagePointF(annotation->latexCalloutPoint(0), page);
        const QPointF pointB = normPointToPagePointF(annotation->latexCalloutPoint(1), page);
        const QPointF pointC = normPointToPagePointF(annotation->latexCalloutPoint(2), page);
        const double linePadding = qMax(8.0, qMax(1.0, borderWidth) * 5.0);
        appearanceRectPoints = QRectF(QPointF(std::min({boxRectPoints.left(), pointA.x(), pointB.x(), pointC.x()}) - linePadding, std::min({boxRectPoints.top(), pointA.y(), pointB.y(), pointC.y()}) - linePadding),
                                      QPointF(std::max({boxRectPoints.right(), pointA.x(), pointB.x(), pointC.x()}) + linePadding, std::max({boxRectPoints.bottom(), pointA.y(), pointB.y(), pointC.y()}) + linePadding))
                                   .normalized();
        if (expandedBoundary) {
            *expandedBoundary = normRectToRectF(pageRectToNormRect(appearanceRectPoints, page->pageSizeF()));
        }
    }

    const QSizeF outerSizePoints(appearanceRectPoints.width(), appearanceRectPoints.height());
    const double frameX = boxRectPoints.left() - appearanceRectPoints.left();
    const double frameY = boxRectPoints.top() - appearanceRectPoints.top();
    const double frameWidth = boxRectPoints.width();
    const double frameHeight = boxRectPoints.height();

    options.outerSize = outerSizePoints;
    options.contentOffset = QPointF(contentInset, contentInset);
    options.frameRect = QRectF(frameX, frameY, frameWidth, frameHeight);
    options.alignContentToFrameTopLeft = true;
    options.contentFrameInset = contentInset;
    if (annotation->latexNoteType() == Okular::Annotation::LatexNoteBoxed || annotation->latexNoteType() == Okular::Annotation::LatexNoteCallout) {
        options.borderWidth = qMax(0.0, borderWidth);
        options.fillColor = annotation->latexFillColor();
        options.borderColor = annotation->latexBorderColor().isValid() ? annotation->latexBorderColor() : QColor(Qt::black);
    } else {
        options.borderWidth = 0.0;
        options.fillColor = Qt::transparent;
        options.borderColor = Qt::transparent;
    }
    if (annotation->latexNoteType() == Okular::Annotation::LatexNoteCallout) {
        QVector<QPointF> leaderLine;
        leaderLine
            << QPointF(normPointToPagePointF(annotation->latexCalloutPoint(0), page).x() - appearanceRectPoints.left(), normPointToPagePointF(annotation->latexCalloutPoint(0), page).y() - appearanceRectPoints.top())
            << QPointF(normPointToPagePointF(annotation->latexCalloutPoint(1), page).x() - appearanceRectPoints.left(), normPointToPagePointF(annotation->latexCalloutPoint(1), page).y() - appearanceRectPoints.top())
            << QPointF(normPointToPagePointF(annotation->latexCalloutPoint(2), page).x() - appearanceRectPoints.left(), normPointToPagePointF(annotation->latexCalloutPoint(2), page).y() - appearanceRectPoints.top());
        options.leaderLine = leaderLine;
    }

    return options;
}

}

// Poppler and Okular share the same flag values, but we don't want to export internal flags
static int maskExportedFlags(int flags)
{
    return flags &
        (Okular::Annotation::Hidden | Okular::Annotation::FixedSize | Okular::Annotation::FixedRotation | Okular::Annotation::DenyPrint | Okular::Annotation::DenyWrite | Okular::Annotation::DenyDelete |
         Okular::Annotation::ToggleHidingOnMouse);
}

// BEGIN PopplerAnnotationProxy implementation
PopplerAnnotationProxy::PopplerAnnotationProxy(Poppler::Document *doc, QMutex *userMutex, QHash<Okular::Annotation *, Poppler::Annotation *> *annotsOnOpenHash, const QVector<int> *pageOrder)
    : ppl_doc(doc)
    , mutex(userMutex)
    , annotationsOnOpenHash(annotsOnOpenHash)
    , m_pageOrder(pageOrder)
{
}

PopplerAnnotationProxy::~PopplerAnnotationProxy()
{
}

bool PopplerAnnotationProxy::supports(Capability cap) const
{
    switch (cap) {
    case Addition:
    case Modification:
    case Removal:
        return true;
    default:
        return false;
    }
}

int PopplerAnnotationProxy::nativePageForLogicalPage(int logicalPage) const
{
    if (m_pageOrder && logicalPage >= 0 && logicalPage < m_pageOrder->size()) {
        return m_pageOrder->at(logicalPage);
    }
    return logicalPage;
}

static Poppler::TextAnnotation::TextType okularToPoppler(Okular::TextAnnotation::TextType ott)
{
    switch (ott) {
    case Okular::TextAnnotation::Linked:
        return Poppler::TextAnnotation::Linked;
    case Okular::TextAnnotation::InPlace:
        return Poppler::TextAnnotation::InPlace;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << ott;
    }

    return Poppler::TextAnnotation::Linked;
}

static Poppler::Annotation::LineEffect okularToPoppler(Okular::Annotation::LineEffect ole)
{
    switch (ole) {
    case Okular::Annotation::NoEffect:
        return Poppler::Annotation::NoEffect;
    case Okular::Annotation::Cloudy:
        return Poppler::Annotation::Cloudy;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << ole;
    }

    return Poppler::Annotation::NoEffect;
}

static Poppler::Annotation::LineStyle okularToPoppler(Okular::Annotation::LineStyle ols)
{
    switch (ols) {
    case Okular::Annotation::Solid:
        return Poppler::Annotation::Solid;
    case Okular::Annotation::Dashed:
        return Poppler::Annotation::Dashed;
    case Okular::Annotation::Beveled:
        return Poppler::Annotation::Beveled;
    case Okular::Annotation::Inset:
        return Poppler::Annotation::Inset;
    case Okular::Annotation::Underline:
        return Poppler::Annotation::Underline;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << ols;
    }

    return Poppler::Annotation::Solid;
}

static Poppler::TextAnnotation::InplaceIntent okularToPoppler(Okular::TextAnnotation::InplaceIntent oii)
{
    switch (oii) {
    case Okular::TextAnnotation::Unknown:
        return Poppler::TextAnnotation::Unknown;
    case Okular::TextAnnotation::Callout:
        return Poppler::TextAnnotation::Callout;
    case Okular::TextAnnotation::TypeWriter:
        return Poppler::TextAnnotation::TypeWriter;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << oii;
    }

    return Poppler::TextAnnotation::Unknown;
}

static Poppler::LineAnnotation::TermStyle okularToPoppler(Okular::LineAnnotation::TermStyle ots)
{
    switch (ots) {
    case Okular::LineAnnotation::Square:
        return Poppler::LineAnnotation::Square;
    case Okular::LineAnnotation::Circle:
        return Poppler::LineAnnotation::Circle;
    case Okular::LineAnnotation::Diamond:
        return Poppler::LineAnnotation::Diamond;
    case Okular::LineAnnotation::OpenArrow:
        return Poppler::LineAnnotation::OpenArrow;
    case Okular::LineAnnotation::ClosedArrow:
        return Poppler::LineAnnotation::ClosedArrow;
    case Okular::LineAnnotation::None:
        return Poppler::LineAnnotation::None;
    case Okular::LineAnnotation::Butt:
        return Poppler::LineAnnotation::Butt;
    case Okular::LineAnnotation::ROpenArrow:
        return Poppler::LineAnnotation::ROpenArrow;
    case Okular::LineAnnotation::RClosedArrow:
        return Poppler::LineAnnotation::RClosedArrow;
    case Okular::LineAnnotation::Slash:
        return Poppler::LineAnnotation::Slash;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << ots;
    }

    return Poppler::LineAnnotation::None;
}

static Poppler::LineAnnotation::LineIntent okularToPoppler(Okular::LineAnnotation::LineIntent oli)
{
    switch (oli) {
    case Okular::LineAnnotation::Unknown:
        return Poppler::LineAnnotation::Unknown;
    case Okular::LineAnnotation::Arrow:
        return Poppler::LineAnnotation::Arrow;
    case Okular::LineAnnotation::Dimension:
        return Poppler::LineAnnotation::Dimension;
    case Okular::LineAnnotation::PolygonCloud:
        return Poppler::LineAnnotation::PolygonCloud;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << oli;
    }

    return Poppler::LineAnnotation::Unknown;
}

static Poppler::GeomAnnotation::GeomType okularToPoppler(Okular::GeomAnnotation::GeomType ogt)
{
    switch (ogt) {
    case Okular::GeomAnnotation::InscribedSquare:
        return Poppler::GeomAnnotation::InscribedSquare;
    case Okular::GeomAnnotation::InscribedCircle:
        return Poppler::GeomAnnotation::InscribedCircle;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << ogt;
    }

    return Poppler::GeomAnnotation::InscribedSquare;
}

static Poppler::HighlightAnnotation::HighlightType okularToPoppler(Okular::HighlightAnnotation::HighlightType oht)
{
    switch (oht) {
    case Okular::HighlightAnnotation::Highlight:
        return Poppler::HighlightAnnotation::Highlight;
    case Okular::HighlightAnnotation::Squiggly:
        return Poppler::HighlightAnnotation::Squiggly;
    case Okular::HighlightAnnotation::Underline:
        return Poppler::HighlightAnnotation::Underline;
    case Okular::HighlightAnnotation::StrikeOut:
        return Poppler::HighlightAnnotation::StrikeOut;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << oht;
    }

    return Poppler::HighlightAnnotation::Highlight;
}

static Poppler::CaretAnnotation::CaretSymbol okularToPoppler(Okular::CaretAnnotation::CaretSymbol ocs)
{
    switch (ocs) {
    case Okular::CaretAnnotation::None:
        return Poppler::CaretAnnotation::None;
    case Okular::CaretAnnotation::P:
        return Poppler::CaretAnnotation::P;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << ocs;
    }

    return Poppler::CaretAnnotation::None;
}

static Poppler::Annotation::Style okularToPoppler(const Okular::Annotation::Style &oStyle)
{
    Poppler::Annotation::Style pStyle;
    pStyle.setColor(oStyle.color());
    pStyle.setOpacity(oStyle.opacity());
    pStyle.setLineEffect(okularToPoppler(oStyle.lineEffect()));
    pStyle.setEffectIntensity(oStyle.effectIntensity());
    pStyle.setWidth(oStyle.width());
    pStyle.setLineStyle(okularToPoppler(oStyle.lineStyle()));
    pStyle.setXCorners(oStyle.xCorners());
    pStyle.setYCorners(oStyle.yCorners());

    return pStyle;
}

static Poppler::Annotation::Popup okularToPoppler(const Okular::Annotation::Window &oWindow)
{
    Poppler::Annotation::Popup pWindow;
    pWindow.setGeometry(QRectF(oWindow.topLeft().x, oWindow.topLeft().y, oWindow.width(), oWindow.height()));
    // flags being ints is super fragile, should be enums on both ends, but Poppler::Annotation::setPopup is a noop so it's not like it matters
    pWindow.setFlags(oWindow.flags());
    pWindow.setTitle(oWindow.title());
    pWindow.setSummary(oWindow.summary());

    return pWindow;
}

static void setSharedAnnotationPropertiesToPopplerAnnotation(const Okular::Annotation *okularAnnotation, Poppler::Annotation *popplerAnnotation)
{
    popplerAnnotation->setAuthor(okularAnnotation->author());
    popplerAnnotation->setContents(okularAnnotation->contents());
    popplerAnnotation->setUniqueName(okularAnnotation->uniqueName());

    // Note: flags and boundary must be set first in order to correctly handle
    // FixedRotation annotations.
    popplerAnnotation->setFlags(static_cast<Poppler::Annotation::Flags>(maskExportedFlags(okularAnnotation->flags())));
    popplerAnnotation->setBoundary(normRectToRectF(okularAnnotation->boundingRectangle()));

    popplerAnnotation->setStyle(okularToPoppler(okularAnnotation->style()));
    popplerAnnotation->setPopup(okularToPoppler(okularAnnotation->window()));

    popplerAnnotation->setCreationDate(okularAnnotation->creationDate());
    popplerAnnotation->setModificationDate(okularAnnotation->modificationDate());
}

static bool setPopplerStampAnnotationCustomImage(const Poppler::Page *page, Poppler::StampAnnotation *pStampAnnotation, const Okular::StampAnnotation *oStampAnnotation)
{
    const QString imagePath = oStampAnnotation->stampImagePath();
    if (imagePath.isEmpty()) {
        return false;
    }

    const QFileInfo stampFileInfo(imagePath);
    if (!stampFileInfo.exists() || !stampFileInfo.isFile()) {
        return false;
    }
    QSize targetSize;

    // Try to detect the native resolution of the image file.
    QImageReader reader(imagePath);
    if (reader.canRead()) {
        const QByteArray format = reader.format();
        // If it is a raster image (PNG, JPG, etc.), use the native size.
        // We strictly avoid downscaling user-provided signature scans.
        if (format != "svg" && format != "svgz") {
            targetSize = reader.size();
        }
    }

    // Fallback for SVGs, named icons, or failed reads:
    // Calculate a high-DPI size based on the PDF page geometry.
    if (targetSize.isEmpty()) {
        // Get the annotation rectangle in PDF user units (points).
        const QSizeF pageSizePoints = page->pageSizeF();
        const Okular::NormalizedRect &nRect = oStampAnnotation->boundingRectangle();

        const double widthPoints = nRect.width() * pageSizePoints.width();
        const double heightPoints = nRect.height() * pageSizePoints.height();

        // Map from PDF points (72 per inch) to a target high DPI (e.g., 288 = 4x).
        constexpr double kPdfDpi = 72.0;
        constexpr double kStampDpi = 288.0;
        const double scale = kStampDpi / kPdfDpi;

        int pixelWidth = std::max(1, qRound(widthPoints * scale));
        int pixelHeight = std::max(1, qRound(heightPoints * scale));

        // Clamp to a reasonable max to prevent memory exhaustion on huge SVGs
        constexpr int kMaxStampPixels = 4096;
        if (pixelWidth > kMaxStampPixels || pixelHeight > kMaxStampPixels) {
            const double ratio = static_cast<double>(pixelWidth) / pixelHeight;
            if (ratio > 1.0) {
                pixelWidth = kMaxStampPixels;
                pixelHeight = pixelWidth / ratio;
            } else {
                pixelHeight = kMaxStampPixels;
                pixelWidth = pixelHeight * ratio;
            }
        }
        targetSize = QSize(pixelWidth, pixelHeight);
    }

    // Load with our calculated size
    QImage image = Okular::AnnotationUtils::loadStamp(imagePath, targetSize).toImage();

    if (!image.isNull()) {
        pStampAnnotation->setStampAppearanceImage(image);
        return true;
    }
    return false;
}

static QRectF freeTextLayoutBoundaryFromOkularAnnotation(const Okular::TextAnnotation *oTextAnnotation)
{
    // For callouts, Okular's TextAnnotation bounding rectangle represents the
    // text box. Poppler keeps that box in /RD and may expand /Rect to include
    // callout leader points.
    return normRectToRectF(oTextAnnotation->boundingRectangle());
}

static void updatePopplerFreeTextPropertiesFromOkularAnnotation(const Okular::TextAnnotation *oTextAnnotation, Poppler::TextAnnotation *pTextAnnotation)
{
    pTextAnnotation->setTextIcon(oTextAnnotation->textIcon());
    pTextAnnotation->setTextFontName(normalizedPdfBase14FontName(oTextAnnotation->textFontName()));
    pTextAnnotation->setTextFontPointSize(oTextAnnotation->textFontPointSize());
    pTextAnnotation->setTextColor(oTextAnnotation->textColor());
    pTextAnnotation->setOkularBorderColor(oTextAnnotation->inplaceBorderColor());
    pTextAnnotation->setInplaceAlign(static_cast<Poppler::TextAnnotation::InplaceAlignPosition>(oTextAnnotation->inplaceAlignment()));
    pTextAnnotation->setInplaceIntent(okularToPoppler(oTextAnnotation->inplaceIntent()));
    QList<QPointF> calloutPoints;
    if (oTextAnnotation->inplaceIntent() == Okular::TextAnnotation::Callout) {
        pTextAnnotation->setOkularInplaceBoundary(freeTextLayoutBoundaryFromOkularAnnotation(oTextAnnotation));
        bool hasCalloutPoints = false;
        for (int i = 0; i < 3 && !hasCalloutPoints; ++i) {
            const Okular::NormalizedPoint point = oTextAnnotation->inplaceCallout(i);
            hasCalloutPoints = point.x != 0.0 || point.y != 0.0;
        }
        if (hasCalloutPoints) {
            for (int i = 0; i < 3; ++i) {
                const Okular::NormalizedPoint point = oTextAnnotation->inplaceCallout(i);
                if (std::isfinite(point.x) && std::isfinite(point.y)) {
                    calloutPoints.append(normPointToPointF(point));
                }
            }
        }
    }
    pTextAnnotation->setCalloutPoints(calloutPoints);
#ifdef POPPLER_QT6_HAS_ANNOTATION_CUSTOM_SCALAR_PROPERTIES
    if (oTextAnnotation->isTemplateNote()) {
        pTextAnnotation->setCustomStringProperty(TemplateNoteDataKey, oTextAnnotation->templateNoteData());
    } else {
        pTextAnnotation->removeCustomProperty(TemplateNoteDataKey);
    }
#endif
}

static void updatePopplerAnnotationFromOkularAnnotation(const Okular::LineAnnotation *oLineAnnotation, Poppler::LineAnnotation *pLineAnnotation)
{
    QList<QPointF> points;
    const QList<Okular::NormalizedPoint> annotPoints = oLineAnnotation->linePoints();
    for (const Okular::NormalizedPoint &p : annotPoints) {
        points.append(normPointToPointF(p));
    }
    pLineAnnotation->setLinePoints(points);
    pLineAnnotation->setLineStartStyle(okularToPoppler(oLineAnnotation->lineStartStyle()));
    pLineAnnotation->setLineEndStyle(okularToPoppler(oLineAnnotation->lineEndStyle()));
    pLineAnnotation->setLineClosed(oLineAnnotation->lineClosed());
    pLineAnnotation->setLineInnerColor(oLineAnnotation->lineInnerColor());
    pLineAnnotation->setLineLeadingForwardPoint(oLineAnnotation->lineLeadingForwardPoint());
    pLineAnnotation->setLineLeadingBackPoint(oLineAnnotation->lineLeadingBackwardPoint());
    pLineAnnotation->setLineShowCaption(oLineAnnotation->showCaption());
    pLineAnnotation->setLineIntent(okularToPoppler(oLineAnnotation->lineIntent()));
}

static void updatePopplerAnnotationFromOkularAnnotation(const Okular::GeomAnnotation *oGeomAnnotation, Poppler::GeomAnnotation *pGeomAnnotation)
{
    pGeomAnnotation->setGeomType(okularToPoppler(oGeomAnnotation->geometricalType()));
    pGeomAnnotation->setGeomInnerColor(oGeomAnnotation->geometricalInnerColor());
}

static void updatePopplerAnnotationFromOkularAnnotation(const Okular::HighlightAnnotation *oHighlightAnnotation, Poppler::HighlightAnnotation *pHighlightAnnotation)
{
    pHighlightAnnotation->setHighlightType(okularToPoppler(oHighlightAnnotation->highlightType()));

    const QList<Okular::HighlightAnnotation::Quad> &oQuads = oHighlightAnnotation->highlightQuads();
    QList<Poppler::HighlightAnnotation::Quad> pQuads;
    for (const Okular::HighlightAnnotation::Quad &oQuad : oQuads) {
        Poppler::HighlightAnnotation::Quad pQuad;
        pQuad.points[0] = normPointToPointF(oQuad.point(3));
        pQuad.points[1] = normPointToPointF(oQuad.point(2));
        pQuad.points[2] = normPointToPointF(oQuad.point(1));
        pQuad.points[3] = normPointToPointF(oQuad.point(0));
        pQuad.capStart = oQuad.capStart();
        pQuad.capEnd = oQuad.capEnd();
        pQuad.feather = oQuad.feather();
        pQuads << pQuad;
    }
    pHighlightAnnotation->setHighlightQuads(pQuads);
}

static bool updatePopplerAnnotationFromOkularAnnotation(const Okular::StampAnnotation *oStampAnnotation, Poppler::StampAnnotation *pStampAnnotation, const Poppler::Page *page, const Poppler::AnnotationAppearance *sourceAppearance = nullptr)
{
    pStampAnnotation->setStampIconName(oStampAnnotation->stampIconName());
#ifdef POPPLER_QT6_HAS_ANNOTATION_CUSTOM_SCALAR_PROPERTIES
    pStampAnnotation->removeCustomProperty(TemplateNoteDataKey);
    if (oStampAnnotation->isOkularLatex()) {
        pStampAnnotation->setCustomStringProperty(LatexNoteDataKey, latexNoteDataForStampAnnotation(oStampAnnotation, page));

        const QString pdfAppearanceFile = oStampAnnotation->latexAppearancePdfFileName();
        const QFileInfo pdfAppearanceInfo(pdfAppearanceFile);
        qCDebug(OkularPdfDebug) << "Embedding LaTeX Stamp appearance; path:" << pdfAppearanceFile << "exists:" << pdfAppearanceInfo.exists() << "bytes:" << pdfAppearanceInfo.size() << "layout width:" << oStampAnnotation->latexLayoutWidth()
                                << "contents length:" << oStampAnnotation->contents().size();
        if (!pdfAppearanceFile.isEmpty() && pdfAppearanceInfo.exists()) {
            QRectF expandedBoundary;
            const Poppler::StampAnnotation::CustomPdfAppearanceOptions appearanceOptions = latexStampAppearanceOptions(oStampAnnotation, page, &expandedBoundary);
            if (oStampAnnotation->isLatexCallout() && expandedBoundary.isValid()) {
                pStampAnnotation->setBoundary(expandedBoundary);
            }
            const bool appearanceUpdated = pStampAnnotation->setStampCustomPdf(pdfAppearanceFile, 1, appearanceOptions);
            qCDebug(OkularPdfDebug) << "Embedding LaTeX Stamp appearance result:" << appearanceUpdated << "path:" << pdfAppearanceFile;
            if (!appearanceUpdated) {
                qCWarning(OkularPdfDebug) << "Could not embed LaTeX Stamp appearance" << pdfAppearanceFile;
            }
            return appearanceUpdated;
        }
        if (pdfAppearanceFile.isEmpty()) {
            qCDebug(OkularPdfDebug) << "LaTeX Stamp appearance PDF is not available; no runtime path is set";
        } else {
            qCWarning(OkularPdfDebug) << "LaTeX Stamp appearance PDF is not available; path:" << pdfAppearanceFile;
        }
        if (sourceAppearance && oStampAnnotation->latexNoteType() != Okular::Annotation::LatexNoteCallout) {
            pStampAnnotation->setAnnotationAppearance(*sourceAppearance);
            qCDebug(OkularPdfDebug) << "Preserved LaTeX stamp appearance from current AP; no runtime source PDF";
            return true;
        }
#ifdef POPPLER_QT6_HAS_STAMP_APPEARANCE_FROM_CURRENT_APPEARANCE
        {
            QRectF expandedBoundary;
            const Poppler::StampAnnotation::CustomPdfAppearanceOptions appearanceOptions = latexStampAppearanceOptions(oStampAnnotation, page, &expandedBoundary);
            if (oStampAnnotation->isLatexCallout() && expandedBoundary.isValid()) {
                pStampAnnotation->setBoundary(expandedBoundary);
            }
            if (sourceAppearance) {
                pStampAnnotation->setAnnotationAppearance(*sourceAppearance);
            }
            const bool rebuiltAppearance = pStampAnnotation->setStampCustomPdfFromCurrentAppearance(appearanceOptions);
            qCDebug(OkularPdfDebug) << "Rebuilt LaTeX stamp appearance from current AP:" << rebuiltAppearance;
            return rebuiltAppearance;
        }
#endif
        return false;
    }
    pStampAnnotation->removeCustomProperty(LatexNoteDataKey);
#endif
    return setPopplerStampAnnotationCustomImage(page, pStampAnnotation, oStampAnnotation);
}

static void updatePopplerAnnotationFromOkularAnnotation(const Okular::InkAnnotation *oInkAnnotation, Poppler::InkAnnotation *pInkAnnotation)
{
    QList<QList<QPointF>> paths;
    const QList<QList<Okular::NormalizedPoint>> inkPathsList = oInkAnnotation->inkPaths();
    for (const QList<Okular::NormalizedPoint> &path : inkPathsList) {
        QList<QPointF> points;
        for (const Okular::NormalizedPoint &p : path) {
            points.append(normPointToPointF(p));
        }
        paths.append(points);
    }
    pInkAnnotation->setInkPaths(paths);
}

static void updatePopplerAnnotationFromOkularAnnotation(const Okular::CaretAnnotation *oCaretAnnotation, Poppler::CaretAnnotation *pCaretAnnotation)
{
    pCaretAnnotation->setCaretSymbol(okularToPoppler(oCaretAnnotation->caretSymbol()));
}

static Poppler::Annotation *createPopplerAnnotationFromOkularAnnotation(const Okular::TextAnnotation *oTextAnnotation)
{
    Poppler::TextAnnotation *pTextAnnotation = new Poppler::TextAnnotation(okularToPoppler(oTextAnnotation->textType()));

    setSharedAnnotationPropertiesToPopplerAnnotation(oTextAnnotation, pTextAnnotation);
    updatePopplerFreeTextPropertiesFromOkularAnnotation(oTextAnnotation, pTextAnnotation);

    return pTextAnnotation;
}

static Poppler::Annotation *createPopplerAnnotationFromOkularAnnotation(const Okular::LineAnnotation *oLineAnnotation)
{
    const auto points = oLineAnnotation->linePoints();
    Poppler::LineAnnotation *pLineAnnotation = new Poppler::LineAnnotation(points.size() == 2 ? Poppler::LineAnnotation::StraightLine : Poppler::LineAnnotation::Polyline);

    setSharedAnnotationPropertiesToPopplerAnnotation(oLineAnnotation, pLineAnnotation);
    updatePopplerAnnotationFromOkularAnnotation(oLineAnnotation, pLineAnnotation);

    return pLineAnnotation;
}

static Poppler::Annotation *createPopplerAnnotationFromOkularAnnotation(const Okular::GeomAnnotation *oGeomAnnotation)
{
    Poppler::GeomAnnotation *pGeomAnnotation = new Poppler::GeomAnnotation();

    setSharedAnnotationPropertiesToPopplerAnnotation(oGeomAnnotation, pGeomAnnotation);
    updatePopplerAnnotationFromOkularAnnotation(oGeomAnnotation, pGeomAnnotation);

    return pGeomAnnotation;
}

static Poppler::Annotation *createPopplerAnnotationFromOkularAnnotation(const Okular::HighlightAnnotation *oHighlightAnnotation)
{
    Poppler::HighlightAnnotation *pHighlightAnnotation = new Poppler::HighlightAnnotation();

    setSharedAnnotationPropertiesToPopplerAnnotation(oHighlightAnnotation, pHighlightAnnotation);
    updatePopplerAnnotationFromOkularAnnotation(oHighlightAnnotation, pHighlightAnnotation);

    return pHighlightAnnotation;
}

static Poppler::Annotation *createPopplerAnnotationFromOkularAnnotation(const Okular::StampAnnotation *oStampAnnotation, const Poppler::Page *page)
{
    Poppler::StampAnnotation *pStampAnnotation = new Poppler::StampAnnotation();

    setSharedAnnotationPropertiesToPopplerAnnotation(oStampAnnotation, pStampAnnotation);
    updatePopplerAnnotationFromOkularAnnotation(oStampAnnotation, pStampAnnotation, page);

    return pStampAnnotation;
}

#if HAVE_NEW_SIGNATURE_API
static Okular::SigningResult popplerToOkular(Poppler::SignatureAnnotation::SigningResult pResult)
{
    switch (pResult) {
    case Poppler::SignatureAnnotation::SigningSuccess:
        return Okular::SigningSuccess;
    case Poppler::SignatureAnnotation::FieldAlreadySigned:
        return Okular::FieldAlreadySigned;
    case Poppler::SignatureAnnotation::GenericSigningError:
        return Okular::GenericSigningError;
#if POPPLER_VERSION_MACRO >= QT_VERSION_CHECK(24, 12, 0)
    case Poppler::SignatureAnnotation::InternalError:
        return Okular::InternalSigningError;
    case Poppler::SignatureAnnotation::KeyMissing:
        return Okular::KeyMissing;
    case Poppler::SignatureAnnotation::WriteFailed:
        return Okular::SignatureWriteFailed;
    case Poppler::SignatureAnnotation::UserCancelled:
        return Okular::UserCancelled;
#endif
#if POPPLER_VERSION_MACRO >= QT_VERSION_CHECK(25, 02, 90)
    case Poppler::SignatureAnnotation::BadPassphrase:
        return Okular::BadPassphrase;
#endif
    }
    return Okular::GenericSigningError;
}

static QSize calculateImagePixelSize(int page, const Okular::NormalizedRect &bRect, Poppler::Document *pdfdoc)
{
    // 2 is an experimental decided upon fudge factor to compensate for the fact that pageSize is in points
    // but most of this ends up working in pixels anyway
    double width = pdfdoc->page(page)->pageSizeF().width() * bRect.width() * 2;
    double height = pdfdoc->page(page)->pageSizeF().height() * bRect.height() * 2;

    return QSize(width, height);
}

static bool isValidImageSize(QSize size)
{
    // if the image gets too small, embedding it fails horribly, so better detect up front and ignore it
    return size.width() > 5 && size.height() > 5;
}

static void resizeImage(const SignatureImageHelper *helper, QSize size)
{
    QImageReader reader(helper->imagePath);

    QSize imageSize = reader.size();
    if (!reader.size().isNull()) {
        reader.setScaledSize(imageSize.scaled(size, Qt::KeepAspectRatio));
    }
    auto input = reader.read();
    if (!input.isNull()) {
        auto scaled = imagescaling::scaleAndFitCanvas(input, size);
        scaled.save(helper->imageFile->fileName(), "png");
    }
}

static std::unique_ptr<Poppler::Annotation> createPopplerAnnotationFromOkularAnnotation(Okular::SignatureAnnotation *oSignatureAnnotation, Poppler::Document *pdfdoc, int nativePage)
{
    auto pSignatureAnnotation = std::make_unique<Poppler::SignatureAnnotation>();

    auto helper = std::make_shared<SignatureImageHelper>(std::make_unique<QTemporaryFile>(QDir::tempPath() + QStringLiteral("/mengshee_signature_XXXXXXX.png")), oSignatureAnnotation->imagePath());
    helper->imageFile->setAutoRemove(true);
    if (!helper->imageFile->open()) {
        return {};
    }

    pSignatureAnnotation->setBorderColor(QColor(0, 0, 0));
    pSignatureAnnotation->setFontColor(QColor(0, 0, 0));

    setSharedAnnotationPropertiesToPopplerAnnotation(oSignatureAnnotation, pSignatureAnnotation.get());
    pSignatureAnnotation->setLeftText(oSignatureAnnotation->leftText());
    pSignatureAnnotation->setText(oSignatureAnnotation->text());
    pSignatureAnnotation->setFieldPartialName(oSignatureAnnotation->fieldPartialName());
    pSignatureAnnotation->setFontSize(oSignatureAnnotation->fontSize());
    pSignatureAnnotation->setLeftFontSize(oSignatureAnnotation->leftFontSize());

    if (!oSignatureAnnotation->imagePath().isEmpty()) {
        QSize imageSize = calculateImagePixelSize(nativePage, oSignatureAnnotation->boundingRectangle(), pdfdoc);
        if (isValidImageSize(imageSize)) {
            resizeImage(helper.get(), imageSize);
            pSignatureAnnotation->setImagePath(helper->imageFile->fileName());
        }
        oSignatureAnnotation->setNativeData(helper);
    }

    oSignatureAnnotation->setSignFunction([signatureAnnotation = pSignatureAnnotation.get()](const Okular::NewSignatureData &oData, const QString &fileName) -> std::pair<Okular::SigningResult, QString> {
        Poppler::PDFConverter::NewSignatureData pData;
        PDFGenerator::okularToPoppler(oData, &pData);
#if POPPLER_VERSION_MACRO > QT_VERSION_CHECK(25, 06, 0)
        return std::pair<Okular::SigningResult, QString>(popplerToOkular(signatureAnnotation->sign(fileName, pData)), signatureAnnotation->lastSigningErrorDetails().data.toString());
#else
        return std::pair<Okular::SigningResult, QString> {popplerToOkular(signatureAnnotation->sign(fileName, pData)), QString {}};
#endif
    });

    return pSignatureAnnotation;
}
#endif

static Poppler::Annotation *createPopplerAnnotationFromOkularAnnotation(const Okular::InkAnnotation *oInkAnnotation)
{
    Poppler::InkAnnotation *pInkAnnotation = new Poppler::InkAnnotation();

    setSharedAnnotationPropertiesToPopplerAnnotation(oInkAnnotation, pInkAnnotation);
    updatePopplerAnnotationFromOkularAnnotation(oInkAnnotation, pInkAnnotation);

    return pInkAnnotation;
}

static Poppler::Annotation *createPopplerAnnotationFromOkularAnnotation(const Okular::CaretAnnotation *oCaretAnnotation)
{
    Poppler::CaretAnnotation *pCaretAnnotation = new Poppler::CaretAnnotation();

    setSharedAnnotationPropertiesToPopplerAnnotation(oCaretAnnotation, pCaretAnnotation);
    updatePopplerAnnotationFromOkularAnnotation(oCaretAnnotation, pCaretAnnotation);

    return pCaretAnnotation;
}
void PopplerAnnotationProxy::notifyAddition(Okular::Annotation *okl_ann, int page)
{
    if (!okl_ann || !ppl_doc) {
        qCWarning(OkularPdfDebug) << "Cannot add an annotation without a live PDF document and annotation";
        return;
    }

    QMutexLocker ml(mutex);

    std::unique_ptr<Poppler::Page> ppl_page = ppl_doc->page(nativePageForLogicalPage(page));
    if (!ppl_page) {
        qCWarning(OkularPdfDebug) << "Cannot add an annotation to invalid logical page" << page;
        return;
    }

    // Create poppler annotation
    Poppler::Annotation *ppl_ann = nullptr;
    switch (okl_ann->subType()) {
    case Okular::Annotation::AText:
        ppl_ann = createPopplerAnnotationFromOkularAnnotation(static_cast<Okular::TextAnnotation *>(okl_ann));
        break;
    case Okular::Annotation::ALine:
        ppl_ann = createPopplerAnnotationFromOkularAnnotation(static_cast<Okular::LineAnnotation *>(okl_ann));
        break;
    case Okular::Annotation::AGeom:
        ppl_ann = createPopplerAnnotationFromOkularAnnotation(static_cast<Okular::GeomAnnotation *>(okl_ann));
        break;
    case Okular::Annotation::AHighlight:
        ppl_ann = createPopplerAnnotationFromOkularAnnotation(static_cast<Okular::HighlightAnnotation *>(okl_ann));
        break;
    case Okular::Annotation::AStamp: {
        bool wasDenyWriteEnabled = okl_ann->flags() & Okular::Annotation::DenyWrite;

        if (wasDenyWriteEnabled) {
            okl_ann->setFlags(okl_ann->flags() & ~Okular::Annotation::DenyWrite);
        }

        ppl_ann = createPopplerAnnotationFromOkularAnnotation(static_cast<Okular::StampAnnotation *>(okl_ann), ppl_page.get());
        if (deletedStampsAnnotationAppearance.find(static_cast<Okular::StampAnnotation *>(okl_ann)) != deletedStampsAnnotationAppearance.end()) {
            ppl_ann->setAnnotationAppearance(*deletedStampsAnnotationAppearance[static_cast<Okular::StampAnnotation *>(okl_ann)].get());
            deletedStampsAnnotationAppearance.erase(static_cast<Okular::StampAnnotation *>(okl_ann));

            if (wasDenyWriteEnabled) {
                okl_ann->setFlags(okl_ann->flags() | Okular::Annotation::DenyWrite);
            }
        }
    } break;
    case Okular::Annotation::AInk:
        ppl_ann = createPopplerAnnotationFromOkularAnnotation(static_cast<Okular::InkAnnotation *>(okl_ann));
        break;
    case Okular::Annotation::ACaret:
        ppl_ann = createPopplerAnnotationFromOkularAnnotation(static_cast<Okular::CaretAnnotation *>(okl_ann));
        break;
#if HAVE_NEW_SIGNATURE_API
    case Okular::Annotation::AWidget: {
        if (auto signatureAnnt = dynamic_cast<Okular::SignatureAnnotation *>(okl_ann)) {
            signatureAnnt->setPage(page);
            ppl_ann = createPopplerAnnotationFromOkularAnnotation(signatureAnnt, ppl_doc, nativePageForLogicalPage(page)).release();
        } else {
            qCWarning(OkularPdfDebug) << "Unsupported annotation type" << okl_ann->subType();
        }

        break;
    }
#endif

    default:
        qCWarning(OkularPdfDebug) << "Unsupported annotation type" << okl_ann->subType();
        return;
    }

    const auto *stampAppearanceTransfer = okl_ann->subType() == Okular::Annotation::AStamp && okl_ann->nativeData() ? static_cast<const StampAppearanceTransfer *>(okl_ann->nativeData()) : nullptr;

    okl_ann->setFlags(okl_ann->flags() | Okular::Annotation::ExternallyDrawn);

    // Bind poppler object to page
    ppl_page->addAnnotation(ppl_ann);

    if (okl_ann->subType() == Okular::Annotation::AStamp && ppl_ann->subType() == Poppler::Annotation::AStamp) {
        const Okular::StampAnnotation *okl_stampann = static_cast<const Okular::StampAnnotation *>(okl_ann);
        if (okl_stampann->isOkularLatex()) {
            updatePopplerAnnotationFromOkularAnnotation(okl_stampann, static_cast<Poppler::StampAnnotation *>(ppl_ann), ppl_page.get());
        }

        // Apply a clipboard appearance only after the native annotation has
        // been added and all type-specific updates have run. Both operations
        // can otherwise replace the AP with the named fallback stamp (Draft).
        if (stampAppearanceTransfer && stampAppearanceTransfer->document == ppl_doc && stampAppearanceTransfer->appearance) {
            ppl_ann->setAnnotationAppearance(*stampAppearanceTransfer->appearance);
        }
        okl_ann->setNativeData(nullptr);
    }

    // Set pointer to poppler annotation as native Id
    okl_ann->setNativeId(QVariant::fromValue(ppl_ann));
    okl_ann->setDisposeDataFunction(disposeAnnotation);

    qCDebug(OkularPdfDebug) << okl_ann->uniqueName();
}

std::shared_ptr<void> PopplerAnnotationProxy::annotationAppearance(const Okular::Annotation *annotation) const
{
    if (!annotation || annotation->subType() != Okular::Annotation::AStamp || !ppl_doc) {
        return {};
    }

    QMutexLocker locker(mutex);
    auto *popplerAnnotation = qvariant_cast<Poppler::Annotation *>(annotation->nativeId());
    if (!popplerAnnotation || popplerAnnotation->subType() != Poppler::Annotation::AStamp) {
        return {};
    }

    auto transfer = std::make_shared<StampAppearanceTransfer>();
    transfer->document = ppl_doc;
    transfer->appearance = popplerAnnotation->annotationAppearance();
    return transfer->appearance ? transfer : std::shared_ptr<void>();
}

void PopplerAnnotationProxy::notifyModification(const Okular::Annotation *okl_ann, int page, bool appearanceChanged)
{
    Q_UNUSED(page);
    Q_UNUSED(appearanceChanged);

    if (!okl_ann || !ppl_doc) {
        qCWarning(OkularPdfDebug) << "Cannot modify an annotation without a live PDF document and annotation";
        return;
    }

    Poppler::Annotation *ppl_ann = qvariant_cast<Poppler::Annotation *>(okl_ann->nativeId());

    if (!ppl_ann) { // Ignore non-native annotations
        return;
    }

    QMutexLocker ml(mutex);

    if (okl_ann->flags() & (Okular::Annotation::BeingMoved | Okular::Annotation::BeingResized)) {
        // Okular ui already renders the annotation on its own
        ppl_ann->setFlags(Poppler::Annotation::Hidden);
        return;
    }

    std::unique_ptr<Poppler::AnnotationAppearance> preservedAppearance;
    QRectF preservedBoundary;
    if (ppl_ann->subType() == Poppler::Annotation::AStamp) {
        preservedAppearance = ppl_ann->annotationAppearance();
        preservedBoundary = ppl_ann->boundary();
    }

    // Set basic properties
    // Note: flags and boundary must be set first in order to correctly handle
    // FixedRotation annotations.
    ppl_ann->setFlags(static_cast<Poppler::Annotation::Flags>(maskExportedFlags(okl_ann->flags())));
    ppl_ann->setBoundary(normRectToRectF(okl_ann->boundingRectangle()));

    ppl_ann->setAuthor(okl_ann->author());
    ppl_ann->setContents(okl_ann->contents());

    ppl_ann->setStyle(okularToPoppler(okl_ann->style()));

    // Set type-specific properties (if any)
    switch (ppl_ann->subType()) {
    case Poppler::Annotation::AText: {
        const Okular::TextAnnotation *okl_txtann = static_cast<const Okular::TextAnnotation *>(okl_ann);
        Poppler::TextAnnotation *ppl_txtann = static_cast<Poppler::TextAnnotation *>(ppl_ann);
        updatePopplerFreeTextPropertiesFromOkularAnnotation(okl_txtann, ppl_txtann);
        break;
    }
    case Poppler::Annotation::ALine: {
        const Okular::LineAnnotation *okl_lineann = static_cast<const Okular::LineAnnotation *>(okl_ann);
        Poppler::LineAnnotation *ppl_lineann = static_cast<Poppler::LineAnnotation *>(ppl_ann);
        updatePopplerAnnotationFromOkularAnnotation(okl_lineann, ppl_lineann);
        break;
    }
    case Poppler::Annotation::AGeom: {
        const Okular::GeomAnnotation *okl_geomann = static_cast<const Okular::GeomAnnotation *>(okl_ann);
        Poppler::GeomAnnotation *ppl_geomann = static_cast<Poppler::GeomAnnotation *>(ppl_ann);
        updatePopplerAnnotationFromOkularAnnotation(okl_geomann, ppl_geomann);
        break;
    }
    case Poppler::Annotation::AHighlight: {
        const Okular::HighlightAnnotation *okl_hlann = static_cast<const Okular::HighlightAnnotation *>(okl_ann);
        Poppler::HighlightAnnotation *ppl_hlann = static_cast<Poppler::HighlightAnnotation *>(ppl_ann);
        updatePopplerAnnotationFromOkularAnnotation(okl_hlann, ppl_hlann);
        break;
    }
    case Poppler::Annotation::AStamp: {
        const Okular::StampAnnotation *okl_stampann = static_cast<const Okular::StampAnnotation *>(okl_ann);
        Poppler::StampAnnotation *ppl_stampann = static_cast<Poppler::StampAnnotation *>(ppl_ann);
        std::unique_ptr<Poppler::Page> ppl_page = ppl_doc->page(nativePageForLogicalPage(page));
        if (!ppl_page) {
            qCWarning(OkularPdfDebug) << "Cannot update a stamp annotation on invalid logical page" << page;
            break;
        }
        const bool appearanceUpdated = updatePopplerAnnotationFromOkularAnnotation(okl_stampann, ppl_stampann, ppl_page.get(), preservedAppearance.get());
        if (!appearanceUpdated && preservedAppearance) {
            if (okl_stampann->isOkularLatex() && okl_stampann->isLatexCallout() && preservedBoundary.isValid()) {
                ppl_stampann->setBoundary(preservedBoundary);
            }
            ppl_stampann->setAnnotationAppearance(*preservedAppearance);
        }
        break;
    }
    case Poppler::Annotation::AInk: {
        const Okular::InkAnnotation *okl_inkann = static_cast<const Okular::InkAnnotation *>(okl_ann);
        Poppler::InkAnnotation *ppl_inkann = static_cast<Poppler::InkAnnotation *>(ppl_ann);
        updatePopplerAnnotationFromOkularAnnotation(okl_inkann, ppl_inkann);
        break;
    }
    case Poppler::Annotation::AWidget:
#if HAVE_NEW_SIGNATURE_API
    {
        if (auto signature = dynamic_cast<const Okular::SignatureAnnotation *>(okl_ann)) {
            auto helper = static_cast<const SignatureImageHelper *>(signature->nativeData());

            if (helper) {
                auto popplerSigAnnot = static_cast<Poppler::SignatureAnnotation *>(ppl_ann);
                QSize imageSize = calculateImagePixelSize(nativePageForLogicalPage(signature->page()), signature->boundingRectangle(), ppl_doc);
                if (isValidImageSize(imageSize)) {
                    resizeImage(helper, imageSize);
                    popplerSigAnnot->setImagePath(helper->imageFile->fileName());
                } else {
                    popplerSigAnnot->setImagePath({});
                }
            }

            break;
        }
    }
#endif
        [[fallthrough]];
    default:
        qCDebug(OkularPdfDebug) << "Type-specific property modification is not implemented for this annotation type";
        break;
    }

    qCDebug(OkularPdfDebug) << okl_ann->uniqueName();
}

void PopplerAnnotationProxy::notifyRemoval(Okular::Annotation *okl_ann, int page)
{
    if (!okl_ann || !ppl_doc) {
        qCWarning(OkularPdfDebug) << "Cannot remove an annotation without a live PDF document and annotation";
        return;
    }

    Poppler::Annotation *ppl_ann = qvariant_cast<Poppler::Annotation *>(okl_ann->nativeId());

    if (!ppl_ann) { // Ignore non-native annotations
        return;
    }

    QMutexLocker ml(mutex);

    std::unique_ptr<Poppler::Page> ppl_page = ppl_doc->page(nativePageForLogicalPage(page));
    if (!ppl_page) {
        qCWarning(OkularPdfDebug) << "Cannot remove an annotation from invalid logical page" << page;
        return;
    }
    annotationsOnOpenHash->remove(okl_ann);
    if (okl_ann->subType() == Okular::Annotation::AStamp) {
        deletedStampsAnnotationAppearance[static_cast<Okular::StampAnnotation *>(okl_ann)] = ppl_ann->annotationAppearance();
    }
    ppl_page->removeAnnotation(ppl_ann); // Also destroys ppl_ann

    okl_ann->setNativeId(QVariant::fromValue(0)); // So that we don't double-free in disposeAnnotation

    qCDebug(OkularPdfDebug) << okl_ann->uniqueName();
}
// END PopplerAnnotationProxy implementation

static Okular::Annotation::LineStyle popplerToOkular(Poppler::Annotation::LineStyle s)
{
    switch (s) {
    case Poppler::Annotation::Solid:
        return Okular::Annotation::Solid;
    case Poppler::Annotation::Dashed:
        return Okular::Annotation::Dashed;
    case Poppler::Annotation::Beveled:
        return Okular::Annotation::Beveled;
    case Poppler::Annotation::Inset:
        return Okular::Annotation::Inset;
    case Poppler::Annotation::Underline:
        return Okular::Annotation::Underline;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << s;
    }

    return Okular::Annotation::Solid;
}

static Okular::Annotation::LineEffect popplerToOkular(Poppler::Annotation::LineEffect e)
{
    switch (e) {
    case Poppler::Annotation::NoEffect:
        return Okular::Annotation::NoEffect;
    case Poppler::Annotation::Cloudy:
        return Okular::Annotation::Cloudy;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << e;
    }

    return Okular::Annotation::NoEffect;
}

static Okular::Annotation::RevisionScope popplerToOkular(Poppler::Annotation::RevScope s)
{
    switch (s) {
    case Poppler::Annotation::Root:
        Q_UNREACHABLE();
    case Poppler::Annotation::Reply:
        return Okular::Annotation::Reply;
    case Poppler::Annotation::Group:
        return Okular::Annotation::Group;
    case Poppler::Annotation::Delete:
        return Okular::Annotation::Delete;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << s;
    }

    return Okular::Annotation::Reply;
}

static Okular::Annotation::RevisionType popplerToOkular(Poppler::Annotation::RevType t)
{
    switch (t) {
    case Poppler::Annotation::None:
        return Okular::Annotation::None;
    case Poppler::Annotation::Marked:
        return Okular::Annotation::Marked;
    case Poppler::Annotation::Unmarked:
        return Okular::Annotation::Unmarked;
    case Poppler::Annotation::Accepted:
        return Okular::Annotation::Accepted;
    case Poppler::Annotation::Rejected:
        return Okular::Annotation::Rejected;
    case Poppler::Annotation::Cancelled:
        return Okular::Annotation::Cancelled;
    case Poppler::Annotation::Completed:
        return Okular::Annotation::Completed;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << t;
    }

    return Okular::Annotation::None;
}

static Okular::TextAnnotation::TextType popplerToOkular(Poppler::TextAnnotation::TextType ptt)
{
    switch (ptt) {
    case Poppler::TextAnnotation::Linked:
        return Okular::TextAnnotation::Linked;
    case Poppler::TextAnnotation::InPlace:
        return Okular::TextAnnotation::InPlace;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << ptt;
    }

    return Okular::TextAnnotation::Linked;
}

static Okular::TextAnnotation::InplaceIntent popplerToOkular(Poppler::TextAnnotation::InplaceIntent pii)
{
    switch (pii) {
    case Poppler::TextAnnotation::Unknown:
        return Okular::TextAnnotation::Unknown;
    case Poppler::TextAnnotation::Callout:
        return Okular::TextAnnotation::Callout;
    case Poppler::TextAnnotation::TypeWriter:
        return Okular::TextAnnotation::TypeWriter;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << pii;
    }

    return Okular::TextAnnotation::Unknown;
}

static Okular::LineAnnotation::TermStyle popplerToOkular(Poppler::LineAnnotation::TermStyle pts)
{
    switch (pts) {
    case Poppler::LineAnnotation::Square:
        return Okular::LineAnnotation::Square;
    case Poppler::LineAnnotation::Circle:
        return Okular::LineAnnotation::Circle;
    case Poppler::LineAnnotation::Diamond:
        return Okular::LineAnnotation::Diamond;
    case Poppler::LineAnnotation::OpenArrow:
        return Okular::LineAnnotation::OpenArrow;
    case Poppler::LineAnnotation::ClosedArrow:
        return Okular::LineAnnotation::ClosedArrow;
    case Poppler::LineAnnotation::None:
        return Okular::LineAnnotation::None;
    case Poppler::LineAnnotation::Butt:
        return Okular::LineAnnotation::Butt;
    case Poppler::LineAnnotation::ROpenArrow:
        return Okular::LineAnnotation::ROpenArrow;
    case Poppler::LineAnnotation::RClosedArrow:
        return Okular::LineAnnotation::RClosedArrow;
    case Poppler::LineAnnotation::Slash:
        return Okular::LineAnnotation::Slash;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << pts;
    }

    return Okular::LineAnnotation::None;
}

static Okular::LineAnnotation::LineIntent popplerToOkular(Poppler::LineAnnotation::LineIntent pli)
{
    switch (pli) {
    case Poppler::LineAnnotation::Unknown:
        return Okular::LineAnnotation::Unknown;
    case Poppler::LineAnnotation::Arrow:
        return Okular::LineAnnotation::Arrow;
    case Poppler::LineAnnotation::Dimension:
        return Okular::LineAnnotation::Dimension;
    case Poppler::LineAnnotation::PolygonCloud:
        return Okular::LineAnnotation::PolygonCloud;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << pli;
    }

    return Okular::LineAnnotation::Unknown;
}

static Okular::GeomAnnotation::GeomType popplerToOkular(Poppler::GeomAnnotation::GeomType pgt)
{
    switch (pgt) {
    case Poppler::GeomAnnotation::InscribedSquare:
        return Okular::GeomAnnotation::InscribedSquare;
    case Poppler::GeomAnnotation::InscribedCircle:
        return Okular::GeomAnnotation::InscribedCircle;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << pgt;
    }

    return Okular::GeomAnnotation::InscribedSquare;
}

static Okular::HighlightAnnotation::HighlightType popplerToOkular(Poppler::HighlightAnnotation::HighlightType pht)
{
    switch (pht) {
    case Poppler::HighlightAnnotation::Highlight:
        return Okular::HighlightAnnotation::Highlight;
    case Poppler::HighlightAnnotation::Squiggly:
        return Okular::HighlightAnnotation::Squiggly;
    case Poppler::HighlightAnnotation::Underline:
        return Okular::HighlightAnnotation::Underline;
    case Poppler::HighlightAnnotation::StrikeOut:
        return Okular::HighlightAnnotation::StrikeOut;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << pht;
    }

    return Okular::HighlightAnnotation::Highlight;
}

static Okular::CaretAnnotation::CaretSymbol popplerToOkular(Poppler::CaretAnnotation::CaretSymbol pcs)
{
    switch (pcs) {
    case Poppler::CaretAnnotation::None:
        return Okular::CaretAnnotation::None;
    case Poppler::CaretAnnotation::P:
        return Okular::CaretAnnotation::P;
    default:
        qCWarning(OkularPdfDebug) << Q_FUNC_INFO << "unknown value" << pcs;
    }

    return Okular::CaretAnnotation::None;
}

static Okular::Annotation *createAnnotationFromPopplerAnnotation(Poppler::TextAnnotation *popplerAnnotation)
{
    Okular::TextAnnotation *oTextAnn = new Okular::TextAnnotation();

    oTextAnn->setTextType(popplerToOkular(popplerAnnotation->textType()));
    oTextAnn->setTextIcon(popplerAnnotation->textIcon());
    const QString textFontName = popplerAnnotation->textFontName();
    if (!textFontName.isEmpty()) {
        oTextAnn->setTextFontName(normalizedPdfBase14FontName(textFontName));
    }
    oTextAnn->setTextFontPointSize(popplerAnnotation->textFontPointSize());
    oTextAnn->setTextColor(popplerAnnotation->textColor());
    oTextAnn->setInplaceBorderColor(popplerAnnotation->okularBorderColor());
    // this works because we use the same 0:left, 1:center, 2:right meaning both in poppler and okular
    oTextAnn->setInplaceAlignment(popplerAnnotation->inplaceAlign());
    oTextAnn->setInplaceIntent(popplerToOkular(popplerAnnotation->inplaceIntent()));
    for (int i = 0; i < 3; ++i) {
        const QPointF p = popplerAnnotation->calloutPoint(i);
        oTextAnn->setInplaceCallout({p.x(), p.y()}, i);
    }
#ifdef POPPLER_QT6_HAS_ANNOTATION_CUSTOM_SCALAR_PROPERTIES
    if (oTextAnn->textType() == Okular::TextAnnotation::InPlace) {
        oTextAnn->setTemplateNoteData(popplerAnnotation->customStringProperty(TemplateNoteDataKey));
    }
#endif

    return oTextAnn;
}

static QRectF okularBoundaryForPopplerAnnotation(const Poppler::Annotation *popplerAnnotation)
{
    if (popplerAnnotation->subType() == Poppler::Annotation::AText) {
        const auto *textAnnotation = static_cast<const Poppler::TextAnnotation *>(popplerAnnotation);
        if (textAnnotation->textType() == Poppler::TextAnnotation::InPlace && textAnnotation->inplaceIntent() == Poppler::TextAnnotation::Callout) {
            return textAnnotation->okularInplaceBoundary();
        }
    }

    return popplerAnnotation->boundary();
}

static Okular::Annotation *createAnnotationFromPopplerAnnotation(const Poppler::LineAnnotation *popplerAnnotation)
{
    Okular::LineAnnotation *oLineAnn = new Okular::LineAnnotation();

    oLineAnn->setLineStartStyle(popplerToOkular(popplerAnnotation->lineStartStyle()));
    oLineAnn->setLineEndStyle(popplerToOkular(popplerAnnotation->lineEndStyle()));
    oLineAnn->setLineClosed(popplerAnnotation->isLineClosed());
    oLineAnn->setLineInnerColor(popplerAnnotation->lineInnerColor());
    oLineAnn->setLineLeadingForwardPoint(popplerAnnotation->lineLeadingForwardPoint());
    oLineAnn->setLineLeadingBackwardPoint(popplerAnnotation->lineLeadingBackPoint());
    oLineAnn->setShowCaption(popplerAnnotation->lineShowCaption());
    oLineAnn->setLineIntent(popplerToOkular(popplerAnnotation->lineIntent()));

    QList<Okular::NormalizedPoint> points;
    const QList<QPointF> popplerPoints = popplerAnnotation->linePoints();
    for (const QPointF &p : popplerPoints) {
        points << Okular::NormalizedPoint(p.x(), p.y());
    }
    oLineAnn->setLinePoints(points);

    return oLineAnn;
}

static Okular::Annotation *createAnnotationFromPopplerAnnotation(const Poppler::GeomAnnotation *popplerAnnotation)
{
    Okular::GeomAnnotation *oGeomAnn = new Okular::GeomAnnotation();

    oGeomAnn->setGeometricalType(popplerToOkular(popplerAnnotation->geomType()));
    oGeomAnn->setGeometricalInnerColor(popplerAnnotation->geomInnerColor());

    return oGeomAnn;
}

static Okular::Annotation *createAnnotationFromPopplerAnnotation(const Poppler::HighlightAnnotation *popplerAnnotation)
{
    Okular::HighlightAnnotation *oHighlightAnn = new Okular::HighlightAnnotation();

    oHighlightAnn->setHighlightType(popplerToOkular(popplerAnnotation->highlightType()));

    const QList<Poppler::HighlightAnnotation::Quad> popplerHq = popplerAnnotation->highlightQuads();
    QList<Okular::HighlightAnnotation::Quad> &okularHq = oHighlightAnn->highlightQuads();

    for (const Poppler::HighlightAnnotation::Quad &popplerQ : popplerHq) {
        Okular::HighlightAnnotation::Quad q;

        // Poppler stores highlight points in swapped order
        q.setPoint(Okular::NormalizedPoint(popplerQ.points[0].x(), popplerQ.points[0].y()), 3);
        q.setPoint(Okular::NormalizedPoint(popplerQ.points[1].x(), popplerQ.points[1].y()), 2);
        q.setPoint(Okular::NormalizedPoint(popplerQ.points[2].x(), popplerQ.points[2].y()), 1);
        q.setPoint(Okular::NormalizedPoint(popplerQ.points[3].x(), popplerQ.points[3].y()), 0);

        q.setCapStart(popplerQ.capStart);
        q.setCapEnd(popplerQ.capEnd);
        q.setFeather(popplerQ.feather);
        okularHq << q;
    }

    return oHighlightAnn;
}

static Okular::Annotation *createAnnotationFromPopplerAnnotation(const Poppler::InkAnnotation *popplerAnnotation)
{
    Okular::InkAnnotation *oInkAnn = new Okular::InkAnnotation();

    const QList<QList<QPointF>> popplerInkPaths = popplerAnnotation->inkPaths();
    QList<QList<Okular::NormalizedPoint>> okularInkPaths;
    for (const QList<QPointF> &popplerInkPath : popplerInkPaths) {
        QList<Okular::NormalizedPoint> okularInkPath;
        for (const QPointF &popplerPoint : popplerInkPath) {
            okularInkPath << Okular::NormalizedPoint(popplerPoint.x(), popplerPoint.y());
        }
        okularInkPaths << okularInkPath;
    }

    oInkAnn->setInkPaths(okularInkPaths);

    return oInkAnn;
}

static Okular::Annotation *createAnnotationFromPopplerAnnotation(const Poppler::CaretAnnotation *popplerAnnotation)
{
    Okular::CaretAnnotation *oCaretAnn = new Okular::CaretAnnotation();

    oCaretAnn->setCaretSymbol(popplerToOkular(popplerAnnotation->caretSymbol()));

    return oCaretAnn;
}

static Okular::Annotation *createAnnotationFromPopplerAnnotation(const Poppler::StampAnnotation *popplerAnnotation)
{
    Okular::StampAnnotation *oStampAnn = new Okular::StampAnnotation();

    oStampAnn->setStampIconName(popplerAnnotation->stampIconName());
#ifdef POPPLER_QT6_HAS_ANNOTATION_CUSTOM_SCALAR_PROPERTIES
    const ParsedLatexNoteData latexNoteData = parseLatexNoteData(popplerAnnotation->customStringProperty(LatexNoteDataKey));
    oStampAnn->setOkularLatex(latexNoteData.valid);
    if (latexNoteData.valid) {
        oStampAnn->setStampIconName(QStringLiteral("latex-notes"));
        oStampAnn->setLatexNoteType(latexNoteData.type == QLatin1String("callout") ? Okular::Annotation::LatexNoteCallout
                                                                                   : (latexNoteData.type == QLatin1String("boxed") ? Okular::Annotation::LatexNoteBoxed : Okular::Annotation::LatexNotePlain));
        oStampAnn->setLatexPadding(latexNoteData.paddingPoints);
        oStampAnn->setLatexFontSize(latexNoteData.fontSizePoints);
        oStampAnn->setLatexLayoutWidth(latexNoteData.layoutWidthPoints);
        oStampAnn->setLatexTextColor(latexNoteData.textColor);
        oStampAnn->setLatexFillColor(latexNoteData.fillColor);
        oStampAnn->setLatexBorderColor(latexNoteData.borderColor);
        oStampAnn->style().setWidth(latexNoteData.type == QLatin1String("plain") ? 0.0 : latexNoteData.borderWidthPoints);
        if (latexNoteData.type == QLatin1String("callout") && latexNoteData.hasNormalizedCalloutPoints) {
            for (int i = 0; i < 3; ++i) {
                oStampAnn->setLatexCalloutPoint(latexNoteData.normalizedCalloutPoints[i], i);
            }
        }
    }
#endif

    return oStampAnn;
}

Okular::Annotation *createAnnotationFromPopplerAnnotation(Poppler::Annotation *popplerAnnotation, const Poppler::Page &popplerPage, bool *doDelete)
{
    Okular::Annotation *okularAnnotation = nullptr;
    *doDelete = true;
    bool tieToOkularAnn = false;
    bool externallyDrawn = false;
    switch (popplerAnnotation->subType()) {
    case Poppler::Annotation::AFileAttachment: {
        Poppler::FileAttachmentAnnotation *attachann = static_cast<Poppler::FileAttachmentAnnotation *>(popplerAnnotation);
        Okular::FileAttachmentAnnotation *f = new Okular::FileAttachmentAnnotation();
        okularAnnotation = f;
        tieToOkularAnn = true;
        *doDelete = false;

        f->setFileIconName(attachann->fileIconName());
        f->setEmbeddedFile(new PDFEmbeddedFile(attachann->embeddedFile()));

        break;
    }
    case Poppler::Annotation::ASound: {
        Poppler::SoundAnnotation *soundann = static_cast<Poppler::SoundAnnotation *>(popplerAnnotation);
        Okular::SoundAnnotation *s = new Okular::SoundAnnotation();
        okularAnnotation = s;

        s->setSoundIconName(soundann->soundIconName());
        s->setSound(createSoundFromPopplerSound(soundann->sound()));

        break;
    }
    case Poppler::Annotation::AMovie: {
        Poppler::MovieAnnotation *movieann = static_cast<Poppler::MovieAnnotation *>(popplerAnnotation);
        Okular::MovieAnnotation *m = new Okular::MovieAnnotation();
        okularAnnotation = m;
        tieToOkularAnn = true;
        *doDelete = false;

        m->setMovie(createMovieFromPopplerMovie(movieann->movie()));

        break;
    }
    case Poppler::Annotation::AWidget: {
        okularAnnotation = new Okular::WidgetAnnotation();
        break;
    }
    case Poppler::Annotation::AScreen: {
        Okular::ScreenAnnotation *m = new Okular::ScreenAnnotation();
        okularAnnotation = m;
        tieToOkularAnn = true;
        *doDelete = false;
        break;
    }
    case Poppler::Annotation::ARichMedia: {
        const Poppler::RichMediaAnnotation *richmediaann = static_cast<Poppler::RichMediaAnnotation *>(popplerAnnotation);
        const QPair<Okular::Movie *, Okular::EmbeddedFile *> result = createMovieFromPopplerRichMedia(richmediaann);

        if (result.first) {
            Okular::RichMediaAnnotation *r = new Okular::RichMediaAnnotation();
            tieToOkularAnn = true;
            *doDelete = false;
            okularAnnotation = r;

            r->setMovie(result.first);
            r->setEmbeddedFile(result.second);
        }

        break;
    }
    case Poppler::Annotation::AText: {
        externallyDrawn = true;
        tieToOkularAnn = true;
        *doDelete = false;
        okularAnnotation = createAnnotationFromPopplerAnnotation(static_cast<Poppler::TextAnnotation *>(popplerAnnotation));
        break;
    }
    case Poppler::Annotation::ALine: {
        externallyDrawn = true;
        tieToOkularAnn = true;
        *doDelete = false;
        okularAnnotation = createAnnotationFromPopplerAnnotation(static_cast<Poppler::LineAnnotation *>(popplerAnnotation));
        break;
    }
    case Poppler::Annotation::AGeom: {
        externallyDrawn = true;
        tieToOkularAnn = true;
        *doDelete = false;
        okularAnnotation = createAnnotationFromPopplerAnnotation(static_cast<Poppler::GeomAnnotation *>(popplerAnnotation));
        break;
    }
    case Poppler::Annotation::AHighlight: {
        externallyDrawn = true;
        tieToOkularAnn = true;
        *doDelete = false;
        okularAnnotation = createAnnotationFromPopplerAnnotation(static_cast<Poppler::HighlightAnnotation *>(popplerAnnotation));
        break;
    }
    case Poppler::Annotation::AInk: {
        externallyDrawn = true;
        tieToOkularAnn = true;
        *doDelete = false;
        okularAnnotation = createAnnotationFromPopplerAnnotation(static_cast<Poppler::InkAnnotation *>(popplerAnnotation));
        break;
    }
    case Poppler::Annotation::ACaret: {
        externallyDrawn = true;
        tieToOkularAnn = true;
        *doDelete = false;
        okularAnnotation = createAnnotationFromPopplerAnnotation(static_cast<Poppler::CaretAnnotation *>(popplerAnnotation));
        break;
    }
    case Poppler::Annotation::AStamp:
        externallyDrawn = true;
        tieToOkularAnn = true;
        *doDelete = false;
        okularAnnotation = createAnnotationFromPopplerAnnotation(static_cast<Poppler::StampAnnotation *>(popplerAnnotation));
        break;
    default: {
        break;
    }
    }
    if (okularAnnotation) {
        // the Contents field might have lines separated by \r
        QString contents = popplerAnnotation->contents();
        contents.replace(QLatin1Char('\r'), QLatin1Char('\n'));

        okularAnnotation->setAuthor(popplerAnnotation->author());
        okularAnnotation->setContents(contents);
        okularAnnotation->setUniqueName(popplerAnnotation->uniqueName());
        okularAnnotation->setModificationDate(popplerAnnotation->modificationDate());
        okularAnnotation->setCreationDate(popplerAnnotation->creationDate());
        int flags = popplerAnnotation->flags() | Okular::Annotation::External;
        if (okularAnnotation->isOkularLatex()) {
            flags |= Okular::Annotation::FixedRotation;
        }
        okularAnnotation->setFlags(flags);
        okularAnnotation->setBoundingRectangle(Okular::NormalizedRect::fromQRectF(okularBoundaryForPopplerAnnotation(popplerAnnotation)));

        if (externallyDrawn) {
            okularAnnotation->setFlags(okularAnnotation->flags() | Okular::Annotation::ExternallyDrawn);
        }
        if (okularAnnotation->subType() == Okular::Annotation::SubType::AStamp) {
            Okular::StampAnnotation *oStampAnn = static_cast<Okular::StampAnnotation *>(okularAnnotation);
            Poppler::StampAnnotation *pStampAnn = static_cast<Poppler::StampAnnotation *>(popplerAnnotation);
            QFileInfo stampIconFile {oStampAnn->stampImagePath()};
            oStampAnn->setFlags(okularAnnotation->flags() & ~Okular::Annotation::Flag::DenyWrite);
#ifdef POPPLER_QT6_HAS_ANNOTATION_CUSTOM_SCALAR_PROPERTIES
            if (oStampAnn->isOkularLatex() && oStampAnn->isLatexCallout()) {
                const ParsedLatexNoteData latexNoteData = parseLatexNoteData(pStampAnn->customStringProperty(LatexNoteDataKey));
                const QSizeF pageSize = popplerPage.pageSizeF();
                if (latexNoteData.hasBoxRectPoints && pageSize.width() > 0.0 && pageSize.height() > 0.0) {
                    oStampAnn->setBoundingRectangle(pageRectToNormRect(latexNoteData.boxRectPoints, pageSize));
                }
                const bool hasNormalizedCalloutPoints = (oStampAnn->latexCalloutPoint(0).x != 0.0 || oStampAnn->latexCalloutPoint(0).y != 0.0 || oStampAnn->latexCalloutPoint(1).x != 0.0 || oStampAnn->latexCalloutPoint(1).y != 0.0 ||
                                                         oStampAnn->latexCalloutPoint(2).x != 0.0 || oStampAnn->latexCalloutPoint(2).y != 0.0);
                if (!hasNormalizedCalloutPoints && latexNoteData.hasPageCalloutPoints && pageSize.width() > 0.0 && pageSize.height() > 0.0) {
                    for (int i = 0; i < 3; ++i) {
                        oStampAnn->setLatexCalloutPoint(pagePointToNormPoint(latexNoteData.pageCalloutPoints[i], pageSize), i);
                    }
                }
            }
#endif
            if (stampIconFile.exists() && stampIconFile.isFile()) {
                setPopplerStampAnnotationCustomImage(&popplerPage, pStampAnn, oStampAnn);
            }
        }

        // Convert the poppler annotation style to Okular annotation style
        Okular::Annotation::Style &okularStyle = okularAnnotation->style();
        const Poppler::Annotation::Style popplerStyle = popplerAnnotation->style();
        okularStyle.setColor(popplerStyle.color());
        okularStyle.setOpacity(popplerStyle.opacity());
        okularStyle.setWidth(popplerStyle.width());
        okularStyle.setLineStyle(popplerToOkular(popplerStyle.lineStyle()));
        okularStyle.setXCorners(popplerStyle.xCorners());
        okularStyle.setYCorners(popplerStyle.yCorners());
        const QList<double> &dashArray = popplerStyle.dashArray();
        if (dashArray.size() > 0) {
            okularStyle.setMarks(dashArray[0]);
        }
        if (dashArray.size() > 1) {
            okularStyle.setSpaces(dashArray[1]);
        }
        okularStyle.setLineEffect(popplerToOkular(popplerStyle.lineEffect()));
        okularStyle.setEffectIntensity(popplerStyle.effectIntensity());

#ifdef POPPLER_QT6_HAS_ANNOTATION_CUSTOM_SCALAR_PROPERTIES
        if (okularAnnotation->subType() == Okular::Annotation::SubType::AStamp) {
            Okular::StampAnnotation *oStampAnn = static_cast<Okular::StampAnnotation *>(okularAnnotation);
            Poppler::StampAnnotation *pStampAnn = static_cast<Poppler::StampAnnotation *>(popplerAnnotation);
            const ParsedLatexNoteData latexNoteData = parseLatexNoteData(pStampAnn->customStringProperty(LatexNoteDataKey));
            if (latexNoteData.valid) {
                oStampAnn->setLatexNoteType(latexNoteData.type == QLatin1String("callout") ? Okular::Annotation::LatexNoteCallout
                                                                                           : (latexNoteData.type == QLatin1String("boxed") ? Okular::Annotation::LatexNoteBoxed : Okular::Annotation::LatexNotePlain));
                oStampAnn->setLatexFillColor(latexNoteData.fillColor);
                oStampAnn->setLatexBorderColor(latexNoteData.borderColor);
                oStampAnn->setLatexPadding(latexNoteData.paddingPoints);
                oStampAnn->setLatexFontSize(latexNoteData.fontSizePoints);
                oStampAnn->style().setWidth(latexNoteData.type == QLatin1String("plain") ? 0.0 : latexNoteData.borderWidthPoints);
            }
        }
#endif

        // Convert the poppler annotation popup to Okular annotation window
        Okular::Annotation::Window &okularWindow = okularAnnotation->window();
        const Poppler::Annotation::Popup popplerPopup = popplerAnnotation->popup();
        // This assumes that both "flags" int mean the same, but since we don't use the flags in okular anywhere it's not really that important
        okularWindow.setFlags(popplerPopup.flags());
        const QRectF popplerGeometry = popplerPopup.geometry();
        const QSize popplerPageSize = popplerPage.pageSize();
        okularWindow.setTopLeft(Okular::NormalizedPoint(popplerGeometry.top(), popplerGeometry.left(), popplerPageSize.width(), popplerPageSize.height()));
        okularWindow.setWidth(popplerGeometry.width());
        okularWindow.setHeight(popplerGeometry.height());
        okularWindow.setTitle(popplerPopup.title());
        okularWindow.setSummary(popplerPopup.summary());

        // Convert the poppler revisions to Okular revisions
        QList<Okular::Annotation::Revision> &okularRevisions = okularAnnotation->revisions();
        std::vector<std::unique_ptr<Poppler::Annotation>> popplerRevisions = popplerAnnotation->revisions();
        for (auto &popplerRevision : popplerRevisions) {
            bool deletePopplerRevision;
            Okular::Annotation::Revision okularRevision;
            okularRevision.setAnnotation(createAnnotationFromPopplerAnnotation(popplerRevision.get(), popplerPage, &deletePopplerRevision));
            okularRevision.setScope(popplerToOkular(popplerRevision->revisionScope()));
            okularRevision.setType(popplerToOkular(popplerRevision->revisionType()));
            okularRevisions << okularRevision;

            if (!deletePopplerRevision) {
                (void)popplerRevision.release(); //
            }
        }

        if (tieToOkularAnn) {
            okularAnnotation->setNativeId(QVariant::fromValue(popplerAnnotation));
            okularAnnotation->setDisposeDataFunction(disposeAnnotation);
        }
    }
    return okularAnnotation;
}
