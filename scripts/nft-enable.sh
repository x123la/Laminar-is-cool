#!/usr/bin/env bash
set -euo pipefail

# Idempotent: reset table so we don't accumulate duplicate queue rules
sudo nft delete table inet laminar 2>/dev/null || true

sudo nft add table inet laminar
sudo nft 'add chain inet laminar output { type filter hook output priority 0; policy accept; }'

# Queue HTTP+HTTPS egress into NFQUEUE 0. "bypass" is mandatory fail-open safety.
sudo nft add rule inet laminar output tcp dport {80, 443} queue num 0 bypass

echo "Laminar nft rules enabled (inet/laminar)."
