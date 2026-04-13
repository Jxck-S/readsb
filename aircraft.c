#include "readsb.h"

void freeAircraftBack() {
    while (Modes.aircraftBack) {
        struct aircraftBack *prev = Modes.aircraftBack->prev;
        if (Modes.thp) {
            cmMunmap(Modes.aircraftBack, aircraftBackAlloc);
        } else {
            sfree(Modes.aircraftBack);
        }
        Modes.aircraft_data_size -= aircraftBackAlloc;
        Modes.aircraftBack = prev;
    }
}

static inline void lockBack() {
    pthread_mutex_lock(&Modes.aircraftBackMutex);
    //spinLock(&Modes.aircraftBackSpinlock);
}
static inline void unlockBack() {
    pthread_mutex_unlock(&Modes.aircraftBackMutex);
    //spinRelease(&Modes.aircraftBackSpinlock);
}

static struct aircraft *allocAircraft() {
    struct aircraft *a = NULL;

    lockBack();

    if (Modes.aircraftBackFree) {
        //fprintf(stderr, "alloc from freelist\n");
        a = Modes.aircraftBackFree;
        Modes.aircraftBackFree = a->next;

        unlockBack();
        return a;
    }
    if (!Modes.aircraftBack) {
        Modes.aircraft_data_size += aircraftBackAlloc;
        if (Modes.thp) {
            Modes.aircraftBack = cmMmap(aircraftBackAlloc);
        } else {
            Modes.aircraftBack = cmalloc(aircraftBackAlloc);
        }
        Modes.aircraftBack->used = 0;
        Modes.aircraftBack->prev = NULL;
        Modes.aircraftBack->next = NULL;
    }
    if (Modes.aircraftBack->used >= aircraftBackCap) {
        struct aircraftBack *prev = Modes.aircraftBack;
        Modes.aircraft_data_size += aircraftBackAlloc;
        if (Modes.thp) {
            Modes.aircraftBack = cmMmap(aircraftBackAlloc);
        } else {
            Modes.aircraftBack = cmalloc(aircraftBackAlloc);
        }
        Modes.aircraftBack->used = 0;
        Modes.aircraftBack->prev = prev;
        prev->next = Modes.aircraftBack;
        Modes.aircraftBack->next = NULL;
    }

    if (!Modes.aircraftBack || Modes.aircraftBack->used >= aircraftBackCap) {
        fprintf(stderr, "FATAL allocAircraft\n");
        abort();
    }

    //fprintf(stderr, "alloc fresh\n");

    a = &(Modes.aircraftBack->store[Modes.aircraftBack->used]);
    Modes.aircraftBack->used++;

    unlockBack();

    return a;
}
static void deallocAircraft(struct aircraft *a) {
    if (Modes.quickFree) {
        return;
    }

    lockBack();

    a->next = Modes.aircraftBackFree;
    Modes.aircraftBackFree = a;

    unlockBack();
}

static int isMilRange(uint32_t i);
static void updateDetails(struct aircraft *curr, struct char_buffer cb, uint32_t offset);

static inline uint32_t dbHash(uint32_t addr) {
    return addrHash(addr, DB_HASH_BITS);
}

uint32_t aircraftHash(uint32_t addr) {
    return addrHash(addr, Modes.acHashBits);
}

#define EMPTY 0xFFFFFFFF
#define quickMinBits 8
#define quickMaxBits 16
#define quickStride 8
static int quickBits;
static int quickBuckets;
static int quickSize;

struct ap {
    uint32_t addr;
    struct aircraft *ptr;
};

static struct ap *quick;

static void quickResize(int bits) {
    quickBits = bits;
    quickBuckets = (1LL << bits) + quickStride;
    quickSize = sizeof(struct ap) * quickBuckets;

    if (quickBuckets > 64000)
        fprintf(stderr, "quickLookup: changing size to %d!\n", (int) quickBuckets);

    sfree(quick);
    quick = cmalloc(quickSize);
    memset(quick, 0xFF, quickSize);
}

static struct ap *quickGet(uint32_t addr) {
    uint32_t hash = addrHash(addr, quickBits);
    for (unsigned i = 0; i < quickStride; i++) {
        struct ap *q = &(quick[hash + i]);
        if (q->addr == addr) {
            return q;
        }
    }
    return NULL;
}

static void quickMaintenance() {
    for (int i = 0; i < quickBuckets; i++) {
        struct ap *q = &(quick[i]);
        if (q->addr != EMPTY) {
            if (!q->ptr->onActiveList) {
                q->addr = EMPTY;
                q->ptr = NULL;
            }
        }
    }
}

void quickRemove(struct aircraft *a) {
    struct ap *q = quickGet(a->addr);
    if (q) {
        q->addr = EMPTY;
        q->ptr = NULL;
    }
    //fprintf(stderr, "r: %06x\n", a->addr);
}
void quickAdd(struct aircraft *a) {
    struct ap *q = quickGet(a->addr);
    if (q)
        return;

    uint32_t hash = addrHash(a->addr, quickBits);

    for (unsigned i = 0; i < quickStride; i++) {
        q = &quick[hash + i];
        if (q->addr == EMPTY) {
            q->addr = a->addr;
            q->ptr = a;
            return;
        }
    }
}

void quickInit() {
    if (quickBits > quickMinBits && Modes.aircraftActive.len < quickBuckets * 2 / 8) {
        quickResize(quickBits - 1);
    } else if (quickBits < quickMinBits) {
        quickResize(quickMinBits);
    } else if (quickBits < quickMaxBits && Modes.aircraftActive.len > quickBuckets * 5 / 8) {
        quickResize(quickBits + 1);
    }

    static int64_t nextMaintenance;
    int64_t mono = mono_milli_seconds();

    if (mono > nextMaintenance) {
        quickMaintenance();
        nextMaintenance = mono + 1 * MINUTES;
    }

    /*
    for (int i = 0; i < quickBuckets; i++) {
        if (quick[i].addr == EMPTY)
            fprintf(stderr, " ");
        else
            fprintf(stderr, ".");
    }
    fprintf(stderr, "\n");
    */
}
void quickDestroy() {
    sfree(quick);
}


struct aircraft *aircraftGet(uint32_t addr) {

    struct ap *q = quickGet(addr);
    if (q) {
        return q->ptr;
    }

    struct aircraft *a = Modes.aircraft[aircraftHash(addr)];

    while (a && a->addr != addr) {
        a = a->next;
    }
    if (a) {
        quickAdd(a);
    }
    return a;
}

