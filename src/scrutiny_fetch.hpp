// scrutiny_fetch.hpp - fetch + parse Scrutiny /api/summary into g_disks.
#pragma once

// Perform one synchronous fetch + parse + g_disks update.
// Returns true on success (HTTP 200 + valid JSON). On any failure the previous
// disk data is kept and the consecutive-failure counter is incremented.
bool scrutiny_fetch_now();
