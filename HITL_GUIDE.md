# SMASH + Hardware HITL Guide

**Date:** 2026-04-18  
**Target Hardware:** CUAV Nora (STM32H7)  
**Host:** macOS

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                         YOUR MAC                                │
│  ┌─────────────┐  ┌──────────────┐  ┌──────────────────────┐   │
│  │   SMASH     │  │  ChibiOS     │  │   HITL Framework     │   │
│  │  (Model     │  │  Simulator   │  │   (Python/Serial)    │   │
│  │   Checker)  │  │  (POSIX)     │  │                      │   │
│  └─────────────┘  └──────────────┘  └──────────────────────┘   │
│         │                │                      │               │
│         └────────────────┴──────────────────────┘               │
│                          │                                      │
└──────────────────────────┼──────────────────────────────────────┘
                           │ USB/Serial
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│                      CUAV Nora                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  Custom ChibiOS Firmware with SMASH Test Hooks          │    │
│  │  - Thread concurrency tests                              │    │
│  │  - Mutex/semaphore stress tests                          │    │
│  │  - Mailbox/condvar tests                                 │    │
│  │  - Real-time performance metrics                         │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                                 │
│  STM32H743II (Cortex-M7 @ 480MHz)                              │
│  512KB RAM, 2MB Flash                                          │
│  IMU, Baro, Compass, GPS                                       │
└─────────────────────────────────────────────────────────────────┘
```

---

## What SMASH Does vs HITL

| Aspect | SMASH (Model Checker) | HITL (Hardware Testing) |
|--------|----------------------|------------------------|
| **Runs on** | macOS (host) | CUAV Nora (hardware) |
| **Purpose** | Verify concurrency patterns | Test real hardware behavior |
| **Coverage** | All interleavings (exhaustive) | Specific scenarios only |
| **Speed** | Fast (simulation) | Slower (real hardware) |
| **Finds** | Deadlocks, races, inversions | Timing issues, HW bugs |
| **Complement** | ✅ Use BOTH for complete verification |

---

## Prerequisites

### 1. Install ARM Toolchain

```bash
# Install ARM GCC for Cortex-M7
brew install --cask gcc-arm-embedded

# Verify
arm-none-eabi-gcc --version
```

### 2. Install Serial Tools

```bash
# For USB serial communication
brew install pyserial minicom
```

### 3. Verify Nora Connection

```bash
# List USB devices
system_profiler SPUSBDataType

# Find serial port
ls -l /dev/cu.usbserial*
# or
ls -l /dev/cu.usbmodem*

# Expected: /dev/cu.usbmodemXXXX or /dev/cu.usbserial-XXXX
```

---

## Step 1: ChibiOS Port for CUAV Nora

### Hardware Specifications

| Component | Specification |
|-----------|---------------|
| **MCU** | STM32H743II (Cortex-M7) |
| **Clock** | 480 MHz |
| **RAM** | 512 KB |
| **Flash** | 2 MB |
| **UART** | Multiple (for USB serial) |

### Directory Structure

```
chibios-nora/
├── boards/
│   └── CUAV_NORA/
│       ├── board.c
│       ├── board.h
│       └── board.mk
├── configs/
│   └── smash_hitl/
│       ├── chconf.h
│       ├── halconf.h
│       └── mcuconf.h
├── src/
│   ├── main.c
│   ├── smash_hitl_tests.c
│   └── smash_hitl_tests.h
├── Makefile
└── README.md
```

---

## Step 2: HITL Test Framework

### Test Categories

1. **Concurrency Tests** (verify SMASH findings on hardware)
2. **Performance Tests** (measure real timing)
3. **Stress Tests** (long-running stability)
4. **Integration Tests** (with sensors/ peripherals)

### Example Test: Mutex Contention

```c
/* On Nora hardware */
static mutex_t test_mutex;
static volatile int counter = 0;
static volatile bool test_complete = false;

static THD_WORKING_AREA(waThread1, 512);
static THD_FUNCTION(Thread1, arg) {
    (void)arg;
    while (!test_complete) {
        chMtxLock(&test_mutex);
        counter++;
        chMtxUnlock(&test_mutex);
        chThdSleepMilliseconds(10);
    }
}

