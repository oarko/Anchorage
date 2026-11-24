# Anchorage

Dolphin integration for managing drives, network shares and `/etc/fstab` on KDE Plasma 6.

**Status: early development.** Nothing here touches your system yet.
See [`docs/SPEC.md`](docs/SPEC.md) for the agreed design.

## Building (Kubuntu 26.04)

```sh
sudo apt install build-essential cmake ninja-build extra-cmake-modules qt6-base-dev qt6-tools-dev \
  libkf6coreaddons-dev libkf6i18n-dev libkf6xmlgui-dev libkf6widgetsaddons-dev libkf6auth-dev
```

The full list for later phases (KAuth helper, drives, network shares, fuzzing) is in `CLAUDE.md`.

```sh
cmake --preset dev          # Debug build with AddressSanitizer + UndefinedBehaviorSanitizer
cmake --build --preset dev
ctest --preset dev
./build/dev/bin/anchorage
```

Use the `release` preset for a hardened optimised build (`build/release`).

Anchorage refuses to run as root. Run it as your normal user; it asks for a password
when a change needs administrator rights.

## License

GPL-3.0-or-later. See [`LICENSE`](LICENSE).
