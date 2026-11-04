# Mighty Arp 1.0: release checklist

Tick these off as they land; **Nick** marks the ones only Nick can do. The release itself is a `v1.0` tag pushed to
GitHub: `.github/workflows/release.yml` then builds, tests, signs and publishes it (fm1-base's template, the shared
`zvenson/fm1-build` builder). Release 1.0's package identity is `FM-1_62791001010000`.

## 1. Review and fixes

- [x] GPT's reviews seven to eleven (`2fb4dd1..9e8ff31`): every finding fixed (the release workflow, panics and
      the arp's chords, RESET mid-strum, swung strums, the names' generator, FACTORY's confirm and failing flash)
- [x] The sequencer (PLAY: ARP -> SEQ -> KEYS; SEQ-PLAN.md): GPT's reviews of `424c465..781d851`, every finding
      fixed with a check in `seq2`; the last review found nothing new
- [x] Saves: settings `MARF`, presets `MAU6` are 1.0's, the first formats (no saves exist from before 1.0, so no
      migrations). A later release that adds or removes a sound or a knob bumps them and brings 1.0's forward
- [x] W14 NOISE on EDIT: drawn and named (it read a missing name)
- [ ] Known, left for after 1.0: a root handed from one MIDI channel to another while sounding (channel 1 starts
      C4, channel 2 takes it over, channel 3 holds another root): All Sound Off on channel 2 misses C's chord
      tones (they end at their gate). Rare; fix only if it shows up in real use
- [ ] Open from before: MONO's fade-then-restart (decide: fix for 1.0, or later)

## 2. On the FM-1

- [x] **Nick:** the current build installed (`7b8aa55`); older USER presets (MAU5) gone, as expected
- [ ] **Nick:** save a few USER presets (one with its own sequence) with GLO → PRESETS on KEEP and on CHANGE; power
      off and on; they come back, and on CHANGE each brings its steps (a factory sound the default sequence)
- [ ] **Nick:** the sequencer: write a pattern, ties at GATE 100 %, transpose by key, external clock Stop and
      Continue
- [ ] **Nick:** CPU at the worst realistic case: ten VA notes, long releases, the arp, shimmer up (no DROPS on GLO)
- [ ] **Nick:** a long session: the arp, chords and strum at high tempos, MIDI in and out, external clock
- [ ] **Nick:** levels across the 64 sounds by ear (none much louder or quieter than the rest)

## 3. The repository

- [x] `app/fm1.toml`: `repo = "https://github.com/NickCulbertson/fm1-mighty-arp"`; the landing page's Source link
      and `app/words.tsv`'s `url:` (`nickculbertson.github.io/fm1-mighty-arp/words/`) to match. Another name for
      the repository: change those three
- [x] `app/fm1.toml`: `id_prefix = "FM-1_62791001"` (MBPX on a phone keypad, app 1, 001; Nick's choice). Releases
      are the prefix + XXYYZZ (1.0: `FM-1_62791001010000`, 19 of the 20 characters a package holds). No registry
      exists: optionally tell Charles, or propose a list of IDs in use to fm1-base
- [x] `LICENSE` at the root (MPL-2.0, as fm1-base); the credits in README (fm1-base, felucca-bsp, the fonts)
- [x] Rights: every sound is made by the engine (no samples); the chord voicings are Nick's own (MacroPicoMIDI)
- [x] Submodules: `base/` on fm1-base's public `main`, `base/bsp/` on felucca-bsp's: the builder can check out both
- [x] The browser emulator's clicks: Nick's work (only Claude's automated clicks didn't reach it)

## 4. Documentation

- [x] `app/docs/MANUAL.md`: what it is, installing and going back to stock, every page and knob, the chance arp,
      CHORDS and STRUM, the sequencer, USER presets and PRESETS, RESETS, MIDI and clock
- [x] Manual screenshots (`app/docs/img/`): from the host simulator's `shot`
- [x] README: what it is for someone arriving from GitHub, installing, building, releasing, the licence and credits
- [x] `app/README.md`: the app's files (was the template's)

## 5. Release

- [ ] **Nick:** create the GitHub repository `NickCulbertson/fm1-mighty-arp` (public), then:
      `git remote add origin https://github.com/NickCulbertson/fm1-mighty-arp.git && git push -u origin main`
- [ ] **Nick:** a beta first, to check the workflow end to end:
      `git tag v1.0-beta1 && git push origin v1.0-beta1` (a dash: published as a pre-release, not latest)
- [ ] **Nick:** install the beta's `.fwsc` from the GitHub release onto the FM-1 (not a local build)
- [ ] **Nick:** then `git tag v1.0 && git push origin v1.0`
- [ ] Optional, after 1.0: the site (`base/tools/publish_site.sh`): the browser emulator, an installer page, the
      manual and the screen reader's words, on GitHub Pages (`nickculbertson.github.io/fm1-mighty-arp`)
