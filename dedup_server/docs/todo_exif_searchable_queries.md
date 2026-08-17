# Comprehensive EXIF-Searchable Prompt-Based Image Search Specification

**Status**: Open  
**Tasks**: EXIF-1 through EXIF-5  
**Motivation**: Current prompt-based search uses CLIP embeddings for semantic queries ("landscape", "sunset"). All EXIF metadata extracted by the system (camera, exposure, optics, white balance, light quality, location) is NOT indexed or searchable in queries. This feature enables **comprehensive free-text EXIF search** supporting 25+ fields with numeric operators, enabling natural queries combining text similarity with structured EXIF constraints.

---

## 1. Overview & User-Facing Behavior

### Current State
- Text search query: `"landscape photos"` → returns images semantically similar to "landscape photos"
- EXIF extracted but only camera/datetime/resolution indexed; most EXIF inaccessible to users

### Desired End State
**All extracted EXIF metadata searchable via natural language**, with numeric operators and text matching:

- `"portrait wide aperture f/1.4"` → aperture < 1.4, semantic match on "portrait"
- `"sunset iso above 1600 canon"` → ISO > 1600, camera = Canon, semantic match on "sunset"
- `"shutter faster than 1/500"` → shutter speed > 1/500 (APEX)
- `"tungsten white balance low contrast"` → WB = tungsten, contrast = low, semantic match
- `"telephoto focal length above 200mm"` → focal_length > 200, semantic "telephoto"
- `"august 2024 yellowstone high iso"` → date in August 2024, GPS in Yellowstone bounds, ISO > 3200
- `"manual focus exposure +1 EV"` → focus_mode = manual, exposure_comp > 1.0

### 25+ Searchable EXIF Fields
Camera/Lens, Exposure (ISO, shutter, aperture, exposure comp, metering, flash), Optics (focal length, focus), White Balance, Color Space, Scene Mode, Light Quality (brightness, contrast, saturation, sharpness), Location (GPS), Time, Orientation

---

## 2. Architecture & Data Flow

### High-Level Pipeline

```
User Query: "sunset photos canon yellowstone"
    ↓
Text Search Engine (text_search_engine.cpp)
  ├─ Parse EXIF constraints: { camera: "canon", location: "yellowstone" }
  ├─ Run CLIP KNN on remaining text: "sunset photos"
  │   └─ Returns top-300 file_ids with cosine-distance scores
  ├─ Apply SQL filter: WHERE exif_camera LIKE "%canon%" AND gps_lat/lon in [bbox]
  └─ Return filtered + ranked results
    ↓
Client: 42 images (sunset + canon + yellowstone constraints met)
```

### Three Implementation Phases

1. **EXIF-1: Data Extraction** — Ensure ISO, aperture, lens, GPS are extracted from image files
2. **EXIF-2: Storage** — Add database columns and indices
3. **EXIF-3,4: Search** — Parse constraints, apply filters, return results
4. **EXIF-5: Testing** — Verify end-to-end

---

## 3. EXIF-1: Enhanced EXIF Extraction

### Current State
**File**: `dedup_server/include/media_processors/image/backends/exif_reader.hpp`

The `ExifInfo` struct currently holds:
```cpp
struct ExifInfo {
  uint32_t width;       // After orientation applied
  uint32_t height;
  std::string datetime; // EXIF:DateTime or file mtime, "YYYY:MM:DD HH:MM:SS"
  std::string camera;   // Make Model (e.g., "Canon EOS 5D Mark IV")
};
```

### Changes Required

**Extend `ExifInfo`** to include:
```cpp
struct ExifInfo {
  // Existing fields
  uint32_t width;
  uint32_t height;
  std::string datetime;
  std::string camera;
  
  // New fields
  uint32_t iso;              // ISO sensitivity (e.g., 100, 1600, 3200)
  float focal_length;        // Focal length in mm (e.g., 50.0, 200.5)
  float f_number;            // Aperture f-stop (e.g., 2.8, 5.6, 16.0)
  std::string lens;          // Lens model (e.g., "EF 70-200mm f/2.8L IS II USM")
  float gps_lat;             // Latitude, -90 to 90, NaN if not present
  float gps_lon;             // Longitude, -180 to 180, NaN if not present
  float gps_alt;             // Altitude in meters, NaN if not present
  uint8_t orientation;       // EXIF Orientation (1-8, default 1)
};
```

