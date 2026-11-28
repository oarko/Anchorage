# Anchorage for Fedora (SPEC §12a). Built by packaging/build-rpm.sh in clean Fedora
# containers. The version must match CMakeLists.txt and debian/changelog: CMake stops
# otherwise. A ~beta/~rc version is a beta build (launch warning, more logging, and the
# share tools only suggested).

%global app_id io.github.oarko.anchorage

Name:           anchorage
Version:        0.9.0~beta1
# Must come after Version: a global macro is expanded where it is defined.
%global beta %(echo %{version} | grep -qE '~(beta|rc)' && echo 1 || echo 0)
Release:        1%{?dist}
Summary:        Manage drives, network shares and /etc/fstab on KDE Plasma

License:        GPL-3.0-or-later
URL:            https://github.com/oarko/Anchorage
Source0:        %{name}-%{version}.tar.gz
Source1:        anchorage.sysusers

BuildRequires:  cmake
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  extra-cmake-modules
BuildRequires:  pkgconfig(smbclient)
BuildRequires:  cmake(Qt6Core)
BuildRequires:  cmake(Qt6Concurrent)
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  cmake(Qt6Test)
BuildRequires:  cmake(KF6CoreAddons)
BuildRequires:  cmake(KF6I18n)
BuildRequires:  cmake(KF6XmlGui)
BuildRequires:  cmake(KF6WidgetsAddons)
BuildRequires:  cmake(KF6Auth)
BuildRequires:  cmake(KF6ItemViews)
BuildRequires:  cmake(KF6ColorScheme)
BuildRequires:  cmake(KF6Solid)
BuildRequires:  desktop-file-utils
BuildRequires:  systemd-rpm-macros
BuildRequires:  kf6-rpm-macros

Requires:       polkit
Requires:       systemd

%if %{beta}
# Testers should see Anchorage's own "Install" offer for the share tools.
Suggests:       cifs-utils
Suggests:       nfs-utils
%else
Recommends:     cifs-utils
Recommends:     nfs-utils
%endif

%description
Anchorage is a KDE Plasma application for the places a computer mounts:
internal and USB drives, folders shown in a second place (bind mounts), and
SMB and NFS network shares. It edits /etc/fstab for you, explains each option
in plain words, checks every change before saving it, and keeps a backup of
the file before each change.

Changes to system files are made by a small helper that runs as root through
polkit. Members of the "driveadmins" group can manage everything with their
own password; members of "netmount" can add network shares. The package
creates both groups but adds nobody to them.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -G Ninja \
    -DANCHORAGE_PACKAGE_VERSION=%{version} \
    -DANCHORAGE_WERROR=OFF \
    -DBUILD_TESTING=ON
%cmake_build

%install
%cmake_install
# rpm creates the groups from this file when the package is installed (Fedora 42 and later).
install -Dpm 644 %{SOURCE1} %{buildroot}%{_sysusersdir}/anchorage.conf
install -Dpm 644 debian/anchorage.1 %{buildroot}%{_mandir}/man1/anchorage.1

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/%{app_id}.desktop
export QT_QPA_PLATFORM=offscreen
%ctest

%postun
# Erasing (not upgrading) keeps /etc/anchorage: saved logins that shares in /etc/fstab
# may still use, backups of /etc/fstab and settings. /etc/fstab is never changed.
if [ "$1" -eq 0 ] && [ -d /etc/anchorage ]; then
    echo "anchorage: kept /etc/anchorage (saved logins, backups of /etc/fstab and settings)."
    echo "anchorage: shares in /etc/fstab that use a saved login there need it to keep mounting."
    echo "anchorage: delete it by hand if you don't need it: sudo rm -r /etc/anchorage"
fi

%files
%license LICENSE
%doc README.md
%{_bindir}/anchorage
%{_kf6_libexecdir}/kauth/anchorage-helper
%{_datadir}/applications/%{app_id}.desktop
%{_datadir}/icons/hicolor/scalable/apps/%{app_id}.svg
%{_datadir}/dbus-1/system-services/%{app_id}.service
%{_datadir}/dbus-1/system.d/%{app_id}.conf
%{_datadir}/polkit-1/actions/%{app_id}.policy
%{_datadir}/polkit-1/rules.d/60-anchorage-groups.rules
%{_sysusersdir}/anchorage.conf
%{_mandir}/man1/anchorage.1*

%changelog
* Fri Nov 27 2026 Arraybytes <anchorage@arraybytes.com> - 0.9.0~beta1-1
- First beta for testers: drives, binds, network shares and /etc/fstab, with
  backups and saved logins. Use a virtual machine or a computer you don't
  depend on.
