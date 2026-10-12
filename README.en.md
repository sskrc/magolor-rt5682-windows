# magolor-rt5682-windows

[日本語](README.md) | **English**

`jslcodec`: an unofficial Windows driver for the Realtek RT5682 headset codec that makes the headphone jack (headphone output and headset microphone) of the **Acer Chromebook R753T (board "Magolor", Intel Jasper Lake)** work on Windows.

**It does not produce sound on its own.** It is used together with the DSP driver [sskrc/magolor-audio-windows](https://github.com/sskrc/magolor-audio-windows) (`jslaudio`): `jslaudio` sends audio data and clocks over the DSP's SSP0, and `jslcodec` configures the RT5682. They are independent drivers bound to different devices and share no source code.

## About AI authorship

The driver, scripts and documentation in this repository were **written by an AI (Anthropic's Claude, via Claude Code)**. The author directed the work, installed it on real hardware and tested it.
The code has been verified to work on real hardware, but it has **not been exhaustively reviewed by a human**. It is a kernel-mode driver, so use and modify it with appropriate caution.

## Status

| Feature | Status |
|---|---|
| Headphone output (Windows "Headphones") | ✅ |
| Headset microphone (4-pole, Windows "Headset Microphone") | ✅ |
| Jack insertion/removal detection, headphone vs. headset detection | ✅ (polled every 500 ms) |
| Automatic output switching on insertion (reporting jack state to Windows) | ❌ not yet; switch manually |
| Headset buttons | ❌ not supported |

Tested on: Acer Chromebook R753T (Magolor, Celeron N4500), MrChromebox UEFI firmware, Windows 11 Pro (build 26200).

## ⚠ Warnings

- This is an **unofficial, experimental kernel-mode driver**. Bugs can cause blue screens or data loss. Use it at your own risk.
- It is self-signed (test-signed), so Windows **test-signing mode** is required (see the magolor-audio-windows README).
- It binds to `ACPI\10EC5682`. The clocking (24 MHz MCLK) and jack-detection settings are specific to Magolor, so **on other devices it may not work, or may misbehave**.
- This project is not affiliated with Realtek, Intel, Google or Acer.

## Usage

The build environment (LLVM, WDK / SDK headers and libraries) and the test-signing certificate are the same as for magolor-audio-windows.

1. Install `jslaudio` from magolor-audio-windows first.
2. **Build** (the driver package is written to `pkg\`)
   ```
   powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -LlvmBin <LLVM bin dir> -WdkRoot <WDK "c" dir> -SdkRoot <SDK "c" dir>
   ```
3. **Install (as Administrator)**
   ```
   powershell -ExecutionPolicy Bypass -File scripts\install.ps1
   ```
   It worked if **JSL RT5682 Headset Codec** shows up without errors under "System devices" in Device Manager.
4. Plug in headphones and switch the Windows output to **Headphones** and the input to **Headset Microphone**.

The driver writes its log to `C:\Windows\Temp\jslcodec.log` (reading it requires Administrator rights). Install with `install.ps1 -LogPath <file>` to write it to a file of your choice instead.

To uninstall, find the package whose original name is `jslcodec.inf` with `pnputil /enum-drivers` (`oemNN.inf`) and run `pnputil /delete-driver oemNN.inf /uninstall /force` as Administrator.

## How it works

- I2C (I2C4, address 0x1A) is accessed through the Windows SPB framework.
- At start the codec is initialised with the same sequence as the Linux rt5682 driver: device ID check, headphone calibration, Realtek's register patch and the probe-time setup.
- Clocking follows the Linux Intel machine driver (sof_rt5682): PLL1 derives a ~24.576 MHz system clock from the 24 MHz MCLK, and the DAC / ADC filters run through the I2S1 ASRC. I2S uses 2 slots of 24-bit data.
- The jack state (JD1) is polled every 500 ms. On insertion the combo-jack type is detected and the headphone path is powered; for a headset the microphone path is powered as well. Everything is powered down on removal.

## License

**GPL-2.0-only** ([LICENSE](LICENSE)).

The register initialisation, calibration, jack detection and power sequencing are ported from these Linux kernel drivers:

- `sound/soc/codecs/rt5682.c`, `rt5682-i2c.c`, `rt5682.h` (Copyright 2018 Realtek Semiconductor Corp., author Bard Liao, GPL-2.0-only)
- `sound/soc/intel/boards/sof_rt5682.c` (Intel machine driver, consulted for the clock configuration)

That is why this driver lives in a repository separate from the MIT-licensed magolor-audio-windows.
