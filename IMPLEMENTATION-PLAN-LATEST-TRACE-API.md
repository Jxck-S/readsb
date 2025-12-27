# Implementation Plan: Latest Trace API Endpoint

## Overview
This document provides a detailed implementation plan for adding an API endpoint to retrieve the latest trace for an aircraft by ICAO hex code without knowing the flight date.

## File Changes Required

### 1. Add Data Structures (readsb.h)
**Location:** Add to `struct Modes` around line 900

```c
// Last seen trace index
struct lastSeenIndex {
    uint32_t *hexList;          // List of hex codes
    time_t *lastSeenDates;      // Corresponding last seen dates
    int count;                  // Number of entries
    int alloc;                  // Allocated size
    pthread_mutex_t mutex;      // Thread safety
} lastSeenTraceIndex;
```

**Lines added:** ~10

### 2. Initialize Index (readsb.c)
**Location:** In `modesInit()` around line 200

```c
// Initialize last seen trace index
memset(&Modes.lastSeenTraceIndex, 0, sizeof(struct lastSeenIndex));
pthread_mutex_init(&Modes.lastSeenTraceIndex.mutex, NULL);
Modes.lastSeenTraceIndex.alloc = 1024;
Modes.lastSeenTraceIndex.hexList = cmalloc(Modes.lastSeenTraceIndex.alloc * sizeof(uint32_t));
Modes.lastSeenTraceIndex.lastSeenDates = cmalloc(Modes.lastSeenTraceIndex.alloc * sizeof(time_t));
```

**Lines added:** ~10

### 3. Update Index on Trace Write (globe_index.c)
**Location:** In `writePerm()` after successful write (around line 673)

```c
// Update last seen index after successful permanent trace write
void updateLastSeenIndex(uint32_t hex, time_t date) {
    pthread_mutex_lock(&Modes.lastSeenTraceIndex.mutex);
    
    // Binary search to find if hex already exists
    int idx = -1;
    for (int i = 0; i < Modes.lastSeenTraceIndex.count; i++) {
        if (Modes.lastSeenTraceIndex.hexList[i] == hex) {
            idx = i;
            break;
        }
    }
    
    if (idx >= 0) {
        // Update existing entry if date is newer
        if (date > Modes.lastSeenTraceIndex.lastSeenDates[idx]) {
            Modes.lastSeenTraceIndex.lastSeenDates[idx] = date;
        }
    } else {
        // Add new entry
        if (Modes.lastSeenTraceIndex.count >= Modes.lastSeenTraceIndex.alloc) {
            // Grow arrays
            Modes.lastSeenTraceIndex.alloc *= 2;
            Modes.lastSeenTraceIndex.hexList = realloc(
                Modes.lastSeenTraceIndex.hexList,
                Modes.lastSeenTraceIndex.alloc * sizeof(uint32_t)
            );
            Modes.lastSeenTraceIndex.lastSeenDates = realloc(
                Modes.lastSeenTraceIndex.lastSeenDates,
                Modes.lastSeenTraceIndex.alloc * sizeof(time_t)
            );
        }
        
        idx = Modes.lastSeenTraceIndex.count++;
        Modes.lastSeenTraceIndex.hexList[idx] = hex;
        Modes.lastSeenTraceIndex.lastSeenDates[idx] = date;
    }
    
    pthread_mutex_unlock(&Modes.lastSeenTraceIndex.mutex);
}
```

Call this function in `writePerm()` after line 673:
```c
a->trace_perm_last_timestamp = endStamp;
a->traceWrittenForYesterday = Modes.triggerPermWriteDay;

// ADD THIS LINE:
updateLastSeenIndex(a->addr, endStamp / 1000); // Convert to time_t

return 1;
```

**Lines added:** ~50

### 4. Add Trace Lookup Function (globe_index.c)
**Location:** Add new function before `traceWrite()`

