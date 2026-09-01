# Mengshee

Mengshee is a PDF-centered document and slide editor derived from KDE Okular.
It is focused on technical reading, annotation, LaTeX-backed notes, and
PDF-native slide construction.

The name **Mengshee** is a deliberately adapted spelling of **Mengxi**
(`Mèngxī`, 梦溪, in Hanyu Pinyin). It pays tribute to the Northern Song
polymath Shen Kuo (沈括), traditionally also referred to as Shen Mengxi
(沈梦溪), and to his encyclopedic work
[*Mengxi Bitan*](https://zh.wikisource.org/zh-hans/%E6%A2%A6%E6%BA%AA%E7%AC%94%E8%B0%88)
(`Mèngxī Bǐtán`, 《梦溪笔谈》), commonly known in English as *Dream Pool
Essays*. The name reflects the project's interest in reading, technical
inquiry, careful observation, and the practical recording of knowledge.

Mengshee is not an official KDE Okular release and is not affiliated with or
endorsed by KDE. It inherits a large amount of Okular's document-viewing
infrastructure, but the product direction in this repository is
Mengshee-specific.

## What Mengshee Is For

Mengshee treats a PDF as an editable technical workspace:

- read and annotate PDFs;
- insert, delete, duplicate, and reorder pages;
- use pages as slide canvases;
- add LaTeX-rendered notes as editable visual objects;
- add template notes such as auto-updating page numbers;
- save a normal PDF that other readers can display through standard annotation
  appearance streams.

The long-term direction is a LaTeX-native slide editor built on PDF: direct
visual editing like a slide tool, with the typography and formula quality of a
LaTeX workflow.

## Main Features In This Fork

- **LaTeX notes**
  - Stored as standard PDF stamp annotations.
  - Editable source is preserved in the PDF.
  - Rendered appearance is written as a self-contained PDF appearance stream.
  - Other PDF readers can display the note without StemTeX or temporary files.

- **StemTeX renderer integration**
  - Integrates [StemTeX](https://github.com/zhaiyusci/stemtex) as a fast XeTeX
    renderer for LaTeX notes.
  - Bundles only StemTeX's maintained `unicodemath` base profile.
  - Launches StemTeX Profile Creator for user-selected fonts, packages, and
    custom preambles, kept separate as Mengshee-managed user profiles.
  - Users may select an external TeX Live tree for packages and fonts.
  - Runtime state, fontconfig files, caches, traces, and rendered-note outputs
    are written under Mengshee's per-user application-data directory, not under
    the installation or StemTeX standalone-application directories.

- **Template notes**
  - Stored as standard PDF FreeText annotations.
  - Mengshee-specific template metadata lives in one JSON payload.
  - The visible text and normal appearance are refreshed from document context,
    such as page number, page count, page label, title, author, or date.

- **Page editing**
  - Blank page insertion, page deletion, page duplication, and thumbnail
    reordering are part of the slide-editing model.
  - Page moves preserve annotations and live edit state.
  - Saved PDFs keep standard annotation structures and normal appearance
    streams.

- **Internal-link previews**
  - Internal PDF links can open in independent, splittable auxiliary panes
    without moving the main reading position.

## Downloads

Releases are published on GitHub:

https://github.com/zhaiyusci/mengshee/releases

## Documentation Map

- `docs/latex-native-slides-vision.md`
  - Product direction for Mengshee as a PDF-centered, LaTeX-native slide editor.
- `docs/latex-note-pdf-spec.md`
  - PDF representation and rendering contract for LaTeX notes.
- `docs/template-note-pdf-spec.md`
  - PDF representation, JSON payload, expression language, predefined
    variables, and refresh rules for template notes.
- `docs/page-editing-annotation-model.md`
  - Live document model for page editing and annotation preservation.
- `README.local-components.md`
  - Local component and submodule notes.
- `docs/local-poppler-fork.md`
  - Ownership, build boundary, and upgrade procedure for the locally modified
    Poppler submodule.
- `README.local-linux-build.md`
  - Local Linux build notes, when needed.

The main README is intentionally a project entry point. Detailed PDF schemas
and implementation contracts belong in `docs/`.

## Source Layout

- `shell/`
  - Mengshee desktop application shell.
- `part/`
  - Main viewer part, annotation UI, LaTeX note logic, template note logic, and
    page-view interactions.
- `core/`
  - Document model and shared Okular core code.
- `generators/poppler/`
  - PDF backend integration.
- `external/poppler/`
  - Pinned local Poppler fork, including generic annotation and PDF
    page-sequence extensions required by the PDF backend.
- `docs/`
  - Mengshee-specific design specs.

## PDF Portability Contract

Mengshee-specific editability is stored as private metadata on standard PDF
annotations. Display in other PDF readers must rely on standard annotation
subtypes and self-contained `/AP /N` appearance streams.

This means:

- LaTeX notes are standard `/Stamp` annotations with LaTeX metadata and a normal
  appearance stream.
- Template notes are standard `/FreeText` annotations with template metadata
  and a normal appearance stream.
- Image notes are standard `/Stamp` annotations with embedded image appearance.
- Page editing must preserve annotation identity, geometry, style, contents,
  appearance, and private metadata.

When in doubt, the saved PDF should be readable in Mengshee and displayable in
Adobe Acrobat or another standards-oriented PDF reader.

## Reporting Issues

Report Mengshee-specific issues in this repository:

https://github.com/zhaiyusci/mengshee/issues

Do not report Mengshee fork bugs to KDE Okular unless the problem has been
confirmed in upstream Okular without Mengshee-specific changes.

## Upstream

Mengshee is derived from KDE Okular:

https://invent.kde.org/graphics/okular

Okular's original license and copyright notices remain in the inherited source
files. Mengshee-specific changes in this repository follow the same licensing
terms as the surrounding Okular code unless a file states otherwise.
