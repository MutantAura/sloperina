# Status

Where the work stands, for picking it up again. Last updated 24th November 2026, after the 0.0.12 release.

## Released

**0.0.12** (tag `v0.0.12`, master `2ca5af4`). Since 0.0.11:

- **debug > return to title screen** from anything: cutscenes and other events, game over, the pause screen and map
  (it presses B for them), map loads (it waits; a warp armed for the next exit is cancelled), frame advance (it lets
  the game go). FINDINGS 9.26.
- **debug > actors**: browse current actors (folders by kind, a name filter), an actor's own menu (go to, spawn one
  like it, kill), spawn actor by name or type. FINDINGS 9.27–9.29.
- **debug > free camera**: the view only; the world isn't streamed round it. FINDINGS 9.30.
- **scene > time of day** and a **time of day** watch (24h as a gossip stone tells it, 12h, seconds). FINDINGS 9.31.
- Void out: the fence ground shading (#52), more void conditions and memfiles keeping the 8-unit ring (#53),
  both by MutantAura.
- World overlays freeze and fade over the game's pauses instead of flashing; **settings > menu settings > hide world
  overlays over item wheel** (on at first). FINDINGS 9.32.
- CONTRIBUTORS from GitHub's contributors list.

The game patches are the same as 0.0.11's (patch v6).

## Open pull requests

- **#24** Air walls: fill their faces, see-through in a checker (tomlube). Based on 0.0.4.2 and conflicts in four
  files. Master already has most of it: the face type, `airwall_faces`, `gpuui_world` and `gpuui_face` (taken over
  and reworked by the void view, with faces hidden behind the scenery), and `add_wall_faces`, which only void areas
  use. What's left is the user-facing part: **fill faces**, **faces of** (near Link / all) and **within (m)** for
  ordinary air walls, and the checker. Suggested: add those on top of master's face code, credit tomlube, close #24.

## Unfinished or not checked

- **Time of day before the story starts the clock**: on an early save the clock stays at 06:00 and ignores setting.
  Kaepora in Hyrule Field, or Impa's escort after Zelda's Lullaby, starts it; what they change isn't found. See
  FINDINGS 9.31 for what's ruled out. Next: the quest steps followed by a map reload (may autosave the save: ask
  first), or the GDB stub on the WorldMgr's request at `[wm+0x510]`.
- **The item wheel button the user uses**: when they opened their wheel, the overlays were hidden as for one of the
  game's menu screens, which the item wheel setting doesn't cover. The fade now stops the flashing either way, and the
  user found it fine. If the overlays should stay over that wheel too, read the menu screen value from the log's
  `air walls: game paused (... menu screen ...)` line and treat that value as the wheel.
- **Enemy health bars** still go and come back at once over the game's pauses (enemyhp.c); they could fade the same
  way.
- **Return to title screen, not checked in game**: a game over (setting health to 0 didn't kill Link), a dialogue or
  chest, Link mid-air or swimming.
- **#53, not checked in game**: restoring the ring from a memfile saved off the map, the 3D-versus-sideways snap guard
  in the shading, and the cost of the deep deadly-cell scan (down to 5,000 units on each box rebuild).
- **#52**: the red "stays stuck" shading wasn't seen; the whole-grid re-evaluation when the anchor moves could cost
  frames on slower hardware.
- **Free camera**: streaming the world round the camera isn't possible as is (it streams round Link); moving Link
  with the camera would, but sets off loading zones, triggers and voids.
- **Actors**: fetch (an actor to Link) was dropped (FINDINGS 9.28); whether a room that opens when its enemies die
  counts kills made with **kill** isn't checked.
- **Remote control**: a script can't open a submenu (`do` on one says it's a menu), so pages like an actor's menu or
  **known watches** can't be reached from a script.

## Notes for the next session

- **Pushing**: pushes to master need asking for explicitly (the session's safety check refuses otherwise).
- **Release builds**: run `./docker-make clean` before `scripts/make-release.sh`. The version string is passed to the
  compiler as a flag that Make doesn't track, so a tagged build can otherwise keep the previous version's string
  (0.0.12's first build did).
- **Screenshots of the emulator**: use `PrintWindow` (it asks the window to draw itself), not a copy of the screen,
  which catches whatever window is on top.
- **The user's setup, changed while testing**: `remote_control=1` in settings.ini; **hide world overlays over item
  wheel** was set on by a test script; **map's outer box** was switched back off. The game's autosave recorded test
  warps on the early save (Hyrule Field); the save backups from before testing are in `sloperinaGZ/save-backups`.
- **Local branches** merged into master and safe to delete: `feat/*` from this work (`actors-menu-tidy`,
  `debug-actors`, `free-camera`, `link-menu-layout`, `no-void-outs`, `remote-title`, `time-of-day`, `title-failsafe`,
  `wheel-overlays`), `fix/remote-input-off`, `pr52`, `pr53`. Test branches to delete: `test/all`, `test/combined`,
  `test/freecam-actors`, `test/pr50`, `test/pr52`, `test/pr53`. Old worktrees to remove: the agent worktrees under
  `.claude/worktrees/` and the scratch worktrees `wt24`, `wt50`, `wt51`.
