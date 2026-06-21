// disks.hpp - shared disk data model and thread-safe global state.
#pragma once

#include <cstdint>
#include "config.h"

// Overall health, derived from device_status + bad-sector attributes.
enum class DiskHealth : uint8_t
{
    OK = 0,
    WARN = 1,
    FAIL = 2
};

// One physical disk as reported by Scrutiny /api/summary.
struct Disk
{
    char scrutiny_id[64]; // opaque key from data.summary (Scrutiny UUID)
    char device_name[16]; // e.g. "sda"
    char device_type[12]; // e.g. "SATA", "NVMe"
    char model_name[48];
    char serial_number[32];
    char capacity_label[16]; // human-readable size, e.g. "4 TB"
    uint64_t capacity_bytes;
    int32_t temp;           // Celsius
    int64_t power_on_hours; // hours
    uint64_t device_status; // bitmask: 0 == passed
    int64_t realloc;        // SMART attr 5
    int64_t pending;        // SMART attr 197
    int64_t uncorrectable;  // SMART attr 198
    DiskHealth health;
};

// Compute overall health from the raw fields (rule from requirements).
DiskHealth disk_compute_health(const Disk &d);

// ---------------------------------------------------------------------------
// Global state lifecycle
// ---------------------------------------------------------------------------
void disks_init();

// Atomically replace the disk list (sorted by device_name) and mark fetch ok.
// Pass n == 0 to clear (e.g. empty summary). Returns false if mutex missing.
bool disks_replace(const Disk *arr, int n);

// Copy a consistent snapshot out under lock.
//   out      : caller buffer of `max` entries
//   returns  : number of disks copied
int disks_snapshot(Disk *out, int max);

// Fetch bookkeeping (thread-safe).
void disks_note_fetch_ok();
void disks_note_fetch_fail();
int disks_consec_fail();
int disks_count();
int64_t disks_last_fetch_sec(); // seconds since boot, or -1 if never fetched

// Seconds since boot (monotonic, esp_timer based).
int64_t now_sec();
