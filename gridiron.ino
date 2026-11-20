#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <PNGdec.h>
#include <time.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdlib>

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "secrets.h"

// --------------------------- GAME DATA TYPES ---------------------------

struct Game {
  String id;
  String leagueCode;
  String status;
  String kickoffUtc;
  String homeName;
  String awayName;
  String homeShort;
  String awayShort;
  String homeLogo;
  String awayLogo;
  int homeScore = -1;
  int awayScore = -1;
  int period = 0;
  bool valid = false;
};

struct TeamStyle {
  uint16_t primary;
  uint16_t secondary;
  String abbreviation;
};

// Explicit prototypes for custom data types.
static bool kickoffIsToday(const Game &game);
static bool kickoffIsFuture(const Game &game);
static Game parseMatch(JsonObjectConst item, const String &leagueCode);
static bool fetchLeagueMatches(const char *leagueCode, std::vector<Game> &outGames);
static int priorityRank(const Game &game, const char *const *priority, int count);

static bool earliestScheduledForRank(
  const std::vector<Game> &games,
  const char *const *priority,
  int count,
  int rank,
  bool todayOnly,
  Game &result
);

static bool chooseLiveForPriority(
  const std::vector<Game> &games,
  const char *const *priority,
  int count,
  Game &result
);

static bool chooseAnyLiveNFL(Game &result);

static bool chooseScheduledForPriority(
  const std::vector<Game> &games,
  const char *const *priority,
  int count,
  bool todayOnly,
  Game &result
);

static bool chooseGame(Game &result);
static TeamStyle styleForTeam(const String &name, const String &shortName);
static String statusLabel(const Game &game);

// --------------------------- USER SETTINGS ---------------------------

static const char *API_BASE = "https://api.bigballsdata.com";
static const char *PACIFIC_TZ = "PST8PDT,M3.2.0/2,M11.1.0/2";

static const uint32_t REFRESH_MS = 5UL * 60UL * 1000UL;
static const uint32_t WIFI_RETRY_MS = 15000UL;
static const uint32_t HTTP_TIMEOUT_MS = 12000UL;
static const size_t MAX_LOGO_BYTES = 120000;

static const bool LANDSCAPE_ROTATE_CW = true;
static const int LCD_W = 320;
static const int LCD_H = 820;
static const int UI_W = 820;
static const int UI_H = 320;

// --------------------------- DISPLAY PINS ----------------------------

#define LCD_CS    0
#define LCD_SCL   2
#define LCD_SDA   1
#define LCD_RST   16
#define LCD_DE    40
#define LCD_VSYNC 39
#define LCD_HSYNC 38
#define LCD_PCLK  41
#define LCD_R0    17
#define LCD_R1    46
#define LCD_R2    3
#define LCD_R3    8
#define LCD_R4    18
#define LCD_G0    14
#define LCD_G1    13
#define LCD_G2    12
#define LCD_G3    11
#define LCD_G4    10
#define LCD_G5    9
#define LCD_B0    21
#define LCD_B1    5
#define LCD_B2    45
#define LCD_B3    48
#define LCD_B4    47
#define LCD_BL    6

static const int LCD_PIXEL_CLOCK_HZ = 18 * 1000 * 1000;

// --------------------------- DISPLAY COLORS -------------------------

static const uint16_t C_BG       = 0x0842;
static const uint16_t C_PANEL    = 0x10A3;
static const uint16_t C_PANEL2   = 0x18E4;
static const uint16_t C_BLACK    = 0x0000;
static const uint16_t C_WHITE    = 0xFFFF;
static const uint16_t C_MUTED    = 0xBDF7;
static const uint16_t C_RED      = 0xE986;
static const uint16_t C_GOLD     = 0xFDC0;
static const uint16_t C_GREEN    = 0x35A6;

// --------------------------- DISPLAY DRIVER --------------------------

static esp_lcd_panel_handle_t panelHandle = nullptr;
static uint16_t *frameBuffer = nullptr;

class PanelCanvas : public Adafruit_GFX {
public:
  PanelCanvas() : Adafruit_GFX(UI_W, UI_H) {}

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (!frameBuffer || x < 0 || x >= UI_W || y < 0 || y >= UI_H) return;

    int nativeX;
    int nativeY;

    if (LANDSCAPE_ROTATE_CW) {
      nativeX = y;
      nativeY = LCD_H - 1 - x;
    } else {
      nativeX = LCD_W - 1 - y;
      nativeY = x;
    }

    if (nativeX >= 0 && nativeX < LCD_W && nativeY >= 0 && nativeY < LCD_H) {
      frameBuffer[nativeY * LCD_W + nativeX] = color;
    }
  }

  void fillScreen(uint16_t color) {
    if (!frameBuffer) return;
    for (int i = 0; i < LCD_W * LCD_H; ++i) {
      frameBuffer[i] = color;
    }
  }
};

PanelCanvas gfx;

static void initBacklight() {
  ledc_timer_config_t timerConf = {};
  timerConf.speed_mode = LEDC_LOW_SPEED_MODE;
  timerConf.duty_resolution = LEDC_TIMER_8_BIT;
  timerConf.timer_num = LEDC_TIMER_3;
  timerConf.freq_hz = 50000;
  timerConf.clk_cfg = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&timerConf));

  ledc_channel_config_t channelConf = {};
  channelConf.gpio_num = LCD_BL;
  channelConf.speed_mode = LEDC_LOW_SPEED_MODE;
  channelConf.channel = LEDC_CHANNEL_1;
  channelConf.intr_type = LEDC_INTR_DISABLE;
  channelConf.timer_sel = LEDC_TIMER_3;
  channelConf.duty = 0;
  channelConf.hpoint = 0;
  ESP_ERROR_CHECK(ledc_channel_config(&channelConf));
}

static void spiCommand(uint8_t command) {
  gpio_set_level((gpio_num_t)LCD_CS, 0);
  gpio_set_level((gpio_num_t)LCD_SDA, 0);
  gpio_set_level((gpio_num_t)LCD_SCL, 0);
  delayMicroseconds(1);
  gpio_set_level((gpio_num_t)LCD_SCL, 1);
  delayMicroseconds(1);

  for (int bit = 7; bit >= 0; --bit) {
    gpio_set_level((gpio_num_t)LCD_SDA, (command >> bit) & 1);
    gpio_set_level((gpio_num_t)LCD_SCL, 0);
    delayMicroseconds(1);
    gpio_set_level((gpio_num_t)LCD_SCL, 1);
    delayMicroseconds(1);
  }

  gpio_set_level((gpio_num_t)LCD_CS, 1);
}

static void spiData(uint8_t value) {
  gpio_set_level((gpio_num_t)LCD_CS, 0);
  gpio_set_level((gpio_num_t)LCD_SDA, 1);
  gpio_set_level((gpio_num_t)LCD_SCL, 0);
  delayMicroseconds(1);
  gpio_set_level((gpio_num_t)LCD_SCL, 1);
  delayMicroseconds(1);

  for (int bit = 7; bit >= 0; --bit) {
    gpio_set_level((gpio_num_t)LCD_SDA, (value >> bit) & 1);
    gpio_set_level((gpio_num_t)LCD_SCL, 0);
    delayMicroseconds(1);
    gpio_set_level((gpio_num_t)LCD_SCL, 1);
    delayMicroseconds(1);
  }

  gpio_set_level((gpio_num_t)LCD_CS, 1);
}

