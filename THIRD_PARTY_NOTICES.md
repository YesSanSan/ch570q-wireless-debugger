# Third-party notices

## WCH CH570 / CH572 SDK

- Source: https://github.com/openwch/ch570
- Pinned revision: `1e34c9450cd6df390153952c097cd09dd8a91aed` (EVT v1.3).
- Location: `vendor/ch570`, tracked as a Git submodule, with upstream files unchanged.
- The upstream repository includes an Apache-2.0 `LICENSE`. Individual source/header notices also state that the software (modified or not) and binary are used for microcontrollers manufactured by Nanjing Qinheng Microelectronics. Retain and review those file notices; the project's MIT license does not override them.
- `libCH57xRF.a` and `libISP572.a` are upstream precompiled libraries, not source provided by this project. Review the upstream terms and contact WCH if clarification is needed before redistributing binaries or using the SDK on other manufacturers' devices.
- Build-time compatibility adaptations of `CH57x_sys.c`, `Link.ld` and the optional HID reference are generated under the build directory. They retain upstream notices and do not modify the submodule.

## STM32 OLED example

`examples/ks2-oled/` is a separately built target application licensed under **GPL-3.0-or-later**, including its local supporting files. Its SSD1306 initialization and page addressing are derived from the KS2 OLED driver by **Guilherme Amorim and Renan Guedes (2020)**. Original attribution is retained in `src/ssd1306.c`, and the full GPLv3 text is in `COPYING` within the example. It is not linked into the CH570Q firmware.

## Host dependencies

Python tools use separately installed `hidapi` and `pyserial`. Their packages retain their own licenses. CMake, Ninja, GCC, OpenOCD, GDB and WCH ISP are external tools and are not bundled in this repository.
