# Research notes: animations, icons, and Windows shell identity

Sources consulted before implementing (per the user's instruction to research
rather than improvise).

## 1. Qt animations

Source: <https://doc.qt.io/qt-6/qpropertyanimation.html> (Qt 6.12 docs)

Facts taken from the documentation:

* `QPropertyAnimation` interpolates **Qt properties**; the target must be a
  `QObject` and the property must have a setter.
* **Default duration is 250 ms** if unset.
* If no start value is set, the animation begins from the property's current
  value at the time it is created - which is why a start value must be set
  explicitly when the current value is not the intended origin.
* `QParallelAnimationGroup` is the documented way to run several animations
  together (the docs point to it for "complex animations ... contain several
  objects").
* Connecting to `QVariantAnimation::finished` is the supported way to run
  completion logic (hide a widget, clear an effect).

**Applied in this work:** the Show More / Show Less reveal animates
`maximumHeight` and a `QGraphicsOpacityEffect` **in parallel** via
`QParallelAnimationGroup`, rather than a single property tween. A height-only
tween inside a scroll area is largely invisible because the layout absorbs the
change; the opacity component is what makes the motion perceptible. See
`appactivitypanel.cpp`.

## 2. Windows icon and Start-menu identity

Source: <https://learn.microsoft.com/en-us/windows/win32/shell/appids>
(Application User Model IDs)

Facts taken from the documentation:

* An application should declare an **explicit AppUserModelID**
  (`CompanyName.ProductName[.SubProduct][.Version]`, max 128 chars, no spaces,
  pascal-cased).
* When there is an explicit AppUserModelID, **the same ID must be applied to the
  running process, all its windows, and its shortcuts**. Otherwise the taskbar
  and Start menu cannot associate the window with the shortcut, and the icon
  shown can be wrong or generic.
* The process-level ID is set with **`SetCurrentProcessExplicitAppUserModelID`**,
  and the documentation states it must be called **during initial startup, before
  the application presents any UI**.
* The shortcut carries the ID as the `System.AppUserModel.ID` property, and the
  shortcut's own icon comes from the executable's icon resource.

**Applied in this work:**

* `SetCurrentProcessExplicitAppUserModelID` is called first thing in `main()`,
  before any window is created.
* The icon resource is renamed to **`IDI_ICON1` (ordinal 1)**, which is the
  conventional identifier the shell uses to find an executable's icon. A *named*
  resource is not located by that lookup, which is why the icon appeared in
  Explorer but not in the Start menu.
* A launcher shortcut with the matching AppUserModelID and an explicit icon is
  created for the Start menu, rather than relying on the shell's heuristics.

## 3. Time input for schedules

Nothing in the app's previous schedule dialog followed a documented convention;
the friction the user reported (typing `21:00` and not `21`, 24-hour only,
awkward day layout) is a usability defect rather than an API one. The fix follows
the platform norm:

* Accept **both 12-hour and 24-hour** entry, with `AM`/`PM` written or omitted.
* Accept `21`, `21:00`, `21:30`, `9pm`, `9:30 pm`, and `9 30 pm`.
* Provide **presets** (common windows such as 20:00-21:00) so the common case is
  one click rather than typing.
* Keep day selection as **individual toggle buttons laid out in a fixed grid**
  with equal widths, so the labels fit at any window size.
