#!/usr/bin/env bash
set -euo pipefail

sudo nft delete table inet laminar 2>/dev/null || true
echo "Laminar nft rules disabled."
