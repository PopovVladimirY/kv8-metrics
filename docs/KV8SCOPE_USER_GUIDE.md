# kv8scope User Guide

kv8scope is the Kv8 Software Oscilloscope -- a real-time and offline telemetry
visualisation application for data recorded through the Kv8 Kafka sink.  It
connects directly to a Kafka broker, discovers all Kv8 sessions, and renders
each counter as a per-panel waveform with live statistics.

---

## Table of Contents

1. [Prerequisites](#1-prerequisites)
2. [Kv8 and Kafka Setup](#2-kv8-and-kafka-setup)
3. [Starting kv8scope](#3-starting-kv8scope)
4. [Connecting to Kafka](#4-connecting-to-kafka)
5. [Sessions Panel](#5-sessions-panel)
6. [Scope Window](#6-scope-window)
   - [6.1 Counter List](#61-counter-list)
   - [6.2 Waveform Area](#62-waveform-area)
   - [6.3 Overview Strip](#63-overview-strip)
   - [6.4 Status Bar](#64-status-bar)
7. [Toolbar Reference](#7-toolbar-reference)
8. [Waveform Navigation](#8-waveform-navigation)
9. [Visualization Modes](#9-visualization-modes)
10. [Crossbar](#10-crossbar)
11. [Annotations](#11-annotations)
12. [Log Panel](#12-log-panel)
13. [Per-Counter Options](#13-per-counter-options)
14. [Statistics Panel](#14-statistics-panel)
15. [Themes](#15-themes)
16. [Fonts](#16-fonts)
17. [Configuration File](#17-configuration-file)
18. [Keyboard Shortcut Reference](#18-keyboard-shortcut-reference)

---

## 1. Prerequisites

| Requirement | Version |
|---|---|
| Kafka broker | 3.x (KRaft mode recommended) |
| Docker / Docker Compose | for the bundled broker setup |
| kv8scope binary | built from this repository |

---

## 2. Kv8 and Kafka Setup

### 2.1 Starting the local Kafka broker

The repository ships a ready-made Docker Compose configuration in `./docker/`.
It starts a single-broker Kafka instance in KRaft mode (no ZooKeeper) with
SASL/PLAIN authentication.

```sh
cd docker
docker compose up -d
```

The broker listens on `localhost:19092`.  To stop it without losing recorded data:

```sh
docker compose down
```

To stop and wipe all recorded topics:

```sh
docker compose down -v
```

### 2.2 Default credentials

| Field | Value |
|---|---|
| Brokers | `localhost:19092` |
| Security protocol | `SASL_PLAINTEXT` |
| SASL mechanism | `PLAIN` |
| Username | `kv8producer` |
| Password | `kv8secret` |

These match the credentials pre-configured in `docker/kafka-config/`.

### 2.3 Sending telemetry with kv8feeder

Use the bundled `kv8feeder` example to send live telemetry to Kafka:

```sh
./build/_output_/bin/kv8feeder \
    /KV8.brokers=localhost:19092 \
    /KV8.channel=kv8/my_channel \
    /KV8.user=kv8producer \
    /KV8.pass=kv8secret \
    --duration=30
```

The channel (`kv8/my_channel`) determines the Kafka topic namespace.
kv8scope discovers all sessions under all `kv8/` prefixes automatically.

By default kv8feeder publishes three scalar feeds:

| Feed | Rate | Description |
|---|---|---|
| `Numbers/Counter` | 100 Hz | Wrapping integer counter 0..1023 |
| `Numbers/Wald` | 10,000 Hz | Gaussian random walk -4000..4000 |
| `Scope/Phases` | 100,000 Hz | Piecewise-constant cyclogram -5..5 |

Add `--udt` to also start the two UDT (User-Defined Type) feeds:

```sh
./build/_output_/bin/kv8feeder \
    /KV8.brokers=localhost:19092 \
    /KV8.channel=kv8/my_channel \
    /KV8.user=kv8producer \
    /KV8.pass=kv8secret \
    --udt --duration=30
```

| UDT Feed | Rate | Description |
|---|---|---|
| `Environment/WeatherStation` | 1 Hz | Flat 7-field schema: temperature, humidity, pressure, wind speed/direction, rainfall, UV index |
| `Aerial/Navigation` | 1,000 Hz | Hierarchical: geodetic position, velocity, GPS/baro status |
| `Aerial/Attitude` | 1,000 Hz | Hierarchical: orientation quaternion, angular rate, acceleration |
| `Aerial/Motors` | 1,000 Hz | Flat 5-field schema: battery voltage (6..25.2 V) and 4 motor RPMs (0..12,000) |

The three Aerial feeds share a common Unix-epoch timestamp (`ts_ns`) so
kv8scope can correlate them on a common time axis.  Each UDT field is
exposed as an individual counter in kv8scope.

Rate overrides:

```sh
./kv8feeder --rate.counter=50 --rate.wald=5000 --rate.phases=50000
```

### 2.4 Sending telemetry with kv8probe

`kv8probe` is the bundled benchmark/test producer.  It writes to Kafka using
the same libkv8 API and is a convenient way to verify the full stack:

```sh
./build/_output_/bin/kv8probe \
    --brokers localhost:19092 \
    --prefix kv8/probe_test \
    --count 100000
```

---

## 3. Starting kv8scope

```sh
./build/_output_/bin/kv8scope
```

On first launch a default `kv8scope.json` is created in the working directory.
The application remembers its last connection settings, theme, font, and all
per-session counter configurations.

---

## 4. Connecting to Kafka

Open the connection dialog via **File > Connect...** (or press `Alt+F4` to
exit without connecting).

| Field | Description |
|---|---|
| Brokers | Comma-separated `host:port` list, e.g. `localhost:19092` |
| Security protocol | `PLAINTEXT`, `SASL_PLAINTEXT`, `SASL_SSL`, or `SSL` |
| SASL mechanism | `PLAIN`, `SCRAM-SHA-256`, or `SCRAM-SHA-512` |
| Username | SASL username |
| Password | SASL password |

Click **Test** to verify connectivity, then **OK** to confirm.  kv8scope
immediately begins polling Kafka for sessions.

Connection settings are persisted to `kv8scope.json`; they are restored on the
next launch.

---

## 5. Sessions Panel

The **Sessions** window lists every Kv8 session discovered on the broker.
Sessions are grouped by channel and sorted chronologically.

### Session table columns

| Column | Description |
|---|---|
| (dot) | Green = Online (live), grey = Offline (historical) |
| Session ID | Unique identifier in the form `YYYYMMDDTHHMMSSz-PPPP-RRRR` |
| Name | Human-readable session name from the registry |
| Counters | Total number of counters registered in the session |
| Groups | Number of counter groups |
| Started | Session start timestamp formatted as `YYYY-MM-DD HH:MM:SS` |

### Opening a session

Double-click a session row to open it in a Scope window.  Multiple sessions
can be open simultaneously in separate windows.

### Channel context menu

Right-click a channel header row for:
- **Delete channel** -- permanently removes the channel and all its sessions
  from Kafka after a confirmation prompt.

### Session context menu

Right-click an individual session row for session-level actions (delete, etc.).

---

## 6. Scope Window

Each open session gets its own **Scope** window with three main zones stacked
vertically:

```
+--------------------------------------------------+
|  Toolbar                                         |
+--------------------------------------------------+
|  Counter List  (resizable height, drag divider)  |
+--------------------------------------------------+
|  Waveform Area (stacked per-counter graphs)      |
+--------------------------------------------------+
|  Overview Strip (miniature full-session view)    |
+--------------------------------------------------+
|  Status Bar                                      |
+--------------------------------------------------+
```

### 6.1 Counter List

A scrollable table at the top of the window.  One row per counter, organized
into a collapsible tree that mirrors the counter name hierarchy:

- Counter names use `/` as a path separator (e.g. `cpu/load/user`).
- Each `/` level becomes a collapsible sub-folder row with its own aggregate
  V/E checkboxes.
- The channel group is the top level and is also collapsible.

#### Counter table columns

| Column | Description |
|---|---|
| Name | Counter path with tree indentation; hover abbreviated entries for full path tooltip |
| E | (Online sessions only) Enable/disable data collection from the KV8 sink |
| V | Visibility toggle -- show or hide this counter's trace in the graphs |
| N | Total sample count (full session) |
| Nv | Sample count within the current visible time window |
| Vmin | Minimum value in the visible window |
| Vmax | Maximum value in the visible window |
| Vavg | Mean value in the visible window |
| Vmed | Median value in the visible window |
| Min | Overall minimum (full session) |
| Max | Overall maximum (full session) |
| Avg | Overall mean (full session) |

All columns are resizable; widths are persisted per layout in `kv8scope.json`.

#### Row interactions

| Action | Effect |
|---|---|
| Left-click Name cell | Toggle highlight for this counter (also highlights the corresponding waveform trace) |
| Right-click Name cell | Open context menu (see Section 12) |
| Drag a Name cell | Reorder counters within the same group |
| Click V checkbox | Toggle trace visibility |
| Click E checkbox | Enable/disable realtime data collection (online only) |

Dimmed (faded) rows indicate hidden counters.
Highlighted rows are tinted blue and correspond to the highlighted trace in the
waveform.

### 6.2 Waveform Area

Up to eight stacked graphs are displayed.  Panel assignment follows this logic:

- **Up to 8 visible counters:** one counter per panel (no sharing).
- **More than 8 visible counters:** counters are sorted by their registered
  Y-range minimum, then sliced into contiguous runs of equal size across the
  eight panels.  Counters with similar Y ranges are adjacent after sorting and
  therefore naturally co-located in the same panel.

All graphs share a **single linked X axis**, so panning or zooming one panel
moves all panels simultaneously.  The X-axis tick labels and "Time" label
appear only on the bottom panel to avoid repetition.

The Y axis of each panel automatically adapts to the union of the registered
min/max ranges of all counters currently assigned to it.  When counters are
added, removed, or have their visibility toggled the Y scale resets to the
natural range of the new panel composition.  User Y-zoom (Shift+Scroll) is
preserved across visibility toggles as long as the panel composition is
unchanged.

Trace colors are validated for contrast against the current plot background on
every frame.  Colors that fall below a 3:1 WCAG contrast ratio are
automatically shifted toward white (dark themes) or black (light themes) until
the minimum ratio is met.  This guarantees readability on every theme without
changing the user-assigned palette colors permanently.

Custom Y ranges override the automatic range (see Section 12).

### 6.3 Overview Strip

A 44 px tall miniature rendering of the entire session appears below the
waveform area.  A semi-transparent rectangle marks the currently visible time
window.

- **Click or drag** anywhere in the strip to pan the main view to that position.

### 6.4 Status Bar

Displays the session prefix, online/offline status, total sample count, and the
current time cursor position when the crossbar is active.

---

## 7. Toolbar Reference

| Control | Description |
|---|---|
| `\|\|` / `>` | Pause / resume auto-scroll (live sessions only) |
| **Return to live** | Jump to the most recent data and re-enable auto-scroll |
| **Time window: N s** | Shows the configured realtime time window (set in `kv8scope.json`) |
| **Show All** | Make all counters visible |
| **Hide All** | Hide all counters |
| **[Stats]** / **[Stats ON]** | Toggle the Statistics panel (also Ctrl+I) |
| **[Log]** / **[Log ON N]** | Toggle the Log Panel (also Ctrl+L); badge shows record count |
| **S** | Switch all counters to Simple visualization mode |
| **R** | Switch all counters to Range visualization mode |
| **C** | Switch all counters to Cyclogram visualization mode |
| Crossbar button | Toggle the value crossbar overlay |

The active visualization mode button is highlighted.
Individual counters can override the global mode via their context menu.

---

## 8. Waveform Navigation

Navigation is mouse-driven.  All operations apply to the shared X axis and
propagate to every visible panel simultaneously.

| Action | Effect |
|---|---|
| Scroll wheel | Zoom in/out on the X axis at the cursor position |
| Shift + Scroll | Zoom in/out on the Y axis of the hovered panel only |
| Left-click + drag | Pan left/right along the time axis |
| Click in overview strip | Pan main view to clicked time position |
| Left-click on a trace | Highlight that counter (bidirectional with the counter list) |
| Ctrl + left-click in graph | Place a new annotation at the nearest sample to the cursor |

Y-axis zoom anchors the bottom of the scale to the counter's minimum value;
scaling applies to the range above it.

---

## 9. Visualization Modes

Three rendering modes are available globally (toolbar S/R/C) or per-counter
(right-click context menu).

### Simple (S)
A continuous line connecting sample midpoints.  When multiple samples fall on
the same horizontal pixel, the pixel shows their average value.

### Range (R) -- default
Like Simple, but when multiple samples share a pixel column a vertical
min/max bar is drawn in addition to the midpoint line.  This makes density
and variance immediately visible at high zoom-out.

### Cyclogram (C)
A zero-order-hold staircase: the value extends horizontally to the right until
the next sample, then steps vertically to meet it.  Useful for discrete or
enum-like counters.

Individual sample dots (hollow circles, or solid when multiple samples overlap)
are always rendered regardless of mode.

---

## 10. Crossbar

When the crossbar is enabled (toolbar button or keyboard shortcut) and the
mouse cursor is over any graph panel, vertical and horizontal hairlines are
drawn across all panels at the cursor X position.

A tooltip near the cursor shows:
- The timestamp at the cursor.
- For each visible counter in the panel: counter name, current value (or
  min/max range when multiple samples share that pixel column).

The crossbar is drawn in non-hovered panels as a thin line at the last known
X position so the time reference is always visible.

---

## 11. Annotations

Annotations mark specific moments in time with a title and description.  They
are stored persistently in a dedicated Kafka topic
(`<channel>.<sessionID>._annotations`) alongside the telemetry data and are
available whenever the session is opened.

### Adding an annotation

| Method | Action |
|---|---|
| Ctrl + left-click in the waveform | Opens the editor at the nearest sample to the cursor |
| Ctrl + N | Opens the editor at the centre of the current visible window |

The editor dialog fields:
- **Type** -- bookmark, event, warning, or error.
- **Title** -- short label (shown on the graph as a flag marker).
- **Description** -- free-form text.

Click **Save** to publish to Kafka.  Click **Cancel** to discard.

### Viewing annotations

- Annotation flags are rendered as vertical lines with labels directly on the
  waveform panels.
- Open the **Annotation Panel** with Ctrl+Shift+N.  The panel lists all
  annotations in chronological order; clicking an entry navigates the waveform
  to that point in time.

### Editing or deleting an annotation

Click an annotation flag on the waveform, or select it in the Annotation Panel,
to open the editor in edit mode.  Use the **Delete** button to remove it.

### Toggling annotation visibility

Use the annotation visibility toggle in the Annotation Panel header to show or
hide all flags on the waveform without deleting them.

---

## 12. Log Panel

The Log Panel is a dockable trace-log viewer that runs alongside the
waveforms in every Scope window. It receives every `Kv8LogRecord` published
to the session's `_log` topic by applications instrumented with `kv8log`
(see [KV8LIB_API_REFERENCE.md, section 5](KV8LIB_API_REFERENCE.md#5-kv8log----trace-logging-api)).

### Opening the panel

| Method | Action |
|---|---|
| Toolbar **[Log]** / **[Log ON N]** | Toggle the panel; the count badge shows total records currently in the panel for that session. |
| **Ctrl + L** | Same as the toolbar button while a Scope window is focused. |

### Layout

The panel is a multi-column table; each row is one trace record:

| Column | Description |
|---|---|
| Time | Wall-clock timestamp of the record (microsecond precision). |
| Lvl | Severity badge: `DEBUG`, `INFO`, `WARN`, `ERROR`, `FATAL`. Colour-coded. |
| Thread | OS thread ID at the moment of emission. |
| Site | `<basename>:<line> <function>` -- resolved from the channel registry. |
| Message | UTF-8 payload formatted by the producer. |

WARN, ERROR, and FATAL rows additionally tint their full row background
(amber, red, magenta respectively) so severity events are visible at a
glance even when the panel is dense.

### Toolbar controls

| Control | Description |
|---|---|
| **D / I / W / E / F** check boxes | Severity filter -- shown checked by default. Unchecked levels are hidden but still consumed and stored, so re-enabling does not require a Kafka seek. |
| Filter text box | Substring match against the formatted message. Empty = no filter. |
| **Follow** check box | When enabled, auto-scrolls the table to the newest record. Click any row to disable Follow automatically. |

### Timeline integration

- Records inside the waveform's currently visible X range are highlighted
  with a brighter row background. Pan or zoom the waveform and the
  highlight follows.
- Click a row to seek the waveform timeline to that record's timestamp.
  The waveform centres on the chosen moment without changing zoom.
- Conversely, scrolling the waveform updates the highlight band in the
  Log Panel so the two views stay synchronised.

### Storage and lifetime

- Records are kept in an in-memory ring inside `LogStore`. Old records are
  evicted once the ring fills; the configured capacity is large enough for
  typical sessions but is not unbounded.
- The site registry (file / line / function / format string per call site)
  lives at the channel level, not the session level: opening a session in
  kv8scope replays the channel `_registry` topic from offset 0 so every
  record's site is resolved even when historical sessions are reopened.

---

## 13. Per-Counter Options

Right-click a counter row in the Counter List to open its context menu:

| Item | Description |
|---|---|
| Highlight / Clear highlight | Toggle the bidirectional highlight link with the waveform trace |
| Set Y range... | Open a dialog to enter custom Y min and Y max values |
| Reset Y range | Restore the Y range to the registry-defined min/max |
| Change color... | Open a color picker to set a custom trace color |
| Reset color | Restore the palette-assigned color |
| Visualization mode > Simple | Use Simple mode for this counter only |
| Visualization mode > Range | Use Range mode for this counter only |
| Visualization mode > Cyclogram | Use Cyclogram mode for this counter only |

Custom Y ranges, colors, and per-counter visualization modes are persisted to
`kv8scope.json` keyed by session prefix and counter name.

---

## 14. Statistics Panel

Open the Statistics Panel with **[Stats]** in the toolbar or **Ctrl+I** while
a Scope window is focused.  The panel is a floating window and can be
repositioned freely.

It shows full-duration aggregate statistics for every counter:
- Total sample count
- Global minimum, maximum, and mean
- Per visible-window statistics: sample count, min, max, mean, median

These values come from the StatsEngine accumulator which processes every
ingested sample, independent of the visible time window.

The left column (group/counter name) and the right column (statistics values)
are both resizable by dragging the column divider.  The left column defaults to
15% of the panel width.  Column widths are persisted to `kv8scope.json`.

---

## 15. Themes

Select a theme via **View > Theme**.  The choice is saved to `kv8scope.json`
and restored on next launch.

| Theme | Character |
|---|---|
| **Night Sky** | Very dark blue-black background, bright blue accent |
| **Sahara Desert** | Deep warm-brown background, amber accent |
| **Ocean Deep** | Dark navy background, cyan accent |
| **Classic Dark** | Neutral dark gray background, green accent |
| **Morning Light** | Warm cream background (light), dark amber text |
| **Blizzard** | Ice-blue background (light), navy text |
| **Classic Light** | White background (light), Office-style blue accent |
| **Paper White** | Off-white background (light), neutral grayscale UI |
| **Inferno** | Deep red-orange dark theme |
| **Neon Bubble Gum** | High-saturation magenta-cyan dark theme |
| **Psychedelic Delirium** | Maximum contrast multi-color dark theme |

Trace and overlay label colors are automatically adjusted for each theme: the
palette is validated against the plot background on every frame and shifted
toward white or black as needed to maintain a minimum 3:1 contrast ratio.

---

## 16. Fonts

Select a font and size via **View > Font**.  Changes take effect immediately
and are saved to `kv8scope.json`.

| Font name | Description |
|---|---|
| **Sans Serif** | System default sans-serif (Segoe UI on Windows, Ubuntu/Noto on Linux) |
| **Arial Nova** | Clean humanist sans-serif |
| **Aptos Semibold** | Office-style semibold, very readable at small sizes |
| **Classic Console** | Monospace (Consolas on Windows, DejaVu Mono on Linux) |

Available sizes: 13, 15, 17, 19, 22, 26 px.  If a preferred font file is not
found on the system, kv8scope falls back gracefully to the next available
option.

---

## 17. Configuration File

`kv8scope.json` is read from and written to the working directory.  It is
created automatically on first launch.  Most settings are managed through the
UI but the file can be edited by hand to set defaults.

Key fields:

```jsonc
{
  // Connection
  "brokers":            "localhost:19092",
  "security_protocol":  "SASL_PLAINTEXT",
  "sasl_mechanism":     "PLAIN",
  "username":           "p7producer",
  "password":           "p7secret",
  "session_poll_ms":    2000,           // broker polling interval

  // UI
  "theme":              "night_sky",
  "font_face":          "sans_serif",
  "font_size":          15,
  "default_time_win":   15.0,           // seconds shown in offline view
  "realtime_time_win":  15.0,           // seconds shown in live view
  "max_points_per_trace": 500000,       // cap per-counter sample buffer
  "viz_mode":           "range",        // "simple" | "range" | "cyclogram"
  "cross_bar":          true,

  // Per-session counter overrides (managed automatically)
  "counter_viz_modes":  { ... },
  "table_column_widths": { ... },
  "recent_sessions":    [ ... ]
}
```

To reset all settings to defaults, delete or rename `kv8scope.json` and
restart.

---

## 18. Keyboard Shortcut Reference

| Shortcut | Action |
|---|---|
| Ctrl + I | Toggle Statistics panel |
| Ctrl + L | Toggle Log Panel |
| Ctrl + N | Add annotation at center of visible window |
| Ctrl + Shift + N | Toggle Annotation panel |
| Ctrl + left-click (in graph) | Add annotation at clicked sample |
| Alt + F4 | Exit |
