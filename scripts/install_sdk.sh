#!/usr/bin/env bash
set -euo pipefail

install_root="$(realpath -m "${1:?Укажите каталог установки}")"
download_dir="$install_root/sdk-downloads"
mkdir -p "$download_dir"
cd "$download_dir"

# Только ARM toolchain; QEMU берём из пакетов системы.
release_url=https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1
for archive in zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz; do
    if [ ! -f "$archive" ]; then
        curl --fail --location --retry 2 "$release_url/$archive" -o "$archive"
    fi
done

# Суммы из официального sha256.sum релиза v1.0.1.
sha256sum --check --strict <<'EOF'
ca9bc0ff66fafca1dac9d592a36d953cf16d096a9d09b1c0357f021cf9f6a7eb  zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz
21b85981cb5a1818d9bc53d82af80f208946ec038b982ff1907287572ed3a634  toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz
EOF

tar -xf zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz -C "$install_root"
mkdir -p "$install_root/zephyr-sdk-1.0.1/gnu"
tar -xf toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz -C "$install_root/zephyr-sdk-1.0.1/gnu"
"$install_root/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc" --version
