# Show More / Show Less animation — research and design

Status: research complete, plan agreed, implementation not started.
Scope: the reveal animation of the Application Network Activity panel only. No
change to monitoring, limiting, schedules or data sources.

## 1. The problem, stated precisely

The panel's reveal has been rebuilt several times and each attempt drew a
different complaint:

| Report | Cause found |
|---|---|
| "it pushes" | the whole dashboard canvas re-laid out on every frame, because the tween animated `maximumHeight` on a widget inside a `QScrollArea` |
| "it lags" | a `QGraphicsOpacityEffect` over the panel forced Qt to render the whole subtree (table included) offscreen and composite it every frame |
| "the graph goes down and up" | the viewport was scrolled *twice*: once by the growing panel pushing content, then again by a corrective glide at the end |
| "pushes it to top" | the corrective glide targeted the panel bottom, dragging the top row out of view |
| "first time it's very long" | the reveal measured its height target before any rows existed, and the later correction could only *raise* it |

Every one of those was a real defect. What remains is judged by feel, so this
document sets out what the reference material says the motion *should* be,
before touching it again.

## 2. What the reference material says

### 2.1 Qt: animating a property

From the Qt 6 Animation Framework overview:

- An animation interpolates a **property** between a start and end value; the
  target must be a `QObject` with a settable property.
- A `QPropertyAnimation` performs **linear interpolation** between the values and
  uses an **easing curve** to reshape *time*, not the path:
  "you can also choose an easing curve to control the speed of interpolation
  between 0 and 1, without changing the path."
- The default duration is **250 ms**.
- Ownership: "A QPropertyAnimation should always have a parent that controls its
  lifespan."
- All animations are driven by one **global timer** that sends time updates.

Two consequences for this panel:

1. The curve controls *how the height is distributed over time*. If the curve
   front-loads, the first frame is a large jump — the measured "first frame
   covered ~50 px of a 514 px travel" with `OutCubic`.
2. Since one global timer drives everything, **all** animations on screen tick
   together. Anything else animating during the reveal (a scroll glide, a
   progress bar) competes for the same frame budget.

### 2.2 Qt: easing curve semantics

From the `QEasingCurve` documentation:

| Curve | Description (verbatim from Qt) |
|---|---|
| `OutQuad` | quadratic: **decelerating to zero velocity** |
| `InOutSine` | sinusoidal: **acceleration until halfway, then deceleration** |
| `OutSine` | sinusoidal: **decelerating to zero velocity** |
| `InOutQuad` | quadratic: acceleration until halfway, then deceleration |

The key property: `valueForProgress()` returns the *effective* progress. An
ease-out curve starts at a velocity well above its average, because it must cover
the whole distance while decelerating. An ease-in-out curve starts at **zero**
velocity.

For a reveal that should read as one motion, `InOutSine` is the documented fit:
it begins at zero velocity, so the first displayed frame moves almost nothing and
the eye never catches a discontinuity at the start.

### 2.3 Qt: what makes a widget animation expensive

- `QWidget::setUpdatesEnabled(false)` exists to suppress repaints during a batch
  of changes, which is the documented tool when many geometry updates would
  otherwise each trigger a paint.
- `updateGeometry()` / `sizeHint()` are what layouts consult; changing a
  constraint that alters `sizeHint` invalidates the *parent* layout, which is why
  animating `maximumHeight` re-flows the entire scroll canvas every frame.
- A `QGraphicsEffect` is applied by compositing the widget through an offscreen
  buffer, so it costs a full subtree render per frame.

## 3. Design rules derived from the above

These are the rules the implementation must satisfy. They exist because each was
violated by a previous attempt.

1. **One motion per gesture.** The reveal is the panel's height and nothing else.
   The viewport does not move while the panel is animating, so there is no second
   motion to fight or correct.
2. **Zero-velocity start and end.** Use an ease-in-out curve so neither the first
   nor the last frame carries a visible step.
3. **Pin the container during the tween.** Setting `minimumHeight` and
   `maximumHeight` together stops the parent layout from re-flowing other cards
   on every frame — only the panel's own box changes.
4. **No graphics effect over the panel.** The fade must not force a per-frame
   offscreen render of the table. If a fade is wanted, it must be scoped to the
   tween and removed at the end, or avoided.
5. **The measured target must be the final target.** Measure the height after the
   content that will be shown has been laid out, not before; and allow the
   correction to move in *both* directions.
6. **Nothing else animates during the reveal.** No scroll glide, no paged
   navigation, no progress sweep — they share the global animation timer.
7. **Interruption is normal.** A second click mid-tween must retarget from the
   *current* height, not from a stale endpoint, and must not leave the pin set.

## 4. Candidate designs

| # | Design | How it feels | Risk |
|---|---|---|---|
| A | Pure height unroll, `InOutSine`, ~280 ms, no fade, no settle | calm, single motion, nothing moves but the panel | reads as plain |
| B | A + content fade scoped strictly to the tween | "materialises" | the effect costs an offscreen render per frame *while it runs* |
| C | A + 8 px settle (content rises into place) | composed | a second simultaneous motion; previously reported as a wobble |
| D | No height animation: reserve space instantly, fade content in | crisp | the card below jumps in one frame |

