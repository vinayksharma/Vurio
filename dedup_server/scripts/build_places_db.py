#!/usr/bin/env python3
"""
PLACE-1: build the offline gazetteer `geodata/places.sqlite` from GeoNames.

    python3 scripts/build_places_db.py                # downloads to a temp dir
    python3 scripts/build_places_db.py --dump-dir DIR # reuse downloaded dumps
    python3 scripts/build_places_db.py --out geodata/places.sqlite --min-population 0

Inputs (GeoNames, CC BY 4.0 — https://www.geonames.org/):
    allCountries.zip      every feature on Earth (13.5M rows, ~400 MB zip)
    admin1CodesASCII.txt  first-order admin division names ("US.WY" -> Wyoming)
    countryInfo.txt       country names + geonameids
    featureCodes_en.txt   feature-code labels

Output schema (read-only at runtime; opened by OfflineReverseGeocoder):
    meta(key TEXT PRIMARY KEY, value TEXT)
    fcodes(id INTEGER PRIMARY KEY, code TEXT UNIQUE, kind TEXT, label TEXT)
    countries(cc TEXT PRIMARY KEY, name TEXT, geonameid INTEGER)
    admin1(code TEXT PRIMARY KEY, name TEXT, geonameid INTEGER)   -- "US.WY"
    places(cell INTEGER, id INTEGER, name TEXT, ascii TEXT, lat INTEGER,
           lon INTEGER, fcode INTEGER, cc TEXT, admin1 TEXT, population INTEGER,
           PRIMARY KEY (cell, id)) WITHOUT ROWID
        cell  = floor((lat+90)*100) * 36000 + floor((lon+180)*100)  (0.01° grid)
        lat/lon in 1e-5 degrees; ascii = lowercase ASCII name, NULL when it
        equals lower(name)
    idx_places_name  ON places(name COLLATE NOCASE)
    idx_places_ascii ON places(ascii) WHERE ascii IS NOT NULL
    shore(cell INTEGER, kind TEXT, lat1, lon1, lat2, lon2 INTEGER)   -- PLACE-4
        Natural Earth 10 m coastline / lake shores / river centrelines as
        line segments (1e-5°), bucketed by 0.1° cell:
        cell = floor((lat+90)*10) * 3600 + floor((lon+180)*10); a segment is
        stored under every cell its bbox touches.
    idx_shore_cell ON shore(cell)

`--update-shore` rebuilds only the shore table inside an existing output file
(Natural Earth GeoJSON from github.com/nvkelso/natural-earth-vector; public
domain).

The curated keep-list (spec §3.1) drops streams, wells, farms, schools,
churches, hotels, cemeteries, generic buildings, roads, undersea and
vegetation features, and historical/abandoned places: 13.5M -> ~9.1M rows,
~730 MB. Everything a photo can plausibly be "at" or "near" stays.
"""

import argparse
import csv
import io
import os
import sqlite3
import sys
import tempfile
import time
import urllib.request
import zipfile

GEONAMES = "https://download.geonames.org/export/dump/"
NATURAL_EARTH = "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/"
SHORE_FILES = {
    "coast": "ne_10m_coastline.geojson",
    "lake": "ne_10m_lakes.geojson",
    "river": "ne_10m_rivers_lake_centerlines.geojson",
}
SHORE_LON_STRIDE = 3600

CELL_LON_STRIDE = 36000

