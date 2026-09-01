/*
    SPDX-FileCopyrightText: 2006 Chu Xiaodong <xiaodongchu@gmail.com>
    SPDX-FileCopyrightText: 2006 Pino Toscano <pino@kde.org>

    Work sponsored by the LiMux project of the city of Munich:
    SPDX-FileCopyrightText: 2017 Klarälvdalens Datakonsult AB a KDAB Group company <info@kdab.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "annotwindow.h"

// qt/kde includes
#include <KLocalizedString>
#include <KTextEdit>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QBoxLayout>
#include <QCloseEvent>
#include <QDateTime>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QList>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizeGrip>
#include <QStyle>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>

#if HAVE_QSCINTILLA
#if defined(QT_NO_KEYWORDS)
#define signals public
#define slots
#endif
#include <Qsci/qscilexertex.h>
#include <Qsci/qsciscintilla.h>
#include <Scintilla.h>
#if defined(QT_NO_KEYWORDS)
#undef slots
#undef signals
#endif
#endif

// local includes
#include "core/annotations.h"
#include "core/document.h"
#include "core/page.h"
#include "gui/guiutils.h"
#include "latexnoteutils.h"
#include "latexrenderer.h"
#include "settings.h"
#include <KMessageBox>
#include <core/utils.h>

#include <cmath>

namespace
{
constexpr int popupCaretWidth = 3;

QColor popupPaperColor(const QColor &annotationColor)
{
    const QColor baseColor = annotationColor.isValid() ? annotationColor.toHsl() : QColor(Qt::yellow).toHsl();
    const qreal hue = baseColor.hslHueF() >= 0.0 ? baseColor.hslHueF() : 0.0;
    const qreal lightness = qMax(0.90, baseColor.lightnessF());
    return QColor::fromHslF(hue, baseColor.hslSaturationF(), lightness);
}

class PopupTextEdit final : public KTextEdit
{
public:
    explicit PopupTextEdit(QWidget *parent)
        : KTextEdit(parent)
    {
        setCursorWidth(popupCaretWidth);
    }

    void setCaretColor(const QColor &color)
    {
        m_caretColor = color;
        viewport()->update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        KTextEdit::paintEvent(event);
        if (isReadOnly() || !hasFocus()) {
            return;
        }

        QRect caret = cursorRect();
        caret.setWidth(popupCaretWidth);
        QPainter painter(viewport());
        painter.fillRect(caret, m_caretColor);
    }

private:
    QColor m_caretColor = Qt::black;
};

#if HAVE_QSCINTILLA
void keepCaretVisible(QsciScintilla *editor)
{
    // Scintilla documents a zero caret period as "steady on".
    editor->SendScintilla(SCI_SETCARETPERIOD, 0);
}
#endif

bool latexAnnotation(Okular::Annotation *annotation)
{
    return LatexNoteUtils::annotationIsLatex(annotation);
}

#if HAVE_QSCINTILLA
QsciScintilla *createLatexSourceEditor(QWidget *parent, const QString &contents)
{
    auto *editor = new QsciScintilla(parent);
    editor->setUtf8(true);
    editor->setText(contents);
    editor->setWrapMode(QsciScintilla::WrapWord);
    editor->setMarginLineNumbers(0, true);
    editor->setMarginWidth(0, QStringLiteral("000"));
    editor->setBraceMatching(QsciScintilla::SloppyBraceMatch);
    editor->setCaretLineVisible(true);
    editor->setCaretLineBackgroundColor(QColor(245, 248, 255));
    editor->setCaretWidth(popupCaretWidth);
    keepCaretVisible(editor);
    editor->setAutoIndent(true);
    editor->setIndentationsUseTabs(false);
    editor->setIndentationWidth(2);
    editor->setTabWidth(2);
    editor->setFolding(QsciScintilla::BoxedTreeFoldStyle);
    editor->setScrollWidthTracking(true);

    const QFont editorFont(QStringLiteral("Consolas"), 11);
    editor->setFont(editorFont);
    auto *lexer = new QsciLexerTeX(editor);
    lexer->setDefaultFont(editorFont);
    lexer->setColor(QColor(34, 34, 34), QsciLexerTeX::Default);
    lexer->setColor(QColor(0, 92, 175), QsciLexerTeX::Command);
    lexer->setColor(QColor(105, 58, 8), QsciLexerTeX::Special);
    lexer->setColor(QColor(100, 70, 160), QsciLexerTeX::Group);
    lexer->setColor(QColor(20, 120, 70), QsciLexerTeX::Symbol);
    lexer->setColor(QColor(34, 34, 34), QsciLexerTeX::Text);
    editor->setLexer(lexer);
    editor->SendScintilla(SCI_EMPTYUNDOBUFFER);
    editor->setModified(false);
    return editor;
}

int characterPositionForScintillaBytePosition(const QsciScintilla *editor, int bytePosition)
{
    if (!editor || bytePosition <= 0) {
        return 0;
    }
    return QString::fromUtf8(editor->bytes(0, bytePosition)).size();
}

int scintillaBytePositionForCharacterPosition(const QsciScintilla *editor, int characterPosition)
{
    if (!editor || characterPosition <= 0) {
        return 0;
    }
    const QString text = editor->text();
    return text.left(qBound(0, characterPosition, text.size())).toUtf8().size();
}
#endif
}

class CloseButton : public QPushButton
{
    Q_OBJECT

public:
    explicit CloseButton(QWidget *parent = Q_NULLPTR)
        : QPushButton(parent)
    {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        QSize size = QSize(14, 14);
        setFixedSize(size);
        setIcon(style()->standardIcon(QStyle::SP_DockWidgetCloseButton));
        setIconSize(size);
        setToolTip(i18n("Close this note"));
        setCursor(Qt::ArrowCursor);
    }
};

class MovableTitle : public QWidget
{
    Q_OBJECT

public:
    explicit MovableTitle(AnnotWindow *parent, int numberPreviousAnnotation = 0)
        : QWidget(parent)
    {
        countPreviousAnnotation = numberPreviousAnnotation;

        // First zone: titleLabel + buttonClose
        QVBoxLayout *mainlay = new QVBoxLayout(this);
        mainlay->setContentsMargins(0, 0, 0, 0);
        mainlay->setSpacing(0);
        // close button row
        QHBoxLayout *buttonlay = new QHBoxLayout();
        titleLabel = new QLabel(this);
        QFont f = titleLabel->font();
        f.setBold(true);
        titleLabel->setFont(f);
        titleLabel->setCursor(Qt::SizeAllCursor);
        buttonlay->addWidget(titleLabel);
        dateLabel = new QLabel(this);
        buttonlay->addWidget(dateLabel);
        CloseButton *close = new CloseButton(this);
        connect(close, &QAbstractButton::clicked, parent, &QWidget::close);
        buttonlay->addWidget(close);

        QScrollArea *scrollArea = new QScrollArea(this);
        scrollArea->setWidgetResizable(true);
        scrollArea->setFrameStyle(QFrame::NoFrame);
        scrollArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        QWidget *scrollContainer = new QWidget(scrollArea);
        QVBoxLayout *scrollLayout = new QVBoxLayout(scrollContainer);
        scrollLayout->setContentsMargins(0, 0, 0, 0);

        for (qsizetype i = 0; i < numberPreviousAnnotation; i++) {
            // For authorLabel
            QLabel *newAuthorLabel = new QLabel(this);
            newAuthorLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
            listPreviousAuthorAndDateLabel.append(newAuthorLabel);
            scrollLayout->addWidget(newAuthorLabel);

            // For contents
            QLabel *newContentsLabel = new QLabel(this);
            newContentsLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
            listPreviousContentLabel.append(newContentsLabel);
            scrollLayout->addWidget(newContentsLabel);

            // Add a horizontal black line after each comment
            QFrame *line = new QFrame(scrollContainer);
            line->setFrameShape(QFrame::HLine);
            line->setFrameShadow(QFrame::Sunken);
            line->setStyleSheet(QStringLiteral("color: rgba(0,0,0,0.1);"));
            scrollLayout->addWidget(line);
        }

        scrollArea->setWidget(scrollContainer);

        // Third zone: authorAndDateLabel (author name with date)
        authorAndDateLabel = new QLabel(this);
        authorAndDateLabel->setContentsMargins(0, 10, 0, 0);
        authorAndDateLabel->setStyleSheet(QStringLiteral("font-weight: bold"));
        authorAndDateLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        authorAndDateLabel->setCursor(Qt::SizeAllCursor);

        // Last zone: Useful properties for editable text zone
        QFrame *bottomFrame = new QFrame(this);
        QVBoxLayout *bottomLayout = new QVBoxLayout(bottomFrame);
        bottomLayout->setContentsMargins(0, 0, 0, 0);
        bottomLayout->setSpacing(0);

        QHBoxLayout *optionlay = new QHBoxLayout();
        optionlay->addWidget(authorAndDateLabel);
        optionButton = new QToolButton(this);
        QString opttext = i18n("Options");
        optionButton->setText(opttext);
        optionButton->setAutoRaise(true);
        QSize s = QFontMetrics(optionButton->font()).boundingRect(opttext).size() + QSize(8, 8);
        optionButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        optionButton->setFixedSize(s);
        optionlay->addWidget(optionButton);
        // ### disabled for now
        optionButton->hide();
        latexButton = new QToolButton(this);
        QHBoxLayout *latexlay = new QHBoxLayout();
        QString latextext = i18n("This annotation may contain LaTeX code.\nClick here to render.");
        latexButton->setText(latextext);
        latexButton->setAutoRaise(true);
        s = QFontMetrics(latexButton->font()).boundingRect(0, 0, this->width(), this->height(), 0, latextext).size() + QSize(8, 8);
        latexButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        latexButton->setFixedSize(s);
        latexButton->setCheckable(true);
        latexButton->setVisible(false);
        latexlay->addSpacing(1);
        latexlay->addWidget(latexButton);
        latexlay->addSpacing(1);
        connect(latexButton, &QToolButton::clicked, parent, &AnnotWindow::renderLatex);
        connect(parent, &AnnotWindow::containsLatex, latexButton, &QWidget::setVisible);

        bottomLayout->addLayout(optionlay);

        // Adds all widgets and layouts with mainlay
        // First zone
        mainlay->addLayout(buttonlay);
        // Second zone
        if (numberPreviousAnnotation > 0) {
            mainlay->addWidget(scrollArea);
        }
        // Third zone
        mainlay->addWidget(authorAndDateLabel);
        // Last zone
        mainlay->addWidget(bottomFrame);
        mainlay->addLayout(latexlay);

        setScrollArea(scrollArea);

        // Event filters
        titleLabel->installEventFilter(this);
        authorAndDateLabel->installEventFilter(this);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != titleLabel && watched != authorAndDateLabel) {
            return false;
        }

        QMouseEvent *me = nullptr;
        switch (event->type()) {
        case QEvent::MouseButtonPress:
            me = static_cast<QMouseEvent *>(event);
            mousePressPos = me->pos();
            parentWidget()->raise();
            break;
        case QEvent::MouseButtonRelease:
            mousePressPos = QPoint();
            break;
        case QEvent::MouseMove: {
            me = static_cast<QMouseEvent *>(event);
            const QPoint mouseMovePos = me->pos();
            const QPoint mouseDelta = mouseMovePos - mousePressPos;

            const auto annotWidget = parentWidget();
            QPoint newPositionPoint = annotWidget->pos() + mouseDelta;
            annotWidget->move(newPositionPoint);
            break;
        }
        default:
            return false;
        }
        return true;
    }

    int numberPreviousAnnotation()
    {
        return countPreviousAnnotation;
    }

    void setTitle(const QString &title)
    {
        titleLabel->setText(QStringLiteral(" ") + title);
    }

    void setAuthorAndDate(const QString &author, const QDateTime &dt)
    {
        QString auth;
        (author.isNull() || author.isEmpty()) ? auth = i18n("Unknown author") : auth = author;

        QString dateTime;
        dt.isValid() ? dateTime = QLocale().toString(dt.toTimeSpec(Qt::LocalTime), QLocale::ShortFormat) : dateTime = i18n("No date recognized");

        authorAndDateLabel->setText(QStringLiteral(" %1 (%2)").arg(auth, dateTime));
    }

    void connectOptionButton(QObject *recv, const char *method)
    {
        connect(optionButton, SIGNAL(clicked()), recv, method);
    }

    void uncheckLatexButton()
    {
        latexButton->setChecked(false);
    }

    void setAuthorAndDateAtList(int index, const QString &author, const QDateTime &dt)
    {
        if (index < listPreviousAuthorAndDateLabel.size()) {
            QString auth;
            (author.isNull() || author.isEmpty()) ? auth = i18n("Unknown author") : auth = author;

            QString dateTime;
            dt.isValid() ? dateTime = QLocale().toString(dt.toTimeSpec(Qt::LocalTime), QLocale::ShortFormat) : dateTime = i18n("No date recognized");

            listPreviousAuthorAndDateLabel[index]->setText(QStringLiteral(" > %1 (%2)").arg(auth, dateTime));
        }
    }

    void setContentAtList(int index, const QString &content)
    {
        if (index < listPreviousContentLabel.size()) {
            listPreviousContentLabel[index]->setText(content);
        }
    }

    void setScrollArea(QScrollArea *scrollArea)
    {
        scrollAr = scrollArea;
    }

    QScrollArea *scrollArea()
    {
        return scrollAr;
    }

private:
    QLabel *titleLabel;
    int countPreviousAnnotation = 0;
    QList<QLabel *> listPreviousAuthorAndDateLabel;
    QList<QLabel *> listPreviousContentLabel;
    QLabel *dateLabel;
    QLabel *authorAndDateLabel;
    QPoint mousePressPos;
    QToolButton *optionButton;
    QToolButton *latexButton;
    QScrollArea *scrollAr = nullptr;
};

// Qt::SubWindow is needed to make QSizeGrip work
AnnotWindow::AnnotWindow(QWidget *parent, QRect initialViewportBounds, Okular::Annotation *annot, Okular::Document *document, int page)
    : QFrame(parent, Qt::SubWindow)
    , m_viewportBounds(initialViewportBounds)
    , m_annot(annot)
    , m_document(document)
    , m_page(page)
{
    setAutoFillBackground(true);
    setFrameStyle(Panel | Raised);
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName(QStringLiteral("AnnotWindow"));

    const bool canEditAnnotation = m_document->canModifyPageAnnotation(annot);

    int countNumberPreviousAnnotation = 0;

    Okular::Annotation *currentAnnot = annot;
    while (currentAnnot) {
        Okular::Annotation *child = currentAnnot->previousAnnotation();
        if (child) {
            countNumberPreviousAnnotation++;
            currentAnnot = child;
        } else {
            break;
        }
    }

    const bool isLatexSourceEditor = latexAnnotation(m_annot);
#if HAVE_QSCINTILLA
    if (isLatexSourceEditor) {
        m_latexSourceEdit = createLatexSourceEditor(this, m_annot->contents());
        m_editorWidget = m_latexSourceEdit;
    }
#endif
    if (!m_editorWidget) {
        textEdit = new PopupTextEdit(this);
        textEdit->setAcceptRichText(false);
        textEdit->setPlainText(m_annot->contents());
        m_editorWidget = textEdit;
    }
    setFocusProxy(m_editorWidget);
    m_lastLatexNoteCompileSource = m_annot->contents();
    m_editorWidget->installEventFilter(this);

    m_prevCursorPos = editorCursorPosition();
    m_prevAnchorPos = editorAnchorPosition();

#if HAVE_QSCINTILLA
    if (m_latexSourceEdit) {
        connect(m_latexSourceEdit, &QsciScintilla::textChanged, this, &AnnotWindow::slotWindowTextChanged);
    }
#endif
    if (textEdit) {
        connect(textEdit, &KTextEdit::textChanged, this, &AnnotWindow::slotWindowTextChanged);
    }
    connect(m_document, &Okular::Document::annotationContentsChangedByUndoRedo, this, &AnnotWindow::slotHandleContentsChangedByUndoRedo);

    if (!canEditAnnotation) {
        setEditorReadOnly(true);
    }

    QVBoxLayout *mainlay = new QVBoxLayout(this);
    mainlay->setContentsMargins(2, 2, 2, 2);
    mainlay->setSpacing(0);
    m_title = new MovableTitle(this, countNumberPreviousAnnotation);
    mainlay->addWidget(m_title);
    mainlay->addWidget(m_editorWidget);
    QHBoxLayout *lowerlay = new QHBoxLayout();
    mainlay->addLayout(lowerlay);
    lowerlay->addItem(new QSpacerItem(5, 5, QSizePolicy::Expanding, QSizePolicy::Fixed));
    QSizeGrip *sb = new QSizeGrip(this);
    lowerlay->addWidget(sb);

    m_latexRenderer = new GuiUtils::LatexRenderer();
    // The Q_EMIT below is not wrong even if emitting signals from the constructor it's usually wrong
    // in this case the signal it's connected to inside MovableTitle constructor a few lines above
    Q_EMIT containsLatex(!latexAnnotation(m_annot) && GuiUtils::LatexRenderer::mightContainLatex(m_annot->contents())); // clazy:exclude=incorrect-emit

    m_title->setTitle(m_annot->window().summary());
    m_title->connectOptionButton(this, SLOT(slotOptionBtn()));

    const auto initialPosition = qApp->isRightToLeft() //
        ? initialViewportBounds.topRight() + QPoint(-defaultSize.width() - defaultPosition.x(), defaultPosition.y())
        : initialViewportBounds.topLeft() + defaultPosition;
    setGeometry(QRect(initialPosition, defaultSize));

    reloadInfo();
    connect(Okular::Settings::self(), &KCoreConfigSkeleton::configChanged, this, &AnnotWindow::reloadInfo);

    QScrollArea *scrollArea = m_title->scrollArea();
    if (scrollArea) {
        // Adapt the size of the scrollArea between 5 and 125px that depends of the content height size.
        if (scrollArea->widget() && scrollArea->widget()->layout()) {
            int contentHeight = scrollArea->widget()->layout()->sizeHint().height();
            int finalHeight = qBound(5, contentHeight + 5, 135);
            scrollArea->setFixedHeight(finalHeight);
        }

        // Put the scrollbar at the lowest position (to show firstly the most recent comments)
        if (scrollArea->verticalScrollBar()) {
            QTimer::singleShot(20, this, [scrollArea]() { scrollArea->verticalScrollBar()->setValue(scrollArea->verticalScrollBar()->maximum()); });
        }
    }
}

AnnotWindow::~AnnotWindow()
{
    delete m_latexRenderer;
    delete m_editorWidget;
}

Okular::Annotation *AnnotWindow::annotation() const
{
    return m_annot;
}

void AnnotWindow::updateAnnotation(Okular::Annotation *a)
{
    m_annot = a;
    if (latexAnnotation(m_annot)) {
        m_lastLatexNoteCompileSource = m_annot->contents();
    }
}

void AnnotWindow::reloadInfo()
{
    QColor newcolor;
    const bool isLatexNote = latexAnnotation(m_annot);
    if (isLatexNote) {
        newcolor = QColor(255, 248, 225);
    } else if (m_annot->subType() == Okular::Annotation::AText) {
        Okular::TextAnnotation *textAnn = static_cast<Okular::TextAnnotation *>(m_annot);
        if (textAnn->textType() == Okular::TextAnnotation::InPlace && textAnn->inplaceIntent() == Okular::TextAnnotation::TypeWriter) {
            newcolor = QColor(0xfd, 0xfd, 0x96);
        }
    }
    if (!newcolor.isValid()) {
        newcolor = m_annot->style().color().isValid() ? QColor(m_annot->style().color().red(), m_annot->style().color().green(), m_annot->style().color().blue(), 255) : Qt::yellow;
    }
    newcolor = popupPaperColor(newcolor);
    if (newcolor != m_color) {
        m_color = newcolor;
        const QColor foreground = Qt::black;
        QPalette windowPalette(m_color);
        windowPalette.setColor(QPalette::WindowText, foreground);
        windowPalette.setColor(QPalette::Text, foreground);
        setPalette(windowPalette);
        QPalette pl = editorPalette();
        pl.setColor(QPalette::Base, m_color);
        pl.setColor(QPalette::Text, foreground);
        pl.setColor(QPalette::WindowText, foreground);
        setEditorPalette(pl);
    }

    if (textEdit) {
        QFont popupFont = textEdit->font();
        popupFont.setPointSize(qBound(8, static_cast<int>(Okular::Settings::annotationPopupTextFontSize()), 36));
        textEdit->setFont(popupFont);
    }

    Okular::Annotation *parent = m_annot;
    Okular::Annotation *child = nullptr;
    int count = m_title->numberPreviousAnnotation() - 1;

    while (parent) {
        child = parent->previousAnnotation();
        if (child) {
            m_title->setAuthorAndDateAtList(count, child->author(), child->modificationDate());
            m_title->setContentAtList(count, child->contents());
            parent = child;
        } else {
            break;
        }

        count--;
    }

    m_title->setAuthorAndDate(m_annot->author(), m_annot->modificationDate());
}

int AnnotWindow::pageNumber() const
{
    return m_page;
}

void AnnotWindow::updateViewportBounds(QRect bounds)
{
    m_viewportBounds = bounds;
    fixupGeometry();
}

void AnnotWindow::fixupGeometry()
{
    const auto bounds = m_viewportBounds;
    if (bounds.isEmpty()) {
        return;
    }

    // Preserve user resizing and only constrain the popup to the visible viewport.
    const QSize constrainedSize(std::min(bounds.width(), width()), std::min(bounds.height(), height()));

    const QPoint position( //
        bounds.x() + std::max(0, std::min(bounds.width() - constrainedSize.width(), x() - bounds.x())),
        bounds.y() + std::max(0, std::min(bounds.height() - constrainedSize.height(), y() - bounds.y())));

    const QRect constrainedGeometry(position, constrainedSize);
    if (geometry() != constrainedGeometry) {
        setGeometry(constrainedGeometry);
    }
}

QString AnnotWindow::editorPlainText() const
{
#if HAVE_QSCINTILLA
    if (m_latexSourceEdit) {
        return m_latexSourceEdit->text();
    }
#endif
    return textEdit ? textEdit->toPlainText() : QString();
}

void AnnotWindow::setEditorPlainText(const QString &text)
{
#if HAVE_QSCINTILLA
    if (m_latexSourceEdit) {
        m_latexSourceEdit->setText(text);
        m_latexSourceEdit->SendScintilla(SCI_EMPTYUNDOBUFFER);
        m_latexSourceEdit->setModified(false);
        return;
    }
#endif
    if (textEdit) {
        textEdit->setPlainText(text);
    }
}

int AnnotWindow::editorCursorPosition() const
{
#if HAVE_QSCINTILLA
    if (m_latexSourceEdit) {
        const int bytePosition = static_cast<int>(m_latexSourceEdit->SendScintilla(SCI_GETCURRENTPOS));
        return characterPositionForScintillaBytePosition(m_latexSourceEdit, bytePosition);
    }
#endif
    return textEdit ? textEdit->textCursor().position() : 0;
}

int AnnotWindow::editorAnchorPosition() const
{
#if HAVE_QSCINTILLA
    if (m_latexSourceEdit) {
        const int bytePosition = static_cast<int>(m_latexSourceEdit->SendScintilla(SCI_GETANCHOR));
        return characterPositionForScintillaBytePosition(m_latexSourceEdit, bytePosition);
    }
#endif
    return textEdit ? textEdit->textCursor().anchor() : 0;
}

void AnnotWindow::setEditorSelection(int anchorPos, int cursorPos)
{
#if HAVE_QSCINTILLA
    if (m_latexSourceEdit) {
        int anchorLine = 0;
        int anchorIndex = 0;
        int cursorLine = 0;
        int cursorIndex = 0;
        m_latexSourceEdit->lineIndexFromPosition(scintillaBytePositionForCharacterPosition(m_latexSourceEdit, anchorPos), &anchorLine, &anchorIndex);
        m_latexSourceEdit->lineIndexFromPosition(scintillaBytePositionForCharacterPosition(m_latexSourceEdit, cursorPos), &cursorLine, &cursorIndex);
        m_latexSourceEdit->setSelection(anchorLine, anchorIndex, cursorLine, cursorIndex);
        return;
    }
#endif
    if (textEdit) {
        QTextCursor cursor = textEdit->textCursor();
        cursor.setPosition(anchorPos);
        cursor.setPosition(cursorPos, QTextCursor::KeepAnchor);
        textEdit->setTextCursor(cursor);
    }
}

bool AnnotWindow::editorUndoAvailable() const
{
#if HAVE_QSCINTILLA
    if (m_latexSourceEdit) {
        return m_latexSourceEdit->isUndoAvailable();
    }
#endif
    return textEdit && textEdit->document()->isUndoAvailable();
}

bool AnnotWindow::editorRedoAvailable() const
{
#if HAVE_QSCINTILLA
    if (m_latexSourceEdit) {
        return m_latexSourceEdit->isRedoAvailable();
    }
#endif
    return textEdit && textEdit->document()->isRedoAvailable();
}

void AnnotWindow::clearEditorUndoRedo()
{
#if HAVE_QSCINTILLA
    if (m_latexSourceEdit) {
        m_latexSourceEdit->SendScintilla(SCI_EMPTYUNDOBUFFER);
        m_latexSourceEdit->setModified(false);
        return;
    }
#endif
    if (textEdit) {
        textEdit->document()->clearUndoRedoStacks();
    }
}

void AnnotWindow::setEditorReadOnly(bool readOnly)
{
#if HAVE_QSCINTILLA
    if (m_latexSourceEdit) {
        m_latexSourceEdit->setReadOnly(readOnly);
        return;
    }
#endif
    if (textEdit) {
        textEdit->setReadOnly(readOnly);
    }
}

void AnnotWindow::setEditorPalette(const QPalette &palette)
{
#if HAVE_QSCINTILLA
    if (m_latexSourceEdit) {
        m_latexSourceEdit->setPalette(palette);
        const QColor base = palette.color(QPalette::Base);
        const QColor foreground = palette.color(QPalette::Text);
        m_latexSourceEdit->setPaper(base);
        m_latexSourceEdit->setCaretForegroundColor(foreground);
        m_latexSourceEdit->setCaretLineBackgroundColor(base.lightness() < 128 ? base.lighter(135) : base.darker(105));
        if (auto *lexer = m_latexSourceEdit->lexer()) {
            lexer->setPaper(base);
        }
        return;
    }
#endif
    if (textEdit) {
        textEdit->setPalette(palette);
        static_cast<PopupTextEdit *>(textEdit)->setCaretColor(palette.color(QPalette::Text));
    }
}

QPalette AnnotWindow::editorPalette() const
{
    if (m_editorWidget) {
        return m_editorWidget->palette();
    }
    return palette();
}

void AnnotWindow::showEvent(QShowEvent *event)
{
    QFrame::showEvent(event);

    // focus the content area by default
    if (m_editorWidget) {
        m_editorWidget->setFocus();
    }
}

void AnnotWindow::moveEvent(QMoveEvent *event)
{
    QFrame::moveEvent(event);
    fixupGeometry();
}

void AnnotWindow::resizeEvent(QResizeEvent *event)
{
    QFrame::resizeEvent(event);
    fixupGeometry();
}

void AnnotWindow::closeEvent(QCloseEvent *event)
{
    commitWindowText();
    updateLatexNoteAppearance();
    QFrame::closeEvent(event);
}

bool AnnotWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_editorWidget) {
        if (event->type() == QEvent::ShortcutOverride) {
            QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Escape) {
                event->accept();
                return true;
            }
        } else if (event->type() == QEvent::KeyPress) {
            QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent == QKeySequence::Undo && !editorUndoAvailable()) {
                m_document->undo();
                return true;
            } else if (keyEvent == QKeySequence::Redo && !editorRedoAvailable()) {
                m_document->redo();
                return true;
            } else if (keyEvent->key() == Qt::Key_Escape) {
                close();
                return true;
            }
        } else if (event->type() == QEvent::FocusIn) {
            raise();
        } else if (event->type() == QEvent::FocusOut) {
            commitWindowText();
            updateLatexNoteAppearance();
        }
    }
    return QFrame::eventFilter(watched, event);
}

void AnnotWindow::slotOptionBtn()
{
    // TODO: call context menu in pageview
    // Q_EMIT sig...
}

void AnnotWindow::commitWindowText()
{
    const QString contents = editorPlainText();
    const int cursorPos = editorCursorPosition();
    if (contents != m_annot->contents()) {
        m_document->editPageAnnotationContents(m_page, m_annot, contents, cursorPos, m_prevCursorPos, m_prevAnchorPos);
    }
    m_prevCursorPos = cursorPos;
    m_prevAnchorPos = editorAnchorPosition();
    clearEditorUndoRedo();
}

void AnnotWindow::slotWindowTextChanged()
{
    Q_EMIT containsLatex(!latexAnnotation(m_annot) && GuiUtils::LatexRenderer::mightContainLatex(editorPlainText()));
}

void AnnotWindow::updateLatexNoteAppearance()
{
    if (!LatexNoteUtils::annotationIsLatex(m_annot) || m_annot->contents().trimmed().isEmpty()) {
        return;
    }

    const QString latexSource = m_annot->contents();
    if (latexSource == m_lastLatexNoteCompileSource) {
        return;
    }
    m_lastLatexNoteCompileSource = latexSource;
    const QString annotationUniqueName = m_annot->uniqueName();
    if (annotationUniqueName.isEmpty()) {
        return;
    }

    const Okular::Page *page = m_document->page(m_page);
    const QColor textColor = LatexNoteUtils::colorForLatexAnnotation(m_annot);
    if (Okular::TextAnnotation *textAnnotation = LatexNoteUtils::annotationAsLatexTextAnnotation(m_annot)) {
        LatexNoteUtils::updateLatexTextAnnotationAppearanceAsync(this,
                                                                 m_document,
                                                                 m_page,
                                                                 annotationUniqueName,
                                                                 latexSource,
                                                                 textColor,
                                                                 textAnnotation->style().color(),
                                                                 textAnnotation->inplaceBorderColor(),
                                                                 LatexNoteUtils::layoutWidthForLatexTextAnnotation(textAnnotation, page),
                                                                 textAnnotation->inplaceIntent() != Okular::TextAnnotation::TypeWriter);
    } else if (Okular::StampAnnotation *stampAnnotation = LatexNoteUtils::annotationAsLatexStampAnnotation(m_annot)) {
        Q_UNUSED(page);
        const bool boxed = stampAnnotation->latexNoteType() != Okular::Annotation::LatexNotePlain;
        QColor fillColor = boxed ? stampAnnotation->latexFillColor() : Qt::transparent;
        if (boxed && (!fillColor.isValid() || fillColor.alpha() == 0)) {
            fillColor = Qt::yellow;
        }
        QColor borderColor = boxed ? stampAnnotation->latexBorderColor() : Qt::transparent;
        if (boxed && (!borderColor.isValid() || borderColor.alpha() == 0)) {
            borderColor = textColor;
        }
        LatexNoteUtils::updateLatexStampAnnotationAppearanceAsync(this,
                                                                  m_document,
                                                                  m_page,
                                                                  annotationUniqueName,
                                                                  latexSource,
                                                                  textColor,
                                                                  fillColor,
                                                                  borderColor,
                                                                  stampAnnotation->latexLayoutWidth(),
                                                                  boxed);
    }
}

void AnnotWindow::renderLatex(bool render)
{
    if (!textEdit) {
        return;
    }

    if (render) {
        commitWindowText();
        textEdit->setReadOnly(true);
        textEdit->setAcceptRichText(true);
        QString contents = m_annot->contents();
        contents = Qt::convertFromPlainText(contents);
        QColor fontColor = textEdit->textColor();
        int fontSize = textEdit->fontPointSize();
        QString latexOutput;
        GuiUtils::LatexRenderer::Error errorCode = m_latexRenderer->renderLatexInHtml(contents, fontColor, fontSize, Okular::Utils::realDpi(nullptr).width(), latexOutput);
        switch (errorCode) {
        case GuiUtils::LatexRenderer::LatexFailed:
            KMessageBox::error(this, latexOutput, i18n("LaTeX rendering failed"));
            m_title->uncheckLatexButton();
            renderLatex(false);
            break;
        case GuiUtils::LatexRenderer::NoError:
        default:
            textEdit->setHtml(contents);
            break;
        }
    } else {
        textEdit->setAcceptRichText(false);
        textEdit->setPlainText(m_annot->contents());
        textEdit->setReadOnly(false);
    }
}

void AnnotWindow::slotHandleContentsChangedByUndoRedo(Okular::Annotation *annot, const QString &contents, int cursorPos, int anchorPos)
{
    if (annot != m_annot) {
        return;
    }

    setEditorPlainText(contents);
    setEditorSelection(anchorPos, cursorPos);
    m_prevCursorPos = cursorPos;
    m_prevAnchorPos = anchorPos;
    if (m_editorWidget) {
        m_editorWidget->setFocus();
    }
    Q_EMIT containsLatex(!latexAnnotation(m_annot) && GuiUtils::LatexRenderer::mightContainLatex(m_annot->contents()));
}

#include "annotwindow.moc"
