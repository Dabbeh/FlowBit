# Plan: scrollbars, and the Settings card polish

## What was asked

1. Make the scrollbar "much better" — the current one is bad.
2. Add smooth animation to it.
3. Tidy the Settings card: the checkboxes should be obvious, and the black does not sit right.

## What the current scrollbar is

```css
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: %(strong)s; border-radius: 5px; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: %(muted)s; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
```

Three faults, and the Qt documentation names the first directly:

> Note that if one property or sub-control is customized, all the other properties or
> sub-controls must be customized as well.
> -- Qt Style Sheets Examples, Customizing QScrollBar

- **`add-page` and `sub-page` are never styled.** These are the track above and below the
  handle. Left unset while the handle is styled, they fall back to platform drawing, which on
  this build renders as an unthemed block that does not match the card it sits in.
- **The bar is 10px wide with no track**, so it reads as a floating grey pill rather than a
  scrollbar. There is nothing to show how much of the page is visible.
- **No hover expansion and no visual change while scrolling**, so it is hard to grab and gives
  no feedback that it is active.

A fourth, subtler one: `sub-line` and `add-line` are given a height but the horizontal bar's
are not, so the two orientations are styled inconsistently.

## What animation can and cannot do here

Qt style sheets are static. There is no transition property, and no way to animate a QSS value
from the stylesheet itself.

So a smooth scrollbar has to come from code. There are three things worth animating, and they
are different in kind:

| Effect | Mechanism | Cost |
|---|---|---|
| Handle fades in on hover, out when idle | `QGraphicsOpacityEffect` + `QPropertyAnimation` | One offscreen render while animating |
| Handle widens on hover | Animated `QSS` via a dynamic property | A stylesheet re-polish per frame |
| Smooth scrolling to an offset | `QPropertyAnimation` on the `value()` property | None |

The third is the one that actually improves using the app. The first is what makes it feel
alive. The second is the one to avoid: re-polishing a stylesheet every frame is expensive and
this project has already been bitten by per-frame cost.

**A note on `QGraphicsOpacityEffect`.** This project learned earlier that
`QWidget::setParent()` deletes a `QGraphicsEffect`, and that an effect costs a full offscreen
subtree render per frame. A scrollbar handle is a tiny widget, so the cost is negligible here -
but the effect must be owned carefully, and the animation must stop when it finishes rather
than running forever.

## The plan

### Phase 1 — Style every sub-control

- Give `add-page` and `sub-page` an explicit transparent background, so the track is the card's
  background rather than platform drawing.
- Widen the bar to 12px with 2px of inset, so the handle does not touch the card edge.
- Style the handle with a radius that suits its width, and give it a clear hover and pressed
  state.
- Do the same for the horizontal bar, which is currently inconsistent.

### Phase 2 — A scrollbar that can be seen and grabbed

- The track becomes faintly visible when the pointer is over the scroll area, and transparent
  otherwise. A permanent track competes with the content; an invisible one is hard to hit.
- The handle is wider while hovered or dragged.

### Phase 3 — Smooth animation

1. **Smooth scroll to an offset.** A `QPropertyAnimation` on the scrollbar's `value`. The
   existing `m_dashScrollAnim` in `mainwindow.cpp` already does this for the dashboard, so the
   pattern is proven in this codebase - this is the same idea applied where the settings page
   scrolls.
2. **Fade on hover.** The handle's opacity rises while the pointer is over the scroll area and
   falls when it leaves, with the animation stopped at each end so nothing runs idle.

### Phase 4 — The Settings card

- **Checkboxes need a tick.** A filled square alone is ambiguous on a dark card, and the two
  states differ only in brightness. A drawn tick makes them unmistakable.
- **"The black does not sit there"** — the `note` label style used for the hint lines and the
  error text is being rendered in a colour that reads as a black block against the card. Give
  the notes a proper muted tone on the card's own background, and give the error its own
  treatment rather than sharing the note style.
- **The error text currently pushes the layout.** `Could not check: ...` sits inline beside the
  button and wraps to three lines, breaking the card's rhythm. It moves below the row.
- **Align the controls.** The two checkboxes sit hard against the right edge while the combo has
  a fixed width, so the column of controls is ragged. Give them a consistent right margin.

### Phase 5 — Verify

- Render the Settings page and look at it. Two bugs this session were found only by looking at
  an image after the measurements said the layout was fine.
- Render with the pointer simulated over the scroll area, to confirm the hover states.
- Re-run `test_startup_card` and `test_settings_roundtrip`, which both touch this card.

## Order

Phase 1 and 2 are stylesheet only and cannot break behaviour. Phase 3 touches code, so it comes
after the appearance is settled. Phase 4 is independent and can be done at any point.

Phase 5 is not optional. This session has repeatedly produced confident measurements of the
wrong thing, and the screenshot is what caught it each time.
