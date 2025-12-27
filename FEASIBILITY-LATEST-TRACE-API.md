# Feasibility Study: Latest Trace API Endpoint

## Problem Statement
Add an API endpoint that allows retrieving the latest trace of an aircraft by ICAO hex code without knowing its last flown date.

## Current System Analysis

### Trace Storage Structure
1. **Live Traces** (in `/run/readsb/traces/`):
   - `trace_recent_<hex>.json` - Recent points (last ~5 minutes)
   - `trace_full_<hex>.json` - Full trace (up to 24 hours)

2. **Historic Traces** (in `--write-globe-history` directory):
   - Organized by date: `YYYY-MM-DD/traces/<hex_prefix>/trace_full_<hex>.json`
   - One file per aircraft per day
   - Compressed with gzip

3. **Trace Generation**:
   - Traces are written periodically based on `--json-trace-interval` (default 30s)
   - Historic traces written at end of day
   - Files remain for configured retention period

### Current API Endpoints
From `api.c`, existing query formats:
- `/?find_hex=<hex>` - Returns current aircraft state
- `/?box=...` - Geographic queries
- `/?circle=...` - Radius queries
- No trace retrieval endpoint exists

### Trace Access Pattern
To access historic traces, you currently need:
1. Aircraft ICAO hex code
2. Date the aircraft was seen
3. File path: `<history_dir>/<date>/traces/<hex_prefix>/trace_full_<hex>.json`

## Proposed Solution

### New API Endpoint
```
GET /api/v1/trace/latest/<hex>
or
/?get_latest_trace=<hex>
```

### Required Components

#### 1. Last Seen Index/Map
Create a persistent data structure that maps ICAO hex → last flight date.

**Options:**
- **A. In-Memory Hash Table** (simplest, but lost on restart)
  - `struct { uint32_t hex; time_t last_date; }` 
  - Hash table similar to existing `apiBuffer` structure
  - Updated whenever trace is written
  
- **B. Persistent File Index** (survives restarts)
  - SQLite database: `CREATE TABLE last_seen (hex TEXT PRIMARY KEY, last_date INTEGER)`
  - Or simple text file: `<hex>,<last_date>\n`
  - Updated atomically when traces are written
  
- **C. Directory Scanning** (no separate index needed)
  - Scan filesystem to find most recent date directory containing hex
  - Slower but requires no additional storage

**Recommendation:** Start with option A (in-memory), add option B (persistent) if needed.

#### 2. Index Update Logic
Update the last-seen index when:
- `writePerm()` writes a historic trace (in `globe_index.c:575`)
- Track `a->trace_perm_last_timestamp` already exists, use this
- Add hook to update index map

#### 3. API Handler Function
Add to `api.c` (in `parseFetch()` around line 1206):
```c
// Pseudo-code
if (byteMatchStrict(option, "get_latest_trace")) {
    uint32_t hex = parseHex(value);
    time_t last_date = lookupLastSeenDate(hex);
    if (last_date > 0) {
        char* trace_json = loadHistoricTrace(hex, last_date);
        return trace_json;
    }
    return invalid; // or return empty result
}
```

#### 4. Trace Loading Function
```c
// New function in globe_index.c
char* loadHistoricTrace(uint32_t hex, time_t date) {
    // 1. Construct path: <history_dir>/<date>/traces/<hex%256>/trace_full_<hex>.json.gz
    // 2. Check if file exists
    // 3. Decompress if .gz
    // 4. Return JSON content
    // 5. Handle fallback to /run/readsb if no historic trace
}
```

## Implementation Complexity

### Minimal Changes (Phase 1 - In-Memory Only)
**Estimated effort: 2-4 hours**

1. Add in-memory hash table structure (100 lines)
   - Similar to `apiBuffer->hexHash` structure
   - Store `{ hex, last_date }` pairs

2. Update index on trace write (20 lines)
   - Hook into `writePerm()` function
   - Call `updateLastSeenIndex(hex, date)`

3. Add API endpoint (150 lines)
   - Parse new query parameter
   - Lookup in index
   - Load and return trace file
   - Handle file not found

4. Add trace loading utility (100 lines)
   - Path construction
   - File reading
   - gzip decompression (use existing ZSTD code as reference)

**Total: ~370 lines of C code**

