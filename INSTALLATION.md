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


## 4. Configure API keys

The watch uses three free services: 
* https://ipgeolocation.io, 
* https://zipcodestack.com, 
* https://imezonedb.com.  

You will have to register with each one and obtain an API key.  Then copy 
'secrets.h.template' to 'secrets.h' and add your API keys.

## 5. Calendar Configuration (Optional)

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

If you are using Google Calendar, the iCal URL can be found by following these steps:

1. Go to https://calendar.google.com/
2. Click the gear icon and then click Settings.
3. In the left nav bar, look for the name of the calendar you want to setup.
4. Click the calendar name, after it expands, click the Integrate calendar option.
5. Look for the section title 'Secret address in iCal format' and click the copy button to the right.

Refer to online help for other services.  Note this code relies on being able to filter on start-min and start-max for performance.  Google and others support this.  I have not tested services that do not. I expect my code will work either way but it couldlead to memory pressure and/or slowness due to having to download the entire calendar with every load.


## 6. EEZ Studio Setup (Optional, for UI Editing)

You only need EEZ Studio if you want to modify the UI.

1. Install EEZ Studio: https://www.envox.eu/studio/studio-introduction/
2. Open your EEZ project.
3. Set generated build output to `src/src`.

The firmware expects generated UI files in `src/src`, so this path must match.
