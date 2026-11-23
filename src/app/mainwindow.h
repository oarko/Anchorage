// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <KXmlGuiWindow>

// Main window, layout C (Dolphin-like): Places | tiles | info panel. See docs/SPEC.md §15.
// Skeleton only for now.
class MainWindow : public KXmlGuiWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
};
