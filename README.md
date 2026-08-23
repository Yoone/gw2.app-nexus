# GW2.app Nexus Module

Access your [GW2.app](https://gw2.app) lists in game, as a
[Nexus](https://raidcore.gg/gw2/nexus) addon.

Supports waypoint copying, achievement tracking, timers, story progress, equipped gear,
Trading Post price/order/history watchers, daily fractals, Wizard's Vault, PSNA, and much
more. Open [gw2.app/nexus](https://gw2.app/nexus) to get started.

This is a port of the [Blish HUD module](https://github.com/Yoone/gw2.app-blishhud). It puts
your lists on screen while you play. The website draws each row and sends it to the addon as
a picture, so everything above comes directly from GW2.app.

## Local development (macOS)

```sh
make toolchain   # one-time: brew install mingw-w64
make check       # verify compiler + GW2/Nexus install are found
make dev         # build, deploy into the GW2 prefix, launch the game
make logs        # tail Nexus.log
```

Also available: `build`, `deploy`, `undeploy`, `run`, `log`, `clean`.

The module can be dynamically re-loaded by Nexus using the UI in game (after a `make deploy`).