void freeAircraft(struct aircraft *a) {
    if (Modes.quickFree) {
        traceCleanupNoUnlink(a);
        deallocAircraft(a);
        return;
    }

    quickRemove(a);

    // remove from the globeList
    set_globe_index(a, -5);

    if (a->onActiveList) {
        ca_remove(&Modes.aircraftActive, a);
    }
    traceCleanup(a);

    memset(a, 0x0, sizeof (struct aircraft));
    deallocAircraft(a);
}

void aircraftZeroTail(struct aircraft *a) {
    memset(&a->zeroStart, 0x0, &a->zeroEnd - &a->zeroStart);
}

struct aircraft *aircraftCreate(uint32_t addr) {
    struct aircraft *a = aircraftGet(addr);
    if (a) {
        fprintf(stderr, "<3>hex: %06x, aicraftCreate(): returning already existing aircraft\n", addr);
        return a;
    }
    a = allocAircraft();

    // Default everything to zero/NULL
    memset(a, 0, sizeof (struct aircraft));

    // Now initialise things that should not be 0/NULL to their defaults
    a->addr = addr;
    a->addrtype = ADDR_UNKNOWN;

    // defaults until we see a message otherwise
    a->adsb_version = -1;
    a->adsb_hrd = HEADING_MAGNETIC;
    a->adsb_tah = HEADING_GROUND_TRACK;

    if (Modes.json_globe_index) {
        a->globe_index = -5;
    }

    // initialize data validity ages
    //adjustExpire(a, 58);

    updateTypeReg(a);

    uint32_t hash = aircraftHash(addr);

    // this isn't needed as this happens on separate areas of the hashtable when it's called in
    // parallel
    //pthread_mutex_lock(&Modes.aircraftCreateMutex);

    a->next = Modes.aircraft[hash];
    Modes.aircraft[hash] = a;

    //pthread_mutex_unlock(&Modes.aircraftCreateMutex);

    return a;
}

void toBinCraft(struct aircraft *a, struct binCraft *new, int64_t now) {

    memset(new, 0, sizeof(struct binCraft));
    new->hex = a->addr;
    new->seen = (int32_t) nearbyint((now - a->seen) / 100.0);

    new->callsign_valid = trackDataValid(&a->callsign_valid);
    for (unsigned i = 0; i < sizeof(new->callsign); i++)
        new->callsign[i] = a->callsign[i] * new->callsign_valid;

    if (Modes.db) {
        memcpy(new->registration, a->registration, sizeof(new->registration));
        memcpy(new->typeCode, a->typeCode, sizeof(new->typeCode));
        new->dbFlags = a->dbFlags;
    }
    new->extraFlags |= ((nogps(now, a)) << 0);

    if (Modes.json_globe_index || Modes.apiShutdownDelay) {
        new->messages = (uint16_t) nearbyint(10 * a->messageRate);
    } else {
        new->messages = (uint16_t) a->messages;
    }

    new->position_valid = trackDataValid(&a->pos_reliable_valid);

    if (new->position_valid || now < a->seenPosReliable + 14 * 24 * HOURS) {
        new->seen_pos = (int32_t) nearbyint((now - a->seenPosReliable) / 100.0);
        new->lat = (int32_t) nearbyint(a->latReliable * 1E6);
        new->lon = (int32_t) nearbyint(a->lonReliable * 1E6);
        new->pos_nic = a->pos_nic_reliable;
        new->pos_rc = a->pos_rc_reliable;
    }

    new->baro_alt_valid = altBaroReliable(a);

    new->baro_alt = (int16_t) nearbyint(a->baro_alt * BINCRAFT_ALT_FACTOR);

    new->geom_alt = (int16_t) nearbyint(a->geom_alt * BINCRAFT_ALT_FACTOR);
    new->baro_rate = (int16_t) nearbyint(a->baro_rate / 8.0);
    new->geom_rate = (int16_t) nearbyint(a->geom_rate / 8.0);
    new->ias = a->ias;
    new->tas = a->tas;

    new->squawk = a->squawk;
    new->category = a->category * (now < a->category_updated + Modes.trackExpireJaero);
    // Aircraft category A0 - D7 encoded as a single hex byte. 00 = unset
    new->nav_altitude_mcp = (uint16_t) nearbyint(a->nav_altitude_mcp / 4.0);
    new->nav_altitude_fms = (uint16_t) nearbyint(a->nav_altitude_fms / 4.0);

    new->nav_qnh = (int16_t) nearbyint(a->nav_qnh * 10.0);
    new->gs = (int16_t) nearbyint(a->gs * 10.0);
    new->mach = (int16_t) nearbyint(a->mach * 1000.0);

    new->track_rate = (int16_t) nearbyint(a->track_rate * 100.0);
    new->roll = (int16_t) nearbyint(a->roll * 100.0);

    if (trackDataValid(&a->track_valid))
        new->track = (int16_t) nearbyint(a->track * 90.0);
    else
        new->track = (int16_t) nearbyint(a->calc_track * 90.0);

    new->mag_heading = (int16_t) nearbyint(a->mag_heading * 90.0);
    new->true_heading = (int16_t) nearbyint(a->true_heading * 90.0);
    new->nav_heading = (int16_t) nearbyint(a->nav_heading * 90.0);

    new->emergency = a->emergency;
    new->airground = a->airground * trackDataValid(&a->airground_valid);

    new->addrtype = a->addrtype;
    new->nav_modes = a->nav_modes;
    new->nav_altitude_src = a->nav_altitude_src;
    new->sil_type = a->sil_type;

    new->wind_valid = (now < a->wind_updated + TRACK_EXPIRE && abs(a->wind_altitude - a->baro_alt) < 500);
    new->wind_direction = (int) nearbyint(a->wind_direction) * new->wind_valid;
    new->wind_speed = (int) nearbyint(a->wind_speed) * new->wind_valid;

    new->temp_valid = (now < a->oat_updated + TRACK_EXPIRE);
    new->oat = (int) nearbyint(a->oat) * new->temp_valid;
    new->tat = (int) nearbyint(a->tat) * new->temp_valid;

    if (a->adsb_version < 0)
        new->adsb_version = 15;
    else
        new->adsb_version = a->adsb_version;

    if (a->adsr_version < 0)
        new->adsr_version = 15;
    else
        new->adsr_version = a->adsr_version;

    if (a->tisb_version < 0)
        new->tisb_version = 15;
    else
        new->tisb_version = a->tisb_version;

    new->nic_a = a->nic_a;
    new->nic_c = a->nic_c;
    new->nic_baro = a->nic_baro;
    new->nac_p = a->nac_p;
    new->nac_v = a->nac_v;
    new->sil = a->sil;
    new->gva = a->gva;
    new->sda = a->sda;
    new->alert = a->alert;
    new->spi = a->spi;

    //new->signal = get8bitSignal(a); old version
    new->signal = nearbyint((getSignal(a) + 50.0f) * (255.0f / 50.0f));
    //fprintf(stderr, "%0.1f %u\n", getSignal(a), new->signal);

#if defined(TRACKS_UUID)
    new->receiverId = (uint32_t) (a->receiverId >> 32);
#endif

    if (Modes.netReceiverId) {
        new->receiverCount = a->receiverCount;
    }

#define F(f) do { new->f##_valid = trackDataValid(&a->f##_valid); new->f *= new->f##_valid; } while (0)
    F(geom_alt);
    F(gs);
    F(ias);
    F(tas);
    F(mach);
    F(track);
    F(track_rate);
    F(roll);
    F(mag_heading);
    F(true_heading);
    F(baro_rate);
    F(geom_rate);
    F(nic_a);
    F(nic_c);
    F(nic_baro);
    F(nac_p);
    F(nac_v);
    F(sil);
    F(gva);
    F(sda);
    F(squawk);
    F(emergency);
    F(nav_qnh);
    F(nav_altitude_mcp);
    F(nav_altitude_fms);
    F(nav_altitude_src);
    F(nav_heading);
    F(nav_modes);
    F(alert);
    F(spi);
#undef F
}