static void st7701Send(uint8_t command, const uint8_t *data, uint8_t length) {
  spiCommand(command);
  for (uint8_t i = 0; i < length; ++i) {
    spiData(data[i]);
  }
}

static void initST7701() {
  gpio_set_level((gpio_num_t)LCD_RST, 0);
  delay(10);
  gpio_set_level((gpio_num_t)LCD_RST, 1);
  delay(120);

  const uint8_t c1[] = {0x77, 0x01, 0x00, 0x00, 0x13}; st7701Send(0xFF, c1, 5);
  const uint8_t c2[] = {0x08}; st7701Send(0xEF, c2, 1);
  const uint8_t c3[] = {0x77, 0x01, 0x00, 0x00, 0x10}; st7701Send(0xFF, c3, 5);
  const uint8_t c4[] = {0xE5, 0x02}; st7701Send(0xC0, c4, 2);
  const uint8_t c5[] = {0x15, 0x0A}; st7701Send(0xC1, c5, 2);
  const uint8_t c6[] = {0x07, 0x02}; st7701Send(0xC2, c6, 2);
  const uint8_t c7[] = {0x10}; st7701Send(0xCC, c7, 1);

  const uint8_t b0[] = {0x00, 0x08, 0x51, 0x0D, 0xCE, 0x06, 0x00, 0x08, 0x08, 0x24, 0x05, 0xD0, 0x0F, 0x6F, 0x36, 0x1F}; st7701Send(0xB0, b0, 16);
  const uint8_t b1[] = {0x00, 0x10, 0x4F, 0x0C, 0x11, 0x05, 0x00, 0x07, 0x07, 0x18, 0x02, 0xD3, 0x11, 0x6E, 0x34, 0x1F}; st7701Send(0xB1, b1, 16);

  const uint8_t c8[] = {0x77, 0x01, 0x00, 0x00, 0x11}; st7701Send(0xFF, c8, 5);
  const uint8_t b02[] = {0x4D}; st7701Send(0xB0, b02, 1);
  const uint8_t b12[] = {0x37}; st7701Send(0xB1, b12, 1);
  const uint8_t b2[] = {0x87}; st7701Send(0xB2, b2, 1);
  const uint8_t b3[] = {0x80}; st7701Send(0xB3, b3, 1);
  const uint8_t b5[] = {0x4A}; st7701Send(0xB5, b5, 1);
  const uint8_t b7[] = {0x85}; st7701Send(0xB7, b7, 1);
  const uint8_t b8[] = {0x21}; st7701Send(0xB8, b8, 1);
  const uint8_t b9[] = {0x00, 0x13}; st7701Send(0xB9, b9, 2);
  const uint8_t c02[] = {0x09}; st7701Send(0xC0, c02, 1);
  const uint8_t c12[] = {0x78}; st7701Send(0xC1, c12, 1);
  const uint8_t c22[] = {0x78}; st7701Send(0xC2, c22, 1);
  const uint8_t d0[] = {0x88}; st7701Send(0xD0, d0, 1);

  const uint8_t e0[] = {0x80, 0x00, 0x02}; st7701Send(0xE0, e0, 3);
  delay(100);
  const uint8_t e1[] = {0x0F, 0xA0, 0x00, 0x00, 0x10, 0xA0, 0x00, 0x00, 0x00, 0x60, 0x60}; st7701Send(0xE1, e1, 11);
  const uint8_t e2[] = {0x30, 0x30, 0x60, 0x60, 0x45, 0xA0, 0x00, 0x00, 0x46, 0xA0, 0x00, 0x00, 0x00}; st7701Send(0xE2, e2, 13);
  const uint8_t e3[] = {0x00, 0x00, 0x33, 0x33}; st7701Send(0xE3, e3, 4);
  const uint8_t e4[] = {0x44, 0x44}; st7701Send(0xE4, e4, 2);
  const uint8_t e5[] = {0x0F, 0x4A, 0xA0, 0xA0, 0x11, 0x4A, 0xA0, 0xA0, 0x13, 0x4A, 0xA0, 0xA0, 0x15, 0x4A, 0xA0, 0xA0}; st7701Send(0xE5, e5, 16);
  const uint8_t e6[] = {0x00, 0x00, 0x33, 0x33}; st7701Send(0xE6, e6, 4);
  const uint8_t e7[] = {0x44, 0x44}; st7701Send(0xE7, e7, 2);
  const uint8_t e8[] = {0x10, 0x4A, 0xA0, 0xA0, 0x12, 0x4A, 0xA0, 0xA0, 0x14, 0x4A, 0xA0, 0xA0, 0x16, 0x4A, 0xA0, 0xA0}; st7701Send(0xE8, e8, 16);
  const uint8_t eb[] = {0x02, 0x00, 0x4E, 0x4E, 0xEE, 0x44, 0x00}; st7701Send(0xEB, eb, 7);
  const uint8_t ed[] = {0xFF, 0xFF, 0x04, 0x56, 0x72, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x27, 0x65, 0x40, 0xFF, 0xFF}; st7701Send(0xED, ed, 16);
  const uint8_t ef[] = {0x08, 0x08, 0x08, 0x40, 0x3F, 0x64}; st7701Send(0xEF, ef, 6);

  const uint8_t c9[] = {0x77, 0x01, 0x00, 0x00, 0x13}; st7701Send(0xFF, c9, 5);
  const uint8_t e82[] = {0x00, 0x0E}; st7701Send(0xE8, e82, 2);
  const uint8_t c10[] = {0x77, 0x01, 0x00, 0x00, 0x00}; st7701Send(0xFF, c10, 5);
  spiCommand(0x11);
  delay(120);
  const uint8_t c11[] = {0x77, 0x01, 0x00, 0x00, 0x13}; st7701Send(0xFF, c11, 5);
  const uint8_t e83[] = {0x00, 0x0C}; st7701Send(0xE8, e83, 2);
  delay(10);
  const uint8_t e84[] = {0x00, 0x00}; st7701Send(0xE8, e84, 2);
  const uint8_t c12final[] = {0x77, 0x01, 0x00, 0x00, 0x00}; st7701Send(0xFF, c12final, 5);
  const uint8_t pixfmt[] = {0x55}; st7701Send(0x3A, pixfmt, 1);
  const uint8_t madctl[] = {0x00}; st7701Send(0x36, madctl, 1);
  const uint8_t te[] = {0x00}; st7701Send(0x35, te, 1);
  spiCommand(0x29);
  delay(20);
}

