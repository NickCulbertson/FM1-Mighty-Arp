# Mighty Arp 0.1: release checklist

Not ready to tag yet. Tick these off as they land; **Nick** marks the ones only Nick can do, the rest Claude can do
in the repo. The release itself is a `v0.1` tag pushed to GitHub: `.github/workflows/release.yml` then builds,
tests, signs and publishes it (fm1-base's template, the shared `zvenson/fm1-build` builder).

## 1. Review and fixes

- [x] GPT's seventh review (`2fb4dd1..ce7429c`): all five findings fixed in `2539d9b` (release workflow, panic and
      the arp's chords, RESET mid-strum, swung strums, the names' generator); tests green
- [x] GPT's eighth review (`ce7429c..8ce5404`): both panic findings fixed (a channel's queued and sounding arp
      notes stop, another's play on; All Notes Off releases the arp's gated voices)
- [x] GPT's ninth review (`8ce5404..03fc476`): FACTORY with a failing flash keeps the presets; FACTORY clears
      the keys held; All Sound Off on a channel the arp didn't play for leaves its notes; the manual's path
- [x] GPT's tenth review (`03fc476..9ace38b`): FACTORY never goes through SURE? in one turn; All Sound Off stops
      only the voices its channel's notes made; a key held through FACTORY sounds again
- [x] GPT's eleventh review (`9ace38b..9e8ff31`): FACTORY confirmed by SAVE (no knob turn erases); a note two
      channels hold stops only when neither does
- [ ] Known, left for the beta: a root handed from one MIDI channel to another while sounding (channel 1 starts
      C4, channel 2 takes it over, channel 3 holds another root): All Sound Off on channel 2 misses C's chord
      tones (they end at their gate). Rare; fix only if it shows up in real use
- [ ] GPT's suggested beta test on the FM-1: ten VA notes, long releases, shimmer up, a swung strum, MIDI in and out
- [ ] Open from before: MONO's fade-then-restart (decide: fix for 0.1, or later)
- [x] Migration fixtures: not needed. There are no saves from before 0.1, so the old-format migrations are gone;
      0.1's formats (settings MARE, presets MAU5) are the first. A later release that adds or removes a sound or a
      knob bumps them and brings 0.1's forward
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
- [x] `app/fm1.toml`: `id_prefix = "FM-1_62791001"` (MBPX on a phone keypad, app 1, 001; Nick's choice).
      Releases are the prefix + XXYYZZ (0.1: `FM-1_62791001000100`, 19 of the 20 characters a package holds). No registry exists: optionally tell Charles, or propose a
      list of IDs in use to fm1-base
- [ ] `app/words.tsv`: the `url:` line where the site will put its words (now a guess)
- [ ] The `CODEX-REVIEW-PROMPT*.md` files: into `docs/reviews/`, or out of the repository
- [ ] A `LICENSE` file at the root (MPL-2.0, as fm1-base), and the credits in README: fm1-base (Charles
      Vestal), felucca-bsp, Barlow (OFL) in the splash and fonts
- [ ] Rights check: every sound here is made by the engine (no samples), so
      nothing to clear. Confirm nothing else was copied in (the chord voicings are Nick's own, MacroPicoMIDI)
- [x] Submodules: `base/` on fm1-base's public `main`, `base/bsp/` on felucca-bsp's: the builder can check out both

## 4. Documentation

- [x] `app/docs/MANUAL.md`: Mighty Arp's own manual (now the template's): what it is, the buttons, every page
      and knob, the chance arp, CHORDS and STRUM, USER presets and PARAMS, RESET, MIDI and clock, installing and
      going back to stock. SPEC.md and README.md have most of it
- [x] Manual screenshots (`app/docs/img/`, now the template's): from the host simulator's `shot`
- [ ] README: a short "what it is" at the top for someone arriving from GitHub, and the install steps

## 5. Release

- [ ] **Nick:** create the GitHub repository (public) and push `main`
- [ ] **Nick:** a beta first, to check the workflow end to end:
      `git tag v0.1-beta1 && git push origin v0.1-beta1` (a dash: published as a pre-release, not latest)
- [ ] **Nick:** install the beta's `.fwsc` from the GitHub release onto the FM-1 (not a local build)
- [ ] **Nick:** then `git tag v0.1 && git push origin v0.1`
- [ ] Optional, after 0.1: the site (`tools/make_site.py`, `publish_site.sh`): the browser emulator, an
      installer page and the screen reader's words, on GitHub Pages
