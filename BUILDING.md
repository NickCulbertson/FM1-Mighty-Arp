# Building Mighty Arp

Mighty Arp is an app on [fm1-base](https://github.com/charlesvestal/fm1-base) (`base/`, a git submodule, with
felucca-bsp inside it). Clone with `--recursive`, or after cloning:

```
git submodule update --init --recursive
```

`./build.sh` runs fm1-base's build (`base/tools/build.py`) on `app/`, whose settings are in `app/fm1.toml`. It
makes, in `app/build/`:

| File | What |
| --- | --- |
| `mightyarp.bin` | the firmware app |
| `loader/ota.bin` | the update loader (felucca-bsp's) |
| `mightyarp.fwsc` | the installable package (app + loader) |

## Prerequisites (macOS)

- Python 3 with Pillow: `pip3 install Pillow`, with raqm for the fonts (Homebrew's Python, with libraqm). Where the
  system's FriBiDi is the wrong architecture, `FRIBIDI_LIB` names a folder with an arm64 `libfribidi.0.dylib`
  (default `~/.local/fribidi-arm64/lib`, used if it is there).
- Docker Desktop, or podman with a `docker` script that runs `exec podman "$@"` first on your PATH. The JieLi
  toolchain is Linux x86-64 only; the build runs it in a `linux/amd64` `debian:bookworm-slim` container
  (Rosetta on Apple silicon). Keep the source tree in a folder the container can share, e.g. under `/Users`.
- The JieLi Linux toolchain (clang 4.0.1 for pi32v2, from JieLi's package server): see fm1-base's README.
- The JieLi AC79 SDK (Apache-2.0). The package uses three of its files (`cpu/wl82/tools/uboot.boot`,
  `cfg_tool.bin`, `cfg/eq_cfg_hw.bin`); none of it is part of this tree or of git.

  ```
  git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
      https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
  ```

- Emscripten (optional, for the browser emulator): on the PATH, or in `~/emsdk` (`EMSDK`).

On Linux x86-64 the toolchain runs natively and no container is needed.

## Build

```
./build.sh                    # app/build/mightyarp.fwsc
./build.sh --release X.Y      # app/build/mightyarp-X.Y.fwsc, identity FM-1_6279XXYYZZ
./build.sh --host             # the simulator: app/build/host/mightyarp_host
./build.sh --web              # the browser emulator: app/build/emu/ (serve it over http)
```

`JIELI_TOOLCHAIN` and `AC79_SDK` override the default locations (`~/.jieli/toolchain`, `~/fw-AC79_AIoT_SDK`),
`PYTHON` the Python that runs the build. The build also checks the image, RAM and pool sizes, that register access
stays in felucca-bsp, and that no double-precision code is linked.

Mighty Arp's package identity is `FM-1_6279` (`id_prefix` in `app/fm1.toml`): development builds report
`FM-1_627900`, release 1.0 `FM-1_6279010000`, 1.1 `FM-1_6279010100`. The part before the version stays the same for
every release, so updates install over
each other and keep what players saved.

## Tests

```
sh tests/run_tests.sh             # Mighty Arp's
sh base/tests/run_tests.sh        # fm1-base's own
```

Mighty Arp's run the whole app in the simulator from the scripts in `app/tests/*.txt`: the pages and knobs, every
sound (`sounds.txt`, made from the sound list by `tests/make_sounds_test.py`), the arp's modes, chords and strum,
the sequencer, presets and saves across a reboot, MIDI and clock, panics, overload and recovery. Screenshots land
in `app/build/host/`.

`app/build/host/mightyarp_host SCRIPT OUTDIR` runs one script. The commands are listed at the top of
`base/host/plat_host.c`, and Mighty Arp's own checks (`expect NAME`) in `app_expect()` at the end of `app/app.c`.

## Install

From the command line (needs `pip3 install mido python-rtmidi`):

```
python3 base/tools/fm1_install.py app/build/mightyarp.fwsc
```

Installing firmware is at your own risk. If the FM-1 no
longer starts but reaches the chip's update mode (4C4A:8057 on USB), `base/bsp/tools/fm1_rescue.sh` puts stock
firmware back from a Mac.

## Publishing a release

A release is built on GitHub from its tag, so anyone can check that the package came from this source:
`.github/workflows/release.yml` calls the shared FM-1 builder,
[`zvenson/fm1-build` at `v1`](https://github.com/zvenson/fm1-build/tree/v1). The builder checks out the tagged
commit, installs JieLi's Linux toolchain and the AC79 SDK at its fixed tag, runs `./build.sh --release X.Y` (with
fm1-base and felucca-bsp at the commits this one links) and `tests/run_tests.sh` (a failing test stops the
release), then signs build provenance for `mightyarp-X.Y.fwsc` and `mightyarp-X.Y-app.bin`. The Firmware Hub shows
such packages as built from source.

1. Optionally write the notes first, as a draft release named after the tag (`vX.Y`): the workflow keeps a draft's
   notes; without one it makes a draft from the commits.
2. Push the tag (`vX.Y`; in GitHub Desktop: History, right-click the commit, Create Tag, then Push origin). The
   workflow builds, tests, attests, checks the SHA-256 sums and the signatures, then publishes the draft. A tag
   with a suffix (`v1.0-beta1`) is a prerelease. A published release is never rebuilt: a new build needs a new tag.
3. The site from that package: download it into `app/build/` (from the release page, or `gh release download vX.Y
   --pattern '*.fwsc' --dir app/build --clobber`), then `tools/make_pages.sh X.Y`: it builds the browser emulator
   and the site (landing page, emulator, fm1-base's web installer, manual, the words files, the firmware) and
   commits it to the local `gh-pages` branch. Push that branch (GitHub Desktop: switch to `gh-pages`, Push origin,
   back to `main`); GitHub Pages serves it at <https://nickculbertson.github.io/FM1-Mighty-Arp/>.

To check a downloaded package yourself (GitHub CLI, signed in):

```sh
tag=vX.Y repo=NickCulbertson/FM1-Mighty-Arp
gh release download "$tag" --repo "$repo" --dir build/verify --pattern '*.fwsc'
git fetch origin "refs/tags/$tag:refs/tags/$tag"
gh attestation verify "build/verify/mightyarp-${tag#v}.fwsc" --repo "$repo" \
  --signer-workflow zvenson/fm1-build/.github/workflows/build.yml \
  --source-ref "refs/tags/$tag" --source-digest "$(git rev-parse "$tag^{commit}")"
```

A local release build (`./build.sh --release X.Y`, package `app/build/mightyarp-X.Y.fwsc`) is still how to try one
on the FM-1 before tagging.
