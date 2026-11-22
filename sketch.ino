/*
  ParkUABC - Estacionamiento inteligente (ESP32)
  - 2 sensores para la barrera (entrada y salida) + servomotor
  - 3 espacios de estacionamiento con sensor ultrasonico
  - 3 LEDs: verde (2 o mas libres), amarillo (1 libre), rojo (0 libres)
  - LCD 16x2 I2C: muestra cuantos espacios quedan
  - Envia datos a ThingSpeak cada 2 minutos (en una tarea aparte para no frenar la barrera)

  Campos del canal de ThingSpeak:
     field1..field3 = estado de cada espacio (1 = ocupado, 0 = libre)
     field4 = espacios disponibles
     field5 = porcentaje de ocupacion
     field6 = entradas acumuladas   field7 = salidas acumuladas
*/

#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------------- CONFIGURACION WiFi / THINGSPEAK ----------------
const char* WIFI_SSID  = "Wokwi-GUEST";       // en Wokwi siempre es esta red
const char* WIFI_PASS  = "";
const int   WIFI_CANAL = 6;
const char* TS_WRITE_KEY = "Q86L6CXFREYPM7RO";           // Write API Key del canal
const unsigned long INTERVALO_TS = 120000;               // 2 minutos (el plan gratis exige >= 15 s)

// ---------------- PINES ----------------

// Entrada
const int trigEntrada = 33; //12
const int echoEntrada = 25; //11

// Salida
const int trigSalida = 26;
const int echoSalida = 27;

// Estacionamientos
const int trigEspacios[] = {12, 4, 5};
const int echoEspacios[] = {14, 16, 18};

// LEDs
const int ledVerde = 19;
const int ledAmarillo = 17;
const int ledRojo = 0;

// Servo
const int pinServo = 2;

// LCD I2C: SDA = 21, SCL = 22
LiquidCrystal_I2C lcd(0x27, 16, 2);

Servo barrera;

// ----------- VARIABLES -----------

const int NUM_ESPACIOS = 3;
const int DISTANCIA_CARRO = 4;
const int DISTANCIA_OCUPADO = 3;

bool espacios[3] = {false, false, false};
bool candidato[3] = {false, false, false};

int estabilidad[3] = {0, 0, 0};
volatile int disponibles = 3;      // volatile: tambien lo lee la tarea de ThingSpeak
volatile int entradas = 0;         // autos que han entrado (acumulado)
volatile int salidas = 0;          // autos que han salido (acumulado)

int ultimaClaveLCD = -1;
unsigned long ultimoPrint = 0;

// Estados
enum Estado {
  LIBRE,
  ENTRANDO,
  SALIENDO,
  ESPERANDO
};

Estado estado = LIBRE;

// ----------- MEDIR DISTANCIA -----------

float medirDistancia(int trig, int echo) {

  digitalWrite(trig, LOW);
  delayMicroseconds(2);

  digitalWrite(trig, HIGH);
  delayMicroseconds(10);

  digitalWrite(trig, LOW);

  long duracion = pulseIn(echo, HIGH, 25000);

  if (duracion == 0) {
    return 999;
  }

  return duracion * 0.0343 / 2;
}

// ----------- ACTUALIZAR ESPACIOS -----------

void actualizarEspacios() {

  int libres = 0;

  for (int i = 0; i < 3; i++) {

    float distancia = medirDistancia(
      trigEspacios[i],
      echoEspacios[i]
    );

    bool detectado =
      distancia > 0 && distancia < DISTANCIA_OCUPADO;

    if (detectado == candidato[i]) {
      estabilidad[i]++;
    } else {
      candidato[i] = detectado;
      estabilidad[i] = 1;
    }

    if (estabilidad[i] >= 3) {
      espacios[i] = candidato[i];
    }

    if (!espacios[i]) {
      libres++;
    }
  }

  disponibles = libres;
}

// ----------- ACTUALIZAR LEDS -----------

void actualizarLEDs() {

  digitalWrite(ledVerde, LOW);
  digitalWrite(ledAmarillo, LOW);
  digitalWrite(ledRojo, LOW);

  if (disponibles >= 2) {
    digitalWrite(ledVerde, HIGH);
  }
  else if (disponibles == 1) {
    digitalWrite(ledAmarillo, HIGH);
  }
  else {
    digitalWrite(ledRojo, HIGH);
  }
}

// ----------- LCD -----------

// Muestra cuantos espacios quedan. Solo redibuja cuando algo cambia.
void actualizarLCD() {

  int mascara = 0;
  for (int i = 0; i < NUM_ESPACIOS; i++) {
    if (espacios[i]) mascara |= (1 << i);
  }
  int clave = mascara * 10 + disponibles;
  if (clave == ultimaClaveLCD) return;
  ultimaClaveLCD = clave;

  String a = "Libres: " + String(disponibles) + " de " + String(NUM_ESPACIOS);
  String b;
  if (disponibles >= 2)       b = "DISPONIBLE";
  else if (disponibles == 1)  b = "CASI LLENO";
  else                        b = "LLENO";

  while (a.length() < 16) a += ' ';
  while (b.length() < 16) b += ' ';

  lcd.setCursor(0, 0);
  lcd.print(a);
  lcd.setCursor(0, 1);
  lcd.print(b);
}

