
// General utilities not specific to this app to support:
// - wifi
// - NTP
// - remote logging
// - base64 encoding
// - device sleep
//
// s60sc 2021, some functions based on code contributed by gemi254

#include "appGlobals.h"

bool dbgVerbose = false;
bool timeSynchronized = false;
bool monitorOpen = true;
bool dataFilesChecked = false;
// allow any startup failures to be reported via browser for remote devices
char startupFailure[50] = {0};

/************************** Wifi **************************/

char hostName[32] = ""; // Default Host name
char ST_SSID[32]  = ""; //Default router ssid
char ST_Pass[MAX_PWD_LEN] = ""; //Default router passd

// leave following blank for dhcp
char ST_ip[16]  = ""; // Static IP
char ST_sn[16]  = ""; // subnet normally 255.255.255.0
char ST_gw[16]  = ""; // gateway to internet, normally router IP
char ST_ns1[16] = ""; // DNS Server, can be router IP (needed for SNTP)
char ST_ns2[16] = ""; // alternative DNS Server, can be blank

// Access point Config Portal SSID and Pass
char AP_SSID[32] = "";
char AP_Pass[MAX_PWD_LEN] = "";
char AP_ip[16]  = ""; //Leave blank to use 192.168.4.1
char AP_sn[16]  = "";
char AP_gw[16]  = "";

// basic HTTP Authentication access to web page
char Auth_Name[16] = ""; 
char Auth_Pass[MAX_PWD_LEN] = "";

int responseTimeoutSecs = 10; // time to wait for FTP or SMTP response
bool allowAP = true;  // set to true to allow AP to startup if cannot connect to STA (router)
int wifiTimeoutSecs = 30; // how often to check wifi status

static esp_ping_handle_t pingHandle = NULL;
static void startPing();

static void setupMdnsHost() {  
  // set up MDNS service 
  char mdnsName[15]; // max mdns host name length
  snprintf(mdnsName, 15, hostName);
  if (MDNS.begin(mdnsName)) {
    // Add service to MDNS
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("ws", "udp", 83);
    // MDNS.addService("ftp", "tcp", 21);    
    LOG_INF("mDNS service: http://%s.local", mdnsName);
  } else LOG_ERR("mDNS host: %s Failed", mdnsName);
  debugMemory("setupMdnsHost");
}

const char* getEncType(int ssidIndex) {
  switch (WiFi.encryptionType(ssidIndex)) {
    case (WIFI_AUTH_OPEN):
      return "Open";
    case (WIFI_AUTH_WEP):
      return "WEP";
    case (WIFI_AUTH_WPA_PSK):
      return "WPA_PSK";
    case (WIFI_AUTH_WPA2_PSK):
      return "WPA2_PSK";
    case (WIFI_AUTH_WPA_WPA2_PSK):
      return "WPA_WPA2_PSK";
    case (WIFI_AUTH_WPA2_ENTERPRISE):
      return "WPA2_ENTERPRISE";
    case (WIFI_AUTH_MAX):
      return "AUTH_MAX";
    default:
      return "Not listed";
  }
  return "n/a";
}

static void onWiFiEvent(WiFiEvent_t event) {
  // callback to report on wifi events
  if (event == ARDUINO_EVENT_WIFI_READY);
  else if (event == ARDUINO_EVENT_WIFI_SCAN_DONE);  
  else if (event == ARDUINO_EVENT_WIFI_STA_START) LOG_INF("Wifi Station started, connecting to: %s", ST_SSID);
  else if (event == ARDUINO_EVENT_WIFI_STA_STOP) LOG_INF("Wifi Station stopped");
  else if (event == ARDUINO_EVENT_WIFI_AP_START) LOG_INF("Wifi AP SSID: %s started, use 'http://%s' to connect", AP_SSID, WiFi.softAPIP().toString().c_str());
  else if (event == ARDUINO_EVENT_WIFI_AP_STOP) LOG_INF("Wifi AP stopped");
  else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) LOG_INF("Wifi Station IP, use 'http://%s' to connect", WiFi.localIP().toString().c_str()); 
  else if (event == ARDUINO_EVENT_WIFI_STA_LOST_IP) LOG_INF("Wifi Station lost IP");
  else if (event == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED);
  else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) LOG_INF("WiFi Station connection to %s, using hostname: %s", ST_SSID, hostName);
  else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) LOG_INF("WiFi Station disconnected");
  else if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) LOG_INF("WiFi AP client connection");
  else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) LOG_INF("WiFi AP client disconnection");
  else LOG_WRN("WiFi Unhandled event %d", event);
}

