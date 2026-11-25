// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "result.h"

#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringList>

// Disks, partitions and file systems, as reported by `lsblk --json` (util-linux).
// lsblk reads the same libblkid/udev data as Solid, needs no root, and its JSON output
// can be captured as test fixtures (tests/data/lsblk-*.json).
namespace anchorage::drives
{

struct Volume {
    QString name; // sdb1
    QString path; // /dev/sdb1
    QString type; // disk, part, loop, crypt, lvm, rom, ...
    qint64 size = -1; // bytes
    QString fsType; // btrfs, ext4, vfat, ntfs, swap, ... (empty: no file system)
    QString fsVersion;
    QString label;
    QString uuid;
    QString partUuid;
    QString partLabel;
    QString partTypeName; // "EFI System", "Windows recovery environment", ...
    QStringList mountPoints; // where the kernel has it mounted right now
    qint64 fsSize = -1; // only known while mounted
    qint64 fsUsed = -1;
    bool readOnly = false;

    // From the whole disk this volume belongs to.
    QString diskPath; // /dev/sdb
    QString diskModel; // Samsung SSD 870 QVO 2TB
    QString transport; // sata, nvme, usb, ...
    bool removable = false; // removable media or hot-pluggable (USB sticks, card readers)
    bool rotational = false;
};

// Parses `lsblk --json --bytes -o <columns()>` output into a flat list (disks first,
// then their partitions). Unknown or missing fields are left empty.
Result<QList<Volume>> parseLsblk(QByteArrayView json);

// Columns to ask lsblk for.
QString columns();

// Runs /usr/bin/lsblk and parses it.
Result<QList<Volume>> scan();

// Volumes a person would recognise as "a drive": has a file system, and isn't a snap
// image (squashfs loop device), RAM disk, CD/DVD, RAID/LVM building block or encrypted
// container. Other loop devices (attached disk images) are shown.
bool isUserVisible(const Volume &v);

} // namespace anchorage::drives
