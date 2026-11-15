# KVision to-do list

Written 10 Nov 2026. Items 1 to 5 come from reading the Node-RED project
[Supergiovane/node-red-contrib-hikvision-ultimate](https://github.com/Supergiovane/node-red-contrib-hikvision-ultimate)
(MIT licence), which also reads Hikvision event streams. We take ideas from it, not code. Its README, changelog and
a few source files were read; its whole code base was not reviewed.

Status key: `[ ]` not started, `[~]` started, `[x]` done.

## From the Node-RED project

### 1. [ ] Drop stale reports from the screen
- **Why:** after a stall or a reconnect a camera can send a backlog. Today a late report still flashes a frame, and only
  the log line is marked `received HH:mm:ss`. On 10 Nov a burst of 68 log lines landed in one second.
- **Idea:** do not show a frame for a report whose camera time is more than about 10 seconds old. Still write it to the
  log. Their changelog (version 1.4.0) filters backlog events by `dateTime` and caps each batch.
- **Where:** `src/hikvisionevents.cpp`, in the same place the "late" test for the log already lives (`logDetection`).
- **Done when:** a replayed old report does not show a frame, and the log still has it.

### 2. [ ] Optional merge of duplicate reports
- **Why:** the same event can arrive twice. Garage3 sent two line crossings in the same second and we could not tell a
  real second crossing from a repeat. Their changelog mentions duplicates from the same event arriving twice.
- **Idea:** a short window (for example 1 second) in which an identical report for the same camera, kind and target is
  merged. Make it a setting and leave it off by default, because every real crossing matters for the interval study.
- **Done when:** the setting exists and, switched on, two identical reports in the window give one frame and one line.

### 3. [ ] "Detection offline" mark on a tile
- **Why:** a quiet camera and a dead connection look the same on screen today.
- **Idea:** their Device Status node reads `GET /ISAPI/System/deviceInfo` every 30 seconds, marks a device offline after 2
  failed checks, and backs off for 30 seconds after HTTP 401, 403, 429 or 503. Do the same for the connection a tile
  uses, with a small mark on the tile. Keep our rule never to send a login we know is wrong (a wrong password can lock a
  camera).
- **Where:** `src/hikvisionevents.cpp` (the connection already counts failures and has `listenFailed`) and `src/Player.qml`.
- **Done when:** unplugging a camera shows the mark within about a minute, and plugging it back in clears it.

### 4. [x] "Check detection setup" for a camera (done, not yet committed; fixes are the next step)
- **Why:** on 10 Nov nearly every "no events" problem was a setting in the camera or recorder: metadata off, a rule
  switched off, no Notify Surveillance Center, no zone drawn, a switch that did not save. Their README says the same: "For NVR/DVR,
  remember to select Notify Alarm Center... otherwise the NVR won't emit any alarm event."
- **Idea:** a right-click item that reads (read-only) and lists what is missing for that camera. The reads are listed in
  `docs/AUDIT-2026-11-10.md`, section "Reads that answer 'why no events'".
- **Never changes anything.** It only reports.
- **Done when:** run on Garage3 as it was before the fixes, it names metadata, the rule switches and Notify Surveillance Center.

### 5. [x] Keep the picture that comes with an event (done, not yet committed)
- **Done so far:** the setup check reports whether event pictures carry the rule and target frames (tested on Garage3's
  real pictures: frame on = outline found on the reported rectangle, frame off = none). Pictures are not kept yet.
- **Not possible:** reading or setting the camera's "Overlay Rule Frame and Target Frame" switch over ISAPI. It is not
  in any of about 70 settings paths tried; it is only in the camera's web page.
- **Why:** both the recorder's SDK alarm and the camera's own stream attach a snapshot with the box drawn on it (about
  250 KB). Their node passes it on (`picName`, `picUrl`, the image output).
- **Idea:** keep the latest picture per camera, show it in the detection log window next to the entry, and store a
  small copy beside the log. Needs a size limit and a clean-up so the folder cannot grow without end.
- **Where:** `src/hikvisionevents.cpp` (the stream parser currently throws the picture parts away),
  `src/DetectionLogWindow.qml`.
- **Done when:** a log entry shows the picture from that moment.

## Left over from 10 Nov 2026 (see the audit trail)

- [~] **Release the source and event-type filters.** Committed (`99bc238`), with README and changelog entries.
      Only tested against a fake recorder and by checking numbers. Try it on the real NVR with a screen, then bump
      the version and make a release.
- [ ] **Find out why the "NVR cameras switched on" list emptied itself** (`targetNvrOnCameras=[]` appeared in the
      saved configuration). Best guess: one of the offscreen test runs of the app saved an empty list. Not proven.
      It was restored by hand to 14 cameras. If it empties again, look at how the settings are written on exit.
- [ ] **Decide what to do with the temporary Notify Surveillance Center on Garage3 (NVR channel 17).** The backups hold the
      "record only" originals. See the audit trail for how to put them back.
- [ ] **Livingroom2 (NVR channel 18)** has no smart detection set up and is a different model. Not touched.
- [ ] **Doorbell (192.168.20.185)** did not answer the device-type questions, so how KVision classifies it is untested.
- [ ] **Dual-lens camera (Cabin PTZ, two lenses on one connection):** whether each lens reports with its own channel
      number was never watched. Test it.
- [ ] **A DVR (analogue recorder)** was never tested; none is available.
- [ ] **Check the Target Validity setting on Garage3.** It was changed to High in the camera's page, but a read-back
      of the line rule showed `mediumLow`. Unconfirmed.
- [ ] **Interval study:** the question "does the camera pause between reports" is not settled. Test with a counted
      series of crossings through both the camera and the recorder at once. See the audit trail, "Open questions".
- [ ] **Tidy up** `/tmp/evprobe`, `/tmp/vcalive`, `/tmp/nvrlive`, `/tmp/g3live`: probe programs and logs from 10 Nov.
      They disappear when the machine restarts. Copy anything worth keeping first.

## Added 12 Nov 2026 (see section 10 of the audit trail)

- [ ] **Camera settings window: test it fully** (layout, week strips, the all-cameras table and both copy buttons). The
      reading code was tested from the command line only.
- [ ] **Linked events: say "could not be read" when the recorder does not answer.** One run showed "nothing on the
      recorder is linked" for the Front Door's audio because the recorder dropped the answer.
- [ ] **Image Parameters Switch:** the scene that is selected on a "Not Switch" camera is not in
      `/ISAPI/Image/channels/1/displayParamSwitch`; where it lives is unknown. Cabin Inside, DoggieCam and the Doorbell
      do not offer the request at all.
- [ ] **Restore:** only tested on DoggieCam, and only the device name was checked afterwards. Try one more camera and
      compare more settings (before a restore, take the safety backup it makes, and keep it).
- [ ] **Face detection on Livingroom2 / Garage2 (192.0.0.108):** enabled on the camera, but no face event appeared on its
      stream or the NVR's in two live tests. `OtherEvents.log` would show one if it ever came. Unknown whether this model
      sends one.
- [ ] **Audio at the Front Door does not sound by day.** The likely cause is the camera's own audible alarm hours
      (00:00-05:45, 21:00-24:00). Not tested by triggering the event by day and by night. The other cameras with limited
      hours are in the all-cameras table (orange).
- [ ] **Four NVR rules that flash lights are switched off:** Pano region entrance, FrontDoor line crossing, PTZ intrusion,
      Driveway line crossing. Decide whether they should be on.
- [ ] **Watch `logs/OtherEvents.log` for a while** to see which event types the cameras send that KVision does not know.
      `IO` and `duration` are skipped on purpose. Tamper types were left out on purpose.
- [ ] **Old camera quirk:** Cabin Inside (192.0.0.11, firmware V5.4.800) returns a configuration export even without a key,
      so its file may not be encrypted. Treat it as sensitive.

## To investigate: camera controls found in the Hik-Connect app (14 Nov 2026)

Found by reading the Hik-Connect 5.4.0 and 7.0.220 apps (files only, nothing installed) and by asking the BackYard Left
camera (192.0.0.60, DS-2CD2347G2-LSU/SL, firmware V5.7.23) what it supports. All four are on-demand controls KVision does
not have yet. Decide where each goes (right-click Quick tools menu, Camera Tools tab, or both) and show it only on cameras
that report the feature.

### 6. [x] Flashing light (built-in strobe) - built, in the Quick tools menu
- **Built:** Quick tools > "Activate Flashing Light". It sends `PUT /ISAPI/Event/triggers/notifications/whiteLightAlarm/trigger?format=json`
  with `{"WhiteLightAlarmTrigger":{"triggerType":"start"}}`, and the camera runs its own programmed cycle (duration 1 to 60 s,
  set on the camera). There is no stop item on purpose. Shown only on cameras whose `.../whiteLightAlarm/capabilities?format=json`
  answers with `WhiteLightAlarmCap`. Tested working.
- **Does not flash on a G3 (DS-2CD2387G3-LIS2UY/SL, firmware V5.8.40, 192.168.20.137):** the camera answers OK (`statusCode 1`)
  to the trigger, with and without the app's extra `whiteLightTriggerSchedule:false` flag, but the light does not flash
  (watched three times). The Hik-Connect app does not make it flash either. It flashes on the 192.0.0.x cameras. The web page
  has no manual strobe test (its scripts only read and write the light's settings), so no other call was found. That camera's
  supplement light is in `colorVuWhiteLight` mode (limit 15); whether another mode (`eventIntelligence`) lets the trigger
  flash is not tried, because it changes the camera's night picture.
- **Conclusion: the G3 has no on-demand strobe.** Tried on 192.168.20.137, all with no flash:
  1. the trigger call, three times (the camera answers OK each time);
  2. the same with the app's extra `whiteLightTriggerSchedule:false`;
  3. the day/night idea from hikvision_bridge issue #52 (https://github.com/yannik2480/hikvision_bridge/issues/52);
  4. the alarm-input workaround from openvigil (https://github.com/sm26449/openvigil, `app/camctl.py`): add a `whiteLight`
     notification to `/ISAPI/Event/triggers/IO-1`, then set `/ISAPI/System/IO/inputs/1` `<triggering>` to `low` for 15 s
     while watching. Everything was put back and read back identical to the backup.
  The web page has no strobe test (its `ManualWhiteLight` is the steady supplement-light brightness, 0 to 100). Both GitHub
  projects say newer IPC firmware has no manual strobe; it flashes only when a linked detection fires. The 192.0.0.x
  cameras (G2) still flash from the trigger call (192.0.0.60 verified). Not tried: linking `whiteLight` to a real detection
  event such as line crossing, which would flash on a real event only.
- **Decision (user):** keep the item visible on every camera that lists the strobe and rely on the tooltip, because a future firmware may add the call.
- **For now:** the menu item stays on every camera that lists the strobe. Its notice says "command accepted", not "flashing", and its tooltip warns that some newer cameras accept the command without flashing.

### 7. [x] Warning message (siren / spoken warning) - built, in the Quick tools menu, works on the G3
- **Built:** Quick tools > "Play Warning Message". On a camera that lists `isSupportAudioTest` it reads the camera's saved sound
  (`.../AudioAlarm?format=json`: audioClass, alertAudioID or customAudioID) and sends the web page's own Test call,
  `PUT /ISAPI/Event/triggers/notifications/AudioAlarm/<id>/test?format=json` (no body). The id is the alert sound number, 12
  for a prompt sound, or 12 plus the custom number (15 when above 3). Otherwise, or if that fails, it falls back to the
  app's `PUT .../AudioAlarm/trigger?format=json` with `{"AudioAlarmTrigger":{"triggerType":"start"}}`.
- **Tested:** played on the G3 (192.168.20.137, sound 3 at volume 66, once). The app's trigger call does not play on that
  camera, which is why the Test call is used first.
- **Shown only** on cameras whose `.../AudioAlarm/capabilities?format=json` answers with `AudioAlarmCap`.
- **No stop** (the sound ends by itself after its repeat count, 1 to 50).

### 8. [x] Noise filter (audio) - built, goes in the Quick tools menu
- **Found:** `/ISAPI/System/TwoWayAudio/channels/1` has `<noisereduce>` (true or false). The web page shows it as
  Configuration > Video/Audio > Audio > "Environmental Noise Filtering". It is an on/off switch on the camera.
- **Levels:** the app's 0 to 5 levels are not on the camera. They are `setANRParam` in the phone app's own player, saved
  on the phone only. A level setting would have to be a filter inside KVision's player (not looked at yet).
- **Verified:** switching it on and off on a real camera clearly changes the live sound at high volume. A write, a read
  back and a restore on 192.0.0.60 worked and left every other field unchanged.
- **Built:** Quick tools > "Noise filter (camera sound)", a tick item that reads the camera when the menu opens, then
  flips it and reads it back. Hidden on a camera without the address (Doorbell, 192.168.20.156).

### 9. [ ] Reboot camera
- **Not yet tried.** The app has `ISAPI/System/reboot` (the newer one also takes a child-device ID for recorders).
  Normally `PUT /ISAPI/System/reboot` with no body. The camera is off for about a minute and the stream drops.
- **To decide:** a clear confirmation first, a wait-and-reconnect, and whether it can be used on the recorder itself.
  Never run it on the Driveway camera (192.168.20.22), which was locked out earlier.
- **Unverified:** the address, the empty body and the downtime come from general knowledge of Hikvision cameras, not
  from a test.

### 10. [ ] Event destination test - goes in Settings > Camera Tools, NOT BUILT (probably not useful)
- **Found:** the app has `Event/notification/httpHosts` with a `.../httpHosts/<id>/test` call.
- **Read from 192.168.20.137 and 192.0.0.60:** all three HTTP host slots are `0.0.0.0:80` and unused. KVision does not use them: it
  opens its own event stream to the camera (`alertStream`), so there is no destination to test. A test call would only matter
  if KVision set up a push address on the camera, which it does not.
- **Idea instead:** a "can this camera's event stream be opened" check already exists in the Setup check. Leave this item unless a
  push address is ever added.

### 11. [x] Camera clock and NTP - built (read only), in Settings > Camera Tools > "Camera clock and firmware"
- **Built:** Check and Check all read `GET /ISAPI/System/time` and compare `localTime` with this computer's clock (halfway through
  the request). Within 5 s it says "matches", under 2 minutes it says how many seconds off, and over 2 minutes it is flagged in
  orange. It also names the time source (NTP server, or "set by hand, so it drifts"). Nothing is written.
- **Read on 192.168.20.137 and 192.0.0.60:** both in NTP mode and both matched this computer. 192.168.20.137 uses us.pool.ntp.org
  and 192.0.0.60 uses 192.0.0.13.
- **Check all** opens a window (`src/CameraHealthWindow.qml`) with one card per camera, filled in as each camera answers, a count
  of how many are fine and how many need a look, and a "Check again" button. Check (one camera) still shows its answer in the box.
- **Not built:** setting the time or the NTP server from KVision (a write; do it in the camera's web page, System > Time Settings).

### 12. [~] Firmware version and update check - version shown, "update may be available" from a note you keep; apply NOT built
- **Built:** the "Camera clock and firmware" box shows each camera's firmware version, build date and model (`System/deviceInfo`).
  After a Check, a row appears with the model: type the newest firmware version you see on Hikvision's page for that model and
  press Save (stored in the config file under `NewestFirmware`, one value per model). "Open Hikvision page" opens Hikvision's product search for the start of the model name in your browser
  (DS-2CD2347G2-LSU/SL gives `https://www.hikvision.com/en/search/?keyword=ds-2cd2347&q=ds-2cd2347&active=Products`, the form the
  user found works; the first form tried, without `q` and `active=Products`, found no products). Check and Check all then say "Update may be available" (orange) when the camera's version is
  lower than the note, "matches" when equal, "newer than the note" when higher, and "none noted for this model yet" otherwise.
  The versions are compared number by number (V5.8.9 is older than V5.8.40). It always says "may be", never "is".
- **Tested:** the comparison and the per-model storage (model names contain "/", which is stored as "|") with a small test program
  (10 version cases, all as expected); the box itself has not been tried by a person yet.
- **Can the cameras check for an update themselves? No.** `GET /ISAPI/System/onlineUpgrade/version?check=false` answers 403
  `SW_ONLINE_UPGRADE_SUP not support` on the G3 (192.168.20.137) and the PTZ 192.168.20.66, and 403 on every other camera read
  (192.0.0.4, .30, .60, .150, 192.168.20.51, .88, .90, .97). The 192.168.20.185 doorbell has no such address (404).
- **Can KVision read Hikvision's site for the newest version? No.** The firmware download page and the product pages answer a
  program with a small script that sets a cookie and reloads, and no content: an anti-bot check, which we do not get around. The
  page does list the current firmware when opened in a browser, which is why the note is typed by hand.
- **Not built (risky):** applying a firmware file the user downloaded: `PUT /ISAPI/System/updateFirmware` (the camera's web page
  uses it), `System/upgradeStatus` for progress (answers on the G3: `upgrading=false`). A failed or wrong flash can leave a camera
  unusable. If ever built: one camera at a time, the file's model checked against `deviceInfo`, the settings backed up first (the
  Camera backup tool), a clear warning that the camera restarts, two confirmations, never "upgrade all".

### 13. [ ] Supplement light and dehaze
- **Why:** simple on-demand helpers for night views.
- **Found:** `Image/channels/<id>/supplementLight` (with `/capabilities`), `.../irLight`, `.../ircutFilter` and
  `.../dehaze`.
- **Not yet tried.** Read the capabilities first to see what each camera allows. Change one field, read it back, and
  restore the old value afterwards.


## Ideas not taken from the Node-RED project (on purpose)

- Alarm panel, doorbell, speaker and access-control nodes: outside what KVision is for.
- Network discovery of devices by multicast: KVision adds recorders by address, and we do not want to scan the network.
- Licence-plate recognition: only worth it if a plate camera is added.
- Other events on the same stream (tamper, defocus, scene change, video loss, storage errors, temperature): worth a
  look later as a separate "camera problem" frame.
- From reading other Hikvision projects (12 Nov): automatic discovery of devices on the network (SADP), and a warning
  when a camera's clock is more than a few minutes off (digest login fails). Neither done.
