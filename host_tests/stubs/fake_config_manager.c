// Fake config_manager for host tests — only what image_processor.c links.
#include "config_manager.h"

display_orientation_t test_display_orientation = DISPLAY_ORIENTATION_LANDSCAPE;

display_orientation_t config_manager_get_display_orientation(void)
{
    return test_display_orientation;
}

bool config_manager_get_caption_invert_colors_enabled(void)
{
    return false;
}

const char *config_manager_get_weather_icon_set(void)
{
    return "none";
}

bool config_manager_get_weather_icon_colored(void)
{
    return false;
}
