// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "inventory.h"

#include <QWidget>

#include <optional>

class QFormLayout;
class QLabel;
class QPushButton;

// Right-hand panel: details of the selected tile (layout C, SPEC §15).
class InfoPanel : public QWidget
{
    Q_OBJECT

public:
    explicit InfoPanel(QWidget *parent = nullptr);

    void showItem(const std::optional<anchorage::inventory::Item> &item);
    void showSummary(int total, int mounted, const QString &viewName);
    void showMessage(const QString &title, const QString &text, const QString &iconName);

private:
    void addRow(const QString &label, const QString &value, bool mono = false);
    void clearRows();

    QLabel *m_icon;
    QLabel *m_title;
    QLabel *m_subtitle;
    QFormLayout *m_form;
    QWidget *m_fstabBox;
    QLabel *m_fstabLine;
    QPushButton *m_open;
    QPushButton *m_copy;
    QLabel *m_note;
    std::optional<anchorage::inventory::Item> m_item;
};
