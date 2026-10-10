# NVIDIA Linux Driver Triage & Hardware Recovery Playbook

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform: Linux](https://img.shields.io/badge/Platform-Linux%20Kernel%206.x%2F7.x-orange.svg)]()
[![Hardware: NVIDIA Ampere / Ada / Blackwell](https://img.shields.io/badge/GPU-RTX%2030xx%20%7C%2040xx%20%7C%2050xx-green.svg)]()

Production-grade diagnostics, recovery automations, and permanent preventative fixes for NVIDIA Linux driver failures, GSP firmware RPC timeouts (`Xid 119`), GPU lockups (`Xid 154: GPU Reset Required`), and device node desyncs inside sandboxed containers.

---

## 📑 Table of Contents

- [The Anatomy: Failure Modes](#-the-anatomy-failure-modes)
  - [Failure Mode 1: GSP Firmware Hang & Xid 119/154](#1-gsp-firmware-hang--xid-119154)
  - [Failure Mode 2: Missing `/dev/nvidia*` Nodes (Container & Sandbox Desync)](#2-missing-devnvidia-nodes-container--sandbox-desync)
  - [Failure Mode 3: Desktop Process Locks (`Device or resource busy`)](#3-desktop-process-locks-device-or-resource-busy)
- [Diagnostic Toolkit](#-diagnostic-toolkit)
- [Recovery Procedures](#-recovery-procedures)
  - [Tier 1: Non-Destructive PCIe Hot-Reset (Live CLI)](#tier-1-non-destructive-pcie-hot-reset-live-cli)
  - [Tier 2: TTY Multi-User Driver Reload](#tier-2-tty-multi-user-driver-reload)
  - [Tier 3: Bare-Metal Power-Rail Reboot](#tier-3-bare-metal-power-rail-reboot)
- [Permanent Hardening & Prevention](#-permanent-hardening--prevention)
  - [1. Disable GSP Firmware (Fallback to CPU-RM)](#1-disable-gsp-firmware-fallback-to-cpu-rm)
  - [2. Enforce Persistence Mode](#2-enforce-persistence-mode)
  - [3. Configure PM3 / Dynamic Power Management Rules](#3-configure-pm3--dynamic-power-management-rules)
- [Expected Outputs & Verification](#-expected-outputs--verification)
  - [Healthy vs Failing `nvidia-smi`](#healthy-vs-failing-nvidia-smi)
  - [Driver API Validation (Python / C / CUDA)](#driver-api-validation-python--c--cuda)
- [Automated Scripts Included](#-automated-scripts-included)

---

## 🧬 The Anatomy: Failure Modes

### 1. GSP Firmware Hang & Xid 119/154

Starting with NVIDIA Driver 510+ and default in 555/570/595+, NVIDIA offloads power management, clocking, and thermal regulation to an embedded RISC-V co-processor called the **GSP (GPU System Processor)**.

When GSP firmware stops responding to the CPU during runtime power state transitions:
1. The kernel driver issues RPC request `GSP_RM_CONTROL` (function 76).
2. After 5–6 seconds, kernel raises:
   ```text
   NVRM: Xid (PCI:0000:01:00): 119, Timeout after 6s of waiting for RPC response from GPU0 GSP!
   NVRM: Xid (PCI:0000:01:00): 154, GPU recovery action changed from 0x0 (None) to 0x1 (GPU Reset Required)
   ```
3. Once marked `GPU Reset Required`:
   - All thermal, fan, and clock queries return `ERR!`.
   - `cuInit()` immediately fails with error code `100` (`CUDA_ERROR_NO_DEVICE`).
   - `nvidia-smi --gpu-reset` reports `Resetting GPU ... is not supported` on mobile/laptop GPUs.

### 2. Missing `/dev/nvidia*` Nodes (Container & Sandbox Desync)

When running inside sandboxed environments (such as Flatpak, Snap, `bwrap`, or container runtimes):
- Host kernel drivers may be loaded (`lsmod | grep nvidia`), but `/dev/nvidia0`, `/dev/nvidiactl`, `/dev/nvidia-uvm` are omitted from the mount namespace.
- Running `nvidia-smi` results in:
  ```text
  NVIDIA-SMI has failed because it couldn't communicate with the NVIDIA driver.
  Make sure that the latest NVIDIA driver is installed and running.
  ```
- **Fix:** Mount host character devices (`/dev/nvidia*`) or run with direct hardware capability (`--dev-bind /dev/nvidia* /dev/nvidia*`).

### 3. Desktop Process Locks (`Device or resource busy`)

On hybrid laptops (Optimus / PRIME), desktop compositors (`gnome-shell`, `wayland`, `chrome`) allocate surface memory through `/dev/nvidia0` or `/dev/nvidia-modeset`. Attempting `modprobe -r nvidia` fails with:
```text
modprobe: FATAL: Module nvidia is in use.
```

---

## 🛠 Diagnostic Toolkit

Run these diagnostic commands to determine the exact failure tier:

| Goal | Command | What to Look For |
| :--- | :--- | :--- |
| **Check GPU State** | `nvidia-smi` | `ERR!` in Temp/Power/Fan or failure to communicate |
| **Inspect Kernel Xid Logs** | `journalctl -k -b \| grep -E "NVRM\|Xid"` | `Xid 119` (GSP RPC timeout), `Xid 154` (Reset required) |
| **Check Loaded Modules** | `lsmod \| grep -i nvidia` | `nvidia`, `nvidia_uvm`, `nvidia_modeset`, `nvidia_drm` |
| **Check Active Locks** | `lsof /dev/nvidia*` or `fuser -v /dev/nvidia*` | Processes preventing clean driver unbind |
| **Probe PCI Power State** | `cat /sys/bus/pci/devices/0000:01:00.0/power_state` | `D0` (Active), `D3cold` (Suspended/Unreachable) |
| **Verify Driver Version** | `cat /proc/driver/nvidia/version` | Ensure kernel module version matches userland packages |

---

## ⚔️ Recovery Procedures

### Tier 1: Non-Destructive PCIe Hot-Reset (Live CLI)

If no display server processes hold the device open:

```bash
# 1. Stop background power management daemons
sudo systemctl stop nvidia-powerd nvidia-persistenced 2>/dev/null || true

# 2. Trigger a secondary PCIe device remove & rescan (powers down link)
GPU_PCI="0000:01:00.0" # Adjust to match your `lspci | grep -i nvidia` bus ID
echo 1 | sudo tee "/sys/bus/pci/devices/${GPU_PCI}/remove"
sleep 2
echo 1 | sudo tee /sys/bus/pci/rescan
sleep 2

# 3. Reload drivers
sudo modprobe nvidia
sudo modprobe nvidia_uvm
sudo modprobe nvidia_modeset
sudo modprobe nvidia_drm

# 4. Verify recovery
nvidia-smi
```

### Tier 2: TTY Multi-User Driver Reload

When `gnome-shell` or `chrome` locks the GPU memory mappings:

1. Switch to a virtual console: press **`Ctrl` + `Alt` + `F3`** and log in.
2. Drop to multi-user target (shuts down graphical display session):
   ```bash
   sudo systemctl isolate multi-user.target
   ```
3. Remove kernel modules and trigger PCIe link cycle:
   ```bash
   sudo modprobe -r nvidia_uvm nvidia_drm nvidia_modeset nvidia
   echo 1 | sudo tee /sys/bus/pci/devices/0000:01:00.0/remove
   sleep 2
   echo 1 | sudo tee /sys/bus/pci/rescan
   sleep 2
   sudo modprobe nvidia nvidia_uvm nvidia_modeset nvidia_drm
   ```
4. Restart graphical desktop:
   ```bash
   sudo systemctl isolate graphical.target
   ```

### Tier 3: Bare-Metal Power-Rail Reboot

On laptop motherboards with proprietary ACPI firmware (e.g. Acer Predator, ASUS ROG, Lenovo Legion), GSP firmware crashes may lock the internal EC power rail. A full system reboot forces the platform firmware to renegotiate PCIe training and reload GSP microcode cleanly:

```bash
sudo reboot
```

---

## 🛡 Permanent Hardening & Prevention

### 1. Disable GSP Firmware (Fallback to CPU-RM)

If running Linux kernel 6.12+ or 7.x with NVIDIA driver 560/570/595 on Ampere (RTX 30-series) and encountering frequent GSP timeouts:

```bash
# Force legacy CPU-driven Resource Manager
echo "options nvidia NVreg_EnableGpuFirmware=0" | sudo tee /etc/modprobe.d/nvidia-gsp.conf

# Rebuild initramfs to apply on early boot
sudo update-initramfs -u
```

### 2. Enforce Persistence Mode

Prevent the GPU from entering deep power-saving states (`D3cold`) between CLI invocations, which triggers the GSP wake race condition:

```bash
sudo systemctl enable --now nvidia-persistenced
```

Or configure via `nvidia-smi`:
```bash
sudo nvidia-smi -pm 1
```

### 3. Configure Dynamic Power Management Rules

For hybrid laptops running PRIME / Wayland, set fine-grained runtime power management in `/etc/modprobe.d/nvidia-pm.conf`:

```ini
options nvidia "NVreg_DynamicPowerManagement=0x02"
options nvidia-drm modeset=1 fbdev=1
```

---

## 📊 Expected Outputs & Verification

### Healthy vs Failing `nvidia-smi`

#### Failing Output (`Xid 119/154` Lock):
```text
+-----------------------------------------------------------------------------------------+
| NVIDIA-SMI 595.99.02              Driver Version: 595.99.02      CUDA Version: 13.2     |
|=========================================+========================+======================|
|   0  NVIDIA GeForce RTX 3070 ...    Off |   00000000:01:00.0 N/A |                  N/A |
|ERR!  ERR! ERR!             N/A  /  N/A  |      13MiB /   8192MiB |     N/A      Default |
|                                         |                        |                 ERR! |
+-----------------------------------------+------------------------+----------------------+
```

#### Restored Healthy Output:
```text
+-----------------------------------------------------------------------------------------+
| NVIDIA-SMI 595.99.02              Driver Version: 595.99.02      CUDA Version: 13.2     |
|=========================================+========================+======================|
|   0  NVIDIA GeForce RTX 3070 ...     On |   00000000:01:00.0 Off |                  N/A |
| N/A   42C    P8             14W / 115W  |       1MiB /   8192MiB |      0%      Default |
|                                         |                        |                  N/A |
+-----------------------------------------+------------------------+----------------------+
```

### Driver API Validation (Python / C / CUDA)

Run the included standalone C tester:
```bash
gcc scripts/cuda_probe.c -ldl -o /tmp/cuda_probe && /tmp/cuda_probe
```

**Expected Healthy Output:**
```text
CUDA driver library loaded successfully (libcuda.so.1).
cuInit(0) returned CUDA_SUCCESS (0).
Detected 1 CUDA device(s).
Device 0: NVIDIA GeForce RTX 3070 Laptop GPU
CUDA context created and destroyed cleanly!
Silicon state: OPERATIONAL.
```

---

## 📦 Automated Scripts Included

- **`bin/nvidia-triage`**: Fully autonomous diagnostic scanner that inspects kernel logs, Xid events, process locks, and PCIe power states.
- **`bin/nvidia-recover`**: Guided/automated PCIe link remove-and-rescan script.
- **`scripts/cuda_probe.c`**: Bare-metal CUDA driver API test using dynamic runtime loading (`dlopen`). Zero SDK installation required.

---

## 📜 License

MIT License. Designed for Linux systems administrators, machine learning engineers, and autonomous agent environments.