# GeoNames feature codes kept per class. "*" keeps the whole class.
KEEP = {
    "P": {"*"},          # populated places; historical/abandoned dropped below
    "A": {"*"},          # admin divisions; historical dropped below
    "T": {"*"},          # terrain: mountains, hills, islands, valleys, beaches...
    "L": {"PRK", "RES", "RESN", "RESW", "RESF", "RESH", "RESA", "AREA", "LCTY",
          "BTL", "CST", "PRT", "ZN", "RGN", "CONT", "RGNL", "RGNE", "RGNH",
          "CTRB", "AMUS", "MVA"},
    "H": {"LK", "LKS", "FLLS", "OVF", "BAY", "BCH", "GLCR", "SPNG", "RSV", "PND",
          "LGN", "STR", "INLT", "FJD", "CHN", "HBR", "RF", "SEA", "OCN", "GULF",
          "SD", "LKN", "HTSP", "GYSR", "ANCH", "COVE", "CNYN", "DLTA", "ESTY",
          "LKI", "MOOR", "SBKH", "SHOL", "SND", "STRT", "WTRF"},
    "S": {"MNMT", "HSTS", "MUS", "CSTL", "PAL", "TOWR", "LTHSE", "BDG", "ZOO",
          "AMTH", "STDM", "UNIV", "ARCH", "CAVE", "RUIN", "TMPL", "PGDA", "SHRN",
          "AIRP", "ANS", "OBS", "OBPT", "PYR", "PYRS", "SQR", "THTR", "GDN",
          "MAR", "PIER", "WALL", "WALLA", "SPA", "RECR", "RECG", "RSRT", "CMP",
          "STNB", "RSTN", "PRT", "CTRR", "BTYD", "FT"},
    "R": {"TRL", "PRMN"},
    # U (undersea) and V (vegetation) are dropped entirely.
}
DROP_P = {"PPLW", "PPLQ", "PPLH"}   # destroyed / abandoned / historical

# Role each feature class plays in a label / search (see OfflineReverseGeocoder).
def kind_for(cls, code):
    if cls == "P":
        return "locality"
    if cls == "A":
        if code in ("PCLI", "PCLD", "PCLF", "PCLS", "PCL", "PCLIX", "TERR"):
            return "country"
        if code == "ADM1":
            return "region"
        if code == "ADM2":
            return "county"
        return "district"
    if cls == "L":
        if code in ("PRK", "RES", "RESN", "RESW", "RESF", "RESH", "RESA"):
            return "park"
        return "area"
    if cls == "H":
        return "water"
    if cls == "T":
        return "terrain"
    if cls == "S":
        return "landmark"
    if cls == "R":
        return "trail"
    return "other"


def keep(cls, code):
    codes = KEEP.get(cls)
    if not codes:
        return False
    if cls == "P" and code in DROP_P:
        return False
    if cls == "A" and code.endswith("H"):
        return False
    return "*" in codes or code in codes


def fetch(url, dest):
    if os.path.exists(dest):
        print(f"  reuse {dest}")
        return dest
    print(f"  download {url}")
    urllib.request.urlretrieve(url, dest)
    return dest


def shore_lat_idx(lat):
    return min(max(int((lat + 90.0) * 10.0), 0), 1799)


def shore_lon_idx(lon):
    return min(max(int((lon + 180.0) * 10.0), 0), SHORE_LON_STRIDE - 1)


def shore_cells(lat1, lon1, lat2, lon2):
    """Every 0.1° cell the segment's bbox touches (interior cells included:
    the runtime scans only ±1 cell, so a long straight reach must be
    findable from its middle, not just from its endpoints)."""
    la0, la1 = sorted((shore_lat_idx(lat1), shore_lat_idx(lat2)))
    lo0, lo1 = sorted((shore_lon_idx(lon1), shore_lon_idx(lon2)))
    for li in range(la0, la1 + 1):
        for lo in range(lo0, lo1 + 1):
            yield li * SHORE_LON_STRIDE + lo


def iter_lines(geometry):
    """Yield coordinate lists (each a polyline) from any GeoJSON geometry."""
    if not geometry:
        return
    t, c = geometry["type"], geometry["coordinates"]
    if t == "LineString":
        yield c
    elif t == "MultiLineString":
        for line in c:
            yield line
    elif t == "Polygon":
        for ring in c:
            yield ring
    elif t == "MultiPolygon":
        for poly in c:
            for ring in poly:
                yield ring


