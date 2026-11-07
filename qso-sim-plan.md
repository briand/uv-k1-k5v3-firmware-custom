# QSO drill: follow-up plan before merging

Branch: `dev/qso_sim` (commit 56d4553d, Andrew's QSO drill, band noise and DAC
synthesizer, rebased onto current main). This branch is **not going into v2.0**. It
is aimed at the release after that. This file lists the changes we'd like before
merging, for Andrew to review and comment on.

## Where it stands

- It builds and fits in the CW preset, but only with LTO. With `-DENABLE_LTO=OFF`
  it overflows flash by about 1.9 KB. LTO is new to this firmware and untested with
  overlay apps and multiboot; see "LTO: needs testing" below.
- The new code reserves about **1.8 KB of RAM for good**, used only while code
  practice is open. All of it is file-scope `static` arrays in `.bss`, not stack.
  The linker reserves it at boot and it is never released, though only practice
  code touches it: `cpo.c`, `cpocall.c` and `cpoqso.c` are the only callers of the
  synth and DAC.

  | Owner | Buffers | Bytes |
  |---|---|---|
  | QSO drill (`cpoqso.c`) | `s_pile` 224, `s_say` 200, `s_over` 96, `s_tok` 96, calls/names/QTH ~60, scalars ~40 | ~720 |
  | Synthesizer (`cwsynth.c`) | `s_voices` 340, `s_acc` 320, `s_sine_ram` 128 | ~790 |
  | DAC driver (`dacaudio.c`) | `s_buf` 320 | 320 |

  RAM that only one occasional feature uses is the main thing we want fixed.

## Change Options

| Item | Targets | Effect | Priority |
|---|---|---|---|
| 1. Sector cache borrow | **RAM** | Frees the ~1.8 KB reserved all the time. Costs a little flash: pointer access plus the borrow/return/guard code, a few hundred bytes at most (measure when built). | **High.** RAM is the scarcer resource, and this wins back the most. The risk is real but contained: the write guard makes a missed path lose a write, not corrupt flash. Medium work. |
| 4. Menu Option B | **Flash** (and consistency) | Removes most of practice's ~1.1–1.3 KB of menu code. The new main-menu items cost some of that back. RAM barely changes. | **Medium.** A solid flash win plus a consistent UI. Medium work; the risk is in the round trip between practice and the main menu, and in losing live preview. |
| 3. QSO without SYNTH tier | **Both, in that build only** | The QSO-only build leaves out the synth code and its ~1.1 KB of synth and DAC RAM. Builds that include SYNTH don't change. | **Medium.** Low-to-medium work on code paths that mostly exist already, low risk. It adds choice rather than freeing space in the full build. |
| 4. Menu Option A | **Flash** (smaller) and consistency | A few hundred bytes from shared drawing code. | **Low.** Cheap and safe, but a small win, and B replaces it. Do it only if B's round trip proves awkward. |
| 2. Overlay-app text assets | **Flash** | Moves the QSO text tables out of the main image. No change to RAM; the tables are copied into the borrowed workspace while practice is open. | **Low.** The size of the flash win is unmeasured. Packaging the asset app and handling a missing one is high work with some risk. Hold until flash is the limit. |

Priority weighs what's won (RAM counts for more than flash) against risk and work. The
rows are in priority order, which matches the suggested order below.

## 1. Borrow the flash sector cache for practice state (main proposal)

**Targets: RAM.** The ~1.8 KB of state reserved all the time. Not a flash saving.

`driver/py25q16.c` keeps a 4 KB `SectorCache` for read-modify-write of the SPI flash.
It is used only inside `PY25Q16_WriteBuffer` and `PY25Q16_SectorErase`; plain reads
skip it. The multiboot overlay already reuses the same buffer as its workspace
(`.bss.mb_workspace`, `PY25Q16_OverlayBuffer()`), so lending it out is an existing
pattern.

Nothing in code practice writes to flash while it runs. Settings changed in practice
are compared on the way out, and `CPO_Exit` raises `gRequestSaveSettings` only after
practice has ended. So the cache sits unused the whole time practice is open, and
the ~1.8 KB above fits in it with room to spare.

### Shape

- Put all of the QSO, synth and DAC state into one struct, `CPO_Workspace_t`, and
  give each file a pointer into it in place of its file-scope statics.
  - A `_Static_assert(sizeof(CPO_Workspace_t) <= SECTOR_SIZE)` keeps it honest.
  - Small scalars that the IRQ path or the status bar reads outside practice can
    stay as globals. Only the bulky arrays need to move.
