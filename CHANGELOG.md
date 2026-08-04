# Changelog

## v1.4.1 - 2026-08-04

- Fixes direct-device installation by using the Windows endpoint property store instead of writing the protected `FxProperties` registry key directly.
- Stores the full endpoint identifier so uninstall can restore the previous effect on the same endpoint.
- Reports the exact Windows operation that fails if endpoint installation is still denied.

## v1.4.0 - 2026-08-04

- Bundles the virtual-cable cleaner and direct-device cleaner into one remapper executable.
- Adds a direct-device Windows audio processing object for the USB PnP microphone.
- Embeds the direct cleaner payload in the remapper and extracts it only when Windows installation requires a DLL.
- Adds adjustable high-pass and gate settings for the direct-device cleaner.
- Adds a reproducible release build script, ZIP package, and SHA256 checksum.

## v1.2.0

- Added the original standalone virtual-cable audio cleaner.
- Added USB microphone filtering, hum notches, adaptive noise reduction, and gate controls.