def build_shore(db, dump_dir):
    """PLACE-4: shoreline segments for distance-to-water."""
    import json
    db.executescript(
        "DROP TABLE IF EXISTS shore;"
        "CREATE TABLE shore(cell INTEGER NOT NULL, kind TEXT NOT NULL,"
        " lat1 INTEGER NOT NULL, lon1 INTEGER NOT NULL, lat2 INTEGER NOT NULL, lon2 INTEGER NOT NULL);")
    total = 0
    for kind, fname in SHORE_FILES.items():
        path = fetch(NATURAL_EARTH + fname, os.path.join(dump_dir, fname))
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        rows = []
        for feat in data.get("features", []):
            for line in iter_lines(feat.get("geometry")):
                for (lon1, lat1), (lon2, lat2) in zip(line, line[1:]):
                    if lon1 == lon2 and lat1 == lat2:
                        continue
                    for cell in shore_cells(lat1, lon1, lat2, lon2):
                        rows.append((cell, kind, int(round(lat1 * 1e5)), int(round(lon1 * 1e5)),
                                     int(round(lat2 * 1e5)), int(round(lon2 * 1e5))))
        db.executemany("INSERT INTO shore VALUES (?,?,?,?,?,?)", rows)
        print(f"  shore/{kind}: {len(rows):,} segment rows")
        total += len(rows)
    db.execute("CREATE INDEX idx_shore_cell ON shore(cell)")
    db.execute("INSERT OR REPLACE INTO meta VALUES ('shore_rows', ?)", (str(total),))
    db.execute("INSERT OR REPLACE INTO meta VALUES ('shore_source', 'Natural Earth 10m (public domain)')")
    db.commit()
    return total


