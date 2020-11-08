#include <Arduino.h>
#include <map>
#include <sstream>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP8266SAM.h>
#include <AudioOutputI2S.h>
#include <Adafruit_NeoPixel.h>


// Setup ---------------------------------

// Pins
#define LED_PIN D3
#define LED_COUNT 3
#define MOTOR_PIN D7

// Location

// BER
#define LOCATION_LAT "52.364598"
#define LOCATION_LON "13.471815"

// Fernsehturm
//#define LOCATION_LAT "52.520803"
//#define LOCATION_LON "13.40945"

// Flughafen Leipzig/Halle
//#define LOCATION_LAT "51.42399"
//#define LOCATION_LON "12.23635"

// London
//#define LOCATION_LAT "51.42"
//#define LOCATION_LON "0"

// NYC
//#define LOCATION_LAT "40.68"
//#define LOCATION_LON "-74.03"

// WiFi networks
std::map<std::string, std::string> networks = {
  {"**SSID**", "**PW**"},
  {"**SSID2**", "**PW**"},
};

// Settings
#define SEARCH_INTERVAL_S 15
// Radius can be: 1, 5, 10, 25, 100, 250.
#define SEARCH_RADIUS_NM "5"
#define CO2_KG_PER_FLIGHT_KM 12
// cries once for every N tons.
#define CRY_PER_CO2_TONS 5

// BER
#define MY_AIRPORT "EDDB"

// API
#define RAPIDAPI_KEY "*****"
#define ADSBEXCHANGE_API_HOST "adsbexchange-com1.p.rapidapi.com"
#define AERODATABOX_API_HOST "aerodatabox.p.rapidapi.com"

#define DEBUG_JSON false

// ---------------------------------------

std::map<std::string, std::vector<std::string>> randomText = {
  {"intro", {
    "Oh no.",
    "Oh shit.",
    "Bummer.",
    "Damn.",
  }},
  {"intro2", {
    "There is another",
    "I can see a",
  }},
};

template<typename T>
std::string toString(const T &value) {
  std::ostringstream oss;
  oss << value;
  return oss.str();
}