static bool setWifiAP() {
  // Set access point with static ip if provided
  if (strlen(AP_ip) > 1) {
    LOG_INF("Set AP static IP :%s, %s, %s", AP_ip, AP_gw, AP_sn);  
    IPAddress _ip, _gw, _sn, _ns1 ,_ns2;
    _ip.fromString(AP_ip);
    _gw.fromString(AP_gw);
    _sn.fromString(AP_sn);
    // set static ip
    WiFi.softAPConfig(_ip, _gw, _sn);
  } 
  WiFi.softAP(AP_SSID, AP_Pass);
  return true;
}

static bool setWifiSTA() {
  // set station with static ip if provided
  if (strlen(ST_SSID)) { 
    if (strlen(ST_ip) > 1) {
      IPAddress _ip, _gw, _sn, _ns1, _ns2;
      if (!_ip.fromString(ST_ip)) LOG_ERR("Failed to parse IP: %s", ST_ip);
      else {
        _ip.fromString(ST_ip);
        _gw.fromString(ST_gw);
        _sn.fromString(ST_sn);
        _ns1.fromString(ST_ns1);
        _ns2.fromString(ST_ns2);
        // set static ip
        WiFi.config(_ip, _gw, _sn, _ns1); // need DNS for SNTP
        LOG_INF("Wifi Station set static IP");
      } 
    } else LOG_INF("Wifi Station IP from DHCP");
    WiFi.begin(ST_SSID, ST_Pass);
    return true;
  } else LOG_WRN("No Station SSID provided, use AP");
  return false;
}

bool startWifi(bool firstcall) {
  // start wifi station (and wifi AP if allowed or station not defined)
  WiFi.disconnect();
  if (firstcall) {
    WiFi.persistent(false); // prevent the flash storage WiFi credentials
    WiFi.setAutoReconnect(false); // Set whether module will attempt to reconnect to an access point in case it is disconnected
    WiFi.setAutoConnect(false);
    WiFi.onEvent(onWiFiEvent);
    WiFi.setHostname(hostName);
  }
  WiFi.mode(WIFI_AP_STA);
  bool station = setWifiSTA();
  debugMemory("setWifiSTA");
  if (firstcall && (!station || allowAP)) setWifiAP(); // AP always allowed if no Station SSID eg on first time use
  debugMemory("setWifiAP");
  if (station) {
    // connect to Wifi station
    uint32_t startAttemptTime = millis();
    // Stop trying on failure timeout, will try to reconnect later by ping
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 5000)  {
      Serial.print(".");
      delay(500);
      Serial.flush();
    }
    if (pingHandle == NULL) startPing();
    debugMemory("startPing");
  }
#if CONFIG_IDF_TARGET_ESP32S3
  setupMdnsHost(); // not on ESP32 as uses 6k of heap
#endif
  // show stats of requested SSID
  int numNetworks = WiFi.scanNetworks();
  for (int i=0; i < numNetworks; i++) {
    if (!strcmp(WiFi.SSID(i).c_str(), ST_SSID))
      LOG_INF("Wifi stats - signal strength: %d dBm; Encryption: %s; channel: %u", WiFi.RSSI(i), getEncType(i), WiFi.channel(i));
  }
  return WiFi.status() == WL_CONNECTED ? true : false;
}

