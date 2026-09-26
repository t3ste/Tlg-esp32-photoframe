# Alarm Clock - user guide

The frame can act as a bedside alarm clock: at the set time it rings a melody
on its speaker. You stop it with the KEY button, with a spoken **stop word**, or
from the Web UI.

## What you need

| Feature | Needs |
| --- | --- |
| Alarm (schedule, ringing, KEY, button setting, Web UI tab) | the **Alarm Clock firmware** on a board with a speaker (today: `waveshare_photopainter_73`) |
| Stop by voice, microphone tools | the Alarm Clock firmware on a board with a speaker **and** a microphone (today: `waveshare_photopainter_73`) |

Regular firmware builds contain none of this. To get the Alarm Clock firmware,
choose the **Alarm Clock** option in the web flasher, or switch in the Web UI's
Updates tab ("Alarm Clock firmware"). The **Alarm Clock** tab only appears when
the frame runs it.

## How it works

- The alarm is **armed** when it has a schedule (one or more rules), and off when
  it has none.
- At the alarm time the frame wakes from deep sleep, **without WiFi, picture
  change or Agenda render**, and only rings. If the frame is always on (USB
  power or Deep Sleep off), it rings the same way.
- Ringing = a repeating four-note melody (G-C-E-C, 1.2 s) followed by a 5 s
  pause. It rings until the **ring duration** is over (default 60 s, up to
  600 s; a started melody is always finished) or until you stop it.
- Sound: the alarm has its **own volume**, an optional **volume ramp-up** and a choice of
  **six melodies** (Alarm Clock tab, "Sound"). The Chimes volume and the Chimes **quiet hours**
  never apply to the alarm - it always rings.

## Stopping a ringing alarm

| How | Details |
| --- | --- |
| **KEY, short press** | Stops the alarm. That press does not change the picture. Always works. |
| **Stop word** | Say your taught word during a pause between the melodies (see below). |
| **Web UI** | "Stop" button under "Try the alarm" (for a test ring started there). |
| **Time** | The alarm ends by itself after the ring duration. |

## Setting the alarm with the buttons

The frame's two buttons can set one alarm time without any app. This alarm rings
**Monday to Friday**; other days need the Web UI or Telegram.

1. **Hold KEY for 3 s.** A short beep sounds while you still hold it (release
   now), then two quick beeps confirm you are in setting mode. The time starts
   at 00:00.
2. **BOOT, short press: hour +1** (0-23, wraps around). The frame beeps the hour:
   - hour 0 (midnight): one long low tone;
   - hours 1-11: that many beeps, high pitch (1 = 1 am);
   - hours 12-23: 12 = twelve beeps, 13 = 1 beep ... 23 = 11 beeps, lower pitch.
3. **KEY, short press: minutes +10** (:00, :10 ... :50, wraps around). Beeps:
   :00 = one long high tone, otherwise one beep per 10 minutes (3 beeps = :30).
4. **Hold KEY for 3 s again to confirm.** A low-short / high-long tone means
   "armed"; the frame then goes back to sleep.
5. **Do nothing for 10 s** and the edit is thrown away **and the alarm is
   disarmed** (high-short / low-long tone).

Outside the setting mode the buttons keep their normal jobs (KEY short: next
picture; BOOT long: hotspot). A press of KEY during a ring only stops the ring.

Telegram alternative: `/alarm_cron 0 7 1-5` (minute hour weekdays) and `/alarm_off`.

## Stop by voice

The frame learns a short word from you (for example "Stop" or "Ruhe"). While the
alarm rings it listens **only in the pauses between the melodies** (the speaker
would drown your voice otherwise) and stops when it hears the word.

- It recognises **your** voice and pronunciation, not just the word. Teach it
  yourself, in the room where you will use it, quiet and close to the frame.
- Nothing is recorded or sent anywhere. Only a small pattern of the word's
  sound is stored on the frame; no audio.
- Use one short word (under 1.5 s). Words that sound alike are rejected, but
  choose something distinctive rather than a common word.
