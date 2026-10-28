# Geometry Dash - Dreamcast port

A Sega Dreamcast port of the Geometry Dash PS2 port, built from the same game
code as the PSP/3DS ports (the PS2 port is a fork of
[rLeo2023/geometrydash-psp](https://github.com/rLeo2023/geometrydash-psp),
which is a fork of [AleFunky/gd3ds](https://github.com/AleFunky/gd3ds)).

The game code is the 3DS one, mostly unchanged. The Dreamcast side replaces
libctru, citro2d and citro3d with KallistiOS and the PowerVR:

| Path | What |
|---|---|
| `dc/src/c2d_dc.c` | the renderer: citro2d on the PVR, both 3DS screens on 640x480 |
| `dc/src/ctru_dc.c` | input (maple controller, touch cursor), time, threads, paths |
| `dc/src/ndsp_dc.c` | the DSP channel gains the game sets, on the AICA |
| `dc/src/wav_dc.c` | the sound effects (`snd_sfx`) |
| `dc/src/mp3_dc.c` | the music: the PS2 port's 4-bit IMA ADPCM `.wav` songs, streamed |
| `dc/src/jsonc_dc.c` | the small part of json-c the config file needs |
| `dc/include/` | the 3DS headers the game includes (`3ds.h`, `citro2d.h`, ...) |
| `dc/tools/mkatlas.py` | `.t3s` sprite lists to Dreamcast sheets (ARGB4444, twiddled) |
| `dc/tools/build_assets.sh` | builds `dc/disc/romfs/` (sheets, levels, songs, menus) |
| `dc/tools/package.sh` | `gd.elf` + assets -> a self-boot `gd.iso` |

## What the port does about the Dreamcast's memory

The Dreamcast has 16 MB of main RAM and 8 MB of video RAM; the PSP port could
keep every sprite sheet in its 32 MB of main RAM. Here the sheets stay on the
disc and only the pages being drawn live in VRAM, uploaded the first time they
are used and evicted least-recently-used when room is needed. A page that
cannot be made room for is skipped (and logged), not drawn with the wrong
pixels.

Songs are the PS2 port's IMA ADPCM `.wav` (32 kHz stereo, 4 bits a sample):
the SH-4 decodes them into the AICA stream, which is cheap enough at 60 fps
and keeps the CD reads small.

## Building

Needs:

- KallistiOS and its `sh-elf` toolchain (`$KOS_BASE`, default `~/dc/kos`)
- `kos-ports` with `zlib` built
- Python 3 with Pillow (the sprite sheet converter)
- `mkisofs` (cdrtools) and KOS's `scramble`/`makeip` for the disc

```
export KOS_BASE=$HOME/dc/kos
. $KOS_BASE/environ.sh
cd $KOS_PORTS/zlib && make install     # once
cd -

./dc/tools/build_assets.sh             # dc/disc/romfs/ (~94 MB, mostly songs)
./dc/build.sh                          # gd.elf
./dc/tools/package.sh                  # gd.iso
```

Run `gd.iso` in an emulator (Flycast, Redream) or burn it (the ISO is a
MIL-CD data track; use your usual CDI/GDI tool for real hardware).

The PS2 assets are read from
`Geometry Dash PS2 Port OptiJuegos/Version ELF/GDPS2/romfs` by default; pass
another path as the first argument of `build_assets.sh`.

## Controls

| Dreamcast | 3DS |
|---|---|
| A | A / touch (when the touch screen is shown) |
| B | B (back, quit) |
| X | X |
| Y | Y |
| Start | Start |
| L / R | L / R |
| D-pad | d-pad (menus: from button to button) |
| Analog stick | circle pad, or the touch cursor in menus |

Menus with a PSP layout are drawn as one screen (the phone game's layout),
with the touch screen's half laid over the top screen. Gameplay fills the
screen with the level, and the pause/practice buttons over it.

## Known limitations

- Saves and the config live on a RAM disk for now (`/ram/gd3ds/`): they last
  the session. The VMU has no directories and only 200 KB; the save file is
  the next thing to wire up.
- Online levels are not there (the network stub answers every request with an
  error, as on the PSP).
- Custom levels/songs are read from `external_levels/` and `saved_songs/` on
  the RAM disk, so they need to be on the disc and copied there (not done
  yet).
- No VMU icons or CD audio tracks; music is streamed from the data track.

## Credits

The game code, art and levels: RobTop Games; the 3DS port: AleFunky; the PSP
port: rLeo; the PS2 port: OptiJuegos. This port only adds the Dreamcast
platform layer.
