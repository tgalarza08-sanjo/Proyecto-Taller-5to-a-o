wemos_code.ino

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>
#include <time.h>
#include <memory>

// UrbaPark - Wemos D1 Mini / ESP8266
// Puente entre Arduino Mega y Cloud Firestore.
// Serial a 9600 para comunicarse con el Mega.

const char* WIFI_SSID = "ifon";
const char* WIFI_PASSWORD = "12345678";

const char* FIREBASE_API_KEY = "AIzaSyBpifbCMpT_Q9NgkLNoq_IkR46d5nYor0k";
const char* FIREBASE_PROJECT_ID = "urbapark-bd";

const char* HARDWARE_EMAIL = "hardware@urbapark.com";
const char* HARDWARE_PASSWORD = "bella123_";

const unsigned long POLL_FIRESTORE_MS = 3000;
const unsigned long TOKEN_REFRESH_MS = 45UL * 60UL * 1000UL;
const unsigned long SERIAL_BAUD = 9600;

String idToken = "";
unsigned long tokenMillis = 0;
unsigned long lastPoll = 0;

std::unique_ptr<BearSSL::WiFiClientSecure> secureClient;

struct PlazaCache {
  String status;
  String activeReservationId;
  String reservationStatus;
  String entryCode;
  String exitCode;
  bool sensorOccupied;
};

PlazaCache plazas[4];

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(800);

  secureClient.reset(new BearSSL::WiFiClientSecure);
  secureClient->setInsecure();

  conectarWiFi();
  sincronizarHora();

  if (!loginFirebase()) {
    delay(5000);
    ESP.restart();
  }

  leerEstadoCompleto();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    conectarWiFi();
  }

  if (idToken.length() == 0 || millis() - tokenMillis > TOKEN_REFRESH_MS) {
    loginFirebase();
  }

  leerMega();

  if (millis() - lastPoll >= POLL_FIRESTORE_MS) {
    lastPoll = millis();
    leerEstadoCompleto();
  }

  yield();
}

void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 60) {
    delay(250);
    intentos++;
    yield();
  }

  if (WiFi.status() != WL_CONNECTED) {
    delay(3000);
    ESP.restart();
  }
}

void sincronizarHora() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  time_t now = time(nullptr);
  int intentos = 0;

  while (now < 1700000000 && intentos < 40) {
    delay(250);
    now = time(nullptr);
    intentos++;
    yield();
  }
}

bool loginFirebase() {
  String url = String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") + FIREBASE_API_KEY;

  StaticJsonDocument<256> bodyDoc;
  bodyDoc["email"] = HARDWARE_EMAIL;
  bodyDoc["password"] = HARDWARE_PASSWORD;
  bodyDoc["returnSecureToken"] = true;

  String body;
  serializeJson(bodyDoc, body);

  String payload;
  int code = httpRequest("POST", url, body, "", payload);

  if (code != 200) {
    return false;
  }

  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    return false;
  }

  idToken = doc["idToken"].as<String>();
  tokenMillis = millis();

  return idToken.length() > 0;
}

void leerMega() {
  while (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd.length() == 0) continue;

    if (cmd.startsWith("SENSOR:")) {
      procesarSensor(cmd);
    } else if (cmd.startsWith("CODE:")) {
      String codigo = cmd.substring(5);
      codigo.trim();
      validarCodigo(codigo);
    }
  }
}

void procesarSensor(String cmd) {
  int sep1 = cmd.indexOf(':');
  int sep2 = cmd.indexOf(':', sep1 + 1);

  if (sep1 == -1 || sep2 == -1) return;

  int plazaNum = cmd.substring(sep1 + 1, sep2).toInt();
  String valor = cmd.substring(sep2 + 1);

  if (plazaNum < 1 || plazaNum > 4) return;

  int index = plazaNum - 1;
  bool ocupado = valor == "1";

  plazas[index].sensorOccupied = ocupado;

  if (ocupado) {
    if (plazas[index].status != "ocupada") {
      if (actualizarPlaza(plazaNum, "ocupada", false)) {
        plazas[index].status = "ocupada";
        enviarEstadoAlMega(plazaNum, "ocupada");
      }
    }

    if (plazas[index].activeReservationId.length() > 0 &&
        plazas[index].reservationStatus == "pendiente") {
      if (actualizarReserva(plazas[index].activeReservationId, "ocupada", "arrivedAt")) {
        plazas[index].reservationStatus = "ocupada";
      }
    }

    return;
  }

  if (plazas[index].status == "ocupada" &&
      plazas[index].activeReservationId.length() == 0) {
    if (actualizarPlaza(plazaNum, "libre", false)) {
      plazas[index].status = "libre";
      enviarEstadoAlMega(plazaNum, "libre");
    }
  }
}

