#include "include/network.h"

static bool cyw43Initialized = false;
static wifi_mode_t wifiCurrentMode = WIFI_MODE_STA;
static wifi_network_info_t wifiNetworkInfo = {.rssi = INT16_MIN};
static wifi_scan_data_t wifiScanData = {0};
static bool wifiScanInProgress = false;
static char wifiHostname[NETWORK_MAX_STRING_LENGTH];
static ip_addr_t currentIp = {0};
static uint8_t cyw43Mac[NETWORK_MAC_SIZE];
static char cyw43MacStr[NETWORK_MAX_STRING_LENGTH];
static wifi_sta_conn_status_t connectionStatus = DISCONNECTED;
static char connectionStatusStr[NETWORK_MAX_STRING_LENGTH] = {0};
#if LWIP_MDNS_RESPONDER
// mDNS lifecycle flags. The lwIP mDNS responder must be initialized at
// most once per boot, and a netif may have at most one service entry
// registered at a time. Track both so reconnects (which run
// network_wifiStaConnect repeatedly) don't double-register or leak
// state.
static bool mdnsInitialized = false;
static bool mdnsStaRegistered = false;
#endif

// Static variable to store the callback function
static NetworkPollingCallback networkPollingCallback = NULL;

static void network_clearCurrentNetworkInfo(void);

static void network_resetConnectionState(void) {
  memset(&currentIp, 0, sizeof(currentIp));
  connectionStatus = DISCONNECTED;
  snprintf(connectionStatusStr, sizeof(connectionStatusStr), "LINK DOWN");
}

static void network_resetRuntimeState(void) {
  wifiScanInProgress = false;
  network_resetConnectionState();
  memset(cyw43Mac, 0, sizeof(cyw43Mac));
  cyw43MacStr[0] = '\0';
  network_clearCurrentNetworkInfo();
#if LWIP_MDNS_RESPONDER
  mdnsInitialized = false;
  mdnsStaRegistered = false;
#endif
}

static void network_resetStaInterface(struct netif *nif) {
#if LWIP_MDNS_RESPONDER
  if (mdnsStaRegistered) {
    cyw43_arch_lwip_begin();
    mdns_resp_remove_netif(nif);
    cyw43_arch_lwip_end();
    mdnsStaRegistered = false;
  }
#else
  LWIP_UNUSED_ARG(nif);
#endif
  cyw43_arch_disable_sta_mode();
  cyw43_arch_enable_sta_mode();
  network_resetConnectionState();
}

static const char *picoSerialStr() {
  static char buf[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];
  pico_unique_board_id_t boardId;

  memset(&boardId, 0, sizeof(boardId));
  pico_get_unique_board_id(&boardId);
  for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
    snprintf(&buf[i * 2], 3, "%02x", boardId.id[i]);
  }

  return buf;
}

static void network_clearCurrentNetworkInfo(void) {
  memset(&wifiNetworkInfo, 0, sizeof(wifiNetworkInfo));
  wifiNetworkInfo.rssi = INT16_MIN;
}

#ifdef CYW43_WL_GPIO_LED_PIN
static void network_updateCurrentNetworkInfoRadio(void) {
  if ((!cyw43Initialized) || (wifiCurrentMode != WIFI_MODE_STA)) {
    return;
  }

  uint8_t bssid[NETWORK_MAC_SIZE] = {0};
  if (cyw43_wifi_get_bssid(&cyw43_state, bssid) == 0) {
    snprintf(wifiNetworkInfo.bssid, sizeof(wifiNetworkInfo.bssid),
             "%02x:%02x:%02x:%02x:%02x:%02x", bssid[0], bssid[1], bssid[2],
             bssid[3], bssid[4], bssid[5]);
  } else {
    wifiNetworkInfo.bssid[0] = '\0';
  }

  int32_t rssi = 0;
  if (cyw43_wifi_get_rssi(&cyw43_state, &rssi) == 0) {
    wifiNetworkInfo.rssi = (int16_t)rssi;
  } else {
    wifiNetworkInfo.rssi = INT16_MIN;
  }
}
#endif

// Strip leading and trailing ASCII whitespace (' ', '\t', '\r', '\n')
// from a NUL-terminated string in place. Returns a pointer into the
// same buffer; the input pointer is consumed only for the leading-
// whitespace skip, the trailing whitespace is replaced with '\0'.
static char *network_trim_ascii_spaces(char *text) {
  while (*text == ' ' || *text == '\t') {
    ++text;
  }
  size_t len = strlen(text);
  while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                     text[len - 1] == '\r' || text[len - 1] == '\n')) {
    text[--len] = '\0';
  }
  return text;
}

