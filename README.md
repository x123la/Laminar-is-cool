# Laminar: Fluid-Dynamic Traffic Smoothing

> **"Treating packets not as discrete units, but as a compressible fluid."**

Laminar is a high-performance, experimental Linux user-space traffic smoother. It intercepts network traffic via Netfilter Queue (NFQUEUE), buffers it, and releases it according to a strict control loop driven by a **1D Viscous Burgers Equation** solver.

By modeling network traffic as a fluid flowing through a pipe, Laminar attempts to maintain a target Reynolds number (Re ≈ 2000), keeping the flow in a "laminar" state rather than a turbulent one, thereby minimizing jitter and bufferbloat while maximizing throughput.

---

## 🏗 Architecture

Laminar utilizes a "Split-Brain" architecture to balance high-speed packet processing with complex mathematical modeling:

### 1. Data Plane (C11 + libnetfilter_queue)
- **Role:** High-speed packet interception, buffering, and delayed verdict enforcement.
- **Mechanism:** Sits on a dedicated thread, polling the kernel via `Netlink` socket.
- **Key Features:**
  - **Zero-Copy-ish Design:** Uses `nfq_set_verdict_batch` to minimize syscall overhead.
  - **Fail-Open Safety:** Automatically accepts packets if the ring buffer overflows or the process crashes (via `nft` bypass rules).
  - **Starvation Protection:** Implements a strict burst limit (64 packets) per poll cycle to prevent receive-livelocks.
  - **Jumbo Frame Support:** 64KB receive buffer to handle GSO/TSO and Loopback traffic without truncation.

### 2. Control Plane (Chapel)
- **Role:** The "Physics Physics Engine" of the router.
- **Frequency:** Runs at a strict **1000 Hz** (1ms time step).
- **Physics Model:**
  - Solves the **Viscous Burgers Equation** (`u_t + u u_x = ν u_xx`) on a 64-point grid.
  - **Viscosity Control:** Dynamically adjusts viscosity (`ν`) to target a Reynolds number of ~2000.
  - **Pressure Coupling:** Reads the "pressure" (bytes currently buffered in C ring) and translates it into a "density" (`ρ`), applying a pressure gradient force to the fluid velocity `u`.
  - **Output:** Calculates a `release_budget_bytes` every millisecond and atomically pushes it to the Data Plane.

---

## 🌊 The Physics Model

Laminar does not use Token Buckets (`TBF`) or HTB. It uses **Computational Fluid Dynamics (CFD)** concepts:

1.  **Inflow Boundary (`u_in`):** The rate of bytes entering the C ring buffer is realized as the "inlet velocity" of the fluid.
2.  **The Pipe:** A 1D grid of 64 points representing the smooth pipeline.
3.  **Viscosity (`ν`):** Controls how "thick" the fluid is.
    - If `ν` is too high, flow is sluggish (high latency).
    - If `ν` is too low, the equation becomes unstable/turbulent (shockwaves, jitter).
    - Laminar dynamically controls `ν` to ride the edge of stability.
4.  **Pressure (`ρ`):** Approximated by `Filled_Buffer_Size / Max_Capacity`. High pressure pushes the fluid out faster (`u_cmd`) to drain the queue.

---

## 📋 Requirements

To build and run Laminar, you need a standard Linux development environment with Netfilter libraries and the Chapel compiler.

### Core Dependencies
- **Linux Kernel** (Recent, with `nf_tables` support)
- **nftables** (Command line tool)
- **libnetfilter_queue** (Dev headers)
- **libnfnetlink** (Dev headers)
- **GCC** (or Clang) & **Make**
- **pkg-config**