### Full Solution (Phase 2 - With Persistence)
**Estimated effort: 4-8 hours additional**

5. Add persistent index (200 lines)
   - SQLite or simple file-based index
   - Load on startup
   - Save on shutdown and periodically

6. Index maintenance (50 lines)
   - Cleanup old entries
   - Handle date rollover
   - Atomic updates

**Total additional: ~250 lines**

## Performance Considerations

### Memory Usage
- In-memory index: ~12 bytes per aircraft
- For 100,000 aircraft: ~1.2 MB
- Negligible compared to existing buffers

### CPU Usage
- Hash lookup: O(1), ~100 nanoseconds
- File load: depends on I/O, ~1-10ms for gzipped file
- Should handle 100+ req/s easily

### Disk I/O
- Reading historic traces from disk
- Consider caching recently accessed traces
- Existing `--full-trace-dir` option already optimizes this

## Potential Issues & Solutions

### Issue 1: Aircraft Not Found
**Problem:** Aircraft never seen or trace expired
**Solution:** Return 404 or empty result with appropriate message

### Issue 2: Multiple Date Files
**Problem:** Trace spans multiple days
**Solution:** Only return latest day's trace, or concatenate if needed

### Issue 3: Index Corruption
**Problem:** In-memory index lost on crash
**Solution:** Phase 2 persistent index solves this

### Issue 4: Race Conditions
**Problem:** Trace file being written while being read
**Solution:** Use atomic writes (already done with temp file + rename)

### Issue 5: Disk Space
**Problem:** Old traces accumulate
**Solution:** Already handled by existing retention logic

## Alternative Approaches

### Alternative 1: Scan on Demand
- No index needed
- Scan filesystem to find latest trace
- **Pros:** No maintenance, no memory
- **Cons:** Slow (100-1000ms), high I/O

### Alternative 2: Extend Existing Endpoint
- Add date parameter with "latest" option: `/?find_hex=<hex>&trace_date=latest`
- **Pros:** Consistent with existing API
- **Cons:** Overloads existing endpoint

### Alternative 3: Metadata-Only Endpoint
- Return just the date + URL to trace, not the trace itself
- Client makes second request for actual trace
- **Pros:** Faster, less I/O
- **Cons:** Two round trips

## Recommendation

### Recommended Approach: Phased Implementation

**Phase 1: Minimal Viable Product (DO THIS FIRST)**
1. In-memory index only
2. Simple API endpoint
3. Basic error handling
4. Test with representative dataset

**Phase 2: Production Hardening (IF NEEDED)**
1. Add persistent index
2. Add caching layer
3. Add monitoring/metrics
4. Performance optimization

### API Design Recommendation
```
GET /re-api/?latest_trace=<hex>

Response (success):
{
  "hex": "a12345",
  "trace_date": "2024-12-27",
  "trace_url": "/history/2024-12-27/traces/45/trace_full_a12345.json",
  "trace": { ... full trace JSON ... }
}

Response (not found):
{
  "hex": "a12345",
  "error": "No trace data found",
  "msg": "Aircraft has no historic trace data"
}
```

## Testing Strategy

1. **Unit Tests:**
   - Index lookup functions
   - Path construction
   - File loading

2. **Integration Tests:**
   - API endpoint with valid hex
   - API endpoint with invalid hex
   - API endpoint with missing trace file

3. **Performance Tests:**
   - 100+ concurrent requests
   - Memory usage monitoring
   - Response time < 100ms

## Conclusion

### Feasibility: ✅ HIGHLY FEASIBLE

This feature is **straightforward to implement** with minimal changes to the existing codebase. The main components (trace storage, API infrastructure) are already in place. The only missing piece is the index mapping hex codes to last flight dates.

### Recommended Next Steps

1. ✅ **Approve this design** (stakeholder review)
2. Implement Phase 1 (2-4 hours)
3. Test with production data
4. Evaluate need for Phase 2 based on usage

### Risk Assessment: LOW
- No breaking changes to existing functionality
- Isolated feature, easy to disable if issues arise
- Leverages existing, battle-tested trace infrastructure

### Effort: LOW-MEDIUM
- Phase 1: 2-4 hours
- Phase 2: 4-8 hours
- Total: 6-12 hours development + testing
