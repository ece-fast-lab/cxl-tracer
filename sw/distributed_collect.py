#!/usr/bin/env python3
"""
Distributed Hardware Trace Collection Script

This script orchestrates trace collection across two machines:
- Controller (local): Runs trace controller and target workload
- Trace receiver (remote): Runs trace receiver via SSH

NOTE: This script should be run with sudo for proper hardware access and process control.

Usage:
    sudo python3 distributed_collect.py [options] -- <target_command>
    
Examples:
    # Run for specific duration (30 seconds)
    sudo python3 distributed_collect.py --trace-receiver-ip 192.168.1.100 --pci-device 0000:49:00.1 --duration 30 -- \
        numactl -m 1 -N 1 ./zstd -f test_data_1gb.bin

    # Run until target command finishes (using defaults)
    sudo python3 distributed_collect.py -- \
        numactl -m 1 -N 1 ./my_app arg1
        
    # Explicitly follow target completion with custom settings
    sudo python3 distributed_collect.py --trace-receiver-ip 10.0.1.50 --duration 0 -- \
        numactl -m 1 -N 1 ./my_app arg1

Requirements:
    - Run with sudo for PCI device access and process control
    - SSH access to trace receiver machine (keys should be set up for root user or target user)
    - trace_controller and trace_receiver binaries compiled on both machines
    - Passwordless sudo configured on trace receiver machine for trace_receiver binary
    
Setup on trace receiver machine:
    1. Configure passwordless sudo for trace_receiver:
       sudo visudo
       # Add line: username ALL=(ALL) NOPASSWD: /path/to/trace_receiver
    
    2. Alternative: SSH as root directly:
       # Enable root SSH in /etc/ssh/sshd_config: PermitRootLogin yes
       # Use --trace-receiver-user root when running the script
"""

import argparse
import os
import signal
import subprocess
import sys
import time
import threading
from pathlib import Path