- Add `PY25Q16_BorrowCache()` and `PY25Q16_ReturnCache()` (names open to change).
  - **Borrow:** waits for flash idle, invalidates the cache, sets a `s_lent` flag
    and returns the buffer.
  - **Return:** clears the flag and invalidates again, so the next write re-reads
    its sector rather than trusting the cache contents.
- `CPO_Enter` borrows first and then `memset`s the workspace. `CPO_Exit` returns it
  last, after `CWSYNTH_StopNow()` has stopped the DAC DMA, which reads `s_buf` and
  `s_acc` from the IRQ, and before the deferred settings save runs.
- **Guard:** while `s_lent` is set, `PY25Q16_WriteBuffer` and `PY25Q16_SectorErase`
  refuse to run (return early, plus a debug assert). That turns any write path we
  missed into a lost write rather than corrupted flash.

### Prerequisite: no serial during practice

CHIRP and the UART command set write flash, so the serial command handler is shut
off while practice is open.
- Most CW operators are on TRS or USB key input in practice anyway, and those use
  the port.
- A host that connects mid-practice just gets no answer until the user leaves
  practice.
- This is also the only real exposure the guard above protects against.

### Other write paths to audit

- `gRequestSaveChannel` and `gRequestSaveVFO`: practice captures the keys, but
  check that nothing in the main loop's periodic work can still raise them. Battery,
  backlight and the scan list are the places to look.
- Multiboot overlay launch: it can't be reached from practice, but assert it anyway.
- AirCopy and the flash dump/restore commands: covered by the serial shutdown.

### Expected result

About 1.8 KB of RAM back for the main firmware, with no change to how practice
behaves. The cost is roughly 30 lines in the flash driver, plus the struct move in
`cpoqso.c`, `cwsynth.c` and `dacaudio.c`.

### Rejected alternative: the stack gap

Placing the state at the top of RAM, below the deepest point the stack reaches,
works on paper. But it depends on a measured stack depth that changes as features
grow, and nothing fails cleanly when that measure goes stale. The cache has a
defined size and owner and a guard we control.

## 2. Text tables as overlay-app assets (optional, after #1)

**Targets: flash.** No change to RAM.

Each overlay app slot carries up to 3840 bytes of assets at `slot + 0x100`, and the
firmware already reads another app's assets (`app_config_proxy_resolve`). The QSO
drill's text (names, QTHs, call templates, phrase tables) could live there:
- **What stays put:** the drill code stays in the main firmware.
- **What moves:** the tables live in a data-only "app".
- **How it loads:** practice copies them into the borrowed workspace on entry.

This frees flash, which helps the LTO-off overflow and gives room for later
features. It costs some complexity in packaging, and in what happens when the
asset app is missing (the drill could fall back to a tiny built-in set, or refuse
to start).

We'll only do this if #1 lands cleanly and flash becomes the next limit.

## 3. Build tiers: QSO without SYNTH

**Targets: flash and RAM, only in builds that leave SYNTH out.** It's mainly about
choice: a QSO drill for builds that can't afford the synth.

Today, QSO requires SYNTH and SYNTH requires CPO (enforced in
`App/CMakeLists.txt`). That's because the other operator's replies are sent on a
synthesizer voice (`CWSYNTH_Send` in `cpoqso.c`), like the band noise and the pile.
But one station at a time doesn't need the synth. The BK4819 tone generator that
plays your own sidetone can play it:
- `LocalToneOn` in `cwapp.c` turns the tone on with `BK4819_SetAF(BK4819_AF_ALAM)`
  and sets its pitch on every key-down. The error cue already swaps in a different
  frequency there.
- `cpocall.c`'s copy drill already plays its calls this way, through
  `CW_StartTextPlayback`, when the synth isn't running.

### Plan

- **Send path:** at about line 1509 of `cpoqso.c`, where the reply goes out, use
  keyer text playback when there's no synth. Today that spot just skips ahead "as
  if it had sent" (`s_state = s_after`). `s_speaking` then follows
  `gCW_PlaybackActive` in place of `CWSYNTH_IsSending`.