// fromBinCraft: inverse of toBinCraft — apply a binCraft snapshot record to the local aircraft state.
// remote_now is the int64 "now" field from the binCraft file header (absolute milliseconds on the remote).
// The local "now" is derived so that relative timestamps in the record are correctly translated.
void fromBinCraft(struct binCraft *b, int64_t remote_now) {
    if (!b || b->hex == 0 || b->hex > 0xFFFFFF)
        return;

    int64_t local_now = mstime();

    // Translate relative centisecond offsets from the remote snapshot back to absolute local timestamps.
    // b->seen and b->seen_pos are centiseconds *ago* relative to remote_now.
    int64_t seen_abs = remote_now - (int64_t)b->seen * 100;
    // Age of the snapshot itself
    int64_t snapshot_age = local_now - remote_now;
    // Clamp: if snapshot is from the future (clock skew), treat as just arrived
    if (snapshot_age < 0) snapshot_age = 0;
    int64_t local_seen = seen_abs + snapshot_age;

    // Don't ingest data that is too old to matter
    if (local_now - local_seen > 30 * MINUTES)
        return;

    struct aircraft *a = aircraftGet(b->hex);
    if (!a)
        a = aircraftCreate(b->hex);
    if (!a)
        return;

    // Update the "last seen" timestamp only if the new data is fresher
    if (local_seen > a->seen)
        a->seen = local_seen;

    // -- addrtype --
    // Only raise, never lower addrtype (higher enum value = lower priority address)
    if (b->addrtype < a->addrtype)
        a->addrtype = b->addrtype;

    // -- callsign --
    if (b->callsign_valid && local_now - local_seen < TRACK_EXPIRE_LONG) {
        if (a->callsign_valid.source == SOURCE_INVALID ||
                a->callsign_valid.updated < local_seen) {
            memcpy(a->callsign, b->callsign, sizeof(b->callsign));
            a->callsign[sizeof(a->callsign) - 1] = '\0';
            a->callsign_valid.source = SOURCE_SBS;
            a->callsign_valid.last_source = SOURCE_SBS;
            a->callsign_valid.updated = local_seen;
            a->callsign_valid.stale = 0;
        }
    }

    // -- position --
    if (b->position_valid) {
        int64_t seen_pos_abs = remote_now - (int64_t)b->seen_pos * 100 + snapshot_age;
        if (seen_pos_abs > a->seenPosReliable && local_now - seen_pos_abs < 30 * MINUTES) {
            double new_lat = b->lat / 1E6;
            double new_lon = b->lon / 1E6;
            // Only accept if coordinates changed or position is brand new
            if (a->seenPosReliable == 0 ||
                    (int32_t)(new_lat * 1E6) != (int32_t)(a->latReliable * 1E6) ||
                    (int32_t)(new_lon * 1E6) != (int32_t)(a->lonReliable * 1E6)) {
                a->latReliable = new_lat;
                a->lonReliable = new_lon;
                a->lat = new_lat;
                a->lon = new_lon;
                a->seenPosReliable = seen_pos_abs;
                a->seen_pos = seen_pos_abs;
                a->pos_nic = b->pos_nic;
                a->pos_rc = b->pos_rc;
                a->pos_nic_reliable = b->pos_nic;
                a->pos_rc_reliable = b->pos_rc;
                a->pos_reliable_odd = fmaxf(a->pos_reliable_odd, Modes.json_reliable);
                a->pos_reliable_even = fmaxf(a->pos_reliable_even, Modes.json_reliable);
                a->pos_reliable_valid.source = SOURCE_SBS;
                a->pos_reliable_valid.last_source = SOURCE_SBS;
                a->pos_reliable_valid.updated = seen_pos_abs;
                a->pos_reliable_valid.stale = 0;
                a->position_valid.source = SOURCE_SBS;
                a->position_valid.last_source = SOURCE_SBS;
                a->position_valid.updated = seen_pos_abs;
                a->position_valid.stale = 0;
                if (Modes.json_globe_index)
                    set_globe_index(a, globe_index(new_lat, new_lon));
            }
        }
    }

    float alt_scale_factor = 1.0f / BINCRAFT_ALT_FACTOR;

    if (b->baro_alt_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->baro_alt_valid.source == SOURCE_INVALID || a->baro_alt_valid.updated < local_seen) {
            a->baro_alt = (int32_t)(b->baro_alt * alt_scale_factor);
            a->baro_alt_valid.source = SOURCE_SBS;
            a->baro_alt_valid.last_source = SOURCE_SBS;
            a->baro_alt_valid.updated = local_seen;
            a->baro_alt_valid.stale = 0;
        }
    }
    if (b->geom_alt_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->geom_alt_valid.source == SOURCE_INVALID || a->geom_alt_valid.updated < local_seen) {
            a->geom_alt = (int32_t)(b->geom_alt * alt_scale_factor);
            a->geom_alt_valid.source = SOURCE_SBS;
            a->geom_alt_valid.last_source = SOURCE_SBS;
            a->geom_alt_valid.updated = local_seen;
            a->geom_alt_valid.stale = 0;
        }
    }

    if (b->baro_rate_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->baro_rate_valid.source == SOURCE_INVALID || a->baro_rate_valid.updated < local_seen) {
            a->baro_rate = (int32_t)b->baro_rate * 8;
            a->baro_rate_valid.source = SOURCE_SBS;
            a->baro_rate_valid.last_source = SOURCE_SBS;
            a->baro_rate_valid.updated = local_seen;
            a->baro_rate_valid.stale = 0;
        }
    }
    if (b->geom_rate_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->geom_rate_valid.source == SOURCE_INVALID || a->geom_rate_valid.updated < local_seen) {
            a->geom_rate = (int32_t)b->geom_rate * 8;
            a->geom_rate_valid.source = SOURCE_SBS;
            a->geom_rate_valid.last_source = SOURCE_SBS;
            a->geom_rate_valid.updated = local_seen;
            a->geom_rate_valid.stale = 0;
        }
    }

    if (b->gs_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->gs_valid.source == SOURCE_INVALID || a->gs_valid.updated < local_seen) {
            a->gs = b->gs / 10.0f;
            a->gs_valid.source = SOURCE_SBS;
            a->gs_valid.last_source = SOURCE_SBS;
            a->gs_valid.updated = local_seen;
            a->gs_valid.stale = 0;
        }
    }

    if (b->ias_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->ias_valid.source == SOURCE_INVALID || a->ias_valid.updated < local_seen) {
            a->ias = b->ias;
            a->ias_valid.source = SOURCE_SBS;
            a->ias_valid.last_source = SOURCE_SBS;
            a->ias_valid.updated = local_seen;
            a->ias_valid.stale = 0;
        }
    }
    if (b->tas_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->tas_valid.source == SOURCE_INVALID || a->tas_valid.updated < local_seen) {
            a->tas = b->tas;
            a->tas_valid.source = SOURCE_SBS;
            a->tas_valid.last_source = SOURCE_SBS;
            a->tas_valid.updated = local_seen;
            a->tas_valid.stale = 0;
        }
    }
    if (b->mach_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->mach_valid.source == SOURCE_INVALID || a->mach_valid.updated < local_seen) {
            a->mach = b->mach / 1000.0f;
            a->mach_valid.source = SOURCE_SBS;
            a->mach_valid.last_source = SOURCE_SBS;
            a->mach_valid.updated = local_seen;
            a->mach_valid.stale = 0;
        }
    }

    if (b->track_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->track_valid.source == SOURCE_INVALID || a->track_valid.updated < local_seen) {
            a->track = b->track / 90.0f;
            a->track_valid.source = SOURCE_SBS;
            a->track_valid.last_source = SOURCE_SBS;
            a->track_valid.updated = local_seen;
            a->track_valid.stale = 0;
        }
    }
    if (b->track_rate_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->track_rate_valid.source == SOURCE_INVALID || a->track_rate_valid.updated < local_seen) {
            a->track_rate = b->track_rate / 100.0f;
            a->track_rate_valid.source = SOURCE_SBS;
            a->track_rate_valid.last_source = SOURCE_SBS;
            a->track_rate_valid.updated = local_seen;
            a->track_rate_valid.stale = 0;
        }
    }
    if (b->roll_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->roll_valid.source == SOURCE_INVALID || a->roll_valid.updated < local_seen) {
            a->roll = b->roll / 100.0f;
            a->roll_valid.source = SOURCE_SBS;
            a->roll_valid.last_source = SOURCE_SBS;
            a->roll_valid.updated = local_seen;
            a->roll_valid.stale = 0;
        }
    }
    if (b->mag_heading_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->mag_heading_valid.source == SOURCE_INVALID || a->mag_heading_valid.updated < local_seen) {
            a->mag_heading = b->mag_heading / 90.0f;
            a->mag_heading_valid.source = SOURCE_SBS;
            a->mag_heading_valid.last_source = SOURCE_SBS;
            a->mag_heading_valid.updated = local_seen;
            a->mag_heading_valid.stale = 0;
        }
    }
    if (b->true_heading_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->true_heading_valid.source == SOURCE_INVALID || a->true_heading_valid.updated < local_seen) {
            a->true_heading = b->true_heading / 90.0f;
            a->true_heading_valid.source = SOURCE_SBS;
            a->true_heading_valid.last_source = SOURCE_SBS;
            a->true_heading_valid.updated = local_seen;
            a->true_heading_valid.stale = 0;
        }
    }

    if (b->squawk_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->squawk_valid.source == SOURCE_INVALID || a->squawk_valid.updated < local_seen) {
            a->squawk = b->squawk;
            a->squawk_valid.source = SOURCE_SBS;
            a->squawk_valid.last_source = SOURCE_SBS;
            a->squawk_valid.updated = local_seen;
            a->squawk_valid.stale = 0;
        }
    }
    if (b->emergency_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->emergency_valid.source == SOURCE_INVALID || a->emergency_valid.updated < local_seen) {
            a->emergency = b->emergency;
            a->emergency_valid.source = SOURCE_SBS;
            a->emergency_valid.last_source = SOURCE_SBS;
            a->emergency_valid.updated = local_seen;
            a->emergency_valid.stale = 0;
        }
    }
    if (b->spi_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->spi_valid.source == SOURCE_INVALID || a->spi_valid.updated < local_seen) {
            a->spi = b->spi;
            a->spi_valid.source = SOURCE_SBS;
            a->spi_valid.last_source = SOURCE_SBS;
            a->spi_valid.updated = local_seen;
            a->spi_valid.stale = 0;
        }
    }
    if (b->alert_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->alert_valid.source == SOURCE_INVALID || a->alert_valid.updated < local_seen) {
            a->alert = b->alert;
            a->alert_valid.source = SOURCE_SBS;
            a->alert_valid.last_source = SOURCE_SBS;
            a->alert_valid.updated = local_seen;
            a->alert_valid.stale = 0;
        }
    }

    // -- airground --
    if (b->airground != AG_INVALID && local_now - local_seen < TRACK_EXPIRE_LONG) {
        if (a->airground_valid.source == SOURCE_INVALID || a->airground_valid.updated < local_seen) {
            a->airground = b->airground;
            a->airground_valid.source = SOURCE_SBS;
            a->airground_valid.last_source = SOURCE_SBS;
            a->airground_valid.updated = local_seen;
            a->airground_valid.stale = 0;
        }
    }

    // -- nav fields --
    if (b->nav_altitude_mcp_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->nav_altitude_mcp_valid.source == SOURCE_INVALID || a->nav_altitude_mcp_valid.updated < local_seen) {
            a->nav_altitude_mcp = (uint32_t)b->nav_altitude_mcp * 4;
            a->nav_altitude_mcp_valid.source = SOURCE_SBS;
            a->nav_altitude_mcp_valid.last_source = SOURCE_SBS;
            a->nav_altitude_mcp_valid.updated = local_seen;
            a->nav_altitude_mcp_valid.stale = 0;
        }
    }
    if (b->nav_altitude_fms_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->nav_altitude_fms_valid.source == SOURCE_INVALID || a->nav_altitude_fms_valid.updated < local_seen) {
            a->nav_altitude_fms = (uint32_t)b->nav_altitude_fms * 4;
            a->nav_altitude_fms_valid.source = SOURCE_SBS;
            a->nav_altitude_fms_valid.last_source = SOURCE_SBS;
            a->nav_altitude_fms_valid.updated = local_seen;
            a->nav_altitude_fms_valid.stale = 0;
        }
    }
    if (b->nav_altitude_src_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->nav_altitude_src_valid.source == SOURCE_INVALID || a->nav_altitude_src_valid.updated < local_seen) {
            a->nav_altitude_src = b->nav_altitude_src;
            a->nav_altitude_src_valid.source = SOURCE_SBS;
            a->nav_altitude_src_valid.last_source = SOURCE_SBS;
            a->nav_altitude_src_valid.updated = local_seen;
            a->nav_altitude_src_valid.stale = 0;
        }
    }
    if (b->nav_qnh_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->nav_qnh_valid.source == SOURCE_INVALID || a->nav_qnh_valid.updated < local_seen) {
            a->nav_qnh = b->nav_qnh / 10.0f;
            a->nav_qnh_valid.source = SOURCE_SBS;
            a->nav_qnh_valid.last_source = SOURCE_SBS;
            a->nav_qnh_valid.updated = local_seen;
            a->nav_qnh_valid.stale = 0;
        }
    }
    if (b->nav_heading_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->nav_heading_valid.source == SOURCE_INVALID || a->nav_heading_valid.updated < local_seen) {
            a->nav_heading = b->nav_heading / 90.0f;
            a->nav_heading_valid.source = SOURCE_SBS;
            a->nav_heading_valid.last_source = SOURCE_SBS;
            a->nav_heading_valid.updated = local_seen;
            a->nav_heading_valid.stale = 0;
        }
    }
    if (b->nav_modes_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->nav_modes_valid.source == SOURCE_INVALID || a->nav_modes_valid.updated < local_seen) {
            a->nav_modes = b->nav_modes;
            a->nav_modes_valid.source = SOURCE_SBS;
            a->nav_modes_valid.last_source = SOURCE_SBS;
            a->nav_modes_valid.updated = local_seen;
            a->nav_modes_valid.stale = 0;
        }
    }

    // -- wind / temp --
    if (b->wind_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (!a->wind_updated || a->wind_updated < local_seen) {
            a->wind_speed = b->wind_speed;
            a->wind_direction = b->wind_direction;
            a->wind_altitude = a->baro_alt;
            a->wind_updated = local_seen;
        }
    }
    if (b->temp_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (!a->oat_updated || a->oat_updated < local_seen) {
            a->oat = b->oat;
            a->oat_updated = local_seen;
        }
        if (!a->tat_updated || a->tat_updated < local_seen) {
            a->tat = b->tat;
            a->tat_updated = local_seen;
        }
    }

    // -- NIC / NAC / SIL / GVA / SDA validity bits --
    if (b->nic_a_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->nic_a_valid.source == SOURCE_INVALID || a->nic_a_valid.updated < local_seen) {
            a->nic_a = b->nic_a;
            a->nic_a_valid.source = SOURCE_SBS;
            a->nic_a_valid.last_source = SOURCE_SBS;
            a->nic_a_valid.updated = local_seen;
            a->nic_a_valid.stale = 0;
        }
    }
    if (b->nic_c_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->nic_c_valid.source == SOURCE_INVALID || a->nic_c_valid.updated < local_seen) {
            a->nic_c = b->nic_c;
            a->nic_c_valid.source = SOURCE_SBS;
            a->nic_c_valid.last_source = SOURCE_SBS;
            a->nic_c_valid.updated = local_seen;
            a->nic_c_valid.stale = 0;
        }
    }
    if (b->nic_baro_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->nic_baro_valid.source == SOURCE_INVALID || a->nic_baro_valid.updated < local_seen) {
            a->nic_baro = b->nic_baro;
            a->nic_baro_valid.source = SOURCE_SBS;
            a->nic_baro_valid.last_source = SOURCE_SBS;
            a->nic_baro_valid.updated = local_seen;
            a->nic_baro_valid.stale = 0;
        }
    }
    if (b->nac_p_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->nac_p_valid.source == SOURCE_INVALID || a->nac_p_valid.updated < local_seen) {
            a->nac_p = b->nac_p;
            a->nac_p_valid.source = SOURCE_SBS;
            a->nac_p_valid.last_source = SOURCE_SBS;
            a->nac_p_valid.updated = local_seen;
            a->nac_p_valid.stale = 0;
        }
    }
    if (b->nac_v_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->nac_v_valid.source == SOURCE_INVALID || a->nac_v_valid.updated < local_seen) {
            a->nac_v = b->nac_v;
            a->nac_v_valid.source = SOURCE_SBS;
            a->nac_v_valid.last_source = SOURCE_SBS;
            a->nac_v_valid.updated = local_seen;
            a->nac_v_valid.stale = 0;
        }
    }
    if (b->sil_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->sil_valid.source == SOURCE_INVALID || a->sil_valid.updated < local_seen) {
            a->sil = b->sil;
            a->sil_valid.source = SOURCE_SBS;
            a->sil_valid.last_source = SOURCE_SBS;
            a->sil_valid.updated = local_seen;
            a->sil_valid.stale = 0;
        }
    }
    if (b->gva_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->gva_valid.source == SOURCE_INVALID || a->gva_valid.updated < local_seen) {
            a->gva = b->gva;
            a->gva_valid.source = SOURCE_SBS;
            a->gva_valid.last_source = SOURCE_SBS;
            a->gva_valid.updated = local_seen;
            a->gva_valid.stale = 0;
        }
    }
    if (b->sda_valid && local_now - local_seen < TRACK_EXPIRE) {
        if (a->sda_valid.source == SOURCE_INVALID || a->sda_valid.updated < local_seen) {
            a->sda = b->sda;
            a->sda_valid.source = SOURCE_SBS;
            a->sda_valid.last_source = SOURCE_SBS;
            a->sda_valid.updated = local_seen;
            a->sda_valid.stale = 0;
        }
    }

    // -- ADS-B / ADS-R / TIS-B version --
    if (b->adsb_version != 15) { // 15 == not seen
        a->adsb_version = (int32_t)b->adsb_version;
    }
    if (b->adsr_version != 15) {
        a->adsr_version = (int32_t)b->adsr_version;
    }
    if (b->tisb_version != 15) {
        a->tisb_version = (int32_t)b->tisb_version;
    }

    a->sil_type = b->sil_type;

    // -- category --
    if (b->category && local_now - local_seen < TRACK_EXPIRE_LONG) {
        if (a->category == 0 || a->category_updated < local_seen) {
            a->category = b->category;
            a->category_updated = local_seen;
        }
    }

    // -- Add aircraft to active list so all outputs can see it --
    if (!a->onActiveList && includeAircraftJson(local_now, a)) {
        updateValidities(a, local_now);
        ca_add(&Modes.aircraftActive, a);
        a->onActiveList = 1;
    }
}

