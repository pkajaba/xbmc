# DTS:X to Dolby TrueHD (Atmos) on CoreELEC: how to try it

## 1. What this is, and its status

This branch adds an optional Kodi audio codec, `CDVDAudioCodecDtsToAtmos`. For a passthrough setup whose sound bar or receiver cannot take DTS:X but can take Dolby TrueHD with Atmos over HDMI eARC, it converts DTS:X to TrueHD in real time. The conversion is done by a separate Rust library (`libhz_stream_ffi.so`, from the harlettizer project) that Kodi loads at run time. Without the library the codec simply refuses to open and Kodi plays as it did before.

Status: experimental. There is no upstream pull request. It has been tried on one Ugoos AM6B+ (Amlogic S922X) running CoreELEC 21.

- Works: Atmos-flagged TrueHD reaches the sound bar and plays.
- Not verified: that the sound bar renders the objects, and that the height channels are heard.
- Known: the conversion adds about 87 ms of latency by design. The codec does not report it to Kodi, so lip sync needs the audio offset set by hand.

## 2. What you need

- A Ugoos AM6B+ with CoreELEC 21 (Kodi from `pannal/xbmc`, branch `aml-4.9-21.3_dev`). Other Amlogic-ng boxes are untested.
- An AVR or sound bar on eARC that accepts TrueHD.
- A Rust toolchain, 1.89 or newer, with `rustup target add armv7-unknown-linux-gnueabihf`.
- The CoreELEC build tree, or at least its armv7 glibc sysroot and cross linker.
- These three branches:
  - `https://github.com/pkajaba/xbmc`, branch `feat/transcode-audio-codec` (this one)
  - `https://github.com/pkajaba/harlettizer`, branch `feat/stream-crate`
  - `https://github.com/pkajaba/harletty-bridge`, branch `fix/xll-extension-waveforms`

About the key: the Atmos object metadata is signed with a 32-byte key that you must supply yourself. This project ships no key and does not say where to get one. Without a key everything works, TrueHD plays, and the object metadata is unsigned (some sound bars refuse unsigned metadata or mute it). Never put a key in an issue, a log or a screenshot.

## 3. Build the library

The kernel on the box is 64-bit but Kodi's userspace is 32-bit ARM, so the library must be armv7 with glibc.

```
git clone -b feat/stream-crate https://github.com/pkajaba/harlettizer
cd harlettizer
RUSTFLAGS="-C opt-level=3 -C target-cpu=cortex-a73" \
  cargo build --release --target armv7-unknown-linux-gnueabihf -p hz-stream-ffi
# -> target/armv7-unknown-linux-gnueabihf/release/libhz_stream_ffi.so
```

The linker needs a glibc armhf sysroot. `rust-lld` alone fails with "unable to find library -lc" (seen on a Mac). Point cargo at the CoreELEC cross compiler and sysroot, for example with `CARGO_TARGET_ARMV7_UNKNOWN_LINUX_GNUEABIHF_LINKER=<tree>/toolchain/bin/<triplet>-gcc`. The exact names depend on your tree and are not verified here.

Until the decoder fix is merged upstream, the workspace pins the `dca` crate to a revision that exists only on the `fix/xll-extension-waveforms` branch of the bridge fork. Cargo cannot fetch it from `harletty/harletty-bridge`. Add a source replacement outside the repo, in `harlettizer/.cargo/config.toml` (do not commit it):

```
[patch."https://github.com/harletty/harletty-bridge"]
dca = { git = "https://github.com/pkajaba/harletty-bridge", rev = "dea43c675072b51b706d20c0bd924ded7c50b116" }
```

If cargo complains that the patch is unused or that the revision is not found, check that the `rev` in `crates/hz-stream/Cargo.toml` still matches, and use `branch = "fix/xll-extension-waveforms"` instead. (The patch syntax is the standard cargo one; it has not been run against these forks yet.)

The intended CoreELEC route is the recipe `contrib/coreelec/dtsx2atmos/package.mk` in this tree. It builds `hz-stream-ffi` offline from a harlettizer release tarball with vendored dependencies and installs `libhz_stream_ffi.so` into `/usr/lib`. `PKG_URL` and `PKG_SHA256` are placeholders until a release tarball exists, and the recipe has never run in a CoreELEC tree. Until then, use the cargo command above.

## 4. Build kodi.bin

Unverified: nothing in this branch has been compiled yet. The branch is a normal change on top of the CoreELEC Kodi source, so the route is:

