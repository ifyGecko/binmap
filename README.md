# binmap

A binary visualization tool inspired by Veles / Cantor.Dust. It memory-maps any
file, treats its raw bytes as a data source, and renders them through one of
**10 visualization techniques** — locality-preserving 2D layouts, statistical
overlays, N-gram fingerprints, and two rotatable 3D trigraph projections —
using SDL3 for rendering.

Use it to:
- fingerprint a file's type at a glance,
- find boundaries between code, data, padding, strings, and compressed regions,
- spot embedded files, encrypted blobs, or steganographic payloads.

binmap is intentionally narrow: throw an unknown file at it, cycle through the
views, decide what to *do next* with the file. It is not a reverse-engineering
suite and does not aim to give precise structural answers.

## Building

Requirements: `pkg-config`, a C11 compiler, **SDL3** (`libsdl3-dev` /
`sdl3-devel`), and a Linux system.

```sh
make
./binmap path/to/file
```

## Controls

| Key | Action |
|-----|--------|
| `TAB` / `SHIFT+TAB` | Next / previous view |
| `L` | Toggle the on-screen legend |
| `D` | Toggle a short description of the current view |
| `A` | Toggle auto-rotate (3D views only) |
| `←` `→` `↑` `↓` | Manual rotate (3D views only) |
| `+` `-` / mouse wheel | Zoom in / out (3D views only) |
| `R` | Reset 3D view (yaw, pitch, zoom, auto-rotate) |
| `[` / `]` | Nudge range start backward / forward |
| `,` / `.` | Nudge range end backward / forward |
| `SHIFT` + key | Coarse range step |
| `0` | Reset range to the full file |
| Mouse drag | Adjust range via the slider at the bottom |
| `ESC` / `Q` | Quit |

The status bar shows the current view, the file name and size, and (for 3D
views) live yaw / pitch / zoom and whether auto-rotate is on. Press `D` for
an in-app reminder of what the current view actually shows.

---

## Views

Views are ordered as they appear in the TAB cycle. The "Technique" line states
the byte → pixel mapping; the "When valuable" line states what it shows that
the other views don't.

### 1. Byte Class Stripe

- **Technique** — Row-major layout. Each pixel covers one byte (or one
  `size/(W·H)` aggregate when the file is larger than the canvas). Each byte
  is colored by class: `0x00` dark blue, control bytes red, whitespace green,
  printable ASCII blue, high-bit-set amber, `0xFF` white.
- **When valuable** — The fastest possible orientation view. Headers,
  string-heavy regions (`.rodata`, manifests), null padding, `0xFF` padding,
  and binary code stand out by color *immediately*. This is the right place to
  *start* on an unfamiliar file. The same renderer also powers the file
  minimap behind the range slider.

### 2. Hilbert Curve

- **Technique** — Bytes laid along a 2D Hilbert curve on the largest
  power-of-two grid that fits the canvas. Bytes that were adjacent in the file
  stay adjacent on screen.
- **When valuable** — Region boundaries (text/code/data) appear as **visually
  coherent blocks** instead of being smeared across rows by row-major layout.
  Compressed vs. uncompressed sections, embedded files, header vs. body
  transitions — all snap into clean rectangles.

### 3. Digraph Dot Plot

- **Technique** — For every pair `(byte[i], byte[i+1])`, increment
  `counts[a][b]` in a 256×256 grid. Render with log-scaled intensity. (The
  classic Cantor.Dust / Veles signature view.)
- **When valuable** — **File-type fingerprinting**: x86 code, ARM code, ASCII
  text, JPEG, MP3, gzip, AES output, and PNG all have visually distinct
  signatures in this view. A single image classifies the whole file.

### 4. Entropy Heatmap

- **Technique** — Chunk the file (chunk size adapts to file size and canvas).
  Compute Shannon entropy per chunk in bits/byte (0..8). Plot each chunk as a
  pixel in row-major order, colored on a viridis-ish blue→green→yellow→red
  palette.
- **When valuable** — Instantly highlights **compressed, encrypted, or random
  regions** vs. structured data. Sections of ELF/PE/Mach-O binaries, embedded
  zlib/png/jpeg/zip streams, and AES blobs all show as crisp high-entropy
  bands. Industry-standard view for forensics and malware analysis.

### 5. Strings Density

- **Technique** — Slide a window over the file; for each chunk, count bytes
  that participate in a run of ≥ 4 consecutive printable-ASCII bytes (the
  `strings(1)` rule). Plot density per chunk on a black→orange→yellow→white
  palette.
- **When valuable** — The classic "**where do the strings live**" view used in
  reverse engineering, malware analysis, and firmware inspection. Symbol
  tables, error message tables, embedded configuration, and user-facing text
  light up; obfuscated or encrypted text disappears.

