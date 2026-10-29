#pragma once

#include "theme.h"
#include "units.h"
#include <QDialog>
#include <QElapsedTimer>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace net {

class UpdateDownloadDialog : public QDialog {
public:
    explicit UpdateDownloadDialog(const QString &version, QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(QStringLiteral("Updating Flowbit"));
        setWindowModality(Qt::WindowModal);
        setMinimumWidth(460);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(28, 24, 28, 24);
        layout->setSpacing(16);
        auto *eyebrow = new QLabel(QStringLiteral("FLOWBIT  /  UPDATE"), this);
        eyebrow->setObjectName(QStringLiteral("sub"));
        layout->addWidget(eyebrow);
        auto *title = new QLabel(QStringLiteral("Downloading version %1").arg(version), this);
        title->setFont(theme::uiFont(16, true));
        layout->addWidget(title);
        auto *note = new QLabel(QStringLiteral("Your update is on its way. You can cancel at any time."), this);
        note->setWordWrap(true);
        note->setObjectName(QStringLiteral("sub"));
        layout->addWidget(note);
        auto *row = new QHBoxLayout();
        m_status = new QLabel(QStringLiteral("Connecting…"), this);
        m_percent = new QLabel(QStringLiteral("—"), this);
        row->addWidget(m_status);
        row->addStretch();
        row->addWidget(m_percent);
        layout->addLayout(row);
        m_bar = new QProgressBar(this);
        m_bar->setTextVisible(false);
        m_bar->setFixedHeight(8);
        m_bar->setRange(0, 0);
        const auto &p = theme::palette();
        m_bar->setStyleSheet(QStringLiteral(
            "QProgressBar { border: none; border-radius: 4px; background: %1; }"
            "QProgressBar::chunk { border-radius: 4px; background: %2; }")
            .arg(p.border.name(), p.textPrimary.name()));
        layout->addWidget(m_bar);
        m_detail = new QLabel(QStringLiteral("Waiting for the download to start"), this);
        m_detail->setObjectName(QStringLiteral("sub"));
        layout->addWidget(m_detail);
        auto *footer = new QHBoxLayout();
        auto *hint = new QLabel(QStringLiteral("The download will be verified before installing."), this);
        hint->setObjectName(QStringLiteral("sub"));
        hint->setWordWrap(true);
        footer->addWidget(hint, 1);
        auto *cancel = new QPushButton(QStringLiteral("Cancel download"), this);
        cancel->setObjectName(QStringLiteral("ghost"));
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        footer->addWidget(cancel);
        layout->addLayout(footer);
        m_clock.start();
    }

    void setProgress(qint64 received, qint64 total)
    {
        m_status->setText(QStringLiteral("Downloading update"));
        const double seconds = m_clock.elapsed() / 1000.0;
        const double speed = seconds > 0 ? received / seconds : 0;
        QString detail = formatBytes(double(received));
        if (total > 0) {
            const int percent = qBound(0, int(100.0 * received / total), 100);
            m_bar->setRange(0, 100);
            m_bar->setValue(percent);
            m_percent->setText(QStringLiteral("%1%").arg(percent));
            detail += QStringLiteral(" of %1").arg(formatBytes(double(total)));
        } else {
            m_bar->setRange(0, 0);
            m_percent->setText(QStringLiteral("—"));
        }
        if (speed > 0 && seconds >= 1) {
            detail += QStringLiteral("  ·  %1").arg(theme::formatBytesPerSec(speed));
            if (total > received) {
                const int remaining = qMax(1, int((total - received) / speed));
                detail += remaining < 60 ? QStringLiteral("  ·  %1 sec left").arg(remaining)
                                         : QStringLiteral("  ·  %1 min left").arg((remaining + 59) / 60);
            }
        }
        m_detail->setText(detail);
        if (total > 0 && received >= total) {
            m_status->setText(QStringLiteral("Verifying download…"));
            m_detail->setText(QStringLiteral("Checking the update before installation"));
        }
    }

private:
    QElapsedTimer m_clock;
    QLabel *m_status = nullptr;
    QLabel *m_percent = nullptr;
    QLabel *m_detail = nullptr;
    QProgressBar *m_bar = nullptr;
};
} // namespace net
