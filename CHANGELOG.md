# Sila changelog

Flash `Sila.bin` (from the Releases page) at address **0x0** with https://espressif.github.io/esptool-js/
Since v3.3 your pet survives this: it keeps a backup outside the firmware area.
(Don't tick "erase flash" - that would wipe the backup too.)

## v3.7 (2026-11-02) - more alive: calling, poking, falling, eating power, a game
Tested with Mohanad on the real stick while logging it over USB.
- Double clap = calling it: "you called?", it looks at you; wakes it up happily
  if asleep and turns the screen on. 3+ claps are still a rhythm (ignored).
- Poke its side: 1-3 pokes it giggles, 4-5 "hey!", 6+ within 8 s it gets grumpy
  ("stop poking!", -1 happy). The accelerometer misses short taps, so a tap the
  mic hears while the stick wobbles counts too. Pokes are no longer taken for claps.
- Free fall: screams "AAAAH!" while falling. Caught softly (hand, pillow):
  "wheee!" (+2 happy). Landing harder than 3.5 g: "ow!", dizzy, -2 happy, -1 health.
- Eats electricity: plugging in makes sparks and chewing, "yum! power!" (+5 food),
  a bolt shows while charging and it doesn't get hungry while plugged in. Low
  battery (<15%) makes it tired: "low battery...", then "charge me!".
- New PLAY card in the menu: "Catch" - tilt to move Sila, catch WiFi snacks (+1)
  and hearts (+3), dodge bugs. 3 lives, speeds up. Rewards happiness and food;
  best score is saved (also in the flash-proof backup).
- Removed shake-to-play (shaking for hearts didn't feel right). Shake still
  hatches the egg and wakes the screen.
- USB serial logs IMU-EV lines (pokes, falls, landings).

## v3.6.2 (2026-11-02) - ears tuned live on the real stick
Tuned with the stick plugged into the laptop: the laptop talked and played
drum beats while Sila's ears were logged over USB, then Mohanad clapped,
talked, played a song and dropped a book.
- Music: drum-heavy songs never made it dance (the gaps between hits looked
  like speech pauses). A very steady beat now counts as music anyway. It also
  only dances while the music is actually playing (it used to keep dancing ~5 s
  after the song ended).
- Claps: a clap must stand alone - it is held ~0.6 s and dropped if a rhythm
  follows. Drum hits at the start of a song no longer count as claps (was 9-25
  false claps per song, now 0). Single and double claps are both claps (it can
  tell which).
- Bangs: real claps clip the mic at 28-38 dB, so they were taken for bangs
  (yelp). Now a bang is a loud hit that *rings* (dropped book), or a short hit
  over 40 dB. Shock-waking from sleep needs 38 dB.
- USB serial also logs the ears (EAR lines) and every clap/bang decision.

## v3.6.1 (2026-11-02) - the real charging fix
- Found by reading the stick's power chip live over USB: the 5V boost (for the
  Grove port) was switched on by default. On the StickS3 it shares the USB 5V
  line, so the battery was feeding the charger - the charge light flickered on
  and off every second, the battery stayed stuck at ~3.65 V while "charging",
  and it drained even when idle. Boost is now off (Sila doesn't use Grove).
  Measured right after: the charger stays on steadily and the battery climbs.
- "Full / 100%" now needs the charger to say done AND a full battery voltage
  (4.1 V+). A flickering charge pin can't fake 100% anymore.
- Charging voltage lift measured at ~25 mV (v3.6 guessed 120 mV, which made
  the % read too low while charging).
- USB serial (115200) prints one power line per second for diagnosis.

## v3.6 (2026-11-02) - black screen and battery fixes
- Black screen after a hard shake or turning it to portrait: the face is drawn
  on its own picture buffer, and that buffer was thrown away and re-made every
  time the face turned between landscape and portrait. Once WiFi/Bluetooth had
  chopped up the memory, re-making it failed and the face stayed black until a
  restart. Now both buffers (landscape + portrait) are made once at start-up
  and never thrown away.
- Battery life: the WiFi radio was left switched on between scans, listening
  all the time - most of the battery. It's now only on during a scan.
  Bluetooth is fully switched off between its radar looks too. This also
  leaves more of the charger's power for the battery, so it can finish charging.
- Battery %: it used a straight line from 3.3 V to 4.15 V, which is wrong for
  this kind of battery (39% shown when it was nearly empty). Now uses a real
  battery curve, averages the voltage over ~30 s (WiFi scans no longer knock
  1% off each time), and subtracts the charger's voltage boost while charging,
  so unplugging no longer makes it drop suddenly. When the charger says done, it shows 100%.
- System page Batt voltage is the averaged value.

## v3.5 (2026-10-31) - claps and music, take three
- Claps: the mic clips at ~90 dB, so a clap's *average* loudness only reached
  ~76 dB and was never "loud enough". Claps are now found by the sudden jump of
  the *peak* sample. A clap gives a quick "!?" and then it looks at you; a very
  loud bang also makes it yelp.
- Music: the old beat finder listened only to deep bass, which laptop and phone
  speakers barely play. It now listens for every "new sound starts" pulse
  (drums, hi-hats, chords), checks they repeat at a steady tempo over 6 s,
  and that there are no talking-style pauses. Simulated: 41 of 45 songs
  (85-140 bpm, laptop/phone/Bluetooth speaker, 60-70 dB) dance; quiet room,
  talking and a fan never do.
- System page, Ears row: now shows `level  room  beat %`. While music plays,
  beat % is how sure it is there's a beat (dances from 24%).

## v3.4 (2026-10-31) - ears rebuilt, shock-wake fix
- Ears: the mic picks up a lot of low rumble, which looked like a never-ending
  bass beat (dancing + listening all the time, even asleep). Now:
  - rumble/hum is filtered out first
  - "quiet" = the room's usual level, not its quietest moment
  - dancing needs music clearly louder than the room and 7+ evenly spaced kicks
  - listening needs speech-like sound (loud and constantly changing), so a fan doesn't count
  - claps must jump up in one instant; talking can't fake a clap anymore
  - no "dancing!"/"listening..." badges while asleep
- Shock wake: the screen now glides up and the yelp comes 0.3 s later, and the
  Bluetooth radar waits 30 s after any wake-up. Before, backlight + speaker +
  radio all switched on in the same instant.
- Safety net: if it ever freezes for 10 s it reboots by itself (pet is saved),
  and if it restarted because of a crash/freeze/low power, the System page
  shows it in amber next to the version.

## v3.3 (2026-10-31) - fixes
- Updates are one file again: Sila.bin at 0x0. The pet now keeps a backup copy
  in a separate flash area that firmware files never reach, and restores itself
  after a flash. "New Egg" still really resets it.
  (The v3.2 update file was fine - the flasher's address box defaults to 0x1000,
  which overwrites the bootloader: black screen + USB reconnect loop.)
- Ears retuned to your room (quiet 50-55 dB, talking 65-75 dB):
  - claps must be a big (+26 dB) and *short* sound - no more triggering on
    the start of a sentence
  - a bang must come out of quiet and be short; turning music up no longer
    counts as a bang
  - music: beats are now detected from the bass (kick drum), with a forgiving
    tempo check; claps are ignored while music plays
  - ears ignore button clicks, plugging in the charger and the mic's own
    start-up "pop" (that was why pressing A or charging made it mad)
- Sleeping: sounds only make it stir. Only a really loud bang (or a hard jolt)
  wakes it in shock - touching the button no longer does.
- Rocked to sleep: no more eyes re-opening in blue before closing. Eyes go
  straight from heavy to closed, and only turn sleepy-blue once mostly closed.

## v3.2 (2026-10-31) - senses
- Save-safe updates: firmware now comes as Sila-update.bin (flash at 0x10000).
  The old single file at 0x0 overwrote the save area, which is why the pet
  reset after every update.
- Ears (microphone): talk to it and it listens (teal sound waves - your voice),
  clap or knock and it looks at you, play music with a beat and it dances,
  a loud bang startles it. Only loudness and rhythm are measured on the stick;
  nothing is recorded. Settings > Ears (mic).
- Rock it side to side like a baby: eyelids get heavy and it falls asleep in
  your arms. Small bumps make it stir ("mm..."); a hard jolt or a loud bang
  wakes it in shock ("!?"). Lay it down while it sleeps and it stays in bed.
  A button press wakes it gently; on its own it naps about 15 minutes.
- Walks: step counter. "walkies!" while you walk, +1 happiness and +1 health
  every 100 steps, a celebration at the 3000-step daily goal. New WALKS card
  and page (today, best day, total). "Today" is a pet-day (24 h of its life).
- People radar: a 2 s passive Bluetooth listen every 3 minutes counts devices
  nearby (phones, earbuds). "4 people near!" when company arrives. Skips scans
  while asleep, hot or on low battery. Settings > People Radar.
- WiFi page is now RADAR (WiFi + people). Settings list scrolls (9 items).
  System page shows live ear level and radar results.

## v3.1 (2026-10-31) - Sila
- New name: Sila (صلة, "connection / bond"), made by Mohanad.
  Wordmark: "si" in teal (you) + "la" in terracotta (the pet). It fades in under
  the eyes on boot, sits on the menu and signs the System page.
  Your pet carries over - its save data is untouched.
- Battery: no more charging / not charging flicker. USB voltage decides
  "plugged in", the charger's status pin is smoothed over 10 s, and there are
  3 clear states: battery / charging (animated teal) / full (green).
  The % no longer jumps around: it only goes down on battery, up when charging.
- Heat: the screen used to redraw as fast as possible with the CPU at 100%.
  Now 30 fps (slower when dimmed), CPU at 160 MHz instead of 240.
  The chip temperature is on the System page; if it gets hot (>72 C) Sila
  slows down its drawing and WiFi scans until it cools, and shows a sweat drop.

## v3.0 (2026-10-31) - alive
- Floating face: it stays level with the real world at ANY angle (not just
  90 degree steps) through a soft spring that overshoots a little, like
  something floating in a bottle. Moving the stick pushes it around and it
  settles; the eyes lag behind your movement. It also breathes gently.
- Unique face per life stage: Baby (big shiny pupils, rosy cheeks), Teen
  (classic), Adult (wider, sharper, thicker brows), Elder (droopy eyes, bushy
  brows, round glasses). Evolving shows a sparkle celebration + "I'm TEEN now!".
- Idle quirks: every so often it winks, yawns, or looks right at you.
- Colour language: teal = you, terracotta = the pet's
  voice. Every section and stat has its own meaningful colour.
- New menu: a colourful card carousel with icons; each card is its section's
  colour. All pages recoloured to match. System page is signed by both of us.
- First boot: just the egg - press A or shake it to hatch. Eyes pop out.
- Every other boot: black screen, then the eyes pop in with a bounce, blink and
  look around. Fixed the small glitch when it switched to the home screen.

## v2.2.1 (2026-10-31)
- Fixed: screen stuck sideways (portrait) in v2.2. The panel was never set to
  landscape at startup, so menus were squeezed and the face was turned.

## v2.2 (2026-10-31)
- Smooth rotation: the face now spins smoothly to its new orientation
  instead of jumping, and leans with a real tilt.
- Spin it around a lot (4 turns in 10 s) and it gets dizzy: spiral eyes,
  a small hit to happiness and health, then it feels sick for ~45 s.
- Lie-down sleep fixed and stricter:
  - only sleeps when really flat (within ~10 deg of level) for 4 s, so it
    stays awake when propped at an angle
  - wakes when picked up: tilted past ~20 deg or bumped
  - no more naps on its own - it only sleeps when you lay it down
- Screen stays on by default. It only dims while the pet sleeps in bed.
  Settings > Idle Screen: Always on / Dim 1min / Off 1min.
- New boot: the very first boot hatches the egg by itself. Every boot after
  that the pet wakes up - eyes peek, close, yawn, open wide, look around.
- Charging noise: the audio codec chip is now powered down too, not just the
  amp. System page shows "Amp: on/off" so you can check it.
- Classic golem view removed (and its images: firmware is 35% smaller).
- Hold A on the home screen now pats the pet (+happiness, hearts).

## v2.1 (2026-10-31)
- Fixed: buzzing/hiss from the speaker while charging. The speaker amp was
  left on all the time and amplified charger noise. It is now only powered
  while a sound plays (firmware issue, not hardware).
- Face auto-rotates with the stick, including portrait when it hangs from
  your keys, and leans a little when you tilt it. Controls > Auto Rotate.
  Menus stay landscape ("Flip Screen" renamed to "Flip Menus").
- New hunting look: no more red lines. Sonar rings pulse out behind the
  face, the eyes narrow and dark pupils sweep side to side.
- No WiFi around now makes the pet BORED, not sick. It only gets sick from
  low health, or after 3 hours without any WiFi.
- Settings > Scan Rate: Normal (about 1 scan/min) or Saver (about 1 every
  1.5-2 min) for better battery.
- Sleeping in bed (laid down) heals faster: +1 health every minute.
- System page shows the tilt sensor reading (for checking rotation).

## v2.0 (2026-10-30) - new look
- Whole UI redesigned in one theme inspired by the new CiferTech TamaFi:
  black + teal frames, coral labels, pixel font, segmented bars.
  Boot, egg/hatching, menu, every page and game over are all new.
  See screens_preview.png.
- New face style: wide white eyes with a soft halo, eyebrows, little mouth.
  New expressions: pink "target lock" eyes + scanlines while hunting WiFi,
  sly tilted look + lock icon when an open network is nearby, alert look +
  orange WiFi icon when there are lots of networks, X X eyes on game over.
  A badge under the face shows "hunting...", "9 networks", "hungry", etc.
- Home screen is just the face now (no stat lines). Stats moved to a new
  VITALS page (B from home) with a bar and a plain-English hint for each.
- Removed the coloured border that flashed during WiFi scans.
- Screen now DIMS instead of going black, and keeps animating, so you can
  watch it sleep. Settings > Idle Screen: Dim / Off / Always on.
- Lie-down sleep: lay the stick flat and still for 4 s and the pet goes to
  bed. It sleeps until you pick it up. While in bed it heals, gets hungry
  3x slower and doesn't scan WiFi (saves battery). Settings > Lie-down Sleep.
- Menu reorganised: Vitals, Pet Info, WiFi Radar, Controls, Settings, System.
- Removed the Mood Glow option (no more border).

## v1.4 (2026-10-30)
- NEW: Robot-eyes face on the home screen (now the default view).
  Big cyan eyes + mouth that react to the pet: happy squint, droopy when hungry,
  narrowed and scanning while hunting WiFi, one big eye when curious, half-closed
  when bored, green and shaky when sick, closing to lines with zZ when sleeping,
  chewing after a meal, hearts when you shake it. Blinks and looks around.
  Thin hunger/happy/health bars along the bottom. Battery icon appears only
  when low or charging. See face_preview.png.
- Hold A on the home screen to switch between eyes and golem.
  Also in Menu > Controls > Home View.
- Home screen now opens the menu when A is released (so holding A can switch views).

## v1.3 (2026-10-30)
- Hunger rebalanced for carrying all day:
  - Stats drop slower: hunger and happiness -1 every 3 min, health -1 every 5 min.
  - Exploring no longer costs food (it cost 5 each time and was the main drain).
  - Every successful WiFi hunt feeds at least 10.
  - A hungry pet (under 50) now eats before exploring.
  - A hunt that finds no WiFi no longer costs food.
- Fixed an upstream balance bug: in busy WiFi areas the pet explored forever,
  never ate or slept, and slowly died. Explore urge is now capped, and a weak
  pet (health under 50) prefers to nap.

## v1.2 (2026-10-30)
- Quieter: new Sound setting (Menu > Controls > Sound). Default "Key" only beeps
  for your button presses, hatching, evolving and play. "All" = old behaviour.
  Autonomous sounds never play while the screen is asleep.
- Pet thinks less often (every 20-45 s instead of 8-15 s). Fewer WiFi scans,
  better battery. Hunger/happiness/health drop 2.5x slower to match.
- Easier, smarter shake: three quick flicks of the wrist instead of one hard
  jolt. Single bumps from walking no longer count.
- Evolving now plays its own fanfare.

## v1.1 (2026-10-30)
- Fixed: screen went black after using the B button and never woke up again
  (auto-sleep timer bug).

## v1.0 (2026-10-30)
- First port of TamaFi to the M5StickS3.