static uint32_t getCountryCode(char *code, char **validCountryStr) {
  *validCountryStr = "XX";
  // empty configuration select worldwide
  if (strlen(code) == 0) {
    return CYW43_COUNTRY_WORLDWIDE;
  }

  if (strlen(code) != 2) {
    return CYW43_COUNTRY_WORLDWIDE;
  }

  // current supported country code
  // https://www.raspberrypi.com/documentation/pico-sdk/networking.html#CYW43_COUNTRY_
  // ISO-3166-alpha-2
  // XX select worldwide
  char *validCountryCode[] = {
      "XX", "AU", "AR", "AT", "BE", "BR", "CA", "CL", "CN", "CO", "CZ",
      "DK", "EE", "FI", "FR", "DE", "GR", "HK", "HU", "IS", "IN", "IL",
      "IT", "JP", "KE", "LV", "LI", "LT", "LU", "MY", "MT", "MX", "NL",
      "NZ", "NG", "NO", "PE", "PH", "PL", "PT", "SG", "SK", "SI", "ZA",
      "KR", "ES", "SE", "CH", "TW", "TH", "TR", "GB", "US"};

  char country[3] = {toupper(code[0]), toupper(code[1]), 0};
  for (int i = 0; i < (sizeof(validCountryCode) / sizeof(validCountryCode[0]));
       i++) {
    if (!strcmp(country, validCountryCode[i])) {
      *validCountryStr = validCountryCode[i];
      return CYW43_COUNTRY(country[0], country[1], 0);
    }
  }
  return CYW43_COUNTRY_WORLDWIDE;
}

/**
 * @brief Deinitializes the network.
 *
 * This function deinitializes the network by setting the `cyw43_initialized`
 * flag to false and calling `cyw43_arch_deinit()`. It is important to set the
 * flag to false because calling a cyw43 function before initialization will
 * cause a crash.
 */
#ifdef CYW43_WL_GPIO_LED_PIN
void network_deInit() {
  if (cyw43Initialized) {
    DPRINTF("Deinitializing the network\n");
    cyw43Initialized = false;
    cyw43_arch_deinit();
    network_resetRuntimeState();
    DPRINTF("Network deinitialized\n");
  } else {
    network_resetRuntimeState();
    DPRINTF("Network already deinitialized\n");
  }
}
#endif

// NOLINTBEGIN(readability-magic-numbers)
static u_int32_t getAuthPicoCode(uint16_t connectCode) {
  switch (connectCode) {
    case 0:
      return CYW43_AUTH_OPEN;
    case 1:
    case 2:
      return CYW43_AUTH_WPA_TKIP_PSK;
    case 3:
    case 4:
    case 5:
      return CYW43_AUTH_WPA2_AES_PSK;
    case 6:
    case 7:
    case 8:
      return CYW43_AUTH_WPA2_MIXED_PSK;
    default:
      return CYW43_AUTH_OPEN;
  }
}
// NOLINTEND(readability-magic-numbers)

// Function to parse and clean up an SSID string
// If the SSID is longer than MAX_SSID_LENGTH, it is truncated.
// If the SSID does not comply with the standard, it is cleaned up.
// Returns true if valid, false otherwise.
bool network_parseSSID(const char *ssid, char *outSSID) {
  if (ssid == NULL || outSSID == NULL) {
    return false;
  }

  // IEEE 802.11 SSID rules:
  //  - Length: 1..32 bytes (MAX_SSID_LENGTH must be <= 33)
  //  - May not be all spaces
  //  - Cannot have control/non-printable chars (typically 0x20..0x7E allowed)

  size_t inLen = strnlen(ssid, MAX_SSID_LENGTH * 2);  // catch crazy input
  if (inLen == 0) {
    outSSID[0] = '\0';
    return false;
  }

  // Clean: Copy only allowed chars up to MAX_SSID_LENGTH-1
  size_t outLen = 0;
  for (size_t i = 0; i < inLen && outLen < MAX_SSID_LENGTH - 1; ++i) {
    char c = ssid[i];
    // Only printable ASCII (0x20-0x7E), disallow control characters
    if ((unsigned char)c >= 0x20 && (unsigned char)c <= 0x7E) {
      outSSID[outLen++] = c;
    }
  }
  outSSID[outLen] = '\0';

  // Check: Not empty, not all spaces, not all filtered out
  if (outLen == 0) return false;
  for (size_t i = 0; i < outLen; ++i) {
    if (outSSID[i] != ' ') return true;
  }
  return false;
}

// Function to parse and clean up the password string
// of a WiFi network complying with the IEEE 802.11 standard.
// If the password is longer than WIFI_AP_PASS_MAX_LENGTH,
// it is truncated.
// Returns true if valid, false otherwise.
bool network_parsePassword(const char *password, char *outPassword) {
  if (password == NULL || outPassword == NULL) {
    return false;
  }

  // IEEE 802.11 password rules:
  //  - WPA2 minimum length: 8 chars (WEP: 5/13/16/29/etc)
  //  - WPA2 maximum length: 63 bytes
  //  - Only printable ASCII chars (0x20-0x7E) are valid
  //  - Not all spaces

  size_t inLen =
      strnlen(password, WIFI_AP_PASS_MAX_LENGTH * 2);  // catch crazy input
  if (inLen == 0) {
    outPassword[0] = '\0';
    return false;
  }

  // Clean: Copy only allowed chars up to WIFI_AP_PASS_MAX_LENGTH-1
  size_t outLen = 0;
  for (size_t i = 0; i < inLen && outLen < WIFI_AP_PASS_MAX_LENGTH - 1; ++i) {
    char c = password[i];
    // Only printable ASCII (0x20-0x7E)
    if ((unsigned char)c >= 0x20 && (unsigned char)c <= 0x7E) {
      outPassword[outLen++] = c;
    }
  }
  outPassword[outLen] = '\0';

  // Check: Not empty, not all spaces, not all filtered out
  if (outLen == 0) return false;
  for (size_t i = 0; i < outLen; ++i) {
    if (outPassword[i] != ' ') {
      // Ensure password length is at least 8 characters
      if (outLen >= 8) return true;
      return false;
    }
  }
  return false;
}

