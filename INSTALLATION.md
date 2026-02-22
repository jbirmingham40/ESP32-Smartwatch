# Installation and Setup

This guide covers the minimum setup needed to build, flash, and run the watch.

## 1. Install PlatformIO (Required)

You need PlatformIO to build and deploy this project.

Recommended setup:

1. Install VS Code.
2. Install the PlatformIO IDE extension in VS Code.
3. Open this project folder in VS Code.

You can then build and flash from either the PlatformIO UI or terminal.

## 2. Build and Deploy Firmware (Required)

From the project root:

```bash
pio run
pio run -t upload
pio device monitor
```

This project uses the `waveshare_s3_146` environment defined in `platformio.ini`.

## 3. Prepare SD Card (Required)

The watch uses the SD card for alarm/timer sounds and persisted settings.

1. Format a blank SD card.
2. Copy everything from `sd-card/` in this repo to the root of the SD card.
3. Insert the SD card into the watch.

Settings are stored at:

- `/System/config.json`

## 4. Calendar Configuration (Optional)

If you want calendar events on the watch, manually add your iCal URLs to `/System/config.json`.
There is currently no UI for this setup.

Example:

```json
{
  "ical_urls": [
    "https://abc.com/ical/url",
    "https://cde.com/ical/url"
  ]
}
```

## 5. EEZ Studio Setup (Optional, for UI Editing)

You only need EEZ Studio if you want to modify the UI.

1. Install EEZ Studio: https://www.envox.eu/studio/studio-introduction/
2. Open your EEZ project.
3. Set generated build output to `src/src`.

The firmware expects generated UI files in `src/src`, so this path must match.
