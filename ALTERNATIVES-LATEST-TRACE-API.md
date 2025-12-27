# Alternative Approaches: Latest Trace API

## Overview
This document explores different architectural approaches for implementing the "latest trace by ICAO" feature, with analysis of trade-offs.

## Approach 1: In-Memory Index (RECOMMENDED)

### Description
Maintain a simple in-memory hash table mapping hex codes to last seen dates.

### Implementation
```c
struct {
    uint32_t hex;
    time_t last_date;
} lastSeenEntry;

struct lastSeenIndex {
    lastSeenEntry *entries;
    int count;
    int alloc;
    pthread_mutex_t mutex;
} index;
```

### Pros
✅ **Simple**: ~300 lines of code  
✅ **Fast**: O(1) lookup with hash table  
✅ **Low memory**: ~12 bytes per aircraft  
✅ **No dependencies**: Pure C, no libraries  
✅ **Zero persistence overhead**: No disk I/O  

### Cons
❌ **Lost on restart**: Index rebuilt from scratch  
❌ **Cold start**: Takes time to populate after restart  
❌ **Memory only**: Not suitable for huge datasets (10M+ aircraft)  

### When to Use
- Small to medium deployments (< 1M aircraft)
- Fast restarts acceptable
- Minimal complexity preferred

### Resource Requirements
- Memory: 1.2 MB per 100k aircraft
- CPU: Negligible
- Disk: 0 bytes

---

## Approach 2: Persistent SQLite Index

### Description
Use SQLite database to persist hex → date mappings.

### Implementation
```sql
CREATE TABLE last_seen (
    hex TEXT PRIMARY KEY,
    last_date INTEGER,
    last_updated INTEGER
);

CREATE INDEX idx_last_date ON last_seen(last_date);
```

### Pros
✅ **Survives restarts**: Index persisted to disk  
✅ **ACID guarantees**: Transactions ensure consistency  
✅ **Queryable**: Can add analytics/reporting  
✅ **Scalable**: Handles millions of entries  
✅ **Battle-tested**: SQLite is rock-solid  

### Cons
❌ **Complexity**: Requires SQLite library  
❌ **Disk I/O**: Write on every trace update  
❌ **Lock contention**: Writes can block reads  
❌ **File corruption**: Needs recovery mechanisms  
❌ **~500 lines**: More code to maintain  

### When to Use
- Large deployments (> 1M aircraft)
- High availability requirements
- Need historical analytics

### Resource Requirements
- Memory: ~100 KB (SQLite overhead) + ~12 bytes per row
- CPU: ~10 microseconds per lookup, ~100 microseconds per write
- Disk: ~1 MB per 100k aircraft + WAL

---

## Approach 3: Filesystem Scanning

### Description
No index at all. Scan filesystem to find latest trace on demand.

### Implementation
```c
char* findLatestTrace(uint32_t hex) {
    // 1. List all date directories in history dir
    // 2. Sort by date descending
    // 3. Check each date dir for hex trace file
    // 4. Return first found
}
```

### Pros
✅ **Zero maintenance**: No index to update  
✅ **Always accurate**: Reflects actual filesystem  
✅ **Simple**: ~100 lines of code  
✅ **No memory overhead**: No persistent structures  

### Cons
❌ **Very slow**: 100-1000ms per request  
❌ **High I/O**: Many stat() calls per request  
❌ **Not scalable**: Gets worse with more dates  
❌ **CPU intensive**: Directory traversal  

### When to Use
- Infrequent usage (< 1 req/min)
- Very small datasets
- Prototyping/testing only

### Resource Requirements
- Memory: None
- CPU: ~50-500ms per query
- Disk: High I/O load

---

## Approach 4: Metadata Sidecar Files

### Description
For each trace file, create a metadata sidecar with date info.

### Implementation
```
2024-12-27/traces/ab/trace_full_abc123.json       # Trace data
2024-12-27/traces/ab/trace_full_abc123.json.meta  # Metadata
```

```json
// trace_full_abc123.json.meta
{
  "hex": "abc123",
  "date": "2024-12-27",
  "timestamp": 1703721600,
  "points": 1234
}
```

### Pros
✅ **Decentralized**: No single index file  
✅ **Atomic**: Metadata updates with trace  
✅ **Self-describing**: Trace files carry their own metadata  
✅ **Easy to sync**: rsync-friendly  