- **Another operator's pitch:** give the playback a tone offset, for example a
  `CW_StartTextPlaybackPitch(text, eff_wpm, offset_hz)` that stores the offset for
  `LocalToneOn` to add. Then the station is clearly not your own sidetone. Use the
  station's existing `st->pitch` (already random per caller for the synth voices),
  or a fixed offset such as +150 Hz, kept inside the BK4819's comfortable range.
- **Timing:** the playback already handles the station's speed and Farnsworth
  spacing (`eff_wpm`).

### What a single tone generator limits

- One sound at a time: no band noise, no pile calling over each other, and no
  station that keeps sending while you key. When you key over it, the playback has
  to stop and count as "you broke in." Check how the keyer handles paddle input
  during playback today.
- `s_playback_buf` is 93 bytes, so the longest over must fit. Either split long
  overs into chained playbacks, or check the longest `s_say` (up to 200) against it.
- The pile is capped at 1, or callers queue and go one after another.

Making QSO stand alone would give three useful tiers:

| Tier | Flags | What you get |
|---|---|---|
| Practice | `CPO` | Code practice, callsign send/copy drills |
| + QSO | `CPO` + `CPO_QSO` | QSO drill, one station at a time on the BK4819 tone, at its own pitch |
| + Synth | `CPO` + `CPO_QSO` + `CPO_SYNTH` | Band noise, pileups, overlapping voices |

