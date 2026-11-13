# Working on HSF

Thanks for helping. Pull requests and bug reports are welcome.

## Licence and credit

HSF is under the [PolyForm Noncommercial License 1.0.0](LICENSE): free to learn from, change and share, but never for money.

- Everyone whose pull request is merged is credited by name in the Contributors section of the [README](README.md) and in the release notes of the version that includes their work.
- By opening a pull request, you agree that Capt-Hunter may use your contribution in HSF under any licence, including commercial ones.
- Contribute only your own work. No code from other projects unless its licence allows it and it is credited in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and never GPL code.

## Before you start

- Read [docs/architecture.md](docs/architecture.md) and the [source map](docs/source-map.md). They explain the routes and where each lives.
- Read the lessons in [docs/troubleshooting.md](docs/troubleshooting.md#4-lessons-learned). Each one cost days to find.
- Set up the build as described in [docs/building-and-testing.md](docs/building-and-testing.md).

## Rules that are not negotiable

- **Anti-cheat.** Never run HSF, a test tool or an install while a game with online anti-cheat is running (Call of Duty, Battlefield, Valorant and the like). Run `HSF/tools/guard.ps1` first; the install scripts do this themselves.
- **No third-party code or files in the repository or a release.** This covers LaunchPad, LumeniteFX, MFG-Unlocker, OptiScaler and any other mod. Players download those themselves. Do not copy code from GPL projects.
- **No driver or system changes.** HSF never changes driver profiles, system settings or file permissions.
- **Players never edit JSON.** Settings live in the in-game menu, and in `HSF.ini` for testers.

## Making a change

1. Branch from `main` as `hsf-<topic>`.
2. Build both x64 and x86 when you touch shared code (the menu, `dlssg_link.h`, depth, overlays). The x64 build should have no warnings at `/W4`.
3. Run the offline tests that cover your change, and look for `FAIL` in their output.
4. Test in a game. Changes to shared code need one D3D11 game and one D3D12 game. Install only while the game is closed, and never start two copies of it.
5. Write comments that say *why*, not what happened on which day. History belongs in the commit message.
6. Keep `docs/` in step: a new `HSF.ini` key goes in [settings.md](docs/settings.md), and a new file goes in the [source map](docs/source-map.md).
7. Open a pull request into `main`. After it merges, delete the branch.

## Judging quality

The eye test decides. A measurement never overrides a clear improvement you can see in play. Every improvement goes in unless it costs real performance.

## Reporting a problem

Include the game, its API, your GPU and driver, the route that the HSF menu shows, and the log. Use COPY LOG TO CLIPBOARD in the DEBUG section, or see [the logs](docs/troubleshooting.md#2-logs).
