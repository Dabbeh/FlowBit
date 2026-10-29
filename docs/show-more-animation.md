# Show More animation

## Pending resize fix

The full dashboard regression reproduced an immediate 283-pixel jump when
opening after the window had been resized while the panel was hidden. The
previous checks watched the graph and missed the activity panel and lower cards.

`show()` delivers the panel's pending resize event synchronously. Setting
`m_expanded` first allowed `resizeEvent()` to set the full frame height before
`animateToHeight()` read its starting value. The animation then had no distance
left to travel. `setExpanded()` now keeps the collapsed state while `show()`
delivers that resize, then measures the content and starts the reveal from the
preserved visible height. Reversals still preserve the running timeline.

## Card shaking and frame cadence

The card's top edge was observed moving from y=343 to 340, then 337, then back
to 343 during expansion at 1536x816. The graph did not move and the scroll value
remained zero. The canvas was being enlarged while the panel was already growing,
temporarily leaving the layout too little room and squeezing its spacing.

The dashboard now reserves the larger endpoint's canvas extent before starting
the reveal and activates its layout. Its trailing stretch absorbs spare space.
The reserve is released when the transition finishes. The card's top edge stays
anchored instead of depending on a later scroll-area resize to restore spacing.

## Narrow-window layout

The user confirmed the wide-window animation was smooth, but still saw a small
shake in narrow windows. In that orientation the activity panel sits inside
ResponsivePair between Live Speed and Usage, adding a second layout level.

Sampling positions only after `processEvents()` missed this defect. A paint
event observer reproduced the card alternating between y=343 and y=337-342
while opening, and y=343 and y=345-349 while closing. The inner layout could
paint using its previous container height before the canvas processed the new
size hint. Six narrow sizes failed; the wide sizes stayed steady.

Every reveal tick in stacked mode now activates the inner layout, propagates
ResponsivePair's new size hint, and then activates the canvas layout before
painting. This order matters: activating the inner layout after the canvas
could still leave an occasional two-pixel displacement near the breakpoint.
The wide mode keeps its existing path. Tests record positions at
paint events as well as between event-loop passes, so the transient bad frames
are no longer invisible to the regression checks.

Launching directly into the narrow layout also exposed a separate 258-pixel
scroll jump when the focused Show Less footer disappeared. Qt transferred focus
into the next card and scrolled it into view. Collapse now returns focus to the
Show More opener before animation, preserving the current scroll value so the
scroll return remains on the reveal timeline.

## Rendering and timing

The transition keeps the live table at a fixed full content height and animates
only its enclosing clipping frame. No pixmap is substituted and no live content
is hidden and restored during movement. Height-only frames do not measure the
table or resize its columns.

A local precise QTimer uses elapsed time to advance a 360 ms sine curve. It
updates at roughly 120 Hz on ordinary displays and follows faster display rates
up to 240 Hz. It is not a vsync guarantee. The timer and the temporary Windows
timer-resolution request stop at completion, so an idle panel has no animation
clock running. Reversals preserve the current timeline position.

The table, waiting message and error message share a stacked body. Switching
states no longer removes the table's space. The collapsed frame retains a
zero-height layout slot, so hiding it does not remove a layout gap in the final
frame.

The same progress drives any necessary scroll return. There is no separate
scroll animation after collapse. User scrolling cancels the automatic scroll
adjustment.

## Qt references

- [Layout management and geometry costs](https://doc.qt.io/qt-6/layout.html#further-notes)
- [Widget painting and double buffering](https://doc.qt.io/qt-6/qwidget.html#paintEvent)
- [Precise timer accuracy](https://doc.qt.io/qt-6/qtimer.html#accuracy-and-timer-resolution)
- [Hidden widgets receive pending resize events before being shown](https://doc.qt.io/qt-6/qwidget.html#size-prop)
- [Qt 6.10 layout activation and parent geometry notification](https://github.com/qt/qtbase/blob/v6.10.0/src/widgets/kernel/qlayout.cpp#L896-L1040)

## Verification

The full-dashboard checks include the frame before the click and measure the activity panel, limiter
and scheduler, alongside the graph. It checks first opening, opening after
resizing, collapse, reversal in both directions, and collapse from the focused
footer while scrolled. Window sizes: 880x650, 880x900, 1000x650, 1140x650,
1160x650, 1180x650, 1200x760, 1536x816 and 1920x1040. The first launch is also
in the narrow 880x650 layout. Scroll-relative card position is checked at paint
time during the footer-driven collapse.

The final full-dashboard run passed all 234 checks with zero failures. The user
also confirmed that the isolated small-window preview was smooth on their display.

The earlier pending-resize fix removed a 283-pixel opening step. The new canvas
reservation and synchronous nested layout remove the smaller up/down displacements
reproduced above. The tested sizes report zero top-edge movement and no backtracking. The live table
remains visible at the same position throughout both directions. The separate
timer suite measured an 8 ms median and 9 ms worst interval between changing
heights, with a 10-pixel largest step, gradual acceleration/deceleration and no
overshoot (previously 16-17 ms and 22 pixels).

The fixture uses a unique temporary data directory and disables update checks,
so saved statistics and update dialogs cannot contaminate later runs.

These are offscreen regression measurements, not a guarantee of frame pacing
under every desktop load. An isolated native window titled
`Flowbit - Small Window Preview` is available for checking the real display
without installing. A static screenshot does not establish perceived smoothness.
