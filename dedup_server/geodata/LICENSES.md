# Geodata licenses

## places.sqlite (not committed — build with `scripts/build_places_db.py`)

Derived from the **GeoNames** gazetteer (https://www.geonames.org/), which is
licensed under the Creative Commons Attribution 4.0 License
(https://creativecommons.org/licenses/by/4.0/). Attribution: "Place data ©
GeoNames, CC BY 4.0". The build script downloads `allCountries.zip`,
`admin1CodesASCII.txt`, `countryInfo.txt` and `featureCodes_en.txt` at build
time on the developer machine; the end-user application never downloads
gazetteer data and never sends coordinates anywhere — reverse geocoding is
entirely offline.

## world_locations.geojson

Distilled from **Natural Earth** (https://www.naturalearthdata.com/), which
is in the public domain. No attribution required.
