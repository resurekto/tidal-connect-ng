# tidal-connect-ng

TCNG is a reimplementation of the TIDAL Connect receiver `tidal_connect_application`.
Phones running the TIDAL app discover it on the network (mDNS `_tidalconnect._tcp`), connect
over secure WebSocket and cast to it; it fetches the streams from TIDAL and plays them
through PortAudio. A local "speaker controller" can attach over a Unix socket (spkcon).

Unlike the original, which runs only on ARM Linux and which TIDAL caps at 16-bit/44.1 kHz,
TCNG runs on Linux, macOS and Windows and can play hi-res (up to 24-bit/192 kHz) through
[python-tidal](#hi-res-streaming). MQA, MPEG-H and Dolby Atmos are not supported.

## Build

CMake ≥ 3.25, a C++20 compiler, pkg-config, and the system libraries OpenSSL ≥ 3, libcurl,
libFLAC/FLAC++, FFmpeg (avformat, avcodec, avutil, swresample) and DNS-SD (built into macOS;
`libavahi-compat-libdnssd` on Linux; Bonjour SDK on Windows). libwebsockets, PortAudio,
spdlog, fmt, CLI11, nlohmann/json and Catch2 are fetched by CMake.

```sh
# macOS
brew install cmake pkgconf openssl@3 flac ffmpeg
# Debian/Ubuntu/Raspberry Pi OS
sudo apt install cmake g++ pkg-config libssl-dev libcurl4-openssl-dev libflac++-dev \
  libavformat-dev libavcodec-dev libavutil-dev libswresample-dev \
  libavahi-compat-libdnssd-dev libasound2-dev libpulse-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

## Install (Linux)

You need a [certificate](#certificate) and a [client id](#client-id) first. The receiver
runs as a systemd user service of the account that owns the audio session (PipeWire), here
`user`; the service file comes from `examples/raspberry-pi-os/`, a complete example setup
(Raspberry Pi OS, PipeWire, HiFiBerry hardware volume; see its
[README](examples/raspberry-pi-os/README.md)). Edit the name, model, client id and audio
device in it.

```sh
sudo install -m 755 build/src/tidal-connect-ng /usr/local/bin/
sudo install -d -o user -g user -m 700 /usr/local/etc/tidal-connect-ng/cert
sudo install -o user -g user -m 600 device.pem /usr/local/etc/tidal-connect-ng/cert/device.pem
install -D -m 644 examples/raspberry-pi-os/rootfs/home/user/.config/systemd/user/tidal-connect-ng.service \
  ~/.config/systemd/user/tidal-connect-ng.service
sudo loginctl enable-linger user          # start at boot without a login
systemctl --user daemon-reload && systemctl --user enable --now tidal-connect-ng
journalctl --user -u tidal-connect-ng -f
```

## Run

```sh
./build/src/tidal-connect-ng \
  --tc-certificate-path /usr/local/etc/tidal-connect-ng/cert/device.pem \
  --clientid <client id> \
  --friendly-name "Living Room" \
  --model-name "My Streamer" \
  --playback-device default
```

`--playback-device default` is ALSA's default device; on macOS and Windows leave it out.
`--help` lists all options. The original's options are accepted under the same names, except
the MQA, MPEG-H and Atmos ones (`--codec-mqa`, `--enable-mqa-passthrough`, `--codec-mpegh`,
`--codec-atmos`), `--disable-app-security` and `--disable-web-security` (TCNG always
verifies TIDAL's TLS certificates).

## Hi-res streaming

TIDAL serves this receiver at most 16-bit/44.1 kHz. For HI_RES_LOSSLESS (up to
24-bit/192 kHz), point TCNG at a [python-tidal](https://github.com/tamland/python-tidal)
checkout and a session logged in through it; tracks are then resolved by python-tidal and
streamed as DASH segments.

| Option | Meaning |
|---|---|
| `--python-tidal-path <dir>` | python-tidal checkout (put on `PYTHONPATH`). Turns the feature on. |
| `--python-tidal-session <file>` | tidalapi session file; refreshed tokens are written back. |
| `--python-bin <file>` | Python with python-tidal's dependencies. Default: `python3`, then `python`, from `PATH` (Windows: `python` first). |
| `--python-tidal-login` | Log in to TIDAL (PKCE), write the session file and exit. |

The session must come from the PKCE login: TIDAL only serves HI_RES_LOSSLESS to PKCE
sessions, and a classic device-login session silently caps playback at 16-bit/44.1 kHz. If
you already have one (python-tidal's `login_session_file(..., do_pkce=True)`), point
`--python-tidal-session` at it; otherwise `--python-tidal-login` creates it:

```sh
git clone https://github.com/tamland/python-tidal ~/python-tidal
python3 -m venv ~/tidal-venv && ~/tidal-venv/bin/pip install -e ~/python-tidal  # its dependencies

# once: prints a TIDAL login link, then asks for the redirected URL
./build/src/tidal-connect-ng \
  --python-tidal-path ~/python-tidal \
  --python-tidal-session ~/.config/tidal-connect-ng/tidal-session.json \
  --python-bin ~/tidal-venv/bin/python \
  --python-tidal-login

./build/src/tidal-connect-ng ... \
  --python-tidal-path ~/python-tidal \
  --python-tidal-session ~/.config/tidal-connect-ng/tidal-session.json \
  --python-bin ~/tidal-venv/bin/python
```

A track that python-tidal cannot resolve (missing python, expired session, network) fails
instead of falling back to the built-in fetch; the log line `python-tidal: track=... failed`
says why.

## Volume control

The original receiver never applied the app's volume; it only forwarded it to a separate
speaker-controller program. TCNG applies it itself, chosen with `--volume`:

| `--volume` | What happens |
|---|---|
| `mixer` (default) | The OS mixer of the output: ALSA on Linux (ALSA's `default` device, its first control with a playback volume, as the original's controller did; with PipeWire that is PipeWire's volume), CoreAudio on macOS, the WASAPI endpoint volume on Windows. The stream stays bit-perfect. |
| `software` | The samples are scaled (16-bit sources are widened to 32-bit first). Not bit-perfect below 100%. |
| `off` | Not applied; only forwarded to a speaker controller, as the original. |

The slider follows alsamixer's perceptual curve; at startup and when a session starts, TCNG
reads the mixer and shows the app its real level; mute and 0 also mute the control where it
has a switch.

Whether `mixer` reaches the DAC's own volume depends on the OS. With PipeWire, the card's
volume control must be mapped in its ALSA card profile, otherwise PipeWire scales in software
(`pw-dump`: `softVolumes` below 1). `examples/raspberry-pi-os/` does this for a HiFiBerry
DAC+, whose control is named `Digital`, with a WirePlumber 0.4 rule and a profile set under
`.config/`; copy them to `~/.config/` and run
`systemctl --user restart wireplumber pipewire pipewire-pulse`. Without a sound server, point
`--playback-device` at the card's `hw:` device and name its control; `--alsa-mixer-control`
is Linux-only, and startup fails, listing the card's controls, if the name does not exist:

```sh
./build/src/tidal-connect-ng ... \
  --playback-device "<name> (hw:0,0)" \
  --alsa-mixer-control Digital
```

## Certificate

The TIDAL apps only connect to a receiver that presents a TIDAL Connect device certificate
over TLS. TCNG does not ship one or contain any private keys: you supply the certificate of
a device you own, as one PEM file holding the certificate, any chain certificates and the
private key, in that order (the layout `openssl` writes), passed with
`--tc-certificate-path`. Keep it private: `chmod 600`, never commit or share it.

**Where to get one.** The original receiver (`tidal_connect_application`, from iFi's Zen
Stream) keeps its certificate in an encrypted `.dat` file, which TCNG does not read. Builds
of it for the Raspberry Pi are available online, for example the Docker image
[edgecrush3r/tidal-connect](https://hub.docker.com/r/edgecrush3r/tidal-connect). One way to
get a PEM is to run that receiver under `gdb` on a Linux machine that runs 32-bit ARM
binaries (a Raspberry Pi, 32- or 64-bit OS) and write out the certificate and key it hands
to OpenSSL:

<details>
<summary>gdb recipe</summary>

1. Export the image's filesystem (on any machine with Docker), copy `tc-rootfs.tar` to the
   ARM machine and unpack it there:

   ```sh
   docker create --platform linux/arm --name tc edgecrush3r/tidal-connect
   docker export tc > tc-rootfs.tar && docker rm tc
   # on the ARM machine:
   mkdir tc && sudo tar -xf tc-rootfs.tar -C tc
   ```

2. Stop any TIDAL Connect receiver so port 2019 is free, and save this as `dump-cert.gdb`:

   ```gdb
   set pagination off
   set confirm off
   handle SIGILL nostop noprint pass
   handle SIGPIPE nostop noprint pass
   set breakpoint pending on
   set $out = 0
   define open_out
     if $out == 0
       set $out = (void*)BIO_new_file("/tmp/device.pem", "w")
     end
   end
   break SSL_CTX_use_certificate
   commands
     silent
     open_out
     call (int)PEM_write_bio_X509($out, $r1)
     continue
   end
   break SSL_CTX_ctrl if $r1 == 14
   commands
     silent
     open_out
     call (int)PEM_write_bio_X509($out, $r3)
     continue
   end
   break SSL_CTX_use_PrivateKey
   commands
     silent
     open_out
     call (int)PEM_write_bio_PrivateKey($out, $r1, 0, 0, 0, 0, 0)
     call (int)BIO_free($out)
     set $out = 0
     printf "certificate and key written to /tmp/device.pem\n"
     continue
   end
   continue
   ```

3. Start the receiver inside the image's filesystem with one of its `.dat` certificates
   (`chroot` is required: with `LD_LIBRARY_PATH` on the image's libraries it does not start,
   because they are older than the host's C library), then attach gdb:

   ```sh
   sudo chroot tc /app/ifi-tidal-release/bin/tidal_connect_application \
     --tc-certificate-path /app/ifi-tidal-release/id_certificate/IfiAudio_ZenStream.dat \
     --friendly-name "My Speaker" \
     --playback-device default &
   sudo gdb -q \
     -ex "set sysroot $PWD/tc" \
     -p "$(pgrep -x tidal_connect_a)" \
     -x dump-cert.gdb
   ```

4. In another shell, open one TLS connection: `openssl s_client -connect 127.0.0.1:2019 </dev/null`.
5. When gdb prints `certificate and key written`, quit it (`Ctrl+C`, then `quit`), stop the
   receiver (`sudo pkill -x tidal_connect_a`) and install the file, which is inside `tc/`,
   owned by the account that runs TCNG (here `user`):

   ```sh
   sudo install -d -o user -g user -m 700 /usr/local/etc/tidal-connect-ng/cert
   sudo install -o user -g user -m 600 tc/tmp/device.pem /usr/local/etc/tidal-connect-ng/cert/device.pem
   sudo rm tc/tmp/device.pem
   F=/usr/local/etc/tidal-connect-ng/cert/device.pem
   [ "$(sudo openssl x509 -in $F -noout -pubkey)" = "$(sudo openssl pkey -in $F -pubout)" ] && echo key matches
   ```

6. The [client id](#client-id) is a 16-character string built into the same receiver,
   stored right after the mDNS service name:

   ```sh
   strings tc/app/ifi-tidal-release/bin/tidal_connect_application | grep -A2 '^_tidalconnect._tcp$' | grep -E '^[A-Za-z0-9]{16}$'
   ```

The registers (`$r1`, `$r3`) are for 32-bit ARM.

</details>

For development, any self-signed PEM works for the transport, but the TIDAL apps will not
accept it:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 30 -subj "/CN=tcng"
cat cert.pem key.pem > device.pem
```

## Client id

TIDAL also identifies a receiver by a client id, which TCNG sends with the app's token to
TIDAL's token endpoint to obtain the token it streams with. Pass it with `--clientid`; TCNG
does not ship one. It belongs with the certificate: use the id of the receiver the
certificate came from (step 6 of the [recipe](#certificate) reads it from the same image),
or the token exchange fails and nothing plays. With `--python-tidal-path` it is not used.

## Known bugs

- **The TIDAL app labels the stream LOW**, whatever quality TCNG reports (`HI_RES_LOSSLESS`,
  `HI_RES` and `LOSSLESS` were tried). The label comes from the app, not from the receiver.
- **Seeks and skips waste bandwidth on hi-res streams** (`--python-tidal-path`): everything
  read ahead is discarded, at 24-bit/192 kHz about 11 s of decoded audio plus 2–3 prefetched
  segments, roughly 13–16 MB per seek. Playback that runs through wastes nothing.

## License

[0BSD](LICENSE): use it for anything, no conditions.
