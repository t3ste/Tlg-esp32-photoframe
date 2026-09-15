#ifndef CLIMATE_HISTORY_H
#define CLIMATE_HISTORY_H

#include "cJSON.h"

// Records the current SHTC3 temperature/humidity reading to the persisted
// history log. No-op if climate logging is disabled
// (config_manager_get_climate_logging_enabled()), the sensor read fails
// (unsupported board or a transient I2C error), or no persistent storage is
// mounted. Called once per successfully displayed image (display_manager.c)
// and once per Agenda render (agenda_manager.c) - unlike battery history,
// which only has the former call site, Agenda mode skips the normal photo
// pipeline entirely and would otherwise almost never log for anyone running
// it as their primary display.
//
// Automatically clears the whole history and starts a fresh log once the
// oldest entry is more than CLIMATE_HISTORY_MAX_AGE_DAYS old - see
// config.h. There's no equivalent of battery history's "fresh charge"
// reset trigger (no comparable natural cycle boundary for climate).
void climate_history_record(void);

// Builds the JSON payload for the Web UI climate-history chart:
// {"entries": [{"t": <unix_ts>, "temp_c": <float>, "hum": <float 0-100>,
//               "tcat": <0=Bad/1=Good/2=Super>, "hcat": <0-2>}, ...]}
// tcat/hcat are computed from the *currently configured* room type at
// build time, so the Web UI needs no copy of the threshold table. Caller
// owns the returned object (cJSON_Delete when done). Returns NULL on
// allocation failure.
cJSON *climate_history_build_json(void);

// Deletes the persisted history log (user-requested reset from the Web
// UI's Climate History card). No-op if no log exists yet.
void climate_history_reset(void);

#endif
