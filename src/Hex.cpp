
// 
//  Display clock, and commute time witj delay betweem each in the settings.  With clock using top and botton digits and commute in the middle with a north or south arrow
//  Google API for commute time (based on traffic) is expensive,, so there is an array to state how many automatic API pulls per hour, and if to skip the weekend days
//  API can be pulled manually with touch sensor.  Either way, if the API has not been hit for a period of time (30mins by default) it will stop showing
//  Zambretti formula used to gather air pressure data and provide a local weather forecast.  Background of clock/commute will represent the forecast and 2nd touch sensor will speak the time & forecast
//  User things to change denoted by a "User option" remark
//  https://developers.google.com/maps/documentation/directions/overview
//
//  Attributions to:
//  https://hackaday.io/project/173732/instructions
//  https://drive.google.com/drive/u/1/folders/1vSOFGcGl1IOswgpjikX-N6DDl-dHUQP0
//  https://gist.github.com/Jerware/b82ad4768f9935c8acfccc98c9211111
//  https://github.com/3KUdelta/Solar_WiFi_Weather_Station
//  http://integritext.net/DrKFS/zambretti.htm
//
//
//  **Put into platformio.ini
//  build_flags =
//       -DSSID_NAME="SSID"
//       -DPASSWORD_NAME="password"
//       -DAPI_KEY="API key"


#include <ArduinoJson.h>
#include <FastLED.h>
#include <HTTPClient.h>
#include "DFRobotDFPlayerMini.h"
#include <HardwareSerial.h>
#include "BMP280.h"
#include "Wire.h"
#include "SPIFFS.h"
#include <NTPClient.h>
#include "ESP8266FtpServer.h"
#include <EEPROM.h>

#define PATTERN_TIME      10          // Seconds to show each pattern on autoChange [10]
#define kMatrixWidth      8          // Matrix width [8]
#define kMatrixHeight     15         // Matrix height [15]
// #define kMatrixWidth      10          // Matrix width [8]
// #define kMatrixHeight     10         // Matrix height [15]
#define MAX_DIMENSION ((kMatrixWidth>kMatrixHeight) ? kMatrixWidth : kMatrixHeight)   // Largest dimension of matrix
#define str(s) #s
#define xstr(s) str(s)

//User option - Print verbose output
int verbose_output = 0; // 0 = No serial print, 1 = serial print

// Used to check RAM availability. Usage: Serial.println(freeRam());
int freeRam () {
  extern int __heap_start, *__brkval;
  int v;
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval);
}


// ---- LEDS  ----  //
//User option
#define LED_PIN     5
#define NUM_LEDS    (kMatrixWidth * kMatrixHeight)                                       // Total number of Leds
#define BRIGHTNESS    100
#define LED_TYPE    WS2811
#define COLOR_ORDER RGB
CRGB leds[NUM_LEDS];

//Digit color values in RGB
int r=255;
int g=128;
int b=0;

//Clock background color values in RGB
int br=0;
int bg=20;
int bb=10;

//Commute background color values in RGB
int cr=20;
int cg=0;
int cb=10;

//Commute digits color values in RGB
int crd=255;
int cgd=255;
int cbd=0;


int commute_flag = 0;   //used to switch LED display from clock to commute time
int commute_flag_on = 0;  //Use to only show once and not repeatidely re-display
// ---- LEDS  ----  //


// Button stuff
bool autoChangeVisuals = false;

// ---- SOUND  ----  //
HardwareSerial MySerial(1);
DFRobotDFPlayerMini myDFPlayer;
int mp3vol = 20;                //Volume for DF card player.  Keep at 0, used as a flag to skip functions if not wifimanager credentials (no sound option)
int mp3_selected = 1;           //Default mp3 to play ("mp3/0001.mp3" on SDcard)
TaskHandle_t speakforecast;
TaskHandle_t speakchime;
// ---- SOUND  ----  //


// ---- Network  ----  //
#ifndef SSID_NAME
#define SSID_NAME "WIFI_SSID" //Default SSID if not build flag from PlatformIO doesn't work
#endif

#ifndef PASSWORD_NAME
#define PASSWORD_NAME "WIFI_PASSWORD" //Default WiFi Password if not build flag from PlatformIO doesn't work
#endif

#ifndef API_KEY
#define API_KEY "API_KEY" //API for Google
#endif

//Gets SSID/PASSWORD from PlatformIO.ini build flags
const char ssid[] = xstr(SSID_NAME);      //  your network SSID (name)
const char pass[] = xstr(PASSWORD_NAME);  // your network password
const char google_API_key[] = xstr(API_KEY);  // your network password

FtpServer ftpSrv;
// ---- Network  ----  //


// ---- Google API pieces  ----  //
//String google_API = "https://maps.googleapis.com/maps/api/distancematrix/json?origins=34+Sylvan+way+Silverstream+Upper+Hutt&destinations=1+Willis+st+Wellington&mode=driving&language=en&key=AIzaSyDulwyvIGXCiXG3R4HhDk0Qm31DKT61HCE";
//User option
String google_API_start = "https://maps.googleapis.com/maps/api/distancematrix/json?origins=";
String google_API_home = "34+Sylvan+way+Silverstream+Upper+Hutt";
String google_API_mode = "driving";
String google_API_traffic = "departure_time=now&duration_in_traffic";
String google_API_lang = "en";
String google_API_work = "1+Willis+st+Wellington";

//Array 0-23 (midnight - 11pm) with the number of API pulls from Google each hour.  1st digit is the hour (e.g 9 = 9am - 9.59am) 
//Keep to something 60 divisible by e.g 1, 2, 5, 10, 15, 20, 30.   **Be aware for example 4 means 15mins intervals.  Start of hour, 15mins, 30min, 45mins.  It will not trigger at the top of the following hour (add a 1 if needed)
//User option
int API_pull_array[24][2] = {{0,0},{1,0},{2,0},{3,0},{4,0},{5,0},{6,2},{7,4},{8,4},{9,3},{10,1},{11,0},{12,0},{13,0},{14,4},{15,4},{16,4},{17,4},{18,1},{19,0},{20,0},{21,0},{22,0},{23,0}};
int show_weekend = 0;   //0 = Don't automatically get google API during weekends
unsigned long last_API_limit = 30 * 60 * 1000;      //Max time to display commute time since API pulled, if exceeded won't display unless touch sensor triggered

//Go to https://www.googleapis.com/, click padlock icon, certificate, details tab, version V3, copy to file, Base 64 X509.  Add terminating charactors etc
//User option
const char* root_ca = \
  "-----BEGIN CERTIFICATE-----\n" \
  "MIIDujCCAqKgAwIBAgILBAAAAAABD4Ym5g0wDQYJKoZIhvcNAQEFBQAwTDEgMB4G\n" \
  "A1UECxMXR2xvYmFsU2lnbiBSb290IENBIC0gUjIxEzARBgNVBAoTCkdsb2JhbFNp\n" \
  "Z24xEzARBgNVBAMTCkdsb2JhbFNpZ24wHhcNMDYxMjE1MDgwMDAwWhcNMjExMjE1\n" \
  "MDgwMDAwWjBMMSAwHgYDVQQLExdHbG9iYWxTaWduIFJvb3QgQ0EgLSBSMjETMBEG\n" \
  "A1UEChMKR2xvYmFsU2lnbjETMBEGA1UEAxMKR2xvYmFsU2lnbjCCASIwDQYJKoZI\n" \
  "hvcNAQEBBQADggEPADCCAQoCggEBAKbPJA6+Lm8omUVCxKs+IVSbC9N/hHD6ErPL\n" \
  "v4dfxn+G07IwXNb9rfF73OX4YJYJkhD10FPe+3t+c4isUoh7SqbKSaZeqKeMWhG8\n" \
  "eoLrvozps6yWJQeXSpkqBy+0Hne/ig+1AnwblrjFuTosvNYSuetZfeLQBoZfXklq\n" \
  "tTleiDTsvHgMCJiEbKjNS7SgfQx5TfC4LcshytVsW33hoCmEofnTlEnLJGKRILzd\n" \
  "C9XZzPnqJworc5HGnRusyMvo4KD0L5CLTfuwNhv2GXqF4G3yYROIXJ/gkwpRl4pa\n" \
  "zq+r1feqCapgvdzZX99yqWATXgAByUr6P6TqBwMhAo6CygPCm48CAwEAAaOBnDCB\n" \
  "mTAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUm+IH\n" \
  "V2ccHsBqBt5ZtJot39wZhi4wNgYDVR0fBC8wLTAroCmgJ4YlaHR0cDovL2NybC5n\n" \
  "bG9iYWxzaWduLm5ldC9yb290LXIyLmNybDAfBgNVHSMEGDAWgBSb4gdXZxwewGoG\n" \
  "3lm0mi3f3BmGLjANBgkqhkiG9w0BAQUFAAOCAQEAmYFThxxol4aR7OBKuEQLq4Gs\n" \
  "J0/WwbgcQ3izDJr86iw8bmEbTUsp9Z8FHSbBuOmDAGJFtqkIk7mpM0sYmsL4h4hO\n" \
  "291xNBrBVNpGP+DTKqttVCL1OmLNIG+6KYnX3ZHu01yiPqFbQfXf5WRDLenVOavS\n" \
  "ot+3i9DAgBkcRcAtjOj4LaR0VknFBbVPFd5uRHg5h6h+u/N5GJG79G+dwfCMNYxd\n" \
  "AfvDbbnvRG15RjF+Cv6pgsH/76tuIMRQyV+dTZsXjAzlAcmgQWpzU/qlULRuJQ/7\n" \
  "TBj0/VLZjmmx6BEP3ojY+x1J96relc8geMJgEtslQIxq/H5COEBkEveegeGTLg==\n" \
  "-----END CERTIFICATE-----\n";

String API_request(String);
String API_request_string(String,String);
const uint16_t JSON_extract(String);

int north_south_flag = 0;     //used with touch sensor to pull a north or south
int API_trigger = 0;
int API_minute = 0;
unsigned long last_API_pull;                        //When the last google API pulled, in millis
// ---- Google API pieces  ----  //


// ---- Timing & time management ----  //
//User option
//hour with mp3 to play 2nd digit: 100=weather, 101 = time, 102 = time & forefast, 001 = morepork, 002 = cuckoo, 003 = Spring, 004 - submarine hooter
int hour_chime_array[24][2] = {{0,0},{1,0},{2,0},{3,0},{4,0},{5,0},{6,0},{7,2},{8,102},{9,102},{10,102},{11,102},{12,2},{13,102},{14,102},{15,102},{16,102},{17,102},{18,102},{19,102},{20,1},{21,102},{22,0},{23,0}};  
int hour_chime_flag = 0;
int secs_todisplay_commute = 10 * 1000;     //How long commute time is shown for
int display_commute_every = 1 * 30 * 1000;     //How often to diplay commute time
int gmt_offset = 12 * 60 * 60;    //Don't use negative GMT or day of week is wrong (goes backward in time)
int DST = 1;    //flips between -1,0,1 by touch
uint32_t delayamount = 30 * 1000;                     //Calculate zambretti forecast
uint32_t zambretti_delayamount = 30 * 60 * 1000; //Update pressure data to SPIFFs every 30mins
uint32_t showtime_delayamount = 60 * 1000;       //Display local every 60s
uint32_t pressure_read_interval = 5 * 60 * 1000; //5mins x 60 sec x 1000 millis

unsigned long secs_todisplay_commute_millis;
unsigned long display_commute_every_millis;
unsigned long millis_unix_timestamp;
int mode = 0;   //0=Clock, 1=Weather
long previousTime = 0;
int second;
int hour;
int old_hour;
int minute;
int old_minute;
int hour24;
int day;    //(0 to 6) starting on Sunday (0)
String AMPM;
int force_time = 0;   //forces the time to be displayed, needed with the checks for new_min/new_hour if you want to show after a comnmute mode

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "0.nz.pool.ntp.org", gmt_offset, 15 * 60 * 1000);    //update every 15mins
// ---- Timing & time management ----  //


// ---- Middle LED Arrays  ----  //
int numeral1, numeral2, numeral3;