static void pingSuccess(esp_ping_handle_t hdl, void *args) {
  if (!timeSynchronized) getLocalNTP();
  if (!dataFilesChecked) dataFilesChecked = checkDataFiles();
  if (doRemoteStream) startWebsocketClient();
}

static void pingTimeout(esp_ping_handle_t hdl, void *args) {
//  esp_ping_stop(pingHandle);
//  esp_ping_delete_session(pingHandle);
//  pingHandle = NULL;
  LOG_WRN("Failed to ping gateway, restart wifi ...");
  startWifi(false);
}

static void startPing() {
  IPAddress ipAddr = WiFi.gatewayIP();
  ip_addr_t pingDest; 
  IP_ADDR4(&pingDest, ipAddr[0], ipAddr[1], ipAddr[2], ipAddr[3]);
  esp_ping_config_t pingConfig = ESP_PING_DEFAULT_CONFIG();
  pingConfig.target_addr = pingDest;  
  pingConfig.count = ESP_PING_COUNT_INFINITE;
  pingConfig.interval_ms = wifiTimeoutSecs * 1000;
  pingConfig.timeout_ms = 5000;
#if CONFIG_IDF_TARGET_ESP32S3
  pingConfig.task_stack_size = 1024 * 6;
#else
  pingConfig.task_stack_size = 1024 * 4;
#endif
  pingConfig.task_prio = 1;
  // set ping task callback functions 
  esp_ping_callbacks_t cbs;
  cbs.on_ping_success = pingSuccess;
  cbs.on_ping_timeout = pingTimeout;
  cbs.on_ping_end = NULL; 
  cbs.cb_args = NULL;
  esp_ping_new_session(&pingConfig, &cbs, &pingHandle);
  esp_ping_start(pingHandle);
  LOG_INF("Started ping monitoring");
}


/************************** NTP  **************************/

char timezone[64] = "GMT0BST,M3.5.0/01,M10.5.0/02"; 

static inline time_t getEpoch() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec;
}

void dateFormat(char* inBuff, size_t inBuffLen, bool isFolder) {
  // construct filename from date/time
  time_t currEpoch = getEpoch();
  if (isFolder) strftime(inBuff, inBuffLen, "/%Y%m%d", localtime(&currEpoch));
  else strftime(inBuff, inBuffLen, "/%Y%m%d/%Y%m%d_%H%M%S", localtime(&currEpoch));
}

bool getLocalNTP() {
  // get current time from NTP server and apply to ESP32
  const char* ntpServer = "pool.ntp.org";
  configTzTime(timezone, ntpServer);
  if (getEpoch() > 10000) {
    time_t currEpoch = getEpoch();
    char timeFormat[20];
    strftime(timeFormat, sizeof(timeFormat), "%d/%m/%Y %H:%M:%S", localtime(&currEpoch));
    timeSynchronized = true;
    LOG_INF("Got current time from NTP: %s", timeFormat);
    return true;
  }
  else {
    LOG_WRN("Not yet synced with NTP");
    return false;
  }
}

void syncToBrowser(const char *val) {
  if (timeSynchronized) return;
  
  // Synchronize clock to browser clock if no sync with NTP
  LOG_INF("Sync clock to: %s with tz:%s", val, timezone);
  struct tm now;
  getLocalTime(&now, 0);

  int Year, Month, Day, Hour, Minute, Second ;
  sscanf(val, "%d-%d-%dT%d:%d:%d", &Year, &Month, &Day, &Hour, &Minute, &Second);

  struct tm t;
  t.tm_year = Year - 1900;
  t.tm_mon  = Month - 1;    // Month, 0 - jan
  t.tm_mday = Day;          // Day of the month
  t.tm_hour = Hour;
  t.tm_min  = Minute;
  t.tm_sec  = Second;

  time_t t_of_day = mktime(&t);
  timeval epoch = {t_of_day, 0};
  struct timezone utc = {0, 0};
  settimeofday(&epoch, &utc);
  //setenv("TZ", timezone, 1);
//  Serial.print(&now, "Before sync: %B %d %Y %H:%M:%S (%A) ");
  getLocalTime(&now, 0);
//  Serial.println(&now, "After sync: %B %d %Y %H:%M:%S (%A)");
  timeSynchronized = true;
}

