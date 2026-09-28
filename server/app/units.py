"""Conversions between SI and the units this pipeline speaks.

Aviation data here is feet, knots and feet per minute, because that is what
adsb.fi, adsb.lol and airplanes.live report, what the firmware's field
contract expects, and what the panel prints. Two upstreams are SI instead -
OpenSky state vectors and SondeHub balloon telemetry - and merging either
unconverted puts an airliner at "11,000 ft" against another source's
36,000, or a balloon at 30,000 ft when it is at 30,000 metres.

One place for the factors, because the bug they prevent is silent: a
wrong constant produces a plausible number, and plausible numbers do not
get noticed.
"""

METRES_TO_FEET = 3.280839895
MPS_TO_KNOTS = 1.943844
MPS_TO_FEET_PER_MINUTE = 196.8503937
