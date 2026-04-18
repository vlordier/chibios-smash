# ChibiOS for CUAV Nora - HITL Template

**Status:** Template/Starting Point  
**Target:** CUAV Nora (STM32H743II)  
**Host:** macOS with HITL controller

---

## Quick Start

### 1. Prerequisites

```bash
# Install ARM toolchain
brew install --cask gcc-arm-embedded

# Install Python HITL dependencies
pip3 install pyserial matplotlib

# Verify tools
arm-none-eabi-gcc --version
python3 -c "import serial; print('pyserial OK')"
```

### 2. Build Firmware

```bash
cd chibios-nora/
make clean
make all
```

### 3. Flash to Nora

```bash
# Using dfu-util (Nora has DFU bootloader)
make flash

# Or using ST-Link
st-flash write build/chibios-nora.bin 0x8000000
```

### 4. Run HITL Tests

```bash
cd ../hitl/
python3 hitl_controller.py
```

---

## Project Structure

```
chibios-nora/
├── boards/
│   └── CUAV_NORA/
│       ├── board.c       # Board initialization
│       ├── board.h       # Board definitions
│       └── board.mk      # Board build rules
├── configs/
│   └── smash_hitl/
│       ├── chconf.h      # ChibiOS configuration
│       ├── halconf.h     # HAL configuration
│       └── mcuconf.h     # MCU-specific config
├── src/
│   ├── main.c            # Main entry point
│   ├── smash_hitl_tests.c # HITL test implementations
│   └── smash_hitl_tests.h # Test declarations
├── build/                # Build output
├── Makefile              # Main build file
└── README.md             # This file
```

---

## Available HITL Commands

When connected via serial terminal:

```
HELP                - Show available commands
TEST <name>         - Run specific test
  MUTEX_BASIC
  MUTEX_CONTENTION
  SEMAPHORE_BASIC
  CONDVAR_SIGNAL
  MAILBOX_FIFO
  PRIORITY_INVERSION
  STACK_USAGE
  SCHEDULER_LATENCY
INFO                - Show system info
RESET               - Reset test statistics
EXIT                - Exit test mode
```

---

## Example Test Output

```
NORA: TEST MUTEX_CONTENTION
NORA: Starting mutex contention test...
NORA: Thread1 created (prio=10)
NORA: Thread2 created (prio=10)
NORA: Running for 5 seconds...
NORA: Thread1 iterations: 523
NORA: Thread2 iterations: 349
NORA: Total counter: 872
NORA: Expected: 872 (no lost updates)
NORA: Max contention latency: 45µs
NORA: TEST_COMPLETE: PASS
```

---

## Troubleshooting

### Nora Not Detected

```bash
# List USB devices
system_profiler SPUSBDataType

# List serial ports
ls -l /dev/cu.usbmodem*

# Fix permissions
sudo chmod 666 /dev/cu.usbmodemXXXX
```

### Build Fails

```bash
# Check toolchain path
which arm-none-eabi-gcc

# Clean rebuild
make clean all

# Verbose build
make V=1
```

### Flash Fails

```bash
# Put Nora in DFU mode (hold bootloader button while plugging in)
dfu-util --list

# Flash via DFU
make flash_dfu
```

---

## Integration with SMASH

### Workflow

1. **SMASH Model Checking** (on macOS)
   ```bash
   cd chibios-smash/
   make test
   # Finds all possible concurrency bugs
   ```

2. **HITL Verification** (on Nora hardware)
   ```bash
   cd hitl/
   python3 hitl_controller.py
   # Confirms real hardware behavior
   ```

3. **Compare Results**
   - SMASH: Exhaustive interleaving analysis
   - HITL: Real hardware timing and behavior
   - Both should agree on correctness

### Example: Priority Inversion

**SMASH finds:**
```
Priority inversion detected!
T2 (high, prio=20) blocked on M0 owned by T0 (low, prio=5)
T1 (med, prio=10) can preempt T0
→ Unbounded inversion possible
```

**HITL confirms:**
```
NORA: TEST PRIORITY_INVERSION
NORA: T0 (low=5) locks M0
NORA: T2 (high=20) blocks on M0
NORA: T0 boosted to prio=20 ✓
NORA: T1 (med=10) cannot preempt ✓
NORA: TEST_COMPLETE: PASS
```

---

## Next Steps

1. **Port ChibiOS to Nora**
   - Use STM32H7xx reference BSP
   - Add Nora-specific pinout
   - Configure UART for USB serial

2. **Implement Test Suite**
   - Start with basic mutex/semaphore
   - Add condvar/mailbox tests
   - Add sensor integration tests

3. **Add Performance Profiling**
   - Scheduling latency measurement
   - ISR latency measurement
   - Memory usage tracking

4. **Long-Running Tests**
   - Stability over hours/days
   - Memory leak detection
   - Stack overflow detection

---

## References

- **CUAV Nora Documentation:** https://www.cuav.net/en/nora/
- **ChibiOS for STM32H7:** https://www.chibios.org/
- **STM32H743 Reference Manual:** https://www.st.com/
- **SMASH Model Checker:** This repository

---

*Template created: 2026-04-18*  
*SMASH version: 2.0.0-alpha*  
*Target: CUAV Nora (STM32H743II)*