static inline void sanitize(char *str, int len) {
    unsigned char b2 = (1<<7) + (1<<6); // 2 byte code or more
    unsigned char b3 = (1<<7) + (1<<6) + (1<<5); // 3 byte code or more
    unsigned char b4 = (1<<7) + (1<<6) + (1<<5) + (1<<4); // 4 byte code

    int debug = 0;

    // UTF that goes beyond our string is cut off by terminating the string

    if (len >= 3 && (str[len - 3] & b4) == b4) {
        if (debug) { fprintf(stderr, "b4%d\n", str[len - 3]); }
        str[len - 3] = '\0';
    }
    if (len >= 2 && (str[len - 2] & b3) == b3) {
        if (debug) { fprintf(stderr, "b3%d\n", str[len - 2]); }
        str[len - 2] = '\0';
    }
    if (len >= 1 && (str[len - 1] & b2) == b2) {
        if (debug) { fprintf(stderr, "b2%d\n", str[len - 1]); }
        str[len - 1] = '\0';
    }
    char *p = str;
    // careful str might be unterminated, use len
    while(p - str < len) {
        if (*p == '"') {
            //if (debug) { fprintf(stderr, "quotation marks: %s\n", str); }
            // replace with single quote
            *p = '\'';
        }
        if (*p > 0 && *p < 0x1f) {
            if (debug) { fprintf(stderr, "non-printable: %s\n", str); }
            // replace with space
            *p = ' ';
        }
        p++;
    }
    if (p - 1 >= str && *(p - 1) == '\\') {
        *(p - 1) = '\0';
    }
}
static char *sprintDB2(char *p, char *end, dbEntry *d) {
    struct aircraft aBack;
    struct aircraft *a = &aBack;

    updateDetails(a, Modes.db2Raw, d->rawOffset);
    a->addr = d->addr;
    p = safe_snprintf(p, end, "\n\"%s%06x\":{", (a->addr & MODES_NON_ICAO_ADDRESS) ? "~" : "", a->addr & 0xFFFFFF);
    char *regInfo = p;
    if (a->registration[0])
        p = safe_snprintf(p, end, "\"r\":\"%.*s\",", (int) sizeof(a->registration), a->registration);
    if (a->typeCode[0])
        p = safe_snprintf(p, end, "\"t\":\"%.*s\",", (int) sizeof(a->typeCode), a->typeCode);
    if (a->typeLong[0])
        p = safe_snprintf(p, end, "\"desc\":\"%.*s\",", (int) sizeof(a->typeLong), a->typeLong);
    if (a->dbFlags)
        p = safe_snprintf(p, end, "\"dbFlags\":%u,", a->dbFlags);
    if (a->ownOp[0])
        p = safe_snprintf(p, end, "\"ownOp\":\"%.*s\",", (int) sizeof(a->ownOp), a->ownOp);
    if (a->year[0])
        p = safe_snprintf(p, end, "\"year\":\"%.*s\",", (int) sizeof(a->year), a->year);
    if (p == regInfo)
        p = safe_snprintf(p, end, "\"noRegData\":true,");
    if (*(p-1) == ',')
        p--;
    p = safe_snprintf(p, end, "},");
    return p;
}
static void db2ToJson() {
    size_t buflen = 32 * 1024 * 1024;
    char *buf = (char *) cmalloc(buflen), *p = buf, *end = buf + buflen;
    p = safe_snprintf(p, end, "{");

    for (int j = 0; j < DB_BUCKETS; j++) {
        if (0 && j % 1000 == 0) {
            fprintf(stderr, "db print %d\n", j);
        }
        for (dbEntry *d = Modes.db2Index[j]; d; d = d->next) {
            p = sprintDB2(p, end, d);
            if ((p + 1000) >= end) {
                int used = p - buf;
                buflen *= 2;
                buf = (char *) realloc(buf, buflen);
                p = buf + used;
                end = buf + buflen;
            }
        }
    }

    if (*(p-1) == ',')
        p--;
    p = safe_snprintf(p, end, "\n}");
    struct char_buffer cb2;
    cb2.len = p - buf;
    cb2.buffer = buf;
    writeJsonToFile(Modes.json_dir, "db.json", cb2); // location changed
    free(buf);
}