void formatElapsedTime(char* timeStr, uint32_t timeVal) {
  uint32_t secs = timeVal / 1000; //convert milliseconds to seconds
  uint32_t mins = secs / 60; //convert seconds to minutes
  uint32_t hours = mins / 60; //convert minutes to hours
  uint32_t days = hours / 24; //convert hours to days
  secs = secs - (mins * 60); //subtract the converted seconds to minutes in order to display 59 secs max
  mins = mins - (hours * 60); //subtract the converted minutes to hours in order to display 59 minutes max
  hours = hours - (days * 24); //subtract the converted hours to days in order to display 23 hours max
  sprintf(timeStr, "%u-%02u:%02u:%02u", days, hours, mins, secs);
}


/********************** misc functions ************************/

bool changeExtension(char* outName, const char* inName, const char* newExt) {
  // replace original file extension with supplied extension
  size_t inNamePtr = strlen(inName);
  // find '.' before extension text
  while (inNamePtr > 0 && inName[inNamePtr] != '.') inNamePtr--;
  inNamePtr++;
  size_t extLen = strlen(newExt);
  memcpy(outName, inName, inNamePtr);
  memcpy(outName + inNamePtr, newExt, extLen);
  outName[inNamePtr + extLen] = 0;
  return (inNamePtr > 1) ? true : false;
}

void showProgress() {
  // show progess as dots if not verbose
  static uint8_t dotCnt = 0;
////  if (!dbgVerbose) {
    Serial.print("."); // progress marker
    if (++dotCnt >= 50) {
      dotCnt = 0;
      Serial.println("");
////    }
    Serial.flush();
  }
}

void urlDecode(char* inVal) {
  // replace url encoded characters
  std::string decodeVal(inVal); 
  std::string replaceVal = decodeVal;
  std::smatch match; 
  while (regex_search(decodeVal, match, std::regex("(%)([0-9A-Fa-f]{2})"))) {
    std::string s(1, static_cast<char>(std::strtoul(match.str(2).c_str(),nullptr,16))); // hex to ascii 
    replaceVal = std::regex_replace(replaceVal, std::regex(match.str(0)), s);
    decodeVal = match.suffix().str();
  }
  strcpy(inVal, replaceVal.c_str());
}

void listBuff (const uint8_t* b, size_t len) {
  // output buffer content as hex, 16 bytes per line
  if (!len || !b) LOG_WRN("Nothing to print");
  else {
    for (size_t i = 0; i < len; i += 16) {
      int linelen = (len - i) < 16 ? (len - i) : 16;
      for (size_t k = 0; k < linelen; k++) printf(" %02x", b[i+k]);
      puts(" ");
    }
  }
}

size_t isSubArray(uint8_t* haystack, uint8_t* needle, size_t hSize, size_t nSize) {
  // find a subarray (needle) in another array (haystack)
  size_t h = 0, n = 0; // Two pointers to traverse the arrays
  // Traverse both arrays simultaneously
  while (h < hSize && n < nSize) {
    // If element matches, increment both pointers
    if (haystack[h] == needle[n]) {
      h++;
      n++;
      // If needle is completely traversed
      if (n == nSize) return h; // position of end of needle
    } else {
      // if not, increment h and reset n
      h = h - n + 1;
      n = 0;
    }
  }
  return 0; // not found
}

void removeChar(char* s, char c) {
  // remove specified character from string
  int writer = 0, reader = 0;
  while (s[reader]) {
    if (s[reader] != c) s[writer++] = s[reader];
    reader++;       
  }
  s[writer] = 0;
}

