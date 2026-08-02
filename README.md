# c:geo Pebble Map Watchapp

A PebbleOS watchapp that shows the live map rendered by c:geo and accepts
commands to refresh, zoom, and change the refresh interval.

## Building

Requires the [Pebble SDK](https://developer.rebble.io/sdks/) (SDK 3 or newer).

```bash
cd pebble-map
pebble build
pebble install --phone <phone_ip>
```

The generated `.pbw` can also be sideloaded via the Pebble/Rebble phone app.

## How it works

- The watchapp opens an `AppMessage` channel with the UUID
  `9ec749ec-29ea-4c42-9b4b-9e1f0f1a1b0c`.
- On launch it requests a `CMD_REFRESH` from c:geo.
- c:geo's `UnifiedMapActivity` captures the currently selected map source
  (Google, Mapsforge, VTM, ...) as a 144x168 full-color 8-bit bitmap, splits
  it into chunks and sends them back to the watchapp.
- The watchapp reassembles the chunks into a `GBitmap` displayed full screen.
- The map auto-refreshes every 5 seconds; this can be toggled with a
  long-press on **SELECT**.
- **UP** zooms in, **DOWN** zooms out, **SELECT** requests an immediate
  refresh.