### Cons
❌ **Doubled files**: 2x number of files  
❌ **Still needs scanning**: Or separate index  
❌ **Redundant data**: Info already in trace JSON  
❌ **I/O intensive**: More open/close operations  

### When to Use
- Multi-server deployments
- File-based replication
- Manual inspection needs

### Resource Requirements
- Memory: None (or small index)
- CPU: Low
- Disk: +10% storage for metadata files

---

## Approach 5: Hybrid: Index + Filesystem Fallback

### Description
Use in-memory index for fast path, fall back to filesystem scan for misses.

### Implementation
```c
struct char_buffer getLatestTrace(uint32_t hex) {
    // 1. Check in-memory index (O(1))
    time_t date = lookupIndex(hex);
    
    if (date > 0) {
        // Fast path: load from known date
        return loadTrace(hex, date);
    }
    
    // Slow path: scan filesystem
    date = scanForLatestTrace(hex);
    
    if (date > 0) {
        // Update index for next time
        updateIndex(hex, date);
        return loadTrace(hex, date);
    }
    
    return empty;
}
```

### Pros
✅ **Fast common case**: Index handles most requests  
✅ **Never stale**: Filesystem is source of truth  
✅ **Self-healing**: Index repairs itself  
✅ **Graceful degradation**: Works even if index corrupted  

### Cons
❌ **Complex**: Two code paths to maintain  
❌ **Variable latency**: Some requests much slower  
❌ **Race conditions**: Index and filesystem can disagree  

### When to Use
- High reliability requirements
- Can tolerate occasional slow requests
- Want best of both worlds

### Resource Requirements
- Memory: Same as Approach 1
- CPU: Mostly O(1), occasionally O(n)
- Disk: Minimal I/O except on misses

---

## Approach 6: Redis/Memcached External Index

### Description
Use external caching layer for index.

### Implementation
```bash
redis-cli SET "trace:abc123" "1703721600"
redis-cli GET "trace:abc123"
```

### Pros
✅ **Shared across instances**: Multiple readsb processes  
✅ **Distributed**: Can scale horizontally  
✅ **TTL support**: Auto-expire old entries  
✅ **Replication**: Built-in redundancy  

### Cons
❌ **External dependency**: Requires Redis/Memcached  
❌ **Network latency**: ~1ms per lookup  
❌ **Complexity**: More moving parts  
❌ **Operations burden**: Need to maintain cache cluster  

### When to Use
- Multi-server deployments
- Already using Redis/Memcached
- Need distributed cache

### Resource Requirements
- Memory: Depends on Redis config
- CPU: Low (network bound)
- Disk: Redis persistence config

---

## Comparison Matrix

| Approach | Code | Speed | Memory | Disk | Restarts | Scalability | Complexity |
|----------|------|-------|--------|------|----------|-------------|------------|
| 1. In-Memory Index | 300 | O(1) ~100ns | 12B/ac | 0 | Lost | 1M ac | ⭐⭐⭐⭐⭐ Low |
| 2. SQLite | 500 | O(1) ~10μs | 12B/ac | 1MB/100k | Persisted | 10M+ ac | ⭐⭐⭐ Medium |
| 3. Filesystem Scan | 100 | O(n) ~500ms | 0 | 0 | N/A | 1k dates | ⭐⭐⭐⭐ Low |
| 4. Metadata Sidecar | 200 | O(n) ~200ms | 0 | +10% | N/A | 10k dates | ⭐⭐⭐ Medium |
| 5. Hybrid | 400 | O(1)/O(n) | 12B/ac | 0 | Self-heal | 1M ac | ⭐⭐ Complex |
| 6. Redis | 300 | O(1) ~1ms | Redis | Redis | Persisted | 100M+ ac | ⭐⭐ Complex |

---

## Recommendation by Use Case

### Small Deployment (< 10k aircraft, single server)
**Use Approach 1: In-Memory Index**
- Simplest implementation
- Sufficient performance
- Low resource usage

### Medium Deployment (10k-1M aircraft, single server)
**Use Approach 1 → Approach 2 migration path**
- Start with in-memory (Phase 1)
- Add SQLite persistence if restarts become painful (Phase 2)

### Large Deployment (1M+ aircraft, single server)
**Use Approach 2: SQLite**
- Handles large datasets
- Persisted across restarts
- Good performance

### Multi-Server Deployment
**Use Approach 6: Redis**
- Shared index across servers
- Horizontal scaling
- Already have Redis infrastructure