// get next CSV token based on the assumption eot points to the previous delimiter
static inline int nextToken(char delim, char **sot, char **eot, char **eol) {
    *sot = *eot + 1;
    if (*sot >= *eol)
        return 0;
    *eot = memchr(*sot, delim, *eol - *sot);

    if (!*eot)
        return 0;

    return 1;
}

static int is_df18_exception(uint32_t addr) {
    switch (addr) {
        case 0xa08508:
        case 0xab33a0:
        case 0xa7d24c:
        case 0xa6e2cd:
        case 0xaa8fca:
        case 0xac808b:
        case 0x48f6f7:
        case 0x7cbc3d:
        case 0x7c453a:
        case 0x401cf9:
        case 0x40206a:
        case 0xa3227d:
        case 0x478676:
        case 0x40389d:
        case 0x405acf:
        case 0xc82452:
        case 0x40334a:
            return 1;
        default:
            return 0;
    }
}


// meant to be used with this DB: https://raw.githubusercontent.com/wiedehopf/tar1090-db/csv/aircraft.csv.gz
int dbUpdate(int64_t now) {
    static int64_t next_db_check;
    if (now < next_db_check) {
        return 0;
    }
    // db update check every 30 seconds
    next_db_check = now + 30 * SECONDS;
    // this update only takes effect in dbFinishUpdate with all other threads locked

    char *filename = Modes.db_file;
    if (!filename) {
        return 0;
    }

    struct timespec watch;
    startWatch(&watch);

    gzFile gzfp = NULL;
    struct char_buffer cb = {0};

    int fd = open(filename, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "dbUpdate: open db-file failed:");
        perror(filename);
        return 0;
    }

    //fprintf(stderr, "checking for db Update\n");

    struct stat fileinfo = {0};
    if (fstat(fd, &fileinfo)) {
        fprintf(stderr, "%s: dbUpdate: fstat failed, wat?!\n", filename);
        goto DBU0;
    }
    int64_t modTime = fileinfo.st_mtim.tv_sec;

    if (Modes.dbModificationTime == modTime)
        goto DBU0;

    gzfp = gzdopen(fd, "r");
    if (!gzfp) {
        fprintf(stderr, "db update error: gzdopen failed.\n");
        goto DBU0;
    }


    cb = readWholeGz(gzfp, filename);
    if (!cb.buffer) {
        fprintf(stderr, "database read failed due to readWholeGz.\n");
        goto DBU0;
    }
    if (cb.len < 1000) {
        fprintf(stderr, "database file very small, bailing out of dbUpdate.\n");
        goto DBU0;
    }

    if (1) {
        // reallocate so we don't waste memory
        char *oldBuffer = cb.buffer;
        cb.len++; // for adding zero termination
        cb.buffer = realloc(cb.buffer, cb.len);
        if (!cb.buffer) {
            fprintf(stderr, "database read failed due to realloc\n");
            sfree(oldBuffer);
            goto DBU0;
        }
        cb.buffer[cb.len - 1] = '\0'; // zero terminate for good measure
    }

    int alloc = 0;

    // memchr is not faster, seems the compiler is smart enough to optimize a simple loop that counts the newlines
    for (uint32_t i = 0; i < cb.len; i++) {
        if (cb.buffer[i] == '\n')
            alloc++;
    }

    int indexSize = DB_BUCKETS * sizeof(void*);
    int entriesSize = alloc * sizeof(dbEntry);

    Modes.db2 = cmalloc(entriesSize);
    Modes.db2Raw = cb;
    Modes.db2Index = cmalloc(indexSize);
    memset(Modes.db2Index, 0, indexSize);

    if (0) {
        fprintf(stderr, "db mem usage: total %d index %d entries %d text %d kB\n",
                indexSize / 1024 +  entriesSize / 1024 +  (int) (cb.len / 1024),
                indexSize / 1024, entriesSize / 1024, (int) (cb.len / 1024));
    }

    if (!Modes.db2 || !Modes.db2Index) {
        fprintf(stderr, "db update error: malloc failure!\n");
        goto DBU0;
    }

    fprintf(stderr, "Database update in progress!\n");

    char *eob = cb.buffer + cb.len;
    char *sol = cb.buffer;
    char *eol;
    int i;
    for (i = 0; eob > sol && (eol = memchr(sol, '\n', eob - sol)); sol = eol + 1) {

        char *sot;
        char *eot = sol - 1; // this pointer must not be dereferenced, nextToken will increment it.

        dbEntry *curr = &Modes.db2[i];
        memset(curr, 0, sizeof(dbEntry));

        if (!nextToken(';', &sot, &eot, &eol)) continue;
        curr->addr = strtol(sot, NULL, 16);
        if (curr->addr == 0)
            continue;

        if (!nextToken(';', &sot, &eot, &eol)) continue;
        curr->rawOffset = sot - cb.buffer;

        i++; // increment db array index
        // add to hashtable
        dbPut(curr->addr, Modes.db2Index, curr);
    }

    if (i < 1) {
        fprintf(stderr, "db update error: DB has no entries, maybe old / incorrect format?!\n");
        goto DBU0;
    }
    //fflush(stdout);

    //fprintf(stderr, "dbUpdate() done\n");

    gzclose(gzfp);
    Modes.dbModificationTime = modTime;
    if (Modes.json_dir) {
        free(writeJsonToFile(Modes.json_dir, "receiver.json", generateReceiverJson()).buffer);
    }


    double elapsed = stopWatch(&watch) / 1000.0;
    fprintf(stderr, "Database update first part took %.3f seconds!\n", elapsed);


    // write database to json dir for testing
    if (Modes.json_dir && Modes.debug_dbJson) {
        db2ToJson();
    }

    return 1;
