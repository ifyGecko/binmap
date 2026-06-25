# binmap

A binary visualization tool inspired by Veles / Cantor.Dust. It memory-maps any
file, treats its raw bytes as a data source, and renders them through one of
**20 visualization techniques** — locality-preserving 2D layouts, statistical
overlays, N-gram fingerprints, polar / circular layouts, and rotatable 3D
coordinate systems — using SDL3 for rendering.

Use it to:
- fingerprint a file's type at a glance,
- find boundaries between code, data, padding, strings, and compressed regions,
- detect record sizes, page sizes, and other structural periods,
- spot embedded files, encrypted blobs, or steganographic payloads.

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
| `A` | Toggle auto-rotate (3D views only) |
| `←` `→` `↑` `↓` | Manual rotate (3D views only) |
| `+` `-` / mouse wheel | Zoom in / out (3D views only) |
| `R` | Reset 3D view (yaw, pitch, zoom, auto-rotate) |
| `ESC` / `Q` | Quit |

The status bar shows the current view, the file name and size, and (for 3D
views) live yaw / pitch / zoom and whether auto-rotate is on.

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
  *start* on an unfamiliar file.

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

### 4. Markov Chord

- **Technique** — Place all 256 byte values around a circle. For each digraph
  `(a, b)` draw a quadratic Bezier arc from `a` to `b` curving through the
  center; color and intensity by log of the transition count. Top-K
  transitions only (threshold auto-tuned).
- **When valuable** — Complementary to the digraph dot plot. The dot plot
  shows *which* pairs occur; the chord view shows the **structure of the
  transition graph** — clusters of mutually-connected byte values stand out
  (e.g. text → text, opcode → operand, structured frame headers). Visually
  fingerprint-like; different file types produce dramatically different
  chord patterns.

### 5. Entropy Heatmap

- **Technique** — Chunk the file (chunk size adapts to file size and canvas).
  Compute Shannon entropy per chunk in bits/byte (0..8). Plot each chunk as a
  pixel in row-major order, colored on a viridis-ish blue→green→yellow→red
  palette.
- **When valuable** — Instantly highlights **compressed, encrypted, or random
  regions** vs. structured data. Sections of ELF/PE/Mach-O binaries, embedded
  zlib/png/jpeg/zip streams, and AES blobs all show as crisp high-entropy
  bands. Industry-standard view for forensics and malware analysis.

### 6. Autocorrelation

- **Technique** — Compute Pearson autocorrelation of the byte sequence at lags
  `1..W` (W = canvas width minus margins). Plot as a signed bar graph: red bars
  upward for positive correlation, blue bars downward for negative. Reference
  grid lines at ±0.25, ±0.5, ±0.75, ±1.0. Sampling stride adapts so the view
  computes in a fixed time budget regardless of file size.
- **When valuable** — **Quantitatively reveals record sizes and repetition
  periods.** Strong spikes appear at multiples of the period — e.g. a 16-byte
  record format shows spikes at lag 16, 32, 48… A 4 KB page-aligned structure
  spikes at 4096, 8192, etc. Nothing else in the toolkit shows this
  information.

### 7. Strings Density

- **Technique** — Slide a window over the file; for each chunk, count bytes
  that participate in a run of ≥ 4 consecutive printable-ASCII bytes (the
  `strings(1)` rule). Plot density per chunk on a black→orange→yellow→white
  palette.
- **When valuable** — The classic "**where do the strings live**" view used in
  reverse engineering, malware analysis, and firmware inspection. Symbol
  tables, error message tables, embedded configuration, and user-facing text
  light up; obfuscated or encrypted text disappears.

### 8. Self-Similarity

- **Technique** — Split the file into N chunks (N up to 512). Compute the
  256-bin byte histogram for each chunk. Compute the L1-distance between every
  pair of chunk histograms. Render the N×N distance matrix as a heatmap on the
  shared `heat_color` palette (low distance = bright = similar).
- **When valuable** — Repeating sections, periodic structure, and embedded
  files manifest as **off-diagonal bright lines or block patterns**. The
  diagonal is always bright (self-similarity). Multiple copies of a similar
  region produce visible checkerboard or stripe patterns — the same way DNA
  self-similarity matrices reveal tandem repeats.

### 9. Bit-Plane View

- **Technique** — Eight sub-images in a 4×2 grid, one per bit position. For
  each pixel of each sub-image, the byte's bit at that position is drawn
  black (0) or white (1). Each sub-panel labelled `BIT 0` … `BIT 7`.
- **When valuable** — **Bit-aligned structure invisible to byte-level views.**
  Useful for low-level protocol/format analysis, custom packed records,
  parity-padded data, and steganography. A "noisy" bit 0 with structured high
  bits is a telltale of LSB stego.

### 10. Byte Histogram

- **Technique** — 256-bin histogram of byte values, rendered as a log-scaled
  bar chart. Bars colored by their byte class. Reference decade grid lines
  in the background. Y-axis title shows the maximum count.
- **When valuable** — The byte distribution alone is a strong fingerprint.
  ASCII text peaks in `0x20-0x7E` with a tall space bar; uniformly random
  data (encrypted/compressed) is flat; x86 code has spikes at common opcodes;
  UTF-8 has characteristic high-bit patterns. Quick sanity check for any
  file.

### 11. Z-Order (Morton)

- **Technique** — Same idea as Hilbert: 2D layout along a space-filling curve
  on a power-of-two grid. Uses Morton (Z-order) interleaving of x/y bits
  instead of the Hilbert recursion.
- **When valuable** — Mostly here as a comparison view against the Hilbert
  layout. Morton has worse locality preservation than Hilbert (visible "L"
  artifacts at quadrant boundaries), but it's cheaper to compute and shows the
  same data with different structural emphasis. Sometimes a region looks
  cleaner in Morton than Hilbert simply because of how the curve crosses
  boundaries.

