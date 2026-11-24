# Changelog

> [!IMPORTANT]
> This is a translation of the authoritative [French changelog](CHANGELOG.md), based on
> [`v0.13.0-rc.10`](https://github.com/Jiraiya43/EntryNav2-Android-Auto/blob/v0.13.0-rc.10/CHANGELOG.md).
> It may become outdated; consult the French changelog for the source of truth.

All notable changes to this project. Format inspired by [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
versions follow [SemVer](https://semver.org/).

Each version states whether it was **tried in a car** or only **tested on a PC** (PC tests and ARM programs under
QEMU with the car's libraries).

## [Unreleased]

## [0.13.0-rc.10] - 2026-11-24 · PC

Release candidate: updates from a USB stick, without a PC (archive signed by the project, everything checked before
anything is written, an interrupted update finished by the next one), and Android Auto's arrows on the cluster of
F-series cars over MOST. This version itself still installs over SSH (installation steps 1 to 3); later ones can be
installed from a USB stick. Tested on PC and under QEMU, **not yet tried in a car**.

### Added

- **Update from a USB stick**, with neither a PC nor SSH: the archive of a published version, copied as is to the root
  of a stick, then *Mettre à jour par clé USB* (*Update from USB stick*) in the Android Auto menu. A first press checks
  everything without installing anything (the project's signature, the hash of every file, programs built for this
  unit, the libraries and functions they need present on the car) and says what a second press installs: a newer
  version, an older one, or the same one again. The second press, never during a call or in reverse gear, stops
  Android Auto, takes exactly the archive that was checked, writes the new files beside the old ones, reads them back,
  then puts them in place one by one by renaming; the unit restarts by itself. Refused without any change, the
  install starts Android Auto again. A power cut during the swap is finished by the next update, from the list kept on
  the data partition. Report in `EntryAA-update.txt` on the stick and in the copied logs
  ([installation](docs/INSTALLATION.md#par-clé-usb-sans-pc)). Published archives now hold an `UPDATE` list signed by
  the project's key; an archive built by oneself (`tools/package_release.py --unsigned`) installs by SSH only. This
  version itself still installs over SSH, once; later ones from a USB stick. Tested on PC and under QEMU with the
  car's glibc and libcrypto, not yet in a car.
- Version number in the title of the *Help* page of the Android Auto menu in iDrive (for example "Help -
  0.13.0-rc.10"): a photo of the page is enough to know which version runs in the car. Its six lines stay the
  same. Tested on PC.
- The phone sees the car's series ("BMW G20", "MINI F56") instead of "iDrive" when the `vehicle` file gives no model.
  The iDrive app reads it in the coding, where CarPlay reads it to introduce the car to an iPhone, never reading the
  rest of that block, which holds the car's VIN ([#15](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/15)).
  The file's `model=` line always wins. Tested on PC.
- Android Auto's arrows on the cluster of F-series cars, over MOST: arrow, distance, unit and street, in the guidance
  fields the BMW navigation fills (`HUD_Terminal` block). Only without an Ethernet cluster (G-series cars do not
  change), never during BMW guidance, nothing created on the cluster. A cluster that answers seconds late after a cold
  iDrive start is waited for (10 s per call, eight calls ahead), and the arrow follows the guidance when it moves to
  the head-up display
  ([compatibility](docs/COMPATIBILITE.md#flèches-sur-le-compteur-des-série-f),
  [#79](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/79)). Tested on PC only, from the description of a
  6WA cluster (F52).

### Fixed

- Android Auto reception: the size limit of a message is checked with no risk of overflow on ARM, separately for each
  message. A message that is too large is dropped and its remaining fragments are ignored up to its end; a whole message
  does not disturb the fragments in progress ([#122](https://github.com/Jiraiya43/EntryNav2-Android-Auto/pull/122)).
  Tested on PC.

## [0.13.0-rc.9] - 2026-11-22 · PC

Release candidate: logs without a PC (kept after the car sleeps, copied to a USB stick from the iDrive menu) and
wireless that comes back quickly after a drop (Bluetooth link dropped by the car's stack, access point beacon
rebuilt only once). Tested on PC and under QEMU, **not yet tried in a car**.

### Added

- Logs without a PC or SSH. The Android Auto menu in iDrive has two more rows: *Keep logs*, which turns the logs on
  (the `maintenance` file), and *Copy logs to USB stick*, which copies them onto a USB stick plugged into the car, in
  an `EntryAA-logs/<date>` folder, with the head unit's software version. The row then says whether the copy worked,
  or that there is no stick or no log. Nothing is deleted or changed on the head unit
  ([maintenance](docs/MAINTENANCE.md#journaux)). Tested on PC.

### Fixed

- Service log lost when the car went to sleep: it lived in RAM (`/tmp`), and the log of a drive was gone before it
  could be fetched ([#101](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/101),
  [#88](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/88)). With logs on, it now lives on the data
  partition like the app's: 512 KB at most, plus the previous 512 KB in `service.old.log`, checked every 5 s so that
  it never fills the partition. `/tmp/entry-aa-service.log` stays as a link to it. If the partition refuses to open
  it, it goes back to RAM as before. Tested on PC.
- Wireless: after a drop, the phone took 3 to 7 s to come back instead of 2 s with 0.13.0-rc.8
  ([discussion #30](https://github.com/Jiraiya43/EntryNav2-Android-Auto/discussions/30), F45). While it joins the
  Wi-Fi, the phone reopens its Bluetooth link about every 400 ms, and the service had the access point rebuild its
  beacon each time (up to 6 times in 4 s), which can disconnect it mid-join. The beacon is now rebuilt once per
  start of the access point; the setting itself is still made each time. Tested on PC.
- Wireless: restarting the phone after a drop (0.13.0-rc.7) closed the Bluetooth link on our side only. The phone
  saw nothing and kept waiting: back after 70 s, or not at all, on the MINI F56 of
  [#88](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/88). The service now asks the car's Bluetooth
  stack to drop that link, as Stop does: the phone sees it close, opens it again and gets a new request, as after
  Stop then Start, where it came back within 2 s. Tested on PC.

## [0.13.0-rc.8] - 2026-11-22 · PC

Release candidate: sturdier wireless (no U-APSD offered to the phone, session restarted after a failed send or a
broken encryption, Bluetooth given back on stop), a picture that no longer freezes after lost messages or a stalled
decoder, the driver's side read from the car's coding, no more blank screen after reversing. Tested on PC and under
QEMU, **not yet tried in a car**.

### Added

- Driver's side read from the car's coding: the iDrive app asks iDrive whether the car is coded left-hand or
  right-hand drive, as it already does for the brand and the keys, and the service tells the phone. A right-hand
  drive car no longer needs a `side=right` line in the `vehicle` file; that line is still accepted and wins over the
  coding. When the coding says nothing certain, left-hand drive as before. The app's log writes `driver side: left` or
  `right`, the service's log the side it received. Tested on PC.

### Changed

- Wireless: a picture that arrives less than 25 ms after the previous one waits for the rest of that delay before
  going to the decoder. Over Wi-Fi, pictures can arrive in pairs, a few milliseconds apart: the screen only draws the
  last one received at each refresh, the first of the pair is never shown, and the picture stutters although nothing is
  lost on the network. The picture that follows a held one never waits: the added delay stays at 25 ms at most and does
  not build up, and well-spaced pictures go through at once. USB mode is unchanged. The maintenance log line written
  every 10 s counts the pictures that arrived too close to the previous one, those held and the longest wait, to see
  whether our cars have this defect. Tested on PC.

### Fixed

- Wireless: on stop, Bluetooth could fail to be given back while the phone was still looking for the car, and the
  following Starts were then refused in less than a second. While the Android Auto record stays on its Bluetooth slot,
  the phone can reconnect to it before the supervisor, which looked at the slot every 100 ms, has seen it free, and the
  car's Bluetooth stack refuses to change the record of a connected slot. This is the most likely explanation of
  [#88](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/88) (MINI F56, Galaxy Z Fold: two Starts refused
  after Stop). The supervisor now looks at the slot every 10 ms, puts the original record back as soon as it is free,
  and asks for the disconnection again when the phone came back first (4 times at most, 8 s at most). The earlier
  checks remain, and an unconfirmed restore always keeps its log. Tested on PC.
- Wireless: when Wi-Fi stalled for several seconds during a send from the car, the session went on over a broken
  connection. The send given up after 5 s could leave a message cut in two, and everything the car sent afterwards was
  unreadable for the phone, without the session ending. A failed send now ends the session like a drop: nothing more
  goes out, and the phone is restarted as after a lost session. Tested on PC.
- Wireless: an encrypted message damaged on the way left the session running on dead encryption. In TLS, a record that
  fails its check is fatal: nothing decrypts afterwards, not even the good ones, and the session waited for 15 more
  unreadable frames before ending. The first message the car sent next (often the ack of that picture) also went out
  with OpenSSL's alert in its place. The session now ends at that frame without sending anything more, and the phone is
  restarted as after a lost session. A record cut over two frames is still read normally, and 16 frames in a row that
  bring no complete record still end the session. Tested on PC.
- Picture (or guidance prompts) frozen for the rest of the session after a few lost messages. The phone only sends 10
  messages ahead without an ack from the car; a message the car dropped (picture too short, fragments that arrived
  incomplete, unreadable fragment) was never acked and kept one of those places for good, and at the tenth the stream
  stopped. Each dropped message is now acked once, and the rest of a message lost on the way is ignored quietly up to
  its last fragment. Tested on PC.
- Picture: if the car's video decoder stopped giving back its buffers (after a decoding error, its watchdog or an
  abort), the Android Auto picture stayed frozen until the end of the session, while the phone kept sending pictures
  that found no room. When no buffer has come back for 3 s while the picture is on screen, the service notes it once
  and ends the session like a drop: the phone comes back on a fresh decoder (wireless: its Bluetooth link is closed to
  start again; USB: it is asked again). The following pictures no longer wait 20 ms each for a buffer that will not
  come. Tested on PC.
- Assistant microphone: a microphone that gave no more sound blocked reading, and with it stopping Android Auto (Stop
  or end of the service). A microphone that failed again right after being repaired was also closed and reopened
  without a pause, in a loop, as long as the Assistant listened. The microphone is now read without blocking, in
  100 ms slices; 2 s without sound count as a failure, it is reopened after 1 s, and after 3 failures in a row without
  any sound it waits for the phone's next request. Stopping stays immediate. Tested on PC.
- Car GPS: a single speed of 70 m/s (252 km/h) or more is enough for Android Auto to ignore the speed of every
  following position until the end of the session. The service accepted up to 150 m/s, and the speed computed between
  two positions could exceed 70 m/s when a position arrived late. Every speed sent now stays below 70 m/s: a faster
  position goes without its speed, and a computed speed that is too high is not sent, nor the bearing that goes with
  it. Tested on PC.
- Car description: the phone always received Google's model (the Desktop Head Unit), that is left-hand drive, petrol
  plus electric and a Tesla Supercharger connector. A right-hand drive car was thus announced as left-hand drive, and
  Android Auto laid its screen out for the wrong side. The `vehicle` file now accepts `side=left` or `side=right` and
  `fuel=petrol` or `fuel=diesel` (without them: the side coded in the car, see "Added", and petrol); the phone receives
  the driver's side, a single fuel and no charging connector ([installation](docs/INSTALLATION.md)). Tested on PC.
- Touch: when the Android Auto picture went away (volume, iDrive window, reverse gear, MENU) as the finger lifted, the
  phone could keep that finger down until the next touch. The release that arrived just after the picture left was not
  sent, and the service forgot that finger with nothing left to release. The release of a finger the phone saw touch
  down is now always sent to it; nothing else goes out once the picture is gone. Tested on PC.
- Keys: COM pressed just before a call started stayed held on the phone until the picture went away. During a call,
  COM is left to iDrive's call screen, and that rule also held back the release of a press already sent to Android
  Auto. The release now always follows its press; a COM pressed during the call stays with iDrive, as before. Tested
  on PC.
- iDrive app: Android Auto could step aside without any key being pressed. When iDrive left the blank page for one
  poll, came back to it, then left it again for one poll less than 3 s later, the app took those two departures for a
  confirmed iDrive key (MENU, MEDIA…). The first departure is now forgotten as soon as the blank page comes back; a
  real key is still confirmed at the next poll. Tested on PC.
- iDrive app: blank screen after reversing
  ([#88](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/88), MINI F56). Park assist stays on for a while
  after reversing, and Android Auto stayed hidden as long as it was. When BACK closed its view (or it closed by
  itself), iDrive went back to Android Auto's blank page without its picture, until park assist ended or the driver
  went through the menu. That return to the blank page, out of reverse, now counts as choosing Android Auto, which
  stays on screen until park assist next changes. The service's log no longer says "picture back" when leaving
  reverse while park assist still hides the picture. Tested on PC.
- Wireless: picture and sound stuttering with some phones
  ([#138](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/138), X1 F48). The car's Wi-Fi offers U-APSD, a
  phone power-saving mode, and the Wi-Fi chip then held the phone's frames back for several seconds: on peppearci's
  X1 F48, its counter of expired U-APSD waits reached 1700 in 12 minutes, and the stuttering was gone without U-APSD.
  Before each Wi-Fi exchange with the phone, the service now sets hostapd to stop offering it, then has it rebuild its
  beacon, from which the phone asks for U-APSD when it connects. That rebuild disconnects the devices present: it is
  only done when none is on the car's Wi-Fi, so an iPhone on CarPlay is never cut off. The setting stays in memory, no
  file of the car changes. The service's log says what was done (`car access point:` line,
  [maintenance](docs/MAINTENANCE.md)). Tested on PC.

## [0.13.0-rc.7] - 2026-11-20 · PC

Release candidate: after a wireless drop, Android Auto comes back by itself when the phone keeps its Bluetooth link,
instead of waiting for Stop then Start. Tested on PC and under QEMU, **not yet tried in a car**.

### Fixed

- Wireless: after a drop, Android Auto did not come back by itself when the phone kept its Bluetooth link open. The
  service answered that link without ever asking the phone to come back, and Stop then Start was needed
  ([#88](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/88): MINI F56, Galaxy Z Fold, back within 2 s
  after the manual restart). A lost session (phone silent for 5 s, broken connection) now closes that link: the phone
  opens it again and receives a new request, as after a restart. Leaving Android Auto on the phone is not a drop, and
  USB mode is unchanged. Tested on PC.

## [0.13.0-rc.6] - 2026-11-20 · PC

Release candidate for the wireless slowness: the Android Auto connection gets the Wi-Fi "video" priority, like the
car's CarPlay. Also: the camera after reversing, incomplete Android Auto messages. Tested on PC and under QEMU,
**not yet tried in a car**.

### Changed

- Wireless: the Android Auto connection gets the Wi-Fi "video" priority, as the car's CarPlay does for its picture
  (Apple's code in BMW's CarPlay: `IP_TOS` 0x80, Wi-Fi video category). What the car sends to the phone (picture acks,
  touch, keys, TCP acks) then goes before ordinary traffic; until now, nothing was marked. A trial against the wireless
  slowness ([discussion #30](https://github.com/Jiraiya43/EntryNav2-Android-Auto/discussions/30),
  [#101](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/101),
  [#129](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/129)): the effect remains to be measured in a car.
  USB mode is unchanged. Tested on PC.
- Android Auto reception: reuse buffers throughout each connection without changing protocol limits. Reassembled
  message buffers larger than 1 MiB are released after use. Tested on PC.

### Fixed

- Reverse gear: leaving reverse sometimes arrives before the camera closes, and Android Auto could come back too
  early. The app now waits until iDrive comes back to one of the app's pages, or until the driver chooses Android Auto
  (Open, entry), 15 s at most ([#114](https://github.com/Jiraiya43/EntryNav2-Android-Auto/pull/114), adapted from
  AramayisAntonyan's work). Tested on PC, not yet tried in a car.
- Android Auto reception: a message fragment (MIDDLE or LAST) that arrives without its start is rejected, instead of
  giving an extra picture or ack ([#115](https://github.com/Jiraiya43/EntryNav2-Android-Auto/pull/115), adapted from
  Sq00ky). Tested on PC.

## [0.13.0-rc.5] - 2026-11-19 · PC

Release candidate: no black screen when changing driving mode if iDrive is not set to show the mode window,
USB-specific compatibility check, BMW Apps link in the wireless log. Tested on PC and under QEMU,
**not yet tried in a car**.

### Added

- Wireless log: each `[WifiLink]` line says whether a phone holds the Bluetooth link for BMW Apps (My BMW,
  AAIdrive, the phone's *Apps* option in the car), which shares a chip with Wi-Fi. With *Apps* enabled, wireless
  Android Auto started poorly on an F40 ([#71](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/71)):
  the logs will show whether this link is active during slowdowns. Read-only, only when logging is enabled.
  Tested on PC.

### Fixed

- Compatibility check: a failed Bluetooth check (`alps`, `uuid`, `spp`) reported `@block` as if it prevented all
  installation, although only wireless uses it. Its detail now says so (`wireless only`), and with the
  `/var/local/data/AA/usb` file it reports `@info`. An `@info|usb|` line shows that USB mode is selected
  ([#106](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/106): 2017 F20 using USB, Bluetooth library
  from a different build). Tested on PC.
- Driving mode changes: Android Auto disappeared for 3.5 s to show the mode window even when the mode display was
  disabled in iDrive, causing a black screen at every change
  ([discussion #28](https://github.com/Jiraiya43/EntryNav2-Android-Auto/discussions/28)). The app now reads this
  iDrive setting, and Android Auto stays on screen when it is off. If the setting cannot be read, behavior is
  unchanged. Read from the reference car's system and tested on PC, not yet tried in a car.

## [0.13.0-rc.4] - 2026-11-18 · PC

Release candidate: volume overlay during a call, updated compatibility check for 800x480 screens. Tested on PC and
under QEMU, **not yet tried in a car**; outside calls, nothing changes on screen.

### Fixed

- Service log: `[SDR] main VideoConfig` always showed 0 for the left margin; it now shows the value sent to the
  phone (40 on a MINI). The phone already received the correct value.
- Compatibility check: an 800x480 screen is no longer flagged with `@warn|screen|` as never tried in a car. It has
  worked on an i3, an F20, an X1 F48 and a MINI F57; the warning remains for other sizes.
- Volume overlay during a call: it did not appear. During a call, the controller and steering wheel buttons adjust
  call volume, which iDrive reports on a different scale (25 to 100 on a MINI F56), and the service ignored it.
  It now shows the overlay after a button press or on a change, never merely when a call arrives
  ([#88](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/88)). Tested on PC using that car's log,
  not yet tried in a car.

## [0.13.0-rc.3] - 2026-11-16 · PC

Release candidate: Android Auto moves to the background while parking assistance is opened with its button. Tested
on PC and under QEMU, **not yet tried in a car**; nothing changes while parking assistance is off.

### Added

- Parking assistance opened with its button without selecting reverse: Android Auto stayed over the camera. The
  iDrive app monitors `sensors.PDCStatus`: when parking assistance turns on, Android Auto moves to the background as
  it does in reverse and returns when it ends. As parking assistance stays on for a while after reversing, choosing
  Android Auto during that time (Open, the entry button, double MEDIA) keeps it on screen until the next status
  change ([#26](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/26)).
- App log: the screen displayed by iDrive (`hmi.graphicalContext`) at each change, to identify camera and panorama
  screen numbers for each car.

## [0.13.0-rc.2] - 2026-11-16 · PC

Release candidate: two fixes for 0.13.0-rc.1, MINI touch input and Start being rejected after the clock moves
backwards. Tested on PC and under QEMU, **not yet tried in a car**; a BMW receives the same car description and
touch input as before.

### Fixed

- MINI: touch input landed 40 pixels to the right of the finger since 0.13.0-rc.1. The edges hidden by the circle are
  margins for the phone, whose display starts 40 columns further in; it reads touch in its own pixels, so each point
  is now given in that coordinate system ([#78](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/78),
  [#88](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/88)). Nothing changes on a BMW.
- Start rejected ("pressed again within 3 s") after stopping, until the unit's clock, set back by a few hours after
  startup, caught up with the previous start time. The iDrive app's delays (Start debounce, service shutdown,
  automatic start 30 s after iDrive, guard reset) now use the unit's monotonic clock, unaffected by clock changes;
  previously automatic start could also wait that long
  ([#86](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/86)).

## [0.13.0-rc.1] - 2026-11-15 · PC

Release candidate: MAP opens Android Auto navigation, MINIs are recognized (brand and round screen), volume and
mute behave like iDrive, plus the fixes from 0.12.1-rc.1 through rc.4. Tested on PC and under QEMU,
**not yet tried in a car**; a BMW receives the same car description as before.

### Added

- MAP opens Android Auto navigation when it is on screen, like NAV, instead of returning the screen to the BMW map;
  this is how iDrive handles CarPlay. Only MENU and OPTION leave Android Auto
  ([#1](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/1)).
- MINI: the brand coded in iDrive is read at startup; a MINI identifies itself to the phone as "MINI" rather than
  "BMW". Its screen is framed by a circle that hides the edges: the phone keeps its buttons out of 40 columns on
  each side of a 1280-wide screen, 25 on an 800-wide screen
  ([#56](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/56),
  [#64](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/64),
  [#78](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/78)).
- Mute on the volume overlay: pressing the volume button toggles mute in iDrive; one volume step clears it. The
  overlay shows a crossed-out speaker.
- Corrupted picture after a lost video frame: the session asks the phone for a full frame, not during the first
  5 seconds of video and at most once every 10 s.

### Fixed

- Volume overlay opening by itself on connection with an empty bar: the car also sends another source's level
  (with a minimum above 0), and sends the same level repeatedly. Only controller volume opens the overlay, on a
  change or just after a volume button press, like the iDrive bar
  ([#56](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/56)).
- Broken encrypted stream: after 16 consecutive frames fail to decrypt, the session ends rather than keeping a
  frozen picture until the silence timeout.
- `[WifiLink]` log: no more false "signal 0 dBm" (the chip did not measure the level) or transmit rate to the
  phone, which the driver does not know (it reported 1 Mbit/s everywhere).

## [0.12.1-rc.4] - 2026-11-14 · PC

Release candidate to try double-pressing MEDIA after another button on the older controller (the F20 in #55), on
top of 0.12.1-rc.3. Tested on PC and under QEMU.

### Fixed

- Double-pressing MEDIA had no effect on the older controller after another button (MENU, for example): another
  button made the service forget MEDIA's state, and the older controller, which sends only one message per press,
  restored it only on the next press, losing that press for the gesture. Another button now cancels the gesture
  without forgetting the state, and each press on the older controller is treated as preceded by a release. Found
  and analyzed on an F20 ([#55](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/55)). Tested on PC
  and under QEMU.

## [0.12.1-rc.3] - 2026-11-14 · PC

Release candidate to try the MEDIA button on the F20 in #55 (controller coded 0x5141), on top of 0.12.1-rc.2.
Tested on PC and under QEMU; nothing changes on a G-type controller.

### Fixed

- MEDIA on an F20 with the small ZBE3 controller still had no effect in 0.12.1-rc.1: this car is coded with the
  0x5141 variant, and iDrive translates its media button to code 15, which had no Android Auto key. Code 15 comes
  only from this button: it is now MEDIA, like code 5 on other small F controllers (opens Android Auto music,
  used for the double press). Reported with the coding read from the car
  ([#55](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/55)). Tested on PC and under QEMU.

## [0.12.1-rc.2] - 2026-11-14 · PC

Release candidate to try finger swipes on an F45 (reading finger state like iDrive), on top of 0.12.1-rc.1. Tested
on PC and under QEMU; nothing changes on the reference G20. When installed over a version older than 0.12.1-rc.1,
it requires a head unit restart.

### Fixed

- Swipes received as taps on an F45: scrolling a list or moving the map was impossible; Android Auto opened the
  touched item ([discussion #30](https://github.com/Jiraiya43/EntryNav2-Android-Auto/discussions/30)). The service
  read finger state from 4 bits; iDrive reads it from 2 bits (press, move, release) and stores the adjacent 2 bits
  separately. On a screen that sets those adjacent bits, movements were discarded and the finger appeared to lift
  after about 100 ms. The service now splits the byte like iDrive. Nothing changes on the reference G20, whose screen
  leaves those bits at 0. Tested on PC and under QEMU; not yet tried in a car.

## [0.12.1-rc.1] - 2026-11-14 · PC

Release candidate to try buttons interpreted according to car coding on F-series controllers (F20, F45), especially
small 5-button controllers. The 6-line *Help* page was seen in the reference G20; the buttons interpreted according
to coding have only been tested on PC and under QEMU. It changes the iDrive app: when installed over a previous
version, it requires a head unit restart.

### Fixed

- Older controller buttons interpreted incorrectly on small F-series controllers: on an F20 (5-button ZBE3), MEDIA
  was read as COM and COM as MAP, and double-pressing MEDIA did not work
  ([#55](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/55)). iDrive chooses each button's key based on
  the car's coding (G- or F-type controller): the iDrive app now reads the same coding at startup and writes it for
  the service (`/tmp/entry-aad-controller`), which translates buttons like iDrive. The media button on a small F
  controller opens Android Auto music and is used for the double press; NAV on an F controller opens its navigation.
  Without this file (older app), the previous table remains. Nothing changes on a G-type controller. Tested on PC
  and under QEMU; not yet tried in a car.
- *Help* page: in the car it displays only 6 lines of about 36 characters and does not scroll (its lines cannot be
  selected). The credit, in positions 8 to 10, remained invisible, and several lines were cut off. The page now fits
  in 6 short lines: the credit first ("Free: github.com/Jiraiya43", the profile leading to the repository), then NAV,
  MEDIA, COM, MENU and BACK, and the phone paired for audio.

## [0.12.0] - 2026-11-13 · car

First stable release of the 0.12 series: 0.12.0-rc.12, tried in the reference G20, plus two lines on the iDrive
*Help* page. The code of the three programs is that of rc.12; only the version number announced to the phone changes.
The modified *Help* page has only been seen on PC. It changes the iDrive app: when installed over a previous version,
it requires a head unit restart.

### Added

- *Help* page in the iDrive Android Auto menu: two lines at the bottom say that the software is free and not for sale,
  with the repository address (`github.com/Jiraiya43/EntryNav2-Android-Auto`). They remain visible in any redistributed
  copy of the archive. Text in English, French and German.

## [0.12.0-rc.12] - 2026-11-13 · car

Release candidate, **tried in the reference G20** (software 202111011321), wirelessly, with an identical test kit:
automatic start on every startup, Android Auto on Bluetooth slot 5, double MEDIA press, BACK, MAP and MENU.
Versions 0.12.0-rc.7 through rc.11 were used only for test kits and were not published. The archive is now named
`entrynav2-android-auto-<version>-armhf.tar`. It changes the iDrive app: when installed over a previous version,
it requires a head unit restart.

### Added

- Return to an Android Auto session connected in the background with two short MEDIA presses, at most 500 ms between
  releases. The controller reader already in the service detects the gesture; the iDrive app checks the session,
  shutdown, calls and reverse again before opening the picture through the *Open* button's path. A single press
  keeps its usual effect; on screen, MEDIA keeps its Android Auto controls. No new process, MOST provider or modified
  BMW file. Proposed by [@wowDaiver](https://github.com/wowDaiver)
  ([#40](https://github.com/Jiraiya43/EntryNav2-Android-Auto/pull/40)). Tried in the reference G20 (CCS35up,
  wireless) and an X3 G01 (USB cable); not yet on the older controller.

### Changed

- The project is now called **EntryNav2 (EntryEvo) Android Auto**, and its repository
  `Jiraiya43/EntryNav2-Android-Auto`. The old address `Jiraiya43/entry-aad` redirects to the new one, including
  published release downloads. Names on the car do not change: programs, directories, iDrive app and settings stay
  the same, and an existing installation updates as before.
- Bluetooth: Android Auto now takes the free serial slot 5 in the car's Bluetooth stack instead of slot 0 for BMW
  Apps, where My BMW connects. While Android Auto was running, My BMW could no longer find the car, and the BMW
  service for that slot treated each link as its own; one user saw wireless slowdowns disappear after blocking My BMW
  ([#44](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/44)). If slot 5 is not free, Android Auto stays
  on slot 0 as before; the slot is returned on shutdown. Tried in the reference G20: Android Auto starts and stays
  connected on slot 5, while slot 0 remains available to My BMW. The effect on slowdowns remains to be confirmed by
  those who experienced them.

### Fixed

- Android Auto moving to the background about 1.5 seconds after pressing BACK when it had been opened with NAV,
  MEDIA or COM (for example BACK in its map). iDrive also opens its own screen underneath, and BACK returns it there;
  the iDrive app mistook this change for a key leaving Android Auto. BACK and a left tilt are now handled like NAV,
  MEDIA and COM: Android Auto keeps the screen and iDrive is returned to the blank page. MENU, MAP and OPTION still
  leave Android Auto, even immediately after BACK. Seen in an F30's logs
  ([#43](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/43)). Tried in the reference G20 (BACK and left
  tilt in Android Auto, MAP and MENU leaving it); the exact F30 case, BACK after NAV, remains to be confirmed on an F30.
- Android Auto session disconnected 5 seconds after MENU or MAP when no music is playing: some phones send nothing
  while iDrive has the screen, and the service thought they had gone away. After 2 seconds without a message, the
  head unit now sends the phone a ping itself, then one per second. A phone that responds keeps its session; one
  that does not still loses it after 5 seconds, as before. Found by [@wowDaiver](https://github.com/wowDaiver) on an
  X3 G01 over USB ([#40](https://github.com/Jiraiya43/EntryNav2-Android-Auto/pull/40)). Tried in the reference G20
  without visible change; its phone sends its own pings, so the silent-phone case was verified only on PC and under
  QEMU.
- Missing arrows on the G01 / EntryNav2 002.146.002 cluster: the cluster rejected the second route model in the
  captured journey with `0x26`, then the service cleared guidance. Creating
  `/var/local/data/AA/kombi-single-maneuver` and restarting Android Auto selects a graph limited to the next
  maneuver; without this file, the original graph remains unchanged. No new network calls: a subset of the existing
  guidance methods is sent. Tried in a car on November 12, 2026, based on 0.12.0-rc.2: arrows confirmed with Google
  Maps and Yandex Maps, startup and cleanup acknowledged. The current port is verified on PC; long journeys,
  successive maneuvers and lanes remain to be validated while driving.
- Controller still operating iDrive under Android Auto when the latter returned over an iDrive screen (double MEDIA
  press, automatic start): iDrive then rejected the blank page and Android Auto stayed on screen without it. After
  the first rejection, the app opens its own blank window and requests the blank page underneath, as when iDrive
  rejects its menu at startup. Tried in the reference G20: double MEDIA on the Media screen and automatic start;
  the blank page is obtained and the controller operates only Android Auto.

## [0.12.0-rc.6] - 2026-11-12 · car

Release candidate, **tried in a BMW G20** wirelessly: iDrive accepts the new app, and Android Auto comes on screen
by itself and stays there. At that startup, iDrive accepted the first request: going through the app window when it
refuses has only run in PC tests, like the screen adaptation and older controller, which were separately confirmed
on a BMW i3. It changes the iDrive app: when installed over a previous version, it requires a head unit restart.

### Added

- Screens other than 1280x480: the service reads the screen size from the car and asks the phone for a suitable
  picture (800x480, 1280x720 or 1920x1080), without configuration. The compatibility check no longer blocks on
  size: it reports it (`@ok|video|800x480`) and warns if it has not been tried (`@warn|screen|`). On a 1280x480
  screen, what is sent to the phone is identical byte for byte.
  [Details](docs/COMPATIBILITE.md#autres-tailles-décran) ([#19](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/19)).
  Tried in an i3 with an 800x480 screen, by cable; other sizes are tested on PC.
- Logs: every Bluetooth connection the car opens, accepts or loses now says whether the device is the Android Auto
  phone or another device (no address is logged), and every disconnection gives the reason from the car's Bluetooth
  stack (`reason=`). The head unit's Bluetooth and Wi-Fi share a chip: these lines show what Bluetooth is doing while
  Android Auto uses Wi-Fi.
- Service: the Android Auto Bluetooth link returning during a session is now listened to. On this unit the phone
  reopens it about every 12 seconds; the service still never writes to it first, so as not to restart the current
  session, but it answers a ping and logs what the phone sends.
- Logs: every 10 seconds of a wireless or wired session, the phone's longest silence
  (`[Session] in 10 s: the phone's longest silence ... ms`). A projecting phone sends something every second:
  several seconds of silence indicate a stalled connection, which nearby `[WifiLink]` and Bluetooth lines help explain.

### Fixed

- Android Auto appearing by itself when the car was unlocked, then disappearing after 4 seconds: it had to be
  reopened manually. iDrive does not execute an app's screen requests while one of its own windows is on top, and
  with the car unlocked but not started, that can last until a button is pressed. Since 0.9.4 the app then removed
  Android Auto. Now the service keeps it in the background on connection while the app puts iDrive on its page. If
  iDrive refuses, the app opens its own blank window, which iDrive allows an app to do to come to the front, requests
  its page underneath, closes the window, and Android Auto appears: 2 seconds in all. If even that is refused, Android
  Auto still appears and stays on screen, and the app requests its page again every second; until then the controller
  also operates the hidden iDrive screen underneath. Changes the iDrive app: restart the unit after installation.
- Android Auto refusing to start for an entire journey, with `UDP 127.0.0.1:52790 already taken` in the service log.
  The iDrive app and service communicated over a local network port in the range the unit randomly assigns to its
  own programs: on some startups another program received that port, and the service wrongly believed another service
  was already running. They now communicate over an abstract Unix socket, without a port that another program could
  receive by chance. Present since 0.10.0, seen once on the reference car. Changes the iDrive app: restart the unit
  after installation. Tried in a car.
- Unresponsive controller on a car with the older controller (BMW i3): the service could decode its messages (C10,
  C20) but never requested them, so the car did not send them. It now requests them when the controller does not
  respond using the G20 protocol within 2 seconds. A responding car, like the G20, receives nothing extra, and only
  one protocol is kept per car. Reported by kedzior12
  ([#19](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/19)), who tried it on an i3: rotation, press and
  tilt reach Android Auto.

## [0.12.0-rc.2] - 2026-11-11 · PC

Release candidate, **tested on PC only**: to be tried in a car. It changes the iDrive app: when installed over
0.12.0-rc.1, it requires a head unit restart.

### Added

- iDrive menu: the **USB cable instead of wireless** checkbox selects the mode without a PC or SSH. It creates or
  removes the `usb` file; if Android Auto is running, it stops and restarts in the new mode. The mode is never
  inferred from what is plugged in: a passenger charging a phone changes nothing. A help line reminds users that audio
  remains on Bluetooth. [Details](docs/USB.md). Tested on PC.
- Logs: Wi-Fi quality with the phone, a `[WifiLink]` line at connection and every 10 seconds thereafter (frequency,
  signal, rates, retransmitted and lost frames, round-trip time). Read-only, only when logging is enabled, nothing
  in USB mode. [Reading these lines](docs/MAINTENANCE.md#qualité-du-wi-fi). Tested on PC.

### Changed

- Installation: the version number is now written only twice (the `scp` command and the `V=` line), instead of
  repeated in every command ([#19](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/19)).
- Roadmap: USB cable is no longer "out of scope"; its remaining tests are listed, and the first compatibility report
  from another car is recorded ([#17](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/17)).

## [0.12.0-rc.1] - 2026-11-11 · car

Release candidate, **tried in a BMW G20**: Android Auto starts wirelessly and by USB cable. Still to confirm by cable:
reliability on a long journey, unplugging while on the road, and other phones and cars. The 0.11.0-rc.1 changes are
included; 0.11.0 will not be released separately.

### Added

- Experimental, optional Android Auto over USB: with `/var/local/data/AA/usb`, the service waits for an Android
  phone on the car's USB ports instead of preparing Bluetooth and Wi-Fi. Without this file, nothing changes: Android
  Auto remains wireless. Audio stays on the car's Bluetooth. [Enabling and limitations](docs/USB.md). Idea, first car
  tests and observations on BMW's USB service: [@wowDaiver](https://github.com/wowDaiver)
  ([#18](https://github.com/Jiraiya43/EntryNav2-Android-Auto/pull/18)). Tried in a car.
- New writes to a device in the car: accessory mode requests (AOA 51, 52, 53) and a USB reset, sent only to a
  connected Android phone and only in USB mode. No USB interface is taken from a BMW driver or program.

## [0.11.0-rc.1] - 2026-11-10 · PC

Release candidate, **tested on PC only**: to be tried in a car before 0.11.0. When installed over 0.10.0, it requires
a head unit restart.

### Fixed

- iDrive blank page: its delays (1, 3, 10 and 15 s) no longer depend on the car's clock, which GPS can reset while
  driving ([#2](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/2)). Tested on PC.
- Reverse: the blank page is no longer requested over the reversing camera when the camera takes over before Android
  Auto moves aside ([#3](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/3)). Tested on PC.

### Changed

- Without a `vehicle` file, the phone sees a BMW "iDrive" instead of an "M340i", the test car's model
  ([#8](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/8)). The `vehicle` file supplies the actual model.
  Tested on PC.
- The `evo-trial` supervisor is now called `aa-supervisor`
  ([#7](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/7)): its program is `/opt/aa/aa-supervisor`,
  and the service's `--trial-seconds` option becomes `--limit-seconds`. Installation step 3 removes the old program.
  Tested on PC.
- The service announces the project version (`VERSION` file) to the phone as the head unit's software version,
  instead of the fixed value `1.3.33` ([#9](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/9)).
  Tested on PC.
- Origins of external data (Android Auto certificate, Google's DHU description, waiting images, car captures) recorded
  in the files and licence. The programs are unchanged.

## [0.10.0] - 2026-11-10 · car

First public release. Tried in a 2019 BMW G20, iDrive software `202111011321`.

### Added

- **Wireless Android Auto** in iDrive: the phone pairs over Bluetooth, then joins the car's Wi-Fi.
- **iDrive app**: an *Android Auto* entry in the Media menu, with Start, Stop, automatic start and help.
  Automatically started Android Auto stays on screen, with the controller having no effect on iDrive.
- **Original controls**: controller, buttons (NAV and MEDIA open Android Auto apps), touch, steering wheel buttons,
  voice button for the Assistant, car microphone.
- **Car GPS** sent to the phone, day and night, driving modes, reversing camera, volume overlay.
- **Guidance on the cluster and head-up display**: arrow, distance, street, roundabout exit.
- **Compatibility check** without modifying anything (`evo-trial --compat`) and **installation procedure** over SSH
  ([docs/INSTALLATION.md](docs/INSTALLATION.md)), with `entry-aad-X.Y.Z-armhf.tar` archive and checksums.
- **Safeguards**: the app moves aside if iDrive crashes several times after loading it; Bluetooth is returned to
  iDrive on shutdown.
- **Tests**: every byte sent to the phone and cluster is compared with a fixed capture, on PC and under QEMU.

### Limitations

- Tried in only one car. The installation procedure was rehearsed with the car's BusyBox under QEMU, but has not yet
  been followed in a car.
- See the [known limitations](ROADMAP.md#limites-connues).

[Unreleased]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/compare/v0.13.0-rc.10...HEAD
[0.13.0-rc.10]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.13.0-rc.10
[0.13.0-rc.9]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.13.0-rc.9
[0.13.0-rc.8]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.13.0-rc.8
[0.13.0-rc.7]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.13.0-rc.7
[0.13.0-rc.6]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.13.0-rc.6
[0.13.0-rc.5]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.13.0-rc.5
[0.13.0-rc.4]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.13.0-rc.4
[0.13.0-rc.3]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.13.0-rc.3
[0.13.0-rc.2]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.13.0-rc.2
[0.13.0-rc.1]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.13.0-rc.1
[0.12.1-rc.4]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.12.1-rc.4
[0.12.1-rc.3]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.12.1-rc.3
[0.12.1-rc.2]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.12.1-rc.2
[0.12.1-rc.1]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.12.1-rc.1
[0.12.0]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.12.0
[0.12.0-rc.12]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.12.0-rc.12
[0.12.0-rc.6]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.12.0-rc.6
[0.12.0-rc.2]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.12.0-rc.2
[0.12.0-rc.1]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.12.0-rc.1
[0.11.0-rc.1]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.11.0-rc.1
[0.10.0]: https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases/tag/v0.10.0
