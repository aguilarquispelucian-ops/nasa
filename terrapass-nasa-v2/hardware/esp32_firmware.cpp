/**
 * =====================================================================================
 * PROYECTO: TerraPass v2.0 - NASA Space Apps Challenge 2026 ("Bailando con los SARs")
 * AUTORES : Jhon Alberto Aguilar Quispe & Crisber Ambicho Reynoso
 * MODULO  : Estacion Terrena IoT de Calibracion y Validacion (Cal/Val Node - ESP32)
 * DESTINO : Supabase PostgreSQL + PostGIS + Realtime (Tabla: public.ground_truth)
 * =====================================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <DHTesp.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <math.h>

// -------------------------------------------------------------------------------------
// 1. CREDENCIALES DE RED Y SUPABASE REST API
// -------------------------------------------------------------------------------------
const char* WIFI_SSID     = "Wokwi-GUEST";
const char* WIFI_PASS     = "";

const char* SUPABASE_URL  = "https://ntbrfedcrblamjfujryf.supabase.co/rest/v1/ground_truth";
const char* SUPABASE_KEY  = "sb_publishable_AM2Rw2pH3qJjRyW9oZw1Nw_BD9xsYF_";

// -------------------------------------------------------------------------------------
// 2. DEFINICION DE PINES GPIO (CONECTADOS EN DIAGRAM.JSON)
// -------------------------------------------------------------------------------------
#define PIN_SOIL_ADC   34   // Entrada analogica ADC1_CH6: Sonda de humedad de suelo
#define PIN_DHT22      4    // Entrada digital OneWire: Sensor climatico DHT22
#define PIN_TRIG       5    // Salida digital: Trigger ultrasonico HC-SR04 (Nivel de agua)
#define PIN_ECHO       18   // Entrada digital: Echo ultrasonico HC-SR04
#define PIN_BTN_AOI    15   // Boton fisico (Pull-Up) para alternar AOI (per / ven / ctl)
#define PIN_LED_OK     2    // LED Verde: Confirmacion HTTP 201 en Supabase
#define PIN_LED_ALERT  19   // LED Rojo: Alerta de deformacion sismica o inundacion
#define I2C_SDA        21   // Bus I2C SDA (MPU6050 + Pantalla OLED SSD1306)
#define I2C_SCL        22   // Bus I2C SCL

#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64

// -------------------------------------------------------------------------------------
// 3. INSTANCIAS DE SENSORES Y VARIABLES DE ESTADO
// -------------------------------------------------------------------------------------
DHTesp dht;
Adafruit_MPU6050 mpu;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

bool mpuAvailable  = false;
bool oledAvailable = false;

struct AoiProfile {
  const char* aoi_id;
  const char* node_id;
  int cell_id;
  const char* label;
};

const AoiProfile AOI_LIST[3] = {
  { "per", "NODE-PER-01", 2, "PERU ARROZAL" },
  { "ven", "NODE-VEN-01", 1, "VENEZUELA SISMO" },
  { "ctl", "NODE-CTL-01", 4, "ATACAMA CONTROL" }
};

int currentAoiIdx          = 0; // Inicia en Peru (NODE-PER-01)
unsigned long lastSendTime = 0;
unsigned long lastBtnCheck = 0;
const unsigned long SEND_INTERVAL_MS = 5000;
int lastHttpCode           = 0;
unsigned long packetCount  = 0;

// Estructura de telemetria Cal/Val
struct GroundTruthData {
  float soilMoisturePct;
  float waterLevelCm;
  float dielectricEps;
  float ambientTempC;
  float airHumidityPct;
  float vibrationG;
  float tiltDeg;
  String localRegime;
};

// -------------------------------------------------------------------------------------
// 4. FUNCIONES FISICAS Y DE ADQUISICION DE SENSORES
// -------------------------------------------------------------------------------------

float computeToppDielectric(float moisturePct) {
  float theta = constrain(moisturePct / 100.0f, 0.01f, 0.98f);
  float eps = 3.03f + (9.3f * theta) + (146.0f * theta * theta) - (76.7f * theta * theta * theta);
  return constrain(eps, 3.0f, 80.0f);
}

float readUltrasonicWaterLevelCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  long durationUs = pulseIn(PIN_ECHO, HIGH, 30000);
  if (durationUs <= 0) {
    return 0.0f;
  }
  float distanceCm = (durationUs * 0.0343f) / 2.0f;
  if (distanceCm >= 100.0f) return 0.0f;
  return constrain(100.0f - distanceCm, 0.0f, 95.0f);
}

GroundTruthData sampleAllSensors() {
  GroundTruthData d;

  // A. Humedad Volumetrica de Suelo (ADC 12-bits: 0 - 4095)
  int rawAdc = analogRead(PIN_SOIL_ADC);
  d.soilMoisturePct = (rawAdc / 4095.0f) * 98.0f;

  // B. Nivel de lamina de agua (HC-SR04)
  d.waterLevelCm = readUltrasonicWaterLevelCm();

  // C. Constante Dielectrica Real para Radar Banda L (Ecuacion de Topp)
  d.dielectricEps = computeToppDielectric(d.soilMoisturePct);

  // D. Temperatura y Humedad Atmosferica (DHT22)
  TempAndHumidity th = dht.getTempAndHumidity();
  if (isnan(th.temperature) || isnan(th.humidity)) {
    d.ambientTempC   = 27.5f;
    d.airHumidityPct = 78.0f;
  } else {
    d.ambientTempC   = th.temperature;
    d.airHumidityPct = th.humidity;
  }

  // E. Acelerometro y Giroscopio Sismico (MPU6050)
  if (mpuAvailable) {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);

    float ax_g = a.acceleration.x / 9.80665f;
    float ay_g = a.acceleration.y / 9.80665f;
    float az_g = a.acceleration.z / 9.80665f;

    float totalG = sqrtf((ax_g * ax_g) + (ay_g * ay_g) + (az_g * az_g));
    d.vibrationG = fabs(totalG - 1.0f);

    float pitchRad = atan2f(ax_g, sqrtf((ay_g * ay_g) + (az_g * az_g)));
    float rollRad  = atan2f(ay_g, sqrtf((ax_g * ax_g) + (az_g * az_g)));
    d.tiltDeg      = sqrtf(sq(pitchRad * 57.2958f) + sq(rollRad * 57.2958f));
  } else {
    d.vibrationG = 0.008f;
    d.tiltDeg    = 0.05f;
  }

  // F. Reglas de Clasificacion en el Borde (Edge Triage Geofisico)
  if (d.vibrationG >= 0.150f || d.tiltDeg >= 1.50f) {
    d.localRegime = "DEFORMACION_SISMICA";
    digitalWrite(PIN_LED_ALERT, HIGH);
  } else if (d.waterLevelCm >= 15.0f && d.soilMoisturePct >= 75.0f) {
    d.localRegime = "LAMINA_AGUA_ARROZAL";
    digitalWrite(PIN_LED_ALERT, HIGH);
  } else if (d.soilMoisturePct >= 82.0f) {
    d.localRegime = "INUNDACION_BAJO_DOSEL";
    digitalWrite(PIN_LED_ALERT, HIGH);
  } else if (d.soilMoisturePct <= 15.0f && d.waterLevelCm < 1.0f) {
    d.localRegime = "HIPERARIDO_ESTABLE";
    digitalWrite(PIN_LED_ALERT, LOW);
  } else {
    d.localRegime = "ESTABLE_NOMINAL";
    digitalWrite(PIN_LED_ALERT, LOW);
  }

  return d;
}

void updateOledScreen(const GroundTruthData& d, int httpStatus) {
  if (!oledAvailable) return;

  const AoiProfile& prof = AOI_LIST[currentAoiIdx];
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("TERRAPASS ");
  display.print(prof.node_id);

  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  display.setCursor(0, 14);
  display.printf("Suelo: %.1f%% Eps:%.1f", d.soilMoisturePct, d.dielectricEps);

  display.setCursor(0, 25);
  display.printf("Agua : %.1fcm T:%.1fC", d.waterLevelCm, d.ambientTempC);

  display.setCursor(0, 36);
  display.printf("Sismo: %.3fg Tilt:%.1f", d.vibrationG, d.tiltDeg);

  display.setCursor(0, 47);
  display.print(d.localRegime.substring(0, 20));

  display.setCursor(0, 56);
  display.printf("SB HTTP:%d Pkt:#%lu", httpStatus, packetCount);

  display.display();
}

int sendToSupabase(const GroundTruthData& d) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WARN] WiFi desconectado. Intentando reconectar...");
    WiFi.reconnect();
    return -1;
  }

  const AoiProfile& prof = AOI_LIST[currentAoiIdx];

  StaticJsonDocument<512> doc;
  doc["node_id"]           = prof.node_id;
  doc["aoi_id"]            = prof.aoi_id;
  doc["cell_id"]           = prof.cell_id;
  doc["soil_moisture_pct"] = serialized(String(d.soilMoisturePct, 2));
  doc["water_level_cm"]    = serialized(String(d.waterLevelCm, 1));
  doc["dielectric_eps"]    = serialized(String(d.dielectricEps, 1));
  doc["ambient_temp_c"]    = serialized(String(d.ambientTempC, 2));
  doc["air_humidity_pct"]  = serialized(String(d.airHumidityPct, 2));
  doc["vibration_g"]       = serialized(String(d.vibrationG, 3));
  doc["tilt_deg"]          = serialized(String(d.tiltDeg, 2));
  doc["local_regime"]      = d.localRegime;

  String jsonPayload;
  serializeJson(doc, jsonPayload);

  digitalWrite(PIN_LED_OK, HIGH);

  HTTPClient http;
  http.begin(SUPABASE_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Prefer", "return=minimal");

  int httpResponseCode = http.POST(jsonPayload);
  String responseBody  = http.getString();
  http.end();

  digitalWrite(PIN_LED_OK, LOW);
  packetCount++;

  Serial.println("------------------------------------------------------------------");
  Serial.printf("[TX #%lu -> SUPABASE] Nodo: %s (%s) | HTTP Status: %d\n",
                packetCount, prof.node_id, prof.aoi_id, httpResponseCode);
  Serial.printf("  -> Humedad Suelo : %.2f %%  | Constante Dielectrica (eps_r): %.1f\n",
                d.soilMoisturePct, d.dielectricEps);
  Serial.printf("  -> Nivel de Agua : %.1f cm  | Clima: %.1f C / %.1f %% HR\n",
                d.waterLevelCm, d.ambientTempC, d.airHumidityPct);
  Serial.printf("  -> Vibracion IMU : %.3f g   | Inclinacion: %.2f deg\n",
                d.vibrationG, d.tiltDeg);
  Serial.printf("  -> Regimen Local : %s\n", d.localRegime.c_str());
  if (httpResponseCode >= 400) {
    Serial.println("  -> Detalle Error : " + responseBody);
  }

  return httpResponseCode;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_SOIL_ADC, INPUT);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_BTN_AOI, INPUT_PULLUP);
  pinMode(PIN_LED_OK, OUTPUT);
  pinMode(PIN_LED_ALERT, OUTPUT);

  digitalWrite(PIN_LED_OK, LOW);
  digitalWrite(PIN_LED_ALERT, LOW);

  Wire.begin(I2C_SDA, I2C_SCL);

  Serial.println("==================================================================");
  Serial.println(" TERRAPASS v2.0 - ESTACION TERRENA CAL/VAL (NISAR SAR + SUPABASE)");
  Serial.println(" Autores: Jhon Alberto Aguilar Quispe & Crisber Ambicho Reynoso");
  Serial.println("==================================================================");

  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    oledAvailable = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 10);
    display.println("TERRAPASS CAL/VAL");
    display.println("Iniciando sensores...");
    display.display();
    Serial.println("[OK] Pantalla OLED SSD1306 (128x64) inicializada en 0x3C.");
  }

  dht.setup(PIN_DHT22, DHTesp::DHT22);
  Serial.println("[OK] Sensor climatico DHT22 configurado en GPIO 4.");

  if (mpu.begin()) {
    mpuAvailable = true;
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    Serial.println("[OK] Acelerometro sismico MPU6050 inicializado en I2C (21, 22).");
  }

  Serial.print("[WIFI] Conectando a ");
  Serial.print(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(350);
    Serial.print(".");
  }
  Serial.println("\n[WIFI] Conectado exitosamente! IP Local: " + WiFi.localIP().toString());

  GroundTruthData firstSample = sampleAllSensors();
  lastHttpCode = sendToSupabase(firstSample);
  updateOledScreen(firstSample, lastHttpCode);
  lastSendTime = millis();
}

void loop() {
  unsigned long now = millis();

  if (now - lastBtnCheck > 250) {
    if (digitalRead(PIN_BTN_AOI) == LOW) {
      currentAoiIdx = (currentAoiIdx + 1) % 3;
      const AoiProfile& prof = AOI_LIST[currentAoiIdx];
      Serial.printf("\n>>> [CAMBIO DE ZONA AOI] Ahora transmitiendo como: %s (%s) <<<\n",
                    prof.node_id, prof.label);
      GroundTruthData immediateSample = sampleAllSensors();
      lastHttpCode = sendToSupabase(immediateSample);
      updateOledScreen(immediateSample, lastHttpCode);
      lastSendTime = now;
      lastBtnCheck = now + 400;
    }
  }

  if (now - lastSendTime >= SEND_INTERVAL_MS) {
    lastSendTime = now;
    GroundTruthData sample = sampleAllSensors();
    lastHttpCode = sendToSupabase(sample);
    updateOledScreen(sample, lastHttpCode);
  }
}