DBU0:
    if (gzfp)
        gzclose(gzfp);
    free(cb.buffer);
    free(Modes.db2);
    free(Modes.db2Index);
    Modes.db2 = NULL;
    Modes.db2Index = NULL;
    Modes.db2Raw.buffer = NULL;
    Modes.db2Raw.len = 0;
    close(fd);
    return 1;
}

static void updateTypeRegRange(void *arg, threadpool_threadbuffers_t *threadbuffers) {
    MODES_NOTUSED(threadbuffers);
    readsb_task_t *info = (readsb_task_t *) arg;
    //fprintf(stderr, "%d %d\n", info->from, info->to);
    for (int j = info->from; j < info->to; j++) {
        for (struct aircraft *a = Modes.aircraft[j]; a; a = a->next) {
            updateTypeReg(a);
        }
    }
}

int dbFinishUpdate() {
    if (!Modes.db2) {
        return 0;
    }
    // finish db update

    struct timespec watch;
    startWatch(&watch);


    sfree(Modes.dbIndex);
    sfree(Modes.dbRaw.buffer);
    sfree(Modes.db);

    Modes.dbIndex = Modes.db2Index;
    Modes.dbRaw = Modes.db2Raw;
    Modes.db = Modes.db2;
    Modes.db2Index = NULL;
    Modes.db2 = NULL;
    Modes.db2Raw.buffer = NULL;
    Modes.db2Raw.len = 0;

    /*
    for (int j = 0; j < Modes.acBuckets; j++) {
        for (struct aircraft *a = Modes.aircraft[j]; a; a = a->next) {
            updateTypeReg(a);
        }
    }
    */

    int64_t now = mstime();

    threadpool_distribute_and_run(Modes.allPool, Modes.allTasks, updateTypeRegRange, Modes.acBuckets, 0, now);

    double elapsed = stopWatch(&watch) / 1000.0;
    fprintf(stderr, "Database update done! (critical part took %.3f seconds)\n", elapsed);



    return 1;
}


