#ifndef MDNS_SERVICE_H
#define MDNS_SERVICE_H

#include "esp_err.h"

esp_err_t mdns_service_init(void);
esp_err_t mdns_service_update_hostname(void);

#endif