void checkMemory() {
  LOG_INF("Free: heap %u, block: %u, pSRAM %u", ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), ESP.getFreePsram());
}

void debugMemory(const char* caller) {
  if (CHECK_MEM) {
    delay(FLUSH_DELAY);
    printf("%s > Free: heap %u, block: %u, pSRAM %u\n", caller, ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), ESP.getFreePsram());
  }
}

void doRestart(String restartStr) {
  flush_log(true);
  LOG_WRN("Controlled restart: %s", restartStr.c_str());
  delay(2000);
  ESP.restart();
}

float smooth(float latestVal, float smoothedVal, float alpha) {
  // simple Exponential Moving Average filter 
  // where alpha between 0.0 (max smooth) and 1.0 (no smooth)
  return (latestVal * alpha) + smoothedVal * (1.0 - alpha);
}

/*********************** Remote loggging ***********************/
/*
 * Log mode selection in user interface: 
 * false : log to serial / web monitor only
 * true  : also saves log on SD card. To download the log generated, either:
 *  - To view the log, press Show Log button on the browser
 * - To clear the log file contents, on log web page press Clear Log link
 */
 
#define MAX_FMT 1000
#define MAX_OUT 1100
static va_list arglist;
static char fmtBuf[MAX_FMT];
static char outBuf[MAX_OUT];
static TaskHandle_t logHandle = NULL;
static SemaphoreHandle_t logSemaphore = NULL;
static SemaphoreHandle_t logMutex = NULL;
static int logWait = 100; // ms
bool useLogColors = false;  // true to colorise log messages (eg if using idf.py, but not arduino)


#define LOG_FORMAT_BUF_LEN 512
#define WRITE_CACHE_CYCLE 5
bool logMode = false; // 
static FILE* log_remote_fp = NULL;
static uint32_t counter_write = 0;

void flush_log(bool andClose) {
  if (log_remote_fp != NULL) {
    fsync(fileno(log_remote_fp));  
    fflush(log_remote_fp);
    if (andClose) {
      LOG_INF("Closed SD file for logging");
      fclose(log_remote_fp);
      log_remote_fp = NULL;
    } else delay(1000);
  }  
}

static void remote_log_init_SD() {
#if !CONFIG_IDF_TARGET_ESP32C3
  SD_MMC.mkdir(DATA_DIR);
  // Open remote file
  log_remote_fp = NULL;
  log_remote_fp = fopen("/sdcard" LOG_FILE_PATH, "a");
  if (log_remote_fp == NULL) {LOG_ERR("Failed to open SD log file %s", LOG_FILE_PATH);}
  else {LOG_INF("Opened SD file for logging");}
#endif
}

void reset_log() {
#if !CONFIG_IDF_TARGET_ESP32C3
  flush_log(true); // Close log file
  SD_MMC.remove(LOG_FILE_PATH);
  LOG_INF("Cleared log file");
  if (logMode) remote_log_init_SD();   
#endif
}

void remote_log_init() {
  // setup required log mode
  if (logMode) {
    flush_log(false);
    remote_log_init_SD();
  } else flush_log(true);
}

static void logTask(void *arg) {
  // separate task to reduce stack size in other tasks
  while(true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vsnprintf(outBuf, MAX_OUT, fmtBuf, arglist);
    va_end(arglist);
    xSemaphoreGive(logSemaphore);
  }
}

