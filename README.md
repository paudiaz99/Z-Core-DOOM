Doom classic port to Z-Core
=======================================

This is a port of classic Doom to simple RISC-V platforms, originally by
[smunaut](https://github.com/smunaut/doom_riscv), adapted to run on the
**Z-Core**, a custom RV32IMZicsr soft-core implemented on FPGA with SDRAM,
VGA output, and hardware timer support.

The code is structured to make platform adaptation straightforward, with
platform-specific logic cleanly separated from the core game code.

A buildable Linux/X11 version is kept available to allow local testing
without hardware.

## Z-Core Platform

- CPU: RV32IMZicsr soft-core
- Display: VGA controller (320x200)
- Memory: 64MB SDRAM
- Input: handled via `doom_input.py`

## Related

- [Z-Core-FPGA](https://github.com/paudiaz99/Z-Core-FPGA) — the FPGA SoC this port targets