- If no word is taught, the switch is off, or the microphone can't be opened,
  the alarm simply rings as usual. KEY always works.
- Recognition is not perfect: if the frame misses the word, say it again in the
  next pause (they come every ~6 s).

### Configuring it (Web UI)

Open the **Alarm Clock** tab, card **Stop by voice**:

1. **Teach n/5** - press it, then say the word within 3 s. "Word added" confirms
   it. Repeat up to 5 times; **about 4 examples, spoken slightly differently
   (a bit faster / slower), give the best result.** If nothing was heard or the
   word was too long, you get a hint and can try again. If it says the word "does not sound like the earlier examples", you said
   something else or in a very different way - repeat it as before, or press
   **Forget** to start over with a new word.
2. **Test** - listens for 10 s and reports how many utterances it heard and how
   often it recognised the word. Say the word and a few other words: only the
   word should count.
3. **Stop the ringing alarm with the stop word** - the switch (enabled after the
   first example).
4. **Detection threshold** - how close a spoken word must be to a taught example to count
   (a distance: smaller = stricter). **Automatic** derives it from how much your examples
   differ (at least 4 - often too strict with only one or two examples of a real voice).
   Switch Automatic off to set it with the slider. Workflow: run **Test**, say your word and
   read its *best distance*; put the threshold a little above that value. If other words
   start to be accepted, lower it again or teach more examples (more examples lower the
   distance of your own word).
5. **Forget** - deletes all taught examples (start again after changing your
   word, or if it works badly).

## Web UI - Alarm Clock tab at a glance

| Control | What it does |
| --- | --- |
| Alarm armed / disarmed | Off clears the schedule; on needs a schedule rule. |
| Schedule | Times and weekdays of the alarm (several rules possible). |
| Ring duration | How long it rings if not stopped (1-600 s). |
| Alarm tone | The melody: G4-C5-E5-C5 (default), C5-E5-G5-E5 bright and friendly, A4-C5-E5-C5 soft and pleasant, G4-D5-B4-D5 clear and attention-grabbing, F4-A4-C5-A4 warm and calm, C5-G4-E5-C5 distinctive and a little more dynamic. |
| Volume | The alarm's own volume, 10-100 % (not the Chimes volume). The speaker is quiet below about 40 % and distorts above about 90 %. |
| Volume ramp-up | Seconds until the full volume is reached: 0 = off (full volume at once), up to 120 s. It starts at about 8 % of the volume and rises evenly; 20-60 s is usually enough. Keep the ring duration longer than the ramp. |
| Stop by voice | Switch, **Teach**, **Test**, **Forget** (above). Voice boards only. |
| Try the alarm | **Ring now** rings with the current settings (incl. the stop word), **Stop** ends it, and the last ring's ending is shown (time / KEY / voice / Web UI). |
| Microphone (voice boards) | **Live level**: bars for both microphones with the noise floor (blue) and the threshold (red) - the frame stays awake while it is on. Threshold: automatic (noise floor + 20 dB) or a fixed slider. **Log level**: writes the level to the debug log for 15 s. **Self-test**: plays tones and checks that the microphone hears them. |

Schedule, armed switch, ring duration, tone, volume and ramp-up are saved with the tab's
normal **Save** button (then **Ring now** plays the new sound); the voice controls act
immediately.

## Troubleshooting

- **No Alarm Clock tab** - the frame runs the regular firmware; install the Alarm Clock variant.
- **No "Stop by voice" card** - the board has no microphone.
- **The word is not recognised** - Forget and teach again (4 examples, near the
  frame), check with Test, look at the noise with Live level; a very loud room
  or a very quiet, mumbled word is hard.
- **The alarm did not ring** - it needs a schedule (Armed), a speaker, and the
  frame must have the correct time (set by WiFi/NTP at least once).
- **The alarm rang and the picture did not change** - by design.
- **A running microphone test or Teach stops** when an alarm starts: the ringing alarm always has priority.
