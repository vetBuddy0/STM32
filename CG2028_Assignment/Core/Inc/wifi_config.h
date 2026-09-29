/******************************************************************************
 * @file           : wifi_config.h
 * @brief          : Wi-Fi network and receiver address.
 *
 * Set these before flashing. The ISM43362 only does WPA2-personal on 2.4 GHz,
 * so NUS Wi-Fi won't work - use a phone hotspot.
 ******************************************************************************/
#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#define WIFI_SSID           "LAPTOP-0UMHDVJV 3492"
#define WIFI_PASSWORD       "Bk114%62"

/* Laptop running receiver.py. A Windows hotspot gives the laptop 192.168.137.1 */
#define RECEIVER_IP         { 192, 168, 137, 1 }
#define RECEIVER_PORT       2028

#define WEARABLE_PORT       1234

#endif /* WIFI_CONFIG_H */