### The Control Plane: Chapel
You need the **Chapel** compiler (`chpl`).
- **Version:** 2.x (Tested with 2.7)
- **Download:** [Chapel-lang.org](https://chapel-lang.org/download.html)

---

## 🚀 Installation

We provide bootstrap scripts to automate dependency installation for common distributions.

### 1. Check Dependencies
Run the checker to see what you are missing:
```bash
./scripts/check-deps.sh
```

### 2. Install Tools (Automated)

**Ubuntu/Debian:**
```bash
./scripts/bootstrap-ubuntu.sh
```

**Fedora/RHEL:**
```bash
./scripts/bootstrap-fedora.sh
```

### 3. Install Chapel manually (if needed)
The bootstrap scripts install C libraries but simply guide you to install Chapel. Use the commands output by `./scripts/check-deps.sh` to get the specific `.deb` or `.rpm` for your system.

### 4. Build
Once dependencies are present, compile the project:
```bash
make
```
*This produces the `laminar` binary.*

---

## ⚔️ Usage: The Ritual

Laminar requires `root` privileges to access the Netfilter Queue. It is designed to be run as a foreground process.

### Step 1: Hijack Traffic
We must tell the Linux kernel to send traffic to Laminar instead of sending it straight to the network card. We use `nftables` for this.

**Enable interception (HTTP/HTTPS egress):**
```bash
./scripts/nft-enable.sh
```
*> This script creates a fail-open rule. If Laminar is not running, traffic "bypasses" the queue and flows normally.*

### Step 2: Ignite the Engine
Run the binary. It will initialize the NFQUEUE handler and start the 1000 Hz physics loop.
```bash
sudo ./laminar
```

**Output:**
You will see a 10 Hz telemetry log:
```text
[LAMINAR] rho=0.12 u_in=5.4 u_exit=5.2 u_cmd=6.1 nu=0.04 Re=1850 pressure_bytes=419430 budget_bytes=15000
```
- **regime**: `[LAMINAR]` or `[TURBULENT]` (re > 2000).
- **rho**: Queue occupancy (0.0 to 1.0).
- **Re**: Reynolds Number (Target is ~2000).

### Step 3: Shutdown
Press `Ctrl+C` to stop Laminar.
- The application will perform a "Fail-Open Flush", accepting all remaining packets in the buffer before exiting.

### Step 4: Release Traffic
Remove the firewall rules to return the system to normal networking.
```bash
./scripts/nft-disable.sh
```

---

## 🛡 Safety & robustness

Laminar is designed for "Production-Grade" stability despite its experimental nature.

1.  **Fail-Open Firewall Rules:** The `nft` rules use `queue num 0 bypass`. If the `laminar` process dies, the kernel stops queuing packets and passes them through immediately. You won't start losing internet access.
2.  **Ring Buffer Overflow:** If the userspace application is too slow and the ring fills up, new packets are immediately accepted (`NF_ACCEPT`) rather than dropped.
3.  **Hard Real-Time Protection:**
    - **Deadlines:** Packets older than **25ms** are forcibly released to prevent self-inflicted TCP timeouts.
    - **Drift Detection:** The 1000 Hz clock compensates for sleep drift. If the system falls >50ms behind (due to CPU overload), it performs a "hard resync" (skips ticks) to prevent a death spiral.
4.  **Starvation Prevention:** The network receiver thread has a "burst limit" (64 packets). It *must* yield execution to the processing/releasing logic, preventing a scenario where a flood of incoming packets prevents outgoing packets from ever being released.

---

## 📂 Repository Structure

```text
.
├── Makefile                # Build system configuration
├── README.md               # You are here
├── LICENSE                 # MIT License
├── src/
│   ├── laminar.chpl        # MAIN: Chapel control plane (Physics engine)
│   ├── sluice.c            # LAYER: C data plane (Netlink/NFQUEUE interaction)
│   ├── sluice.h            # Header for C <-> Chapel FFI
│   ├── tick.c              # High-precision monotonic clock & drift correction
│   └── tick.h              # Header for clock module
└── scripts/
    ├── check-deps.sh       # Dependency verifier & install guide
    ├── bootstrap-ubuntu.sh # Apt-get installer for libraries
    ├── bootstrap-fedora.sh # Dnf installer for libraries
    ├── nft-enable.sh       # Sets up NFQUEUE redirection rules
    └── nft-disable.sh      # Tears down firewall rules
```

---

## 📜 License

MIT License. See [LICENSE](LICENSE) file for details.

Copyright (c) 2026 Lucas Alonso Basanko & The Google DeepMind Team.
