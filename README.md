# mic-hotkey-remapper

Small Windows tray utility for the generic USB PnP desk microphone sold with a physical mute button.

The microphone disconnects from USB when its button is toggled off. This utility watches that USB presence state and sends a configurable keyboard shortcut when the microphone connects or disconnects.

## Features

- Hold mode: hold the configured shortcut while the microphone is connected.
- Tap mode: press the configured shortcut once on connect and once on disconnect.
- Native modifier combinations such as `CTRL+ALT+F13`.
- Green/red microphone tray icon showing whether remapping is enabled.
- Session-only tray toggle to disable remapping without closing the application.
- Built-in virtual-cable audio cleaner with high-pass, hum notch, noise reduction, and gate controls.
- Optional direct-device audio cleaner that keeps the original microphone endpoint available to STT applications.

## Usage

Run `mic-hotkey-remapper.exe`. On first launch, select a mode, click the shortcut field, press a key combination, and save.

Configuration is stored at `%APPDATA%\MicHotkeyRemapper\config.ini`. No administrator permission is needed for remapping. The `Start with Windows` option starts the remapper at logon.

Right-click the microphone tray icon to enable or disable remapping, open configuration, open either audio cleaner, or exit. Disabling remapping releases a held shortcut immediately.

## Audio cleanup

### Virtual-cable cleaner

Open `Audio cleaner (virtual cable)` from the tray menu. Select the USB PnP microphone as the input and a virtual cable playback endpoint, usually `CABLE Input`, as the destination. Select the matching recording endpoint, usually `CABLE Output`, in the STT application. The destination is intentionally a playback endpoint because the cleaner renders audio into it; the matching recording endpoint is what STT reads.

This executable cannot create a new Windows microphone endpoint on its own. If no virtual-cable playback/recording pair appears, install and configure a virtual audio cable separately, then click `Refresh devices`. Selecting speakers or headphones only monitors the cleaned signal and is not usable as an STT microphone, so the cleaner blocks that route.

The cleaner applies a 90 Hz high-pass filter, 50 Hz and 100 Hz hum notches, adaptive spectral noise reduction, and a soft gate. Its settings are stored at `%APPDATA%\MicHotkeyRemapper\audio-cleaner.ini`.

Keep the microphone silent for about one second after starting so the cleaner can measure the noise floor. Lower the gate threshold if quiet speech is cut off, or increase noise reduction if hiss remains during speech.

### Direct-device cleaner

Open `Audio cleaner (direct device)` while `Microphone (USB PnP Sound Device)` is connected. Choose the high-pass and gate settings, then click `Install direct cleaner`. This installs the tuned cleanup into the original microphone endpoint, so STT applications can keep selecting the original microphone instead of a virtual cable.

The direct cleaner is bundled into the same executable. Windows requires an actual DLL for an audio processing object, so installation extracts the embedded payload to `%ProgramData%\MicHotkeyRemapper`. This is an implementation detail and is not a second application that must be distributed beside the EXE.

The direct cleaner requires administrator permission. Close and reopen audio applications after installing, changing, or uninstalling it. It now includes adaptive spectral noise reduction in addition to the high-pass, hum notches, and gate. Use either the virtual-cable route or the direct-device route for a given STT setup, not both.

## Building

Install the Visual Studio C++ build tools and Windows SDK. From a Visual Studio Developer Command Prompt, run:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\build-release-both.ps1 -Version 1.4.6 -Clean
```

The multi-architecture script invokes the Visual Studio tools for both targets. It builds the APO DLL as an intermediate input, embeds the matching DLL in each final remapper executable, and writes these ignored outputs:

- `build\release-x64\mic-hotkey-remapper.exe`
- `build\release-arm64\mic-hotkey-remapper.exe`
- `dist\mic-hotkey-remapper-v1.4.6.zip`
- `dist\mic-hotkey-remapper-v1.4.6.zip.sha256`

The shipped ZIP contains `x64\mic-hotkey-remapper.exe` and `arm64\mic-hotkey-remapper.exe`, each with its matching audio cleaner embedded, plus this README and the MIT license. Run the ARM64 executable on ARM64 Windows. Run the x64 executable on regular Intel or AMD Windows. The intermediate APO DLL is not shipped beside either EXE.

## Release

Published releases and signed checksums are available on the [GitHub releases page](https://github.com/Microck/mic-hotkey-remapper/releases).

## License

MIT. See [LICENSE](LICENSE).