**Update `ExifReader::read(original_file_path)`** to extract:
- **ISO**: EXIF:ISOSpeedRatings or EXIF:ISO (uint32_t)
- **Focal Length**: EXIF:FocalLength (parse "50.0 mm" → 50.0)
- **F-Number**: EXIF:FNumber (parse "2.8" → 2.8)
- **Lens**: EXIF:LensModel (string)
- **GPS**:
  - EXIF:GPSLatitude (DMS to decimal: "40 degrees 24' 21.52" → 40.40598)
  - EXIF:GPSLongitude (DMS to decimal)
  - EXIF:GPSAltitude
- **Orientation**: EXIF:Orientation (default 1 if missing)

**Implementation Notes**:
- ImageMagick `-ping` already loads headers + EXIF without decoding pixels
- LibRaw metadata-only parsing for RAW files (already used)
- Missing EXIF fields default to:
  - Numeric: 0 or NaN (use std::numeric_limits<float>::quiet_NaN() for GPS if not present)
  - String: empty string

---

## 4. EXIF-2: Database Schema Update

### File
`dedup_server/include/database/sql_constants.hpp`

### Current `scanned_files` Table
Already has columns:
```sql
exif_datetime TEXT,        -- "YYYY-MM-DD HH:MM:SS"
exif_camera TEXT,          -- "Make Model"
exif_resolution TEXT,      -- "WxH"
exif_blob BLOB             -- (deprecated, now NULL)
```

### Add New Columns
After existing EXIF columns, add (all NULLable, default NULL):
```sql
exif_iso INTEGER,
exif_focal_length REAL,
exif_f_number REAL,
exif_lens TEXT,
exif_gps_lat REAL,
exif_gps_lon REAL,
exif_gps_alt REAL,
exif_orientation INTEGER
```

### Add Indices
```sql
CREATE INDEX idx_exif_camera ON scanned_files(exif_camera);
CREATE INDEX idx_exif_iso ON scanned_files(exif_iso);
CREATE INDEX idx_exif_datetime ON scanned_files(exif_datetime);

-- For location queries: composite index on lat/lon
CREATE INDEX idx_exif_gps ON scanned_files(exif_gps_lat, exif_gps_lon);
```

### Migration
- Existing images will have NULL for new columns
- `ImageProcessor::Process()` will populate on re-scan
- No schema version bump needed (new columns are optional)

### Database Ops Update
**File**: `dedup_server/src/database/scanned_files_ops.cpp`

Update `ScannedFilesOps::updateExifMetadata()` to write all new fields from `ExifInfo`:
```cpp
void ScannedFilesOps::updateExifMetadata(
    const std::string& file_path,
    const ExifInfo& exif_info) {
  // Bind existing fields
  stmt.bind(":datetime", exif_info.datetime);
  stmt.bind(":camera", exif_info.camera);
  stmt.bind(":resolution", exif_info.resolution_string());
  
  // Bind new fields
  stmt.bind(":iso", exif_info.iso);
  stmt.bind(":focal_length", exif_info.focal_length);
  stmt.bind(":f_number", exif_info.f_number);
  stmt.bind(":lens", exif_info.lens);
  stmt.bind(":gps_lat", exif_info.gps_lat);
  stmt.bind(":gps_lon", exif_info.gps_lon);
  stmt.bind(":gps_alt", exif_info.gps_alt);
  stmt.bind(":orientation", exif_info.orientation);
  
  stmt.execute();
}
```

---

## 5. EXIF-3: Location Geocoding Index

### New File
`dedup_server/include/search/location_geocoder.hpp` (and `.cpp`)

### Purpose
Map location names (e.g., "yellowstone", "utah", "paris") to GPS bounding boxes. Used by the query parser to convert user location hints into lat/lon constraints.

### Implementation