static void initRgbPanel() {
  esp_lcd_rgb_panel_config_t config = {};
  config.clk_src = LCD_CLK_SRC_DEFAULT;
  config.timings.pclk_hz = LCD_PIXEL_CLOCK_HZ;
  config.timings.h_res = LCD_W;
  config.timings.v_res = LCD_H;
  config.timings.hsync_pulse_width = 6;
  config.timings.hsync_back_porch = 30;
  config.timings.hsync_front_porch = 30;
  config.timings.vsync_pulse_width = 40;
  config.timings.vsync_back_porch = 20;
  config.timings.vsync_front_porch = 20;
  config.timings.flags.pclk_active_neg = true;
  config.data_width = 16;
  config.bits_per_pixel = 16;
  config.num_fbs = 1;
  config.bounce_buffer_size_px = 10 * LCD_W;
  config.sram_trans_align = 0;
  config.psram_trans_align = 64;
  config.hsync_gpio_num = LCD_HSYNC;
  config.vsync_gpio_num = LCD_VSYNC;
  config.de_gpio_num = LCD_DE;
  config.pclk_gpio_num = LCD_PCLK;
  config.disp_gpio_num = -1;

  config.data_gpio_nums[0] = LCD_B0;
  config.data_gpio_nums[1] = LCD_B1;
  config.data_gpio_nums[2] = LCD_B2;
  config.data_gpio_nums[3] = LCD_B3;
  config.data_gpio_nums[4] = LCD_B4;
  config.data_gpio_nums[5] = LCD_G0;
  config.data_gpio_nums[6] = LCD_G1;
  config.data_gpio_nums[7] = LCD_G2;
  config.data_gpio_nums[8] = LCD_G3;
  config.data_gpio_nums[9] = LCD_G4;
  config.data_gpio_nums[10] = LCD_G5;
  config.data_gpio_nums[11] = LCD_R0;
  config.data_gpio_nums[12] = LCD_R1;
  config.data_gpio_nums[13] = LCD_R2;
  config.data_gpio_nums[14] = LCD_R3;
  config.data_gpio_nums[15] = LCD_R4;
  config.flags.fb_in_psram = true;

  ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&config, &panelHandle));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panelHandle));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panelHandle));

  void *fb = nullptr;
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panelHandle, 1, &fb));
  frameBuffer = static_cast<uint16_t *>(fb);
}

static void initDisplay() {
  gpio_set_direction((gpio_num_t)LCD_RST, GPIO_MODE_OUTPUT);
  gpio_set_direction((gpio_num_t)LCD_CS, GPIO_MODE_OUTPUT);
  gpio_set_direction((gpio_num_t)LCD_SDA, GPIO_MODE_OUTPUT);
  gpio_set_direction((gpio_num_t)LCD_SCL, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)LCD_CS, 1);
  gpio_set_level((gpio_num_t)LCD_SCL, 1);
  initBacklight();
  initST7701();
  initRgbPanel();
}

// --------------------------- GLOBAL GAME STATE -----------------------

std::vector<Game> gamesNFL;
std::vector<Game> gamesCFB;
Game selectedGame;
bool hasSelectedGame = false;
bool apiOnline = false;
String lastApiError;
String lastUpdateText = "WAITING FOR DATA";
uint32_t lastRefreshMs = 0;
uint32_t lastWifiAttemptMs = 0;

static const char *NFL_PRIORITY[] = {
  "Washington Commanders", "New York Giants", "Detroit Lions", "Buffalo Bills"
};

static const char *CFB_PRIORITY[] = {
  "Oregon", "USC", "UCLA", "San Diego State"
};

// --------------------------- TIME HELPERS ----------------------------

static bool timeIsValid() {
  return time(nullptr) > 1700000000;
}

static time_t parseIsoUtc(const String &value) {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;

  if (sscanf(value.c_str(), "%d-%d-%dT%d:%d:%d",
             &year, &month, &day, &hour, &minute, &second) < 6) {
    return 0;
  }

  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = second;

  // Convert UTC calendar fields to Unix seconds without timegm().
  int y = year;
  unsigned m = (unsigned)month;
  unsigned d = (unsigned)day;
  y -= (m <= 2);

  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned mp = (unsigned)((int)m + (m > 2 ? -3 : 9));
  const unsigned doy = (153U * mp + 2U) / 5U + d - 1U;
  const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
  const int64_t days = (int64_t)era * 146097LL + (int64_t)doe - 719468LL;

  return (time_t)(days * 86400LL + hour * 3600LL + minute * 60LL + second);
}

static bool localDayForKickoff(const String &kickoffUtc, int &year, int &month, int &day, int &weekday) {
  time_t stamp = parseIsoUtc(kickoffUtc);
  if (stamp <= 0) return false;

  struct tm localTime = {};
  localtime_r(&stamp, &localTime);

  year = localTime.tm_year;
  month = localTime.tm_mon;
  day = localTime.tm_mday;
  weekday = localTime.tm_wday;
  return true;
}

static bool isSaturdayLocal() {
  if (!timeIsValid()) return false;

  time_t now = time(nullptr);
  struct tm localTime = {};
  localtime_r(&now, &localTime);

  return localTime.tm_wday == 6;
}

static bool kickoffIsToday(const Game &game) {
  if (!timeIsValid()) return false;

  time_t stamp = parseIsoUtc(game.kickoffUtc);
  if (stamp <= 0) return false;

  time_t now = time(nullptr);
  struct tm a = {}, b = {};

  localtime_r(&stamp, &a);
  localtime_r(&now, &b);

  return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday;
}

static bool kickoffIsFuture(const Game &game) {
  // Never treat a missing/invalid kickoff or an unsynchronized clock as a
  // confirmed upcoming game. This prevents stale schedule entries appearing.
  if (!timeIsValid()) return false;

  time_t stamp = parseIsoUtc(game.kickoffUtc);
  if (stamp <= 0) return false;

  return stamp > time(nullptr) - 60;
}

static String kickoffLabel(const String &kickoffUtc) {
  time_t stamp = parseIsoUtc(kickoffUtc);
  if (stamp <= 0) return "KICKOFF TIME TBD";

  struct tm localTime = {};
  localtime_r(&stamp, &localTime);

  char buffer[40];
  strftime(buffer, sizeof(buffer), "%a %b %d  %I:%M %p", &localTime);

  String result(buffer);
  result.replace(" 0", " ");
  return result;
}

// --------------------------- TEAM MATCHING ---------------------------

static String lowerCopy(String text) {
  text.toLowerCase();
  return text;
}

static bool containsTeam(const String &name, const char *target) {
  String n = lowerCopy(name);
  String t = lowerCopy(String(target));

  if (t == "usc") {
    return n.indexOf("usc") >= 0 || n.indexOf("southern california") >= 0;
  }

  if (t == "san diego state") {
    return n.indexOf("san diego state") >= 0 || n.indexOf("sdsu") >= 0;
  }

  return n.indexOf(t) >= 0;
}

static bool isTrackedNFL(const String &home, const String &away) {
  for (const char *target : NFL_PRIORITY) {
    if (containsTeam(home, target) || containsTeam(away, target)) return true;
  }
  return false;
}

static bool isTrackedCFB(const String &home, const String &away) {
  for (const char *target : CFB_PRIORITY) {
    if (containsTeam(home, target) || containsTeam(away, target)) return true;
  }
  return false;
}

static bool isLiveStatus(const String &status) {
  String s = lowerCopy(status);
  return s == "live" || s == "in_progress" || s == "inprogress";
}

static bool isFinishedStatus(const String &status) {
  String s = lowerCopy(status);
  return s == "finished" || s == "complete" || s == "completed";
}

static int readScore(JsonVariantConst value) {
  if (value.is<int>()) return value.as<int>();
  if (value.is<long>()) return (int)value.as<long>();
  return -1;
}