int digit1_array[10][11] = {{8, 29, 30, 43, 44, 59, 60, 61, 62},{5, 30, 43, 46, 59, 62},{8, 29, 30, 43, 45, 46, 60, 61, 62},{8, 29, 30, 43, 45, 46, 59, 61, 62},{7, 44, 45, 43, 46, 59, 62, 75},{8, 29, 30, 44, 45, 46, 59, 61, 62},{9, 29, 30, 44, 45, 46, 59, 60, 61, 62},{6, 29, 30, 43, 46, 59, 62},{10, 29, 30, 43, 44, 45, 46, 59, 60, 61, 62},{8, 29, 30, 43, 44, 45, 46, 59, 62}};
int digit2_array[10][11] = {{8, 25, 24, 32, 33, 48, 49, 57, 56},{5, 24, 33, 40, 49, 56},{8, 25, 24, 33, 40, 41, 48, 57, 56},{8, 25, 24, 33, 40, 41, 49, 57, 56},{7, 32, 40, 33, 41, 49, 56, 65},{8, 25, 24, 32, 40, 41, 49, 57, 56},{9, 25, 24, 32, 40, 41, 48, 49, 57, 56},{6, 24, 25, 33, 40, 49, 56},{10, 25, 24, 32, 33, 40, 41, 48, 49, 57, 56},{8, 24, 25, 32, 33, 40, 41, 49, 56}};
int digit3_array[10][11] = {{8, 35, 36, 37, 38, 53, 54, 67, 68},{5, 36, 37, 52, 53, 68},{8, 35, 36, 37, 51, 52, 54, 67, 68},{8, 35, 36, 37, 51, 52, 53, 67, 68},{7, 38, 51, 37, 52, 53, 68, 69},{8, 35, 36, 38, 51, 52, 53, 67, 68},{9, 35, 36, 38, 51, 52, 53, 54, 67, 68},{6, 35, 36, 37, 52, 53, 68},{10, 35, 36, 37, 38, 51, 52, 53, 54, 67, 68},{8, 35, 36, 37, 38, 51, 52, 53, 68}};
// ---- Middle LED Arrays  ----  //


// ---- Zambretti  ----  //
//User option
#define P0 1013.25
#define ELEVATION (100)             //Enter your elevation in m ASL to calculate rel pressure (ASL/QNH) at your place
String HEMISPHERE = "SOUTH";      //Set to NORTH or SOUTH

// FORECAST CALCULATION
unsigned long current_timestamp; // Actual timestamp read from NTPtime_t now;
unsigned long saved_timestamp;   // Timestamp stored in SPIFFS
unsigned long millis_unix_timestamp_baseline;
float pressure_value[12];      // Array for the historical pressure values (6 hours, all 30 mins)
float pressure_difference[12]; // Array to calculate trend with pressure differences

// FORECAST RESULT
volatile int accuracy;           // Counter, if enough values for accurate forecasting
String ZambrettisWords; // Final statement about weather forecast
String old_ZambrettisWords; // Final statement about weather forecast
String trend_in_words;  // Trend in words
char ZambrettiLetter();
String ZambrettiSays(char code);
int CalculateTrend();
int16_t readTempData();
volatile int Zambretti_mp3 = 126;    //no forecast by default
volatile int Zambretti_trend_mp3 = 0;
int Zambretti_LED = 0; //1=Stormy (X>Z), 2=Rain (T>W), 3=Unsettled (P>S), 4=Showery (I>O), 5=Fine (A>H)

//Pressure
int pressure;
int minpressure = 100000;
int maxpressure = 0;
int Zambrettiarraysize = 36;
int Zambretti_array[36];  //Store readings for 3hr period
long Zambretti_count = 1; //cycle from 1 to 36 for each 5min reading
long pressure_read_millis;
int write_timestamp;
int accuracy_in_percent;
int accuracygate = 12; //Must reach this level (12 x 30min) to give a forecast
float measured_temp;
float measured_humi;
float measured_pres;
float SLpressure_hPa; // needed for rel pressure calculation
float HeatIndex;      // Heat Index in °C
float volt;
int rel_pressure_rounded;
double DewpointTemperature;
float DewPointSpread; // Difference between actual temperature and dewpoint

uint32_t delt_t = 0;                                                            // used to control display output rate
uint32_t count = 0, sumCount = 0, timecount = 0, working_modecount = 0, zambretticount = 0; // used to control display output rate
float deltat = 0.0f, sum = 0.0f;          // integration interval for both filter schemes
uint32_t lastUpdate = 0, firstUpdate = 0; // used to calculate integration interval
uint32_t Now = 0;
BMP280 bmp;

const char TEXT_RISING_FAST[] = "Rising fast";
const char TEXT_RISING[] = "Rising";
const char TEXT_RISING_SLOW[] = "Rising slow";
const char TEXT_STEADY[] = "Steady";
const char TEXT_FALLING_SLOW[] = "Falling slow";
const char TEXT_FALLING[] = "Falling";
const char TEXT_FALLING_FAST[] = "Falling fast";
const char TEXT_ZAMBRETTI_A[] = "Settled Fine Weather";
const char TEXT_ZAMBRETTI_B[] = "Fine Weather";
const char TEXT_ZAMBRETTI_C[] = "Becoming Fine";
const char TEXT_ZAMBRETTI_D[] = "Fine, Becoming Less Settled";
const char TEXT_ZAMBRETTI_E[] = "Fine, Possibly showers";
const char TEXT_ZAMBRETTI_F[] = "Fairly Fine, Improving";
const char TEXT_ZAMBRETTI_G[] = "Fairly Fine, Possibly showers early";
const char TEXT_ZAMBRETTI_H[] = "Fairly Fine, Showers Later";
const char TEXT_ZAMBRETTI_I[] = "Showery Early, Improving";
const char TEXT_ZAMBRETTI_J[] = "Changeable Improving";
const char TEXT_ZAMBRETTI_K[] = "Fairly Fine, Showers likely";
const char TEXT_ZAMBRETTI_L[] = "Rather Unsettled Clearing Later";
const char TEXT_ZAMBRETTI_M[] = "Unsettled, Probably Improving";
const char TEXT_ZAMBRETTI_N[] = "Showery Bright Intervals";
const char TEXT_ZAMBRETTI_O[] = "Showery Becoming Unsettled";
const char TEXT_ZAMBRETTI_P[] = "Changeable some rain";
const char TEXT_ZAMBRETTI_Q[] = "Unsettled, short fine Intervals";
const char TEXT_ZAMBRETTI_R[] = "Unsettled, Rain later";
const char TEXT_ZAMBRETTI_S[] = "Unsettled, rain at times";
const char TEXT_ZAMBRETTI_T[] = "Very Unsettled, Finer at times";
const char TEXT_ZAMBRETTI_U[] = "Rain at times, Worse later";
const char TEXT_ZAMBRETTI_V[] = "Rain at times, becoming very unsettled";
const char TEXT_ZAMBRETTI_W[] = "Rain at Frequent Intervals";
const char TEXT_ZAMBRETTI_X[] = "Very Unsettled, Rain";
const char TEXT_ZAMBRETTI_Y[] = "Stormy, possibly improving";
const char TEXT_ZAMBRETTI_Z[] = "Stormy, much rain";
const char TEXT_ZAMBRETTI_DEFAULT[] = "Sorry, no forecast for the moment";
// ---- Zambretti  ----  //



// ---- Weather function vars  ----  //
String weather_mode = "rain"; //rain or sun
int weather_RGB_R = 255;    //colour of the background
int weather_RGB_G = 255;
int weather_RGB_B = 255;
int rain_RGB_R = 255;       //colour of the rain
int rain_RGB_G = 255;
int rain_RGB_B = 255;

int weather_time = 75;   //speed of function (raining a little or lots)
int weather_density = 10;  //255 = solid background
int weather_background_flag = 1;  //flag so only fills in background once


//Touch sensor
int touchPin1 = 12;     //google API update
int touchPin2 = 13;      //speak forecast
int touch_threshold = 20;
int short_threshold_press = 250;   //ms for short press
int long_threshold_press = 2000;   //ms for long press
int touch_timeout = 5000;
long touch_flag = millis();
int touch_bounce = 100;

// Helper to map XY coordinates to irregular matrix
#define LAST_VISIBLE_LED 100
uint16_t XY (uint16_t x, uint16_t y) {
  // any out of bounds address maps to the first hidden pixel
  if ( (x >= kMatrixWidth) || (y >= kMatrixHeight) ) {
    return (LAST_VISIBLE_LED + 1);
  }

  const uint16_t XYTable[] = {
     0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,
    29,  28,  27,  26,  25,  24,  23,  22,  21,  20,  19,  18,  17,  16,  15,
    30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,
    59,  58,  57,  56,  55,  54,  53,  52,  51,  50,  49,  48,  47,  46,  45,
    60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,
    89,  88,  87,  86,  85,  84,  83,  82,  81,  80,  79,  78,  77,  76,  75,
    90,  91,  92,  93,  94,  95,  96,  97,  98,  99, 100, 101, 102, 103, 104,
   119, 118, 117, 116, 115, 114, 113, 112, 111, 110, 109, 108, 107, 106, 105,
   120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134,
   149, 148, 147, 146, 145, 144, 143, 142, 141, 140, 139, 138, 137, 136, 135,
   150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164,
   179, 178, 177, 176, 175, 174, 173, 172, 171, 170, 169, 168, 167, 166, 165,
   180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194,
   209, 208, 207, 206, 205, 204, 203, 202, 201, 200, 199, 198, 197, 196, 195,
   210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224,
   239, 238, 237, 236, 235, 234, 233, 232, 231, 230, 229, 228, 227, 226, 225,
   240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254,
   269, 268, 267, 266, 265, 264, 263, 262, 261, 260, 259, 258, 257, 256, 255,
   270, 271, 272, 273, 274, 275, 276, 277, 278, 279, 280, 281, 282, 283, 284,
   299, 298, 297, 296, 295, 294, 293, 292, 291, 290, 289, 288, 287, 286, 285
  };

  uint16_t i = (y * kMatrixWidth) + x;
  uint16_t j = XYTable[i];
  return j;
}

//Sun
DEFINE_GRADIENT_PALETTE( sunPal_gp ) {
  50,   50,    50,    0,      //black
  220,  255,  255,  0,      //dark red
  255,  255,  255,  0,      //red
  220,  255,  255,  0,      //yellow
  255,  255,  255,  0 };  //white

uint8_t sunPixels[NUM_LEDS];
CRGBPalette16 _currentPalette = sunPal_gp;
// ---- Weather function vars  ----  //


//Declare functions
void google_API();
void google_API_hour_check_fetch();
String API_request(String);
String API_request_string(String,String);
const uint16_t JSON_extract(String);

void connectWifi();
void updateLocalTime();
void rain_run();
void sun_run();
void do_sun();
void spread_sun(uint16_t src);
void sun_setBottomRow(uint16_t col);
bool sun_runPattern();
void touch_check();
void reset_clockdigits();
void background_clock();
void background_commute();
void doclock();
void chime_check();
void Commute_digits_LED();
void doLEDs();
void Test_LEDs();
void runMatrix();
void measurementEvent();
void MinMax_pressure_handle();
void commute_flip_check();
int CalculateTrend();
void Zambretti_process();
char ZambrettiLetter();
void Zambretti_calc();
String ZambrettiSays(char code);

void SPIFFS_init();
void ReadFromSPIFFS();
void WriteToSPIFFS(int write_timestamp);
void UpdateSPIFFS();
void FirstTimeRun();
void speak_forecast(void *parameters);
void speak_chime(void *parameters);
void SpeakClock();
void chime_check();
void show_touch();

void hour12();
void hour1();
void hour2();
void hour3();
void hour4();
void hour5();
void hour6();
void hour7();
void hour8();
void hour9();
void hour10();
void hour11();
void min0();
void min1();
void min2();
void min3();
void min4();
void min5();
void min6();
void min7();
void min8();
void min9();
void min10();
void min11();
void min12();
void min13();
void min14();
void min15();
void min16();
void min17();
void min18();
void min19();
void min20();
void min21();
void min21();
void min22();
void min23();
void min24();
void min25();
void min26();
void min27();
void min28();
void min29();
void min30();
void min31();
void min32();
void min33();
void min34();
void min35();
void min36();
void min37();
void min38();
void min39();
void min40();
void min41();
void min42();
void min43();
void min44();
void min45();
void min46();
void min47();
void min48();
void min49();
void min50();
void min51();
void min52();
void min53();
void min54();
void min55();
void min56();
void min57();
void min58();
void min59();
void min60();


