# Status

Where the work stands, for picking it up again. Last updated 27th November 2026: 0.0.15 released; since then, the
drown timer watch and respawn points (not released, not committed yet).

## Since 0.0.15 (not released)

- **Drowning animals** (drownview.c, FINDINGS 9.47): cuccos, dogs, horses and Epona drown in deep water and come back.
  In the actor browser they have **drown timer watches** (four watches: **drown state** in words, and in frames
  **drown timer** until it starts to fade out, **fade timer**, **respawn in** until it's put back; a cucco's grab
  range hidden while its drowning switches lifting off) and **respawn point** (pink ring and beam where it comes back:
  a cucco's WarpPos, its last safe landing; a dog's home; a horse's last landing). Remote `drown`, `aidump`. Checked
  in game on 1.1.1 (27th November): a cucco thrown into Zora's River (in the water, looking for land, one jump out),
  one spawned in the river (back in 40 to 0, put back at WarpPos's x and z), Epona spawned in Lake Hylia (back in 358
  to 0, put back on her landing). Epona's three watches checked against frame advance's frames (drown 168 to 0 over
  153 frames, respawn within 5%, the fade 17 to 0 over 14 frames, no flicker between steps). A cucco's four watches
  checked in Zora's River (in the water 25 to 0, looking for land, jump 1 of 3, jumped out), and its lift radius not
  drawn while it drowned in Lake Hylia (drawn while carried). **Not checked:** a dog, **finding a safe spot** (a horse
  whose landing isn't safe), 1.0.0.

## Released

**0.0.15** (tag `v0.0.15`, GitHub release with `sloperinaGZ-0.0.15.zip`). Since 0.0.14:

- **scene > environment > hitboxes**: attack hitboxes, bump boxes, hurtboxes, invincibility and collectables, each
  off / outline / fill, drawn as the air walls are; the shield guard angle in 3D for Link and enemies (details below).
  Bodies the game took out of the world or switched collisions off for (Link's spare set at his load spot, a Stalfos's
  parked capsule) are left out (FINDINGS 9.45).
- **Frame advance's camera tools**: the free camera, actor browser and explorer while frame advance holds the game.
- **Actor browser**: chests (TBox_*) among objects, what a chest holds and a pot drops (its layout entry, FINDINGS
  9.46), a chest's open treasure box radius and **open prompt break line**; only the hitbox kinds and interaction
  parts an actor has; minus only for an actor with options.
- Merged pull request **#58** (SokolskyNikita): lank / lonk mode nothing / lank / lonk in any map, apply and reload.
- Remote: `hitlist [name | #number]`, `hitparts`, `guardbits`, `pausetype`, `equip shield`, `nohit`, `physdump`,
  `frametrace`; a script's pause and run work while a camera tool has frame advance's world stopped.
- **1.0.0 checked in game** (27th November): the pause manager (all 20 types, the same masks), frame advance's freeze
  with the free camera flown, hitboxes (no spare bodies), a Stalfos's four kinds and its guard drawn, a rupee's
  collectable, the castle-wall chest's contents, radius and prompt break line, and #58's lonk and back (two loads,
  Link put back).

**0.0.14** (tag `v0.0.14`, GitHub release with `sloperinaGZ-0.0.14.zip`). Since 0.0.13:

- **scene > environment** folded into **air walls**, **guards** and **actors** submenus.
- **Guard vision** (guards): the Gerudo guards' and castle soldiers' sight on the ground, cut by what hides Link,
  green searching / blue alerted / red caught / grey off duty, and their **patrol paths** (guardview.c, FINDINGS 9.36).
  Checked on 1.1.1 in Hyrule Castle's courtyard and Gerudo's Fortress, and with spawned guards on both builds.
- **Interaction ranges** (actors): where Link can talk to or act on an actor, each part its own option: **talk
  radius** (a door's **open door prompt radius**), **z-target talk**, **target break line** and a door's **door
  open/close radius** with the red half disc it pushes through as it shuts. Built as the game's own checks are, from
  the actor's live model box (FINDINGS 9.37, checked edge by edge round Npc_Hylia_022_Shop_Child), with the game's
  own A target and ZL candidate as markers.
- **Actor browser**: the beam locks on to an actor it rests on and the camera follows it; **minus** opens the actor's
  options, its own switches stacked with checkboxes in its panel: a guard's **vision** / **patrol path**, any walker's
  **path** (the layout's rail it's linked to), the interaction parts, and for a door **close timer watch** (the **door
  close timer** watch: frames before it shuts, 135 while held open, found in the door's AI or Link's walk-through
  event) and **open door** / **close door** (opens it by itself as for an NPC, or shuts it even with Link holding it
  open). x, y, z of the actor or the cursor in the browser, the explorer and the free camera (FINDINGS 9.38).
- **link > position**: **x / y / z** moved by a **step** (0.01 to 10 m).
- **lank / lonk mode** (renamed from Lunk): **apply** reloads the map itself.
- Remote control: `actor`, `attention`, `findref`, `peek`, `doortimer`; tools/gdbwatch.py (the GDB stub: watch,
  break, read), tools/interactions.py, tools/rails.py.

**0.0.13** (tag `v0.0.13`, GitHub release with `sloperinaGZ-0.0.13.zip`). Since 0.0.12:

- **debug > actors** reworked: **browse currently loaded actors** (the free camera over a beam: d-left/right from
  actor to actor, L/R the kind, A go to, Y kill or despawn; **look at** under it), **quick spawn** with a short list
  of each kind and every actor of it the game has (a name filter, a five-row scroller; **character equipment** under
  characters), placing with the sticks (Sims style; **spawn where**). Browse current actors, the actor's own menu and
  spawn actor by name or type are gone; their list is in `docs/reference/spawnable-actors.xlsx` for review.
- **scene > explorer**: a map's dungeon rooms, grottos and interiors (data/places.json, tools/places.py), with take
  Link on/off. FINDINGS 9.33.
- **debug > free camera** reworked to the explorer's controls (zoom, height, back to Link, L3 control Link/camera,
  follow Link, R3 hide controls; its up kept straight, FINDINGS 9.30); **R + right stick up** toggles it.
- The tools' controls in a bar at the bottom centre with the game's button glyphs, fading in and out; watches and
  input display hidden meanwhile unless **menu settings** says otherwise; the module's other commands wait.
- Menus moved: equips under link, story under debug, **save / memfile management** (save management, memfiles)
  above debug, reload map at the foot of scene, return to title screen at the foot of debug. Labels no longer pushed in
  by a checkbox next to them; a scroller keeps the panel at its longest line.
- Merged pull requests: #54 quick spawn (FINDINGS 9.34), #55 warps > override age, #57 lank / lonk mode (beta,
  FINDINGS 9.35).
- Remote control: `open <menu path>` (the menu on screen there), `get` reads a row's own text, `do` presses a list's
  arrow.

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

## 0.0.15 in detail: hitboxes

- **scene > environment > hitboxes** (hitview.c, FINDINGS 9.39): the game's own physics shapes near Link or the
  camera, read live from both Havok worlds each frame, in gz's hit view colours and, as gz, only what can touch
  something now (switched on; a sensor with a contact collection): **attacks** (red, gz's AT, while their hit mask
  has Link's layer in it), **bump boxes** (white, gz's OC), **hurtboxes** (blue, gz's AC), **invincibility** (purple,
  filled: Link's or a fightable actor's hurtboxes while invincible; gz has none then). The user found a red box they
  stood in unhurt (the wagon horse's, no contact collection): fixed by those two checks. Not yet seen in game after
  the fix: an attack drawn only while it's switched on (a Deku Baba's bite).
  Checked on 1.1.1 with a Deku Baba (its bite red as it lunges, Link blue for about half a second after each bite),
  a Deku Scrub (its nut red) and the market's NPCs (grey). About 2 FPS on Lichii.
- **Enemy health / actor browser:** no bar and no life line for actors that can't be killed (no Death component, or
  still invincible from the start: NPCs, Deku Scrubs, Bongo Bongo's hands, signs, whose life is the game's default
  100). The browser's life line says **invincible** while it is.