static String readString(JsonVariantConst value) {
  if (value.is<const char *>()) return String(value.as<const char *>());
  return "";
}

// --------------------------- API HELPERS -----------------------------

static bool httpsGet(const String &url, String &body, const char *apiKey = API_KEY) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(client, url)) {
    lastApiError = "HTTP begin failed";
    return false;
  }

  http.addHeader("x-api-key", apiKey);

  const char *headerKeys[] = {"Content-Type"};
  http.collectHeaders(headerKeys, 1);

  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    lastApiError = "HTTP " + String(code);
    Serial.printf("API request failed (%d): %s\n", code, url.c_str());
    http.end();
    return false;
  }

  body = http.getString();
  http.end();

  if (body.length() == 0) {
    lastApiError = "Empty API response";
    return false;
  }

  return true;
}

static Game parseMatch(JsonObjectConst item, const String &leagueCode) {
  Game game;

  game.id = readString(item["id"]);
  game.leagueCode = leagueCode;
  game.status = readString(item["status"]);
  game.kickoffUtc = readString(item["kickoff_utc"]);

  JsonObjectConst home = item["home"].as<JsonObjectConst>();
  JsonObjectConst away = item["away"].as<JsonObjectConst>();

  game.homeName = readString(home["name"]);
  game.awayName = readString(away["name"]);
  game.homeShort = readString(home["short_name"]);
  game.awayShort = readString(away["short_name"]);
  game.homeLogo = readString(home["logo_url"]);
  game.awayLogo = readString(away["logo_url"]);

  JsonObjectConst score = item["score"].as<JsonObjectConst>();
  game.homeScore = readScore(score["home"]);
  game.awayScore = readScore(score["away"]);

  game.valid = game.id.length() > 0 &&
               game.homeName.length() > 0 &&
               game.awayName.length() > 0;

  return game;
}

static bool fetchLeagueMatches(const char *leagueCode, std::vector<Game> &outGames) {
  String url = String(API_BASE) +
               "/v1/matches?sport=american_football&league=" +
               leagueCode + "&limit=200";

  String body;
  if (!httpsGet(url, body)) return false;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, body);

  if (error) {
    lastApiError = String("JSON parse: ") + error.c_str();
    Serial.println(lastApiError);
    return false;
  }

  JsonVariantConst data = doc["data"];
  if (!data.is<JsonArrayConst>()) {
    lastApiError = "API data field was not an array";
    Serial.println(lastApiError);
    return false;
  }

  std::vector<Game> newGames;
  JsonArrayConst matches = data.as<JsonArrayConst>();

  for (JsonObjectConst item : matches) {
    Game game = parseMatch(item, leagueCode);
    if (!game.valid) continue;

    // Keep every NFL game so the scoreboard can fall back to any live game.
    // College football remains limited to the teams in CFB_PRIORITY.
    bool tracked = (String(leagueCode) == "nfl") ||
                   isTrackedCFB(game.homeName, game.awayName);

    if (tracked) newGames.push_back(game);
  }

  outGames.swap(newGames);

  Serial.printf("Loaded %u %s matches\\n",
                (unsigned)outGames.size(), leagueCode);
  return true;
}

// --------------------------- GAME SELECTOR ---------------------------

static int priorityRank(const Game &game, const char *const *priority, int count) {
  for (int rank = 0; rank < count; ++rank) {
    if (containsTeam(game.homeName, priority[rank]) ||
        containsTeam(game.awayName, priority[rank])) {
      return rank;
    }
  }

  return 999;
}

static bool earliestScheduledForRank(const std::vector<Game> &games,
                                     const char *const *priority, int count,
                                     int rank, bool todayOnly, Game &result) {
  bool found = false;
  time_t earliest = 0;

  for (const Game &game : games) {
    if (!game.valid || isLiveStatus(game.status) || isFinishedStatus(game.status)) continue;

    String status = lowerCopy(game.status);
    if (status != "scheduled") continue;

    if (!kickoffIsFuture(game)) continue;
    if (priorityRank(game, priority, count) != rank) continue;
    if (todayOnly && !kickoffIsToday(game)) continue;

    time_t stamp = parseIsoUtc(game.kickoffUtc);

    if (!found || (stamp > 0 && (earliest == 0 || stamp < earliest))) {
      result = game;
      earliest = stamp;
      found = true;
    }
  }

  return found;
}

static bool chooseLiveForPriority(const std::vector<Game> &games,
                                  const char *const *priority, int count,
                                  Game &result) {
  for (int rank = 0; rank < count; ++rank) {
    for (const Game &game : games) {
      if (game.valid &&
          isLiveStatus(game.status) &&
          priorityRank(game, priority, count) == rank) {
        result = game;
        return true;
      }
    }
  }

  return false;
}

static bool chooseAnyLiveNFL(Game &result) {
  for (const Game &game : gamesNFL) {
    if (game.valid && isLiveStatus(game.status)) {
      result = game;
      return true;
    }
  }

  return false;
}

static bool chooseScheduledForPriority(const std::vector<Game> &games,
                                       const char *const *priority, int count,
                                       bool todayOnly, Game &result) {
  for (int rank = 0; rank < count; ++rank) {
    if (earliestScheduledForRank(games, priority, count, rank, todayOnly, result)) {
      return true;
    }
  }

  return false;
}

static bool chooseGame(Game &result) {
  const int nflCount = sizeof(NFL_PRIORITY) / sizeof(NFL_PRIORITY[0]);
  const int cfbCount = sizeof(CFB_PRIORITY) / sizeof(CFB_PRIORITY[0]);
  const bool saturday = isSaturdayLocal();

  // 1. Preferred NFL teams that are live.
  if (chooseLiveForPriority(gamesNFL, NFL_PRIORITY, nflCount, result)) {
    return true;
  }

  // 2. Preferred college teams that are live on Saturday.
  if (saturday &&
      chooseLiveForPriority(gamesCFB, CFB_PRIORITY, cfbCount, result)) {
    return true;
  }

  // 3. Preferred college games scheduled for today on Saturday.
  if (saturday &&
      chooseScheduledForPriority(gamesCFB, CFB_PRIORITY, cfbCount, true, result)) {
    return true;
  }

  // 4. Any other live NFL game.
  if (chooseAnyLiveNFL(result)) {
    return true;
  }

  // 5. Next scheduled preferred NFL game.
  if (chooseScheduledForPriority(gamesNFL, NFL_PRIORITY, nflCount, false, result)) {
    return true;
  }

  // 6. Oregon's next scheduled college game.
  const char *oregonOnly[] = {"Oregon"};
  if (chooseScheduledForPriority(gamesCFB, oregonOnly, 1, false, result)) {
    return true;
  }

  // 7. Other preferred college games scheduled on Saturday.
  if (saturday &&
      chooseScheduledForPriority(gamesCFB, CFB_PRIORITY, cfbCount, false, result)) {
    return true;
  }

  return false;
}

// --------------------------- TEAM COLORS / LOGOS ---------------------