void setup() {
  Serial.begin(9600);
  delay(500);
  connectWifi();

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection( TypicalLEDStrip );
  FastLED.setBrightness(BRIGHTNESS);
  MySerial.begin(9600, SERIAL_8N1, 23, 18);     //DFPlayer
  
  // Set offset time in seconds to adjust for your timezone, for example:
  // GMT +1 = 3600
  // GMT +8 = 28800
  // GMT -1 = -3600
  // GMT 0 = 0
  timeClient.begin();
  timeClient.setTimeOffset(gmt_offset + (DST * 3600));

  display_commute_every_millis = millis();

  touch_pad_init();

//BMP setup
  if(!bmp.begin()){
    Serial.println("BMP init failed!");
    while(1);
  }
  else Serial.println("BMP init success!");
  
  bmp.setOversampling(4);

  Wire.begin(); //Initial I2C bus for BMP sensor
  delay(1000);

  if (!myDFPlayer.begin(MySerial)) {  //Use softwareSerial to communicate with mp3.
    
    Serial.println(myDFPlayer.readType(),HEX);
    Serial.println(F("Unable to begin:"));
    while(true);
  }
  myDFPlayer.volume(mp3vol);  //Set volume value (0~30).
  Serial.println(F("DFPlayer Mini online."));
  
  myDFPlayer.setTimeOut(500); //Set serial communictaion time out 500ms


  Test_LEDs();

  //Initiate array with 0 (no value = no prediction)
  for (int x = 1; x <= Zambrettiarraysize; x++)
  {
    Zambretti_array[x] = 990;
  }

  pressure_read_millis = millis();

  timeClient.forceUpdate();   //need to force and update to get correct time
  updateLocalTime();

  old_hour = hour;
  old_minute = minute;
  
  saved_timestamp = current_timestamp;
  millis_unix_timestamp = millis(); //millis time tracking reset to current millis when getting new NTP time
  millis_unix_timestamp_baseline = current_timestamp;

    Serial.println();
    Serial.print("Current timestamp GMT: ");
    Serial.print(current_timestamp);    
    Serial.print(" - ");
    Serial.print(hour);
    Serial.print(":");
    Serial.print(minute);    
    Serial.print(":");
    Serial.print(second);    
    Serial.print(AMPM);  
    Serial.print(" - ");      
    Serial.print("day:");
    Serial.println(day); 
    Serial.println();

  SPIFFS_init();

  if (SPIFFS.begin())
  {
    Serial.println("SPIFFS opened!");
    Serial.println("");
  }

  //Initial run - Do this now so if rebooted gets old data and starts with history, otherwise we wait for 30mins for this to happen
  measurementEvent(); //Get BMP280 Pressure data
    yield();
  ReadFromSPIFFS(); //Read the previous SPIFFs
    yield();
  UpdateSPIFFS(); //Update the SPIFFs
    yield();
  Zambretti_calc();

  zambretticount = millis(); //Initial count for Zambretti update
  timecount = millis();      //Initial count for NTP update
  count = millis();          //Initial count for Geiger LED update update
  working_modecount = millis();

  //Force API pull at startup if passes weekend check
  API_trigger = 1;

  if (show_weekend == 1 || (show_weekend == 0 && (day != 6 && day != 0))){
      google_API();      //Get new commute time
  }  

  API_trigger = 0;

  Serial.println();
  Serial.print("Zambretti says: ");
  Serial.print(ZambrettisWords);
  Serial.print(", ");
  Serial.println(trend_in_words);
  Serial.print("Prediction accuracy: ");
  Serial.print(accuracy_in_percent);
  Serial.println("%");
  Serial.print("Zambretti mp3 = ");
  Serial.println(Zambretti_mp3);

  if (accuracy < 12)
  {
    Serial.println("Reason: Not enough weather data yet.");
    Serial.print("We need ");
    Serial.print((12 - accuracy) / 2);
    Serial.println(" hours more to get sufficient data.");
  }
  Serial.println();

  /////FTP Setup, ensure SPIFFS is started before ftp;  /////////
  if (SPIFFS.begin(true)) {
      Serial.println("FTP Check - SPIFFS opened!");
      ftpSrv.begin("andrew","test");    //username, password for ftp.
  }


}

void loop()
{
    while(!timeClient.update()) {
    timeClient.forceUpdate();
    }

    ftpSrv.handleFTP();               //Keep FTP working
      yield();
    updateLocalTime();                //Get the time
      yield();
    google_API_hour_check_fetch();    //work out if the current minute for the hour has an API pull
      yield();
    commute_flip_check();             //figure out what to display, clock or commute time
      yield();    
    doLEDs();                         //Work out what LEDs to display and show them
      yield();
    Zambretti_process();              //Update forecast every 30s, updates SPIFFs every 30mins
      yield();
    touch_check();                    //Check if touch sensors used
      yield();
    chime_check();                    //Once time is updated, and displayed check chimes last (so sounds play after screen updated with time)
      yield();    
}

void show_touch() 
{
//Test function to display the touch values on the display
    int mins = touchRead(touchPin2);
    numeral1 = trunc(mins / 100);
    numeral2 = trunc((mins - (numeral1 *100))/10);
    numeral3 = trunc(mins - (numeral1 *100) - (numeral2 *10));
    background_commute();
    Commute_digits_LED();
    FastLED.show();
    delay (500);  
}

void connectWifi() 
{
  WiFi.begin(ssid, pass);
  Serial.print("Connecting to ");
  Serial.println(ssid);
  while (WiFi.status() != WL_CONNECTED) 
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected!");
  Serial.println(WiFi.localIP());
  Serial.println();
}

void Commute_digits_LED(){

//digit 1
int y1 = digit1_array[numeral1][0];
for (int x1=1; x1 <= y1; x1++){
  leds[(digit1_array[numeral1][x1])-1] = CRGB(crd,cgd,cbd);
}

//digit 2
y1 = digit2_array[numeral2][0];
for (int x1=1; x1 <= y1; x1++){
  leds[(digit2_array[numeral2][x1])-1] = CRGB(crd,cgd,cbd);
}

//digit 3
y1 = digit3_array[numeral3][0];
for (int x1=1; x1 <= y1; x1++){
  leds[(digit3_array[numeral3][x1])-1] = CRGB(crd,cgd,cbd);
}

}

void updateLocalTime()
{
    current_timestamp = timeClient.getEpochTime();

    day = (((current_timestamp / 86400L) + 4 ) % 7);
    //day = timeClient.getDay();    //only gets day for GMT not for our timezone

    hour = timeClient.getHours();
    minute = timeClient.getMinutes();
    second = timeClient.getSeconds();
    hour24 = hour;

    AMPM = "AM";

    if (hour >= 12){
        AMPM = "PM";
        }

    if (hour > 12){
        hour = hour - 12;
        }
  }

void google_API(){
 
    int mins;
    int hours;
    int total_mins;

    String google_API_request;
    String API_result;

    if ((AMPM == "AM" && north_south_flag == 0) || north_south_flag == 2){
    google_API_request = API_request_string("south",google_API_mode);
      API_result = API_request(google_API_request);
      Serial.println(API_result);
    
    uint16_t estimated_time = JSON_extract(API_result);

    Serial.print("Estimate time (sec) = ");
    Serial.println(estimated_time);

    total_mins = int(estimated_time/60);
    hours = trunc(total_mins/60);
    mins = round((total_mins - hours) +0.5);    //add 0.5 (30sec) to force a round up
  
    Serial.print("Estimate time hours/mins = ");
    Serial.print(hours);
    Serial.print(":");
    Serial.println(mins);
    }

    if ((AMPM == "PM" && north_south_flag == 0) || north_south_flag == 1){
    google_API_request = API_request_string("north",google_API_mode);
      API_result = API_request(google_API_request);
      Serial.println(API_result);

    uint16_t estimated_time = JSON_extract(API_result);

    Serial.print("Estimate time (sec) = ");
    Serial.println(estimated_time);

    total_mins = int(estimated_time/60);
    hours = trunc(total_mins/60);
    mins = round((total_mins - hours) +0.5);    //add 0.5 (30sec) to force a round up
  
    Serial.print("Estimate time hours/mins = ");
    Serial.print(hours);
    Serial.print(":");
    Serial.println(mins);
    }

    numeral1 = trunc(mins / 100);
    numeral2 = trunc((mins - (numeral1 *100))/10);
    numeral3 = trunc(mins - (numeral1 *100) - (numeral2 *10));

    last_API_pull = millis();
}

const uint16_t JSON_extract(String json_payload){

  // Inside the brackets, 200 is the capacity of the memory pool in bytes.
  // Don't forget to change this value to match your JSON document.
  // Use arduinojson.org/v6/assistant to compute the capacity.
  StaticJsonDocument<600> doc;
  
  // Deserialize the JSON document
  DeserializationError error = deserializeJson(doc, json_payload);

  // Test if parsing succeeds.
  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.c_str());
    return 0;
  }

  // Fetch values.
  const uint16_t travel_seconds = doc["rows"][0]["elements"][0]["duration_in_traffic"]["value"];

  return travel_seconds;
}


//Create API request and call API function
String API_request_string(String direction, String google_API_mode) {

String google_API_payload;

if (direction == "south"){
  google_API_payload = google_API_start + google_API_home + "&destinations=" + google_API_work + "&mode=" + google_API_mode + "&" + google_API_traffic + "&language=" + google_API_lang + "&key=" + google_API_key;
  }

if (direction == "north"){
  google_API_payload = google_API_start + google_API_work + "&destinations=" + google_API_home + "&mode=" + google_API_mode + "&" + google_API_traffic + "&language=" + google_API_lang + "&key=" + google_API_key;
}
  Serial.println("");
  Serial.print("Google API string = ");
  Serial.println(google_API_payload);
  Serial.println();

return google_API_payload;
}



String API_request(String http_address){

  Serial.println("Making google API request");
  
  if ((WiFi.status() == WL_CONNECTED)) { //Check the current connection status
 
    HTTPClient http;
    String payload;

    http.begin(http_address, root_ca); //Specify the URL and certificate
    int httpCode = http.GET();                                                  //Make the request
 
    if (httpCode > 0) { //Check for the returning code
 
        payload = http.getString();
        Serial.println(httpCode);
      }
 
    else {
      Serial.println("Error on HTTP request");
    }
 
    http.end(); //Free the resources
    return payload;  
  }

  return "";
}


void doclock(){
    if(hour==0||hour==12)
    {
      hour12();
    }
    if(hour==1)
    {
      hour1();
    }
    if(hour==2)
    {
      hour2();
    }
    if(hour==3)
    {
      hour3();
    }
    if(hour==4)
    {
      hour4();
    }
    if(hour==5)
    {
      hour5();
    }
    if(hour==6)
    {
      hour6();
    }
    if(hour==7)
    {
      hour7();
    }
    if(hour==8)
    {
      hour8();
    }
    if(hour==9)
    {
      hour9();
    }
    if(hour==10)
    {
      hour10();
    }
    if(hour==11)
    {
      hour11();
    }

    if(minute==0)
    {
      min0();
    }
    if(minute==1)
    {
      min1();
    }
    if(minute==2)
    {
      min2();
    }
    if(minute==3)
    {
      min3();
    }
    if(minute==4)
    {
      min4();
    }
    if(minute==5)
    {
      min5();
    }
    if(minute==6)
    {
      min6();
    }
    if(minute==7)
    {
      min7();
    }
    if(minute==8)
    {
      min8();
    }
    if(minute==9)
    {
      min9();
    }
    if(minute==10)
    {
      min10();
    }
    if(minute==11)
    {
      min11();
    }
    if(minute==12)
    {
      min12();
    }
    if(minute==13)
    {
      min13();
    }
    if(minute==14)
    {
      min14();
    }
    if(minute==15)
    {
      min15();
    }
    if(minute==16)
    {
      min16();
    }
    if(minute==17)
    {
      min17();
    }
    if(minute==18)
    {
      min18();
    }
    if(minute==19)
    {
      min19();
    }
    if(minute==20)
    {
      min20();
    }
    if(minute==21)
    {
      min21();
    }
    if(minute==22)
    {
      min22();
    }
    if(minute==23)
    {
      min23();
    }
    if(minute==24)
    {
      min24();
    }
    if(minute==25)
    {
      min25();
    }
    if(minute==26)
    {
      min26();
    }
    if(minute==27)
    {
      min27();
    }
    if(minute==28)
    {
      min28();
    }
    if(minute==29)
    {
      min29();
    }
    if(minute==30)
    {
      min30();
    }
    if(minute==31)
    {
      min31();
    }
    if(minute==32)
    {
      min32();
    }
    if(minute==33)
    {
      min33();
    }
    if(minute==34)
    {
      min34();
    }
    if(minute==35)
    {
      min35();
    }
    if(minute==36)
    {
      min36();
    }
    if(minute==37)
    {
      min37();
    }
    if(minute==38)
    {
      min38();
    }
    if(minute==39)
    {
      min39();
    }
    if(minute==40)
    {
      min40();
    }
    if(minute==41)
    {
      min41();
    }
    if(minute==42)
    {
      min42();
    }
    if(minute==43)
    {
      min43();
    }
    if(minute==44)
    {
      min44();
    }
    if(minute==45)
    {
      min45();
    }
    if(minute==46)
    {
      min46();
    }
    if(minute==47)
    {
      min47();
    }
    if(minute==48)
    {
      min48();
    }
    if(minute==49)
    {
      min49();
    }
    if(minute==50)
    {
      min50();
    }
    if(minute==51)
    {
      min51();
    }
    if(minute==52)
    {
      min52();
    }
    if(minute==53)
    {
      min53();
    }
    if(minute==54)
    {
      min54();
    }
    if(minute==55)
    {
      min55();
    }
    if(minute==56)
    {
      min56();
    }
    if(minute==57)
    {
      min57();
    }
    if(minute==58)
    {
      min58();
    }
    if(minute==59)
    {
      min59();
    }
    if(minute==60)
    {
      min60();
    }
}