void validarCodigo(String codigo) {
  if (codigo.length() != 4) {
    enviarDenegado("CODIGO_INCOMPLETO");
    return;
  }

  leerEstadoCompleto();

  bool existeComoEntrada = false;
  bool existeComoSalida = false;
  bool hayReservaActiva = false;
  bool salidaNoPagada = false;

  for (int i = 0; i < 4; i++) {
    int plazaNum = i + 1;

    if (plazas[i].activeReservationId.length() > 0) {
      hayReservaActiva = true;
    }

    if (plazas[i].entryCode == codigo) {
      existeComoEntrada = true;

      if (plazas[i].reservationStatus == "pendiente" &&
          plazas[i].activeReservationId.length() > 0) {
        bool okReserva = actualizarReserva(plazas[i].activeReservationId, "ocupada", "arrivedAt");
        bool okPlaza = actualizarPlaza(plazaNum, "ocupada", false);

        if (okReserva && okPlaza) {
          plazas[i].reservationStatus = "ocupada";
          plazas[i].status = "ocupada";
          enviarEstadoAlMega(plazaNum, "ocupada");
          enviarAbrir("ENTRY", plazaNum);
        } else {
          enviarDenegado("ERROR_ACTUALIZAR_ENTRADA");
        }

        return;
      }
    }

    if (plazas[i].exitCode == codigo) {
      existeComoSalida = true;

      if (plazas[i].reservationStatus == "pagada" &&
          plazas[i].activeReservationId.length() > 0) {
        bool okReserva = actualizarReserva(plazas[i].activeReservationId, "finalizada", "finishedAt");
        bool okPlaza = actualizarPlaza(plazaNum, "libre", true);

        if (okReserva && okPlaza) {
          plazas[i].reservationStatus = "finalizada";
          plazas[i].status = "libre";
          plazas[i].activeReservationId = "";
          plazas[i].entryCode = "";
          plazas[i].exitCode = "";
          enviarEstadoAlMega(plazaNum, "libre");
          enviarAbrir("EXIT", plazaNum);
        } else {
          enviarDenegado("ERROR_ACTUALIZAR_SALIDA");
        }

        return;
      } else {
        salidaNoPagada = true;
      }
    }
  }

  if (!hayReservaActiva) {
    enviarDenegado("SIN_RESERVA_ACTIVA");
  } else if (existeComoEntrada) {
    enviarDenegado("ENTRADA_NO_PENDIENTE");
  } else if (existeComoSalida && salidaNoPagada) {
    enviarDenegado("SALIDA_NO_PAGADA");
  } else if (!existeComoEntrada && !existeComoSalida) {
    enviarDenegado("CODIGO_NO_EXISTE");
  } else {
    enviarDenegado("RECHAZO_DESCONOCIDO");
  }
}

void leerEstadoCompleto() {
  for (int plazaNum = 1; plazaNum <= 4; plazaNum++) {
    leerPlaza(plazaNum);
    yield();
  }
}

bool leerPlaza(int plazaNum) {
  String payload;
  String path = String("parking_spots/spot_") + plazaNum;

  int code = firestoreGet(path, payload);

  if (code != 200) {
    return false;
  }

  DynamicJsonDocument doc(8192);

  if (deserializeJson(doc, payload)) {
    return false;
  }

  JsonObject fields = doc["fields"];

  String status = fields["status"]["stringValue"] | "libre";
  String activeReservationId = fields["activeReservationId"]["stringValue"] | "";

  int index = plazaNum - 1;
  bool cambioEstado = plazas[index].status != status;

  plazas[index].status = status;
  plazas[index].activeReservationId = activeReservationId;
  plazas[index].reservationStatus = "";
  plazas[index].entryCode = "";
  plazas[index].exitCode = "";

  if (activeReservationId.length() > 0) {
    leerReserva(index, activeReservationId);
  }

  if (cambioEstado) {
    enviarEstadoAlMega(plazaNum, status);
  }

  return true;
}

