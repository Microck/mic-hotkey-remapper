# mic-hotkey-remapper

windows utility that watches a specific USB microphone (`VID_08BB` / `PID_2902`) and fires a configurable hotkey when the device connects or disconnects. built for [this desk mic](https://s.click.aliexpress.com/e/_c3EKq1QJ).

the mic physically disconnects when its mute button is toggled off, so this watches USB presence instead of HID input.

---

### modes

- **hold**: device arrival sends key-down, device removal sends key-up.
- **tap**: each arrival/removal sends one complete key press.

---

### usage

run the exe. a config window opens on first launch. pick a mode, click the shortcut field, press a key combo (e.g. `CTRL+ALT+F13`), and save.

config is stored at `%APPDATA%\MicHotkeyRemapper\config.ini`. enable `Start with Windows` to run at logon. no admin privileges required.

when running, the app places a microphone icon in the notification area. green means remapping is enabled; red means it is disabled. right-click the icon to enable or disable remapping, open configuration, or exit. disabling remapping is session-only and releases a held shortcut immediately.

---

### build

open a Visual Studio Developer Command Prompt:

```
cl /nologo /std:c++17 /O2 /MT /EHsc mic-hotkey-remapper.cpp /link /SUBSYSTEM:WINDOWS
```

libs are linked by pragmas in the source.

---

### license

MIT
