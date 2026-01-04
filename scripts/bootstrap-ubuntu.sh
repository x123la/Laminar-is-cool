#!/usr/bin/env bash
set -euo pipefail

sudo apt-get update
sudo apt-get install -y build-essential pkg-config nftables libnetfilter-queue-dev libnfnetlink-dev

echo "Netfilter deps installed. Now install Chapel 2.7 for your distro/arch:"
echo "See: ./scripts/check-deps.sh output for exact wget + install commands."