// Setter for the callback function
void network_setPollingCallback(NetworkPollingCallback callback) {
  networkPollingCallback = callback;
}

// NOLINTBEGIN(readability-magic-numbers)
const char *network_getAuthTypeString(uint16_t connectCode) {
  switch (connectCode) {
    case 0:
      return "OPEN";
    case 1:
    case 2:
      return "WPA_TKIP_PSK";
    case 3:
    case 4:
    case 5:
      return "WPA2_AES_PSK";
    case 6:
    case 7:
    case 8:
      return "WPA2_MIXED_PSK";
    default:
      return "OPEN";
  }
}
// NOLINTEND(readability-magic-numbers)

// NOLINTBEGIN(readability-magic-numbers)
const char *network_getAuthTypeStringShort(uint16_t connectCode) {
  switch (connectCode) {
    case 1:
    case 2:
      return "WPA";
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
      return "WPA2";
    default:
      return "OPEN";
  }
}
// NOLINTEND(readability-magic-numbers)

/**
 * @brief Initializes the CYW43 WiFi chip if it has not been initialized
 * already.
 *
 * This function checks if the CYW43 WiFi chip has already been initialized. If
 * not, it sets the initialization flag to true and proceeds to initialize the
 * chip. It should not be called to initialize the WiFi network, only the chip.
 * For example, it can be used to detect the VBUS pin state or toogling the
 * green LED.
 *
 * @return int Returns 0 on successful initialization or if the chip was already
 * initialized. Returns -1 if the initialization fails.
 */
#ifdef CYW43_WL_GPIO_LED_PIN
int network_initChipOnly() {
  if (cyw43Initialized) {
    return 0;
  }
  DPRINTF("CYW43 Logging level: %d\n", CYW43_VERBOSE_DEBUG);
  int res;
  DPRINTF("Initialization CYW43 chip ONLY...\n");

  if ((res = cyw43_arch_init())) {
    DPRINTF("Failed to initialize CYW43: %d\n", res);
    return -1;
  }
  // Set the flag only after init succeeds; if it failed and we set it
  // earlier, the rest of the system would think WiFi was up.
  cyw43Initialized = true;
  network_resetRuntimeState();
  return 0;
}
#endif

/**
 * @brief Initialize the WiFi network with the specified mode.
 *
 * This function initializes the WiFi network and sets the country code and
 * power management settings. It supports both STA (Station) and AP (Access
 * Point) modes.
 *
 * @param mode The WiFi mode to initialize. It can be either WIFI_MODE_STA or
 * WIFI_MODE_AP.
 * @return int Returns 0 on success, or -1 on failure.
 */
