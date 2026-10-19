# Why the reveal was not smooth, and what actually fixed it

## What the Qt documentation establishes

Two statements from the Qt 6 documentation shaped this investigation:

1. *"Behind the scenes, all animations are controlled by a global timer, which sends updates about
   all animations that are running."* — The Animation Framework overview.

   So frame pacing is shared. Anything else scheduled on that same event loop competes with the
   animation directly; there is no separate animation thread to absorb the work.

2. `QPropertyAnimation` interpolates a property through its setter, which for a widget property
   means a layout pass whenever the value participates in a layout.

   So animating a height is only cheap if the thing being resized does not re-lay-out its children
   on every frame.

## What the measurements said (and did not say)

A probe was written that samples the reveal directly: the interval between height ticks, and how
many times the table inside the panel is resized during the motion.

```
frames sampled   : 20
table resizes    : 1     ← NOT layout thrash: the layout ran once for the whole motion
frame gap median : 17 ms
frame gap worst  : 30 ms
frames over 20ms : 4 of 19
```

This **contradicted the working theory** — that per-frame relayout was the cost. The layout was
already cheap. The problem was that individual frames were arriving 20–30ms apart while the median
was a clean 17ms: the motion was stuttering, not heavy.

A second measurement answered where: the long frames clustered at the END of the reveal.

## The three causes, in order of impact

### 1. The application never asked Windows for a fine timer (the real one)

Windows schedules timers on a **15.6ms grid** by default. A ~60fps animation wants a tick every
16.7ms, so it receives either one grid tick (17ms) or — when the request lands just past the
boundary — two (30ms). That is exactly the alternating pattern measured.

**The animation test harness had always called `timeBeginPeriod(1)` itself**, which is why the
suite reported a smooth reveal while the application felt rough. The harness was measuring a
machine state the application never creates — the test was hiding the defect, not catching it.

Fix: the panel now holds a 1ms timer resolution for the lifetime of a tween and releases it
immediately afterwards, balanced by a counter so overlapping tweens cannot release it early. It is
held for ~340ms at a time, not permanently, so the system-wide power cost is bounded.

### 2. The last frame did the finishing work

The completion handler rebuilt every row on the same frame that drew the end of the motion. The
rows had already been filled before the tween started, so that rebuild was redundant — and it was
paying for formatting every cell inside the final frame. It is now deferred one event-loop turn,
after the final frame is presented.

### 3. The periodic refresh ran during the tween

The 1-second refresh and the 3-second rank timer both rebuilt the table whenever the panel was
expanded, with no check for a tween in flight. `isAnimating()` is now exposed and both callers
respect it; the monitor tick is deferred too, which costs nothing because bytes accumulate under
their own lock and the next tick finalises the same interval.

The reveal also fills its content and settles the layout *before* the first animated frame, so
frame one is not simultaneously measuring and positioning every row.

## Result

```
frames sampled   : 19   (5 consecutive runs)
frame gap median : 17 ms
long-frame positions: #18 only  (the final boundary frame, where motion is 1px)
mid-motion stalls: none
```

Peak step size also fell from 17px to 15px per frame by moving the reveal from 280ms to 340ms.

## What is still not verifiable from here

The probe runs against staged Qt DLLs in a test directory, and the reveal it measures has six rows
rather than whatever a real machine shows. Frames on a real desktop, with a compositor and a live
graph redrawing nearby, may still differ. The measurements above prove the mechanism was wrong and
is now right; they do not prove the feel on the user's monitor.
