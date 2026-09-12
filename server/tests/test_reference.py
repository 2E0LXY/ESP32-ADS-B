"""Offline operator, type, country and livery lookups.

The silhouette mapping is the valuable part, so it is checked against real
designators with known answers rather than only for "returns something".
"""

import pytest

from app.reference import ReferenceData, shape_for_type

# Loading parses five files; once for the whole module rather than per test.
@pytest.fixture(scope="module")
def reference():
    data = ReferenceData("reference")
    data.load()
    return data


def test_the_lists_actually_loaded(reference):
    """A path mistake would otherwise degrade silently to no enrichment."""
    assert reference.stats()["airlines"] > 5000
    assert reference.stats()["types"] > 2500
    assert reference.stats()["hex_ranges"] > 150
    assert reference.stats()["liveries"] > 400


@pytest.mark.parametrize("designator,shape", [
    ("A388", "heavy"),        # four jets
    ("B744", "heavy"),
    ("B738", "airliner"),     # two jets
    ("A320", "airliner"),
    ("DH8D", "twin"),         # two turboprops
    ("AT76", "twin"),
    ("C172", "light"),        # single piston
    ("P28A", "light"),   # PA28 is not a designator; the Cherokee is P28A
    ("EC35", "helicopter"),
    ("R44", "helicopter"),
    ("F16", "fighter"),       # single jet
    ("GLID", "glider"),       # generic designators, no class or engine data
    ("BALL", "balloon"),
    ("GYRO", "helicopter"),
    ("UAV", "drone"),
    ("ULAC", "light"),
])
def test_real_designators_get_the_right_silhouette(reference, designator, shape):
    info = reference.aircraft_type(designator)
    assert info is not None, f"{designator} is missing from the type list"
    assert info["shape"] == shape


def test_a_single_turboprop_is_a_light_aircraft_not_a_regional_twin():
    """A PC-12 or TBM reads as light. Mapping every turboprop to the twin
    outline put a regional airliner shape on a single-engine aeroplane."""
    assert shape_for_type("LandPlane", "1/Turboprop/Turboshaft") == "light"
    assert shape_for_type("LandPlane", "2/Turboprop/Turboshaft") == "twin"


def test_rotorcraft_beat_engine_count():
    """A helicopter is a helicopter whatever it is powered by."""
    for klass in ("Helicopter", "Gyrocopter", "Tiltrotor", "helicopter"):
        assert shape_for_type(klass, "2/Jet") == "helicopter"


def test_the_class_spelling_mistakes_are_tolerated():
    """ICAOList.csv spells LandPlane four ways, including 'Landplace' and
    'Landplne'. Dropping those rows would lose 29 aircraft."""
    for klass in ("LandPlane", "Landplane", "Landplace", "Landplne", "Landplance"):
        assert shape_for_type(klass, "2/Jet") == "airliner"


def test_no_engine_data_means_no_opinion():
    """None, not a guess: the device's own category-based fallback is better
    than inventing a shape from nothing."""
    assert shape_for_type("0", "/") is None
    assert shape_for_type("LandPlane", "") is None


def test_retired_type_codes_still_resolve(reference):
    """An older feed still sends CL61, which ICAO replaced with CL60."""
    assert reference.aircraft_type("CL61") == reference.aircraft_type("CL60")
    assert reference.aircraft_type("CL61") is not None


def test_operator_comes_from_the_callsign_prefix(reference):
    assert reference.airline_for_callsign("RYR2BH")["name"].startswith("Ryanair")
    assert reference.airline_for_callsign("KLM43E")["iata"] == "KL"
    # A bare three-letter code is not a flight.
    assert reference.airline_for_callsign("RYR") is None
    assert reference.airline_for_callsign("") is None
    assert reference.airline_for_callsign(None) is None


def test_country_comes_from_the_hex_address(reference):
    """Works for every aircraft, unlike a registration prefix."""
    assert reference.country_for_hex("4ca2d5") == "Ireland"
    assert reference.country_for_hex("400a1b") == "United Kingdom"
    assert reference.country_for_hex("3c4b26") == "Germany"
    assert reference.country_for_hex("a12345") == "United States"
    # Unallocated ranges are not an answer, and neither is rubbish.
    assert reference.country_for_hex("000001") is None
    assert reference.country_for_hex("not-hex") is None
    assert reference.country_for_hex(None) is None


def test_enrichment_fills_gaps_without_overwriting_the_feed(reference):
    out = reference.enrich({"hex": "4ca2d5", "flight": "RYR2BH", "t": "B738"})
    assert out["shape"] == "airliner"
    assert out["type_name"] == "Boeing 737-800"
    assert out["cou"] == "Ireland"
    assert out["ownOp"].startswith("Ryanair")

    # An upstream that knows the registered operator knows better than a
    # lookup by callsign prefix.
    kept = reference.enrich({"hex": "4ca2d5", "flight": "RYR2BH", "t": "B738",
                             "ownOp": "Ryanair UK", "cou": "United Kingdom"})
    assert kept["ownOp"] == "Ryanair UK"
    assert kept["cou"] == "United Kingdom"


def test_enrichment_leaves_an_unknown_aircraft_alone(reference):
    original = {"hex": "zzzzzz", "flight": "QQQ9", "t": "NOPE"}
    assert reference.enrich(dict(original)) == original


def test_the_unassigned_designator_placeholder_is_not_a_model(reference):
    """ICAOList.csv has a row reading "type not (yet) assigned a designator",
    which was being reported as an aircraft's model. The 31 ZZZZ- prefixed
    rows are the same thing and can never match what a feed sends."""
    assert reference.aircraft_type("ZZZZ") is None
    assert reference.aircraft_type("ZZZZ-SG26") is None
    assert not [code for code in reference.types if code.startswith("ZZZZ")]
    out = reference.enrich({"hex": "aabbcc", "flight": "TEST1", "t": "ZZZZ"})
    assert "type_name" not in out


def test_acronyms_are_not_mangled(reference):
    """Plain capitalisation turned KLM into "Klm" and DAC into "Dac"."""
    assert "KLM" in reference.airlines["KLM"]["name"]
    assert reference.airlines["RYR"]["name"].endswith("DAC")


def test_a_special_livery_is_reported(reference):
    out = reference.enrich({"hex": "a12345", "flight": "JBU123", "t": "A320", "r": "N197JB"})
    assert out["livery"] == "Tartan"


def test_missing_files_degrade_rather_than_raise(tmp_path):
    """A path mistake in a deployment must not stop the service booting."""
    empty = ReferenceData(str(tmp_path))
    empty.load()
    assert empty.stats()["airlines"] == 0
    assert empty.enrich({"hex": "4ca2d5", "flight": "RYR2BH"}) == {"hex": "4ca2d5", "flight": "RYR2BH"}
