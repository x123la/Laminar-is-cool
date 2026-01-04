#!/usr/bin/env bash
set -euo pipefail

fail=0

need_cmd() {
  local c="$1"
  if ! command -v "$c" >/dev/null 2>&1; then
    echo "MISSING: $c"
    fail=1
  fi
}

echo "== Laminar dependency check =="

need_cmd gcc
need_cmd make
need_cmd pkg-config
need_cmd nft
need_cmd sudo
need_cmd chpl

if command -v pkg-config >/dev/null 2>&1; then
  if ! pkg-config --exists libnetfilter_queue; then
    echo "MISSING: pkg-config(libnetfilter_queue) / libnetfilter_queue dev headers"
    fail=1
  fi
fi

if [[ "$fail" -ne 0 ]]; then
  echo
  echo "== Install commands (EXACT) =="

  echo "-- Ubuntu/Debian (netfilter deps) --"
  echo "sudo apt-get update && sudo apt-get install -y build-essential pkg-config nftables libnetfilter-queue-dev libnfnetlink-dev"

  echo
  echo "-- Fedora (netfilter deps) --"
  echo "sudo dnf install -y gcc make pkgconf-pkg-config nftables libnetfilter_queue-devel libnfnetlink-devel"

  echo
  echo "-- Chapel 2.7 packages (pick the one matching your distro/arch) --"
  echo "Ubuntu 24.04 amd64:"
  echo "  wget -O chapel.deb https://github.com/chapel-lang/chapel/releases/download/2.7.0/chapel-2.7.0-1.ubuntu24.amd64.deb"
  echo "  sudo apt install ./chapel.deb"
  echo "Debian 12 amd64:"
  echo "  wget -O chapel.deb https://github.com/chapel-lang/chapel/releases/download/2.7.0/chapel-2.7.0-1.debian12.amd64.deb"
  echo "  sudo apt install ./chapel.deb"
  echo "Fedora 42 x86_64:"
  echo "  wget -O chapel.rpm https://github.com/chapel-lang/chapel/releases/download/2.7.0/chapel-2.7.0-1.fc42.x86_64.rpm"
  echo "  sudo dnf install ./chapel.rpm"

  echo
  echo "After installing, re-run:"
  echo "  ./scripts/check-deps.sh"
  echo "  make"

  exit 1
fi

echo "OK: All required tools and headers appear present."
