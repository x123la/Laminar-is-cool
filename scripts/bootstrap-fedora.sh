#!/usr/bin/env bash
set -euo pipefail

sudo dnf install -y gcc make pkgconf-pkg-config nftables libnetfilter_queue-devel libnfnetlink-devel

echo "Netfilter deps installed. Now install Chapel 2.7 for your distro/arch:"
echo "See: ./scripts/check-deps.sh output for exact wget + install commands."
