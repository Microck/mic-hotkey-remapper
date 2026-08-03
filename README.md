# mic-hotkey-remapper

windows utility that watches a specific USB microphone (`VID_08BB` / `PID_2902`) and fires a configurable hotkey when the device connects or disconnects. built for [this desk mic](https://s.click.aliexpress.com/e/_c3EKq1QJ).

the mic physically disconnects when its mute button is toggled off, so this watches USB presence instead of HID input.

<img width="422" height="267" alt="image" src="https://github.com/user-attachments/assets/ff495b1d-e92b-41f4-a272-8b451967cbbe" />


---

### modes

- **hold**: device arrival sends key-down, device removal sends key-up.
- **tap**: each arrival/removal sends one complete key press.

---

### usage

run the exe. a config window opens on first launch. pick a mode, click the shortcut field, press a key combo (e.g. `CTRL+ALT+F13`), and save.

config is stored at `%APPDATA%\MicHotkeyRemapper\config.ini`. enable `Start with Windows` to run at logon. no admin privileges required.

when running, the app places a microphone icon in the notification area. green means remapping is enabled; red means it is disabled. right-click the icon to enable or disable remapping, open configuration, or exit. disabling remapping is session-only and releases a held shortcut immediately.

the tray menu also opens either embedded audio-cleaner mode. No second cleaner executable is required.

---

### build

open a Visual Studio Developer Command Prompt:

```
mkdir release-build
cl /nologo /std:c++17 /O2 /MT /LD /EHsc- mic-audio-apo.cpp /Fo release-build\\mic-audio-apo.obj /link /SUBSYSTEM:WINDOWS /NODEFAULTLIB:atls.lib /DEF:mic-audio-apo.def /OUT:release-build\\mic-audio-apo.dll
rc /nologo /fo release-build\\mic-hotkey-remapper-resources.res mic-hotkey-remapper-resources.rc
cl /nologo /std:c++17 /O2 /MT /EHsc /D MIC_HOTKEY_REMAPPER_EMBEDDED /c mic-audio-cleaner.cpp /Fo release-build\\mic-audio-cleaner.obj
cl /nologo /std:c++17 /O2 /MT /EHsc /D MIC_HOTKEY_REMAPPER_EMBEDDED /c mic-audio-cleaner-apo.cpp /Fo release-build\\mic-audio-cleaner-apo.obj
cl /nologo /std:c++17 /O2 /MT /EHsc /c mic-hotkey-remapper.cpp /Fo release-build\\mic-hotkey-remapper.obj
cl /nologo release-build\\mic-hotkey-remapper.obj release-build\\mic-audio-cleaner.obj release-build\\mic-audio-cleaner-apo.obj release-build\\mic-hotkey-remapper-resources.res /link /SUBSYSTEM:WINDOWS /OUT:release-build\\mic-hotkey-remapper.exe
```

The APO DLL is an intermediate build input. It is embedded into the final remapper executable as a resource and is not needed beside the shipped EXE. Libraries are linked by pragmas in the source.

---

### audio cleanup

The virtual-cable cleaner is opened from the remapper tray menu. It captures a microphone with WASAPI, applies a 90 Hz high-pass filter, fixed 50/100 Hz hum notches tuned for the USB PnP mic, adaptive spectral noise suppression, and a soft noise gate, then renders the cleaned stream to a selectable Windows audio output.

for STT, install a software virtual audio cable such as VB-CABLE. select its playback endpoint, usually named `CABLE Input`, as the cleaner output. then select the matching recording endpoint, usually named `CABLE Output`, as the microphone in the STT application.

the cleaner exposes three settings: high-pass cutoff in Hz, noise reduction percentage, and gate threshold as a multiple of the measured noise floor. start with the default values, keep the mic silent for about one second after starting, then lower the gate threshold if quiet speech is being cut off or increase noise reduction if hiss remains during speech.

the cleaner keeps retrying the configured devices, so it can recover when this microphone disconnects while its physical button is toggled off. its settings are stored at `%APPDATA%\MicHotkeyRemapper\audio-cleaner.ini`.

### direct-device cleanup

The direct-device cleaner is also opened from the remapper tray menu. This is the no-cable option. Open it while `Microphone (USB PnP Sound Device)` is connected, choose the high-pass and gate settings, and click `Install direct cleaner`. Windows will then run the tuned cleanup inside the original microphone endpoint, so STT applications can keep selecting the original microphone instead of a virtual cable.

The direct option is bundled in the same remapper executable. When installing or uninstalling, it extracts its embedded APO payload to `%ProgramData%\\MicHotkeyRemapper` because the Windows audio engine requires an actual DLL for an APO. That extracted file is an implementation detail, not a second app or a file that needs to be distributed beside the remapper.

the direct option requires administrator permission and applies a low-latency real-time profile: 90 Hz high-pass by default, 50/100 Hz hum notches measured from this USB PnP mic, and a soft adaptive gate. The portable virtual-cable cleaner remains the stronger option when adaptive spectral noise reduction is needed. Use one route or the other for a given STT setup, not both.

close and reopen audio applications after installing or changing the direct profile. Use the same installer to uninstall it and restore the endpoint's previous effect setting.

---

### license

MIT