**Chosen: A.** It is the only candidate that satisfies rules 1, 2, 4 and 6 with
no compensating mechanism. B and C were both tried and both were reported as
problems: C is the "wobble", B carries a per-frame cost that is exactly the
"lags" defect from the history above.

The visible difference from today's build: the fade and the 8 px settle are
removed, the curve changes from `OutQuad` (decelerating from a fast start) to
`InOutSine` (zero velocity at both ends).

## 5. Plan

1. **Panel** (`appactivitypanel.cpp`)
   - easing: `OutQuad` → `InOutSine`
   - duration: 220 ms → 280 ms (InOutSine spends its early frames at low speed,
     so the same duration would feel slower to commit)
   - remove the fade animation and its opacity effect entirely, including the
     effect's creation, the `m_fadeAnim` member and the `finished` cleanup
   - remove the settle animation and its margin bookkeeping
   - keep: the pinned min/max height (`animHeight`), the both-directions height
     correction, the source pinning, and the `revealTick` / `revealFinished`
     signals (the dashboard still uses `revealFinished` for the collapse
     bring-back glide)
2. **Dashboard** (`mainwindow.cpp`)
   - unchanged: no viewport motion during the reveal; one minimal glide after a
     collapse only, if the toggle left the viewport
3. **Verification** (`tools/measure_anim3.cpp`, extended)
   - assert the height timeline is monotonic with no step above a threshold
   - assert the first and last frames move less than the mid-tween frames (the
     signature of an ease-in-out curve)
   - assert the panel's top edge and a neighbouring card's x never move
   - render frames at 20 ms intervals for visual comparison
4. **Regression protection**
   - keep `test_panel_geometry` (first-open height == second-open height)
   - add the timeline assertions to a runner so a future curve change is caught
5. **Ship**
   - all suites green, deploy, and the user judges the feel on the real app

## 6. What is explicitly out of scope

- `AppMonitor` and every data source: unchanged. Monitoring currently works and
  was verified by the user; nothing in this work touches it.
- The limiter, the scheduler, the per-app limiter: unchanged.
- The collapse animation keeps its current duration; only the reveal is reshaped.

## 7. Sources

- Qt 6, *The Animation Framework* — https://doc.qt.io/qt-6/animation-overview.html
- Qt 6, *QPropertyAnimation Class* — https://doc.qt.io/qt-6/qpropertyanimation.html
- Qt 6, *QEasingCurve Class* — https://doc.qt.io/qt-6/qeasingcurve.html
- Qt 6, *QWidget Class* (`setUpdatesEnabled`, `updateGeometry`, `sizeHint`) —
  https://doc.qt.io/qt-6/qwidget.html

## 8. Second pass: the transient scrollbar (diagnosed by the user)

The reveal became smooth in a small window, but two defects remained.

### 8.1 The column headings slid sideways

The user identified the cause directly: when the panel opened, the table showed its
own vertical scrollbar for a moment and then it vanished. A scrollbar occupies width,
so its appearance narrowed the table viewport and its disappearance widened it again
- sliding "Application / Download / Upload / Total" right and then left, and forcing
the section to re-measure.

The code had `ScrollBarAsNeeded` with a comment claiming it "reserved the scrollbar
inside the table". It did not: reserving happens only with `AlwaysOn`. The theme sets
the scrollbar to 10px, which is exactly the size of the shift that was seen.

Fix: `ScrollBarAlwaysOn`. The 10px is now reserved for the table's whole life, so the
viewport width cannot change and nothing slides.

Guarded by `tools/test_table_stability.cpp`, which asserts at two window sizes that
the viewport width, the column-width total and the rightmost column's x are all
constant, and that the scrollbar never *vanishes* while the table is shown. That test
also fails loudly if the table is never visible: an earlier version compared empty
ranges and passed without measuring anything, which is worse than a failure.

### 8.2 The graph is still pushed in a maximized window

NOT yet reproduced, and therefore NOT yet fixed. Measurements so far:

| Harness | Result |
|---|---|
| real ResponsivePair + real panel, panel below the pair, 4 window sizes | graph card moves 0px, viewport scrolls 0px |
| same, panel embedded in the pair | graph card moves 0px |
| forced re-parent mid-tween (what orientationChanged does) | still 0px |

So the panel's own tween, the trailing stretch, the pair's orientation logic and
mid-reveal re-parenting are each ruled out as the cause *in isolation*. What the
harnesses still lack: the real dashboard's content sizes, the live graph repainting
during the reveal, and the app's own 1-second refresh timer firing mid-tween.

The next step is to instrument the REAL window rather than another harness: log the
graph card's geometry and the scroll value on a timer while the panel opens in a
maximized window, and read which number actually moves.

## 9. Design rules added by this pass

10. A transient scrollbar is a layout change. Any scrollbar inside an animated region
    must reserve its space permanently, or its appearance will move everything beside
    it.
11. A test that compares empty ranges passes vacuously. Every geometry test must first
    assert that the widget it measures was actually visible.
