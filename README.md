# LG Input Mapper

<img src="assets/icon-512.png" width="96" align="right" alt="">

Remap, disable or repurpose the buttons on your LG Magic Remote, on a rooted
webOS TV, using nothing but the remote:

- **Remap a button** → press the button → choose what it should do:
  - do nothing, act as another button (Play/Pause, Mute, Info, numbers, colours, …),
    launch an app, switch to an input, or run a command (as root; with presets)
  - optionally a **different action when held** (long press), or a hold action
    only, leaving the normal press alone
- Key monitor to see the code of any button (buttons are held back while it is open).
- Starts at boot via Homebrew Channel; changes apply instantly.

It is a from-scratch successor to [Simon34545/lginputhook](https://github.com/Simon34545/lginputhook)
(LG Input Hook), built because that approach stopped working on webOS 24/25.
Tested on a 2025 C5 (webOS 10.3) with an MR25GA remote; it should work on any
rooted webOS 5+ TV on 32-bit or 64-bit ARM.

## Requirements

[Homebrew Channel](https://github.com/webosbrew/webos-homebrew-channel) with
root (root status must show OK in its settings). Launch LG Input Mapper once after
installing: it elevates its service, starts the remapper and enables the boot
script.

Some buttons are deliberately not remappable so the TV stays usable: the
D-pad, OK, Back, Home, Settings, Power, volume and channel.

## Install

### Homebrew Channel

Homebrew Channel > Settings > **Add repository**, and enter:

```
https://raw.githubusercontent.com/fivefold3/webos-homebrew-repo/main/repo.json
```

### Manual install

Grab the `.ipk` from the releases page and install it with
[webOS Dev Manager](https://github.com/webosbrew/dev-manager-desktop) or
`ares-install`. If you used LG Input Hook before, your mappings are imported
on first launch.

## How it works

The original LG Input Hook injected a PHP/frida hook into `lginput2`, which
breaks whenever LG changes glibc. LG Input Mapper does not touch any LG process:

1. `lginputmapperd`, a ~130 KB static C daemon, opens the remote's virtual input
   device (`LGE M-RCU - Builtin [0]`, created by `lginput2` via uinput) and takes
   exclusive control of it (`EVIOCGRAB`), so the TV no longer sees the raw events.
2. Every event is looked up in your mappings and the result is written into a
   sibling device the compositor already reads (`LGE M-RCU - Builtin [2]`).
   surface-manager does not pick up input devices created after it starts, so
   a fresh uinput clone would go unread; injecting into an existing sibling is
   the trick magic_mapper found. A clone is still used as a fallback.
   Devices whose key set the output device cannot deliver (on the C5 that is
   the IR receiver `LGE RCU`) are left untouched rather than half-working.
3. Long presses are detected in the daemon (the remote sends no repeat events),
   exec/launch actions are spawned detached, and the config file is watched
   with inotify so changes apply instantly.

Layout:

- `daemon/` – `lginputmapperd.c` (+ vendored cJSON). Built with `zig cc` as static
  musl binaries for `arm` (soft-float, as used by LG's 32-bit userland) and `aarch64`.
- `service/` – Node service (`com.lginputmapper.app.service`): keeps the daemon
  running, installs the boot script, reads/writes the config, serves status and
  the live key log over Luna.
- `frontend/` – the TV app: vanilla JS with D-pad spatial navigation.
- `shared/keys.js` – known button codes (verified on an MR25GA).

Runtime files: config `/home/root/.config/lginputmapper/config.json`,
state `/tmp/lginputmapperd/{status.json,events.jsonl,daemon.log,service.log}`,
boot script `/var/lib/webosbrew/init.d/lginputmapper`.

### Config format

```json
{
  "version": 2,
  "holdMs": 500,
  "devices": { "include": ["LGE *"], "exclude": [] },
  "output": { "mode": "auto", "device": "LGE M-RCU - Builtin [2]" },
  "mappings": [
    { "key": 1037, "label": "Netflix", "action": { "type": "launch", "app": "org.jellyfin.webos" } },
    { "key": 1086, "action": { "type": "disable" } },
    { "key": 1042, "action": { "type": "replace", "to": 164 },
      "hold": { "type": "exec", "command": "luna-send -n 1 luna://com.webos.service.tvpower/power/turnOffScreen '{\"standbyMode\":\"active\"}'" } }
  ]
}
```

Action types: `pass`, `disable`, `replace` (`to`), `launch` (`app`, optional
`params`), `exec` (`command`, optional `onRelease`). Commands get
`LGINPUTMAPPER_KEY` and `LGINPUTMAPPER_VALUE` in their environment.

## Building

Needs Node ≥ 18 and [zig](https://ziglang.org) (for cross-compiling the daemon).

```sh
npm install
npm run build      # daemon (both arches) + webpack bundles into dist/
npm run package    # dist/com.lginputmapper.app_<version>_all.ipk
```

`scripts/deploy.sh <ssh-host>` copies the ipk over SSH, installs it through
the TV's install service and launches the app. Useful on the TV:
`lginputmapperd --list` prints all input devices and their key bitmaps;
`lginputmapperd --dry-run` logs events without grabbing anything.

## Credits

This project stands on other people's work:

- **Simon34545** — [lginputhook](https://github.com/Simon34545/lginputhook) (LG Input
  Hook), the original remote remapper for rooted webOS TVs and the project this one
  succeeds. **AkshayRao27** documented the button IDs of several LG remotes in that
  repository, which seeded the key table in `shared/keys.js`.
- **Andrew Fraley** — [magic_mapper](https://github.com/andrewfraley/magic_mapper)
  showed that grabbing the Magic Remote's input device and re-injecting events into a
  sibling device works; that is the mechanism `lginputmapperd` uses. Its button table
  (C5 additions by **DanielPower**) contributed codes as well.
- Prior art on the hook-based approach LG Input Hook used, none of whose code is
  used here: **Informatic**'s [inputhook gist](https://gist.github.com/Informatic/319bcaf94436b9136904473ca4f4ec9c),
  **smx-smx**'s [ezinject](https://github.com/smx-smx/ezinject) and
  **sundermann**'s [inputhookpp](https://github.com/sundermann/inputhookpp).
- **webOS Homebrew Channel** ([webosbrew](https://github.com/webosbrew/webos-homebrew-channel))
  provides the root access, service elevation and boot hook this app relies on.

Third-party code shipped in the package:

- [cJSON](https://github.com/DaveGamble/cJSON) — Dave Gamble and contributors, MIT
  (`daemon/vendor/cJSON.LICENSE`).
- [webOSTV.js](https://webostv.developer.lge.com/develop/references/webostvjs-webos) — LG Electronics,
  Apache-2.0, bundled from the `webostvjs` npm package.
- [core-js](https://github.com/zloirock/core-js) — Denis Pushkarev, MIT (polyfills in the app bundle).

## License

MIT. See `LICENSE`.
