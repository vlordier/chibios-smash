#!/usr/bin/env python3
"""
SMASH + ChibiOS HITL Controller
Runs on macOS, communicates with CUAV Nora via USB serial

Usage:
  python3 hitl_controller.py              # Run all tests
  python3 hitl_controller.py --port /dev/cu.usbmodem1234
  python3 hitl_controller.py --test MUTEX_BASIC
  python3 hitl_controller.py --monitor    # Live monitoring mode
"""

import serial
import serial.tools.list_ports
import time
import json
import argparse
import sys
from datetime import datetime
from typing import List, Dict, Optional

class SmashHitlController:
    """HITL controller for SMASH + ChibiOS hardware testing"""
    
    def __init__(self, port: str = None, baud: int = 115200):
        self.port = port
        self.baud = baud
        self.ser = None
        self.results: List[Dict] = []
        self.verbose = True
        
    def find_nora_port(self) -> Optional[str]:
        """Auto-detect CUAV Nora serial port"""
        if self.port:
            return self.port
            
        # Common VID/PID for STM32 USB CDC
        stm32_ports = [
            (0x0483, 0x5740),  # STMicroelectronics Virtual COM Port
            (0x0483, 0x374B),  # STM32F407
            (0x0483, 0x374D),  # STM32F429
        ]
        
        ports = serial.tools.list_ports.comports()
        for p in ports:
            # Check for STM32 VID/PID
            for vid, pid in stm32_ports:
                if p.vid == vid and p.pid == pid:
                    print(f"✓ Found STM32 device: {p.device}")
                    return p.device
            
            # Check for USB serial keywords in description
            if any(kw in p.description.lower() for kw in ['cuav', 'nora', 'stm32', 'virtual com']):
                print(f"✓ Found candidate device: {p.device} ({p.description})")
                return p.device
        
        # Fallback: list all USB serial ports
        usb_ports = [p.device for p in ports if 'usb' in p.device.lower()]
        if usb_ports:
            print(f"⚠ Found USB serial ports (manual selection needed):")
            for p in usb_ports:
                print(f"    {p}")
            return usb_ports[0] if usb_ports else None
        
        return None
    
    def connect(self, timeout: int = 5) -> bool:
        """Connect to Nora via USB serial"""
        detected_port = self.find_nora_port()
        if not detected_port:
            print("✗ No CUAV Nora detected")
            return False
        
        try:
            print(f"  Connecting to {detected_port} at {self.baud} baud...")
            self.ser = serial.Serial(detected_port, self.baud, timeout=1)
            time.sleep(2)  # Wait for reset
            
            # Try to get prompt
            self.ser.reset_input_buffer()
            self.send_command("", wait=False)
            time.sleep(0.5)
            
            print(f"✓ Connected to {detected_port}")
            return True
            
        except serial.SerialException as e:
            print(f"✗ Failed to connect: {e}")
            print("\nTroubleshooting:")
            print("  1. Check Nora is powered and USB connected")
            print("  2. Run: ls -l /dev/cu.usbmodem*")
            print("  3. Check permissions: sudo chmod 666 /dev/cu.usbmodemXXXX")
            return False
    
    def send_command(self, cmd: str, wait: bool = True):
        """Send test command to Nora"""
        if self.ser:
            self.ser.write(f"{cmd}\n".encode())
            if wait:
                time.sleep(0.1)
    
    def read_response(self, timeout: float = 5.0) -> List[str]:
        """Read test results from Nora"""
        start = time.time()
        lines = []
        
        while time.time() - start < timeout:
            if self.ser and self.ser.in_waiting:
                try:
                    line = self.ser.readline().decode('utf-8', errors='ignore').strip()
                    if line:
                        lines.append(line)
                        if self.verbose:
                            print(f"  NORA: {line}")
                        if "TEST_COMPLETE" in line or "ERROR" in line:
                            break
                except Exception as e:
                    if self.verbose:
                        print(f"  Decode error: {e}")
            time.sleep(0.01)
        
        return lines
    
    def run_test(self, test_name: str, timeout: float = 10.0) -> Dict:
        """Run a specific HITL test"""
        print(f"\n{'='*60}")
        print(f"TEST: {test_name}")
        print('='*60)
        
        start_time = time.time()
        self.send_command(f"TEST {test_name}")
        results = self.read_response(timeout=timeout)
        elapsed = time.time() - start_time
        
        test_result = {
            'test': test_name,
            'timestamp': datetime.now().isoformat(),
            'elapsed_sec': elapsed,
            'output': results,
            'success': any('PASS' in r for r in results) or any('TEST_COMPLETE' in r for r in results),
            'errors': [r for r in results if 'ERROR' in r or 'FAIL' in r]
        }
        
        self.results.append(test_result)
        
        # Summary
        if test_result['success'] and not test_result['errors']:
            print(f"✓ {test_name} PASSED ({elapsed:.2f}s)")
        else:
            print(f"✗ {test_name} FAILED or had errors")
            for err in test_result['errors']:
                print(f"    {err}")
        
        return test_result
    
    def run_all_tests(self) -> bool:
        """Run full HITL test suite"""
        print("\n" + "="*60)
        print("SMASH + ChibiOS HITL Test Suite")
        print("Target: CUAV Nora (STM32H7)")
        print("="*60)
        
        tests = [
            ('SYSTEM_INFO', 5.0),       # Get hardware info
            ('MUTEX_BASIC', 5.0),       # Basic mutex test
            ('MUTEX_CONTENTION', 10.0), # Contention stress test
            ('SEMAPHORE_BASIC', 5.0),   # Basic semaphore test
            ('CONDVAR_SIGNAL', 5.0),    # Condition variable test
            ('MAILBOX_FIFO', 5.0),      # Mailbox FIFO test
            ('PRIORITY_INVERSION', 10.0), # Priority inversion test
            ('STACK_USAGE', 5.0),       # Stack high-water mark
            ('SCHEDULER_LATENCY', 10.0), # Scheduling latency
        ]
        
        for test_name, timeout in tests:
            self.run_test(test_name, timeout=timeout)
        
        self.save_results()
        self.print_summary()
        
        # Return True if all tests passed
        return all(r['success'] and not r['errors'] for r in self.results)
    
    def save_results(self, filename: str = None):
        """Save test results to JSON"""
        if not filename:
            filename = f"hitl_results_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json"
        
        with open(filename, 'w') as f:
            json.dump({
                'version': '2.0.0-alpha',
                'target': 'CUAV Nora (STM32H7)',
                'host': 'macOS',
                'timestamp': datetime.now().isoformat(),
                'tests': self.results,
                'summary': {
                    'total': len(self.results),
                    'passed': sum(1 for r in self.results if r['success'] and not r['errors']),
                    'failed': sum(1 for r in self.results if not r['success'] or r['errors'])
                }
            }, f, indent=2)
        
        print(f"\n✓ Results saved to {filename}")
    
    def print_summary(self):
        """Print test summary"""
        print("\n" + "="*60)
        print("TEST SUMMARY")
        print("="*60)
        
        passed = sum(1 for r in self.results if r['success'] and not r['errors'])
        failed = len(self.results) - passed
        
        print(f"  Total:  {len(self.results)}")
        print(f"  Passed: {passed} ✓")
        print(f"  Failed: {failed} ✗")
        print(f"  Success Rate: {100*passed/len(self.results):.1f}%")
        
        if failed > 0:
            print("\n  Failed tests:")
            for r in self.results:
                if not r['success'] or r['errors']:
                    print(f"    - {r['test']}")
    
    def monitor_mode(self):
        """Live monitoring mode - show real-time output from Nora"""
        print("\n" + "="*60)
        print("MONITOR MODE")
        print("Press Ctrl+C to exit")
        print("="*60)
        
        try:
            while True:
                if self.ser and self.ser.in_waiting:
                    try:
                        line = self.ser.readline().decode('utf-8', errors='ignore').strip()
                        if line:
                            print(f"[{datetime.now().strftime('%H:%M:%S')}] {line}")
                    except Exception:
                        pass
                time.sleep(0.01)
        except KeyboardInterrupt:
            print("\n✓ Monitor mode exited")
    
    def disconnect(self):
        """Disconnect from Nora"""
        if self.ser:
            self.send_command("EXIT", wait=False)
            self.ser.close()
            print("\n✓ Disconnected")