### 6. Self-Similarity

- **Technique** — Split the file into N chunks (N up to 512). Compute a
  256-bin byte-frequency histogram for each chunk. Compute the L1-distance
  between every pair of those frequency vectors. Render the N×N distance
  matrix as a heatmap on the shared `heat_color` palette (low distance =
  bright = similar).
- **When valuable** — Repeating sections, periodic structure, and embedded
  files manifest as **off-diagonal bright lines or block patterns**. The
  diagonal is always bright (self-similarity). Multiple copies of a similar
  region produce visible checkerboard or stripe patterns — the same way DNA
  self-similarity matrices reveal tandem repeats.

### 7. Bit-Plane View

- **Technique** — Eight sub-images in a 4×2 grid, one per bit position. For
  each pixel of each sub-image, the byte's bit at that position is drawn
  black (0) or white (1). Each sub-panel labelled `BIT 0` … `BIT 7`.
- **When valuable** — **Bit-aligned structure invisible to byte-level views.**
  Useful for low-level protocol/format analysis, custom packed records,
  parity-padded data, and steganography. A "noisy" bit 0 with structured high
  bits is a telltale of LSB stego.

### 8. RGB Raw

- **Technique** — Treat every consecutive triple of bytes as one `(R, G, B)`
  pixel, laid row-major. The pixel grid auto-scales to the canvas size.
- **When valuable** — **Sometimes literally reveals embedded bitmap data.**
  BMP, raw image dumps, framebuffer captures, and some firmware assets appear
  as actual images. For non-image files, the visual texture is itself a
  fingerprint — code, text, and compressed regions all look distinctively
  different. Popular in CTF / steganography work.

### 9. Trigraph 3D

- **Technique** — For each consecutive triple `(byte[i], byte[i+1],
  byte[i+2])`, plot a point at the Cartesian coordinates `(a, b, c)` in the
  256³ cube. Accumulate into a per-screen-pixel count buffer; map count → log
  color via the heat palette. Wireframe bounding cube drawn for orientation.
  Rotatable, zoomable.
- **When valuable** — **3D extension of the digraph dot plot.** Even more
  powerful at fingerprinting — ASCII text clusters in a tetrahedron of the
  cube; structured binary forms striated planes; AES output uniformly fills
  the cube. Often reveals structure invisible in the 2D digraph.

### 10. Spherical Trigraph 3D

- **Technique** — Same triple as the Cartesian trigraph but interpreted as
  spherical coordinates: `θ = a/256 · 2π` (azimuth), `φ = b/256 · π`
  (inclination), `r = c/255` (radius). The byte triples then live in (or
  around) the unit sphere. Wireframe equator + two meridians drawn for
  orientation. (Veles-style.)
- **When valuable** — Different geometric reading of the same triple data.
  Bias toward particular byte ranges in any of the three positions produces
  visible clustering on the sphere surface (or away from it). Pairs well
  with the Cartesian trigraph as an alternative projection of the same
  N-gram statistic.

---

## When to use which view

A rough decision tree:

- **"What kind of file is this?"** → Digraph dot plot, trigraph 3D.
- **"Where are the regions / sections / boundaries?"** → Hilbert, byte-class
  stripe, entropy heatmap.
- **"Is there compressed / encrypted data, and where?"** → Entropy heatmap,
  strings density (as the inverse signal).
- **"Where are the strings / symbols?"** → Strings density, byte-class
  stripe.
- **"Are there repeated structures / embedded files?"** → Self-similarity,
  Hilbert.
- **"Bit-level structure / steganography?"** → Bit-plane view.
- **"Is there an image / framebuffer / raw bitmap embedded?"** → RGB raw.
- **"Just look pretty for a screenshot."** → Spherical trigraph 3D
  (with auto-rotate).

---

## File layout

```
binmap/
├── Makefile
├── README.md
└── src/
    ├── binmap.h     view enum, app state, render API
    ├── font.h
    ├── font.c       built-in 5x7 bitmap font + draw_text / blit_text
    ├── render.c     all 10 view implementations
    └── main.c       SDL3 init, mmap, event loop, status bar, legend
```

## Limitations

- Linux only. (SDL3 makes porting cheap; not yet packaged.)
- Single-file, interactive viewer — no batch / headless PNG output.
- Fixed color palettes per view (no user-configurable themes).
- 3D views subsample to ~400 k points for animation; very large files
  display structural patterns but not every byte.
- The byte-class palette is shared across most views; views that intend a
  different message (e.g. heat / log-density) use their own dedicated
  palette.