static TeamStyle styleForTeam(const String &name, const String &shortName) {
  String n = lowerCopy(name);

  if (n.indexOf("mississippi state") >= 0) return {0x58A4, 0xC639, "MSST"};
  if (n.indexOf("appalachian state") >= 0) return {0xFE60, 0x0000, "APP"};
  if (n.indexOf("washington state") >= 0) return {0x98E6, 0x5B4E, "WSU"};
  if (n.indexOf("coastal carolina") >= 0) return {0x036E, 0xA3AA, "CCU"};
  if (n.indexOf("san diego state") >= 0) return {0xC086, 0x0000, "SDSU"};
  if (n.indexOf("oklahoma state") >= 0) return {0xFB80, 0x0000, "OKST"};
  if (n.indexOf("michigan state") >= 0) return {0x1A27, 0xFFFF, "MSU"};
  if (n.indexOf("north carolina") >= 0) return {0x4CFA, 0xFFFF, "UNC"};
  if (n.indexOf("south carolina") >= 0) return {0x7001, 0x0000, "SC"};
  if (n.indexOf("florida state") >= 0) return {0x7968, 0xCDD1, "FSU"};
  if (n.indexOf("arizona state") >= 0) return {0x88E8, 0xFE24, "ASU"};
  if (n.indexOf("west virginia") >= 0) return {0xED40, 0x014A, "WVU"};
  if (n.indexOf("virginia tech") >= 0) return {0x6006, 0xCA24, "VT"};
  if (n.indexOf("james madison") >= 0) return {0x4010, 0xCDAE, "JMU"};
  if (n.indexOf("northwestern") >= 0) return {0x4950, 0xFFFF, "NU"};
  if (n.indexOf("kansas state") >= 0) return {0x5151, 0xD69A, "KSU"};
  if (n.indexOf("georgia tech") >= 0) return {0xB50D, 0x018A, "GT"};
  if (n.indexOf("fresno state") >= 0) return {0xD806, 0x01EF, "FRES"};
  if (n.indexOf("oregon state") >= 0) return {0xDA20, 0x0000, "ORST"};
  if (n.indexOf("boise state") >= 0) return {0x0194, 0xD201, "BSU"};
  if (n.indexOf("wake forest") >= 0) return {0x9BE7, 0x0000, "WAKE"};
  if (n.indexOf("commanders") >= 0) return {0x58A2, 0xFDA2, "WAS"};
  if (n.indexOf("buccaneers") >= 0) return {0xD041, 0x3185, "TB"};
  if (n.indexOf("ohio state") >= 0) return {0xB800, 0x632C, "OSU"};
  if (n.indexOf("notre dame") >= 0) return {0x0908, 0xCCA0, "ND"};
  if (n.indexOf("texas tech") >= 0) return {0xC800, 0x0000, "TTU"};
  if (n.indexOf("penn state") >= 0) return {0x00E8, 0xFFFF, "PSU"};
  if (n.indexOf("pittsburgh") >= 0) return {0x01B2, 0xFDC3, "PITT"};
  if (n.indexOf("washington") >= 0) return {0x4970, 0xB52F, "WASH"};
  if (n.indexOf("iowa state") >= 0) return {0xC885, 0xF5E9, "ISU"};
  if (n.indexOf("louisville") >= 0) return {0xA800, 0x0000, "LOU"};
  if (n.indexOf("vanderbilt") >= 0) return {0x8369, 0x0000, "VAN"};
  if (n.indexOf("cardinals") >= 0) return {0x9107, 0xFDA2, "ARI"};
  if (n.indexOf("texas a&m") >= 0) return {0x5000, 0xFFFF, "TAMU"};
  if (n.indexOf("wisconsin") >= 0) return {0xC021, 0xFFFF, "WISC"};
  if (n.indexOf("minnesota") >= 0) return {0x7803, 0xFE66, "MINN"};
  if (n.indexOf("air force") >= 0) return {0x0190, 0x8C71, "AFA"};
  if (n.indexOf("tennessee") >= 0) return {0xFC00, 0xFFFF, "TENN"};
  if (n.indexOf("chargers") >= 0) return {0x0418, 0xFE01, "LAC"};
  if (n.indexOf("seahawks") >= 0) return {0x0108, 0x6DE5, "SEA"};
  if (n.indexOf("steelers") >= 0) return {0xFDA2, 0x10C4, "PIT"};
  if (n.indexOf("dolphins") >= 0) return {0x0472, 0xFA60, "MIA"};
  if (n.indexOf("patriots") >= 0) return {0x0108, 0xC066, "NE"};
  if (n.indexOf("panthers") >= 0) return {0x0439, 0x10C4, "CAR"};
  if (n.indexOf("michigan") >= 0) return {0x0129, 0xFE40, "MICH"};
  if (n.indexOf("oklahoma") >= 0) return {0x80A2, 0xFFDB, "OU"};
  if (n.indexOf("nebraska") >= 0) return {0xE0E7, 0xFFFF, "NEB"};
  if (n.indexOf("illinois") >= 0) return {0xEA44, 0x1149, "ILL"};
  if (n.indexOf("colorado") >= 0) return {0xCDCF, 0x0000, "COLO"};
  if (n.indexOf("nc state") >= 0) return {0xC800, 0x0000, "NCSU"};
  if (n.indexOf("syracuse") >= 0) return {0xF340, 0x006A, "SYR"};
  if (n.indexOf("missouri") >= 0) return {0xF5C5, 0x0000, "MIZ"};
  if (n.indexOf("ole miss") >= 0) return {0xC884, 0x1107, "MISS"};
  if (n.indexOf("kentucky") >= 0) return {0x0194, 0xFFFF, "UK"};
  if (n.indexOf("arkansas") >= 0) return {0x9906, 0xFFFF, "ARK"};
  if (n.indexOf("broncos") >= 0) return {0xFA62, 0x0108, "DEN"};
  if (n.indexOf("raiders") >= 0) return {0x0000, 0xA575, "LV"};
  if (n.indexOf("cowboys") >= 0) return {0x01B2, 0x8492, "DAL"};
  if (n.indexOf("packers") >= 0) return {0x21A6, 0xFDA2, "GB"};
  if (n.indexOf("vikings") >= 0) return {0x4930, 0xFE25, "MIN"};
  if (n.indexOf("bengals") >= 0) return {0xFA62, 0x0000, "CIN"};
  if (n.indexOf("falcons") >= 0) return {0xA0C6, 0x0000, "ATL"};
  if (n.indexOf("jaguars") >= 0) return {0x032F, 0xD505, "JAX"};
  if (n.indexOf("georgia") >= 0) return {0xB865, 0x0000, "UGA"};
  if (n.indexOf("alabama") >= 0) return {0x98C6, 0xFFFF, "ALA"};
  if (n.indexOf("clemson") >= 0) return {0xF320, 0x5170, "CLEM"};
  if (n.indexOf("florida") >= 0) return {0x0114, 0xFA22, "UF"};
  if (n.indexOf("colts") >= 0 || n.indexOf("indianapolis") >= 0) return {0x011F, 0xFFFF, "IND"};
  if (n.indexOf("indiana") >= 0) return {0x9800, 0xFFFF, "IND"};
  if (n.indexOf("arizona") >= 0) return {0xA824, 0x0909, "ARIZ"};
  if (n.indexOf("houston") >= 0) return {0xC885, 0xFFFF, "HOU"};
  if (n.indexOf("memphis") >= 0) return {0x0190, 0x8C71, "MEM"};
  if (n.indexOf("liberty") >= 0) return {0x016C, 0xC086, "LIB"};
  if (n.indexOf("chiefs") >= 0) return {0xE0C6, 0xFDC3, "KC"};
  if (n.indexOf("eagles") >= 0) return {0x026A, 0xA575, "PHI"};
  if (n.indexOf("giants") >= 0) return {0x090C, 0xA0C6, "NYG"};
  if (n.indexOf("browns") >= 0) return {0x30E0, 0xF9E0, "CLE"};
  if (n.indexOf("ravens") >= 0) return {0x20AE, 0x9BE1, "BAL"};
  if (n.indexOf("saints") >= 0) return {0xD5F1, 0x10C4, "NO"};
  if (n.indexOf("titans") >= 0) return {0x0908, 0x4C9B, "TEN"};
  if (n.indexOf("texans") >= 0) return {0x0105, 0xA0C6, "HOU"};
  if (n.indexOf("oregon") >= 0) return {0x1226, 0xFF04, "ORE"};
  if (n.indexOf("purdue") >= 0) return {0x0000, 0xCDD1, "PUR"};
  if (n.indexOf("baylor") >= 0) return {0x1226, 0xFDC3, "BAY"};
  if (n.indexOf("kansas") >= 0) return {0x0297, 0xE801, "KU"};
  if (n.indexOf("auburn") >= 0) return {0x0908, 0xEBA4, "AUB"};
  if (n.indexOf("tulane") >= 0) return {0x0328, 0x447B, "TUL"};
  if (n.indexOf("49ers") >= 0) return {0xA800, 0xB4CB, "SF"};
  if (n.indexOf("bears") >= 0) return {0x08A5, 0xC9C0, "CHI"};
  if (n.indexOf("lions") >= 0) return {0x03B6, 0xB5B7, "DET"};
  if (n.indexOf("bills") >= 0) return {0x0191, 0xC066, "BUF"};
  if (n.indexOf("miami") >= 0) return {0xF384, 0x0286, "MIA"};
  if (n.indexOf("texas") >= 0) return {0xBAA0, 0xFFFF, "TEX"};
  if (n.indexOf("rams") >= 0) return {0x01B2, 0xFD00, "LA"};
  if (n.indexOf("jets") >= 0) return {0x12A8, 0xFFFF, "NYJ"};
  if (n.indexOf("ucla") >= 0) return {0x2B58, 0xF540, "UCLA"};
  if (n.indexOf("iowa") >= 0) return {0x0000, 0xFE60, "IOWA"};
  if (n.indexOf("utah") >= 0) return {0xC800, 0x0000, "UTAH"};
  if (n.indexOf("duke") >= 0) return {0x0190, 0xFFFF, "DUKE"};
  if (n.indexOf("utsa") >= 0) return {0xF2C4, 0x0908, "UTSA"};
  if (n.indexOf("army") >= 0) return {0xD5F2, 0x0000, "ARMY"};
  if (n.indexOf("navy") >= 0) return {0x010B, 0xC5D5, "NAVY"};
  if (n.indexOf("usc") >= 0) return {0x9800, 0xFE25, "USC"};
  if (n.indexOf("lsu") >= 0) return {0x40EF, 0xFE84, "LSU"};
  if (n.indexOf("byu") >= 0) return {0x016B, 0xFFFF, "BYU"};
  if (n.indexOf("tcu") >= 0) return {0x48CF, 0xA555, "TCU"};
  if (n.indexOf("smu") >= 0) return {0xC806, 0x3274, "SMU"};
  

  // Preserve the API abbreviation and neutral colors for unlisted teams.
  if (shortName.length() > 0) return {0x2945, 0xCE79, shortName};
  return {0x2945, 0xCE79, "TEAM"};
}