static THD_WORKING_AREA(waThread2, 512);
static THD_FUNCTION(Thread2, arg) {
    (void)arg;
    while (!test_complete) {
        chMtxLock(&test_mutex);
        counter++;
        chMtxUnlock(&test_mutex);
        chThdSleepMilliseconds(15);
    }
}

/* Start test via serial command */
void hitl_start_mutex_test(void) {
    counter = 0;
    test_complete = false;
    chMtxObjectInit(&test_mutex);
    chThdCreateStatic(waThread1, sizeof(waThread1), NORMALPRIO, Thread1, NULL);
    chThdCreateStatic(waThread2, sizeof(waThread2), NORMALPRIO, Thread2, NULL);
}

/* Report results via serial */
void hitl_report_mutex_test(void) {
    test_complete = true;
    chprintf(USB_SERIAL, "Mutex test: counter=%d (expected ~no lost updates)\r\n", counter);
}
```

---

## Step 3: Python HITL Host (macOS)

### Installation

```bash
pip3 install pyserial matplotlib
```

### HITL Controller (`hitl_controller.py`)

```python
#!/usr/bin/env python3
"""
SMASH + ChibiOS HITL Controller
Runs on macOS, communicates with CUAV Nora via USB serial
"""

import serial
import time
import json
from datetime import datetime

class SmashHitlController:
    def __init__(self, port='/dev/cu.usbmodem1234', baud=115200):
        self.port = port
        self.baud = baud
        self.ser = None
        self.results = []
        
    def connect(self):
        """Connect to Nora via USB serial"""
        try:
            self.ser = serial.Serial(self.port, self.baud, timeout=1)
            print(f"✓ Connected to {self.port}")
            time.sleep(2)  # Wait for reset
            return True
        except serial.SerialException as e:
            print(f"✗ Failed to connect: {e}")
            print("  Check: ls -l /dev/cu.usbmodem*")
            return False
    
    def send_command(self, cmd):
        """Send test command to Nora"""
        if self.ser:
            self.ser.write(f"{cmd}\n".encode())
            time.sleep(0.1)
    
    def read_response(self, timeout=5):
        """Read test results from Nora"""
        start = time.time()
        lines = []
        while time.time() - start < timeout:
            if self.ser and self.ser.in_waiting:
                line = self.ser.readline().decode('utf-8').strip()
                lines.append(line)
                print(f"NORA: {line}")
                if "TEST_COMPLETE" in line:
                    break
        return lines
    
    def run_test(self, test_name):
        """Run a specific HITL test"""
        print(f"\n=== Running {test_name} ===")
        self.send_command(f"TEST {test_name}")
        results = self.read_response(timeout=10)
        self.results.append({
            'test': test_name,
            'timestamp': datetime.now().isoformat(),
            'output': results
        })
        return results
    
    def run_all_tests(self):
        """Run full HITL test suite"""
        tests = [
            'MUTEX_BASIC',
            'MUTEX_CONTENTION',
            'SEMAPHORE_BASIC',
            'CONDVAR_SIGNAL',
            'MAILBOX_FIFO',
            'PRIORITY_INVERSION',
            'STACK_USAGE',
            'SCHEDULER_LATENCY'
        ]
        
        for test in tests:
            self.run_test(test)
        
        self.save_results()
    
    def save_results(self):
        """Save test results to JSON"""
        filename = f"hitl_results_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json"
        with open(filename, 'w') as f:
            json.dump(self.results, f, indent=2)
        print(f"\n✓ Results saved to {filename}")
    
    def disconnect(self):
        """Disconnect from Nora"""
        if self.ser:
            self.ser.close()
            print("✓ Disconnected")

def main():
    controller = SmashHitlController()
    
    if controller.connect():
        try:
            controller.run_all_tests()
        except KeyboardInterrupt:
            print("\n✗ Interrupted")
        finally:
            controller.disconnect()
    else:
        print("\nAvailable serial ports:")
        import glob
        ports = glob.glob('/dev/cu.usb*')
        for p in ports:
            print(f"  {p}")