### Development/Testing
**Use Approach 3: Filesystem Scan**
- No maintenance
- Easy to debug
- Acceptable for low traffic

### High-Reliability Mission-Critical
**Use Approach 5: Hybrid**
- Never returns stale data
- Self-healing
- Graceful degradation

---

## Implementation Recommendation

### Phase 1: MVP (Week 1)
**Implement Approach 1: In-Memory Index**

Rationale:
- Fastest to implement (2-4 hours)
- Proves concept
- Low risk
- Can handle 99% of use cases

### Phase 2: Production (Week 2-3)
**Add SQLite persistence (Approach 2)**

Rationale:
- Solves restart problem
- Still relatively simple
- Industry standard
- Easy to backup/restore

### Phase 3: Advanced Features (Month 2+)
**Evaluate Approach 7 (Linked-List Navigation) or Approach 6 (Redis)**

**Approach 7 - If you need:**
- Chronological trace navigation (prev/next)
- Timeline features
- Flight history UI

**Approach 6 - If you need:**
- Multi-server deployment
- Distributed caching
- Horizontal scaling

Rationale:
- Approach 7 adds powerful navigation without external dependencies
- Approach 6 only for large-scale distributed systems
- Both add operational complexity

---

## Migration Strategy

### From Approach 1 to Approach 2 (In-Memory → SQLite)

1. **Parallel Running** (Week 1)
   - Keep in-memory index running
   - Start writing to SQLite in background
   - Compare results

2. **Switch Reads** (Week 2)
   - Read from SQLite
   - Keep writing to both
   - Monitor performance

3. **Remove Old Code** (Week 3)
   - Remove in-memory index
   - SQLite only

### From Approach 2 to Approach 6 (SQLite → Redis)

1. **Replicate to Redis** (Week 1)
   - Write to both SQLite and Redis
   - Read from SQLite

2. **Switch Reads** (Week 2)
   - Read from Redis
   - Keep writing to both
   - Monitor cache hit rate

3. **Remove SQLite** (Week 3)
   - Redis only
   - Keep SQLite as backup

---

## Cost Analysis

### Approach 1: In-Memory Index
- **Development**: 4 hours × $100/hr = $400
- **Runtime**: $0/month
- **Maintenance**: 1 hour/year = $100/year
- **Total Year 1**: $500

### Approach 2: SQLite
- **Development**: 8 hours × $100/hr = $800
- **Runtime**: $0/month
- **Maintenance**: 2 hours/year = $200/year
- **Total Year 1**: $1,000

### Approach 6: Redis
- **Development**: 6 hours × $100/hr = $600
- **Runtime**: $50/month = $600/year (cloud Redis)
- **Maintenance**: 4 hours/year = $400/year
- **Total Year 1**: $1,600

---

## Testing Strategy by Approach

### Approach 1: In-Memory
```bash
# Test index updates
curl 'http://localhost:8042/?latest_trace=abc123'

# Test after restart (should be empty)
systemctl restart readsb
curl 'http://localhost:8042/?latest_trace=abc123'

# Test with 100k aircraft
for i in {000000..100000}; do
    curl 'http://localhost:8042/?latest_trace='$i &
done | grep "200 OK" | wc -l
```

### Approach 2: SQLite
```bash
# Test persistence
curl 'http://localhost:8042/?latest_trace=abc123'
systemctl restart readsb
curl 'http://localhost:8042/?latest_trace=abc123'  # Should still work

# Test database
sqlite3 /var/lib/readsb/trace_index.db "SELECT * FROM last_seen LIMIT 10;"

# Test concurrent writes
for i in {1..100}; do
    curl -X POST 'http://localhost:8042/update_trace' &
done
```

### Approach 3: Filesystem
```bash
# Test scan performance
time curl 'http://localhost:8042/?latest_trace=abc123'

# Create many date directories
for d in {2024-01-01..2024-12-31}; do
    mkdir -p /history/$d/traces/ab
done

# Test with many dates
time curl 'http://localhost:8042/?latest_trace=abc123'
```

---

## Approach 7: Linked-List Trace Navigation with Synced Map

### Description
Combine in-memory + persistent map with doubly-linked trace metadata. Each trace points to previous and next trace, enabling chronological navigation without date lookups.

### Implementation