void background_clock()
{
  //FastLED.clear();
  fill_solid(&(leds[0]), NUM_LEDS /*number of leds*/, CRGB(br,bg,bb));
}

void background_commute()
{
  //FastLED.clear();
  fill_solid(&(leds[0]), NUM_LEDS /*number of leds*/, CRGB(cr,cg,cb));
}

/*------------------------------------Hour LEDs------------------------------------*/
void hour12()
{
    leds[2] = CRGB(r,g,b);
    leds[4] = CRGB(r,g,b);
    leds[5] = CRGB(r,g,b);
    leds[7] = CRGB(r,g,b);
    leds[10] = CRGB(r,g,b);
    leds[14] = CRGB(r,g,b);
    leds[16] = CRGB(r,g,b);
    leds[17] = CRGB(r,g,b);
    leds[23] = CRGB(r,g,b);
    leds[25] = CRGB(r,g,b);
    leds[30] = CRGB(r,g,b);
    leds[32] = CRGB(r,g,b);
    leds[33] = CRGB(r,g,b);
}
void hour1()
{
    leds[3] = CRGB(r,g,b);
    leds[9] = CRGB(r,g,b);
    leds[15] = CRGB(r,g,b);
    leds[24] = CRGB(r,g,b);
    leds[31] = CRGB(r,g,b);
}
void hour2()
{
    leds[0] = CRGB(r,g,b);
    leds[1] = CRGB(r,g,b);
    leds[4] = CRGB(r,g,b);
    leds[8] = CRGB(r,g,b);
    leds[9] = CRGB(r,g,b);
    leds[15] = CRGB(r,g,b);
    leds[23] = CRGB(r,g,b);
    leds[24] = CRGB(r,g,b);
}
void hour3()
{
    leds[0] = CRGB(r,g,b);
    leds[1] = CRGB(r,g,b);
    leds[4] = CRGB(r,g,b);
    leds[8] = CRGB(r,g,b);
    leds[9] = CRGB(r,g,b);
    leds[16] = CRGB(r,g,b);
    leds[23] = CRGB(r,g,b);
    leds[24] = CRGB(r,g,b);
}
void hour4()
{
    leds[3] = CRGB(r,g,b);
    leds[4] = CRGB(r,g,b);
    leds[8] = CRGB(r,g,b);
    leds[9] = CRGB(r,g,b);
    leds[16] = CRGB(r,g,b);
    leds[23] = CRGB(r,g,b);
}
void hour5()
{
    leds[0] = CRGB(r,g,b);
    leds[1] = CRGB(r,g,b);
    leds[3] = CRGB(r,g,b);
    leds[8] = CRGB(r,g,b);
    leds[9] = CRGB(r,g,b);
    leds[16] = CRGB(r,g,b);
    leds[23] = CRGB(r,g,b);
    leds[24] = CRGB(r,g,b);
}
void hour6()
{
    leds[0] = CRGB(r,g,b);
    leds[1] = CRGB(r,g,b);
    leds[3] = CRGB(r,g,b);
    leds[8] = CRGB(r,g,b);
    leds[9] = CRGB(r,g,b);
    leds[15] = CRGB(r,g,b);
    leds[16] = CRGB(r,g,b);
    leds[23] = CRGB(r,g,b);
    leds[24] = CRGB(r,g,b);
}
void hour7()
{
    leds[0] = CRGB(r,g,b);
    leds[1] = CRGB(r,g,b);
    leds[3] = CRGB(r,g,b);
    leds[4] = CRGB(r,g,b);
    leds[8] = CRGB(r,g,b);
    leds[16] = CRGB(r,g,b);
    leds[23] = CRGB(r,g,b);
}
void hour8()
{
    leds[0] = CRGB(r,g,b);
    leds[1] = CRGB(r,g,b);
    leds[3] = CRGB(r,g,b);
    leds[4] = CRGB(r,g,b);
    leds[8] = CRGB(r,g,b);
    leds[9] = CRGB(r,g,b);
    leds[15] = CRGB(r,g,b);
    leds[16] = CRGB(r,g,b);
    leds[23] = CRGB(r,g,b);
    leds[24] = CRGB(r,g,b);
}
void hour9()
{
    leds[0] = CRGB(r,g,b);
    leds[1] = CRGB(r,g,b);
    leds[3] = CRGB(r,g,b);
    leds[4] = CRGB(r,g,b);
    leds[8] = CRGB(r,g,b);
    leds[9] = CRGB(r,g,b);
    leds[16] = CRGB(r,g,b);
    leds[23] = CRGB(r,g,b);
    leds[24] = CRGB(r,g,b);
}
void hour10()
{
    leds[2] = CRGB(r,g,b);
    leds[4] = CRGB(r,g,b);
    leds[5] = CRGB(r,g,b);
    leds[7] = CRGB(r,g,b);
    leds[8] = CRGB(r,g,b);
    leds[10] = CRGB(r,g,b);
    leds[14] = CRGB(r,g,b);
    leds[22] = CRGB(r,g,b);
    leds[23] = CRGB(r,g,b);
    leds[25] = CRGB(r,g,b);
    leds[30] = CRGB(r,g,b);
    leds[32] = CRGB(r,g,b);
    leds[33] = CRGB(r,g,b);
}
void hour11()
{
    leds[2] = CRGB(r,g,b);
    leds[4] = CRGB(r,g,b);  
    leds[8] = CRGB(r,g,b);
    leds[10] = CRGB(r,g,b);
    leds[14] = CRGB(r,g,b);
    leds[16] = CRGB(r,g,b);
    leds[23] = CRGB(r,g,b);
    leds[25] = CRGB(r,g,b);
    leds[30] = CRGB(r,g,b);
    leds[32] = CRGB(r,g,b);
}