#ifdef CYW43_WL_GPIO_LED_PIN
int network_wifiInit(wifi_mode_t mode) {
  if (cyw43Initialized) {
    DPRINTF("WiFi already initialized\n");
    return 0;
  }
  DPRINTF("CYW43 Logging level: %d\n", CYW43_VERBOSE_DEBUG);
  uint32_t country = CYW43_COUNTRY_WORLDWIDE;
  SettingsConfigEntry *countryEntry =
      settings_find_entry(gconfig_getContext(), PARAM_WIFI_COUNTRY);
  const char *countryStr = "XX";
  if (countryEntry != NULL) {
    char *validCountry = NULL;
    country = getCountryCode(countryEntry->value, &validCountry);
    if (validCountry != NULL) {
      countryStr = validCountry;
      settings_put_string(gconfig_getContext(), PARAM_WIFI_COUNTRY,
                          validCountry);
    }
  }

  int res;
  DPRINTF("Initialization WiFi...\n");

  if ((res = cyw43_arch_init_with_country(country))) {
    DPRINTF("Failed to initialize WiFi: %d\n", res);
    return -1;
  }
  // Set the flag only after init succeeds; if it failed and we set it
  // earlier, the rest of the system would think WiFi was up. Also: do
  // not dereference countryEntry->value here -- countryEntry can be
  // NULL if PARAM_WIFI_COUNTRY is missing from the config.
  cyw43Initialized = true;
  network_resetRuntimeState();
  DPRINTF("Country: %s\n", countryStr);

  // Start STA or AP mode
  if (mode == WIFI_MODE_STA) {
    DPRINTF("Enabling STA mode...\n");
    cyw43_arch_enable_sta_mode();
    wifiCurrentMode = WIFI_MODE_STA;
  } else {
    DPRINTF("Enabling AP mode...\n");
    DPRINTF("Read the SSID, password and auth mode\n");

    DPRINTF("No SSID found in config for AP mode. Setting default SSID\n");

    char ssidStr[MAX_SSID_LENGTH] = WIFI_AP_SSID;
    DPRINTF("SSID: %s\n", ssidStr);

    char passwordStr[WIFI_AP_PASS_MAX_LENGTH] = WIFI_AP_PASS;
    DPRINTF("Password: %s\n", passwordStr);

    int authInt = WIFI_AP_AUTH;  // WPA2_AES_PSK

    DPRINTF("Auth mode: %08x\n", getAuthPicoCode(authInt));
    cyw43_arch_enable_ap_mode(ssidStr, passwordStr, getAuthPicoCode(authInt));

    // Set static IP address for the AP
    struct netif *netif = &cyw43_state.netif[CYW43_ITF_AP];
    ip4_addr_t netmask;
    ip4_addr_t gateway;
    ip4addr_aton(WIFI_AP_NETMASK, &netmask);
    ip4addr_aton(WIFI_AP_GATEWAY, &gateway);

    DPRINTF("GW IP: %s\n", ip4addr_ntoa(&gateway));
    DPRINTF("Mask IP: %s\n", ip4addr_ntoa(&netmask));

#ifdef MICROPY_INCLUDED_LIB_NETUTILS_DHCPSERVER_H
    // Start the dhcp server
    dhcp_server_init(&gateway, &netmask);
    DPRINTF("DHCP server started.\n");
#endif

#ifdef _DNSSERVER_H_
    // Start the dns server
    dns_server_init(&gateway);
    DPRINTF("DNS server started\n");
#endif
    wifiCurrentMode = WIFI_MODE_AP;
  }

  // Setting the power management
  uint32_t pmValue = NETWORK_POWER_MGMT_DISABLED;  // 0: Disable PM
  SettingsConfigEntry *pmEntry =
      settings_find_entry(gconfig_getContext(), PARAM_WIFI_POWER);
  if (pmEntry != NULL) {
    pmValue = strtoul(pmEntry->value, NULL, HEX_BASE);
  }
  if (pmValue < NETWORK_POWER_MGMT_MAX_OPTIONS) {
    switch (pmValue) {
      case 0:
        pmValue = NETWORK_POWER_MGMT_DISABLED;  // DISABLED_PM
        break;
      case 1:
        pmValue = CYW43_PERFORMANCE_PM;  // PERFORMANCE_PM
        break;
      case 2:
        pmValue = CYW43_AGGRESSIVE_PM;  // AGGRESSIVE_PM
        break;
      case 3:
        pmValue = CYW43_DEFAULT_PM;  // DEFAULT_PM
        break;
      default:
        pmValue = CYW43_NO_POWERSAVE_MODE;  // NO_POWERSAVE_MODE
        break;
    }
  }
  DPRINTF("Setting power management to: %08x\n", pmValue);
  cyw43_wifi_pm(&cyw43_state, pmValue);
  return 0;
}
#endif

/**
 * @brief Safely polls the network if the CYW43 module is initialized.
 *
 * This function checks if the CYW43 module has been initialized before
 * calling the `cyw43_arch_poll()` function to poll the network. This ensures
 * that the polling operation is only performed when the module is ready,
 * preventing potential errors or undefined behavior.
 */
void network_safePoll() {
  if (cyw43Initialized) {
    cyw43_arch_poll();
  }
}

/**
 * @brief Scans for available Wi-Fi networks and stores the results.
 *
 * This function initiates a Wi-Fi network scan if the network is initialized
 * and the scan interval has elapsed. It processes the scan results and stores
 * unique networks in the global `wifi_scan_data` structure.
 *
 * @param wifi_scan_time Pointer to the absolute time of the last scan.
 * @param wifi_scan_interval Interval between scans in seconds.
 * @return int Returns 0 on success, -1 if the network is not initialized.
 */
int network_scan(absolute_time_t *wifiScanTime, int wifiScanInterval) {
  if (!cyw43Initialized) {
    // If the network is not initialized, we cancel the scan
    return -1;
  }
  int scan_result(void *env, const cyw43_ev_scan_result_t *result) {
    // Check if the BSSID already exists in the found networks
    bool bssid_exists(wifi_network_info_t * network) {
      for (size_t i = 0; i < wifiScanData.count; i++) {
        if (strcmp(wifiScanData.networks[i].bssid, network->bssid) == 0) {
          return true;  // BSSID found
        }
      }
      return false;  // BSSID not found
    }
    if (result && wifiScanData.count < MAX_NETWORKS) {
      wifi_network_info_t network;

      // Copy SSID
      snprintf(network.ssid, sizeof(network.ssid), "%s", result->ssid);

      // Format BSSID
      snprintf(network.bssid, sizeof(network.bssid),
               "%02x:%02x:%02x:%02x:%02x:%02x", result->bssid[0],
               result->bssid[1], result->bssid[2], result->bssid[3],
               result->bssid[4], result->bssid[5]);

      // Store authentication mode
      network.auth_mode = result->auth_mode;

      // Store signal strength
      network.rssi = result->rssi;

      // Check if BSSID already exists
      if (!bssid_exists(&network)) {
        if (strlen(network.ssid) > 0) {
          wifiScanData.networks[wifiScanData.count] = network;
          wifiScanData.count++;
          DPRINTF("FOUND NETWORK %s (%s) with auth %d and RSSI %d\n",
                  network.ssid, network.bssid, network.auth_mode, network.rssi);
        }
      }
    }
    return 0;
  }
  // DPRINTF("Time diff: %lld\n", absolute_time_diff_us(get_absolute_time(),
  // (absolute_time_t)*wifi_scan_time));
  if (absolute_time_diff_us(get_absolute_time(), *wifiScanTime) < 0) {
    if (!wifiScanInProgress) {
      DPRINTF("Scanning networks...\n");
      cyw43_wifi_scan_options_t scanOptions = {0};
      int err = cyw43_wifi_scan(&cyw43_state, &scanOptions, NULL, scan_result);
      if (err == 0) {
        DPRINTF("Performing wifi scan\n");
        wifiScanInProgress = true;
      } else {
        DPRINTF("Failed to start scan: %d\n", err);
        *wifiScanTime = make_timeout_time_ms(wifiScanInterval * SEC_TO_MS);
      }
    } else {
      if (!cyw43_wifi_scan_active(&cyw43_state)) {
        DPRINTF("Continue scanning...\n");
        wifiScanInProgress = false;
      }
      *wifiScanTime = make_timeout_time_ms(wifiScanInterval * SEC_TO_MS);
    }
  }
  // else {
  //     DPRINTF("Scan already in progress\n");
  // }
}