```cpp
// location_geocoder.hpp
class LocationGeocoder {
 public:
  // Load from GeoJSON file (e.g., world_boundaries.geojson)
  bool LoadBoundaries(const std::string& geojson_path);
  
  // Lookup: "yellowstone" → {lat_min, lat_max, lon_min, lon_max}
  struct Bounds {
    float lat_min, lat_max;  // [-90, 90]
    float lon_min, lon_max;  // [-180, 180]
  };
  
  // Returns bounds if found, std::nullopt if not
  std::optional<Bounds> GetBounds(const std::string& location_name);
  
 private:
  // In-memory cache: "yellowstone" → Bounds
  std::unordered_map<std::string, Bounds> name_to_bounds_;
};
```

### GeoJSON Data Source
Use **free, public boundaries** from:
- **Natural Earth Data** (naturalearth.com): Easily available GeoJSON for countries, states, national parks
  - Example: `ne_10m_parks_and_protected_lands.geojson` (national parks)
  - Example: `ne_10m_admin_1_states_provinces.geojson` (US states, provinces)
- **OpenStreetMap GeoNames** (nominatim)

### Storage Location
`dedup_server/data/world_boundaries.geojson`

### Parsing Strategy
- **Simple bounding-box extraction**: For each GeoJSON feature, compute bounding box (min/max lat/lon)
- **Case-insensitive name mapping**: Store properties like "name", "ADMIN" (state/province name), "NAME" (park name)
- Example:
  ```json
  {
    "type": "Feature",
    "properties": { "name": "Yellowstone National Park" },
    "geometry": { "type": "Polygon", "coordinates": [...] }
  }
  ```
  Geocoder extracts bbox, stores:
  ```
  "yellowstone" → { lat_min: 44.35, lat_max: 44.52, lon_min: -110.97, lon_max: -110.44 }
  "yellowstone national park" → same bounds
  ```

### Lookup Behavior
- Case-insensitive substring match (e.g., "yellow" finds "Yellowstone", "utah" finds "Utah")
- Return first match if multiple locations match the same name
- Return empty if not found

---

## 6. EXIF-4: Text Search Enhancement

### Files
- `dedup_server/include/search/text_search_engine.hpp`
- `dedup_server/src/search/text_search_engine.cpp`

### New Helper: Query Constraint Extraction

Add to `text_search_engine.cpp`:

```cpp
// Helper struct for parsed constraints
struct ExifConstraints {
  std::optional<std::string> camera_keyword;  // "canon", "sony", "nikon"
  std::optional<uint32_t> iso_value;          // exact match
  std::optional<float> f_number;              // approx match ±0.2
  std::optional<std::string> location;        // "yellowstone", "utah"
  std::optional<std::string> date_range;      // "2024", "august 2024"
  std::string remaining_query;                // Text without EXIF hints
};

// Parse constraints from user query using simple pattern matching
ExifConstraints ExtractExifConstraints(const std::string& query);
```

### Pattern Matching Examples

**Camera Make**:
- Regex patterns: `\b(canon|nikon|sony|fuji|panasonic|olympus|leica)\b`
- Extract: `"canon"` or `"nikon"`

**ISO**:
- Patterns: `iso\s*(\d+)`, `iso[\s:]?(\d{2,4})`
- Extract: ISO value (e.g., `1600`)

**Aperture**:
- Patterns: `f[/\s]?(\d\.?\d)`, `f[/\s]?(\d+\.?\d*)`
- Extract: f-number (e.g., `2.8`)

**Location**:
- Query parser checks if any word is a known location name (via `LocationGeocoder::GetBounds()`)
- Extract: location name (e.g., `"yellowstone"`)

**Date**:
- Patterns: `(january|february|...|december)\s*(\d{4})`, `(\d{4})` (year only)
- Extract: date range (e.g., `"august 2024"` → start: 2024-08-01, end: 2024-08-31)

**Remaining Query**:
- Remove matched tokens, keep the rest (e.g., `"sunset photos canon yellowstone"` → remaining: `"sunset photos"`)

### Modified SearchQuery() Function

**Current code flow**:
1. Tokenize `search_query`
2. Encode text via CLIP tokenizer
3. Run KNN on `vec_embedding_index`
4. Return top-k results