Build changes: cut the `QSO requires SYNTH` rule, and put `#ifdef` around the
`CWSYNTH_*` calls in `cpoqso.c` (voice setup, gains and the pile's overlap logic).
- Without SYNTH, the middle tier has none of the synth's ~790 B and the DAC's
  320 B. So, together with #1, the QSO drill costs very little RAM.

## 4. The practice settings menu

**Targets: flash, plus consistency with the main menu.** RAM barely changes. The
practice menu holds only a few bytes of state, and its display struct lives on the
stack.

The practice MENU screen is new code from the ground up. It copies the main menu's
layout by hand (divider at x48, dotted rule, prev/name/next, `nn/nn`) but shares
none of its code. That's why it looks slightly off next to the main menu:

- **Value style:** values are one big line (`UP TO 7`, `15 WPM`, `EASY`). The main
  menu uses its two-line `Label\nvalue` style, in mixed case.
- **Badge:** PRACTICE sits at the bottom of the value pane. The main menu's
  rounded badge sits at the top right.
- **Missing pieces:** there's no category capsule in the status bar, no typing a
  number to jump to a row, and no repeat acceleration.
- **Keys row:** a key legend sits inside the row count.
- **Edit model:** edits apply live, and EXIT reverts. In the main menu, MENU
  confirms and EXIT cancels. The live behavior is deliberate, so `NLevel` and
  similar settings can be heard while you change them.

The menu code is about 1.1–1.3 KB of flash in all (`cpo.c` rows, get/set, keys,
timeout; `ui/cpo.c` `DisplayMenu`).

### Option A: share the drawing (light)

Factor the main menu's frame and its `\n`-splitting value printer out of
`UI_DisplayMenu` into a small helper that both menus call. Make the rounded badge
helper non-static as well.
- Practice values switch to the `Label\nvalue` style.
- The badge moves to the top right.
- Practice keeps its own row table and live-edit model.

This fixes the look, removes duplicate drawing code and saves a few hundred bytes.

### Option B: real main-menu items (fuller, recommended)

Make the practice settings ordinary `MenuList` entries in a "Practice" category,
using the standard `MENU_GetLimits`, `MENU_ShowCurrentSetting` and
`MENU_AcceptSetting` paths. MENU from practice opens the main menu on that category,
and EXIT returns to practice.
- **Look and behavior:** everything matches the rest of the menu, including the
  capsule, number entry and acceleration.
- **Space:** practice sheds most of its menu code, the biggest flash saving here.
- **Live preview:** lost, unless a narrow special case is added (for example, the
  main menu calls a hook when the selection changes on `NLevel`).
- **Keys row:** becomes the help text for the category, or goes away.
- **What needs checking:**
  - The main menu can open scoped to one category.
  - It returns to practice, not to the main screen.
  - Practice's radio state (borrowed modulation, monitor) survives the round trip.
  - Rows that apply only to some drills either hide when they don't apply or stay
    visible, as now.

This also meets the original wish to get settings code out of practice, without the
risks of a settings overlay app (see below).

## Not pursuing

- **A settings-menu overlay app (the f4hwn 6.3 approach).** It's a lot of
  plumbing for a few KB. The app API can only reach the app's own config bytes and
  FM state, with nothing for the CW block, so it would need new API surface.
- **Moving the QSO code into an overlay app.** It runs from the 10 ms tick and talks
  to the keyer and synth all the time, so the call boundary would be everywhere.
- **Scrubbing `CPO_Qso_Tick10ms` for space.** A pass found nothing worth the churn.
  The code is already tight.

## LTO: needs testing before we rely on it

The CW preset fits only because 56d4553d turns on LTO for CW (`"ENABLE_LTO": true`
in `CMakePresets.json`). Every other preset still has it off. We're wary of it:

- **History:** Meng Zhuo turned LTO on by default (164a7273, 2026-01-30: "saves ~4K
  in Flash and ~300 Bytes in RAM"). Armel set it back to off the same day
  (3f3dab78, "Fix ENABLE_LTO to false by default"), with no reason given. We don't
  know what he ran into, so we should ask him, or test for it ourselves.
- **Multiboot already needs special handling:**
  - `mb_flash.c` is built with `-fno-lto` (added in 56d4553d), because
    `cmake/check_mb_ramfunc.cmake` checks that object's RAM stub as the overlay
    safety gate. The gate still passes ("Multiboot RAM stub isolation OK").
  - Anything else that is reached only by address, a linker-script section or the
    overlay app API (the sys table in `app_overlay.c`, `.bss.mb_workspace`, the
    workspace pinned at 0x20000280) is the kind of thing LTO can move, merge or
    drop.
- **Warnings to fix if we keep LTO:**
  - `_ebss` is declared `uint32_t` in `stack_usage.c` (from the 6.2 merge,
    68d68234) but `uint8_t` in `ui/welcome.c` and `apps/app_overlay.c`. Only LTO
    sees all three together, so only CW warns (`-Wlto-type-mismatch`). It's
    harmless, since only the address is used, but should become one shared
    declaration.
  - The assembler warns "setting incorrect section attributes for
    `.data.cwsynth_held`" on LTO's output. That's because the synth's inner loop,
    `Held()`, is code placed in a `.data` section so startup copies it to RAM. The
    map shows it does land in RAM (64 B at 0x20000044), so it works, but a
    dedicated RAM-code section (as `.MBRamFunc` is) would silence the warning.
- **Testing to do on a CW/LTO build:**
  - Boot.
  - Launch and exit every overlay app; check SysInfo's RAM numbers.
  - Multiboot: switch images and run restore.
  - Save and reload settings; CHIRP read and write.
  - Stack-usage readout.
  - Code practice and the QSO drill: synth, DAC and noise.
- **If LTO turns out to be unsafe,** CW needs about 1.9 KB of flash from somewhere
  else: menu Option B, the overlay text assets, or shipping the QSO-without-SYNTH
  tier in CW.

## Suggested order

0. LTO testing above, before building more on top of it.
1. Serial shutdown during practice, plus the flash write guard (small; worth having
   on its own).
2. The sector cache borrow (#1).
3. Menu Option B (or A, if B's round trip proves awkward).
4. The QSO-without-SYNTH tier (#3).
5. Overlay assets (#2), only if flash is tight.

## Questions for Andrew

- Is any QSO or synth state read outside practice (status bar, IRQ after stop,
  persisted between sessions)? Anything that is has to stay outside the borrowed
  workspace.
- How much does live preview matter beyond `NLevel`? That decides between menu
  options A and B.
- For QSO without the synth: when you key over the station, should its playback
  stop (a break-in) or wait for you to finish? And what pitch offset sounds most
  like "another op" without leaving the tone generator's clean range?

## Other feedback

- First off, this whole simulator and the DAC-generated callers thing is fantastic.
  I do want to get this feature in a near-term release.
- The UX still has some rough corners. Button legend really doesn't belong in a menu;
  lots of overlay apps recently show little button legends at the bottom half of the
  screen and that might be the way to go with this? I'm not sure. Following the new
  menu category style of putting the category label up in the top left corner will
  make it seem more unified, which would happen automatically if we move the entire
  menu into the main menu system instead of recreating it anyway. 
- If flashlight is a menu setting, then backlight-held-on should be one too, and
  similarly a saved/restored setting. Neither would need button shortcuts. I might
  be arriving at the thinking that there's no need for a button legend if everything
  aside from WPM is in the menu anyway. Or it's a very abbreveiated legend at the
  bottom of the screen at all times.