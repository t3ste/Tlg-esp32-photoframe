#include "climate.h"

#include <math.h>

// One row per climate_room_type_t value (same order). Bad is an outer
// exclusion band (value below bad_low or above bad_high); Super is the
// innermost optimal band; anything else - including the gap between Bad's
// bound and Super's surrounding comfort range - is Good. Verified against
// the user-supplied reference table; basement humidity has no lower Bad
// bound (RH can't go negative, so 0 is a correct stand-in).
typedef struct {
    float temp_bad_low, temp_bad_high;
    float temp_super_low, temp_super_high;
    float hum_bad_low, hum_bad_high;
    float hum_super_low, hum_super_high;
} climate_room_profile_t;

static const climate_room_profile_t room_profiles[] = {
    [CLIMATE_ROOM_LIVING_ROOM] = {18, 24, 20, 21, 35, 65, 45, 55},
    [CLIMATE_ROOM_BEDROOM] = {15, 21, 16, 18, 35, 65, 45, 55},
    [CLIMATE_ROOM_BATHROOM] = {19, 25, 22, 23, 40, 75, 50, 60},
    [CLIMATE_ROOM_KITCHEN] = {16, 22, 18, 19, 35, 70, 45, 55},
    [CLIMATE_ROOM_BASEMENT] = {10, 18, 15, 17, 0, 70, 50, 55},
};

static const climate_room_profile_t *profile_for(climate_room_type_t room)
{
    if (room < 0 || room >= (int) (sizeof(room_profiles) / sizeof(room_profiles[0]))) {
        room = CLIMATE_ROOM_LIVING_ROOM;
    }
    return &room_profiles[room];
}

static climate_category_t classify(float value, float bad_low, float bad_high, float super_low,
                                   float super_high)
{
    if (value < bad_low || value > bad_high) {
        return CLIMATE_CATEGORY_BAD;
    }
    if (value >= super_low && value <= super_high) {
        return CLIMATE_CATEGORY_SUPER;
    }
    return CLIMATE_CATEGORY_GOOD;
}

climate_category_t climate_classify_temperature(float celsius, climate_room_type_t room)
{
    const climate_room_profile_t *p = profile_for(room);
    return classify(celsius, p->temp_bad_low, p->temp_bad_high, p->temp_super_low,
                    p->temp_super_high);
}

climate_category_t climate_classify_humidity(float humidity_percent, climate_room_type_t room)
{
    const climate_room_profile_t *p = profile_for(room);
    return classify(humidity_percent, p->hum_bad_low, p->hum_bad_high, p->hum_super_low,
                    p->hum_super_high);
}

int climate_celsius_to_fahrenheit(float celsius)
{
    return (int) lroundf(celsius * 9.0f / 5.0f + 32.0f);
}