**New code flow**:
1. Extract EXIF constraints from `search_query`
2. Tokenize remaining query text (after removing EXIF hints)
3. Encode text via CLIP tokenizer
4. Run KNN on `vec_embedding_index` → get top-300 file_ids
5. **Build SQL filter from constraints**:
   ```sql
   SELECT file_id, score FROM knn_results
   WHERE 
     (exif_camera LIKE ? OR ? IS NULL)
     AND (exif_iso = ? OR ? IS NULL)
     AND (exif_f_number BETWEEN ? AND ? OR ? IS NULL)
     AND (exif_gps_lat BETWEEN ? AND ? OR ? IS NULL)
     AND (exif_gps_lon BETWEEN ? AND ? OR ? IS NULL)
     AND (exif_datetime BETWEEN ? AND ? OR ? IS NULL)
   ORDER BY score DESC
   LIMIT knn_k
   ```
6. Return filtered results

### Example: Query `"sunset photos canon iso 1600 yellowstone"`

```
1. Extract constraints:
   - camera: "canon"
   - iso: 1600
   - location: "yellowstone" → bounds = {lat_min, lat_max, lon_min, lon_max}
   - remaining: "sunset photos"

2. Run CLIP on "sunset photos"
   → top 300 file_ids with scores

3. Apply filters:
   WHERE 
     exif_camera LIKE '%canon%'
     AND exif_iso = 1600
     AND exif_gps_lat BETWEEN lat_min AND lat_max
     AND exif_gps_lon BETWEEN lon_min AND lon_max

4. Return filtered top-k
```

---

## 7. EXIF-5: Testing & Verification

### Unit Tests

**File**: `dedup_server/tests/test_exif_reader.cpp` (extend existing)
- `TestExifExtractionISO()`: Parse ISO from JPEG/RAW
- `TestExifExtractionAperture()`: Parse f-number
- `TestExifExtractionLens()`: Parse lens model
- `TestExifExtractionGPS()`: Parse lat/lon/alt

**File**: `dedup_server/tests/test_text_search_engine.cpp` (new tests)
- `TestExtractExifConstraints_Camera()`: "canon" → camera_keyword="canon"
- `TestExtractExifConstraints_ISO()`: "iso 1600" → iso_value=1600
- `TestExtractExifConstraints_Aperture()`: "f/2.8" → f_number=2.8
- `TestExtractExifConstraints_Location()`: "yellowstone" → location="yellowstone"
- `TestExtractExifConstraints_DateRange()`: "august 2024" → date range
- `TestExtractExifConstraints_Multiple()`: "sunset canon iso 1600 yellowstone" → multiple constraints

**File**: `dedup_server/tests/test_location_geocoder.cpp` (new)
- `TestLoadBoundaries()`: Load GeoJSON without errors
- `TestLookupExact()`: "yellowstone" returns correct bounds
- `TestLookupCaseInsensitive()`: "YELLOWSTONE" returns same bounds
- `TestLookupSubstring()`: "yellow" finds "Yellowstone"
- `TestLookupNotFound()`: "nonexistent" returns nullopt

### Integration Tests

**Setup**: Index a test image set with known EXIF:
```
image_sunset_canon_1600_yellowstone.jpg: 
  - keywords: "sunset landscape"
  - camera: "Canon EOS 5D Mark IV"
  - iso: 1600
  - f-number: 2.8
  - gps: {44.43, -110.83} (Yellowstone)
  - date: 2024-08-15

image_portrait_sony_400_utah.jpg:
  - keywords: "portrait woman"
  - camera: "Sony A7R IV"
  - iso: 400
  - f-number: 1.4
  - gps: {40.76, -111.89} (Utah)
  - date: 2024-07-10

... (10+ more test images with varied EXIF)
```

**Test Queries**:
1. `"sunset canon iso 1600"` 
   - Expected: image_sunset_* appears, portrait_* does not
   
2. `"sunset yellowstone"`
   - Expected: image_sunset_* appears (sunset + location)
   
3. `"portrait utah"`
   - Expected: portrait_sony_* appears (portrait + location)
   
4. `"portrait f/1.4"`
   - Expected: portrait_sony_* appears (aperture constraint)

