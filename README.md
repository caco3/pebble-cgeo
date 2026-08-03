# c:geo Pebble Map Watchapp

A PebbleOS watchapp that shows the live map rendered by c:geo and accepts
commands to refresh, zoom, and change the refresh interval.

## Building

Requires the [Pebble SDK](https://developer.rebble.io/sdks/) (SDK 3 or newer).

```bash
cd pebble-cgeo
pebble build
```

The generated `build/pebble-cgeo.pbw` can be pushed to the phone and sideloaded:

```bash
PORT=39375
adb -s 192.168.1.212:$PORT push build/pebble-cgeo.pbw /sdcard/Download/pebble-cgeo.pbw
```

Then install it on the watch from `/sdcard/Download/pebble-cgeo.pbw` using the Pebble/Rebble app.

## How it works

- The watchapp opens an `AppMessage` channel with the UUID
  `9ec749ec-29ea-4c42-9b4b-9e1f0f1a1b0c`.
- On launch it requests a `CMD_REFRESH` from c:geo.
- c:geo's `PebbleMapService` renders the currently selected offline
  Mapsforge/VTM map around your location as a 200x228 8-bit color bitmap,
  splits it into chunks and sends them back to the watchapp.
- The watchapp reassembles the chunks into a `GBitmap` displayed full screen.

## Controls

- **SELECT**: request an immediate refresh
- **UP / DOWN**: zoom in / out on the watch
- **LONG SELECT**: toggle auto-refresh (10 s on / off)
