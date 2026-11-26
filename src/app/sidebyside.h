// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QWidget>

class QLabel;
class QTextEdit;

// Two files next to each other, line by line (the Backups page, SPEC §9):
//  * lines only on the left (removed by restoring): red on the left, filler on the right;
//  * lines only on the right (added back): green on the right, filler on the left;
//  * similar lines: yellow on both sides, the differing characters underlined in red
//    (left, removed) and green (right, added back);
//  * every real line ends with a faint ↵, so a real blank line shows as just ↵ and can't
//    be mistaken for a filler (hatched, no ↵).
// Both sides scroll together.
class SideBySideView : public QWidget
{
    Q_OBJECT

public:
    explicit SideBySideView(QWidget *parent = nullptr);

    void setTitles(const QString &left, const QString &right);
    void setTexts(const QByteArray &left, const QByteArray &right);
    int differences() const { return m_differences; }

    QTextEdit *leftView() const { return m_left; }
    QTextEdit *rightView() const { return m_right; }

private:
    QLabel *m_leftTitle;
    QLabel *m_rightTitle;
    QTextEdit *m_left;
    QTextEdit *m_right;
    int m_differences = 0;
};