// --------------------------- PNG LOGO DECODER -----------------------

PNG png;
static uint16_t *logoPixels = nullptr;
static int logoOutW = 0;
static int logoOutH = 0;
static int logoSourceW = 0;
static int logoSourceH = 0;

static int pngDrawToBuffer(PNGDRAW *draw) {
  static uint16_t linePixels[320];

  if (!logoPixels || draw->iWidth <= 0 || draw->iWidth > 320 ||
      logoOutW <= 0 || logoOutH <= 0) {
    return 1;
  }

  png.getLineAsRGB565(draw, linePixels, PNG_RGB565_LITTLE_ENDIAN, 0x00000000);

  int yStart = (long)draw->y * logoOutH / logoSourceH;
  int yEnd = (long)(draw->y + 1) * logoOutH / logoSourceH;

  if (yEnd <= yStart) yEnd = yStart + 1;
  if (yStart >= logoOutH) return 1;
  if (yEnd > logoOutH) yEnd = logoOutH;

  for (int x = 0; x < logoOutW; ++x) {
    int sourceX = (long)x * logoSourceW / logoOutW;
    if (sourceX >= draw->iWidth) sourceX = draw->iWidth - 1;

    uint16_t pixel = linePixels[sourceX];

    for (int y = yStart; y < yEnd; ++y) {
      logoPixels[y * logoOutW + x] = pixel;
    }
  }

  return 1;
}

static bool loadPngLogo(const String &url, uint16_t *&outPixels,
                        int outputW, int outputH) {
  outPixels = nullptr;

  if (url.length() < 8 || !url.startsWith("https://")) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(client, url)) return false;

  http.addHeader("x-api-key", API_KEY);

  const char *logoHeaderKeys[] = {"Content-Type"};
  http.collectHeaders(logoHeaderKeys, 1);

  int code = http.GET();

  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  String contentType = http.header("Content-Type");
  String bytes = http.getString();
  http.end();

  if (bytes.length() < 8 || bytes.length() > MAX_LOGO_BYTES) return false;

  contentType.toLowerCase();

  bool looksPng = contentType.indexOf("png") >= 0 ||
                  ((uint8_t)bytes[0] == 0x89 &&
                   bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G');

  if (!looksPng) return false;

  uint8_t *raw = (uint8_t *)ps_malloc(bytes.length());
  if (!raw) return false;

  memcpy(raw, bytes.c_str(), bytes.length());

  int rc = png.openRAM(raw, bytes.length(), pngDrawToBuffer);

  if (rc != PNG_SUCCESS) {
    free(raw);
    return false;
  }

  logoSourceW = png.getWidth();
  logoSourceH = png.getHeight();

  if (logoSourceW <= 0 || logoSourceH <= 0 ||
      logoSourceW > 320 || logoSourceH > 320) {
    png.close();
    free(raw);
    return false;
  }

  logoOutW = outputW;
  logoOutH = outputH;

  logoPixels = (uint16_t *)ps_malloc(outputW * outputH * sizeof(uint16_t));

  if (!logoPixels) {
    png.close();
    free(raw);
    return false;
  }

  for (int i = 0; i < outputW * outputH; ++i) {
    logoPixels[i] = C_BLACK;
  }

  rc = png.decode(nullptr, PNG_FAST_PALETTE);

  png.close();
  free(raw);

  if (rc != PNG_SUCCESS) {
    free(logoPixels);
    logoPixels = nullptr;
    return false;
  }

  outPixels = logoPixels;
  logoPixels = nullptr;

  return true;
}

