/*
    SPDX-FileCopyrightText: 2026 Mengshee contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef MENGSHEEPRINTDIALOG_H
#define MENGSHEEPRINTDIALOG_H

#include <QDialog>
#include <QList>
#include <QPageLayout>
#include <QPageSize>
#include <QPrinter>

class QCheckBox;
class QButtonGroup;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPrintPreviewWidget;
class QPushButton;
class QRadioButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QWidget;

namespace Okular
{

/**
 * Application-owned print workspace used on Windows.
 *
 * The native Windows print dialog cannot display previews for Qt's Win32
 * printing path. This dialog keeps the settings and the preview together and
 * sends the accepted settings directly to QPrinter.
 */
class MengsheePrintDialog : public QDialog
{
    Q_OBJECT

public:
    MengsheePrintDialog(QPrinter *printer, int documentPageCount, int currentPage, QPageLayout::Orientation documentOrientation, bool hasPageSelection, QWidget *printOptionsWidget, QWidget *parent = nullptr);

Q_SIGNALS:
    void paintRequested(QPrinter *printer);

private Q_SLOTS:
    void printerChanged();
    void printRangeChanged();
    void pageSizeChanged();
    void orientationChanged();
    void layoutModeChanged(int mode);
    void layoutSettingsChanged();
    void schedulePreviewUpdate();
    void previewPageChanged(int page);
    void previousPreviewPage();
    void nextPreviewPage();
    void updatePreviewNavigation();
    void validateSettings();

private:
    void accept() override;
    void buildUi();
    void populatePrinters();
    void populatePageSizes();
    void populateDuplexOptions();
    void connectPrintOptionChanges();
    void syncLayoutProperties(bool preview);
    void resetPreviewSheet();
    void applyCommonSettings(QPrinter &printer);
    void applyPrintRange(QPrinter &printer, bool preview);
    bool pageRangeIsValid() const;
    QPageLayout::Orientation selectedOrientation() const;
    QPrinter::DuplexMode selectedDuplexMode() const;
    QPageSize selectedPageSize() const;
    QString pageSizeDescription(const QPageSize &pageSize) const;

    QPrinter *m_printer;
    int m_documentPageCount;
    int m_currentDocumentPage;
    int m_previewSheet = 1;
    int m_previewSheetCount = 1;
    QPageLayout::Orientation m_documentOrientation;
    bool m_hasPageSelection;
    bool m_previewUpdatePending = false;
    bool m_settingsValid = false;
    QWidget *m_printOptionsWidget;
    QList<QPageSize> m_pageSizes;

    QComboBox *m_printerCombo = nullptr;
    QSpinBox *m_copiesSpin = nullptr;
    QCheckBox *m_grayscaleCheck = nullptr;
    QCheckBox *m_duplexCheck = nullptr;
    QComboBox *m_duplexBindingCombo = nullptr;
    QRadioButton *m_allPagesRadio = nullptr;
    QRadioButton *m_currentPageRadio = nullptr;
    QRadioButton *m_rangePagesRadio = nullptr;
    QRadioButton *m_selectedPagesRadio = nullptr;
    QLineEdit *m_pageRangeEdit = nullptr;
    QLabel *m_pageRangeError = nullptr;
    QComboBox *m_pageSizeCombo = nullptr;
    QButtonGroup *m_layoutModeGroup = nullptr;
    QStackedWidget *m_layoutSettingsStack = nullptr;
    QDoubleSpinBox *m_posterScaleSpin = nullptr;
    QDoubleSpinBox *m_posterOverlapSpin = nullptr;
    QCheckBox *m_posterCutMarksCheck = nullptr;
    QComboBox *m_pagesPerSheetCombo = nullptr;
    QComboBox *m_pageOrderCombo = nullptr;
    QCheckBox *m_pageBorderCheck = nullptr;
    QComboBox *m_bookletSubsetCombo = nullptr;
    QComboBox *m_bookletBindingCombo = nullptr;
    QRadioButton *m_autoOrientationRadio = nullptr;
    QRadioButton *m_portraitRadio = nullptr;
    QRadioButton *m_landscapeRadio = nullptr;
    QLabel *m_previewDescription = nullptr;
    QPrintPreviewWidget *m_previewWidget = nullptr;
    QPushButton *m_previousPageButton = nullptr;
    QPushButton *m_nextPageButton = nullptr;
    QSlider *m_pageSlider = nullptr;
    QLabel *m_pageNumberLabel = nullptr;
    QDialogButtonBox *m_buttonBox = nullptr;
};

}

#endif