dbEntry *dbGet(uint32_t addr, dbEntry **index) {
    if (!index)
        return NULL;
    dbEntry *d = index[dbHash(addr)];

    while (d && d->addr != addr) {
        d = d->next;
    }
    return d;
}

void dbPut(uint32_t addr, dbEntry **index, dbEntry *d) {
    uint32_t hash = dbHash(addr);
    d->next = index[hash];
    index[hash] = d;
}

static void copyDetailFunc(char *sot, char *eot, char *target, int alloc) {
    int len = imin(alloc, eot - sot);
    memcpy(target, sot, len);
    // terminate if string doesn't fill up alloc
    // this mildly insane program can hopefully handle unterminated database strings
    if (len < alloc) {
        target[len] = '\0';
    }
    sanitize(target, len);
}

static void updateDetails(struct aircraft *curr, struct char_buffer cb, uint32_t offset) {
    //fprintf(stdout, "%u %u\n", offset, (uint32_t) cb.len);
    char *eob = cb.buffer + cb.len;
    char *sol = cb.buffer + offset;
    char *eol = memchr(sol, '\n', eob - sol);
    char *sot;
    char *eot = sol - 1; // this pointer must not be dereferenced, nextToken will increment it.

    if (!eol) goto BAD_ENTRY;

#define copyDetail(d) do { copyDetailFunc(sot, eot, curr->d, sizeof(curr->d)); } while (0)

    if (!nextToken(';', &sot, &eot, &eol)) goto BAD_ENTRY;
    copyDetail(registration);

    if (!nextToken(';', &sot, &eot, &eol)) goto BAD_ENTRY;
    copyDetail(typeCode);

    if (!nextToken(';', &sot, &eot, &eol)) goto BAD_ENTRY;
    curr->dbFlags = 0;
    for (int j = 0; j < 8 * (int) sizeof(curr->dbFlags) && sot < eot; j++, sot++)
        curr->dbFlags |= ((*sot == '1') << j);


    if (!nextToken(';', &sot, &eot, &eol)) goto BAD_ENTRY;
    copyDetail(typeLong);

    if (!nextToken(';', &sot, &eot, &eol)) goto BAD_ENTRY;
    copyDetail(year);

    if (!nextToken(';', &sot, &eot, &eol)) goto BAD_ENTRY;
    copyDetail(ownOp);

#undef copyDetail

    if (false) {
        // debugging output
        fprintf(stdout, "%06X;%.12s;%.4s;%c%c;%.54s\n",
                curr->addr,
                curr->registration,
                curr->typeCode,
                curr->dbFlags & 1 ? '1' : '0',
                curr->dbFlags & 2 ? '1' : '0',
                curr->typeLong);
    }

    return;

BAD_ENTRY:
    curr->registration[0] = '\0';
    curr->typeCode[0] = '\0';
    curr->typeLong[0] = '\0';
    curr->ownOp[0] = '\0';
    curr->year[0] = '\0';
    curr->dbFlags = 0;
}

