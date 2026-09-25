# Test plan v218.6.0 (Alarm Clock + voice stop)

Needs the **Alarm Clock firmware** on the PhotoPainter (COM5/COM6 already have it).

## Voice stop (real voice - not yet tested)
- [ ] COM5: Alarm tab → **Forget** (old synthetic examples), then **Teach** 4x with your own voice
- [ ] **Test** (10 s): your word counts, other words / talking do not
- [ ] Switch **Stop the ringing alarm with the stop word** on → **Ring now** → say the word in a pause: stops?
- [ ] Distance: 0.5 m / 2 m / 4 m from the frame; volume 60 % / 100 %
- [ ] Reliability: 10 tries, count hits; note the misses (fast? mumbled? morning voice?)
- [ ] False stops: 5 min ring with radio / TV speech in the room → must not stop
- [ ] Someone else says the word → should not stop (speaker-dependent, expected)
- [ ] Teach with a word that is too different → hint "does not sound like the earlier examples"

## Alarm basics
- [ ] Scheduled alarm, **USB powered** (always-on): rings, ends after the ring duration
- [ ] Scheduled alarm, **battery + deep sleep**: wakes, rings, goes back to sleep, next alarm still set
- [ ] **KEY short press** stops it, picture does not change, no menu opens
- [ ] Ring duration 10 s ends after a full melody
- [ ] **Live level** on in the Web UI when the alarm fires → alarm wins, meter stops
- [ ] Alarm tab: leaving it stops the live meter; polling only while the tab is open

## Button setting (setalarm.log from earlier)
- [ ] Hold KEY 3 s → beep at 3 s, two beeps in the mode; BOOT = hour, KEY = 10 min; hold KEY 3 s → armed tone
- [ ] Do nothing 10 s → disarmed tone, schedule cleared
- [ ] Web UI shows the button-set alarm as "Schedule 1"; `/alarm_off` (Telegram) disarms

## Builds / release
- [ ] Regular firmware: no Alarm tab, no voice endpoints (404)
- [ ] OTA: switch regular ↔ Alarm Clock variant; "Include pre-releases" finds v218.6.0
- [ ] Web flasher: Pre-release + Alarm Clock option installs the new firmware
- [ ] Other boards build in CI (9 jobs green)
