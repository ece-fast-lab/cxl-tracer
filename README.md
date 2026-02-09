# CXL-Tracer

This repo is a hardware framework for capturing physical memory access traces from CXL memory device. Built on a commodity FPGA platform acting as a CXL Type-3 memory device, it captures complete memory request streams including timestamps without introducing overhead.

The project archive (`cxl-tracer.qar`) was created with **Quartus 24.3**. The current design is targeted for the **Intel Agilex 7 I series development kit R1BES**.

## Architecture

The setup consists of two machines connected to a single FPGA:

1.  **Target Server (Host):** The machine running the workload. It connects to the FPGA via the **PCIe edge connector** (CXL link).
2.  **Collection Server (Receiver):** A separate machine dedicated to storing the trace data. It connects to the FPGA via the **MCIO connectors** (PCIe link).

## Repository Structure

* **`cxl-tracer.qar`**: Quartus project archive for the FPGA design.
* **`sw/`**: Software and utilities for trace control and collection.
    * **`trace_controller`**: Runs on the Target Server to configure the FPGA CSRs via CXL.io.
    * **`trace_receiver`**: Runs on the Collection Server to drain the trace buffer via PCIe.
    * **`distributed_collect.py`**: The main orchestration script that automates the tracing process across both machines.
    * **`Makefile`**: To compile the C utilities.

## Software Setup

### Compilation

Compile the C utilities on both the Target and Collection servers:

```bash
cd sw
make
```

### Requirements

* **Target Server:**
    * Root access (for PCI access).
    * Python 3.
    * SSH access to the Collection Server.
    * **Note:** When identifying the FPGA on the Target Server, ensure you use the BDF with **function 1** (e.g., `0000:10:00.1`).
* **Collection Server:**
    * Root access (for memory mapping and DAX device access).
    * A configured DAX device (e.g., `/dev/dax0.0`) providing access to the trace buffer memory.

## Usage

The system is controlled entirely from the **Target Server** using the `distributed_collect.py` script. This script starts the remote receiver via SSH, configures the FPGA, and launches your specific workload.

### Basic Syntax

```bash
sudo python3 sw/distributed_collect.py [options] -- <target_workload_command>
```

#### Example

```bash
sudo python3 sw/distributed_collect.py \
    --trace-receiver-ip 192.168.1.100 \
    --pci-device 0000:10:00.1 \
    -- \
    numactl -m 1 ./my_workload
```

For more options, run:
```bash
python3 sw/distributed_collect.py --help
```