def cell_of(lat, lon):
    lat_idx = min(max(int((lat + 90.0) * 100.0), 0), 17999)
    lon_idx = min(max(int((lon + 180.0) * 100.0), 0), CELL_LON_STRIDE - 1)
    return lat_idx * CELL_LON_STRIDE + lon_idx


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump-dir", help="directory holding the GeoNames dumps (downloaded if missing)")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "geodata", "places.sqlite"))
    ap.add_argument("--min-population", type=int, default=0,
                    help="drop populated places below this population (0 keeps hamlets; the default)")
    ap.add_argument("--update-shore", action="store_true",
                    help="only (re)build the shore table inside the existing --out file")
    args = ap.parse_args()

    dump_dir = args.dump_dir or tempfile.mkdtemp(prefix="geonames_")
    os.makedirs(dump_dir, exist_ok=True)
    print(f"dumps in {dump_dir}")
    if args.update_shore:
        out = os.path.abspath(args.out)
        if not os.path.exists(out):
            print(f"{out} does not exist; build it first", file=sys.stderr)
            return 1
        db = sqlite3.connect(out)
        n = build_shore(db, dump_dir)
        db.close()
        print(f"updated shore table in {out}: {n:,} rows")
        return 0
    all_zip = fetch(GEONAMES + "allCountries.zip", os.path.join(dump_dir, "allCountries.zip"))
    admin1_txt = fetch(GEONAMES + "admin1CodesASCII.txt", os.path.join(dump_dir, "admin1CodesASCII.txt"))
    country_txt = fetch(GEONAMES + "countryInfo.txt", os.path.join(dump_dir, "countryInfo.txt"))
    fcodes_txt = fetch(GEONAMES + "featureCodes_en.txt", os.path.join(dump_dir, "featureCodes_en.txt"))

    out = os.path.abspath(args.out)
    tmp_out = out + ".building"
    if os.path.exists(tmp_out):
        os.remove(tmp_out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    db = sqlite3.connect(tmp_out)
    db.executescript(
        """
        PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; PRAGMA page_size=4096;
        CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE fcodes(id INTEGER PRIMARY KEY, code TEXT UNIQUE, kind TEXT NOT NULL, label TEXT);
        CREATE TABLE countries(cc TEXT PRIMARY KEY, name TEXT NOT NULL, geonameid INTEGER);
        CREATE TABLE admin1(code TEXT PRIMARY KEY, name TEXT NOT NULL, geonameid INTEGER);
        CREATE TABLE places(
            cell INTEGER NOT NULL, id INTEGER NOT NULL,
            name TEXT NOT NULL, ascii TEXT,
            lat INTEGER NOT NULL, lon INTEGER NOT NULL,
            fcode INTEGER NOT NULL, cc TEXT, admin1 TEXT, population INTEGER,
            PRIMARY KEY (cell, id)) WITHOUT ROWID;
        """
    )

    labels = {}
    with open(fcodes_txt, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 2:
                labels[parts[0]] = parts[1]

    with open(country_txt, encoding="utf-8") as f:
        rows = []
        for line in f:
            if line.startswith("#"):
                continue
            p = line.rstrip("\n").split("\t")
            if len(p) > 16 and p[0] and p[4]:
                rows.append((p[0], p[4], int(p[16]) if p[16].isdigit() else None))
        db.executemany("INSERT OR REPLACE INTO countries VALUES (?,?,?)", rows)
    with open(admin1_txt, encoding="utf-8") as f:
        rows = []
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) >= 4:
                rows.append((p[0], p[1], int(p[3]) if p[3].isdigit() else None))
        db.executemany("INSERT OR REPLACE INTO admin1 VALUES (?,?,?)", rows)

    fcode_ids = {}

    def fcode_id(cls, code):
        key = f"{cls}.{code}"
        fid = fcode_ids.get(key)
        if fid is None:
            fid = len(fcode_ids) + 1
            fcode_ids[key] = fid
            db.execute("INSERT INTO fcodes VALUES (?,?,?,?)", (fid, key, kind_for(cls, code), labels.get(key)))
        return fid

    print("loading allCountries ...")
    t0 = time.time()
    total = kept = 0

    def rows():
        nonlocal total, kept
        with zipfile.ZipFile(all_zip) as z:
            with z.open("allCountries.txt") as raw:
                reader = csv.reader(io.TextIOWrapper(raw, encoding="utf-8", newline=""),
                                    delimiter="\t", quoting=csv.QUOTE_NONE)
                for x in reader:
                    total += 1
                    # 0 id,1 name,2 ascii,3 alt,4 lat,5 lon,6 fclass,7 fcode,8 cc,
                    # 9 cc2,10 admin1,11 admin2,12 admin3,13 admin4,14 pop,15 elev
                    cls, code = x[6], x[7]
                    if not keep(cls, code):
                        continue
                    pop = int(x[14]) if x[14] else 0
                    if cls == "P" and pop < args.min_population:
                        continue
                    name = x[1]
                    if not name:
                        continue
                    lat, lon = float(x[4]), float(x[5])
                    ascii_name = x[2].lower()
                    if not ascii_name or ascii_name == name.lower():
                        ascii_name = None
                    kept += 1
                    if kept % 1_000_000 == 0:
                        print(f"  {kept:,} kept of {total:,} ({time.time() - t0:.0f}s)")
                    yield (cell_of(lat, lon), int(x[0]), name, ascii_name,
                           int(round(lat * 1e5)), int(round(lon * 1e5)),
                           fcode_id(cls, code), x[8] or None, x[10] or None, pop or None)

    db.executemany("INSERT OR IGNORE INTO places VALUES (?,?,?,?,?,?,?,?,?,?)", rows())
    db.commit()
    print(f"  {kept:,} of {total:,} rows kept in {time.time() - t0:.0f}s; indexing ...")
    db.execute("CREATE INDEX idx_places_name ON places(name COLLATE NOCASE)")
    db.execute("CREATE INDEX idx_places_ascii ON places(ascii) WHERE ascii IS NOT NULL")
    db.commit()
    print("building shore table ...")
    build_shore(db, dump_dir)
    db.executemany("INSERT INTO meta VALUES (?,?)", [
        ("schema_version", "1"),
        ("source", "GeoNames allCountries " + time.strftime("%Y-%m-%d", time.gmtime(os.path.getmtime(all_zip)))),
        ("license", "CC BY 4.0 — https://www.geonames.org/ (attribution required)"),
        ("built_at", time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())),
        ("rows", str(kept)),
        ("min_population", str(args.min_population)),
    ])
    db.commit()
    db.execute("VACUUM")
    db.close()
    os.replace(tmp_out, out)
    print(f"wrote {out}: {os.path.getsize(out) / 1e6:.0f} MB, {kept:,} places")
    return 0


if __name__ == "__main__":
    sys.exit(main())