int network_scanIsActive() {
  if (!cyw43Initialized) {
    // If the network is not initialized, we cancel the scan
    DPRINTF("WiFi not initialized.\n");
    return -1;
  }
  return (int)cyw43_wifi_scan_active(&cyw43_state);
}

/**
 * @brief Return the list of found networks.
 *
 * This function returns a pointer to the list of Wi-Fi networks that have been
 * found during a scan.
 *
 * @return wifi_scan_data_t* Pointer to the list of found Wi-Fi networks.
 */
wifi_scan_data_t *network_getFoundNetworks() { return &wifiScanData; }

static void wifiLinkCallback(struct netif *netif) {
  DPRINTF("WiFi Link: %s\n", (netif_is_link_up(netif) ? "UP" : "DOWN"));
  if (!netif_is_link_up(netif)) {
    // Drop currentIp / status / status string so callers don't keep
    // serving stale values after a link drop.
    network_resetConnectionState();
  }
}

static void networkStatusCallback(struct netif *netif) {
  DPRINTF("WiFi Status: %s\n", (netif_is_up(netif) ? "UP" : "DOWN"));
  if (netif_is_up(netif)) {
    connectionStatus = CONNECTED_WIFI_IP;
    snprintf(connectionStatusStr, sizeof(connectionStatusStr), "LINK UP");
    DPRINTF("IP address allocated: %s\n", ipaddr_ntoa(netif_ip_addr4(netif)));
    ip_addr_set(&currentIp, netif_ip_addr4(netif));
  } else {
    network_resetConnectionState();
  }
}

const char *network_WifiStaConnStatusString(
    wifi_sta_conn_process_status_t status) {
  switch (status) {
    case NETWORK_WIFI_STA_CONN_OK:
      return "Connection successful";
    case NETWORK_WIFI_STA_CONN_ERR_NOT_INITIALIZED:
      return "WiFi not initialized ";
    case NETWORK_WIFI_STA_CONN_ERR_INVALID_MODE:
      return "Invalid WiFi mode    ";
    case NETWORK_WIFI_STA_CONN_ERR_MAC_FAILED:
      return "Failed to get MAC    ";
    case NETWORK_WIFI_STA_CONN_ERR_NO_SSID:
      return "No SSID provided     ";
    case NETWORK_WIFI_STA_CONN_ERR_NO_AUTH_MODE:
      return "No auth mode provided ";
    case NETWORK_WIFI_STA_CONN_ERR_CONNECTION_FAILED:
      return "Failed connecting WiFi";
    case NETWORK_WIFI_STA_CONN_ERR_TIMEOUT:
      return "Connection timeout    ";
    default:
      return "Unknown error";
  }
}

#if LWIP_MDNS_RESPONDER
static void srv_txt(struct mdns_service *service, void *txt_userdata) {
  err_t res;
  LWIP_UNUSED_ARG(txt_userdata);

  res = mdns_resp_add_service_txtitem(service, "path=/", 6);
  LWIP_ERROR("mdns add service txt failed\n", (res == ERR_OK), return);
}
#endif

