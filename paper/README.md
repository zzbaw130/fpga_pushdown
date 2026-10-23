# PVLDB LaTeX Template

Official LaTeX template for PVLDB / VLDB submissions, maintained by the
VLDB Proceedings Chairs.

Bundles **acmart.cls v2.19 (2026/06/27)**, pinned from
[CTAN](https://ctan.org/pkg/acmart).

Use this repository directly rather than an old Overleaf copy, a
previous volume's template, or your local TeX installation's default
`acmart.cls`.

## Contents

| File | Purpose |
|---|---|
| `main.tex` | The paper source you edit. |
| `acmart.cls` | ACM base class, v2.19. Do not edit. |
| `pvldb.sty` | PVLDB/VLDB-specific formatting and boilerplate. Do not edit. |
| `ACM-Reference-Format.bst` | Bibliography style matching `acmart.cls` v2.19. |
| `sample.bib` | Example bibliography — replace with your own. |
| `figures/` | Example figure — replace/add your own. |

## Notable formatting behavior

- **`acmart.cls` is pinned to a fixed version**, bundled directly in this
  repo, so output doesn't vary depending on the acmart version an
  author's TeX Live install or Overleaf project happens to default to.
- **ALL-CAPS section headings are explicitly restored**, effective
  starting PVLDB Vol. 20 (2027). acmart v2.x changed its default so
  section headings are no longer auto-capitalized; PVLDB uses ALL-CAPS
  ("1 INTRODUCTION"), so this is set explicitly rather than left to the
  class default.
- **VLDB-specific formatting lives in `pvldb.sty`**, not hardcoded in
  `main.tex`. The PVLDB Reference Format block, license/copyright
  footnote, and Artifact Availability block are rendered by a single
  `\vldbtopmatter` call instead of ~25 lines of inline LaTeX authors could
  edit or break by accident.

## Usage

1. Edit `main.tex`: title, authors, affiliations, abstract, body,
   bibliography. Leave `acmart.cls` and `pvldb.sty` alone.

2. Set the paper-specific values near the top of `main.tex`:
   ```latex
   \renewcommand\vldbdoi{XX.XX/XXX.XX}
   \renewcommand\vldbpages{XXX-XXX}
   \renewcommand\vldbavailabilityurl{URL_TO_YOUR_ARTIFACTS}
   ```
   Leave `\vldbavailabilityurl` empty (`{}`) if there's nothing to link.
   DOI/pages are normally assigned during production — placeholders are
   fine for submission.

3. Don't touch the two blocks marked
   `%%% do not modify the following VLDB block %%`:
   - `\usepackage{pvldb}` right after `\documentclass`
   - `\vldbtopmatter` right after `\maketitle`

   These apply the reference format, license footnote, availability
   block, and ALL-CAPS headings. Removing them will put your paper out
   of compliance with the formatting guidelines.

4. Compile:
   ```
   pdflatex main.tex
   bibtex main
   pdflatex main.tex
   pdflatex main.tex
   ```

5. Check output against the current
   [formatting guidelines](https://www.vldb.org/formatting-guidelines.html)
   before submitting.

## Issues

Open an issue in this repository, or contact the VLDB Proceedings
Chairs, if you hit a bug in the template or a formatting flag you
believe originates here rather than in your own `main.tex`.
