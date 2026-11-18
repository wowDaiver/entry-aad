# Vehicle Telemetry fixtures

Eight short owner-recorded read-only HSFZ capture groups, retained as independent test cases.
Only fixed engine/transmission telemetry and F150 profile identifiers remain: ECU/DID, request bytes,
response frames, status, length and response data/NRC. Interface details, host identities, conditions,
wall-clock times, interpreted values and unrelated records are omitted. Each fixture records the SHA256
of its private source capture for provenance. No VIN, MAC, location, password or firmware binary is included.

The menu fixture contains only reverse-engineered RHMI placement IDs, not BMW XML or artwork.
Definitions were checked against D83BX7C0/GSZFB1 in `emdzej/ediabasx-docs-sgbd`, commit
`b644de8fbfbb4b207f57794e3c7894dc1dc58627`. Positive replies only establish the captured layouts;
the source map still rejects wrong identities/lengths and does not invent unknown measurements.