1. Get the CoreELEC build tree for `PROJECT=Amlogic-ce DEVICE=Amlogic-ng ARCH=arm` (version 21).
2. Make its Kodi package (`packages/mediacenter/kodi/package.mk`) use this branch as its source instead of the stock tarball. Either point `PKG_URL` at a tarball of the branch, or use the tree's mechanism for a local source directory. How to do this depends on the tree and is not verified.
3. Build: `PROJECT=Amlogic-ce DEVICE=Amlogic-ng ARCH=arm ./scripts/build kodi`.
4. The binary ends up under `build.CoreELEC-Amlogic-ng.arm-21/kodi-*/.install_pkg/usr/lib/kodi/kodi.bin`.

Expect the first compile of the new C++ to need small fixes. Reports are welcome (section 7).

## 5. Deploy to the box

The root file system is a read-only squashfs. Everything below goes under `/storage`, and nothing in flash is touched.

```
ssh root@BOX 'mkdir -p /storage/.kodi/lib'
scp libhz_stream_ffi.so root@BOX:/storage/.kodi/lib/
scp kodi.bin root@BOX:/storage/kodi.bin && ssh root@BOX 'chmod +x /storage/kodi.bin'
```

Where the library goes: Kodi opens the bare name `libhz_stream_ffi.so` and leaves the search to the system loader. It does not look in any Kodi directory. `/usr/lib` is read-only on the box, so use the recommended way: keep the library in `/storage/.kodi/lib` and set `LD_LIBRARY_PATH` in the service drop-in (below). (A CoreELEC image built with the package recipe has it in `/usr/lib` and needs no setting.)

Service override. First read the stock unit and copy its `ExecStart` line and any `EnvironmentFile=` lines exactly:

```
ssh root@BOX 'systemctl cat kodi'
```

Then create `/storage/.config/system.d/kodi.service.d/override.conf`, replacing only the binary path:

```
[Service]
Environment=LD_LIBRARY_PATH=/storage/.kodi/lib
ExecStart=
ExecStart=<the stock ExecStart, with the kodi.bin path replaced by /storage/kodi.bin>
```

Do not start `kodi.bin` bare: the stock start goes through a wrapper script that sets up the environment. If the wrapper itself names the binary, read it (`cat /usr/lib/kodi/kodi.sh`) and reproduce its final line with `/storage/kodi.bin`. Unverified: whether the wrapper keeps an `LD_LIBRARY_PATH` set in the unit. If the log in section 6 says the library was not found, add the directory in the wrapper's line instead. Do not add flags you have not checked against `kodi.bin --help`.

```
ssh root@BOX 'systemctl daemon-reload && systemctl restart kodi'
```

Revert to the stock Kodi:

```
ssh root@BOX 'rm -rf /storage/.config/system.d/kodi.service.d && systemctl daemon-reload && systemctl restart kodi'
```

## 6. Enable it and check

Settings, two switches, both needed:

1. Settings, System, Audio: turn on audio passthrough and "TrueHD capable receiver" (id `audiooutput.truehdpassthrough`), with the passthrough device set to your HDMI output.
2. Same page: turn on "Convert DTS:X to Dolby Atmos" (id `audiooutput.transcodedstx`). It is off by default and appears only when the TrueHD option is available. Set the settings level to Standard or higher if you cannot see it.

Optional key, in `/storage/.kodi/userdata/advancedsettings.xml`. If the file exists, merge this in; do not overwrite it:

```
<advancedsettings version="1.0">
  <audio>
    <dtsx2atmoskey><32 bytes as 64 hex digits></dtsx2atmoskey>
  </audio>
</advancedsettings>
```

Kodi redacts the element in its log. A value that is not an even number of hex digits is refused with a warning and the metadata stays unsigned.

Check: play a DTS:X title (DTS-HD MA or HRA with the X extension, 48 kHz) with debug logging on, then read `/storage/.kodi/temp/kodi.log`:

- `CDVDAudioCodecDtsToAtmos: loaded libhz_stream_ffi.so` means the loader found the library. If you see `not found, skipping transcode` (debug level), fix the library path.
- `opened real-time DTS:X to TrueHD Atmos transcode (output: MAT|RAW, key for the object metadata: set|none, unsigned)` means the codec is active and says whether a key was accepted.
- A warning about `<dtsx2atmoskey>` means the key was refused.
- `push failed`, `pull failed` or an "unsupported sample rate" line means trouble. An "opened" line repeating within one title means the context keeps being rebuilt.

## 7. Known limits and where to report

- 48 kHz DTS:X streams only. Anything else uses normal passthrough or decoding.
- Fold-downs are off, and IMAX Enhanced object content is not converted to objects.
- About 87 ms of latency; set Kodi's audio offset (about +87 ms to start with) and measure.
- The only tested box is one S922X. Nothing is guaranteed on other hardware.
- Not compiled as part of an official CoreELEC build yet.

Report problems as GitHub issues on `https://github.com/pkajaba/xbmc`. Include the CoreELEC version, the sound bar model, whether the output was MAT or RAW, and the lines from `kodi.log` quoted above. Remove any key, address or media URL first.
