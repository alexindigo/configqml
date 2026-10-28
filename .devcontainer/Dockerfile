FROM docker.io/library/archlinux:base-devel
RUN pacman -Syu --noconfirm --needed clang shellcheck shfmt git \
    && pacman -Scc --noconfirm
WORKDIR /workspaces/configqml