wifi_sta_conn_process_status_t network_wifiStaConnect() {
  if (!cyw43Initialized) {
    DPRINTF("WiFi not initialized. Cancelling connection\n");
    return NETWORK_WIFI_STA_CONN_ERR_NOT_INITIALIZED;
  }
  if (wifiCurrentMode != WIFI_MODE_STA) {
    DPRINTF("WiFi mode is not STA. Cancelling connection\n");
    return NETWORK_WIFI_STA_CONN_ERR_INVALID_MODE;
  }

  int res;

  // Set the STA mode interface and tear down any state from a previous
  // connect attempt (mDNS service, stale IP/status).
  struct netif *nif = &cyw43_state.netif[CYW43_ITF_STA];
  network_resetStaInterface(nif);

  // Hostname is optional; PARAM_HOSTNAME may be missing entirely.
  SettingsConfigEntry *hostnameEntry =
      settings_find_entry(gconfig_getContext(), PARAM_HOSTNAME);
  const char *hostname =
      (hostnameEntry != NULL) ? hostnameEntry->value : NULL;

  cyw43_arch_lwip_begin();

  if ((hostname != NULL) && (hostname[0] != '\0')) {
    // snprintf instead of strncpy so the buffer is always NUL-terminated
    // even when the source is exactly sizeof(wifiHostname) bytes.
    snprintf(wifiHostname, sizeof(wifiHostname), "%s", hostname);
  } else {
    snprintf(wifiHostname, sizeof(wifiHostname), "SidecarT-%s",
             picoSerialStr());
  }
  DPRINTF("Hostname: %s\n", wifiHostname);
  netif_set_hostname(nif, wifiHostname);

#if LWIP_MDNS_RESPONDER
  // Setup mdns. Initialise the responder at most once per boot, and
  // only register the netif/service if it's not already registered;
  // otherwise reconnects double-register and leak service entries.
  if (!mdnsInitialized) {
    mdns_resp_init();
    mdnsInitialized = true;
  }
  DPRINTF("mDNS host name %s.local\n", wifiHostname);
  err_t mdnsErr = mdns_resp_add_netif(nif, wifiHostname);
  if (mdnsErr == ERR_OK) {
    s8_t mdnsServiceSlot = mdns_resp_add_service(
        nif, "sidecart_httpd", "_http", DNSSD_PROTO_TCP, 80, srv_txt, NULL);
    if (mdnsServiceSlot >= 0) {
      mdnsStaRegistered = true;
    } else {
      DPRINTF("Failed to add mDNS service: %d\n", mdnsServiceSlot);
      mdns_resp_remove_netif(nif);
    }
  } else {
    DPRINTF("Failed to add mDNS netif: %d\n", mdnsErr);
  }
#endif

  // Set callbacks
  netif_set_link_callback(nif, wifiLinkCallback);
  netif_set_status_callback(nif, networkStatusCallback);

  // DHCP or static IP
  if ((settings_find_entry(gconfig_getContext(), PARAM_WIFI_DHCP) != NULL) &&
      (settings_find_entry(gconfig_getContext(), PARAM_WIFI_DHCP)->value[0] ==
           't' ||
       settings_find_entry(gconfig_getContext(), PARAM_WIFI_DHCP)->value[0] ==
           'T')) {
    DPRINTF("DHCP enabled\n");
  } else {
    DPRINTF("Static IP enabled\n");
    dhcp_stop(nif);
    ip_addr_t ipaddr;
    ip_addr_t netmask;
    ip_addr_t gwy;
    ipaddr.addr = ipaddr_addr(
        settings_find_entry(gconfig_getContext(), PARAM_WIFI_IP)->value);
    netmask.addr = ipaddr_addr(
        settings_find_entry(gconfig_getContext(), PARAM_WIFI_NETMASK)->value);
    gwy.addr = ipaddr_addr(
        settings_find_entry(gconfig_getContext(), PARAM_WIFI_GATEWAY)->value);
    netif_set_addr(nif, &ipaddr, &netmask, &gwy);
    DPRINTF("IP: %s\n", ipaddr_ntoa(&ipaddr));
    DPRINTF("Netmask: %s\n", ipaddr_ntoa(&netmask));
    DPRINTF("Gateway: %s\n", ipaddr_ntoa(&gwy));

    // Now set the DNS
    // The values in PARAM_WIFI_DNS are separated by commas. Only one or two
    // values are allowed
    SettingsConfigEntry *entry =
        settings_find_entry(gconfig_getContext(), PARAM_WIFI_DNS);
    if (entry == NULL || entry->value == NULL) {
      DPRINTF("Error: DNS configuration is missing.\n");
    } else {
      // Stack-local copy avoids strdup()/free() and the leak-path on
      // early-return that the previous version had. Tolerate whitespace
      // around the comma separator (e.g. "8.8.8.8, 8.8.4.4") via
      // network_trim_ascii_spaces.
      char dnsCopy[(NETWORK_MAX_STRING_LENGTH * 2) + 2] = {0};
      snprintf(dnsCopy, sizeof(dnsCopy), "%s", entry->value);

      char *dns1 = network_trim_ascii_spaces(dnsCopy);
      char *dns2 = strchr(dns1, ',');
      if (dns2 != NULL) {
        *dns2++ = '\0';
        dns2 = network_trim_ascii_spaces(dns2);
      }

      ip_addr_t dns1Ip;
      ip_addr_t dns2Ip;
      if (dns1[0] == '\0' ||
          (dns1Ip.addr = ipaddr_addr(dns1)) == IPADDR_NONE) {
        DPRINTF("Error: Invalid DNS1 address.\n");
      } else {
        dns_setserver(0, &dns1Ip);
        DPRINTF("DNS1: %s\n", ipaddr_ntoa(&dns1Ip));

        if (dns2 != NULL && dns2[0] != '\0') {
          if ((dns2Ip.addr = ipaddr_addr(dns2)) == IPADDR_NONE) {
            DPRINTF("Error: Invalid DNS2 address.\n");
          } else {
            dns_setserver(1, &dns2Ip);
            DPRINTF("DNS2: %s\n", ipaddr_ntoa(&dns2Ip));
          }
        }
      }
    }
  }
  netif_set_up(nif);

  cyw43_arch_lwip_end();

  // Get the MAC address
  if ((res = cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, cyw43Mac))) {
    DPRINTF("Failed to get MAC address: %d\n", res);
    return NETWORK_WIFI_STA_CONN_ERR_MAC_FAILED;
  }

  SettingsConfigEntry *ssid =
      settings_find_entry(gconfig_getContext(), PARAM_WIFI_SSID);
  if (ssid == NULL || ssid->value == NULL || strlen(ssid->value) == 0) {
    DPRINTF("No SSID found in config. Can't connect\n");
    return NETWORK_WIFI_STA_CONN_ERR_NO_SSID;
  }
  SettingsConfigEntry *authMode =
      settings_find_entry(gconfig_getContext(), PARAM_WIFI_AUTH);
  if (authMode == NULL || authMode->value == NULL ||
      strlen(authMode->value) == 0) {
    DPRINTF("No auth mode found in config. Can't connect\n");
    return NETWORK_WIFI_STA_CONN_ERR_NO_AUTH_MODE;
  }
  // Copy the password into a stack buffer instead of strdup'ing it; this
  // removes the heap allocation (and the leak path that early-returned
  // through the previous strdup/free pair) and keeps the credential off
  // the heap.
  char passwordValueBuf[MAX_PASSWORD_LENGTH] = {0};
  const char *passwordValue = NULL;
  SettingsConfigEntry *password =
      settings_find_entry(gconfig_getContext(), PARAM_WIFI_PASSWORD);
  if (password != NULL && password->value != NULL &&
      strlen(password->value) > 0) {
    snprintf(passwordValueBuf, sizeof(passwordValueBuf), "%s",
             password->value);
    passwordValue = passwordValueBuf;
  } else {
    DPRINTF(
        "No password found in config. Trying to connect without password\n");
  }
  DPRINTF("The password is: %s\n",
          passwordValue != NULL ? passwordValue : "<null>");

  snprintf(wifiNetworkInfo.ssid, sizeof(wifiNetworkInfo.ssid), "%s", ssid->value);
  wifiNetworkInfo.auth_mode = (uint16_t)atoi(authMode->value);
  wifiNetworkInfo.bssid[0] = '\0';
  wifiNetworkInfo.rssi = INT16_MIN;

  uint32_t authValue = getAuthPicoCode(atoi(authMode->value));
  int errorCode = 0;
  DPRINTF("Connecting to SSID=%s, password=%s, auth=%08x. ASYNC\n", ssid->value,
          passwordValue != NULL ? passwordValue : "<null>", authValue);
  errorCode =
      cyw43_arch_wifi_connect_async(ssid->value, passwordValue, authValue);
  if (errorCode != 0) {
    DPRINTF("Failed to connect to WiFi: %d\n", errorCode);
    return NETWORK_WIFI_STA_CONN_ERR_CONNECTION_FAILED;
  }

  // Enter a loop until the device has a WiFi connection with an IP address. Or
  // timesout.
  wifi_sta_conn_status_t prevStatus = DISCONNECTED;
  int wifiConnPollingInterval = 1;  // 1 seconds
  absolute_time_t wifiConnStatusTime = make_timeout_time_ms(1 * SEC_TO_MS);
  absolute_time_t wifiConnConnTimeout =
      make_timeout_time_ms(NETWORK_CONNECT_TIMEOUT * SEC_TO_MS);  // 30 seconds
  while (absolute_time_diff_us(get_absolute_time(), wifiConnConnTimeout) > 0) {
#ifdef BLINK_H
    blink_morse('T');
#endif

    wifi_sta_conn_status_t status =
        network_wifiConnStatus(&wifiConnStatusTime, wifiConnPollingInterval);
#if PICO_CYW43_ARCH_POLL
    network_safePoll();
    cyw43_arch_wait_for_work_until(make_timeout_time_ms(2 * SEC_TO_MS));
#else
    sleep_ms(NETWORK_POLLING_INTERVAL);
#endif
    if (networkPollingCallback != NULL) {
      networkPollingCallback();
    }
    if (status != prevStatus) {
      DPRINTF("WiFi connection status: %s[%i]\n", network_wifiConnStatusStr(),
              status);
      prevStatus = status;
    }
    if (status == CONNECTED_WIFI_IP) {
#ifdef BLINK_H
      blink_on();
#endif
      break;
    }
  }
  if (absolute_time_diff_us(get_absolute_time(), wifiConnConnTimeout) <= 0) {
    DPRINTF("WiFi connection timeout\n");
    // Return the error code
    return NETWORK_WIFI_STA_CONN_ERR_TIMEOUT;
  }

  DPRINTF("Connected. Check the connection status...\n");
  network_updateCurrentNetworkInfoRadio();
  return 0;
}