if __name__ == '__main__':
    main()
```

---

## Step 4: Comparison - SMASH vs HITL Results

### Example: Mutex Contention

**SMASH (Model Checker):**
```
=== test_mutex_deadlock ===
Interleavings explored : 2
States visited         : 35
Pruned by cache        : 16
Deadlocks found        : 2
✓ Exhaustive: found ALL deadlocking interleavings
```

**HITL (Hardware):**
```
NORA: TEST MUTEX_CONTENTION
NORA: Thread1 iterations: 523
NORA: Thread2 iterations: 349
NORA: Total counter: 872 (no lost updates)
NORA: Max latency: 45µs
NORA: TEST_COMPLETE
✓ Real hardware: verified no lost updates under load
```

### Why Both Matter

| Bug Type | Found by SMASH? | Found by HITL? |
|----------|-----------------|----------------|
| Deadlock | ✅ Yes (exhaustive) | ❌ Maybe (depends on timing) |
| Race Condition | ✅ Yes | ❌ Maybe |
| Priority Inversion | ✅ Yes | ✅ Yes (with instrumentation) |
| Stack Overflow | ❌ No (abstract) | ✅ Yes (real hardware) |
| Timing Issues | ❌ No (logical) | ✅ Yes (real clocks) |
| Hardware Bugs | ❌ No | ✅ Yes |

---

## Quick Start

### 1. Verify Connection

```bash
# On macOS
ls -l /dev/cu.usbmodem*
# Should show: /dev/cu.usbmodemXXXX

# Test serial connection
screen /dev/cu.usbmodemXXXX 115200
# Type: HELP
# Should respond with available commands
```

### 2. Run HITL Tests

```bash
cd chibios-smash/hitl/
python3 hitl_controller.py
```

### 3. Compare with SMASH

```bash
# Run SMASH model checking
cd chibios-smash/
make test

# Compare results
# SMASH: Finds ALL possible deadlocks
# HITL: Confirms real hardware behavior matches model
```

---

## Advanced: Automated Regression

### CI/CD Pipeline

```yaml
# .github/workflows/hitl.yml
name: HITL Tests

on: [push, pull_request]

jobs:
  smash-verification:
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v2
      - name: Run SMASH
        run: make test
      
  hitl-hardware:
    runs-on: self-hosted  # Mac with Nora connected
    steps:
      - uses: actions/checkout@v2
      - name: Run HITL
        run: python3 hitl/hitl_controller.py
      - name: Upload Results
        uses: actions/upload-artifact@v2
        with:
          name: hitl-results
          path: hitl_results_*.json
```

---

## Troubleshooting

### Nora Not Detected

```bash
# Check USB permissions
ls -l /dev/cu.usbmodem*

# If no permissions:
sudo chmod 666 /dev/cu.usbmodemXXXX

# Or add to dialout group
sudo dseditgroup -o edit -a $USER -t user dialout
```

### Serial Port Busy

```bash
# Kill processes using serial port
lsof | grep tty.usbmodem
kill -9 <PID>
```

### Build Errors

```bash
# Clean and rebuild
make clean
make all

# Check toolchain
arm-none-eabi-gcc --version
# Should be >= 10.0
```

---

## Next Steps

1. **Port ChibiOS to Nora** - Use STM32H7 reference + Nora pinout
2. **Implement Test Suite** - Start with mutex/semaphore tests
3. **Add Sensor Integration** - Test concurrency with IMU/baro data
4. **Performance Profiling** - Measure real scheduling latency
5. **Long-Running Tests** - Stability over hours/days

---

## References

- **CUAV Nora:** https://www.cuav.net/en/nora/
- **ChibiOS:** https://www.chibios.org/
- **STM32H743:** https://www.st.com/en/microcontrollers-microprocessors/stm32h743ii.html
- **SMASH:** This repository

---

*Guide created: 2026-04-18*  
*SMASH version: 2.0.0-alpha*  
*Target: CUAV Nora on macOS*