void updateTypeReg(struct aircraft *a) {
    dbEntry *d = dbGet(a->addr, Modes.dbIndex);
    if (d) {
        updateDetails(a, Modes.dbRaw, d->rawOffset);
    } else {
        a->registration[0] = '\0';
        a->typeCode[0] = '\0';
        a->typeLong[0] = '\0';
        a->ownOp[0] = '\0';
        a->year[0] = '\0';
        a->dbFlags = 0;
    }
    if (is_df18_exception(a->addr)) {
        a->is_df18_exception = 1;
    }
    if (isMilRange(a->addr)) {
        a->dbFlags |= 1;
    }
}
static int isMilRange(uint32_t i) {
    return
        false
        // us military
        //adf7c8-adf7cf = united states mil_5(uf)
        //adf7d0-adf7df = united states mil_4(uf)
        //adf7e0-adf7ff = united states mil_3(uf)
        //adf800-adffff = united states mil_2(uf)
        //ae0000-afffff = united states mil_1(uf)
        || (i >= 0xadf7c8 && i <= 0xafffff)

        //010070-01008f = egypt_mil
        || (i >= 0x010070 && i <= 0x01008f)

        //0a4000-0a4fff = algeria mil(ap)
        || (i >= 0x0a4000 && i <= 0x0a4fff)

        //33ff00-33ffff = italy mil(iy)
        || (i >= 0x33ff00 && i <= 0x33ffff)

        //350000-37ffff = spain mil(sp)
        || (i >= 0x350000 && i <= 0x37ffff)

        //3aa000-3affff = france mil_1(fs)
        || (i >= 0x3aa000 && i <= 0x3affff)
        //3b7000-3bffff = france mil_2(fs)
        || (i >= 0x3b7000 && i <= 0x3bffff)

        //3ea000-3ebfff = germany mil_1(df)
        || (i >= 0x3ea000 && i <= 0x3ebfff)
        //3f4000-3f7fff = germany mil_2(df)
        //3f8000-3fbfff = germany mil_3(df)
        || (i >= 0x3f4000 && i <= 0x3fbfff)

        //400000-40003f = united kingdom mil_1(ra)
        || (i >= 0x400000 && i <= 0x40003f)
        //43c000-43cfff = united kingdom mil(ra)
        || (i >= 0x43c000 && i <= 0x43cfff)

        //444000-446fff = austria mil(aq)
        || (i >= 0x444000 && i <= 0x446fff)

        //44f000-44ffff = belgium mil(bc)
        || (i >= 0x44f000 && i <= 0x44ffff)

        //457000-457fff = bulgaria mil(bu)
        || (i >= 0x457000 && i <= 0x457fff)

        //45f400-45f4ff = denmark mil(dg)
        || (i >= 0x45f400 && i <= 0x45f4ff)

        //468000-4683ff = greece mil(gc)
        || (i >= 0x468000 && i <= 0x4683ff)

        //473c00-473c0f = hungary mil(hm)
        || (i >= 0x473c00 && i <= 0x473c0f)

        //478100-4781ff = norway mil(nn)
        || (i >= 0x478100 && i <= 0x4781ff)
        //480000-480fff = netherlands mil(nm)
        || (i >= 0x480000 && i <= 0x480fff)
        //48d800-48d87f = poland mil(po)
        || (i >= 0x48d800 && i <= 0x48d87f)
        //497c00-497cff = portugal mil(pu)
        || (i >= 0x497c00 && i <= 0x497cff)
        //498420-49842f = czech republic mil(ct)
        || (i >= 0x498420 && i <= 0x49842f)

        //4b7000-4b7fff = switzerland mil(su)
        || (i >= 0x4b7000 && i <= 0x4b7fff)
        //4b8200-4b82ff = turkey mil(tq)
        || (i >= 0x4b8200 && i <= 0x4b82ff)

        //506f32-506fff = slovenia mil(sj)
        //|| (i >= 0x506f32 && i <= 0x506fff)

        //70c070-70c07f = oman mil(on)
        || (i >= 0x70c070 && i <= 0x70c07f)

        //710258-71025f = saudi arabia mil_1(sx)
        //710260-71027f = saudi arabia mil_2(sx)
        //710280-71028f = saudi arabia mil_3(sx)
        || (i >= 0x710258 && i <= 0x71028f)
        //710380-71039f = saudi arabia mil_4(sx)
        || (i >= 0x710380 && i <= 0x71039f)

        //738a00-738aff = israel mil(iz)
        || (i >= 0x738a00 && i <= 0x738aff)

        //7cf800-7cfaff australia mil
        || (i >= 0x7cf800 && i <= 0x7cfaff)

        //800200-8002ff = india mil(im)
        || (i >= 0x800200 && i <= 0x8002ff)

        //c20000-c3ffff = canada mil(cb)
        || (i >= 0xc20000 && i <= 0xc3ffff)

        //e40000-e41fff = brazil mil(bq)
        || (i >= 0xe40000 && i <= 0xe41fff)

        //e80600-e806ff = chile mil(cq)
        //|| (i >= 0xe80600 && i <= 0xe806ff)
        // disabled due to civilian aircraft in hex range
        ;
}