char *network_wifiConnStatusStr() { return connectionStatusStr; }

const char *network_getWifiModeStr() {
  switch (wifiCurrentMode) {
    case WIFI_MODE_STA:
      return "STA";
    case WIFI_MODE_AP:
      return "AP";
    default:
      return "UNKNOWN";
  }
}

wifi_sta_conn_status_t network_wifiConnStatus(
    absolute_time_t *wifiConnStatusTime, int wifiConStatusInterval) {
  if (!cyw43Initialized) {
    DPRINTF("WiFi not initialized. Cancelling connection\n");
    return -1;
  }
  if (wifiCurrentMode != WIFI_MODE_STA) {
    DPRINTF("WiFi mode is not STA. Cancelling connection\n");
    return -2;
  }
  // If wifi_conn_status_time is NULL or the time has elapsed, check the
  // connection status
  if ((wifiConnStatusTime == NULL) ||
      (absolute_time_diff_us(get_absolute_time(), *wifiConnStatusTime) < 0)) {
    // Check the connection status
    int linkStatus = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    switch (linkStatus) {
      case CYW43_LINK_DOWN: {
        connectionStatus = DISCONNECTED;
        snprintf(connectionStatusStr, sizeof(connectionStatusStr), "LINK DOWN");
        break;
      }
      case CYW43_LINK_JOIN: {
        connectionStatus = CONNECTED_WIFI;
        snprintf(connectionStatusStr, sizeof(connectionStatusStr), "LINK JOIN");
        break;
      }
      case CYW43_LINK_NOIP: {
        connectionStatus = CONNECTED_WIFI_NO_IP;
        snprintf(connectionStatusStr, sizeof(connectionStatusStr),
                 "LINK NO IP");
        break;
      }
      case CYW43_LINK_UP: {
        connectionStatus = CONNECTED_WIFI_IP;
        snprintf(connectionStatusStr, sizeof(connectionStatusStr), "LINK UP");
        network_updateCurrentNetworkInfoRadio();
        break;
      }
      case CYW43_LINK_FAIL: {
        connectionStatus = GENERIC_ERROR;
        snprintf(connectionStatusStr, sizeof(connectionStatusStr), "LINK FAIL");
        break;
      }
      case CYW43_LINK_NONET: {
        connectionStatus = CONNECT_FAILED_ERROR;
        snprintf(connectionStatusStr, sizeof(connectionStatusStr),
                 "LINK NO NET");
        break;
      }
      case CYW43_LINK_BADAUTH: {
        connectionStatus = BADAUTH_ERROR;
        snprintf(connectionStatusStr, sizeof(connectionStatusStr),
                 "LINK BAD AUTH");
        break;
      }
      default: {
        connectionStatus = GENERIC_ERROR;
        snprintf(connectionStatusStr, sizeof(connectionStatusStr),
                 "LINK UNKNOWN");
      }
    }
    *wifiConnStatusTime =
        make_timeout_time_ms(wifiConStatusInterval * SEC_TO_MS);
  }
  // else {
  //     DPRINTF("Connection status check skipped\n");
  // }
  return connectionStatus;
}