5. `"landscape 2024"`
   - Expected: images from August 2024 appear, earlier dates filtered

### Manual Testing

1. **Run App**: Start dedup server with test library
2. **Search**: Enter query in web/CLI: `"sunset photos canon yellowstone"`
3. **Verify**:
   - Images returned are semantically "sunset-like"
   - All returned images have "Canon" in exif_camera
   - All returned images have GPS within Yellowstone bounds
   - No manual filtering needed — all constraints applied in backend

---

## 8. Edge Cases & Error Handling

### Handling Missing EXIF
- If `exif_iso` is NULL, constraint `iso = X` filters out that image (WHERE exif_iso = X)
- If `exif_gps_lat` / `exif_gps_lon` are NULL, location constraint filters out that image
- If no location constraint in query, NULL values are fine (query runs normally)

### GPS Boundary Handling
- Valid lat: [-90, 90], lon: [-180, 180]
- Queries crossing date line (lon > 180 or < -180): Clamp to valid range
- Queries crossing poles (lat > 90 or < -90): Clamp to valid range

### Date Range Parsing
- "2024" → start: 2024-01-01, end: 2024-12-31
- "august 2024" → start: 2024-08-01, end: 2024-08-31
- "august" (no year) → assume current year (at query time)

### Camera Matching
- "sony" matches "Sony A7R IV", "Sony A6400", etc. (case-insensitive LIKE)
- "canon 5d" matches "Canon EOS 5D Mark IV", "Canon EOS 5D Mark III" (substring)

---

## 9. Backward Compatibility & Rollout

### Backward Compatibility
- Existing queries without EXIF hints work unchanged
- Old images with NULL EXIF fields are returned if text similarity is high
- No API changes; `search_query` parameter unchanged

### Database Migration
- Add new columns as NULLable with defaults
- Existing images have NULL in new columns
- On next image scan, new EXIF fields are populated
- No data loss; existing metadata unchanged

### Performance Impact
- EXIF extraction: ~2ms per image (minimal overhead, already using `-ping`)
- Index overhead: ~100 bytes per row for 8 new columns
- Query latency: EXIF filtering is post-KNN, so unchanged for queries without constraints
- Location lookup: O(1) in-memory hashmap, negligible

---

## 10. Files to Create/Modify Summary

| Phase | Task | File(s) | Action |
|-------|------|---------|--------|
| EXIF-1 | Extract | `exif_reader.hpp` | Extend `ExifInfo` struct |
| EXIF-1 | Extract | `exif_reader.cpp` | Implement ISO, f-number, lens, GPS extraction |
| EXIF-1 | Extract | `image_processor.cpp` | Pass full `ExifInfo` to DB update |
| EXIF-2 | Schema | `sql_constants.hpp` | Add 8 new columns + indices |
| EXIF-2 | Schema | `scanned_files_ops.cpp` | Update `updateExifMetadata()` |
| EXIF-3 | Geocoding | `location_geocoder.hpp` (new) | Implement `LocationGeocoder` class |
| EXIF-3 | Geocoding | `location_geocoder.cpp` (new) | Load GeoJSON, lookup bounds |
| EXIF-3 | Geocoding | `world_boundaries.geojson` (new) | Add GeoJSON boundary data |
| EXIF-4 | Search | `text_search_engine.hpp` | Add `ExifConstraints` struct |
| EXIF-4 | Search | `text_search_engine.cpp` | Implement `ExtractExifConstraints()`, modify `SearchQuery()` |
| EXIF-5 | Test | `test_exif_reader.cpp` | Add unit tests for extraction |
| EXIF-5 | Test | `test_text_search_engine.cpp` | Add constraint parsing tests |
| EXIF-5 | Test | `test_location_geocoder.cpp` (new) | Add geocoder tests |

---

## 11. References

- **CLIP Embeddings**: Already in use via `text_search_engine.cpp` (clip-RN50.onnx)
- **EXIF Extraction**: Already in use via `exif_reader.hpp` (ImageMagick, LibRaw)
- **Vector Index**: Already in use via `vec_embedding_index` (sqlite-vec, vec0)
- **GeoJSON**: [Natural Earth Data](https://www.naturalearthdata.com/)