static uint16_t *homeLogoPixels = nullptr;
static uint16_t *awayLogoPixels = nullptr;
static String logoCacheMatchId;

static void clearLogoCache() {
  if (homeLogoPixels) free(homeLogoPixels);
  if (awayLogoPixels) free(awayLogoPixels);

  homeLogoPixels = nullptr;
  awayLogoPixels = nullptr;
  logoCacheMatchId = "";
}

static void loadSelectedLogos() {
  if (!hasSelectedGame || !selectedGame.valid) return;
  if (logoCacheMatchId == selectedGame.id) return;

  clearLogoCache();
  logoCacheMatchId = selectedGame.id;

  uint16_t *loaded = nullptr;

  if (loadPngLogo(selectedGame.homeLogo, loaded, 88, 88)) {
    homeLogoPixels = loaded;
  }

  loaded = nullptr;

  if (loadPngLogo(selectedGame.awayLogo, loaded, 88, 88)) {
    awayLogoPixels = loaded;
  }
}

// --------------------------- UI HELPERS ------------------------------

static void centeredText(const String &value, int x, int y, int textSize,
                         uint16_t color, uint16_t background = C_BLACK) {
  gfx.setFont(nullptr);
  gfx.setTextSize(textSize);
  gfx.setTextColor(color, background);

  int16_t x1, y1;
  uint16_t w, h;

  gfx.getTextBounds(value, 0, 0, &x1, &y1, &w, &h);
  gfx.setCursor(x - (int)w / 2, y - (int)h / 2);
  gfx.print(value);
}

static void leftText(const String &value, int x, int y,
                     int textSize, uint16_t color) {
  gfx.setFont(nullptr);
  gfx.setTextSize(textSize);
  gfx.setTextColor(color, C_BLACK);
  gfx.setCursor(x, y);
  gfx.print(value);
}

static void drawFallbackBadge(const String &teamName, const String &shortName,
                              int x, int y, uint16_t primary, uint16_t secondary) {
  TeamStyle style = styleForTeam(teamName, shortName);

  gfx.fillRoundRect(x, y, 92, 92, 15, C_BLACK);
  gfx.drawRoundRect(x, y, 92, 92, 15, secondary);
  gfx.fillRoundRect(x + 7, y + 7, 78, 78, 11, primary);

  centeredText(style.abbreviation, x + 46, y + 43,
               style.abbreviation.length() >= 4 ? 2 : 3,
               C_WHITE, primary);

  gfx.drawFastHLine(x + 20, y + 69, 52, secondary);
}

static void drawLogo(uint16_t *pixels, const String &teamName,
                     const String &shortName, int x, int y,
                     uint16_t primary, uint16_t secondary) {
  if (pixels) {
    gfx.fillRoundRect(x, y, 92, 92, 15, C_BLACK);
    gfx.drawRoundRect(x, y, 92, 92, 15, secondary);
    gfx.drawRGBBitmap(x + 2, y + 2, pixels, 88, 88);
  } else {
    drawFallbackBadge(teamName, shortName, x, y, primary, secondary);
  }
}

static String statusLabel(const Game &game) {
  if (isLiveStatus(game.status)) return "LIVE";
  if (isFinishedStatus(game.status)) return "FINAL";

  String s = lowerCopy(game.status);

  if (s == "postponed") return "POSTPONED";
  if (s == "suspended") return "SUSPENDED";

  return "UP NEXT";
}

static void drawWaitingScreen(const String &message) {
  const uint16_t NAVY = 0x0861;
  const uint16_t CARD = 0x10A3;
  const uint16_t EDGE = 0x2945;
  const uint16_t RED = 0xF145;

  gfx.fillScreen(NAVY);

  // Broadcast header.
  gfx.fillRect(0, 0, UI_W, 48, C_BLACK);
  gfx.fillRect(0, 46, UI_W, 3, RED);
  gfx.fillRect(0, 0, 8, 48, RED);
  gfx.fillRoundRect(22, 9, 42, 29, 5, RED);
  centeredText("G", 43, 15, 3, C_WHITE, RED);
  leftText("GRIDIRON", 76, 10, 3, C_WHITE);
  leftText("GAMECAST  /  FOOTBALL", 250, 19, 1, C_MUTED);
  gfx.fillRoundRect(704, 10, 92, 27, 7, CARD);
  centeredText("STANDBY", 750, 17, 1, C_MUTED, CARD);

  // Center status card.
  gfx.fillRoundRect(22, 67, 776, 207, 16, CARD);
  gfx.drawRoundRect(22, 67, 776, 207, 16, EDGE);
  gfx.fillRect(22, 67, 7, 207, RED);
  centeredText("LIVE FOOTBALL", 410, 111, 3, C_WHITE, CARD);
  gfx.fillRoundRect(285, 145, 270, 4, 2, RED);
  centeredText(message, 410, 187, 2, C_MUTED, CARD);
  centeredText(WiFi.status() == WL_CONNECTED ? "NETWORK CONNECTED" : "CONNECTING TO NETWORK",
               410, 230, 1, C_MUTED, CARD);

  gfx.fillRect(22, 292, 776, 1, EDGE);
  leftText("GRIDIRON GAMECAST", 24, 301, 1, C_MUTED);
  leftText("ESP32  /  WAVESHARE", 650, 301, 1, C_MUTED);
}

