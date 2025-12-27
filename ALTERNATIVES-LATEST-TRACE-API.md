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

### Phase 3: Scale (Month 2+)
**Evaluate Redis (Approach 6) if needed**

Rationale:
- Only if multi-server needed
- Only if SQLite performance insufficient
- Adds operational complexity

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

## Conclusion

**Recommended Approach: Start with #1, migrate to #2 if needed**

The in-memory index (Approach 1) is the best starting point because:
1. Simple to implement
2. Low risk
3. Sufficient for most use cases
4. Easy to migrate away from if needed

If persistence becomes critical, SQLite (Approach 2) is the natural next step.

Reserve Redis (Approach 6) for multi-server deployments only.

Avoid filesystem scanning (Approach 3) except for testing/prototyping.
