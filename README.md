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

the tray menu also opens the audio cleaner when `mic-audio-cleaner.exe` is kept beside the remapper executable.

---

### build

open a Visual Studio Developer Command Prompt:

```
cl /nologo /std:c++17 /O2 /MT /EHsc mic-hotkey-remapper.cpp /link /SUBSYSTEM:WINDOWS
cl /nologo /std:c++17 /O2 /MT /EHsc mic-audio-cleaner.cpp /link /SUBSYSTEM:WINDOWS
```

libs are linked by pragmas in the source.

---

### audio cleanup

`mic-audio-cleaner.exe` captures a microphone with WASAPI, applies a 90 Hz high-pass filter, fixed 50/100 Hz hum notches tuned for the USB PnP mic, adaptive spectral noise suppression, and a soft noise gate, then renders the cleaned stream to a selectable Windows audio output.

for STT, install a software virtual audio cable such as VB-CABLE. select its playback endpoint, usually named `CABLE Input`, as the cleaner output. then select the matching recording endpoint, usually named `CABLE Output`, as the microphone in the STT application.

the cleaner exposes three settings: high-pass cutoff in Hz, noise reduction percentage, and gate threshold as a multiple of the measured noise floor. start with the default values, keep the mic silent for about one second after starting, then lower the gate threshold if quiet speech is being cut off or increase noise reduction if hiss remains during speech.

the cleaner keeps retrying the configured devices, so it can recover when this microphone disconnects while its physical button is toggled off. its settings are stored at `%APPDATA%\MicHotkeyRemapper\audio-cleaner.ini`.

---

### license

MIT