def main():
    parser = argparse.ArgumentParser(description='SMASH + ChibiOS HITL Controller')
    parser.add_argument('--port', '-p', help='Serial port (auto-detect if not specified)')
    parser.add_argument('--baud', '-b', type=int, default=115200, help='Baud rate (default: 115200)')
    parser.add_argument('--test', '-t', help='Run specific test (default: all tests)')
    parser.add_argument('--monitor', '-m', action='store_true', help='Live monitoring mode')
    parser.add_argument('--quiet', '-q', action='store_true', help='Quiet mode (less output)')
    
    args = parser.parse_args()
    
    controller = SmashHitlController(port=args.port, baud=args.baud)
    controller.verbose = not args.quiet
    
    if controller.connect():
        try:
            if args.monitor:
                controller.monitor_mode()
            elif args.test:
                controller.run_test(args.test)
                controller.save_results()
            else:
                success = controller.run_all_tests()
                sys.exit(0 if success else 1)
        except KeyboardInterrupt:
            print("\n✗ Interrupted")
            sys.exit(1)
        finally:
            controller.disconnect()
    else:
        print("\nAvailable serial ports:")
        ports = serial.tools.list_ports.comports()
        for p in ports:
            print(f"  {p.device} - {p.description}")
        sys.exit(1)


if __name__ == '__main__':
    main()