class DistributedTraceCollector:
    def __init__(
        self,
        trace_receiver_ip,
        pci_device,
        trace_receiver_user="root",
        trace_file="/tmp/trace_record.bin",
        buffer_size_gb=10,
        trace_receiver_path="/fast-lab-share/srikarv2/cxl-experiments/mem_trace/collection",
    ):
        self.trace_receiver_ip = trace_receiver_ip
        self.trace_receiver_user = trace_receiver_user
        self.pci_device = pci_device
        self.trace_file = trace_file
        self.buffer_size_gb = buffer_size_gb
        self.trace_receiver_path = trace_receiver_path

        # Process handles
        self.receiver_process = None
        self.controller_process = None
        self.target_process = None

        # Control flags
        self.should_stop = False

        # Setup signal handling
        signal.signal(signal.SIGINT, self._signal_handler)
        signal.signal(signal.SIGTERM, self._signal_handler)

    def _signal_handler(self, signum, frame):
        print(f"\nReceived signal {signum}. Stopping trace collection...")
        self.should_stop = True

    def start_receiver(self):
        """Start the trace receiver via SSH"""
        print(f"Starting trace receiver on {self.trace_receiver_ip}...")

        # Build SSH command to start receiver
        receiver_cmd = f"cd {self.trace_receiver_path} && sudo ./trace_receiver {self.trace_file} {self.buffer_size_gb}"
        ssh_cmd = [
            "ssh",
            f"{self.trace_receiver_user}@{self.trace_receiver_ip}",
            receiver_cmd,
        ]

        try:
            self.receiver_process = subprocess.Popen(
                ssh_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
            )
            print(f"Receiver started on {self.trace_receiver_ip}")

            # Give receiver time to initialize and start listening
            time.sleep(3)

            # Check if receiver is still running
            if self.receiver_process.poll() is not None:
                stdout, stderr = self.receiver_process.communicate()
                print(
                    f"Receiver exited early with code: {self.receiver_process.returncode}"
                )
                if stdout.strip():
                    print(f"Receiver stdout: {stdout}")
                if stderr.strip():
                    print(f"Receiver stderr: {stderr}")
                return False

            return True

        except Exception as e:
            print(f"Failed to start receiver: {e}")
            return False

    def start_controller(self):
        """Start the trace controller on local machine"""
        print("Starting trace controller...")

        # Get the directory where this script is located
        script_dir = os.path.dirname(os.path.abspath(__file__))
        controller_path = os.path.join(script_dir, "trace_controller")

        controller_cmd = [
            controller_path,
            self.pci_device,
            self.trace_receiver_ip,
        ]

        try:
            self.controller_process = subprocess.Popen(
                controller_cmd,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            print("Controller started")

            # Give controller time to connect and setup
            time.sleep(2)

            # Check if controller is still running
            if self.controller_process.poll() is not None:
                stdout, stderr = self.controller_process.communicate()
                print(
                    f"Controller exited early with code: {self.controller_process.returncode}"
                )
                if stdout.strip():
                    print(f"Controller stdout: {stdout}")
                if stderr.strip():
                    print(f"Controller stderr: {stderr}")
                return False

            return True

        except Exception as e:
            print(f"Failed to start controller: {e}")
            return False

    def run_target_command(self, target_cmd, duration=None, follow_target=False):
        """Run the target command with trace collection"""
        if not target_cmd:
            print("No target command provided")
            return False

        print(f"Target command: {' '.join(target_cmd)}")

        # Send enter to controller to start trace collection
        if self.controller_process and self.controller_process.stdin:
            self.controller_process.stdin.write("\n")
            self.controller_process.stdin.flush()

        print("Starting trace collection...")
        time.sleep(1)

        # Start target command
        print("Executing target command...")
        try:
            self.target_process = subprocess.Popen(
                target_cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,  # Merge stderr into stdout
                text=True,
                bufsize=1,  # Line buffered
                universal_newlines=True,
            )
        except Exception as e:
            print(f"Failed to start target command: {e}")
            return False

        # Wait based on strategy
        if follow_target:
            # Wait for target to complete
            print("Waiting for target command to complete...")
            print("=== Target Program Output ===")
            while self.target_process.poll() is None and not self.should_stop:
                # Read and display target output in real-time
                try:
                    line = self.target_process.stdout.readline()
                    if line:
                        print(f"TARGET: {line.rstrip()}")
                except:
                    pass
                time.sleep(0.1)

            # Get any remaining output
            try:
                remaining_output, _ = self.target_process.communicate(timeout=1)
                if remaining_output:
                    for line in remaining_output.split("\n"):
                        if line.strip():
                            print(f"TARGET: {line.rstrip()}")
            except:
                pass

            if not self.should_stop:
                return_code = self.target_process.wait()
                print(f"=== Target Program Finished (exit code: {return_code}) ===")

        elif duration:
            # Wait for specified duration
            print(f"Running trace collection for {duration} seconds...")
            print("=== Target Program Output ===")
            start_time = time.time()

            while (time.time() - start_time) < duration and not self.should_stop:
                # Check if target command finished early
                if self.target_process.poll() is not None:
                    print("Target command finished before duration elapsed")
                    break

                # Read and display target output
                try:
                    line = self.target_process.stdout.readline()
                    if line:
                        print(f"TARGET: {line.rstrip()}")
                except:
                    pass
                time.sleep(0.1)

        else:
            # Default: wait for manual interruption
            print("Trace collection running. Press Ctrl+C to stop...")
            print("=== Target Program Output ===")
            while not self.should_stop:
                # Check if target command finished
                if self.target_process.poll() is not None:
                    print("Target command finished")
                    break

                # Read and display target output
                try:
                    line = self.target_process.stdout.readline()
                    if line:
                        print(f"TARGET: {line.rstrip()}")
                except:
                    pass
                time.sleep(0.1)

        return True

    def stop_collection(self):
        """Stop trace collection cleanly"""
        print("Stopping trace collection...")

        # Stop target if still running
        if self.target_process and self.target_process.poll() is None:
            print("Terminating target command...")
            self.target_process.terminate()
            try:
                self.target_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.target_process.kill()

        # Stop controller (send SIGINT to trigger cleanup)
        if self.controller_process and self.controller_process.poll() is None:
            print("Stopping trace controller...")
            self.controller_process.send_signal(signal.SIGINT)
            try:
                self.controller_process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.controller_process.kill()

        # Stop receiver via SSH
        if self.receiver_process and self.receiver_process.poll() is None:
            print("Stopping trace receiver...")
            # Send Ctrl+C to the SSH session
            self.receiver_process.send_signal(signal.SIGINT)
            try:
                self.receiver_process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                # Force kill SSH connection
                self.receiver_process.kill()

    def run(self, target_cmd, duration=None, follow_target=False):
        """Main execution flow"""
        try:
            # Start receiver on trace receiver machine
            if not self.start_receiver():
                return False

            # Start controller on local machine
            if not self.start_controller():
                self.stop_collection()
                return False

            # Run target command with trace collection
            success = self.run_target_command(target_cmd, duration, follow_target)

            # Stop everything cleanly
            self.stop_collection()

            if success:
                print("--- Trace Collection Complete ---")
                print(f"Trace file saved on {self.trace_receiver_ip}:{self.trace_file}")

                # Try to extract final statistics from controller output
                if self.controller_process:
                    try:
                        # Get any remaining output from controller
                        stdout, stderr = self.controller_process.communicate(timeout=2)

                        # Look for statistics in the output
                        if stdout:
                            lines = stdout.split("\n")
                            for line in lines:
                                if (
                                    "Dropped traces:" in line
                                    or "Written traces:" in line
                                ):
                                    print(line.strip())

                    except (subprocess.TimeoutExpired, ValueError):
                        # Controller already finished or no stats available
                        pass

                # Also try to get statistics from receiver output
                if self.receiver_process:
                    try:
                        stdout, stderr = self.receiver_process.communicate(timeout=2)

                        # Look for statistics in receiver output
                        if stdout:
                            lines = stdout.split("\n")
                            print("\nTrace Statistics:")
                            for line in lines:
                                if any(
                                    keyword in line
                                    for keyword in [
                                        "Dropped traces:",
                                        "Written traces:",
                                        "Trace statistics:",
                                        "File size:",
                                    ]
                                ):
                                    print(f"  {line.strip()}")

                    except (subprocess.TimeoutExpired, ValueError):
                        pass

            return success

        except Exception as e:
            print(f"Error during trace collection: {e}")
            # Print any remaining output from processes
            if self.controller_process and self.controller_process.poll() is not None:
                try:
                    stdout, stderr = self.controller_process.communicate(timeout=1)
                    if stdout.strip():
                        print(f"Final controller stdout: {stdout}")
                    if stderr.strip():
                        print(f"Final controller stderr: {stderr}")
                except:
                    pass

            if self.receiver_process and self.receiver_process.poll() is not None:
                try:
                    stdout, stderr = self.receiver_process.communicate(timeout=1)
                    if stdout.strip():
                        print(f"Final receiver stdout: {stdout}")
                    if stderr.strip():
                        print(f"Final receiver stderr: {stderr}")
                except:
                    pass

            self.stop_collection()
            return False


def main():
    # Check if running as root
    if os.geteuid() != 0:
        print("Error: This script must be run with sudo for proper hardware access.")
        print(
            "Usage: sudo python3 distributed_collect.py [options] -- <target_command>"
        )
        return 1

    parser = argparse.ArgumentParser(
        description="Distributed hardware trace collection script",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )

    parser.add_argument(
        "--trace-receiver-ip",
        default="192.17.101.173",
        help="IP address of trace receiver (default: 192.17.101.173)",
    )
    parser.add_argument(
        "--pci-device",
        default="0000:40:00.1",
        help="PCI device ID (default: 0000:40:00.1)",
    )

    parser.add_argument(
        "--trace-receiver-user",
        default="srikarv2",
        help="Username for SSH to trace receiver (default: srikarv2)",
    )
    parser.add_argument(
        "--trace-file",
        default="/tmp/trace_record.bin",
        help="Output trace file path on trace receiver",
    )
    parser.add_argument(
        "--buffer-size",
        type=int,
        default=10,
        help="Trace buffer size in GB (default: 10)",
    )
    parser.add_argument(
        "--trace-receiver-path",
        # default="/fast-lab-share/srikarv2/cxl-experiments/mem_trace/collection",
        default="/home/srikarv2",
        help="Path to trace programs on trace receiver",
    )

    # Execution mode options
    parser.add_argument(
        "--duration",
        type=int,
        default=0,
        help="Run trace collection for specified seconds (positive value), or follow target command completion (0 or negative, default: 0)",
    )

    parser.add_argument(
        "target_cmd", nargs="*", help="Target command to run while tracing"
    )

    args = parser.parse_args()

    if not args.target_cmd:
        print("Error: No target command provided")
        parser.print_help()
        return 1

    # Create collector instance
    collector = DistributedTraceCollector(
        trace_receiver_ip=args.trace_receiver_ip,
        pci_device=args.pci_device,
        trace_receiver_user=args.trace_receiver_user,
        trace_file=args.trace_file,
        buffer_size_gb=args.buffer_size,
        trace_receiver_path=args.trace_receiver_path,
    )

    # Run collection
    follow_target = args.duration <= 0
    duration = args.duration if args.duration > 0 else None

    success = collector.run(
        target_cmd=args.target_cmd,
        duration=duration,
        follow_target=follow_target,
    )

    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
