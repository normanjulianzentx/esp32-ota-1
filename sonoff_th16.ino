/*
 * ====================================================================
 * FIRMWARE CONTROL ZENTX - IDENTIDAD MAC, BALIZA WIFI & CLOUD SYNC (v3.5)
 * ====================================================================
 * Novedades v3.5:
 * 1. Identidad de Hardware por Dirección MAC Única (Clave Primaria Inmutable).
 * 2. Baliza WiFi Inteligente (Beacon AP): Transmite SSID con ID Corto e IP:
 *    Ejemplo: ZenTX-A66E-192-168-100-38 (Visible en cualquier celular sin conectarse).
 * 3. Activación de Baliza Automática al arrancar (90s) y bajo demanda (pulsar botón 2.5s-5s).
 * 4. Notificación Cloud a Google Sheets / Telegram (Envío de MAC, IP, SSID y telemetría).
 * 5. Soporte Nativo NetBIOS y Hostname DHCP (http://zentx-control y http://zentx-[ID]).
 * 6. Portal Cautivo Inteligente para Baliza y Alta WiFi.
 * 7. Carga Directa de Firmware por Web UI (/firmware) y Remota por Nube (/update-remote).
 * 8. API Endpoint JSON (/status y /json) con MAC completa e ID corto.
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266NetBIOS.h>
#include <DNSServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <EEPROM.h>

// ===== ESTRUCTURA DE CONFIGURACIÓN PERSISTENTE EEPROM =====
struct Config {
  char wifiSsid[32];   // Nombre de la red WiFi
  char wifiPass[64];   // Contraseña WiFi
  char deviceId[32];   // Nombre comercial del equipo
  float setpointTemp;  // Temperatura objetivo (°C)
  float hysteresis;    // Histéresis (±°C)
  char cloudUrl[160];  // URL de Notificación Cloud (Google Script)
  uint32_t magicNumber;
} config;

#define EEPROM_MAGIC 0x5A3E3005
#define FIRMWARE_VERSION "v3.5"

// ===== ASIGNACIÓN DE PINES HARDWARE ZENTX (SONOFF TH16) =====
#define PIN_RELAY  12  // Relé de potencia (GPIO 12)
#define PIN_LED    13  // LED Azul de estado (GPIO 13 - Activo en LOW)
#define PIN_BUTTON 0   // Botón táctil frontal (GPIO 0)
#define PIN_JACK14 14  // Pin de datos sonda 1-Wire principal (GPIO 14)
#define PIN_JACK4  4   // Pin de datos sonda alternativo (GPIO 4)

OneWire oneWire14(PIN_JACK14);
DallasTemperature ds14(&oneWire14);

OneWire oneWire4(PIN_JACK4);
DallasTemperature ds4(&oneWire4);

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
DNSServer dnsServer;
const byte DNS_PORT = 53;

// ===== VARIABLES DE ESTADO Y MEMORIA RAM =====
String wifiSsid        = "";
String wifiPass        = "";
String deviceId        = "ZenTX_Control";
String cloudUrl        = "https://script.google.com/macros/s/AKfycbwI8YLmUxAY0skVKsqt7EGTUwYBo07p4eQgwwr7wD3a5o-oPJI5wZJCRNZ4RI7y061g/exec";

float setpointTemp     = 4.0;   // Temp. objetivo (°C)
float hysteresis       = 1.5;   // Histéresis (±°C)
bool autoMode          = true;  // Modo Automático (Termostato)

bool relayState        = false;
float temperatureC     = -999.0;
bool sensorError       = true;
bool isAPMode          = false;
String sensorPinStr    = "Sin detectar";

// Baliza WiFi (Beacon AP)
bool beaconActive               = false;
unsigned long beaconEndTime     = 0;
const unsigned long BEACON_TIME = 90000; // 90 segundos iniciales

// Timers para reconexión y parpadeo LED
unsigned long lastWifiCheckTime = 0;
unsigned long lastLedToggleTime = 0;

// ===== FUNCIONES DE IDENTIDAD MAC =====
String getMacAddress() {
  return WiFi.macAddress();
}

String getShortMac() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toUpperCase();
  if (mac.length() >= 4) {
    return mac.substring(mac.length() - 4);
  }
  return mac;
}

// ===== GESTIÓN DE CONFIGURACIÓN EEPROM =====
void loadConfig() {
  EEPROM.get(0, config);
  if (config.magicNumber == EEPROM_MAGIC) {
    wifiSsid     = String(config.wifiSsid);
    wifiPass     = String(config.wifiPass);
    deviceId     = String(config.deviceId);
    setpointTemp = config.setpointTemp;
    hysteresis   = config.hysteresis;
    cloudUrl     = String(config.cloudUrl);

    if (deviceId.length() == 0) deviceId = "ZenTX_Control";
    if (setpointTemp < -40.0 || setpointTemp > 100.0) setpointTemp = 4.0;
    if (hysteresis < 0.1 || hysteresis > 10.0) hysteresis = 1.5;
    if (cloudUrl.length() == 0) {
      cloudUrl = "https://script.google.com/macros/s/AKfycbwI8YLmUxAY0skVKsqt7EGTUwYBo07p4eQgwwr7wD3a5o-oPJI5wZJCRNZ4RI7y061g/exec";
    }

    Serial.println(F("[EEPROM] Configuración cargada con éxito."));
  } else {
    Serial.println(F("[EEPROM] Inicializando memoria con valores por defecto v3.5..."));
    wifiSsid     = "";
    wifiPass     = "";
    deviceId     = "ZenTX_Control";
    setpointTemp = 4.0;
    hysteresis   = 1.5;
    cloudUrl     = "https://script.google.com/macros/s/AKfycbwI8YLmUxAY0skVKsqt7EGTUwYBo07p4eQgwwr7wD3a5o-oPJI5wZJCRNZ4RI7y061g/exec";
    saveConfig();
  }
}

void saveConfig() {
  config.magicNumber = EEPROM_MAGIC;
  wifiSsid.toCharArray(config.wifiSsid, sizeof(config.wifiSsid));
  wifiPass.toCharArray(config.wifiPass, sizeof(config.wifiPass));
  deviceId.toCharArray(config.deviceId, sizeof(config.deviceId));
  config.setpointTemp = setpointTemp;
  config.hysteresis   = hysteresis;
  cloudUrl.toCharArray(config.cloudUrl, sizeof(config.cloudUrl));

  EEPROM.put(0, config);
  EEPROM.commit();
  Serial.println(F("[EEPROM] Guardado completado."));
}

// ===== LECTURA DE SONDA DS18B20 EN GPIO 14 / GPIO 4 =====
void readProbe() {
  pinMode(PIN_JACK14, INPUT_PULLUP);
  ds14.begin();
  ds14.requestTemperatures();
  float t14 = ds14.getTempCByIndex(0);

  if (t14 > -55.0 && t14 < 125.0 && t14 != DEVICE_DISCONNECTED_C && t14 != 85.0) {
    temperatureC = t14;
    sensorError = false;
    sensorPinStr = "DS18B20 (GPIO 14)";
    return;
  }

  pinMode(PIN_JACK4, INPUT_PULLUP);
  ds4.begin();
  ds4.requestTemperatures();
  float t4 = ds4.getTempCByIndex(0);

  if (t4 > -55.0 && t4 < 125.0 && t4 != DEVICE_DISCONNECTED_C && t4 != 85.0) {
    temperatureC = t4;
    sensorError = false;
    sensorPinStr = "DS18B20 (GPIO 4)";
    return;
  }

  sensorError = true;
  sensorPinStr = "⚠️ Sonda Desconectada";
}

// ===== LÓGICA DE CONTROL TERMOSTÁTICO =====
void checkThermostat() {
  if (sensorError || !autoMode) return;

  float upperLimit = setpointTemp + hysteresis;
  float lowerLimit = setpointTemp - hysteresis;

  if (temperatureC >= upperLimit && !relayState) {
    relayState = true;
    digitalWrite(PIN_RELAY, HIGH);
    Serial.println(F("[TERMOSTATO] Umbral Superior -> RELÉ ENCENDIDO"));
  } else if (temperatureC <= lowerLimit && relayState) {
    relayState = false;
    digitalWrite(PIN_RELAY, LOW);
    Serial.println(F("[TERMOSTATO] Umbral Inferior -> RELÉ APAGADO"));
  }
}

// ===== CÁLCULO DE PORCENTAJE DE SEÑAL RSSI =====
int getQuality(int rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
}

// ===== BALIZA WIFI DE RESCATE (BEACON AP) =====
void startBeaconAP(unsigned long durationMs = 90000) {
  if (WiFi.status() != WL_CONNECTED) return;

  String ipStr = WiFi.localIP().toString();
  ipStr.replace(".", "-");

  // Formato SSID: ZenTX-[SHORT_MAC]-[IP_GUIONES] (ej: ZenTX-A66E-192-168-100-38)
  String beaconSsid = "ZenTX-" + getShortMac() + "-" + ipStr;
  if (beaconSsid.length() > 31) {
    beaconSsid = beaconSsid.substring(0, 31);
  }

  WiFi.mode(WIFI_AP_STA);
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(beaconSsid.c_str());

  dnsServer.start(DNS_PORT, "*", apIP);
  beaconActive = true;
  beaconEndTime = millis() + durationMs;

  Serial.print(F("\n[BALIZA] 📶 Baliza WiFi Activa: "));
  Serial.println(beaconSsid);
  Serial.printf("[BALIZA] Tiempo activo: %lu segundos\n", durationMs / 1000);
}

void stopBeaconAP() {
  if (beaconActive) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    beaconActive = false;
    Serial.println(F("[BALIZA] 📴 Baliza WiFi Apagada (Tiempo cumplido)."));
  }
}

// ===== NOTIFICACIÓN A LA NUBE (GOOGLE SHEETS / CLOUD) =====
void sendCloudNotification(String eventType) {
  if (cloudUrl.length() == 0 || WiFi.status() != WL_CONNECTED) return;

  Serial.print(F("[NUBE] Enviando reporte a Google Apps Script (Evento: "));
  Serial.print(eventType);
  Serial.println(F(")..."));

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();
  HTTPClient https;
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  https.setTimeout(6000);

  String fullUrl = cloudUrl;
  fullUrl += (fullUrl.indexOf('?') >= 0) ? "&" : "?";
  fullUrl += "mac=" + getMacAddress();
  fullUrl += "&shortMac=" + getShortMac();
  fullUrl += "&device=" + deviceId;
  fullUrl += "&ip=" + WiFi.localIP().toString();
  fullUrl += "&ssid=" + wifiSsid;
  fullUrl += "&event=" + eventType;
  fullUrl += "&ver=" + String(FIRMWARE_VERSION);
  fullUrl += "&temp=" + (sensorError ? "null" : String(temperatureC, 2));
  fullUrl += "&relay=" + String(relayState ? "1" : "0");
  fullUrl += "&sensorError=" + String(sensorError ? "1" : "0");
  fullUrl += "&setpoint=" + String(setpointTemp, 1);
  fullUrl += "&hysteresis=" + String(hysteresis, 1);

  if (https.begin(*client, fullUrl)) {
    int httpCode = https.GET();
    if (httpCode > 0) {
      Serial.printf("[NUBE] ✅ Respuesta Servidor HTTP: %d\n", httpCode);
    } else {
      Serial.printf("[NUBE] ⚠️ Error HTTPS: %s\n", https.errorToString(httpCode).c_str());
    }
    https.end();
  } else {
    Serial.println(F("[NUBE] ⚠️ Fallo al inicializar conexion HTTPS"));
  }
}

// ===== PÁGINA DEL PORTAL CAUTIVO / BALIZA =====
void handleAPPortal() {
  if (beaconActive) {
    // Pantalla informativa de la Baliza de Rescate
    String html = "<!DOCTYPE html><html lang='es'><head><meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>Baliza ZenTX - " + getShortMac() + "</title>";
    html += "<style>";
    html += "* { box-sizing: border-box; font-family: 'Segoe UI', system-ui, sans-serif; }";
    html += "body { background: #0f172a; color: #f8fafc; text-align: center; margin: 0; padding: 1.5rem; }";
    html += ".card { background: #1e293b; padding: 2rem; border-radius: 1.25rem; max-width: 440px; margin: 0 auto; border: 1px solid #334155; box-shadow: 0 20px 40px rgba(0,0,0,0.5); }";
    html += "h1 { color: #38bdf8; font-size: 1.4rem; margin-top: 0; margin-bottom: 0.3rem; }";
    html += ".ip-box { background: #0284c7; color: white; padding: 1.2rem; border-radius: 1rem; margin: 1.5rem 0; font-size: 1.8rem; font-weight: bold; letter-spacing: 0.05em; }";
    html += ".info { color: #94a3b8; font-size: 0.9rem; text-align: left; background: #0f172a; padding: 1rem; border-radius: 0.75rem; margin-bottom: 1.2rem; border: 1px solid #334155; }";
    html += ".btn { display: block; width: 100%; padding: 0.9rem; border: none; border-radius: 0.75rem; font-size: 1rem; font-weight: bold; background: #22c55e; color: white; cursor: pointer; margin-top: 0.8rem; text-decoration: none; }";
    html += ".btn-sec { background: #334155; font-size: 0.9rem; }";
    html += "</style></head><body>";
    html += "<div class='card'>";
    html += "<h1>📶 Baliza de Rescate ZenTX</h1>";
    html += "<p style='color:#94a3b8; font-size:0.88rem;'>El equipo está conectado exitosamente a la red local.</p>";
    html += "<div class='ip-box'>" + WiFi.localIP().toString() + "</div>";
    html += "<div class='info'>";
    html += "• <strong>Equipo:</strong> " + deviceId + "<br>";
    html += "• <strong>ID Corto:</strong> " + getShortMac() + "<br>";
    html += "• <strong>MAC:</strong> " + getMacAddress() + "<br>";
    html += "• <strong>Red WiFi:</strong> " + wifiSsid + "<br>";
    html += "• <strong>Host:</strong> <a href='http://zentx-control.local' style='color:#38bdf8;'>zentx-control.local</a>";
    html += "</div>";
    html += "<a href='http://" + WiFi.localIP().toString() + "/' class='btn'>🚀 Abrir Panel en Red Local</a>";
    html += "<a href='/start-ap' class='btn btn-sec'>⚙️ Reconfigurar Red WiFi</a>";
    html += "<a href='/stop-beacon' class='btn btn-sec'>📴 Apagar Baliza Ahora</a>";
    html += "</div></body></html>";
    server.send(200, "text/html", html);
    return;
  }

  // Pantalla normal de Alta WiFi
  int n = WiFi.scanNetworks();
  String options = "";
  for (int i = 0; i < n; ++i) {
    String ssidItem = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    int quality = getQuality(rssi);
    bool enc = (WiFi.encryptionType(i) != ENC_TYPE_NONE);
    options += "<option value='" + ssidItem + "'>" + String(enc ? "🔒 " : "🔓 ") + ssidItem + " (" + String(quality) + "% - " + String(rssi) + " dBm)</option>";
  }

  String html = "<!DOCTYPE html><html lang='es'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Alta WiFi - ZenTX Control</title>";
  html += "<style>";
  html += "* { box-sizing: border-box; font-family: 'Segoe UI', system-ui, sans-serif; }";
  html += "body { background: #0f172a; color: #f8fafc; text-align: center; margin: 0; padding: 1.5rem; }";
  html += ".card { background: #1e293b; padding: 2rem; border-radius: 1.25rem; max-width: 440px; margin: 0 auto; border: 1px solid #334155; box-shadow: 0 20px 40px rgba(0,0,0,0.5); }";
  html += "h1 { color: #38bdf8; font-size: 1.4rem; margin-top: 0; margin-bottom: 0.3rem; }";
  html += ".badge-mac { display: inline-block; background: #0f172a; border: 1px solid #475569; padding: 0.3rem 0.8rem; border-radius: 999px; font-size: 0.8rem; color: #38bdf8; margin-bottom: 1rem; }";
  html += "p { color: #94a3b8; font-size: 0.88rem; margin-bottom: 1.2rem; }";
  html += "label { font-size: 0.85rem; color: #cbd5e1; display: block; text-align: left; margin-bottom: 0.4rem; margin-top: 0.8rem; }";
  html += "select, input[type='password'], input[type='text'] { width: 100%; padding: 0.75rem; border-radius: 0.6rem; border: 1px solid #475569; background: #0f172a; color: white; font-size: 0.95rem; }";
  html += ".btn { display: block; width: 100%; padding: 0.9rem; border: none; border-radius: 0.75rem; font-size: 1rem; font-weight: bold; background: #0284c7; color: white; cursor: pointer; margin-top: 1.2rem; text-decoration: none; }";
  html += ".btn-sec { background: #334155; margin-top: 0.6rem; font-size: 0.85rem; }";
  html += ".footer { margin-top: 1.5rem; text-align: center; font-size: 0.78rem; color: #64748b; border-top: 1px solid #334155; padding-top: 0.8rem; }";
  html += "#manual-ssid-div { display: none; margin-top: 0.5rem; }";
  html += "</style>";
  html += "<script>";
  html += "function onSsidChange(sel) {";
  html += "  var mDiv = document.getElementById('manual-ssid-div');";
  html += "  var mInput = document.getElementById('manual_ssid');";
  html += "  if (sel.value === '__MANUAL__') { mDiv.style.display = 'block'; mInput.required = true; }";
  html += "  else { mDiv.style.display = 'none'; mInput.required = false; }";
  html += "}";
  html += "</script>";
  html += "</head><body>";

  html += "<div class='card'>";
  html += "<h1>📶 Alta WiFi - ZenTX Control</h1>";
  html += "<div class='badge-mac'>MAC: <strong>" + getMacAddress() + "</strong> (ID: <strong>" + getShortMac() + "</strong>)</div>";
  html += "<p>Selecciona tu red WiFi local para conectar el equipo de forma permanente.</p>";
  html += "<form action='/wifi-save' method='GET'>";
  html += "<label>Redes WiFi detectadas (2.4 GHz):</label>";
  html += "<select name='ssid' id='ssid_select' onchange='onSsidChange(this)' required>";
  if (n == 0) {
    html += "<option value='__MANUAL__'>⚠️ No se encontraron redes (Ingresar Manualmente)</option>";
  } else {
    html += options;
    html += "<option value='__MANUAL__'>[+] Ingresar red manualmente...</option>";
  }
  html += "</select>";

  html += "<div id='manual-ssid-div'>";
  html += "<label>Nombre de Red WiFi (SSID):</label>";
  html += "<input type='text' name='manual_ssid' id='manual_ssid' placeholder='Nombre exacto de tu red WiFi'>";
  html += "</div>";

  html += "<label>Contraseña de la Red WiFi:</label>";
  html += "<input type='password' name='pass' placeholder='Clave de red'>";

  html += "<label>Nombre / Alias del Equipo:</label>";
  html += "<input type='text' name='dev' value='" + deviceId + "'>";

  html += "<button type='submit' class='btn'>💾 Guardar y Conectar a Red Local</button>";
  html += "<a href='/' class='btn btn-sec'>🔄 Actualizar Lista de Redes</a>";
  html += "</form>";

  html += "<div class='footer'>ZenTX Control | Versión Firmware: <strong style='color:#38bdf8;'>" + String(FIRMWARE_VERSION) + "</strong></div>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

// ===== PÁGINA PRINCIPAL EN MODO NORMAL =====
void handleRoot() {
  if (isAPMode || beaconActive) {
    handleAPPortal();
    return;
  }

  readProbe();
  checkThermostat();

  String html = "<!DOCTYPE html><html lang='es'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>" + deviceId + " (" + getShortMac() + ")</title>";
  html += "<style>";
  html += "* { box-sizing: border-box; font-family: 'Segoe UI', system-ui, sans-serif; }";
  html += "body { background: #0f172a; color: #f8fafc; text-align: center; margin: 0; padding: 1.5rem; }";
  html += ".card { background: #1e293b; padding: 2rem; border-radius: 1.25rem; max-width: 460px; margin: 0 auto; border: 1px solid #334155; box-shadow: 0 20px 40px rgba(0,0,0,0.5); }";
  html += "h1 { color: #38bdf8; font-size: 1.4rem; margin-top: 0; margin-bottom: 0.2rem; }";
  html += ".mac-info { font-size: 0.8rem; color: #94a3b8; margin-bottom: 0.8rem; font-family: monospace; }";
  html += ".sub { color: #94a3b8; font-size: 0.85rem; margin-bottom: 1.2rem; }";
  html += ".badge { display: inline-block; padding: 0.6rem 1.2rem; border-radius: 999px; font-weight: bold; font-size: 1.05rem; margin-bottom: 1.2rem; width: 100%; }";
  html += ".bg-on { background: rgba(34,197,94,0.2); color: #4ade80; border: 1px solid #22c55e; }";
  html += ".bg-off { background: rgba(239,68,68,0.2); color: #f87171; border: 1px solid #ef4444; }";
  html += ".temp-box { background: #0f172a; padding: 1.5rem; border-radius: 1rem; border: 1px solid #334155; margin-bottom: 1.5rem; }";
  html += ".temp-val { font-size: 3rem; font-weight: bold; color: #38bdf8; }";
  html += ".btn { display: block; width: 100%; padding: 0.9rem; margin: 0.6rem 0; border: none; border-radius: 0.75rem; font-weight: bold; font-size: 1rem; cursor: pointer; text-decoration: none; transition: all 0.2s; }";
  html += ".btn-toggle { background: #3b82f6; color: white; }";
  html += ".btn-on { background: #22c55e; color: white; }";
  html += ".btn-off { background: #ef4444; color: white; }";
  html += ".btn-beacon { background: #0284c7; color: white; }";
  html += ".btn-ota { background: #8b5cf6; color: white; }";
  html += ".btn-wifi { background: #d97706; color: white; }";
  html += ".form-group { background: #0f172a; padding: 1rem; border-radius: 0.85rem; border: 1px solid #334155; margin-top: 1.2rem; text-align: left; }";
  html += ".footer { margin-top: 1.5rem; text-align: center; font-size: 0.78rem; color: #64748b; border-top: 1px solid #334155; padding-top: 0.8rem; }";
  html += "label { font-size: 0.85rem; color: #cbd5e1; display: block; margin-bottom: 0.4rem; }";
  html += "input[type='text'], input[type='number'] { width: 100%; padding: 0.6rem; border-radius: 0.5rem; border: 1px solid #475569; background: #1e293b; color: white; font-size: 0.95rem; margin-bottom: 0.8rem; }";
  html += "</style></head><body>";

  html += "<div class='card'>";
  html += "<h1>❄️ " + deviceId + "</h1>";
  html += "<div class='mac-info'>MAC: " + getMacAddress() + " | ID: " + getShortMac() + "</div>";
  html += "<div class='sub'>Red: <strong style='color:#38bdf8;'>" + wifiSsid + "</strong> | IP: <strong style='color:#38bdf8;'>" + WiFi.localIP().toString() + "</strong></div>";

  html += "<div class='badge " + String(relayState ? "bg-on'>COMPRESOR: ENCENDIDO" : "bg-off'>COMPRESOR: APAGADO") + "</div>";

  html += "<div class='temp-box'>";
  html += "<div style='font-size:0.85rem; color:#94a3b8; margin-bottom:0.4rem;'>TEMPERATURA DE SONDA</div>";
  html += "<div class='temp-val'>" + (sensorError ? "ERROR" : String(temperatureC, 2) + " °C") + "</div>";
  html += "<div style='font-size:0.8rem; color:#64748b; margin-top:0.3rem;'>" + sensorPinStr + "</div>";
  html += "</div>";

  html += "<a href='/toggle' class='btn btn-toggle'>⚡ CONMUTAR RELÉ</a>";
  html += "<div style='display:flex; gap:0.5rem;'>";
  html += "<a href='/on' class='btn btn-on' style='flex:1;'>🟢 ENCENDER</a>";
  html += "<a href='/off' class='btn btn-off' style='flex:1;'>🔴 APAGAR</a>";
  html += "</div>";

  // Ajustes de Termostato y Nube
  html += "<form action='/set' method='GET' class='form-group'>";
  html += "<div style='color:#38bdf8; font-weight:bold; margin-bottom:0.8rem;'>🌡️ Ajustes del Equipo</div>";
  html += "<label>Nombre / Alias del Equipo:</label>";
  html += "<input type='text' name='dev' value='" + deviceId + "'>";
  html += "<label>Temperatura Deseada (°C):</label>";
  html += "<input type='number' name='sp' step='0.5' value='" + String(setpointTemp, 1) + "'>";
  html += "<label>Histéresis (±°C):</label>";
  html += "<input type='number' name='hy' step='0.1' value='" + String(hysteresis, 1) + "'>";
  html += "<label>URL Notificación Google Apps Script / Webhook:</label>";
  html += "<input type='text' name='cloud' value='" + cloudUrl + "'>";
  html += "<button type='submit' class='btn btn-toggle' style='margin-top:0;'>💾 Guardar Configuración</button>";
  html += "</form>";

  // Activar Baliza WiFi bajo demanda
  html += "<a href='/start-beacon' class='btn btn-beacon' style='margin-top:1.2rem;'>📶 Emitir Baliza WiFi con IP (2 min)</a>";

  // Botón para Panel de Actualización OTA y Control Remoto
  html += "<a href='/firmware' class='btn btn-ota' style='margin-top:0.4rem;'>⚡ Actualización Web / Nube Remota</a>";

  // Re-activar Portal de Configuración WiFi
  html += "<a href='/start-ap' class='btn btn-wifi' style='margin-top:0.4rem;'>⚙️ Cambiar Red WiFi (Modo Alta)</a>";

  html += "<div class='footer'>ZenTX Control | Versión: <strong style='color:#38bdf8;'>" + String(FIRMWARE_VERSION) + "</strong> | Host: zentx-control.local</div>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

// ===== PÁGINA DE ACTUALIZACIÓN DE FIRMWARE Y CONTROL REMOTO =====
void handleFirmwarePage() {
  String html = "<!DOCTYPE html><html lang='es'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Actualización de Firmware - ZenTX Control</title>";
  html += "<style>";
  html += "* { box-sizing: border-box; font-family: 'Segoe UI', system-ui, sans-serif; }";
  html += "body { background: #0f172a; color: #f8fafc; text-align: center; margin: 0; padding: 1.5rem; }";
  html += ".card { background: #1e293b; padding: 2rem; border-radius: 1.25rem; max-width: 460px; margin: 0 auto; border: 1px solid #334155; box-shadow: 0 20px 40px rgba(0,0,0,0.5); text-align: left; }";
  html += "h1 { color: #38bdf8; font-size: 1.4rem; margin-top: 0; text-align: center; margin-bottom: 0.2rem; }";
  html += ".sub { color: #94a3b8; font-size: 0.85rem; text-align: center; margin-bottom: 1.2rem; }";
  html += ".section { background: #0f172a; padding: 1.2rem; border-radius: 0.85rem; border: 1px solid #334155; margin-bottom: 1.2rem; }";
  html += ".section-title { font-weight: bold; color: #38bdf8; margin-bottom: 0.6rem; font-size: 0.95rem; }";
  html += "p { color: #94a3b8; font-size: 0.85rem; margin-bottom: 0.8rem; line-height: 1.4; }";
  html += "label { font-size: 0.85rem; color: #cbd5e1; display: block; margin-bottom: 0.4rem; }";
  html += "input[type='file'], input[type='text'] { width: 100%; padding: 0.6rem; border-radius: 0.5rem; border: 1px solid #475569; background: #1e293b; color: white; font-size: 0.9rem; margin-bottom: 0.8rem; }";
  html += ".btn { display: block; width: 100%; padding: 0.85rem; border: none; border-radius: 0.75rem; font-weight: bold; font-size: 0.95rem; cursor: pointer; text-decoration: none; text-align: center; color: white; transition: all 0.2s; }";
  html += ".btn-local { background: #0284c7; }";
  html += ".btn-remote { background: #8b5cf6; }";
  html += ".btn-back { background: #475569; margin-top: 1rem; }";
  html += ".footer { margin-top: 1.5rem; text-align: center; font-size: 0.78rem; color: #64748b; border-top: 1px solid #334155; padding-top: 0.8rem; }";
  html += "</style></head><body>";

  html += "<div class='card'>";
  html += "<h1>⚡ Actualización ZenTX Control</h1>";
  html += "<div class='sub'>Versión Instalada: <strong style='color:#38bdf8;'>" + String(FIRMWARE_VERSION) + "</strong> | ID: <strong style='color:#38bdf8;'>" + getShortMac() + "</strong></div>";

  // 1. CARGA LOCAL DESDE LA PÁGINA WEB
  html += "<div class='section'>";
  html += "<div class='section-title'>📂 1. Carga Local de Archivo (.bin)</div>";
  html += "<p>Subir directamente el archivo binario (.bin) compilado desde tu navegador:</p>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  html += "<input type='file' name='update' accept='.bin' required>";
  html += "<button type='submit' class='btn btn-local'>⚡ Flashear Firmware Local</button>";
  html += "</form>";
  html += "</div>";

  // 2. ACTUALIZACIÓN REMOTA POR NUBE / URL
  html += "<div class='section'>";
  html += "<div class='section-title'>☁️ 2. Actualización Remota por Nube / URL</div>";
  html += "<p>Ingresa la URL pública del archivo .bin para actualizar remotamente desde cualquier red:</p>";
  html += "<form method='GET' action='/update-remote'>";
  html += "<label>URL del binario (.bin):</label>";
  html += "<input type='text' name='url' placeholder='https://servidor.com/firmware.bin' required>";
  html += "<button type='submit' class='btn btn-remote'>🚀 Iniciar Actualización Remota</button>";
  html += "</form>";
  html += "</div>";

  html += "<a href='/' class='btn btn-back'>⬅️ Volver al Panel Principal</a>";
  html += "<div class='footer'>ZenTX Control | MAC: " + getMacAddress() + "</div>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

// ===== API ENDPOINT JSON PARA CONSULTA DE VERSIÓN Y TELEMETRÍA (/status Y /json) =====
void handleStatusJson() {
  readProbe();
  String json = "{";
  json += "\"firmware\":\"ZenTX Control\",";
  json += "\"version\":\"" + String(FIRMWARE_VERSION) + "\",";
  json += "\"mac\":\"" + getMacAddress() + "\",";
  json += "\"shortMac\":\"" + getShortMac() + "\",";
  json += "\"deviceId\":\"" + deviceId + "\",";
  json += "\"temperature\":" + (sensorError ? "null" : String(temperatureC, 2)) + ",";
  json += "\"sensorPin\":\"" + sensorPinStr + "\",";
  json += "\"sensorError\":" + String(sensorError ? "true" : "false") + ",";
  json += "\"relayState\":" + String(relayState ? "true" : "false") + ",";
  json += "\"setpointTemp\":" + String(setpointTemp, 1) + ",";
  json += "\"hysteresis\":" + String(hysteresis, 1) + ",";
  json += "\"autoMode\":" + String(autoMode ? "true" : "false") + ",";
  json += "\"wifiSsid\":\"" + wifiSsid + "\",";
  json += "\"ip\":\"" + (isAPMode ? "192.168.4.1" : WiFi.localIP().toString()) + "\",";
  json += "\"beaconActive\":" + String(beaconActive ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

// ===== MANEJADOR DE ACTUALIZACIÓN REMOTA POR URL =====
void handleUpdateRemote() {
  if (!server.hasArg("url")) {
    server.send(400, "text/plain", "Falta parámetro 'url'");
    return;
  }
  String binUrl = server.arg("url");
  binUrl.trim();

  if (binUrl.length() == 0) {
    server.send(400, "text/plain", "URL vacia");
    return;
  }

  server.send(200, "text/html",
    "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Actualizando Remotamente...</title></head>"
    "<body style='background:#0f172a; color:#fff; font-family:sans-serif; text-align:center; padding:3rem;'>"
    "<div style='background:#1e293b; padding:2rem; border-radius:1rem; max-width:440px; margin:0 auto; border:1px solid #334155;'>"
    "<h2 style='color:#38bdf8;'>🚀 Descargando e Instalando Firmware Remoto...</h2>"
    "<p>Conectando a: <br><code style='color:#a855f7;'>" + binUrl + "</code></p>"
    "<p>El equipo se reiniciará automáticamente al finalizar la actualización.</p>"
    "</div></body></html>"
  );

  delay(1000);

  t_httpUpdate_return ret;
  if (binUrl.startsWith("https://")) {
    WiFiClientSecure clientSecure;
    clientSecure.setInsecure();
    ret = ESPhttpUpdate.update(clientSecure, binUrl);
  } else {
    WiFiClient client;
    ret = ESPhttpUpdate.update(client, binUrl);
  }

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("[OTA REMOTO] Error en actualización (%d): %s\n", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println(F("[OTA REMOTO] No hay cambios en el archivo binario."));
      break;
    case HTTP_UPDATE_OK:
      Serial.println(F("[OTA REMOTO] ✅ Actualización completada con éxito. Reiniciando..."));
      ESP.restart();
      break;
  }
}

void handleToggle() {
  autoMode = false;
  relayState = !relayState;
  digitalWrite(PIN_RELAY, relayState ? HIGH : LOW);
  Serial.print(F("[WEB] Relé Conmutado a: "));
  Serial.println(relayState ? F("ENCENDIDO") : F("APAGADO"));
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleOn() {
  autoMode = false;
  relayState = true;
  digitalWrite(PIN_RELAY, HIGH);
  Serial.println(F("[WEB] Relé Forzado: ENCENDIDO"));
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleOff() {
  autoMode = false;
  relayState = false;
  digitalWrite(PIN_RELAY, LOW);
  Serial.println(F("[WEB] Relé Forzado: APAGADO"));
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSet() {
  if (server.hasArg("dev")) {
    deviceId = server.arg("dev");
    deviceId.trim();
  }
  if (server.hasArg("sp")) setpointTemp = server.arg("sp").toFloat();
  if (server.hasArg("hy")) hysteresis = server.arg("hy").toFloat();
  if (server.hasArg("cloud")) {
    cloudUrl = server.arg("cloud");
    cloudUrl.trim();
  }
  autoMode = true;
  saveConfig();

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleStartBeacon() {
  startBeaconAP(120000); // 2 minutos
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleStopBeacon() {
  stopBeaconAP();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleWifiSave() {
  String selectedSsid = "";
  if (server.hasArg("ssid")) selectedSsid = server.arg("ssid");

  if (selectedSsid == "__MANUAL__" && server.hasArg("manual_ssid")) {
    wifiSsid = server.arg("manual_ssid");
  } else {
    wifiSsid = selectedSsid;
  }
  wifiSsid.trim();

  if (server.hasArg("pass")) {
    wifiPass = server.arg("pass");
    wifiPass.trim();
  }

  if (server.hasArg("dev")) {
    deviceId = server.arg("dev");
    deviceId.trim();
  }

  saveConfig();

  String html = "<!DOCTYPE html><html lang='es'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Conectando WiFi - ZenTX</title>";
  html += "<style>body { background:#0f172a; color:#fff; font-family:sans-serif; text-align:center; padding:2rem; }";
  html += ".card { background:#1e293b; padding:2rem; border-radius:1rem; max-width:400px; margin:0 auto; border:1px solid #334155; }</style></head><body>";
  html += "<div class='card'><h2 style='color:#38bdf8;'>✅ Credenciales Guardadas</h2>";
  html += "<p>Conectando a la red local <strong>" + wifiSsid + "</strong>...</p>";
  html += "<p>Al iniciar, el equipo emitirá la baliza <strong>ZenTX-" + getShortMac() + "-[IP]</strong> para que puedas ver su dirección IP directamente en la lista de redes WiFi.</p>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);
  delay(2000);
  ESP.restart();
}

void handleTriggerAP() {
  server.send(200, "text/plain", "Activando Modo Alta WiFi... Conéctate a ZenTX-Control-Setup en unos segundos.");
  delay(1000);
  startAPMode();
}

void startAPMode() {
  isAPMode = true;
  stopBeaconAP();
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);

  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP("ZenTX-Control-Setup");

  dnsServer.start(DNS_PORT, "*", apIP);
  Serial.println(F("\n[WIFI] 📶 MODO ALTA WIFI ACTIVADO"));
  Serial.println(F("[WIFI] Red AP: ZenTX-Control-Setup | IP: http://192.168.4.1"));
}

// Redirección HTTP 302 para Autodetección de Portal Cautivo en Celulares (iOS/Android/Windows)
void handleNotFound() {
  if (isAPMode || beaconActive) {
    String hostHeader = server.hostHeader();
    if (hostHeader != "192.168.4.1" && hostHeader.length() > 0) {
      server.sendHeader("Location", "http://192.168.4.1/", true);
      server.send(302, "text/plain", "");
      return;
    }
    handleAPPortal();
  } else {
    server.send(404, "text/plain", "404: Pagina no encontrada");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println(F("\n======================================================="));
  Serial.println(F("  FIRMWARE CONTROL ZENTX (v3.5 MAC & BALIZA INTELIGENTE)"));
  Serial.println(F("======================================================="));

  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_JACK14, INPUT_PULLUP);
  pinMode(PIN_JACK4, INPUT_PULLUP);

  digitalWrite(PIN_RELAY, LOW);
  digitalWrite(PIN_LED, LOW); // LED ON al arrancar
  delay(1000);
  digitalWrite(PIN_LED, HIGH); // LED OFF

  EEPROM.begin(512);
  loadConfig();

  String shortMac = getShortMac();
  Serial.print(F("[HARDWARE] Dirección MAC Única: "));
  Serial.println(getMacAddress());
  Serial.print(F("[HARDWARE] Identificador Corto: "));
  Serial.println(shortMac);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  // Registro de Hostname DHCP en el router del cliente
  String hostName = "zentx-" + shortMac;
  WiFi.hostname(hostName.c_str());

  if (wifiSsid.length() > 0) {
    Serial.print(F("[WIFI] Conectando a red local: "));
    Serial.println(wifiSsid);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  } else {
    Serial.println(F("[WIFI] Sin red guardada en EEPROM. Iniciando Modo Alta..."));
  }

  unsigned long startWait = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startWait < 12000) {
    delay(300);
    digitalWrite(PIN_LED, !digitalRead(PIN_LED));
    Serial.print(F("."));
  }

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(PIN_LED, LOW); // LED ON fijo indicando conexión exitosa
    isAPMode = false;
    Serial.println(F("\n[WIFI] ✅ CONECTADO CON ÉXITO A LA RED LOCAL!"));
    Serial.print(F("[WIFI] Dirección IP: http://"));
    Serial.println(WiFi.localIP());

    // Protocolos de Descubrimiento de Red (mDNS y NetBIOS para Windows)
    if (MDNS.begin("zentx-control")) {
      MDNS.addService("http", "tcp", 80);
      Serial.println(F("[mDNS] Host activo: http://zentx-control.local"));
    }
    NBNS.begin("zentx-control");
    NBNS.begin(hostName.c_str());
    Serial.println(F("[NetBIOS] Host Windows activo: http://zentx-control"));

    // Activar Baliza WiFi Inteligente con la IP asignada por 90 segundos
    startBeaconAP(BEACON_TIME);

    // Enviar notificación automática a la nube (Google Sheets / Telegram)
    readProbe();
    sendCloudNotification("CONEXION_WIFI");

  } else {
    startAPMode();
  }

  // Rutas del Servidor Web
  server.on("/", handleRoot);
  server.on("/firmware", handleFirmwarePage);
  server.on("/update-remote", handleUpdateRemote);
  server.on("/status", handleStatusJson);
  server.on("/json", handleStatusJson);
  server.on("/toggle", handleToggle);
  server.on("/on", handleOn);
  server.on("/off", handleOff);
  server.on("/set", handleSet);
  server.on("/wifi-save", handleWifiSave);
  server.on("/start-ap", handleTriggerAP);
  server.on("/start-beacon", handleStartBeacon);
  server.on("/stop-beacon", handleStopBeacon);
  server.onNotFound(handleNotFound);

  // Inicializar servidor de actualización OTA local en /update
  httpUpdater.setup(&server, "/update");

  server.begin();
  Serial.println(F("[SISTEMA] Servidor Web ZenTX v3.5 Listo."));
}

void loop() {
  if (isAPMode) {
    dnsServer.processNextRequest();
    server.handleClient();

    // Parpadeo rápido de LED en Modo Alta WiFi (200ms)
    if (millis() - lastLedToggleTime > 200) {
      lastLedToggleTime = millis();
      digitalWrite(PIN_LED, !digitalRead(PIN_LED));
    }
  } else {
    if (beaconActive) {
      dnsServer.processNextRequest();
      if (millis() > beaconEndTime) {
        stopBeaconAP();
      }
    }

    MDNS.update();
    server.handleClient();

    // Verificación de conexión WiFi en segundo plano
    if (millis() - lastWifiCheckTime > 15000) {
      lastWifiCheckTime = millis();
      if (WiFi.status() != WL_CONNECTED && wifiSsid.length() > 0) {
        Serial.println(F("[WIFI] Reconectando a la red local en segundo plano..."));
        WiFi.reconnect();
      }
    }
    digitalWrite(PIN_LED, LOW); // LED ON fijo
  }

  // Lectura periódica de sensores y termostato cada 2s
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 2000) {
    lastCheck = millis();
    readProbe();
    checkThermostat();
  }

  // Control Físico por Botón Frontal (GPIO 0)
  static unsigned long buttonPressStart = 0;
  if (digitalRead(PIN_BUTTON) == LOW) {
    if (buttonPressStart == 0) buttonPressStart = millis();

    // Pulsación larga (> 5s) -> Modo Alta WiFi
    if (millis() - buttonPressStart > 5000 && !isAPMode) {
      Serial.println(F("[BOTÓN] Pulsación larga (>5s) -> Activando Modo Alta WiFi..."));
      for (int i = 0; i < 12; i++) {
        digitalWrite(PIN_LED, !digitalRead(PIN_LED));
        delay(80);
      }
      startAPMode();
      buttonPressStart = 0;
    }
  } else {
    if (buttonPressStart > 0) {
      unsigned long duration = millis() - buttonPressStart;
      buttonPressStart = 0;

      if (duration < 2500) {
        // Pulsación corta (< 2.5s) -> Conmuta relé manualmente
        autoMode = false;
        relayState = !relayState;
        digitalWrite(PIN_RELAY, relayState ? HIGH : LOW);
        Serial.print(F("[BOTÓN] Pulsación corta -> Relé: "));
        Serial.println(relayState ? F("ENCENDIDO") : F("APAGADO"));
      } else if (duration >= 2500 && duration <= 5000 && !isAPMode) {
        // Pulsación media (2.5s a 5s) -> Activar Baliza WiFi de Rescate (2 minutos)
        Serial.println(F("[BOTÓN] Pulsación media (2.5s-5s) -> Activando Baliza WiFi de Rescate..."));
        for (int i = 0; i < 6; i++) {
          digitalWrite(PIN_LED, !digitalRead(PIN_LED));
          delay(100);
        }
        startBeaconAP(120000); // 2 minutos
      }
    }
  }
}