void logPrint(const char *format, ...) {
  // feeds logTask to format message, then outputs as required
  if (xSemaphoreTake(logMutex, logWait / portTICK_PERIOD_MS) == pdTRUE) {
    strncpy(fmtBuf, format, MAX_FMT);
    va_start(arglist, format); 
    vTaskPrioritySet(logHandle, uxTaskPriorityGet(NULL) + 1);
    xTaskNotifyGive(logHandle);
    xSemaphoreTake(logSemaphore, portMAX_DELAY); // wait for logTask to complete        
    // output to monitor console if attached
    if (monitorOpen) Serial.print(outBuf); 
    else delay(10); // allow time for other tasks
    // output to SD if file opened
    if (log_remote_fp != NULL) {
      fwrite(outBuf, sizeof(char), strlen(outBuf), log_remote_fp); // log.txt
      // periodic sync to SD
      if (counter_write++ % WRITE_CACHE_CYCLE == 0) fsync(fileno(log_remote_fp));
    }
    // output to web socket if open
    outBuf[strlen(outBuf) - 1] = 0; // lose final '/n'
    wsAsyncSend(outBuf);
    delay(FLUSH_DELAY);
    xSemaphoreGive(logMutex);
  } 
}

void logSetup() {
  // prep logging environment
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  Serial.println(); 
  debugMemory("init");
  logSemaphore = xSemaphoreCreateBinary(); // flag that log message formatted
  logMutex = xSemaphoreCreateMutex(); // control access to log formatter
  xSemaphoreGive(logSemaphore);
  xSemaphoreGive(logMutex);
  xTaskCreate(logTask, "logTask", 1024 * 2, NULL, 1, &logHandle);
  debugMemory("logSetup");
}

void formatHex(const char* inData, size_t inLen) {
  // format data as hex bytes for output
  char formatted[(inLen * 3) + 1];
  for (int i=0; i<inLen; i++) sprintf(formatted + (i*3), "%02x ", inData[i]);
  formatted[(inLen * 3)] = 0; // terminator
  LOG_WRN("Hex: %s", formatted);
}

/****************** base 64 ******************/

#define BASE64 "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

const uint8_t* encode64chunk(const uint8_t* inp, int rem) {
  // receive 3 byte input buffer and return 4 byte base64 buffer
  rem = 3 - rem; // last chunk may be less than 3 bytes 
  uint32_t buff = 0; // hold 3 bytes as shifted 24 bits
  static uint8_t b64[4];
  // shift input into buffer
  for (int i = 0; i < 3 - rem; i++) buff |= inp[i] << (8*(2-i)); 
  // shift 6 bit output from buffer and encode
  for (int i = 0; i < 4 - rem; i++) b64[i] = BASE64[buff >> (6*(3-i)) & 0x3F]; 
  // filler for last chunk if less than 3 bytes
  for (int i = 0; i < rem; i++) b64[3-i] = '='; 
  return b64;
}

const char* encode64(const char* inp) {
  // helper to base64 encode strings up to 90 chars long
  static char encoded[121]; // space for 4/3 expansion + terminator
  encoded[0] = 0;
  int len = strlen(inp);
  if (len > 90) {
    LOG_WRN("Input string too long: %u chars", len);
    len = 90;
  }
  for (int i = 0; i < len; i += 3) 
    strncat(encoded, (char*)encode64chunk((uint8_t*)inp + i, min(len - i, 3)), 4);
  return encoded;
}


/****************** send device to sleep (light or deep) ******************/

#include <esp_wifi.h>

void goToSleep(int wakeupPin, bool deepSleep) {
#if !CONFIG_IDF_TARGET_ESP32C3
  // if deep sleep, restarts with reset
  // if light sleep, restarts by continuing this function
  LOG_INF("Going into %s sleep", deepSleep ? "deep" : "light");
  delay(100);
  if (deepSleep) {
    esp_sleep_enable_ext0_wakeup((gpio_num_t)wakeupPin, 1); // wakeup on pin high
    esp_deep_sleep_start();
  } else {
    // light sleep
    esp_wifi_stop();
    gpio_wakeup_enable((gpio_num_t)wakeupPin, GPIO_INTR_HIGH_LEVEL); // wakeup on pin high
    esp_light_sleep_start();
  }
  // light sleep restarts here
  LOG_INF("Light sleep wakeup");
  esp_wifi_start();
#else
  LOG_WRN("This function not compatible with ESP32-C3");
#endif
}