static void drawScoreboard() {
  if (!hasSelectedGame || !selectedGame.valid) {
    drawWaitingScreen(apiOnline
                        ? "NO PREFERRED GAME FOUND"
                        : (lastApiError.length()
                             ? lastApiError
                             : "WAITING FOR SPORTS DATA"));
    return;
  }

  // Broadcast palette: deep navy canvas, graphite cards, bright score white.
  const uint16_t NAVY = 0x0861;
  const uint16_t CARD = 0x10A3;
  const uint16_t CARD2 = 0x18E4;
  const uint16_t EDGE = 0x2945;
  const uint16_t RED = 0xF145;
  const uint16_t GREEN = 0x36A6;
  const uint16_t GOLD = 0xFEA0;

  TeamStyle homeStyle = styleForTeam(selectedGame.homeName, selectedGame.homeShort);
  TeamStyle awayStyle = styleForTeam(selectedGame.awayName, selectedGame.awayShort);
  bool live = isLiveStatus(selectedGame.status);
  bool finished = isFinishedStatus(selectedGame.status);
  String label = statusLabel(selectedGame);

  gfx.fillScreen(NAVY);

  // Fine broadcast grid and header rail.
  for (int x = 0; x < UI_W; x += 40) gfx.drawFastVLine(x, 49, 226, 0x10A3);
  gfx.fillRect(0, 0, UI_W, 47, C_BLACK);
  gfx.fillRect(0, 45, UI_W, 3, RED);
  gfx.fillRect(0, 0, 8, 47, RED);

  gfx.fillRoundRect(20, 8, 42, 30, 5, RED);
  centeredText("G", 41, 14, 3, C_WHITE, RED);
  leftText("GRIDIRON", 74, 9, 3, C_WHITE);
  leftText("GAMECAST", 226, 18, 1, C_MUTED);

  String leagueLabel = selectedGame.leagueCode == "nfl" ? "NFL  /  PRO" : "NCAA  /  COLLEGE";
  centeredText(leagueLabel, 505, 18, 1, C_MUTED);

  uint16_t statusColor = live ? RED : (finished ? 0x39C7 : GOLD);
  gfx.fillRoundRect(704, 8, 94, 30, 7, statusColor);
  if (live) gfx.fillCircle(718, 23, 4, C_WHITE);
  centeredText(label, live ? 758 : 751, 15, 1, C_WHITE, statusColor);

  // Main matchup card.
  gfx.fillRoundRect(18, 59, 784, 218, 15, CARD);
  gfx.drawRoundRect(18, 59, 784, 218, 15, EDGE);
  gfx.fillRect(20, 61, 3, 214, RED);

  // Team panels.
  gfx.fillRoundRect(30, 72, 330, 190, 12, CARD2);
  gfx.fillRoundRect(460, 72, 330, 190, 12, CARD2);
  gfx.drawRoundRect(30, 72, 330, 190, 12, homeStyle.primary);
  gfx.drawRoundRect(460, 72, 330, 190, 12, awayStyle.primary);
  gfx.fillRect(30, 72, 330, 5, homeStyle.primary);
  gfx.fillRect(460, 72, 330, 5, awayStyle.primary);

  // Logos and team identity.
  drawLogo(homeLogoPixels, selectedGame.homeName, selectedGame.homeShort,
           47, 119, homeStyle.primary, homeStyle.secondary);
  drawLogo(awayLogoPixels, selectedGame.awayName, selectedGame.awayShort,
           681, 119, awayStyle.primary, awayStyle.secondary);

  centeredText(homeStyle.abbreviation, 212, 91, 2, C_WHITE, CARD2);
  centeredText(awayStyle.abbreviation, 638, 91, 2, C_WHITE, CARD2);
  centeredText(selectedGame.homeName, 212, 111, 1, C_MUTED, CARD2);
  centeredText(selectedGame.awayName, 638, 111, 1, C_MUTED, CARD2);

  // Big broadcast score numerals.
  String homeScore = selectedGame.homeScore >= 0 ? String(selectedGame.homeScore) : "-";
  String awayScore = selectedGame.awayScore >= 0 ? String(selectedGame.awayScore) : "-";
  centeredText(homeScore, 257, 180, 7, C_WHITE, CARD2);
  centeredText(awayScore, 563, 180, 7, C_WHITE, CARD2);

  // Center matchup / game-state column.
  gfx.fillRoundRect(373, 76, 74, 180, 10, C_BLACK);
  gfx.drawRoundRect(373, 76, 74, 180, 10, EDGE);
  centeredText("VS", 410, 104, 2, C_MUTED, C_BLACK);

  if (live && selectedGame.period >= 1 && selectedGame.period <= 4) {
    centeredText("Q" + String(selectedGame.period), 410, 153, 3, C_WHITE, C_BLACK);
  } else if (live && selectedGame.period > 4) {
    centeredText("OT", 410, 153, 3, C_WHITE, C_BLACK);
  } else if (finished) {
    centeredText("FINAL", 410, 153, 1, C_WHITE, C_BLACK);
  } else {
    centeredText("KICK", 410, 146, 1, C_MUTED, C_BLACK);
    centeredText("OFF", 410, 162, 1, C_MUTED, C_BLACK);
  }

  gfx.fillRoundRect(386, 191, 48, 4, 2, live ? RED : (finished ? GREEN : GOLD));
  centeredText(live ? "ON AIR" : (finished ? "FINAL" : "UP NEXT"),
               410, 220, 1, C_WHITE, C_BLACK);

  // Lower information ribbon.
  gfx.fillRoundRect(18, 285, 784, 25, 7, C_BLACK);
  gfx.drawRoundRect(18, 285, 784, 25, 7, EDGE);
  String info = live ? "LIVE GAME  /  SCORE UPDATES AUTOMATICALLY"
                     : (finished ? "GAME COMPLETE" : kickoffLabel(selectedGame.kickoffUtc));
  leftText(info, 31, 293, 1, C_WHITE);

  String connection = apiOnline ? "DATA CONNECTED" : "DATA OFFLINE";
  uint16_t connectionColor = apiOnline ? GREEN : RED;
  gfx.fillCircle(674, 297, 4, connectionColor);
  leftText(connection, 685, 293, 1, C_MUTED);
  gfx.fillRect(18, 316, 784, 2, RED);
}

// --------------------------- NETWORK / REFRESH -----------------------

static void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to Wi-Fi");

  uint32_t start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < 18000UL) {
    delay(350);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWi-Fi connected: %s\n",
                  WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWi-Fi connection did not complete yet.");
  }
}

static void setupClock() {
  configTzTime(PACIFIC_TZ, "pool.ntp.org", "time.nist.gov");

  uint32_t start = millis();

  while (!timeIsValid() && millis() - start < 8000UL) {
    delay(250);
  }

  if (timeIsValid()) {
    time_t now = time(nullptr);
    struct tm localTime = {};

    localtime_r(&now, &localTime);

    char buffer[40];
    strftime(buffer, sizeof(buffer), "%a %b %d %I:%M %p", &localTime);

    Serial.printf("Pacific time set: %s\n", buffer);
  } else {
    Serial.println("NTP time is not ready; live selection still works, schedule ordering may be delayed.");
  }
}

static void refreshScoreboard() {
  if (WiFi.status() != WL_CONNECTED) {
    apiOnline = false;
    hasSelectedGame = false;
    drawScoreboard();
    return;
  }

  bool nflOk = fetchLeagueMatches("nfl", gamesNFL);
  bool cfbOk = fetchLeagueMatches("ncaaf", gamesCFB);

  apiOnline = nflOk || cfbOk;

  Game nextGame;
  bool found = chooseGame(nextGame);

  if (found) {
    bool changed = !hasSelectedGame || selectedGame.id != nextGame.id;

    selectedGame = nextGame;
    hasSelectedGame = true;

    if (changed) {
      loadSelectedLogos();
    }

    lastApiError = "";
    lastUpdateText = "UPDATED";

    Serial.printf("Selected %s vs %s (%s)\n",
                  selectedGame.homeName.c_str(),
                  selectedGame.awayName.c_str(),
                  selectedGame.status.c_str());
  } else {
    hasSelectedGame = false;
    clearLogoCache();

    lastApiError = (nflOk || cfbOk)
                     ? "No preferred upcoming game found"
                     : lastApiError;
  }

  drawScoreboard();
}

// --------------------------- ARDUINO SETUP ---------------------------

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\nGRIDIRON LIVE starting...");

  initDisplay();
  drawWaitingScreen("STARTING UP");

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    setupClock();
    refreshScoreboard();
  } else {
    drawWaitingScreen("CHECK WI-FI SETTINGS");
  }

  lastRefreshMs = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiAttemptMs >= WIFI_RETRY_MS) {
      lastWifiAttemptMs = millis();

      connectWiFi();

      if (WiFi.status() == WL_CONNECTED) {
        setupClock();
        refreshScoreboard();
        lastRefreshMs = millis();
      }
    }
  }

  if (millis() - lastRefreshMs >= REFRESH_MS) {
    lastRefreshMs = millis();

    if (WiFi.status() == WL_CONNECTED) {
      refreshScoreboard();
    }
  }

  delay(20);
}