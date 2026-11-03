# Mighty Arp 0.1: release checklist

Not ready to tag yet. Tick these off as they land; **Nick** marks the ones only Nick can do, the rest Claude can do
in the repo. The release itself is a `v0.1` tag pushed to GitHub: `.github/workflows/release.yml` then builds,
tests, signs and publishes it (fm1-base's template, the shared `zvenson/fm1-build` builder).

## 1. Review and fixes

- [ ] GPT's seventh review: everything since `7980a4f` (USER presets keep everything, PARAMS, RESET, the header).
      Waiting on GPT's credits; Claude writes `CODEX-REVIEW-PROMPT-5.md` when it's time
- [ ] Fix what it finds, tests green (`./build.sh --host && sh tests/run_tests.sh`)
- [ ] Open from before: MONO's fade-then-restart (decide: fix for 0.1, or later)
- [ ] Open from before: migration fixtures from real old firmware (decide: worth it before 0.1, now that there
      are users' saves to protect, or not)

## 2. Play-testing on the FM-1

- [ ] **Nick:** install the current build, check the USER presets already on the unit came forward (their
      sounds right; with PARAMS on CHANGE they load with the arp and effects as new)
- [ ] **Nick:** save a few presets with PARAMS on KEEP and on CHANGE; power off and on; they come back
- [ ] **Nick:** CPU at the worst realistic case: ten VA notes, long releases, the arp, shimmer up (no DROPS)
- [ ] **Nick:** a long session: the arp, chords and strum at high tempos, MIDI in and out, external clock
- [ ] **Nick:** levels across the 60 sounds by ear (none much louder or quieter than the rest)
- [ ] The browser emulator: SOUNDS didn't open from a click on HOME in Claude's session (the tests pass it);
      check it with a real click, fix if it's real

## 3. The repository

- [ ] `app/fm1.toml`: `repo = "https://github.com/<you>/<repo>"`
- [ ] `app/fm1.toml`: an `id_prefix` of its own (now `FM-1_11`, the template's TODO). **Nick:** ask Charles /
      the FM-1 community which prefix is free; releases are the prefix + XXYYZZ (0.1: `000100`)
- [ ] `.github/workflows/release.yml`: the `# <- yours` lines: `pkg=mightyarp` (twice), the concurrency group and
      the title `Mighty Arp`
- [ ] `app/words.tsv`: the `url:` line where the site will put its words (now a guess)
- [ ] The `CODEX-REVIEW-PROMPT*.md` files: into `docs/reviews/`, or out of the repository
- [ ] A `LICENSE` file at the root (MPL-2.0, as fm1-base), and the credits in README: fm1-base (Charles
      Vestal), felucca-bsp, Barlow (OFL) in the splash and fonts
- [ ] Rights check: every sound here is made by the engine (no samples), so
      nothing to clear. Confirm nothing else was copied in (the chord voicings are Nick's own, MacroPicoMIDI)
- [x] Submodules: `base/` on fm1-base's public `main`, `base/bsp/` on felucca-bsp's: the builder can check out both

## 4. Documentation

- [ ] `app/docs/MANUAL.md`: Mighty Arp's own manual (now the template's): what it is, the buttons, every page
      and knob, the chance arp, CHORDS and STRUM, USER presets and PARAMS, RESET, MIDI and clock, installing and
      going back to stock. SPEC.md and README.md have most of it
- [ ] Manual screenshots (`app/docs/img/`, now the template's): from the host simulator's `shot`
- [ ] README: a short "what it is" at the top for someone arriving from GitHub, and the install steps

## 5. Release

- [ ] **Nick:** create the GitHub repository (public) and push `main`
- [ ] **Nick:** a beta first, to check the workflow end to end:
      `git tag v0.1-beta1 && git push origin v0.1-beta1` (a dash: published as a pre-release, not latest)
- [ ] **Nick:** install the beta's `.fwsc` from the GitHub release onto the FM-1 (not a local build)
- [ ] **Nick:** then `git tag v0.1 && git push origin v0.1`
- [ ] Optional, after 0.1: the site (`tools/make_site.py`, `publish_site.sh`): the browser emulator, an
      installer page and the screen reader's words, on GitHub Pages
