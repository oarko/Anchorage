// SPDX-License-Identifier: GPL-3.0-or-later
// Prints Anchorage's view of this machine (fstab + drives + mounts), one item per line:
//   title|kind|mountPoint|fsType|label|mounted|inFstab|system|size
// Used by tests/vm/drives.sh; handy for debugging. Not installed.
#include "drives.h"
#include "fstab.h"
#include "inventory.h"
#include "mounts.h"

#include <QCoreApplication>
#include <QTextStream>

using namespace anchorage;

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);
    const auto volumes = drives::scan();
    const auto current = mounts::readCurrent();
    QString error;
    const auto file = fstab::File::load(QStringLiteral("/etc/fstab"), &error);
    if (!volumes || !current || !file) {
        err << "scan failed: " << (volumes ? (current ? error : current.error()) : volumes.error()) << "\n";
        return 1;
    }
    const char *kinds[] = {"drive", "network", "swap"};
    for (const auto &i : inventory::build(*file, *volumes, *current)) {
        out << i.title << "|" << kinds[static_cast<int>(i.kind)] << "|" << i.mountPoint << "|"
            << (i.volume ? i.volume->fsType : (i.entry ? i.entry->type : QString())) << "|" << (i.volume ? i.volume->label : QString())
            << "|" << (i.mounted ? "mounted" : "-") << "|" << (i.entry ? "fstab" : "-") << "|" << (i.system ? "system" : "-") << "|"
            << inventory::formatSize(i.volume ? i.volume->size : -1) << "\n";
    }
    return 0;
}
