# ARC Sound Override Blocker (`org.tigger2014.arcblock`)

Stops an ARC/eARC soundbar from stealing TV audio on a rooted LG webOS TV.
Pick TV speakers and the soundbar can't grab them back; pick the soundbar
manually and it works with sound.

Tested on an LG webOS TV (`eim`, glibc 2.35, 32-bit ARM soft-float).

NOTE: Fully written with AI using Muse 1.3 with some gentle persuasion along the way.

## Install

On the TV: Homebrew Channel → Settings → Add repository, paste:

```
https://raw.githubusercontent.com/Tigger2014/lgArcAutoBlock/main/apps.json
```

Then install the app from the channel (root required) and flip the toggle ON.
Fresh installs are OFF by design. Flipping the toggle restarts `eim` live
(~30s, audio drops for a moment) — that's the mechanism, not a side effect.

## How it works

- A Thumb-2 inline hook (`arcblock/hookthumb.c`) patches
  `handler_setSystemAudioMode` in `eim` and vetoes exactly the re-asserted
  *System Audio Mode ON* frames the soundbar sends seconds after yielding.
- The veto is output-aware: `SAM-ON` + `tv_speaker*` → dropped;
  `SAM-ON` + `external_arc*` → passes, so manual soundbar selection works.
- The service injects at boot (waits for the `/dev/cec0` holder, then stops),
  persists via `webosbrew/init.d`, and restores your last output after reboot.

Nothing touches system partitions: no `/etc` writes, no systemd edits, no
`ld.so.preload`, no binary patching.

## Layout

```
arcblock/        native injector (arcblock.cpp + hookthumb.c), Dockerfile +
                 soft-float armel cross-build, qemu-arm engine tests
arcblock-app/    homebrew app (UI + root service)
scripts/         deploy.sh (install on TV), package-app.sh (bundle IPK),
                 make-manifest.sh (release manifest)
```

## Build from source

Mac/Linux with Docker + `npm i -g @webos-tools/cli`:

```sh
./arcblock/build.sh          # ARM binaries + engine tests
./scripts/package-app.sh     # -> arcblock-app/org.tigger2014.arcblock_<ver>_all.ipk
./scripts/deploy.sh root@<tv-ip>
```

## Cutting a release

```sh
./scripts/make-manifest.sh  # -> arcblock-app/org.tigger2014.arcblock.manifest.json
git tag v<ver> && git push origin v<ver>
gh release create v<ver> arcblock-app/*.ipk arcblock-app/*.manifest.json --title v<ver>
```

The manifest's `ipkUrl` is the bare filename, so it resolves against
`.../releases/latest/download/...` — future releases reach users with no repo
changes. `apps-repo/` holds the submission pointer for `webosbrew/apps-repo`.

## Uninstall

Toggle OFF in the app, delete it from the Homebrew Channel, and remove
`/var/lib/webosbrew/init.d/arcblock`. Hooks live only in the injected
process — once `eim` restarts (or the TV reboots), nothing remains.

## Credits

Boot-persistence pattern + `ezinject` lineage: `lginputhook` (Simon34545) and
smx-smx's `ezinject`.
