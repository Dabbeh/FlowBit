#include "schedulecard.h"

#include "daypicker.h"
#include "theme.h"
#include "units.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyle>
#include <QVBoxLayout>

namespace net {

namespace {

// A rate rendered the way the reference shows it: "2 MB/s", "512 KB/s", and
// "Unlimited" rather than a bare 0. formatRate already implements exactly that
// convention, so the card does not re-invent it.
QString rateText(RateBps rate)
{
    return formatRate(rate, /*zeroMeansUncapped=*/true);
}

// Minutes until this rule next starts. Negative when it is already running.
// Returns -1 when the rule never matches, so the caller can omit the line rather
// than print a fabricated countdown.
int minutesUntilStartImpl(const Rule &rule, const QDateTime &from)
{
    const QTime start = parseClock(rule.from);
    if (!start.isValid())
        return -1;

    for (int dayOffset = 0; dayOffset <= 7; ++dayOffset) {
        const QDateTime day = from.addDays(dayOffset);
        if (!maskContainsDay(rule.days, day.date().dayOfWeek()))
            continue;

        QDateTime candidate(day.date(), start);
        if (candidate <= from) {
            if (dayOffset == 0 && ruleMatches(rule, from)) {
                // Already inside the window: report the negative offset so the
                // caller can say "ends at" instead.
                const QTime end = parseClock(rule.to);
                QDateTime endsAt(day.date(), end);
                if (end <= start)
                    endsAt = endsAt.addDays(1);
                return -int(from.secsTo(endsAt) / 60);
            }
            continue;
        }
        return int(from.secsTo(candidate) / 60);
    }
    return -1;
}

QString humaniseMinutesImpl(int minutes)
{
    if (minutes < 60)
        return QStringLiteral("%1m").arg(minutes);
    const int h = minutes / 60;
    const int m = minutes % 60;
    if (h >= 24) {
        const int d = h / 24;
        const int rh = h % 24;
        return rh ? QStringLiteral("%1d %2h").arg(d).arg(rh) : QStringLiteral("%1d").arg(d);
    }
    return m ? QStringLiteral("%1h %2m").arg(h).arg(m) : QStringLiteral("%1h").arg(h);
}

} // namespace

ScheduleCard::ScheduleCard(const Rule &rule, QWidget *parent) : QFrame(parent), m_rule(rule)
{
    setObjectName(QStringLiteral("scheduleCard"));
    setFrameShape(QFrame::NoFrame);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_root = new QHBoxLayout(this);
    m_root->setContentsMargins(12, 9, 12, 9);
    m_root->setSpacing(12);

    // ---- identity block: clock, name, when it runs -----------------------
    auto *identity = new QWidget(this);
    auto *idCol = new QHBoxLayout(identity);
    idCol->setContentsMargins(0, 0, 0, 0);
    idCol->setSpacing(10);

    m_clock = new QLabel(QStringLiteral("\u25F7"), identity);
    m_clock->setObjectName(QStringLiteral("cardGlyph"));
    idCol->addWidget(m_clock, 0, Qt::AlignTop);

    auto *nameCol = new QVBoxLayout();
    nameCol->setContentsMargins(0, 0, 0, 0);
    nameCol->setSpacing(1);

    m_name = new QLabel(identity);
    m_name->setObjectName(QStringLiteral("scheduleName"));
    nameCol->addWidget(m_name);

    m_scheduleLine = new QLabel(identity);
    m_scheduleLine->setObjectName(QStringLiteral("scheduleTime"));
    nameCol->addWidget(m_scheduleLine);

    m_nextLine = new QLabel(identity);
    m_nextLine->setObjectName(QStringLiteral("sub"));
    nameCol->addWidget(m_nextLine);

    idCol->addLayout(nameCol);
    m_root->addWidget(identity, 1);

    // ---- limits ----------------------------------------------------------
    auto *limits = new QWidget(this);
    auto *limitRow = new QHBoxLayout(limits);
    limitRow->setContentsMargins(0, 0, 0, 0);
    limitRow->setSpacing(6);

    m_downGlyph = new QLabel(QStringLiteral("\u2193"), limits);
    m_downGlyph->setObjectName(QStringLiteral("downGlyph"));
    limitRow->addWidget(m_downGlyph);

    m_downValue = new QLabel(limits);
    m_downValue->setObjectName(QStringLiteral("limitValue"));
    limitRow->addWidget(m_downValue);

    limitRow->addSpacing(10);

    m_upGlyph = new QLabel(QStringLiteral("\u2191"), limits);
    m_upGlyph->setObjectName(QStringLiteral("upGlyph"));
    limitRow->addWidget(m_upGlyph);

    m_upValue = new QLabel(limits);
    m_upValue->setObjectName(QStringLiteral("limitValue"));
    limitRow->addWidget(m_upValue);

    m_root->addWidget(limits, 0);

    // ---- weekdays --------------------------------------------------------
    m_days = new DayPicker(this);
    m_days->setReadOnly(true);   // display only; editing happens in the dialog
    m_root->addWidget(m_days, 0);

    // ---- actions ---------------------------------------------------------
    m_actions = new QWidget(this);
    m_actionRow = new QHBoxLayout(m_actions);
    m_actionRow->setContentsMargins(0, 0, 0, 0);
    m_actionRow->setSpacing(6);

    m_toggle = new QPushButton(m_actions);
    m_toggle->setObjectName(QStringLiteral("scheduleToggle"));
    m_toggle->setCheckable(true);
    m_toggle->setCursor(Qt::PointingHandCursor);
    m_toggle->setToolTip(QStringLiteral("Enable or disable this schedule"));
    m_actionRow->addWidget(m_toggle);

    m_edit = new QPushButton(QStringLiteral("Edit"), m_actions);
    m_edit->setObjectName(QStringLiteral("ghost"));
    m_edit->setCursor(Qt::PointingHandCursor);
    m_edit->setToolTip(QStringLiteral("Edit this schedule"));
    m_actionRow->addWidget(m_edit);

    m_delete = new QPushButton(QStringLiteral("Delete"), m_actions);
    m_delete->setObjectName(QStringLiteral("danger"));
    m_delete->setCursor(Qt::PointingHandCursor);
    m_delete->setToolTip(QStringLiteral("Delete this schedule"));
    m_actionRow->addWidget(m_delete);

    m_root->addWidget(m_actions, 0);

    // Signals carry the rule id, never a list position.
    connect(m_edit, &QPushButton::clicked, this,
            [this] { emit editRequested(m_rule.id); });
    connect(m_delete, &QPushButton::clicked, this,
            [this] { emit deleteRequested(m_rule.id); });
    connect(m_toggle, &QPushButton::toggled, this, [this](bool on) {
        if (m_updating)
            return;
        m_rule.enabled = on;
        applyEnabledLook();
        emit enabledChanged(m_rule.id, on);
    });

    refreshTexts();
    rebuildLayout();
}

void ScheduleCard::setRule(const Rule &rule)
{
    m_rule = rule;
    refreshTexts();
    applyEnabledLook();
    rebuildLayout();
}

void ScheduleCard::setActiveState(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    // Marked with a left edge rather than a colour, so the card stays monochrome.
    setProperty("active", active);
    style()->unpolish(this);
    style()->polish(this);
    refreshTexts();
    update();
}

void ScheduleCard::refreshTexts()
{
    m_name->setText(m_rule.name.isEmpty() ? QStringLiteral("Untitled schedule") : m_rule.name);

    const QTime from = parseClock(m_rule.from);
    const QTime to = parseClock(m_rule.to);
    const QString fromText = from.isValid() ? from.toString(QStringLiteral("HH:mm"))
                                            : m_rule.from;
    const QString toText = to.isValid() ? to.toString(QStringLiteral("HH:mm")) : m_rule.to;

    const bool overnight = from.isValid() && to.isValid() && to <= from;
    m_scheduleLine->setText(QStringLiteral("\u25F7  %1 \u2192 %2%3")
                                .arg(fromText, toText,
                                     overnight ? QStringLiteral("  (overnight)") : QString()));

    // "Next:" is derived from the real clock, and omitted rather than faked when
    // the rule cannot start.
    const bool running = m_rule.enabled && ruleMatches(m_rule, QDateTime::currentDateTime());
    if (running) {
        const int mins = minutesUntilStartImpl(m_rule, QDateTime::currentDateTime());
        m_nextLine->setText(mins < 0 ? QStringLiteral("Active now \u2014 ends at %1").arg(toText)
                                     : QStringLiteral("Active now"));
    } else {
        const int mins = minutesUntilStartImpl(m_rule, QDateTime::currentDateTime());
        if (!m_rule.enabled)
            m_nextLine->setText(QStringLiteral("Disabled"));
        else if (mins < 0)
            m_nextLine->setText(describeDays(m_rule.days));
        else
            m_nextLine->setText(QStringLiteral("Next: in %1").arg(humaniseMinutesImpl(mins)));
    }

    m_downValue->setText(rateText(m_rule.down));
    m_upValue->setText(rateText(m_rule.up));
    m_days->setMask(m_rule.days);

    applyEnabledLook();
}

void ScheduleCard::applyEnabledLook()
{
    m_toggle->setChecked(m_rule.enabled);
    m_toggle->setText(m_rule.enabled ? QStringLiteral("Enabled")
                                     : QStringLiteral("Disabled"));

    // A disabled card is de-emphasised by text value only - no colour, no opacity
    // tricks that would look broken against either theme.
    const QString nameColor = m_rule.enabled ? theme::kTextPrimary().name()
                                             : theme::kTextMuted().name();
    m_name->setStyleSheet(QStringLiteral("color: %1;").arg(nameColor));

    const QColor downInk = m_rule.enabled ? theme::kGraphDownload() : theme::kTextMuted();
    const QColor upInk = m_rule.enabled ? theme::kGraphUpload() : theme::kTextMuted();
    m_downGlyph->setStyleSheet(QStringLiteral("color: %1; font-weight: 700;").arg(downInk.name()));
    m_upGlyph->setStyleSheet(QStringLiteral("color: %1; font-weight: 700;").arg(upInk.name()));
}

void ScheduleCard::resizeEvent(QResizeEvent *event)
{
    QFrame::resizeEvent(event);
    rebuildLayout();
}

void ScheduleCard::rebuildLayout()
{
    // Below the threshold the action buttons move to their own row underneath, so
    // Edit and Delete can never be pushed off the edge of a narrow card.
    const bool wantCompact = width() > 0 && width() < kCompactThreshold;
    if (m_arranged && wantCompact == m_compact)
        return;
    m_compact = wantCompact;
    m_arranged = true;

    // Detach everything that moves between the two arrangements.
    m_root->removeWidget(m_actions);
    if (m_top) {
        m_top->removeWidget(m_actions);
    }

    if (wantCompact) {
        // Identity + limits on top, actions underneath.
        if (!m_topRow) {
            m_topRow = new QWidget(this);
            m_top = new QHBoxLayout(m_topRow);
            m_top->setContentsMargins(0, 0, 0, 0);
            m_top->setSpacing(12);
        }
        m_root->addWidget(m_actions, 0, Qt::AlignRight);
    } else {
        m_root->addWidget(m_actions, 0, Qt::AlignVCenter);
    }

    if (layout())
        layout()->activate();
    updateGeometry();
}

} // namespace net
