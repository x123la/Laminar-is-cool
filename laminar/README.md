# Laminar

Laminar is a Linux user-space NFQUEUE traffic smoother that treats packets as a compressible fluid.
It delays NFQUEUE verdicts briefly and releases them under a 1000 Hz control loop driven by a
1D viscous Burgers equation + viscosity control targeting Re≈2000.

## Requirements

- Linux
- nftables
- libnetfilter_queue (runtime + development headers)
- pkg-config
- gcc + make
- Chapel compiler (`chpl`)

## Build

```bash
make
```

## Run (Ritual)

### Enable NFQUEUE interception (HTTP/HTTPS egress only, fail-open bypass enabled):

```bash
./scripts/nft-enable.sh
```

### Run Laminar (root required for NFQUEUE):

```bash
sudo ./laminar
```

### Disable rules when finished:

```bash
./scripts/nft-disable.sh
```

## Notes

- **Safety**: nft queue bypass is enabled; if Laminar stops, traffic continues normally.
- The ring buffer also fails open on overflow (immediate NF_ACCEPT).
- Packets older than 25ms are forcibly released to prevent self-inflicted timeouts.
- Output logs print at 10 Hz.