#### Data Structure
```c
// In-memory map (synced to disk)
struct traceMapEntry {
    uint32_t hex;
    time_t latest_date;     // Date of latest trace
    char latest_path[256];  // Path to latest trace
};

// Metadata stored with each trace
struct traceMetadata {
    char date[16];          // "2024-12-27"
    char prev_date[16];     // "2024-12-26" or null
    char next_date[16];     // "2024-12-28" or null (latest = null)
    int64_t timestamp;      // Unix timestamp
    char hex[8];            // ICAO hex
};
```

#### Storage Options

**Option A: Metadata Sidecar Files**
```
2024-12-27/traces/ab/trace_full_abc123.json       # Trace data
2024-12-27/traces/ab/trace_full_abc123.json.meta  # Metadata JSON
```

Metadata file content:
```json
{
  "hex": "abc123",
  "date": "2024-12-27",
  "timestamp": 1703721600,
  "prev": "2024-12-26",
  "next": null,
  "points": 1234
}
```

**Option B: Embedded in Trace JSON**
```json
{
  "icao": "abc123",
  "timestamp": 1703721600.495,
  "_links": {
    "self": "2024-12-27",
    "prev": "2024-12-26",
    "next": null
  },
  "trace": [...]
}
```

#### Persistent Map File
Simple append-only file with periodic compaction:
```
# /var/lib/readsb/trace_map.txt
abc123,2024-12-27,latest
def456,2024-12-26,latest
789abc,2024-12-25,latest
```

Or binary format for efficiency:
```c
struct {
    uint32_t hex;
    time_t date;
    uint8_t flags;  // bit 0: is_latest
} __attribute__((packed));
```

### Write Flow

When writing a new trace:

1. **Read current latest from map**
   ```c
   char *prev_date = lookupLatestDate(hex);  // "2024-12-26"
   ```

2. **Update previous trace metadata**
   ```c
   // Update 2024-12-26 trace to point forward
   updateTraceMeta(hex, prev_date, "next", "2024-12-27");
   ```

3. **Write new trace with metadata**
   ```c
   struct traceMetadata meta = {
       .date = "2024-12-27",
       .prev_date = prev_date,  // "2024-12-26"
       .next_date = NULL,       // This is now latest
       .timestamp = now,
       .hex = hex
   };
   writeTrace(hex, trace_data, &meta);
   ```

4. **Update map**
   ```c
   updateMap(hex, "2024-12-27", true /* is_latest */);
   syncMapToDisk();
   ```

### Read Flow

**Get Latest Trace:**
```c
// 1. Lookup in map (O(1))
char *latest_date = getLatestDate("abc123");  // "2024-12-27"

// 2. Load trace
struct trace *t = loadTrace("abc123", latest_date);

// 3. Navigate backward if needed
if (t->meta.prev_date) {
    struct trace *prev = loadTrace("abc123", t->meta.prev_date);
}
```

**Navigate Timeline:**
```c
// Start at latest
struct trace *curr = getLatestTrace("abc123");

// Go back in time
while (curr && curr->meta.prev_date) {
    curr = loadTrace("abc123", curr->meta.prev_date);
    // Process historical trace...
}
```

### API Extensions

This enables powerful new endpoints:

```
GET /?latest_trace=abc123              # Get latest (same as before)
GET /?trace=abc123&date=2024-12-27     # Get specific date
GET /?trace=abc123&prev=2024-12-27     # Get previous trace
GET /?trace=abc123&next=2024-12-26     # Get next trace
GET /?trace_timeline=abc123            # Get all trace dates (just metadata)
```

### Pros
✅ **Chronological navigation**: Move forward/backward without dates  
✅ **Self-documenting**: Trace history embedded in data  
✅ **No date math**: Just follow pointers  
✅ **Enables timeline features**: "Show all flights this month"  
✅ **Persistent + in-memory**: Best of both worlds  
✅ **Graceful degradation**: Works even if some metadata missing  

### Cons
❌ **Write complexity**: Must update 2-3 files per trace  
❌ **Storage overhead**: Extra metadata files (+10-20%)  
❌ **Atomic updates**: Need to handle partial failures  
❌ **Migration**: Existing traces need metadata retrofitting  
❌ **More code**: ~500-700 lines vs ~300 for simple index  

### When to Use
- Need trace navigation features (prev/next flight)
- Building flight history timeline UI
- Want to avoid date-based queries
- Have reliable disk I/O
- Can handle write complexity

### Resource Requirements
- **Memory**: Same as Approach 1 (12 bytes/aircraft) + map cache
- **Disk**: +10-20% for metadata files
- **CPU**: Slightly higher on writes (update 2-3 files)
- **I/O**: 2-3x writes per trace, but reads still O(1)

