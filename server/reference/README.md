# Reference data

Offline lookup lists used by `app/reference.py`: operator names, aircraft type
designators, country by ICAO hex range, registration prefixes, military
operators and special liveries.

## Provenance is unresolved

**The origin and licensing of these files is not established.** They were
supplied as a set without sources. That matters because type designators and
operator codes are published by ICAO in Doc 8643 and Doc 8585, over which ICAO
asserts copyright, while equivalent community-maintained lists (OpenSky, ADSB
Exchange, Virtual Radar Server, Wikipedia) carry their own terms, usually
attribution.

Until that is settled, the deliberate position is:

- **Server-side only.** Read at startup by the aggregator and used to enrich
  API responses. Nothing here is compiled into a firmware binary.
- **Not in the USB installer** and not in any published release asset, so
  nothing reaches an end customer as redistributable data.
- **Not presented as ours.** No attribution is claimed either way.

This is why the aircraft silhouette is resolved on the server and sent with
each aircraft, rather than compiled into a lookup table on the device - which
would otherwise have been the simpler design. See the module docstring in
`app/reference.py`.

**Before commercial launch**, establish where each file came from and either
record the attribution the licence requires, replace it with a source whose
terms permit redistribution, or drop it.

## The files

| File | Rows used | Notes |
| --- | --- | --- |
| `Airlines.csv` | 6,008 | ICAO code, name, country, radio telephony |
| `ICAO.txt` | 96 | Richer: adds IATA code and active/defunct status. Applied second, so it wins where the two disagree |
| `ICAOList.csv` | 2,735 | Type designator, class, engine configuration, manufacturer and model. Drives the silhouette. 2,767 rows, less `ZZZZ` and the 31 `ZZZZ-` prefixed rows |
| `ICAOTypeConversion.csv` | 65 | Retired designators to current ones, e.g. CL61 to CL60 |
| `ICAOHexRange.csv` | 189 | Hex address range to country. Preferred over registration prefixes because every aircraft has a hex |
| `RegPrefixList.csv` | 244 | Country to registration prefix. **Not currently used** - the hex ranges cover the same ground more reliably |
| `MilICAOOperatorLookUp.csv` | 107 | Military operator name to ICAO code. Many rows in the file have no code and are skipped |
| `MixedColourSchemes.csv` | 531 | Registration to special livery, mostly JetBlue |
| `ICAO Type List WiP.csv` | 0 | **Not used.** See below |

## Why the WiP type list is not used

`ICAO Type List WiP.csv` is newer and larger than `ICAOList.csv` but is
mid-edit and has lost data:

- 347 rows have a placeholder class and no engine configuration, against 12
  in `ICAOList.csv`.
- It contains a duplicated header row.
- It has 72 fewer unique designators: 78 present in `ICAOList.csv` are
  missing from it, while only 6 are new.

It is kept in the repository so the work in progress is not lost, but
`app/reference.py` reads `ICAOList.csv`. Once the WiP file is finished and
covers at least what the older one does, switch `_load_types()` to it.

## Quirks worth knowing

**`ICAOList.csv` spells LandPlane five ways** - `LandPlane`, `Landplane`,
`Landplace`, `Landplne`, `Landplance`. The class is matched case-insensitively
and the typos are tolerated rather than dropping 29 aircraft.

**Twelve designators carry no class or engine data but are still meaningful** -
`GLID`, `BALL`, `SHIP`, `GYRO`, `UHEL`, `DRON`, `UAV`, `FFLO`, `VFHC`, `PARA`,
`ULAC` - and feeds do send them. Their shapes are stated explicitly in
`GENERIC_DESIGNATOR_SHAPES`. Only `ZZZZ`, "type not yet assigned a
designator", is genuinely no answer.

**31 rows have a `ZZZZ-` prefixed designator**, which is the same "no
designator" marker with a model attached. They can never match what a feed
sends and are skipped.

**No weight column.** A two-jet business jet and a 737 both resolve to the
airliner outline. The ADS-B emitter category does carry weight, and the
firmware still uses it where the type is unknown, but refining this properly
would need data these lists do not have.

**Names are shouted** in `Airlines.csv` ("RYANAIR DAC"). They are converted to
mixed case, except for words of three characters or fewer and words with no
vowel, so `KLM` and `DAC` survive intact rather than becoming "Klm" and "Dac".
