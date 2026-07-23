#include <WiFi.h>
#include <WiFiManager.h> 
#include <TFT_eSPI.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// --- IDENTIDAD Y LÍMITES ---
#define DEVICE_ID "PB1"   
#define UMBRAL_MAX 8.0   
#define UMBRAL_MIN 2.0   
#define TIEMPO_GRACIA_ALERTA 30000 

// --- PINES (LilyGo T-Display) ---
#define PIN_DS18B20 21 
#define PIN_DHT 17     
#define PIN_LCD_BL 4   
#define BTN_UP 0       
#define BTN_DWN 35     

// --- OBJETOS ---
TFT_eSPI tft = TFT_eSPI();
OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);
DHT dht(PIN_DHT, DHT11);
WiFiManager wm;

// --- VARIABLES ---
float lecturas[10]; 
int indiceLectura = 0;
unsigned long ultimoEnvio = 0;
unsigned long ultimaLectura = 0;
unsigned long inicioFueraDeRango = 0;
bool sistemaEnAlarma = false;

// URL de tu Script de Google
String serverPath = "https://script.google.com/macros/s/AKfycbxRCVL3BZi8qJePhMVZdyxPegpT_CapgY3FGlvY7SbgP3Hnbw-TwUzgJjZc1hYlDBVw/exec";

void setup() {
  // 1. Iniciar Serial para diagnóstico
  Serial.begin(115200);
  Serial.println("--- TX4 INICIANDO ---");

  // 2. Configurar Pines
  pinMode(PIN_LCD_BL, OUTPUT); digitalWrite(PIN_LCD_BL, HIGH);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DWN, INPUT_PULLUP);
  
  // 3. Iniciar Pantalla
  tft.init(); 
  tft.setRotation(1); 
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_YELLOW);
  tft.drawString("SISTEMA TX4: " + String(DEVICE_ID), 10, 10, 2);

  // 4. Lógica de Reset de WiFi (Botón UP al arrancar)
  Serial.println("Verificando boton reset...");
  if (digitalRead(BTN_UP) == LOW) {
    tft.fillScreen(TFT_RED);
    tft.setTextColor(TFT_WHITE);
    tft.drawString("BORRANDO WIFI...", 10, 50, 4);
    Serial.println("Reset solicitado por hardware.");
    wm.resetSettings(); 
    delay(3000);
    ESP.restart();
  }

  // 5. Conectar WiFi
  Serial.println("Iniciando WiFiManager...");
  tft.drawString("Buscando red...", 10, 40, 2);
  wm.setConfigPortalBlocking(false); 
  wm.autoConnect("TX4_CONFIG_WIFI");

  // 6. Iniciar Sensores
  sensors.begin(); 
  dht.begin();
  for(int i=0; i<10; i++) lecturas[i] = 5.0; 

  // 7. Dibujo inicial fijo
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_YELLOW);
  tft.drawString("TX 4 HELADERA " + String(DEVICE_ID), 5, 5, 2);
  tft.drawFastHLine(0, 100, 240, TFT_DARKGREY);
  Serial.println("Setup completado OK.");
}

void loop() {
  wm.process(); // Maneja el portal WiFi
  unsigned long ahora = millis();

  // --- LECTURA DE SENSORES Y PANTALLA (Cada 2 segundos) ---
  if (ahora - ultimaLectura >= 2000) {
    ultimaLectura = ahora;
    sensors.requestTemperatures();
    float tActual = sensors.getTempCByIndex(0);
    
    if (tActual != -127.0) {
      lecturas[indiceLectura] = tActual;
      indiceLectura = (indiceLectura + 1) % 10;
    }
    float tH = 0;
    for(int i=0; i<10; i++) tH += lecturas[i];
    tH /= 10.0;

    float hAmb = dht.readHumidity();
    float tAmb = dht.readTemperature();

    // Colores y Alertas
    uint16_t colorT = TFT_GREEN;
    String evento = "LOG";
    if (tH >= UMBRAL_MAX) { colorT = TFT_RED; evento = "ALERTA_CALOR"; }
    else if (tH <= UMBRAL_MIN) { colorT = TFT_BLUE; evento = "ALERTA_FRIO"; }

    // Alarma por tiempo
    if (evento != "LOG" && !sistemaEnAlarma) {
      if (inicioFueraDeRango == 0) inicioFueraDeRango = ahora;
      if (ahora - inicioFueraDeRango > TIEMPO_GRACIA_ALERTA) {
        Serial.println("!!! ALERMA DETECTADA !!!");
        enviarTodo(tH, tAmb, hAmb, evento);
        sistemaEnAlarma = true;
      }
    } else if (evento == "LOG") {
      inicioFueraDeRango = 0; sistemaEnAlarma = false;
    }

    // Dibujar datos (Cero parpadeo)
    tft.setTextColor(colorT, TFT_BLACK);
    if (tActual == -127.0) tft.drawString("ERR SENSOR", 10, 40, 4);
    else {
      tft.drawFloat(tH, 1, 10, 30, 7);
      tft.drawString("C", 185, 45, 6);
    }
    
    tft.setTextColor(TFT_SKYBLUE, TFT_BLACK);
    tft.drawString("Amb: " + String(tAmb, 0) + "C ", 5, 108, 4);
    tft.drawString("Hum: " + String(hAmb, 0) + "% ", 130, 108, 4);

    // Icono WiFi
    if (WiFi.status() == WL_CONNECTED) tft.fillCircle(225, 12, 5, TFT_GREEN);
    else tft.fillCircle(225, 12, 5, TFT_RED);
  }

  // --- BOTONES ---
  if (digitalRead(BTN_DWN) == LOW) { // Manual Sync
    Serial.println("Sincronizacion manual solicitada.");
    tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.drawString("SYNC...", 160, 5, 2);
    float tM = 0; for(int i=0; i<10; i++) tM += lecturas[i]; tM /= 10.0;
    enviarTodo(tM, dht.readTemperature(), dht.readHumidity(), "MANUAL");
    delay(500);
  }

  // --- ENVIO AUTOMÁTICO (5 MIN) ---
  if (ahora - ultimoEnvio >= 300000) {
    ultimoEnvio = ahora;
    float tA = 0; for(int i=0; i<10; i++) tA += lecturas[i]; tA /= 10.0;
    enviarTodo(tA, dht.readTemperature(), dht.readHumidity(), "LOG");
  }
}

void enviarTodo(float tH, float tA, float hA, String ev) {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Enviando datos a Google...");
    WiFiClientSecure client;
    client.setInsecure(); // Necesario para evitar problemas de certificados con Google
    
    HTTPClient http;
    String url = serverPath + "?id=" + DEVICE_ID + "&temp=" + String(tH) + "&tempAmb=" + String(tA) + "&humedad=" + String(hA) + "&evento=" + ev;
    
    if (http.begin(client, url)) {
      http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      int code = http.GET();
      if (code > 0) {
        Serial.print("Respuesta Google: "); Serial.println(code);
        tft.setTextColor(TFT_BLACK, TFT_GREEN);
        tft.drawString(" SYNC OK ", 160, 5, 2);
      } else {
        Serial.print("Error HTTP: "); Serial.println(http.errorToString(code));
        tft.setTextColor(TFT_WHITE, TFT_RED);
        tft.drawString(" ERR HTTP ", 160, 5, 2);
      }
      http.end();
    }
  } else {
    Serial.println("Error: WiFi no conectado.");
  }
}