- **Link's own attacks** (PlayerAttack sensors, his sword) drawn red while they're out, as gz's AT list has them:
  seen in game (the sword is **Y** in this game: a remote `input Y` swings). Its sensor is on 4 frames in normal play
  (switched off by a queued request on the 2nd, applied 3 frames later) and 1 frame under frame advance: whatever
  applies the request runs on while frame advance holds the game, so frame advance shortens the live window itself.
  Not followed further.
- **Options** (asked for 27th November): each kind **off / outline / fill**; **only link's**; **through walls** (off
  at first: lines and fills hidden by the scene depth, actors included, as gz's); an actor's own **hitboxes** switch in
  the actor browser's options (every actor now has options). Seen in game: hidden vs through walls, only Link's,
  fill. Not seen: the actor browser's switch.
- **Under the HUD: tried, then turned off** at the user's word, as it flashed (FINDINGS 9.40): the world's overlays
  are drawn at present time again, over the HUD. The code stays behind WORLD_UNDER_HUD (nvn.c, off; gpuui.c's SLOTS
  to 5 with it), and the game's render target calls aren't wrapped while it's off. What it would need: a dependable
  marker for where the game's HUD starts in its 1920x1080 picture.
- **Fill** is solid shapes at about 30% opacity, back faces left out (as gz's); from inside a sphere or capsule its
  inner side.
- **Bump boxes and hurtboxes** only for actors that are out (state 4 or 5): a bombchu's stayed after it went off
  (the user saw it); checked gone. Hidden enemies (a Wolfos in the ground) lose theirs too.
- **Drawn as the air walls are** (asked for 27th November: "not collide with the HUD, Link, prompts and overlays
  when you open the quick equip wheel"): Link hides what's behind him (the lines' depth margin is the walls' 3 cm,
  not the ground lines' 30 cm; through walls, Link's depth alone, not over everything), and they fade out whenever the
  game pauses (the wheel too, whatever the setting). Seen: the wheel (gone), Link and the horse (their far sides
  hidden). Still over the HUD and the action prompt: under the HUD is FINDINGS 9.40's (turned off).
- **The boomerang's attack** didn't show until a second, bigger sensor came on 13 frames into the flight: its first
  (PlayerAttack, on 2 frames after it leaves his hand) is a 32-point shape with more faces than the 40 allowed, and
  the box drawn instead was lines only, which fill leaves out. Now up to 255 faces, and the box is filled. Seen.
- **Shield guard angle** (asked for 27th November, "the radius of how the shields work", and the Hylian and Mirror
  shields): while Link guards, green: every direction within 110° of his front, a bubble with a cone cut out
  behind (FINDINGS 9.42; first drawn as a flat fan, the user asked for its height). Level measured, above and
  below from the parameters. Measured with a Deku Baba on the Deku
  and Hylian shields, the Mirror Shield's front and back alike (no side bite landed); its beam reflection
  (ReflectShield) isn't drawn. Remote `equip shield <actor>` for trying them.
- **Collectables** (asked for 27th November: rupees, drops, bugs, fish): a fifth kind, gold, their pickup sensors
  (FINDINGS 9.44), its own style (settings key hitbox_collectables) and actor browser switch. Seen on a spawned red
  rupee (and a heart and magic jar listed); bugs, fish, butterflies not seen.
- **Enemies' guards** (asked for 27th November, a Stalfos's shield, and Dark Link's): green, 50° each way from their
  front while their animation's Guard hold event is on (FINDINGS 9.43); Link's now from the same event (not his
  action ids). Seen on a Stalfos in Hyrule Field. Dark Link not seen (Water Temple). Lichii hung once after a
  Stalfos spawn (the user's).
- **The actor browser's hitbox switches** are four now (asked for 27th November): attacks, bump boxes, invincibility,
  hurtboxes, each on its own for that actor (a ticked kind is drawn whatever show and its style say). Seen: an NPC's
  bump box alone, then with its hurtbox, then its hurtbox alone, with show off.
- **Frame advance's camera tools** (asked for 27th November): the free camera, the actor browser and the explorer work
  while frame advance holds the game: it draws with its world stopped (`DebugPause` raised with one bit more,
  pausemgr.c, FINDINGS 9.41), and is held again where it was when they're left. Seen in game on 1.1.1: the free
  camera flown and the browser stepped through actors; the actors within 30 m and a boomerang in flight identical
  before and after; one step after moved it one frame. Catch: actors off screen at the pause aren't drawn meanwhile.
  With the free camera, frame advance's button steps frames (the freeze lowered for one frame) and pauses a running
  game: checked by walking Link (L3, control Link): 0.13 m a tap, none between, a hold plays them on.
  1.0.0: checked in game (its pause manager is 1.1.1's offset less 0x70460, as derived).
- **Link's items** drawn while they can hit: Deku Nuts, Din's Fire, the boomerang, slingshot seeds (seen); arrows not
  tried (adult only). Spent ones (put away, state 6) left out.
- **Bomb blasts** drawn as attacks (FINDINGS 9.39): seen with a bomb going off in Link's hands. Not seen: a thrown
  one from outside, bombchus, Dodongos, bomb flowers.
- Remote: `physdump` (raw capture of the Havok bodies for working out layouts offline).
- **Not checked:** boss attacks, Wolfos / Stalfos swings (a Wolfos spawn hung Lichii, below), NPCAttack (moving spikes), fire; enemy
  invincibility other than the Deku Baba's (guarding Stalfos, Iron Knuckle); the timed reservations at `L+0x60`;
  gz's (non-GPU) drawing style.
- **Lichii hangs on some enemy spawns** (twice on 27th November: a Wolfos, a Deku Baba): its GPU thread stops about
  2 s after "ASTC GPU decodes" for the new actor's textures; the same as the hang seen on 25th November. Looks like
  Lichii's GPU-side ASTC path, not the module (the game thread ran on).

## Open pull requests

- **#24** Air walls: fill their faces, see-through in a checker (tomlube). Based on 0.0.4.2 and conflicts in four
  files. Master already has most of it: the face type, `airwall_faces`, `gpuui_world` and `gpuui_face` (taken over
  and reworked by the void view, with faces hidden behind the scenery), and `add_wall_faces`, which only void areas
  use. What's left is the user-facing part: **fill faces**, **faces of** (near Link / all) and **within (m)** for
  ordinary air walls, and the checker. Suggested: add those on top of master's face code, credit tomlube, close #24.

## Unfinished or not checked

- **What stops Link at a chest** (asked for 27th November: its bump box): its 16-point closed body (GroundObject) is
  part of it, but with all four of its bodies' collisions off Link still stands on it (FINDINGS 9.46). Nothing drawn.
- **0.0.15 not checked**: bugs, fish and butterflies as collectables; Dark Link's guard; the lank / lonk look on
  1.0.0 (the reloads ran; no picture, Windows interop from WSL had stopped).
- **Not checked after 0.0.14**: interaction ranges for spheres, cylinder fans and the height bands; chests and other
  doors than the Treasure Chest Shop's; an NPC's **path** (the instance id `[[actor+0x3D8]+0x88]` taken to be the
  layout's Hash: remote `actor` prints both); guard vision in Gerudo's Fortress on duty, blue (alerted) in game, night
  values, 1.0.0 since the paths / status / rays; the door tools and interaction ranges on 1.0.0.
- **Explorer: waking a dungeon room without moving Link.** The game wakes a room only for where Link is, so the
  explorer takes him into each room and B puts him back. The user wants the room to wake for the camera alone, with
  Link left where he is: the plan (finding the game's room change and calling it) is in FINDINGS 9.33.
- **A Lichii hang, seen once** (25th November 2026): with take Link on, the explorer put Link in the Forest Temple's
  room 0 and its Wolfos attacked him; about 3 s later Lichii's GPU thread stopped ("Waited N s for the GPU thread to
  submit", eden_log.txt) and the game froze. The module's log was ordinary up to then (its game thread ran on); not
  seen again, and not known whether the module had a part in it.

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
- **Remote control**: `open` shows a submenu, but a script still can't type into the on-screen keyboard, so the
  quick spawn list shrinking to what its name filter finds (no padding rows, no arrows when it fits) isn't checked.
- **Remove spawned on an owned Epona** reported "0 removed" while she did stop; not looked into.

## Notes for the next session

- **Pushing**: pushes to master need asking for explicitly (the session's safety check refuses otherwise).
- **The user's saves (27th November)**: from 19:56 a 3-heart, item-less adult file was saved over slots 00-04 (not
  by a test; one test autosave at 20:24 went over slot 01's 20-heart save). Slot 05 still has 20 hearts; the
  backup `save-backups/2026-11-27_19-55-35 - Goron City` has 20 hearts in every slot. Not restored: the user's call.
  A copy of the whole save folder from before the 1.0.0 test is in the session's scratchpad (savecopy-before-1.0.0).
- **Release builds**: run `./docker-make clean` before `scripts/make-release.sh`. The version string is passed to the
  compiler as a flag that Make doesn't track, so a tagged build can otherwise keep the previous version's string
  (0.0.12's first build did).
- **Screenshots of the emulator**: use `PrintWindow` (it asks the window to draw itself), not a copy of the screen,
  which catches whatever window is on top.
- **The user's setup, changed while testing**: `remote_control=1` in settings.ini; **hide world overlays over item
  wheel** was set on by a test script; **map's outer box** was switched back off. The game's autosave recorded test
  warps on the early save (Hyrule Field); the save backups from before testing are in `sloperinaGZ/save-backups`.
- **The GDB stub** (tools/gdbwatch.py, FINDINGS 9.37): Lichii's `use_gdbstub` is back to false; the game waits at boot
  for a debugger while it's on, and this stub's `D` leaves a stopped game stopped (gdbwatch resumes with `c`).
- **Local branches** merged into master and safe to delete: `feat/guard-vision`; `feat/*` from earlier work (`actors-menu-tidy`,
  `debug-actors`, `free-camera`, `link-menu-layout`, `no-void-outs`, `remote-title`, `time-of-day`, `title-failsafe`,
  `wheel-overlays`), `fix/remote-input-off`, `pr52`, `pr53`. Test branches to delete: `test/all`, `test/combined`,
  `test/freecam-actors`, `test/pr50`, `test/pr52`, `test/pr53`. Old worktrees to remove: the agent worktrees under
  `.claude/worktrees/` and the scratch worktrees `wt24`, `wt50`, `wt51`.