/**
 * @brief Retrieves the current IP address.
 *
 * This function returns the current IP address stored in the system.
 *
 * @return The current IP address as an ip_addr_t structure.
 */
ip_addr_t network_getCurrentIp() { return currentIp; }

wifi_network_info_t network_getCurrentNetworkInfo() { return wifiNetworkInfo; }

const char *network_getMcuArchStr() {
#if PICO_RP2350
  return "RP2350";
#elif PICO_RP2040
  return "RP2040";
#else
  return "UNKNOWN";
#endif
}

const char *network_getMcuIdStr() { return picoSerialStr(); }

const char *network_getCyw43MacStr() {
  if (!cyw43Initialized) {
    cyw43MacStr[0] = '\0';
    return cyw43MacStr;
  }

  int res = cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, cyw43Mac);
  if (res != 0) {
    DPRINTF("Failed to get MAC address: %d\n", res);
    cyw43MacStr[0] = '\0';
    return cyw43MacStr;
  }

  snprintf(cyw43MacStr, sizeof(cyw43MacStr), "%02x:%02x:%02x:%02x:%02x:%02x",
           cyw43Mac[0], cyw43Mac[1], cyw43Mac[2], cyw43Mac[3], cyw43Mac[4],
           cyw43Mac[5]);
  return cyw43MacStr;
}

bool network_getCurrentRssi(int32_t *rssi) {
  if (rssi == NULL) {
    return false;
  }
  *rssi = 0;
  if (!cyw43Initialized || wifiCurrentMode != WIFI_MODE_STA) {
    return false;
  }
  int res = cyw43_wifi_get_rssi(&cyw43_state, rssi);
  if (res != 0) {
    DPRINTF("Failed to get RSSI: %d\n", res);
    *rssi = 0;
    return false;
  }
  return *rssi != 0;
}

const char *network_getSignalQualityLabel(int32_t rssi) {
  if (rssi >= -30) return "Excellent";
  if (rssi >= -40) return "Very good";
  if (rssi >= -50) return "Good";
  if (rssi >= -60) return "OK";
  if (rssi >= -67) return "Fair";
  if (rssi >= -70) return "Weak";
  if (rssi >= -80) return "Very weak";
  return "Unusable";
}
