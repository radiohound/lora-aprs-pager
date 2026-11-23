# Registering a tocall (draft, not yet submitted)

The pager transmits as the experimental `APZPGR`. A registered code goes in the APRS device-ID list at https://github.com/aprsorg/aprs-deviceid
(`tocalls.yaml`) by pull request. As of 2026-11-23 `APLPG?` is not taken (the neighbours are `APLP0?`, `APLP1?`, `APLPIK` and `APLPS?`).
The `?` stands for a version or variant character, so the pager would transmit as, for example, `APLPG0`.

Draft entry (same layout as the neighbouring entries; fill in the contact before submitting):

     - tocall: APLPG?
       vendor: Walter Dunckel, K6ATV
       model: LoRa APRS Pager (T-LoRa Pager tracker, map and messenger)
       class: tracker
       os: embedded
       contact: <contact email or URL>

When it is accepted, change `TOCALL` in `src/app/app.cpp` to `"APLPG0"` and update the README.
