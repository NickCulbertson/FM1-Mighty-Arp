# fm1-firmware

A firmware for the M-VAVE FM-1, built on [fm1-base](https://github.com/charlesvestal/fm1-base) (MPL-2.0) as a git
submodule in `base/`. The app is in `app/`, started from fm1-base's template.

    git clone --recursive <this repo>
    ./build.sh --host && tests/run_tests.sh      # host simulator, no JieLi toolchain needed
    ./build.sh --web                             # browser emulator: app/build/emu/
    ./build.sh                                   # the device package: app/build/template.fwsc

Still to do from the template's "Make it your own": a name, an own `id_prefix`, USB names and site details in
`app/fm1.toml` and `app/app.h`; the `# url:` line in `app/words.tsv`; the `# <- yours` lines in
`.github/workflows/release.yml`; the manual and landing page.
