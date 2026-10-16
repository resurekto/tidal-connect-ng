# Raspberry Pi OS + HiFiBerry DAC+ example

A tested setup: TCNG runs as a systemd user service on Raspberry Pi OS 12
(Bookworm) with PipeWire, plays to a HiFiBerry DAC+ Pro, and the TIDAL app's volume slider
moves the DAC chip's own volume control. TCNG and PipeWire pass the samples through unscaled
and at their original rate, so the DAC receives the stream bit-perfect, and the PCM5122
does the attenuation itself.

Tested with PipeWire 1.2.7 and WirePlumber 0.4.13. Other players on the Pi (through
PipeWire) keep working and get the same hardware volume.

## How the volume gets to the DAC

```
TIDAL app slider
  → TCNG (--volume mixer, the default): sets the ALSA "default" device's mixer
  → pipewire-alsa: that mixer is the volume of PipeWire's default output
  → WirePlumber rule 51-hifiberry.lua: the HiFiBerry uses profile set hifiberry.conf
  → path hifiberry-output.conf: the output volume is the card's "Digital" control
  → PCM5122 digital volume (0.5 dB steps, -103.5 dB to 0 dB) and mute switch
```

Without the WirePlumber rule, PipeWire doesn't recognise `Digital` (the stock card profiles
look for `Master`, `PCM`, …) and scales the samples in software instead.

## Files

| File | Purpose |
|---|---|
| `rootfs/usr/local/bin/` | The `tidal-connect-ng` binary you build |
| `rootfs/usr/local/etc/tidal-connect-ng/cert/` | Your `device.pem` ([Certificate](../../README.md#certificate)) |
| `rootfs/home/user/.config/systemd/user/tidal-connect-ng.service` | The service; edit the name, model, client id and options |
| `rootfs/home/user/.config/pipewire/pipewire.conf.d/10-allowed-rates.conf` | Lets PipeWire run at the stream's rate (44.1–192 kHz) instead of resampling to 48 kHz |
| `rootfs/home/user/.config/wireplumber/main.lua.d/51-hifiberry.lua` | Gives the HiFiBerry its own profile set |
| `rootfs/home/user/.config/alsa-card-profile/mixer/profile-sets/hifiberry.conf` | One stereo output on the card |
| `rootfs/home/user/.config/alsa-card-profile/mixer/paths/hifiberry-output.conf` | Its volume and mute: the `Digital` control |

`user` is the account that runs the audio session; replace it with yours.

## Install

1. **Enable the DAC.** In `/boot/firmware/config.txt`, add `dtoverlay=hifiberry-dacplus`
   and reboot. `aplay -l` should list `HiFiBerry DAC+ Pro`.
2. **PipeWire.** The desktop image runs it by default. If it's missing,
   install `pipewire pipewire-alsa pipewire-pulse wireplumber`.
3. **Build and install TCNG** (dependencies: [README](../../README.md#build); the
   certificate and client id: [Certificate](../../README.md#certificate),
   [Client id](../../README.md#client-id)):
   ```sh
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
   sudo install -m 755 build/src/tidal-connect-ng /usr/local/bin/
   sudo install -d -o user -g user -m 700 /usr/local/etc/tidal-connect-ng/cert
   sudo install -o user -g user -m 600 device.pem /usr/local/etc/tidal-connect-ng/cert/
   ```
4. **Copy the configuration** (as `user`, from the repo root):
   ```sh
   cp -r examples/raspberry-pi-os/rootfs/home/user/.config/. ~/.config/
   systemctl --user restart wireplumber pipewire pipewire-pulse
   ```
5. **Start TCNG at boot:**
   ```sh
   sudo loginctl enable-linger user
   systemctl --user daemon-reload && systemctl --user enable --now tidal-connect-ng
   journalctl --user -u tidal-connect-ng -f
   ```

## Check it

- `pactl list cards | grep "Active Profile"`: the HiFiBerry shows `output:analog-stereo`.
  Its output is now named `alsa_output.….analog-stereo`; if PipeWire had another default
  before, set it again with `wpctl set-default <id>` (ids from `wpctl status`).
- Move the slider in the app: `amixer -c 0 sget Digital` follows it (for example -18 dB at
  PipeWire volume 0.5), while `pw-dump <sink id> | grep softVolumes` stays `[1.0, 1.0]`.
- While a hi-res track plays, `pw-top` shows the HiFiBerry running at the track's rate
  (for example 192000).

The output stays bit-perfect while TCNG is the only stream. If another program plays at a
different rate at the same time, PipeWire resamples one of them.

## Without PipeWire

For a TIDAL-only device, TCNG can open the card directly. Point `--playback-device` at the
HiFiBerry's `hw:` device, on this Pi
`"snd_rpi_hifiberry_dacplus: HiFiBerry DAC+ Pro HiFi pcm512x-hifi-0 (hw:0,0)"` (given an
unknown name, TCNG prints the valid ones and falls back to the default device), and add
`--alsa-mixer-control Digital`. The PipeWire and WirePlumber files are then not needed, but
the card is TCNG's alone while it plays.

## Other DACs

The WirePlumber rule matches the card name `snd_rpi_hifiberry_dacplus`
(`cat /proc/asound/cards`), and the path names the control `Digital`
(`amixer -c 0 scontrols`). Change both for another card. WirePlumber 0.5 (Debian 13) uses
`.conf` rules (`monitor.alsa.rules`) instead of Lua; this example has only been tested
on 0.4.