// ----------- BARRERA -----------

void abrirBarrera() {
  barrera.write(90);
}

void cerrarBarrera() {
  barrera.write(0);
}

// ----------- THINGSPEAK (tarea en el otro nucleo) -----------

void enviarThingSpeak() {

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    Serial.println("[TS] sin WiFi, se omite el envio");
    return;
  }

  int disp = disponibles;
  int ocupados = NUM_ESPACIOS - disp;
  float pct = 100.0 * ocupados / NUM_ESPACIOS;

  String url = String("http://api.thingspeak.com/update?api_key=") + TS_WRITE_KEY;
  for (int i = 0; i < NUM_ESPACIOS; i++) {
    url += "&field" + String(i + 1) + "=" + String(espacios[i] ? 1 : 0);
  }
  url += "&field4=" + String(disp);
  url += "&field5=" + String(pct, 1);
  url += "&field6=" + String((int)entradas);
  url += "&field7=" + String((int)salidas);

  HTTPClient http;
  http.begin(url);
  http.setTimeout(5000);
  int codigo = http.GET();
  String resp = (codigo > 0) ? http.getString() : "";
  http.end();

  Serial.print("[TS] HTTP ");
  Serial.print(codigo);
  Serial.print(" | registro: ");
  Serial.println(resp);
  if (resp == "0") {
    Serial.println("[TS] ThingSpeak no guardo el dato (revisa la Write API Key o espera 15 s entre envios)");
  }
}

void tareaThingSpeak(void* parametro) {

  Serial.print("Conectando a WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CANAL);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    vTaskDelay(250 / portTICK_PERIOD_MS);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) Serial.println(" conectado");
  else Serial.println(" sin conexion (el sistema sigue funcionando en modo local)");

  vTaskDelay(3000 / portTICK_PERIOD_MS);
  enviarThingSpeak();                       // primer envio al arrancar (para comprobar rapido)
  unsigned long ultimoEnvio = millis();

  for (;;) {
    if (millis() - ultimoEnvio >= INTERVALO_TS) {
      ultimoEnvio = millis();
      enviarThingSpeak();
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ----------- CONFIGURACION -----------

void setup() {

  Serial.begin(9600);

  pinMode(trigEntrada, OUTPUT);
  pinMode(echoEntrada, INPUT);

  pinMode(trigSalida, OUTPUT);
  pinMode(echoSalida, INPUT);

  for (int i = 0; i < 3; i++) {
    pinMode(trigEspacios[i], OUTPUT);
    pinMode(echoEspacios[i], INPUT);
  }

  pinMode(ledVerde, OUTPUT);
  pinMode(ledAmarillo, OUTPUT);
  pinMode(ledRojo, OUTPUT);

  barrera.attach(pinServo);
  cerrarBarrera();

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("ParkUABC        ");
  lcd.setCursor(0, 1);
  lcd.print("Iniciando...    ");

  Serial.println("ESTACIONAMIENTO INTELIGENTE");
  Serial.println("Sistema iniciado");

  // WiFi y ThingSpeak en el nucleo 0; el loop (barrera y sensores) sigue en el nucleo 1
  xTaskCreatePinnedToCore(tareaThingSpeak, "thingspeak", 8192, NULL, 1, NULL, 0);
}

// ----------- LOOP PRINCIPAL -----------

void loop() {

  actualizarEspacios();
  actualizarLEDs();
  actualizarLCD();

  float entrada = medirDistancia(
    trigEntrada, echoEntrada
  );

  float salida = medirDistancia(
    trigSalida, echoSalida
  );

  bool carroEntrada = entrada < DISTANCIA_CARRO;
  bool carroSalida = salida < DISTANCIA_CARRO;

  switch (estado) {

    case LIBRE:

      // Entrada: solamente si hay espacios
      if (carroEntrada && disponibles > 0) {

        Serial.println("Vehiculo entrando");

        abrirBarrera();
        estado = ENTRANDO;
      }

      // Salida: siempre permitida
      else if (carroSalida) {

        Serial.println("Vehiculo saliendo");

        abrirBarrera();
        estado = SALIENDO;
      }

      else if (carroEntrada && disponibles == 0) {

        Serial.println("ESTACIONAMIENTO LLENO");
      }

      break;

    case ENTRANDO:

      // Detectar que el vehiculo paso
      if (carroSalida) {

        cerrarBarrera();

        Serial.println("Entrada completada");
        entradas++;

        estado = ESPERANDO;
      }

      break;

    case SALIENDO:

      // Detectar que el vehiculo paso
      if (carroEntrada) {

        cerrarBarrera();

        Serial.println("Salida completada");
        salidas++;

        estado = ESPERANDO;
      }

      break;

    case ESPERANDO:

      // Esperar a que ambos sensores queden libres
      if (!carroEntrada && !carroSalida) {
        estado = LIBRE;
      }

      break;
  }

  // Informacion (cada 2 s para no saturar el monitor serie)
  if (millis() - ultimoPrint >= 2000) {
    ultimoPrint = millis();

    Serial.print("Espacios disponibles: ");
    Serial.println(disponibles);

    Serial.print("Espacios ocupados: ");
    Serial.println(NUM_ESPACIOS - disponibles);
  }

  delay(100);
}