bool leerReserva(int index, String reservationId) {
  String payload;
  String path = String("reservations/") + reservationId;

  int code = firestoreGet(path, payload);

  if (code != 200) {
    return false;
  }

  DynamicJsonDocument doc(8192);

  if (deserializeJson(doc, payload)) {
    return false;
  }

  JsonObject fields = doc["fields"];

  plazas[index].reservationStatus = fields["status"]["stringValue"] | "";
  plazas[index].entryCode = fields["entryCode"]["stringValue"] | "";
  plazas[index].exitCode = fields["exitCode"]["stringValue"] | "";

  return true;
}

int firestoreGet(String documentPath, String &payload) {
  String url = firestoreDocumentUrl(documentPath);
  return httpRequest("GET", url, "", idToken, payload);
}

bool actualizarPlaza(int plazaNum, String status, bool limpiarReserva) {
  String path = String("parking_spots/spot_") + plazaNum;
  String url = firestoreDocumentUrl(path);

  url += "?updateMask.fieldPaths=status&updateMask.fieldPaths=updatedAt";

  if (limpiarReserva) {
    url += "&updateMask.fieldPaths=activeReservationId";
  }

  DynamicJsonDocument doc(1024);
  doc["fields"]["status"]["stringValue"] = status;
  doc["fields"]["updatedAt"]["timestampValue"] = ahoraIso();

  if (limpiarReserva) {
    doc["fields"]["activeReservationId"]["nullValue"] = "NULL_VALUE";
  }

  String body;
  serializeJson(doc, body);

  String payload;
  int code = httpRequest("PATCH", url, body, idToken, payload);

  return code == 200;
}

bool actualizarReserva(String reservationId, String status, String campoFecha) {
  String path = String("reservations/") + reservationId;
  String url = firestoreDocumentUrl(path);

  url += "?updateMask.fieldPaths=status";

  if (campoFecha.length() > 0) {
    url += "&updateMask.fieldPaths=" + campoFecha;
  }

  DynamicJsonDocument doc(1024);
  doc["fields"]["status"]["stringValue"] = status;

  if (campoFecha.length() > 0) {
    doc["fields"][campoFecha]["timestampValue"] = ahoraIso();
  }

  String body;
  serializeJson(doc, body);

  String payload;
  int code = httpRequest("PATCH", url, body, idToken, payload);

  return code == 200;
}

String firestoreDocumentUrl(String documentPath) {
  String url = "https://firestore.googleapis.com/v1/projects/";
  url += FIREBASE_PROJECT_ID;
  url += "/databases/(default)/documents/";
  url += documentPath;
  return url;
}

int httpRequest(String method, String url, String body, String bearerToken, String &payload) {
  HTTPClient https;
  https.setTimeout(8000);

  if (!https.begin(*secureClient, url)) {
    payload = "";
    return -1;
  }

  https.addHeader("Content-Type", "application/json");

  if (bearerToken.length() > 0) {
    https.addHeader("Authorization", "Bearer " + bearerToken);
  }

  int code = -1;

  if (method == "GET") {
    code = https.GET();
  } else if (method == "POST") {
    code = https.POST(body);
  } else if (method == "PATCH") {
    code = https.sendRequest("PATCH", body);
  }

  payload = https.getString();
  https.end();

  return code;
}

String ahoraIso() {
  time_t now = time(nullptr);

  if (now < 1700000000) {
    return "2026-01-01T00:00:00Z";
  }

  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);

  char buffer[25];

  snprintf(
    buffer,
    sizeof(buffer),
    "%04d-%02d-%02dT%02d:%02d:%02dZ",
    timeinfo.tm_year + 1900,
    timeinfo.tm_mon + 1,
    timeinfo.tm_mday,
    timeinfo.tm_hour,
    timeinfo.tm_min,
    timeinfo.tm_sec
  );

  return String(buffer);
}

void enviarEstadoAlMega(int plazaNum, String status) {
  Serial.print(F("FS:"));
  Serial.print(plazaNum);
  Serial.print(F(":"));
  Serial.println(status);
}

void enviarAbrir(String tipo, int plazaNum) {
  Serial.print(F("OPEN:"));
  Serial.print(tipo);
  Serial.print(F(":"));
  Serial.println(plazaNum);
}

void enviarDenegado(String motivo) {
  Serial.print(F("DENY:"));
  Serial.println(motivo);