/*----------------------------Minutes LEDs----------------------------*/
void min0()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min1()
{
    leds[56] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
}
void min2()
{
    leds[55] = CRGB(r,g,b);
    leds[56] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
}
void min3()
{
    leds[55] = CRGB(r,g,b);
    leds[56] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
}
void min4()
{
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
}
void min5()
{
    leds[55] = CRGB(r,g,b);
    leds[56] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
}
void min6()
{
    leds[55] = CRGB(r,g,b);
    leds[56] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
}
void min7()
{
    leds[55] = CRGB(r,g,b);
    leds[56] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
}
void min8()
{
    leds[55] = CRGB(r,g,b);
    leds[56] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
}
void min9()
{
    leds[55] = CRGB(r,g,b);
    leds[56] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
}
void min10()
{
    leds[62] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min11()
{
    leds[62] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
}
void min12()
{
    leds[62] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min13()
{
    leds[62] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min14()
{
    leds[62] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min15()
{
    leds[62] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min16()
{
    leds[62] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min17()
{
    leds[62] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min18()
{
    leds[62] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min19()
{
    leds[62] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min20()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min21()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min22()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min23()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min24()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min25()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min26()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min27()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min28()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min29()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min30()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min31()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min32()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min33()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min34()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min35()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min36()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min37()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min38()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min39()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min40()
{
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min41()
{
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min42()
{
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min43()
{
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min44()
{
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min45()
{
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min46()
{
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min47()
{
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min48()
{
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min49()
{
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[72] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min50()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min51()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min52()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min53()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min54()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min55()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min56()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min57()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min58()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min59()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[80] = CRGB(r,g,b);
    leds[81] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}
void min60()
{
    leds[62] = CRGB(r,g,b);
    leds[63] = CRGB(r,g,b);
    leds[64] = CRGB(r,g,b);
    leds[65] = CRGB(r,g,b);
    leds[70] = CRGB(r,g,b);
    leds[71] = CRGB(r,g,b);
    leds[73] = CRGB(r,g,b);
    leds[78] = CRGB(r,g,b);
    leds[79] = CRGB(r,g,b);
    leds[85] = CRGB(r,g,b);
    leds[86] = CRGB(r,g,b);
    leds[87] = CRGB(r,g,b);
    leds[88] = CRGB(r,g,b);
    leds[90] = CRGB(r,g,b);
    leds[91] = CRGB(r,g,b);
    leds[92] = CRGB(r,g,b);
    leds[93] = CRGB(r,g,b);
}

void Zambretti_process()
{

    measurementEvent();               //Get pressure readings
      yield();
    MinMax_pressure_handle();         //Maintain min and max pressures for fun

    delt_t = millis() - count;        //Calculate forecast every now and then
      if (delt_t > delayamount)
      {
        Zambretti_calc();             //Calculate weather forecast based on data and print
        count = millis();             //Reset the count
      }

      delt_t = millis() - zambretticount;     //Update pressure data to SPIFFs every 30mins
      if (delt_t > zambretti_delayamount)
      {
        ReadFromSPIFFS(); //Read the previous SPIFFs
          yield();
        UpdateSPIFFS(); //Update the SPIFFs
        
        Serial.print("Zambretti says: ");
        Serial.print(ZambrettisWords);
        Serial.print(", ");
        Serial.println(trend_in_words);
        Serial.print("Prediction accuracy: ");
        Serial.print(accuracy_in_percent);
        Serial.println("%");
        Serial.print("Zambretti mp3 = ");
        Serial.println(Zambretti_mp3);
        if (accuracy < 12)
        {
          Serial.println("Reason: Not enough weather data yet.");
          Serial.print("We need ");
          Serial.print((12 - accuracy) / 2);
          Serial.println(" hours more to get sufficient data.");
        }

        zambretticount = millis(); //Reset the count
      }
    
    //Check if new Zambretti forecast, and if new run background colour function with flag set
    if (ZambrettisWords != old_ZambrettisWords){
     weather_background_flag = 1;         //only run once every weather mode to fill in background
     old_ZambrettisWords = ZambrettisWords;
    }      
}

void Zambretti_calc()
{

  //**************************Calculate Zambretti Forecast*******************************************

  accuracy_in_percent = accuracy * 94 / 12; // 94% is the max predicion accuracy of Zambretti

  ZambrettisWords = ZambrettiSays(char(ZambrettiLetter()));

  if (verbose_output == 1)
  {
    Serial.print("Zambretti says: ");
    Serial.print(ZambrettisWords);
    Serial.print(", ");
    Serial.println(trend_in_words);
    Serial.print("Prediction accuracy: ");
    Serial.print(accuracy_in_percent);
    Serial.println("%");
    Serial.print("Zambretti mp3 = ");
    Serial.println(Zambretti_mp3);
    if (accuracy < 12)
    {
      Serial.println("Reason: Not enough weather data yet.");
      Serial.print("We need ");
      Serial.print((12 - accuracy) / 2);
      Serial.println(" hours more to get sufficient data.");
    }
  }
}

char ZambrettiLetter()
{

  struct tm timeinfo;

  //Serial.println("---> Calculating Zambretti letter");
  char z_letter;
  int(z_trend) = CalculateTrend();
  // Case trend is falling
  if (z_trend == -1)
  {
    float zambretti = 0.0009746 * rel_pressure_rounded * rel_pressure_rounded - 2.1068 * rel_pressure_rounded + 1138.7019;
    if ((timeinfo.tm_mon < 4 || timeinfo.tm_mon > 9) && HEMISPHERE == "NORTH"){
      zambretti = zambretti + 1;
    }    
    
    if ((timeinfo.tm_mon > 4 || timeinfo.tm_mon < 9) && HEMISPHERE == "SOUTH"){
      zambretti = zambretti + 1;
    }

    if (verbose_output == 1)
    {
      Serial.print("Calculated and rounded Zambretti in numbers: ");
      Serial.println(round(zambretti));
      Serial.print("rel_pressure_rounded: ");
      Serial.println(rel_pressure_rounded);
    }

    switch (int(round(zambretti)))
    {
    case 0:
      z_letter = 'A';
      break; //Settled Fine
    case 1:
      z_letter = 'A';
      break; //Settled Fine
    case 2:
      z_letter = 'B';
      break; //Fine Weather
    case 3:
      z_letter = 'D';
      break; //Fine Becoming Less Settled
    case 4:
      z_letter = 'H';
      break; //Fairly Fine Showers Later
    case 5:
      z_letter = 'O';
      break; //Showery Becoming unsettled
    case 6:
      z_letter = 'R';
      break; //Unsettled, Rain later
    case 7:
      z_letter = 'U';
      break; //Rain at times, worse later
    case 8:
      z_letter = 'V';
      break; //Rain at times, becoming very unsettled
    case 9:
      z_letter = 'X';
      break; //Very Unsettled, Rain
    }
  }
  // Case trend is steady
  if (z_trend == 0)
  {
    float zambretti = 138.24 - 0.133 * rel_pressure_rounded;

    if (verbose_output == 1)
    {
      Serial.print("Calculated and rounded Zambretti in numbers: ");
      Serial.println(round(zambretti));
      Serial.print("rel_pressure_rounded: ");
      Serial.println(rel_pressure_rounded);      
    }
    switch (int(round(zambretti)))
    {
    case 0:
      z_letter = 'A';
      break; //Settled Fine
    case 1:
      z_letter = 'A';
      break; //Settled Fine
    case 2:
      z_letter = 'B';
      break; //Fine Weather
    case 3:
      z_letter = 'E';
      break; //Fine, Possibly showers
    case 4:
      z_letter = 'K';
      break; //Fairly Fine, Showers likely
    case 5:
      z_letter = 'N';
      break; //Showery Bright Intervals
    case 6:
      z_letter = 'P';
      break; //Changeable some rain
    case 7:
      z_letter = 'S';
      break; //Unsettled, rain at times
    case 8:
      z_letter = 'W';
      break; //Rain at Frequent Intervals
    case 9:
      z_letter = 'X';
      break; //Very Unsettled, Rain
    case 10:
      z_letter = 'Z';
      break; //Stormy, much rain
    }
  }
  // Case trend is rising
  if (z_trend == 1)
  {
    float zambretti = 142.57 - 0.1376 * rel_pressure_rounded;
    //A Summer rising, improves the prospects by 1 unit over a Winter rising
    if ((timeinfo.tm_mon < 4 || timeinfo.tm_mon > 9) && HEMISPHERE == "NORTH"){
      zambretti = zambretti + 1;
    }    
    
    if ((timeinfo.tm_mon > 4 || timeinfo.tm_mon < 9) && HEMISPHERE == "SOUTH"){
      zambretti = zambretti + 1;
    }

    if (verbose_output == 1)
    {
      Serial.print("Calculated and rounded Zambretti in numbers: ");
      Serial.println(round(zambretti));
      Serial.print("rel_pressure_rounded: ");
      Serial.println(rel_pressure_rounded);      
    }

    switch (int(round(zambretti)))
    {
    case 0:
      z_letter = 'A';
      break; //Settled Fine
    case 1:
      z_letter = 'A';
      break; //Settled Fine
    case 2:
      z_letter = 'B';
      break; //Fine Weather
    case 3:
      z_letter = 'C';
      break; //Becoming Fine
    case 4:
      z_letter = 'F';
      break; //Fairly Fine, Improving
    case 5:
      z_letter = 'G';
      break; //Fairly Fine, Possibly showers, early
    case 6:
      z_letter = 'I';
      break; //Showery Early, Improving
    case 7:
      z_letter = 'J';
      break; //Changeable, Improving
    case 8:
      z_letter = 'L';
      break; //Rather Unsettled Clearing Later
    case 9:
      z_letter = 'M';
      break; //Unsettled, Probably Improving
    case 10:
      z_letter = 'Q';
      break; //Unsettled, short fine Intervals
    case 11:
      z_letter = 'T';
      break; //Very Unsettled, Finer at times
    case 12:
      z_letter = 'Y';
      break; //Stormy, possibly improving
    case 13:
      z_letter = 'Z';
      break;
      ; //Stormy, much rain
    }
  }
  if (verbose_output == 1)
  {
    Serial.print("This is Zambretti's famous letter: ");
    Serial.println(z_letter);
    Serial.print("Zambretti LED = ");
    Serial.println(Zambretti_LED);
    Serial.println("");

  }

  return z_letter;
}

int CalculateTrend()
{
  int trend; // -1 falling; 0 steady; 1 raising

  //--> giving the most recent pressure reads more weight
  pressure_difference[0] = (pressure_value[0] - pressure_value[1]) * 1.5;
  pressure_difference[1] = (pressure_value[0] - pressure_value[2]);
  pressure_difference[2] = (pressure_value[0] - pressure_value[3]) / 1.5;
  pressure_difference[3] = (pressure_value[0] - pressure_value[4]) / 2;
  pressure_difference[4] = (pressure_value[0] - pressure_value[5]) / 2.5;
  pressure_difference[5] = (pressure_value[0] - pressure_value[6]) / 3;
  pressure_difference[6] = (pressure_value[0] - pressure_value[7]) / 3.5;
  pressure_difference[7] = (pressure_value[0] - pressure_value[8]) / 4;
  pressure_difference[8] = (pressure_value[0] - pressure_value[9]) / 4.5;
  pressure_difference[9] = (pressure_value[0] - pressure_value[10]) / 5;
  pressure_difference[10] = (pressure_value[0] - pressure_value[11]) / 5.5;

  //--> calculating the average and storing it into [11]
  pressure_difference[11] = (pressure_difference[0] + pressure_difference[1] + pressure_difference[2] + pressure_difference[3] + pressure_difference[4] + pressure_difference[5] + pressure_difference[6] + pressure_difference[7] + pressure_difference[8] + pressure_difference[9] + pressure_difference[10]) / 11;

  if (verbose_output == 1)
  {
    Serial.print("Current trend: ");
    Serial.print(pressure_difference[11]);
    Serial.print(" -->  ");
  }

  if (pressure_difference[11] > 3.5)
  {
    trend_in_words = TEXT_RISING_FAST;
    Zambretti_trend_mp3 = 127;
    trend = 1;
  }
  else if (pressure_difference[11] > 1.5 && pressure_difference[11] <= 3.5)
  {
    trend_in_words = TEXT_RISING;
    Zambretti_trend_mp3 = 128;
    trend = 1;
  }
  else if (pressure_difference[11] > 0.25 && pressure_difference[11] <= 1.5)
  {
    trend_in_words = TEXT_RISING_SLOW;
    Zambretti_trend_mp3 = 129;
    trend = 1;
  }
  else if (pressure_difference[11] > -0.25 && pressure_difference[11] < 0.25)
  {
    trend_in_words = TEXT_STEADY;
    Zambretti_trend_mp3 = 130;
    trend = 0;
  }
  else if (pressure_difference[11] >= -1.5 && pressure_difference[11] < -0.25)
  {
    trend_in_words = TEXT_FALLING_SLOW;
    Zambretti_trend_mp3 = 131;
    trend = -1;
  }
  else if (pressure_difference[11] >= -3.5 && pressure_difference[11] < -1.5)
  {
    trend_in_words = TEXT_FALLING;
    Zambretti_trend_mp3 = 132;
    trend = -1;
  }
  else if (pressure_difference[11] <= -3.5)
  {
    trend_in_words = TEXT_FALLING_FAST;
    Zambretti_trend_mp3 = 133;
    trend = -1;
  }

  if (verbose_output == 1)
  {
    Serial.println(trend_in_words);
  }

  return trend;
}

String ZambrettiSays(char code)
{

  //code = 'Y';

  Zambretti_LED = 0;
  String zambrettis_words = "";
  Zambretti_mp3 = 126;

  switch (code)
  {
  case 'A':
    zambrettis_words = TEXT_ZAMBRETTI_A;    //"Settled Fine Weather"
    Zambretti_mp3 = 100;

    r=255;    //White purple digits 
    g=255;
    b=255;

    weather_mode = "sun";      //rain or sun
    break; 

  case 'B':
    zambrettis_words = TEXT_ZAMBRETTI_B;    //"Fine Weather"
    Zambretti_mp3 = 101;
    
    r=255;    //White purple digits 
    g=255;
    b=255;

    weather_mode = "sun";      //rain or sun
    break;

  case 'C':
    zambrettis_words = TEXT_ZAMBRETTI_C;    //"Becoming Fine"
    Zambretti_mp3 = 102;
        
    //yellow background
    weather_RGB_R = 255;    //colour of background
    weather_RGB_G = 255;
    weather_RGB_B = 0;
    
    rain_RGB_R = 175;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=255;    //White purple digits 
    g=255;
    b=255;

    weather_mode = "rain";      //rain or sun
    weather_time = 75;   //speed of function (raining a little or lots)
    weather_density = 8;  //Probability of rain (low = more) -1 = none 
    break;

  case 'D':
    zambrettis_words = TEXT_ZAMBRETTI_D;    //"Fine, Becoming Less Settled"
    Zambretti_mp3 = 103;

    //darker yellow background
    weather_RGB_R = 255;    //colour of background
    weather_RGB_G = 150;
    weather_RGB_B = 0;
    
    rain_RGB_R = 175;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=100;    //light purple digits 
    g=96;
    b=255;

    weather_mode = "rain";      //rain or sun
    weather_time = 75;   //speed of function (raining a little or lots)
    weather_density = 3;  //Probability of rain (low = more) -1 = none 
    break;

  case 'E':
    zambrettis_words = TEXT_ZAMBRETTI_E;    //"Fine, Possibly showers"
    Zambretti_mp3 = 104;

    //darker yellow background
    weather_RGB_R = 255;    //colour of background
    weather_RGB_G = 150;
    weather_RGB_B = 0;
    
    rain_RGB_R = 175;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=100;    //light purple digits 
    g=96;
    b=255;

    weather_mode = "rain";      //rain or sun
    weather_time = 75;   //speed of function (raining a little or lots)
    weather_density = 5;  //Probability of rain (low = more) -1 = none     
    break;

  case 'F':
    zambrettis_words = TEXT_ZAMBRETTI_F;    //"Fairly Fine, Improving"
    Zambretti_mp3 = 105;

    //darker yellow background
    weather_RGB_R = 255;    //colour of background
    weather_RGB_G = 230;
    weather_RGB_B = 0;
    
    rain_RGB_R = 175;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=150;    //light grey digits 
    g=150;
    b=185;

    weather_mode = "rain";      //rain or sun
    weather_time = 75;   //speed of function (raining a little or lots)
    weather_density = 5;  //Probability of rain (low = more) -1 = none 
    break;

  case 'G':
    zambrettis_words = TEXT_ZAMBRETTI_G;    //"Fairly Fine, Possibly showers early"
    Zambretti_mp3 = 106;
        
    //darker yellow background
    weather_RGB_R = 255;    //colour of background
    weather_RGB_G = 230;
    weather_RGB_B = 0;
    
    rain_RGB_R = 175;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=150;    //light grey digits 
    g=150;
    b=185;

    weather_mode = "rain";      //rain or sun
    weather_time = 75;   //speed of function (raining a little or lots)
    weather_density = 3;  //Probability of rain (low = more) -1 = none 
    break;

  case 'H':
    zambrettis_words = TEXT_ZAMBRETTI_H;    //"Fairly Fine, Showers Later"
    Zambretti_mp3 = 107;

    //darker yellow background
    weather_RGB_R = 255;    //colour of background
    weather_RGB_G = 230;
    weather_RGB_B = 0;
    
    rain_RGB_R = 175;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=150;    //light grey digits 
    g=150;
    b=185;

    weather_mode = "rain";      //rain or sun
    weather_time = 75;   //speed of function (raining a little or lots)
    weather_density = 3;  //Probability of rain (low = more) -1 = none 
    break;

  case 'I':
    zambrettis_words = TEXT_ZAMBRETTI_I;    //"Showery Early, Improving"
    Zambretti_mp3 = 108;
    
    //light green background
    weather_RGB_R = 55;    //colour of background
    weather_RGB_G = 255;
    weather_RGB_B = 55;
    
    rain_RGB_R = 175;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=150;    //light grey digits 
    g=150;
    b=185;

    weather_mode = "rain";      //rain or sun
    weather_time = 75;   //speed of function (raining a little or lots)
    weather_density = 3;  //Probability of rain (low = more) -1 = none 
    break;
    
  case 'J':
    zambrettis_words = TEXT_ZAMBRETTI_J;    //"Changeable Improving"
    Zambretti_mp3 = 109;

    //light green background
    weather_RGB_R = 100;    //colour of background
    weather_RGB_G = 255;
    weather_RGB_B = 55;
    
    rain_RGB_R = 175;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=255 /2;    //grey digits 
    g=255 /2;
    b=0;

    weather_mode = "rain";      //rain or sun
    weather_time = 75;   //speed of function (raining a little or lots)
    weather_density = 3;  //Probability of rain (low = more) -1 = none 
    break;

  case 'K':
    zambrettis_words = TEXT_ZAMBRETTI_K;    //"Fairly Fine, Showers likely"
    Zambretti_mp3 = 110;

    //darker yellow background
    weather_RGB_R = 255;    //colour of background
    weather_RGB_G = 230;
    weather_RGB_B = 0;
    
    rain_RGB_R = 175;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=150;    //light grey of digits 
    g=150;
    b=185;

    weather_mode = "rain";      //rain or sun
    weather_time = 75;    //speed of function (raining a little or lots)
    weather_density = 5;  //Probability of rain (low = more) -1 = none 
    break;

  case 'L':
    zambrettis_words = TEXT_ZAMBRETTI_L;    //"Rather Unsettled Clearing Later"
    Zambretti_mp3 = 111;

    //Dark pink background
    weather_RGB_R = 255 /2;    //colour of background
    weather_RGB_G = 0;
    weather_RGB_B = 155 /2;
    
    rain_RGB_R = 175;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=150 /2;    //dark grey of digits 
    g=150 /2;
    b=185 /2;

    weather_mode = "rain";      //rain or sun
    weather_time = 55;    //speed of function (raining a little or lots)
    weather_density = 1;  //Probability of rain (low = more) -1 = none 
    break;

  case 'M':
    zambrettis_words = TEXT_ZAMBRETTI_M;    //"Unsettled, Probably Improving"
    Zambretti_mp3 = 112;

    //Dark pink background
    weather_RGB_R = 75;    //colour of background
    weather_RGB_G = 0;
    weather_RGB_B = 155;
    
    rain_RGB_R = 175;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=150;    //light grey of digits 
    g=150;
    b=185;

    weather_mode = "rain";      //rain or sun
    weather_time = 55;    //speed of function (raining a little or lots)
    weather_density = 1;  //Probability of rain (low = more) -1 = none 
    break;

  case 'N':
    zambrettis_words = TEXT_ZAMBRETTI_N;    //"Showery Bright Intervals"
    Zambretti_mp3 = 113;

    //darker yellow background
    weather_RGB_R = 255;    //colour of background
    weather_RGB_G = 230;
    weather_RGB_B = 0;
    
    rain_RGB_R = 175;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=150;    //light grey of digits 
    g=150;
    b=185;

    weather_mode = "rain";      //rain or sun
    weather_time = 75;    //speed of function (raining a little or lots)
    weather_density = 5;  //Probability of rain (low = more) -1 = none 
    break;

  case 'O':
    zambrettis_words = TEXT_ZAMBRETTI_O;    //"Showery Becoming Unsettled"
    Zambretti_mp3 = 114;

    //Orange background
    weather_RGB_R = 255;    //colour of background
    weather_RGB_G = 60;
    weather_RGB_B = 0;
    
    rain_RGB_R = 175;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=150;    //light grey of digits 
    g=150;
    b=185;

    weather_mode = "rain";      //rain or sun
    weather_time = 75;    //speed of function (raining a little or lots)
    weather_density = 5;  //Probability of rain (low = more) -1 = none 
    break;

  case 'P':
    zambrettis_words = TEXT_ZAMBRETTI_P;    //"Changeable some rain"
    Zambretti_mp3 = 115;

    //Light aqua background
    weather_RGB_R = 0;    //colour of background
    weather_RGB_G = 110;
    weather_RGB_B = 110;
    
    rain_RGB_R = 175;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=150;    //light grey of digits 
    g=150;
    b=185;

    weather_mode = "rain";      //rain or sun
    weather_time = 75;   //speed of function (raining a little or lots)
    weather_density = 4;  //Probability of rain (low = more) -1 = none 
    break;

  case 'Q':
    zambrettis_words = TEXT_ZAMBRETTI_Q;    //"Unsettled, short fine Intervals"
    Zambretti_mp3 = 116;

    //Orange background
    weather_RGB_R = 255;    //colour of background
    weather_RGB_G = 60;
    weather_RGB_B = 0;
    
    rain_RGB_R = 175;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=150;    //yellow grey of digits 
    g=150;
    b=0;

    weather_mode = "rain";      //rain or sun
    weather_time = 75;    //speed of function (raining a little or lots)
    weather_density = 5;  //Probability of rain (low = more) -1 = none 
    break;

  case 'R':
    zambrettis_words = TEXT_ZAMBRETTI_R;    //"Unsettled, Rain later"
    Zambretti_mp3 = 117;

    //Orange background
    weather_RGB_R = 255;    //colour of background
    weather_RGB_G = 60;
    weather_RGB_B = 0;
    
    rain_RGB_R = 175;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=150;    //light grey of digits 
    g=150;
    b=185;

    weather_mode = "rain";      //rain or sun
    weather_time = 75;    //speed of function (raining a little or lots)
    weather_density = 5;  //Probability of rain (low = more) -1 = none 
    break;

  case 'S':
    zambrettis_words = TEXT_ZAMBRETTI_S;    //"Unsettled, rain at times"
    Zambretti_mp3 = 118;

    //Orange background
    weather_RGB_R = 255;    //colour of background
    weather_RGB_G = 60;
    weather_RGB_B = 0;
    
    rain_RGB_R = 175;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=150;    //light grey of digits 
    g=150;
    b=185;

    weather_mode = "rain";      //rain or sun
    weather_time = 75;    //speed of function (raining a little or lots)
    weather_density = 5;  //Probability of rain (low = more) -1 = none 
    break;

  case 'T':
    zambrettis_words = TEXT_ZAMBRETTI_T;    //"Very Unsettled, Finer at times"
    Zambretti_mp3 = 119;

    //Dark pink background
    weather_RGB_R = 75;    //colour of background
    weather_RGB_G = 0;
    weather_RGB_B = 155;
    
    rain_RGB_R = 175;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=150;    //yellow grey of digits 
    g=150;
    b=0;

    weather_mode = "rain";      //rain or sun
    weather_time = 55;    //speed of function (raining a little or lots)
    weather_density = 1;  //Probability of rain (low = more) -1 = none 
    break;

  case 'U':
    zambrettis_words = TEXT_ZAMBRETTI_U;    //"Rain at times, Worse later"
    Zambretti_mp3 = 120;

    //Light aqua background
    weather_RGB_R = 0;    //colour of background
    weather_RGB_G = 110;
    weather_RGB_B = 110;
    
    rain_RGB_R = 255;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=250;    //light grey of digits 
    g=50;
    b=0;

    weather_mode = "rain";      //rain or sun
    weather_time = 75;   //speed of function (raining a little or lots)
    weather_density = 1;  //Probability of rain (low = more) -1 = none 
    break;

  case 'V':
    zambrettis_words = TEXT_ZAMBRETTI_V;    //"Rain at times, becoming very unsettled"
    Zambretti_mp3 = 121;

    //Light aqua background
    weather_RGB_R = 0;    //colour of background
    weather_RGB_G = 110;
    weather_RGB_B = 110;
    
    rain_RGB_R = 255;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 255;

    r=255;    //light grey of digits 
    g=0;
    b=0;

    weather_mode = "rain";      //rain or sun
    weather_time = 75;   //speed of function (raining a little or lots)
    weather_density = 1;  //Probability of rain (low = more) -1 = none 
    break;
  
  case 'W':
    zambrettis_words = TEXT_ZAMBRETTI_W;    //"Rain at Frequent Intervals"
    Zambretti_mp3 = 122;

    //light green background
    weather_RGB_R = 100;    //colour of background
    weather_RGB_G = 255;
    weather_RGB_B = 55;
    
    rain_RGB_R = 255;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 255;

    r=255 /2;    //grey digits 
    g=255 /2;
    b=0;

    weather_mode = "rain";      //rain or sun
    weather_time = 75;   //speed of function (raining a little or lots)
    weather_density = 0;  //Probability of rain (low = more) -1 = none 
    break;

  case 'X':
    zambrettis_words = TEXT_ZAMBRETTI_X;    //"Very Unsettled, Rain"
    Zambretti_mp3 = 123;

    //Red background
    weather_RGB_R = 255;    //colour of background
    weather_RGB_G = 0;
    weather_RGB_B = 0;
    
    rain_RGB_R = 255;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 175;

    r=150 /3;    //dark grey of digits 
    g=150 /3;
    b=185 /3;

    weather_mode = "rain";      //rain or sun
    weather_time = 55;    //speed of function (raining a little or lots)
    weather_density = 1;  //Probability of rain (low = more) -1 = none
    break;

  case 'Y':
    zambrettis_words = TEXT_ZAMBRETTI_Y;    //"Stormy, possibly improving"
    Zambretti_mp3 = 124;

    //Red background
    weather_RGB_R = 255;    //colour of background
    weather_RGB_G = 0;
    weather_RGB_B = 0;
    
    rain_RGB_R = 255;     //colour of rain
    rain_RGB_G = 128;
    rain_RGB_B = 0;

    r=150 /3;    //dark grey of digits 
    g=150 /3;
    b=185 /3;

    weather_mode = "rain";      //rain or sun
    weather_time = 55;    //speed of function (raining a little or lots)
    weather_density = 1;  //Probability of rain (low = more) -1 = none
    break;

  case 'Z':
    zambrettis_words = TEXT_ZAMBRETTI_Z;    //"Stormy, much rain"
    Zambretti_mp3 = 125;

    //Red background
    weather_RGB_R = 255;    //colour of background
    weather_RGB_G = 0;
    weather_RGB_B = 0;
    
    rain_RGB_R = 255;     //colour of rain
    rain_RGB_G = 255;
    rain_RGB_B = 255;

    r=150 /3;    //dark grey of digits 
    g=150 /3;
    b=185 /3;

    weather_mode = "rain";      //rain or sun
    weather_time = 55;    //speed of function (raining a little or lots)
    weather_density = 1;  //Probability of rain (low = more) -1 = none
    break;

  default:
    zambrettis_words = TEXT_ZAMBRETTI_DEFAULT;    //"Sorry, no forecast for the moment"
    Zambretti_mp3 = 126;
    break;
  }
  return zambrettis_words;
}


void MinMax_pressure_handle()
{

  if (pressure < minpressure)
  {
    minpressure = pressure;
  }

  if (pressure > maxpressure)
  {
    maxpressure = pressure;
  }

  if (verbose_output == 2)
  {

    Serial.print("Current / Min / Max pressure: ");
    Serial.print(pressure);
    Serial.print(" / ");
    Serial.print(minpressure);
    Serial.print(" / ");
    Serial.println(maxpressure);

    Serial.println();
    Serial.println("********************************************************\n");

    //From BMP280 Sensor
    Serial.print("temperature, pressure: ");
    Serial.print(" C,  ");

    Serial.print(pressure, 1);
    Serial.println(" milli bar");

    Serial.print("Current UNIX Timestamp: ");
    Serial.println(current_timestamp);

    Serial.println();
    Serial.println("********************************************************\n");

  }

  sumCount = 0;
  sum = 0;
}

void measurementEvent()
{

  //Measures absolute Pressure, Temperature, Humidity, Voltage, calculate relative pressure,
  //Dewpoint, Dewpoint Spread, Heat Index


  double T,P;
  char result = bmp.startMeasurment();

if(result!=0){
    delay(result);
    result = bmp.getTemperatureAndPressure(T,P);
    
      if(result!=0)
      {
        double A = bmp.altitude(P,P0);     
      }
      else {
        Serial.println("Error.");
      }
  }
  else {
    Serial.println("Error.");
  }

  // Get temperature
  measured_temp = T;

  // Get pressure
  measured_pres = P;
  pressure = P;

  // Calculate and print relative pressure
  SLpressure_hPa = (((measured_pres * 100.0)/pow((1-((float)(ELEVATION))/44330), 5.255))/100.0);
  rel_pressure_rounded=(int)(SLpressure_hPa+.5);

  // Calculate dewpoint
  double a = 17.271;
  double b = 237.7;
  double tempcalc = (a * measured_temp) / (b + measured_temp) + log(measured_humi * 0.01);
  DewpointTemperature = (b * tempcalc) / (a - tempcalc);

  // Calculate dewpoint spread (difference between actual temp and dewpoint -> the smaller the number: rain or fog
  DewPointSpread = measured_temp - DewpointTemperature;

  // Calculate HI (heatindex in °C) --> HI starts working above 26,7 °C
  if (measured_temp > 26.7)
  {
    double c1 = -8.784, c2 = 1.611, c3 = 2.338, c4 = -0.146, c5 = -1.230e-2, c6 = -1.642e-2, c7 = 2.211e-3, c8 = 7.254e-4, c9 = -2.582e-6;
    double T = measured_temp;
    double R = measured_humi;

    double A = ((c5 * T) + c2) * T + c1;
    double B = ((c7 * T) + c4) * T + c3;
    double C = ((c9 * T) + c8) * T + c6;
    HeatIndex = (C * R + B) * R + A;
  }
  else
  {
    HeatIndex = measured_temp;
  }
} // end of void measurementEvent()

void Test_LEDs()
{
  //Test the LEDs in RGB order
  fill_solid(leds, NUM_LEDS, CRGB(255, 0, 0));
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.show();
  Serial.println("");
  Serial.println("TEST:  Red");
  delay(1000);

  fill_solid(leds, NUM_LEDS, CRGB(0, 255, 0));
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.show();
  Serial.println("TEST:  Green");
  delay(1000);

  fill_solid(leds, NUM_LEDS, CRGB(0, 0, 255));
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.show();
  Serial.println("TEST:  Blue");
  Serial.println();
  delay(1000);

  fill_solid(leds, NUM_LEDS, CRGB(0, 0, 0));
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.show();
}



void SPIFFS_init()
{
  //*****************Checking if SPIFFS available********************************
  Serial.println("SPIFFS Initialization: (First time run can last up to 30 sec - be patient)");

  boolean mounted = SPIFFS.begin(); // load config if it exists. Otherwise use defaults.
  if (!mounted)
  {
    Serial.println("FS not formatted. Doing that now... (can last up to 30 sec).");
    SPIFFS.format();
    Serial.println("FS formatted...");
    Serial.println("");
    SPIFFS.begin();
  }
}

void UpdateSPIFFS()
{

  Serial.print("Timestamp difference: ");
  Serial.println(current_timestamp - saved_timestamp);

  if (current_timestamp - saved_timestamp > 21600)
  { // last save older than 6 hours -> re-initialize values
    Serial.print("Greater than 21600, doing FirstTimeRun  -  current_timestamp: ");
    Serial.print(current_timestamp);
    Serial.print(",   saved_timestamp: ");
    Serial.println(saved_timestamp);
    FirstTimeRun();
  }
  else if (current_timestamp - saved_timestamp > 1800)
  { // it is time for pressure update (1800 sec = 30 min)

    for (int i = 11; i >= 1; i = i - 1)
    {
      pressure_value[i] = pressure_value[i - 1]; // shifting values one to the right
    }

    pressure_value[0] = rel_pressure_rounded; // updating with acutal rel pressure (newest value)

    if (accuracy < 12)
    {
      accuracy = accuracy + 1; // one value more -> accuracy rises (up to 12 = 100%)
    }
    WriteToSPIFFS(current_timestamp); // update timestamp on storage
    Serial.println("writing current_timestamp");
  }
  else
  {
    WriteToSPIFFS(saved_timestamp); // do not update timestamp on storage
    Serial.println("writing saved_timestamp");
  }
}

void FirstTimeRun()
{
  Serial.println("---> Starting initializing process.");
  accuracy = 1;
  char filename[] = "/data.txt";
  File myDataFile = SPIFFS.open(filename, "w"); // Open a file for writing
  if (!myDataFile)
  {
    Serial.println("Failed to open file");
    Serial.println("Stopping process - maybe flash size not set (SPIFFS).");
    exit(0);
  }
  myDataFile.println(current_timestamp); // Saving timestamp to /data.txt
  Serial.print("*!* current_timestamp = ");
  Serial.println(current_timestamp);

  myDataFile.println(accuracy); // Saving accuracy value to /data.txt
  for (int i = 0; i < 12; i++)
  {
    myDataFile.println(rel_pressure_rounded); // Filling pressure array with current pressure
  }
  Serial.println("** Saved initial pressure data. **");
  myDataFile.close();
}

void ReadFromSPIFFS()
{
  char filename[] = "/data.txt";
  File myDataFile = SPIFFS.open(filename, "r"); // Open file for reading
  if (!myDataFile)
  {
    Serial.println("Failed to open file");
    FirstTimeRun(); // no file there -> initializing
  }

  Serial.println("---> Now reading from SPIFFS");

  String temp_data;

  temp_data = myDataFile.readStringUntil('\n');
  saved_timestamp = temp_data.toInt();
  Serial.print("Timestamp from SPIFFS: ");
  Serial.println(saved_timestamp);

  temp_data = myDataFile.readStringUntil('\n');
  accuracy = temp_data.toInt();
  Serial.print("Accuracy value read from SPIFFS: ");
  Serial.println(accuracy);

  Serial.print("Local time: ");
  Serial.print(hour);
  Serial.print(":");
  Serial.print(minute);

  Serial.print(",  Last 12 saved pressure values: ");
  for (int i = 0; i <= 11; i++)
  {
    temp_data = myDataFile.readStringUntil('\n');
    pressure_value[i] = temp_data.toInt();
    Serial.print(pressure_value[i]);
    Serial.print("; ");
  }
  myDataFile.close();
  Serial.println();
}

void WriteToSPIFFS(int write_timestamp)
{
  char filename[] = "/data.txt";
  File myDataFile = SPIFFS.open(filename, "w"); // Open file for writing (appending)
  if (!myDataFile)
  {
    Serial.println("Failed to open file");
  }

  Serial.println("---> Now writing to SPIFFS");

  myDataFile.println(write_timestamp); // Saving timestamp to /data.txt
  myDataFile.println(accuracy);        // Saving accuracy value to /data.txt

  for (int i = 0; i <= 11; i++)
  {
    myDataFile.println(pressure_value[i]); // Filling pressure array with updated values
  }
  myDataFile.close();

  Serial.println("File written. Now reading file again.");
  myDataFile = SPIFFS.open(filename, "r"); // Open file for reading
  Serial.print("Found in /data.txt = ");
  while (myDataFile.available())
  {
    Serial.print(myDataFile.readStringUntil('\n'));
    Serial.print("; ");
  }
  Serial.println();
  myDataFile.close();
}

void commute_flip_check(){

    //Handle the timing of clock vs commute display
    if (commute_flag == 0 && millis() - display_commute_every_millis > display_commute_every){
      if (verbose_output == 1)
      {
      Serial.println("");
      Serial.print(hour);
      Serial.print(":");
      Serial.print(minute);
      Serial.print(" - ");
      Serial.println(" Commute display start ***");
      Serial.println("");
      }
      secs_todisplay_commute_millis = millis();   //Start counter for how long cummute time displayed      
      commute_flag = 1;
      commute_flag_on = 0;
      force_time = 1;   //force time to show even if not a new minute/hour
    }

    if (commute_flag == 1 && millis() - secs_todisplay_commute_millis > secs_todisplay_commute){
      if (verbose_output == 1)
      {      
      Serial.println("");
      Serial.print(hour);
      Serial.print(":");
      Serial.print(minute);
      Serial.print(" - ");
      Serial.println(" Commute display end ***");
      Serial.println("");
      }
      commute_flag = 0;   //Commute time displayed enough, switch back
      display_commute_every_millis = millis();    //reset the counter between comute display modes
      weather_background_flag = 1;         //only run once every weather mode to fill in background
      }
    
        yield();

    //Override commute display, too long since last API pull or equal 0  (API not being pulled)
    if (millis() - last_API_pull > last_API_limit  || (numeral1 + numeral2 + numeral3) == 0 ){
      commute_flag = 0;   
      north_south_flag = 0;   //After timeout override any previous touch press for direction
    }
}

void speak_forecast(void *parameters)
{

        
        Zambretti_calc();

        //If accuracy less than 6hours then make no foreacast
        if (accuracy < accuracygate)
        {
          ZambrettisWords = TEXT_ZAMBRETTI_DEFAULT;
          Zambretti_mp3 = 126;
        }  
        
        const int _Zambretti_trend_mp3 = Zambretti_trend_mp3;
        const int _Zambretti_mp3 = Zambretti_mp3;

        Serial.println();
        Serial.println("*** Speak forecast ***");
        Serial.println();

        Serial.print("Zambretti says: ");
        Serial.print(ZambrettisWords);
        Serial.print(", ");
        Serial.println(trend_in_words);
        Serial.print("accuracy: ");
        Serial.println(accuracy);
        Serial.print("Prediction accuracy %: ");
        Serial.print(accuracy_in_percent);        
        Serial.println("%");
        Serial.print("Zambretti mp3 = ");
        Serial.println(_Zambretti_mp3);
        if (accuracy < 12)
        {
          Serial.println("Reason: Not enough weather data yet.");
          Serial.print("We need ");
          Serial.print((12 - accuracy) / 2);
          Serial.println(" hours more to get sufficient data.");
        }
        
        SpeakClock();
          delay(2000);

        myDFPlayer.playFolder(2, _Zambretti_trend_mp3); //only one of these will have a value
          delay(1000);
          yield();
        myDFPlayer.playFolder(2, _Zambretti_mp3);
          yield();
        delay(3000);
        vTaskDelete(NULL);
}


void doLEDs(){
  //Display LEDs for the mode selected
    if (commute_flag == 0){  

      if (weather_mode == "sun"  && accuracy >= 12){      //I have a forecast play animation
        sun_run();
      }

      if (weather_mode == "rain"  && accuracy >= 12){      //I have a forecast play animation
        rain_run();
      }

      //old hour/old minute checks needed otherwise it would constantly update the LEDs every loop, possible cause of flickering LEDs
      //force time flag is set when exiting the commute mode, possible the time hasn't changed but the time won't redisplay with out
      if ((force_time == 1 && accuracy < 12) || (accuracy < 12 && commute_flag == 0  &  (old_hour !=hour || old_minute !=minute))){   //no forecast, not a commute display and there is a new time
        background_clock();
        doclock();  
        FastLED.show();
        old_minute = minute;
        old_hour = hour;
        force_time = 0;     //reset the force_time once executed
      } 
    }

    //commute flag on is used to only display once, and not redisplay everytime in the loop, reset when existing the commute display mode
    if (commute_flag == 1 && commute_flag_on == 0){       //It's commute display time, and I have displayed it before in the cycle (commute_flag_on)
    background_commute();
    Commute_digits_LED();

    //South arrow
    if ((AMPM == "AM" && north_south_flag == 0) || north_south_flag == 2){
      leds[90] = CRGB(crd,cgd,cbd);
      leds[91] = CRGB(crd,cgd,cbd);
      leds[92] = CRGB(crd,cgd,cbd);
      leds[93] = CRGB(crd,cgd,cbd);
      leds[94] = CRGB(crd,cgd,cbd);
      leds[95] = CRGB(crd,cgd,cbd);
    }

    //North arrow
    if ((AMPM == "PM" && north_south_flag == 0) || north_south_flag == 1){
      leds[0] = CRGB(crd,cgd,cbd);
      leds[1] = CRGB(crd,cgd,cbd);
      leds[2] = CRGB(crd,cgd,cbd);
      leds[3] = CRGB(crd,cgd,cbd);      
      leds[4] = CRGB(crd,cgd,cbd);
      leds[5] = CRGB(crd,cgd,cbd);
    }      
    
    FastLED.show();
    commute_flag_on = 1;      //Set flag to show it's been displayed
    }
}

void google_API_hour_check_fetch(){

    int times_per_hour = API_pull_array[hour24][1];
    
    if (times_per_hour != 0){         //avoid divide by zero error
    float mins_elapse = 60 / times_per_hour;

    if (trunc(minute/mins_elapse) == minute/mins_elapse){
    //it's a whole number, means I am at the right minute  
    
    //set trigger for minute 
    API_trigger = 0;

    if (API_minute == 0){
    API_trigger = 1;
    API_minute = 1;

      // Serial.println();
      // Serial.print("******** API Trigger ******** Minute: ");
      // Serial.print(minute);
      // Serial.println(" ********");
      // Serial.println();
      }
    }
    else
    {
      API_minute = 0;
      API_trigger = 0;
    }
  }
  else
  {
      //times_per_hour = 0 so do nothing
      API_minute = 0;
      API_trigger = 0;
      }


    //Get commute time if passes weekend check and API triggered by current minute
    if ((show_weekend == 1 && API_trigger == 1) || (show_weekend == 0 && API_trigger == 1 && (day != 6 && day != 0))){
        google_API();      //Get new commute time
    }  

}


void rain_run()
{

  reset_clockdigits();

  if(weather_background_flag == 1){
    fill_solid(&(leds[0]), NUM_LEDS /*number of leds*/, CRGB(weather_RGB_R,weather_RGB_G,weather_RGB_B));
    weather_background_flag = 0;    //reset flag so only run's once
  }

  if(millis() - previousTime >= weather_time) {
    // Move bright spots downward
    for (int row = kMatrixHeight - 1; row >= 0; row--) {
      for (int col = 0; col < kMatrixWidth; col++) {
        if (leds[XY(col, row)] == CRGB(rain_RGB_R, rain_RGB_G, rain_RGB_B)) {
          leds[XY(col, row)] = CRGB(weather_RGB_R,weather_RGB_G,weather_RGB_B); // create trail
          if (row < kMatrixHeight - 1) leds[XY(col, row + 1)] = CRGB(rain_RGB_R, rain_RGB_G, rain_RGB_B);
        }
      }
    }
    
    // Fade all leds
    for(int i = 0; i < NUM_LEDS; i++) {
      if (leds[i].g != 255) leds[i].nscale8(255); // only fade trail
    }

    // Spawn new falling spots
    if (weather_density >= 0){    //set to -1 for no rain
    if (random8(weather_density) == 0) // lower number == more frequent spawns
    {
      int8_t spawnX = random8(kMatrixWidth);
      leds[XY(spawnX, 0)] = CRGB(rain_RGB_R, rain_RGB_G, rain_RGB_B);
    }
    }


    doclock();
    FastLED.show();
    previousTime = millis();
  }
  return;
}

void sun_run() {

  //If there is a new minute or a new hour, reset the background to wipe over the previous digits or they pile on top of each other
  //I got get fancy and reset just the previous digits with background colour CRGB(r,g,b)

  reset_clockdigits();

  // Set bottom row to highest index in palette (white)
  sun_setBottomRow(kMatrixHeight);
  sun_runPattern();
}

bool sun_runPattern() {
  do_sun();
  for (int y = 0; y < kMatrixHeight; y++){ 
    for (int x = 0; x < kMatrixWidth; x++) {
      int index = sunPixels[kMatrixWidth * y + x];
      // Index goes from 0 -> kMatrixHeight, palette goes from 0 -> 255 so need to scale it
      uint8_t indexScale = 255 / kMatrixHeight;
      leds[XY(x,y)] = ColorFromPalette(_currentPalette, constrain(index * indexScale, 0, 255), 255, LINEARBLEND);
    }
  }

  doclock();

  FastLED.show();
  return true;
}


void do_sun() {
  for(uint16_t x = 0 ; x < kMatrixWidth; x++) {
    for (uint16_t y = 1; y < kMatrixHeight; y++) {
      spread_sun(y * kMatrixWidth + x);
    }
  }
}

void spread_sun(uint16_t src) {
  if(sunPixels[src] == 0) {
    sunPixels[src - kMatrixWidth] = 0;
  } else {
    // Commented lines moves sun sideways as well as up, but doesn't look good on low res matrix:
    // int16_t dst = src - rand + 1;
    // sunPixels[dst - kMatrixWidth] = sunPixels[src] - random8(1);
    sunPixels[src - kMatrixWidth] = sunPixels[src] - random8(2);
  }
}

void sun_setBottomRow(uint16_t col) {
  for (uint16_t i = 0; i < kMatrixWidth; i++) {
    sunPixels[(kMatrixHeight - 1) * kMatrixWidth + i] = col;
  }
}

void SpeakClock()
{
  int hour_mp3 = 0;
  int minute_mp3 = 0;
  int minute_mp3b = 999;
  int AMPMmp3 = 50;

  //Hours mp3.  Ranges from 30 (00 midnight) to 42 (Twelve)
  hour_mp3 = 30 + hour;

  //Set to PM is 12pm or later
  if (hour24 >= 12)
  {
    AMPMmp3 = 51;
  }

  //Minute mp3.  Specific words for 00 (OClock), 01, 02, 03, 04, 05, 06, 07, 08, 09, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 30, 40, 50
  if (minute <= 19)
  {
    minute_mp3 = 100 + minute;
  }
  else
  {
    if (minute % 10 == 0)     //If it's MOD 0, this means it's at the top of the hour (O'Clock)
    {
      minute_mp3 = 100 + minute;
    }
    else
    {
      minute_mp3 = 100 + (minute - (minute % 10));
      minute_mp3b = 30 + (minute % 10);
    }
  }

      Serial.println();
      Serial.println("****************");
      Serial.print("Local time: ");
      Serial.print(hour);
      Serial.print(":");
      Serial.println(minute);
      Serial.print("Speaking mp3  Hour: ");
      Serial.print(hour_mp3);
      Serial.print(" / ");
      Serial.print(minute_mp3b);
      Serial.print(" AMPM: ");      
      Serial.println(AMPMmp3);
      Serial.println("****************");
      Serial.println();
      
  myDFPlayer.playFolder(1, hour_mp3);     //Play selected mp3 in folder mp3
  delay(1000);
 
  if (verbose_output == 1){
      Serial.print(",  Minute: ");
      Serial.print(1, minute_mp3);
  }

  myDFPlayer.playFolder(1, minute_mp3);     //Play selected mp3 in folder mp3
  delay(1000);
 
  if (verbose_output == 1){   
    Serial.print(" / ");
    Serial.print(minute_mp3b);
    Serial.print(" AMPM: ");
  }

  if (minute_mp3b != 999)
  {
    myDFPlayer.playFolder(1, minute_mp3b);
    delay(1000);
  }

  if (verbose_output == 1){
    Serial.println(AMPMmp3);
    Serial.println("****************");
    Serial.println();
    }
  
  myDFPlayer.playFolder(1, AMPMmp3); //Play selected mp3 in folder mp3

  delay(1000);

}

void touch_check(){

  int fail = 0;

    //Get rid of noise - has to be pressed for 100ms to pass
    if (touchRead(touchPin1) < touch_threshold){
        long delt = millis();
        while (millis() - delt < touch_bounce){
          if (touchRead(touchPin1) > touch_threshold){
            fail = 1;
            }
        }

    //if I get to here with fail = 0 then proceed, else break
    //Touch#1 - Short press: Google API pull south   Long press: Google API pull north
    //measure how long pressed for, if exceeds timeout then breakout
    if (fail == 0){
    delt = millis();

    while (touchRead(touchPin1) < touch_threshold){
      yield();
      if (millis()-delt > touch_timeout){
        break;
      }
    }

    int time_pressed = millis() - delt;

    //If greater than short press, and less than long press
    if (time_pressed > short_threshold_press && time_pressed < long_threshold_press){
        north_south_flag = 1;   //north
        google_API();      //Get new commute time

        //force into commute diplay mode
        secs_todisplay_commute_millis = millis();   //Start counter for how long cummute time displayed      
        commute_flag = 1;
        commute_flag_on = 0;
        force_time = 1;   //force time to show even if not a new minute/hour
        }
    
    //If greater than long press
    if (time_pressed > long_threshold_press){

        north_south_flag = 2;   //south
        google_API();      //Get new commute time

        //force into commute diplay mode
        secs_todisplay_commute_millis = millis();   //Start counter for how long cummute time displayed      
        commute_flag = 1;
        commute_flag_on = 0;
        force_time = 1;   //force time to show even if not a new minute/hour
        }    
       }
    }

  //Touch#2 - Short press: Speak clock & forecast   Long press:  UTC -1/0/+1 cycle
    fail = 0;

    //Get rid of noise - has to be pressed for 100ms to pass
    if (touchRead(touchPin2) < touch_threshold){
        long delt = millis();
        while (millis() - delt < touch_bounce){
          if (touchRead(touchPin2) > touch_threshold){
            fail = 1;
            }
        }

    //if I get to here with fail = 0 then proceed, else break
    //Touch#1 - Short press: Google API pull south   Long press: Google API pull north
    //measure how long pressed for, if exceeds timeout then breakout
    if (fail == 0){
    delt = millis();

    while (touchRead(touchPin2) < touch_threshold){
      yield();
      if (millis()-delt > touch_timeout){
        break;
      }
    }

    int time_pressed = millis() - delt;

    //If greater than short press, and less than long press
    if (time_pressed > short_threshold_press && time_pressed < long_threshold_press){
      
      Serial.println("Touch: Speak forecast");
      xTaskCreate(          //used to run as a seperate process int RTOS
          speak_forecast,
          "speakforecast",
          10000,
          NULL,
          0,
          &speakforecast);

          touch_flag = millis();
        }
    
    //If greater than long press
    if (time_pressed > long_threshold_press){

        Serial.println("Touch: UTC cycle");

        int DSTFlag=0;
        if (DST == -1){
          DST = 0;
          myDFPlayer.playFolder(2, 152);
          DSTFlag = 1; } else
        if (DST == 0 && DSTFlag ==0){
          DST = 1;
          myDFPlayer.playFolder(2, 154);
          DSTFlag = 1; } else
        if (DST == 1 && DSTFlag ==0){
          DST = -1;
          myDFPlayer.playFolder(2, 153);
          DSTFlag=1; }

        timeClient.setTimeOffset(gmt_offset + (DST * 3600));
        timeClient.forceUpdate();
        }    
       }
    }
}

void reset_clockdigits(){
  // If there is a new minute or a new hour, reset the background to wipe over the previous digits or they pile on top of each other
  // reset just the previous digits with background colour CRGB(r,g,b)

  if (old_minute != minute){
    int temp_minute = minute;
    int temp_r = r, temp_g = g, temp_b = b;
  
    minute = old_minute;
    r = rain_RGB_R;   //set old digits to backround colour
    g = rain_RGB_G;
    b = rain_RGB_B;

    doclock();
    FastLED.show();
    minute = temp_minute;
    old_minute = minute;

    r = temp_r;    //restore rgb
    g = temp_g;
    b = temp_b;
    }

  if (old_hour != hour){
    int temp_hour = hour;
    int temp_r = r, temp_g = g, temp_b = b;
  
    hour = old_hour;
    r = rain_RGB_R;   //set old digits to backround colour
    g = rain_RGB_G;
    b = rain_RGB_B;

    doclock();
    FastLED.show();
    hour = temp_hour;
    old_hour = hour;

    r = temp_r;    //restore rgb
    g = temp_g;
    b = temp_b;
    }
}

void chime_check(){
    
    if (hour_chime_flag == 0 && minute == 0 && hour_chime_array[hour24][1] != 0){
      hour_chime_flag = 1;    //flag to only play once at top of hour
      
      xTaskCreate(          //used to run as a seperate process int RTOS
          speak_chime,
          "speakchime",
          10000,
          NULL,
          0,
          &speakchime);
    }

    if (minute != 0){           //reset flag, no top of hour now
      hour_chime_flag = 0;
    }
}

void speak_chime(void *parameters)
{
    const int _Zambretti_trend_mp3 = Zambretti_trend_mp3;
    const int _Zambretti_mp3 = Zambretti_mp3;

    //hour with mp3 to play 100=weather, 101 = time, 102 = time & forefast, 001 = morepork, 002 = cuckoo, 003 = Spring, 004 - submarine hooter 
    if (hour_chime_array[hour24][1] < 100){
          myDFPlayer.playFolder(3, hour_chime_array[hour24][1]);     //Play selected mp3 in folder mp3
        delay(2000);
        }

    if (accuracy >= accuracygate)
    {
        if (hour_chime_array[hour24][1] == 100){
            myDFPlayer.playFolder(2, _Zambretti_trend_mp3); //only one of these will have a value
              delay(1000);
              yield();
            myDFPlayer.playFolder(2, _Zambretti_mp3);
              yield();
            delay(3000);     
        }

        if (hour_chime_array[hour24][1] == 101){
            SpeakClock();
              delay(1500);  
        }

        if (hour_chime_array[hour24][1] == 102){
            SpeakClock();
              delay(1500);        
            myDFPlayer.playFolder(2, _Zambretti_trend_mp3); //only one of these will have a value
              delay(1000);
              yield();
            myDFPlayer.playFolder(2, _Zambretti_mp3);
              yield();
            delay(3000);     
        }
    }

    if (accuracy < accuracygate && hour_chime_array[hour24][1] >= 100){   //Override - can't play forecast (not accurate) play 003.mp3 / spring
          myDFPlayer.playFolder(3, 3);     //Play selected mp3 in folder mp3
        delay(2000);
    }

    vTaskDelete(NULL);

}