ESP8266WiFiMulti wifiMulti;
BearSSL::WiFiClientSecure client;
HTTPClient http;
unsigned long lastSearchTime = 0;
Adafruit_NeoPixel leds(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
AudioOutputI2S *out = nullptr;
ESP8266SAM *sam = new ESP8266SAM;


struct aircraft {
  std::string icao, reg, type, call, opicao;
  float dst{};
  std::string from, from_icao, to, to_icao, model, airline;
  bool isCargo{};
  uint16_t alt{}, flightDistanceKm{}, co2{};
};

std::vector<std::string> seenAircraftIcaos;


void speak(const std::vector<std::string>& textParts);
std::vector<std::string> getAircraftTextParts(aircraft &ac);
void lookForAircraft();
aircraft loadNearestAircraft();
void loadFlightData(aircraft &ac);
void loadDistanceData(aircraft &ac);
bool seenBefore(const std::string& icao);
std::string getRandomText(const std::string& group);
void ensureWiFiConnection();


void setup() {
  Serial.begin(115200);
  Serial.println("\r\nHi.");

  // Pins.
  pinMode(MOTOR_PIN, OUTPUT);

  // LEDs.
  leds.begin();
  leds.clear();
  leds.show();

  // Set up audio.
  out = new AudioOutputI2S();
  out->begin();

  // Sam.
  //sam->SetPitch(150);
  //sam->SetSpeed(170);

  // Set up WiFi.
  WiFi.mode(WIFI_STA);
  for (auto& n : networks) {
    wifiMulti.addAP(n.first.c_str(), n.second.c_str());
  }

  // Avoid HTTPS certificate trouble.
  client.setInsecure();

  // Say Hi.
  digitalWrite(MOTOR_PIN, HIGH);
  sam->Say(out, "I am the Eis-BER!");
  digitalWrite(MOTOR_PIN, LOW);

  lastSearchTime = millis() - (SEARCH_INTERVAL_S * 1000);
}


void loop() {
  if ((millis() - lastSearchTime) < (SEARCH_INTERVAL_S * 1000)) {
    delay(100);
    return;
  }

  ensureWiFiConnection();
  lookForAircraft();
  lastSearchTime = millis();
}


/**
 * Say something and move motor.
 */
void speak(const std::vector<std::string>& textParts) {
  digitalWrite(MOTOR_PIN, HIGH);

  leds.setPixelColor(1, 0, 64, 0, 64);
  leds.setPixelColor(2, 255, 0, 255);
  leds.setPixelColor(3, 0, 64, 0, 64);
  leds.show();

  for (auto &text : textParts) {
    Serial.printf("---> \"%s\"\r\n", text.c_str());
    sam->Say(out, text.c_str());
  }

  digitalWrite(MOTOR_PIN, LOW);

  leds.clear();
  leds.show();
}


/**
 * Get spoken text for aircraft.
 */
std::vector<std::string> getAircraftTextParts(aircraft &ac) {
  std::vector<std::string> parts;

  std::string t1;
  t1.append(getRandomText("intro")).append(" ");
  t1.append(getRandomText("intro2")).append(" ");

  if (!ac.airline.empty()) {
    t1.append(ac.airline).append(" ");
  }
  if (ac.isCargo) {
    t1.append("cargo ");
  }
  t1.append(!ac.model.empty() ? ac.model : "plane").append(" ");
  parts.push_back(t1);

  std::string t2;
  if (!ac.from.empty() || !ac.to.empty()) {
    if (ac.from_icao == MY_AIRPORT) {
      t2.append("starting to ").append(!ac.to.empty() ? ac.to : "someplace").append(" ");
    }
    else if (ac.to_icao == MY_AIRPORT) {
      t2.append("arriving from ").append(!ac.from.empty() ? ac.from : "somewhere").append(" ");
    }
    else {
      t2.append("passing by ");
      if (ac.alt > 1000) {
        t2.append("at ")
         .append(toString((int) round((ac.alt * 0.3048) / 1000))) // feet to km
         .append(" kilometers ");
      }
      t2.append("on its way ");
      if (!ac.from.empty()) {
        t2.append("from ").append(ac.from).append(" ");
      }
      if (!ac.to.empty()) {
        t2.append("to ").append(ac.to).append(" ");
      }
    }
  }
  parts.push_back(t2);

  std::string t3;
  if (ac.co2) {
    t3.append("and it produces another ").append(toString(ac.co2))
      .append(" tons of carbon dioxide ");
  }
  t3.append(". ");
  parts.push_back(t3);

  std::string t4;
  int cries = ac.co2 ? ac.co2 / CRY_PER_CO2_TONS : (int) random(2, 5);
  for (int i=0; i<cries; i++) {
    t4.append("ii ");
  }
  parts.push_back(t4);

  return parts;
}


/**
 * Looks for new airplanes, speaks them out.
 */
void lookForAircraft() {
  Serial.println("--------------------------------------------------");
  aircraft ac = loadNearestAircraft();
  if (ac.icao.empty() || seenBefore(ac.icao)) {
    Serial.println("No new aircraft found.");
    return;
  }
  seenAircraftIcaos.push_back(ac.icao);

  // Load as much data as possible.
  loadFlightData(ac);
  if (!ac.flightDistanceKm && !ac.from_icao.empty() && !ac.to_icao.empty()) {
    loadDistanceData(ac);
  }

  // Calculate CO2 tons.
  if (ac.flightDistanceKm) {
    ac.co2 = (ac.flightDistanceKm * CO2_KG_PER_FLIGHT_KM) / 1000;
  }

  // Simplify model name.
  if (!ac.model.empty()) {
    ac.model = ac.model.substr(0, ac.model.find_first_of("-/"));
  }

  Serial.printf(
    "New aircraft: %s icao:%s type:%s alt:%d dst:%.2f op:%s call:%s %s -> %s (%d km, %dt CO2)\r\n",
    ac.reg.c_str(),
    ac.icao.c_str(),
    ac.type.c_str(),
    ac.alt,
    ac.dst,
    ac.opicao.c_str(),
    ac.call.c_str(),
    ac.from.c_str(),
    ac.to.c_str(),
    ac.flightDistanceKm,
    ac.co2
  );

  speak(getAircraftTextParts(ac));
}


/**
 * Load nearest aircraft (adsbexchange).
 *
 * @return aircraft
 */
aircraft loadNearestAircraft() {
  Serial.println("Loading all nearby aircraft ...");

  aircraft nearestAc;

  std::string acListApiURL =
    "https://" + std::string(ADSBEXCHANGE_API_HOST)
    + std::string("/json/lat/") + std::string(LOCATION_LAT)
    + std::string("/lon/") + std::string(LOCATION_LON)
    + std::string("/dist/") + std::string(SEARCH_RADIUS_NM)
    + std::string("/");
  Serial.println(acListApiURL.c_str());
  http.useHTTP10(true);
  http.setTimeout(10 * 1000);
  http.begin(client, acListApiURL.c_str());
  http.addHeader("x-rapidapi-host", ADSBEXCHANGE_API_HOST);
  http.addHeader("x-rapidapi-key", RAPIDAPI_KEY);
  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf("An HTTP Error occured: %d %s\r\n", httpCode, HTTPClient::errorToString(httpCode).c_str());
    http.end();
    return nearestAc;
  }

  // Define filter.
  StaticJsonDocument<512> filter;
  filter["total"] = true;
  filter["ac"][0]["icao"] = true;
  filter["ac"][0]["reg"] = true;
  filter["ac"][0]["type"] = true;
  filter["ac"][0]["spd"] = true;
  filter["ac"][0]["alt"] = true;
  filter["ac"][0]["call"] = true;
  filter["ac"][0]["opicao"] = true;
  filter["ac"][0]["dst"] = true;

  // Parse JSON response.
  DynamicJsonDocument jsonDoc(4096);
  DeserializationError dsError = deserializeJson(jsonDoc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (dsError) {
    Serial.printf("deserializeJson() failed: %s\r\n", dsError.c_str());
    return nearestAc;
  }
#if DEBUG_JSON
  serializeJson(jsonDoc, Serial);
  Serial.println();
#endif

  // Collect data.
  uint16_t count = jsonDoc["total"].as<uint16_t>();
  Serial.printf("%d aircraft found.\r\n", count);
  if (!count) {
    return nearestAc;
  }
  std::vector<aircraft> allAcs;
  for (unsigned int i = 0; i < count; i++) {
    aircraft ac;
    JsonObject data = jsonDoc["ac"][i].as<JsonObject>();
    Serial.printf("  %d: %s icao:%s type:%s spd:%.2f alt:%d dst:%.2f ground:%d\r\n",
                  i + 1,
                  data["reg"].as<char *>(),
                  data["icao"].as<char *>(),
                  data["type"].as<char *>(),
                  data["spd"].as<float>(),
                  data["alt"].as<uint16_t>(),
                  data["dst"].as<float>(),
                  data["gnd"].as<int>()
    );

    // Skip aircraft on the ground.
    if (data["gnd"].as<int>() > 0) {
      continue;
    }
    if (data["spd"].as<float>() < 50) {
      continue;
    }

    ac.icao     = data["icao"].as<char*>();
    ac.reg      = data["reg"].as<char*>();
    ac.type     = data["type"].as<char*>();
    ac.alt      = data["alt"].as<uint16_t>();
    ac.call     = data["call"].as<char*>();
    ac.opicao   = data["opicao"].as<char*>();
    ac.dst      = data["dst"].as<uint16_t>();

    // Skip aircraft without reg.
    if (ac.reg.empty()) {
      continue;
    }

    allAcs.push_back(ac);
  }

  // Find nearest.
  for (auto& ac : allAcs) {
    if (nearestAc.dst == 0 || ac.dst < nearestAc.dst) {
      nearestAc = ac;
    }
  }

  return nearestAc;
}


/**
 * Load flight data (AeroDataBox).
 */
void loadFlightData(aircraft &ac) {
  Serial.printf("%s: getting flight data ...\r\n", ac.reg.c_str());

  std::string flInfoApiURL =
    "https://" + std::string(AERODATABOX_API_HOST)
    + std::string("/flights/callsign/") + ac.call;
  Serial.println(flInfoApiURL.c_str());
  http.useHTTP10(true);
  http.setTimeout(10 * 1000);
  http.begin(client, flInfoApiURL.c_str());
  http.addHeader("x-rapidapi-host", AERODATABOX_API_HOST);
  http.addHeader("x-rapidapi-key", RAPIDAPI_KEY);
  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf("An HTTP Error occured: %d %s\r\n", httpCode, HTTPClient::errorToString(httpCode).c_str());
    http.end();
    return;
  }

  // Define filter.
  StaticJsonDocument<512> filter;
  filter[0]["greatCircleDistance"]["km"];
  filter[0]["departure"]["airport"]["municipalityName"] = true;
  filter[0]["departure"]["airport"]["icao"] = true;
  filter[0]["arrival"]["airport"]["municipalityName"] = true;
  filter[0]["arrival"]["airport"]["icao"] = true;
  filter[0]["isCargo"] = true;
  filter[0]["aircraft"]["model"] = true;
  filter[0]["airline"]["name"] = true;

  // Parse JSON response.
  DynamicJsonDocument jsonDoc(2048);
  DeserializationError dsError = deserializeJson(jsonDoc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (dsError) {
    Serial.printf("deserializeJson() failed: %s\r\n", dsError.c_str());
    return;
  }
#if DEBUG_JSON
  serializeJson(jsonDoc, Serial);
  Serial.println();
#endif

  // Collect data.
  if (jsonDoc.isNull()) {
    Serial.printf("%s: no flight data found.\r\n", ac.reg.c_str());
    return;
  }
  // Always use first dataset.
  JsonObject data = jsonDoc[0].as<JsonObject>();
  if (!data["greatCircleDistance"].isNull()) {
    ac.flightDistanceKm = data["greatCircleDistance"]["km"].as<float>();
  }
  if (!data["departure"].isNull() && !data["departure"]["airport"].isNull()) {
    ac.from = !data["departure"]["airport"]["municipalityName"].isNull()
             ? data["departure"]["airport"]["municipalityName"].as<char *>() : "";
    ac.from_icao = !data["departure"]["airport"]["icao"].isNull()
                  ? data["departure"]["airport"]["icao"].as<char *>() : "";
  }
  if (!data["arrival"].isNull() && !data["arrival"]["airport"].isNull()) {
    ac.to = !data["arrival"]["airport"]["municipalityName"].isNull()
           ? data["arrival"]["airport"]["municipalityName"].as<char *>() : "";
    ac.to_icao = !data["arrival"]["airport"]["icao"].isNull()
                ? data["arrival"]["airport"]["icao"].as<char *>() : "";
  }
  if (!data["aircraft"].isNull() && !data["aircraft"]["model"].isNull()) {
    ac.model = data["aircraft"]["model"].as<char *>();
  }
  if (!data["airline"].isNull() && !data["airline"]["name"].isNull()) {
    ac.airline = data["airline"]["name"].as<char *>();
  }
  if (!data["isCargo"].isNull()) {
    ac.isCargo = data["isCargo"].as<bool>();
  }
}


/**
 * Load flight distance data (AeroDataBox).
 */
void loadDistanceData(aircraft &ac) {
  if (ac.from_icao.empty() || ac.to_icao.empty()) {
    return;
  }
  Serial.printf("%s: getting flight distance ...\r\n", ac.reg.c_str());

  std::string flDistanceApiURL =
    "https://" + std::string(AERODATABOX_API_HOST)
    + std::string("/airports/icao/") + ac.from_icao
    + std::string("/distance-time/") + ac.to_icao;
  Serial.println(flDistanceApiURL.c_str());
  http.useHTTP10(true);
  http.setTimeout(10 * 1000);
  http.begin(client, flDistanceApiURL.c_str());
  http.addHeader("x-rapidapi-host", AERODATABOX_API_HOST);
  http.addHeader("x-rapidapi-key", RAPIDAPI_KEY);
  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.printf("An HTTP Error occured: %d %s\r\n", httpCode, HTTPClient::errorToString(httpCode).c_str());
    http.end();
    return;
  }

  // Define filter.
  StaticJsonDocument<64> filter;
  filter["greatCircleDistance"]["km"] = true;

  // Parse JSON response.
  DynamicJsonDocument jsonDoc(1024);
  DeserializationError dsError = deserializeJson(jsonDoc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (dsError) {
    Serial.printf("deserializeJson() failed: %s\r\n", dsError.c_str());
    return;
  }
#if DEBUG_JSON
  serializeJson(jsonDoc, Serial);
  Serial.println();
#endif

  // Collect data.
  if (jsonDoc.isNull()) {
    Serial.printf("%s: no flight data found.\r\n", ac.reg.c_str());
    return;
  }
  if (!jsonDoc["greatCircleDistance"].isNull()) {
    ac.flightDistanceKm = jsonDoc["greatCircleDistance"]["km"].as<float>();
  }
}


/**
 * Helper.
 *
 * @param ac aircraft
 * @return seen before or not
 */
bool seenBefore(const std::string& icao) {
  return (std::find(seenAircraftIcaos.begin(), seenAircraftIcaos.end(), icao) != seenAircraftIcaos.end());
}


/**
 * Get random text, see randomText definition.
 * @param group
 * @return
 */
std::string getRandomText(const std::string& group) {
  if (randomText.at(group).empty()) {
    return "";
  }
  int size = randomText.at(group).size();
  return randomText.at(group)[random(size)];
}


/**
 * Ensures WiFi is connected.
 */
void ensureWiFiConnection() {
  if (wifiMulti.run() != WL_CONNECTED) {
    Serial.print("Connecting WiFi .");
    while (wifiMulti.run() != WL_CONNECTED) {
      Serial.print(".");
      delay(500);
    }
    Serial.printf("\r\nWiFi connected (%s).\r\n", WiFi.localIP().toString().c_str());
  }
}
