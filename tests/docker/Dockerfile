# Throwaway environment for running Anchorage's tests as root.
# Root inside the container is real UID 0, so code paths that only run as root
# (setting owners of files and folders) are exercised, while the host is untouched.
FROM ubuntu:26.04

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build extra-cmake-modules \
      qt6-base-dev qt6-tools-dev \
      libkf6coreaddons-dev libkf6i18n-dev libkf6xmlgui-dev libkf6widgetsaddons-dev libkf6auth-dev libkf6itemviews-dev libkf6colorscheme-dev libkf6solid-dev \
      systemd util-linux \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /build
