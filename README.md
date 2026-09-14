# Bloons Adventure Time TD — Nintendo Switch port (Unity 2020.3 / IL2CPP wrapper)

This is a native wrapper / loader that runs the original ARM64 Android build of
**Bloons Adventure Time TD** v1.7.7 on Switch homebrew. It contains no game code and no
game assets — it loads the game's own libraries and recreates, natively, the
thin Android/JNI layer the Unity engine expects.

## Install & run

```
sdmc:/switch/battd
├── battd_nx.nro
├── libmain.so  libunity.so  libil2cpp.so   <- from your APK: lib/arm64-v8a/
├── cursor.png                              <- optional
├── assets/                                 <- the APKs' assets/, merged
└── videos/                                 <- the splash clips + manifest.txt
```

Launch via **title override** (hold R while starting an installed game) or a
forwarder. Applet (album) mode doesn't have enough memory.

**The first launch takes a few minutes and needs an internet connection.** It
packs the 1,224 loose asset files into one archive (about a minute, and it says
so on screen), then the game downloads roughly 244 MB of content. Once all of it
is cached the game starts offline — see `online` under Settings.

Optionally drop a `cursor.png` (up to 64×64, transparency respected, top-left
pixel is the hotspot) in the same folder to replace the on-screen cursor with
your own.

## Controls

| Input | Action |
| --- | --- |
| Touchscreen | Tap (handheld) |
| **+** | Toggle the on-screen cursor |
| **–** | Toggle gyro pointing (tilt/turn the controller to aim) |
| Left stick | Move the cursor |
| **L** / **R** | Recenter the cursor to the middle of the screen (helps gyro aiming) |
| **A** / **ZR** / **ZL** | Tap / confirm (ZL and ZR let you play one-handed) |
| D-pad up / down | Adjust sensitivity of whatever is driving the cursor |

The cursor is on by default when docked and off in handheld; **+** overrides
either way. A USB mouse works in both modes: move to control the cursor,
left-click to tap, and use the scroll wheel to change sensitivity — gyro turns
itself off while a mouse is connected. Your stick, mouse and gyro sensitivities
are remembered in `pointer.cfg` automatically after in-game adjustment.

## Settings

`config.txt` is written next to the `.nro` on first launch, with the options
documented inline:

```
resolution  = 1280    # 1280 to 1920: 1280x720 up to 1920x1080
ui_scale    = 320     # screen density; this picks the 2D art quality
art_quality = 2       # 2 = best, or "auto" to let the game decide
online      = false   # once cached: false plays offline, true keeps updating
memory_mb   = 2048    # RAM reported to the game
```

## Save editing

`save.txt` is written on first launch with every option commented out. Remove
the `#` in front of a line to make changes that are applied next boot.

## Building

Requires devkitPro with the `switch-dev` group plus these portlibs:

```
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-libpng switch-zlib switch-ffmpeg

export DEVKITPRO=/opt/devkitpro
make                        # -> battd_nx.nro
```

Rebuilding `libdrm_nouveau` with `libdrm_nouveau-astc-chipset-fix.patch` is
strongly recommended; without it ASTC textures are software-decoded and the
symptom is a black screen with working audio.

## Credits

The loader/shim infrastructure (so_util, libc_shim, jni_fake, unity_jni,
opensles, nx_pointer, diagnostics) derives from the open-source Switch
`.so`-loader lineage — Andy Nguyen, fgsfds and ChanseyIsTheBest, building on
TheOfficialFloW's Vita/Switch loader tradition — reaching this project via the
Bloons Pop port, with the JNI layer and GC bridge from Daggerfall Unity,
networking from Animal Crossing: Pocket Camp, the engine clock from Bad Piggies,
the keyboard from Daggerfall Unity and PvZ Fusion, the asset packer from Lara
Croft GO, the splash-video player from CloverPit, and the save editor design
from Papa Pear Saga via Bloons Pop. All MIT-licensed. Thanks to everyone in that
lineage for making this approach possible.
