#ifndef CLIMATE_H
#define CLIMATE_H

#include "config.h"
#include "esp_err.h"

// Comfort category a single temperature or humidity reading falls into for
// a given room type - see climate_classify_temperature()/
// climate_classify_humidity(). Temperature and humidity are always
// classified and displayed separately, never merged into one combined
// verdict (by design).
typedef enum {
    CLIMATE_CATEGORY_BAD = 0,
    CLIMATE_CATEGORY_GOOD = 1,
    CLIMATE_CATEGORY_SUPER = 2,
} climate_category_t;

// Classifies a temperature/humidity reading against the given room type's
// preset comfort thresholds (mold/dryness-risk "Bad", healthy "Super" core,
// everything else - including the small gap between Bad's outer bound and
// Super's own surrounding "Good" range - falls back to "Good").
// Thresholds are always in Celsius/%RH regardless of the user's display
// unit preference (see climate_celsius_to_fahrenheit()).
climate_category_t climate_classify_temperature(float celsius, climate_room_type_t room);
climate_category_t climate_classify_humidity(float humidity_percent, climate_room_type_t room);

// Rounds a Celsius value to the nearest whole Fahrenheit degree, for
// display only - never used for classification.
int climate_celsius_to_fahrenheit(float celsius);

// Reads the SHTC3 sensor with the user's calibration offset
// (config_manager_get_climate_temp_offset()/_hum_offset()) already applied -
// use these everywhere a climate value is displayed or logged (overlay
// badge, Agenda header, history log). GET /api/sensor deliberately calls
// board_hal_get_temperature()/get_humidity() directly instead, so it keeps
// showing the raw, un-offset reading - useful as a reference for picking an
// offset in the first place. Same ESP_OK/error contract as the board_hal
// functions they wrap.
esp_err_t climate_read_temperature(float *out_celsius);
esp_err_t climate_read_humidity(float *out_percent);

#endif