### 12. RGB Raw

- **Technique** — Treat every consecutive triple of bytes as one `(R, G, B)`
  pixel, laid row-major. The pixel grid auto-scales to the canvas size.
- **When valuable** — **Sometimes literally reveals embedded bitmap data.**
  BMP, raw image dumps, framebuffer captures, and some firmware assets appear
  as actual images. For non-image files, the visual texture is itself a
  fingerprint — code, text, and compressed regions all look distinctively
  different. Popular in CTF / steganography work.

### 13. Polar Spiral

- **Technique** — Bytes laid along an Archimedean spiral with **equal area per
  byte**: `r(t) = R·√t`, `θ(t) = N·2π·t`, where `t = byte_index/size`. Pixel
  iteration with inverse mapping fills every disk pixel.
- **When valuable** — Single-glance "scan from inside out" reading of a file.
  Byte 0 is the center, the last byte is the rim. Equal-area means a run of
  null padding occupies the same disk area whether near the start or the end
  — useful for size-aware comparisons.

### 14. Concentric Rings

- **Technique** — Polar layout split into discrete concentric rings (one ring
  per ~4 px of radius). Each ring carries a fixed slice of bytes; angular
  position within a ring is the offset inside that slice.
- **When valuable** — **Sharp ring boundaries** make **block-aligned data**
  pop visually. File systems (FAT clusters, ext4 inode tables, NTFS MFT
  records), database pages, and any data with a fixed record size show as
  crisp concentric color bands. Compared to the polar spiral, gives up
  byte-level continuity for structural clarity.

### 15. Circular Hilbert

- **Technique** — Render a Hilbert curve on an n×n grid, then map the grid
  onto the disk via the **Shirley–Chiu concentric (equal-area)
  square↔disk** transform. Inverse map is computed per disk pixel.
- **When valuable** — Combines Hilbert's locality preservation with a
  circular framing. Adjacent bytes stay adjacent visually, but the layout
  fills a disk instead of a square — useful when you want the byte-stream
  layout to share visual identity with the polar / rings views.

### 16. Cylindrical 3D

- **Technique** — Place each byte on the surface of a vertical cylinder:
  axial position `y = i/size`, angular position `θ = (i mod 256)/256 · 2π`.
  Each byte's class palette color drawn as one pixel. Default wrap period
  256 bytes. Rotatable, zoomable.
- **When valuable** — Patterns aligned to **256-byte (or multiples)
  structures** become visible vertical stripes. Useful for spotting page
  alignment, repeated record formats, and instruction-cache-line alignment.
  Cylinder is the simplest 3D layout that demonstrates structural periodicity.

### 17. Helical 3D

- **Technique** — Continuous helix: `θ(i) = i/pitch · 2π`, `y(i) = i/size`.
  Every byte gets a unique 3D position (no overdraw). Default pitch 256
  bytes/revolution. Uses incremental rotation matrices to compute byte
  positions without per-byte trig calls.
- **When valuable** — Unlike the cylinder, which wraps angle by `i mod
  period` and stacks revolutions on top of each other, the helix gives every
  byte a unique location and visualizes the **byte stream's actual
  trajectory**. Vertical alignment of features snaps into place when the
  pitch matches a true record size — making it a *visual period detector*.

### 18. Toroidal 3D

- **Technique** — Two coexisting periods at once. Outer angle
  `u = (i mod 256)/256 · 2π`, inner angle `v = ((i/256) mod 256)/256 · 2π`.
  Position: `x = (R + r·cos v)·cos u`, `y = r·sin v`, `z = (R + r·cos v)·sin u`.
  256 × 256 = 65 536 distinct positions per full wrap.
- **When valuable** — Reveals **nested periodic structure** the cylinder
  can't show alone — e.g. a 256-byte record nested inside a 64 KB page.
  Files with that exact alignment make the surface tile visually. Files
  without that alignment produce non-tiling "static" — itself diagnostic.

### 19. Trigraph 3D

- **Technique** — For each consecutive triple `(byte[i], byte[i+1],
  byte[i+2])`, plot a point at the Cartesian coordinates `(a, b, c)` in the
  256³ cube. Accumulate into a per-screen-pixel count buffer; map count → log
  color via the heat palette. Wireframe bounding cube drawn for orientation.
  Rotatable, zoomable.
- **When valuable** — **3D extension of the digraph dot plot.** Even more
  powerful at fingerprinting — ASCII text clusters in a tetrahedron of the
  cube; structured binary forms striated planes; AES output uniformly fills
  the cube. Often reveals structure invisible in the 2D digraph.

### 20. Spherical Trigraph 3D

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

- **"What kind of file is this?"** → Digraph dot plot, Markov chord, byte
  histogram, trigraph 3D.
- **"Where are the regions / sections / boundaries?"** → Hilbert, byte-class
  stripe, entropy heatmap.
- **"What record / page / block size does this use?"** → Autocorrelation,
  toroidal 3D, helical 3D.
- **"Is there compressed / encrypted data, and where?"** → Entropy heatmap,
  strings density (as the inverse signal).
- **"Where are the strings / symbols?"** → Strings density, byte-class
  stripe.
- **"Are there repeated structures / embedded files?"** → Self-similarity,
  Hilbert.
- **"Bit-level structure / steganography?"** → Bit-plane view.
- **"Is there an image / framebuffer / raw bitmap embedded?"** → RGB raw.
- **"Just look pretty for a screenshot."** → Polar spiral, circular
  Hilbert, spherical trigraph 3D (with auto-rotate).

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
    ├── render.c     all 20 view implementations
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