### Comparison to Basic Index

| Aspect | Basic Index (Approach 1) | Linked List (Approach 7) |
|--------|-------------------------|--------------------------|
| **Complexity** | Low (~300 LOC) | Medium (~700 LOC) |
| **Navigation** | Latest only | Full timeline |
| **Write Ops** | 1 file | 2-3 files |
| **Storage** | 0% overhead | +10-20% overhead |
| **Features** | Get latest | Prev/next/timeline |
| **Migration** | None needed | Retrofit existing traces |

### Migration Strategy

**Phase 1: Add Metadata to New Traces**
- Start writing metadata for all new traces
- Keep simple index working
- Gradually builds linked list going forward

**Phase 2: Retrofit Historical Traces**
- Background job to scan existing traces
- Add metadata files for historical data
- Link traces together chronologically

**Phase 3: Enable Navigation Features**
- Add prev/next API endpoints
- Build timeline UI features
- Deprecate date-based queries

### Code Example

```c
// Update previous trace to point forward
int updateTraceMeta(uint32_t hex, const char *date, const char *field, const char *value) {
    char metapath[PATH_MAX];
    snprintf(metapath, PATH_MAX, 
             "%s/%s/traces/%02x/trace_full_%06x.json.meta",
             Modes.globe_history_dir, date, hex % 256, hex & 0xFFFFFF);
    
    // Read existing metadata
    cJSON *meta = readJSONFile(metapath);
    if (!meta) {
        // Create new metadata
        meta = cJSON_CreateObject();
    }
    
    // Update field
    cJSON_DeleteItemFromObject(meta, field);
    cJSON_AddStringToObject(meta, field, value);
    
    // Write back atomically
    char tmppath[PATH_MAX];
    snprintf(tmppath, PATH_MAX, "%s.tmp", metapath);
    writeJSONFile(tmppath, meta);
    rename(tmppath, metapath);  // Atomic on POSIX
    
    cJSON_Delete(meta);
    return 0;
}
```

---

## Comparison Matrix (Updated)

| Approach | Code | Speed | Memory | Disk | Restarts | Scalability | Complexity | Navigation |
|----------|------|-------|--------|------|----------|-------------|------------|------------|
| 1. In-Memory Index | 300 | O(1) ~100ns | 12B/ac | 0 | Lost | 1M ac | ⭐⭐⭐⭐⭐ Low | Latest only |
| 2. SQLite | 500 | O(1) ~10μs | 12B/ac | 1MB/100k | Persisted | 10M+ ac | ⭐⭐⭐ Medium | Latest only |
| 3. Filesystem Scan | 100 | O(n) ~500ms | 0 | 0 | N/A | 1k dates | ⭐⭐⭐⭐ Low | Full scan |
| 4. Metadata Sidecar | 200 | O(n) ~200ms | 0 | +10% | N/A | 10k dates | ⭐⭐⭐ Medium | Via scan |
| 5. Hybrid | 400 | O(1)/O(n) | 12B/ac | 0 | Self-heal | 1M ac | ⭐⭐ Complex | Latest + scan |
| 6. Redis | 300 | O(1) ~1ms | Redis | Redis | Persisted | 100M+ ac | ⭐⭐ Complex | Latest only |
| **7. Linked List** | **700** | **O(1)** | **12B/ac** | **+15%** | **Persisted** | **1M ac** | **⭐⭐⭐ Medium** | **Full timeline** |

---

## Conclusion

**Recommended Approach: Start with #1, consider #7 for advanced features**

The in-memory index (Approach 1) is the best starting point because:
1. Simple to implement
2. Low risk
3. Sufficient for most use cases
4. Easy to migrate away from if needed

If persistence becomes critical, SQLite (Approach 2) is the natural next step.

**Approach 7 (Linked-List Navigation)** is recommended when:
- You need chronological trace navigation (prev/next flight)
- Building timeline or history features
- Want to eliminate date-based queries
- Can handle the additional write complexity

Reserve Redis (Approach 6) for multi-server deployments only.

Avoid filesystem scanning (Approach 3) except for testing/prototyping.

### Evolution Path

```
Start Here → Phase 1 → Phase 2 (if needed) → Phase 3 (if needed)
───────────────────────────────────────────────────────────────
Approach 1   Approach 2      Approach 7
In-Memory    + SQLite        + Linked List
(MVP)        (Persistence)   (Navigation)
```