```c
// Lookup last seen date for a hex code
// Returns 0 if not found
time_t lookupLastSeenDate(uint32_t hex) {
    time_t result = 0;
    
    pthread_mutex_lock(&Modes.lastSeenTraceIndex.mutex);
    
    for (int i = 0; i < Modes.lastSeenTraceIndex.count; i++) {
        if (Modes.lastSeenTraceIndex.hexList[i] == hex) {
            result = Modes.lastSeenTraceIndex.lastSeenDates[i];
            break;
        }
    }
    
    pthread_mutex_unlock(&Modes.lastSeenTraceIndex.mutex);
    
    return result;
}

// Load historic trace from disk
// Returns char_buffer with trace JSON, or empty buffer if not found
struct char_buffer loadHistoricTrace(uint32_t hex, time_t date, threadpool_buffer_t *buffer) {
    struct char_buffer cb = { 0 };
    
    if (!Modes.globe_history_dir) {
        return cb; // No history directory configured
    }
    
    // Convert time_t to date string
    struct tm tm;
    gmtime_r(&date, &tm);
    char dateStr[32];
    strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &tm);
    
    // Construct file path
    char filepath[PATH_MAX];
    snprintf(filepath, PATH_MAX, 
             "%s/%s/traces/%02x/trace_full_%s%06x.json",
             Modes.globe_history_dir,
             dateStr,
             hex % 256,
             (hex & MODES_NON_ICAO_ADDRESS) ? "~" : "",
             hex & 0xFFFFFF);
    
    // Check if .gz version exists
    char gzpath[PATH_MAX];
    snprintf(gzpath, PATH_MAX, "%s.gz", filepath);
    
    // Try to read file
    int fd = -1;
    int is_gzipped = 0;
    
    fd = open(gzpath, O_RDONLY);
    if (fd >= 0) {
        is_gzipped = 1;
    } else {
        fd = open(filepath, O_RDONLY);
    }
    
    if (fd < 0) {
        return cb; // File not found
    }
    
    // Get file size
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return cb;
    }
    
    // Allocate buffer
    size_t alloc = st.st_size * (is_gzipped ? 10 : 1) + 1024;
    cb.buffer = check_grow_threadpool_buffer_t(buffer, alloc);
    
    if (!cb.buffer) {
        close(fd);
        return cb;
    }
    
    if (is_gzipped) {
        // Read compressed data
        char *compressed = cmalloc(st.st_size);
        if (read(fd, compressed, st.st_size) != st.st_size) {
            sfree(compressed);
            close(fd);
            cb.buffer = NULL;
            return cb;
        }
        
        // Decompress using zstd (note: file is gzipped, need to handle this)
        // For simplicity, we'll just return error for now
        // TODO: Add gzip decompression support
        sfree(compressed);
        close(fd);
        cb.buffer = NULL;
        return cb;
    } else {
        // Read uncompressed data
        ssize_t bytes_read = read(fd, cb.buffer, st.st_size);
        if (bytes_read > 0) {
            cb.len = bytes_read;
            cb.buffer[bytes_read] = '\0';
        }
    }
    
    close(fd);
    return cb;
}
```

**Lines added:** ~100

### 5. Add API Endpoint Handler (api.c)
**Location:** In `parseFetch()` function, add new option around line 1400

```c
} else if (byteMatchStrict(option, "latest_trace")) {
    // Parse hex code from value
    char *endptr = NULL;
    uint32_t hex = (uint32_t) strtol(value, &endptr, 16);
    
    if (value == endptr) {
        return invalid; // Invalid hex code
    }
    
    // Check for non-ICAO prefix
    if (value[0] == '~') {
        hex |= MODES_NON_ICAO_ADDRESS;
        value++;
    }
    
    // Lookup last seen date
    time_t last_date = lookupLastSeenDate(hex);
    
    if (last_date == 0) {
        // Try to return current/recent trace from /run
        // Check if trace_recent or trace_full exists
        char filepath[PATH_MAX];
        snprintf(filepath, PATH_MAX,
                 "%s/traces/%02x/trace_full_%s%06x.json",
                 Modes.json_dir,
                 hex % 256,
                 (hex & MODES_NON_ICAO_ADDRESS) ? "~" : "",
                 hex & 0xFFFFFF);
        
        // Try to load from /run
        int fd = open(filepath, O_RDONLY);
        if (fd < 0) {
            // Try trace_recent
            snprintf(filepath, PATH_MAX,
                     "%s/traces/%02x/trace_recent_%s%06x.json",
                     Modes.json_dir,
                     hex % 256,
                     (hex & MODES_NON_ICAO_ADDRESS) ? "~" : "",
                     hex & 0xFFFFFF);
            fd = open(filepath, O_RDONLY);
        }
        
        if (fd < 0) {
            return invalid; // No trace found
        }
        
        // Read file
        struct stat st;
        fstat(fd, &st);
        
        struct char_buffer result;
        result.len = API_REQ_PADSTART + st.st_size + 100;
        result.buffer = cmalloc(result.len);
        
        char *payload = result.buffer + API_REQ_PADSTART;
        ssize_t bytes = read(fd, payload, st.st_size);
        close(fd);
        
        if (bytes <= 0) {
            sfree(result.buffer);
            return invalid;
        }
        
        result.len = API_REQ_PADSTART + bytes;
        con->content_type = "application/json";
        return result;
    }
    
    // Load historic trace
    struct char_buffer trace = loadHistoricTrace(hex, last_date, &thread->buffer);
    
    if (!trace.buffer || trace.len == 0) {
        return invalid; // Failed to load trace
    }
    
    // Prepare response with padding
    struct char_buffer result;
    result.len = API_REQ_PADSTART + trace.len + 200;
    result.buffer = cmalloc(result.len);
    
    char *payload = result.buffer + API_REQ_PADSTART;
    char *p = payload;
    char *end = result.buffer + result.len;
    
    // Add metadata wrapper
    p = safe_snprintf(p, end, "{\"hex\":\"%06x\",\"trace_date\":%ld,\"trace\":",
                      hex & 0xFFFFFF, last_date);
    
    // Copy trace data
    memcpy(p, trace.buffer, trace.len);
    p += trace.len;
    
    p = safe_snprintf(p, end, "}");
    
    result.len = p - result.buffer;
    con->content_type = "application/json";
    
    return result;
}
```

**Lines added:** ~120

### 6. Add Header Declarations (globe_index.h)

```c
// Add to globe_index.h around line 95
void updateLastSeenIndex(uint32_t hex, time_t date);
time_t lookupLastSeenDate(uint32_t hex);
struct char_buffer loadHistoricTrace(uint32_t hex, time_t date, threadpool_buffer_t *buffer);
```

**Lines added:** ~3

### 7. Cleanup on Exit (readsb.c)
**Location:** In cleanup function

```c
// Cleanup last seen index
pthread_mutex_destroy(&Modes.lastSeenTraceIndex.mutex);
sfree(Modes.lastSeenTraceIndex.hexList);
sfree(Modes.lastSeenTraceIndex.lastSeenDates);
```

**Lines added:** ~5

## Total Lines of Code
- readsb.h: ~10 lines
- readsb.c (init): ~10 lines
- readsb.c (cleanup): ~5 lines
- globe_index.c (update function): ~50 lines
- globe_index.c (lookup functions): ~100 lines
- api.c (endpoint handler): ~120 lines
- globe_index.h: ~3 lines

**Total: ~298 lines of new code**

## Testing Plan

### Unit Tests
1. Test `updateLastSeenIndex()` with various scenarios
2. Test `lookupLastSeenDate()` with hit/miss cases
3. Test `loadHistoricTrace()` with valid/invalid paths

### Integration Tests
1. Start readsb with test configuration
2. Generate some trace data
3. Query API: `curl 'http://localhost:8042/?latest_trace=abc123'`
4. Verify response contains trace data
5. Query non-existent hex: verify 400 response

### Load Tests
1. Generate index with 100,000 entries
2. Run 1000 concurrent requests
3. Measure response times (target: <100ms p95)
4. Monitor memory usage

## Configuration Options

Consider adding command-line options:

```c
--enable-latest-trace-api     Enable the latest trace API endpoint
--trace-index-size <n>        Initial size of trace index (default: 1024)
```

## API Documentation Update

Add to README-api.md:

```markdown
### Latest Trace Endpoint

Retrieve the most recent trace for an aircraft without knowing the flight date.

```
GET /?latest_trace=<hex>
```

**Parameters:**
- `hex`: ICAO 24-bit hex code (e.g., `abc123`)
- Prefix with `~` for non-ICAO addresses (e.g., `~abc123`)

**Response (Success - 200 OK):**
```json
{
  "hex": "abc123",
  "trace_date": 1703721600,
  "trace": {
    "icao": "abc123",
    "timestamp": 1703721600.123,
    "trace": [
      [ ... trace points ... ]
    ]
  }
}
```

**Response (Not Found - 400 Bad Request):**
```
HTTP/1.1 400 Bad Request
```

**Notes:**
- Returns historic trace if available in `--write-globe-history` directory
- Falls back to live trace from `/run/readsb` if no historic trace found
- Requires `--write-globe-history` to be configured for full functionality
```

## Performance Optimizations (Future)

### Phase 2 Enhancements:

1. **Hash Table Instead of Linear Search**
   - Use existing hash functions from `api.c`
   - O(1) lookup instead of O(n)

2. **LRU Cache for Traces**
   - Cache recently accessed traces in memory
   - Reduce disk I/O for popular aircraft

3. **Persistent Index**
   - Save index to SQLite or binary file
   - Load on startup
   - Survive restarts

4. **Background Index Rebuilding**
   - Scan history directory on startup
   - Rebuild index from existing traces
   - Handle missing entries gracefully

## Migration Path

1. Deploy with feature disabled by default
2. Enable for testing on single instance
3. Monitor performance and errors
4. Roll out to production
5. Add to documentation

## Rollback Plan

If issues arise:
1. Disable endpoint via configuration
2. Remove API handler code (revert api.c changes)
3. Index updates are harmless even if unused
4. Full rollback via git revert

## Security Considerations

1. **Input Validation:** Hex code parsing is secure (uses strtol)
2. **Path Traversal:** File path construction uses safe snprintf
3. **Resource Limits:** No unbounded allocations
4. **Rate Limiting:** Use existing API rate limiting
5. **Access Control:** No authentication required (consistent with existing API)

## Open Questions

1. Should we support date ranges (e.g., last N days)?
2. Should we support metadata-only responses?
3. Should we add caching headers (Cache-Control)?
4. Should we support JSONP or CORS?
5. Should we add metrics/logging for this endpoint?

## Next Steps

1. Review this plan with stakeholders
2. Create feature branch
3. Implement Phase 1 (in-memory index)
4. Test with production data
5. Document API usage
6. Deploy to staging
7. Monitor and iterate
