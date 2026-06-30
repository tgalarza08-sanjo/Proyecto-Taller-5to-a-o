#include <Servo.h>
#include <Keypad.h>

// UrbaPark - Arduino Mega
// Controla sensores, LEDs, teclado y barrera.
// El Wemos lee/escribe Firestore y le manda al Mega los estados reales.

struct PlazaHardware {
  int trig;
  int echo;
  int verde;
  int amarillo;
  int rojo;
  bool ocupacionConfirmada;
  bool ocupacionPendiente;
  unsigned long inicioPendiente;
  int confirmaciones;
  String estadoFirestore;
};

PlazaHardware plazas[4] = {
  {22, 24, 27, 25, 23, false, false, 0, 0, "libre"},
  {26, 28, 31, 33, 29, false, false, 0, 0, "libre"},
  {30, 32, 37, 39, 35, false, false, 0, 0, "libre"},
  {34, 36, 45, 43, 41, false, false, 0, 0, "libre"}
};

const int trigFuera = 38;
const int echoFuera = 40;
const int trigDentro = 42;
const int echoDentro = 44;
const int pinBarrera = 46;

Servo barrera;

const int SERVO_CERRADO = 100;
const int SERVO_ABIERTO = 180;
const int SERVO_PASO = 2;
const int SERVO_DEMORA = 15;

const int DIST_AUTO = 4;
const int DIST_BARRERA = 20;

const int CONFIRMACIONES_REQUERIDAS = 3;
const unsigned long TIEMPO_MINIMO_CONFIRMACION = 200;
const unsigned long TIEMPO_CALENTAMIENTO = 3000;
const unsigned long TIMEOUT_CODIGO = 5000;
const unsigned long TIMEOUT_BARRERA = 7000;

bool sistemaListo = false;
unsigned long tiempoInicioSistema = 0;

bool barreraAbierta = false;
unsigned long tiempoAperturaBarrera = 0;
unsigned long tiempoPasoCompleto = 0;
bool autoCompletoPaso = false;

const byte ROWS = 4;
const byte COLS = 3;

char keys[ROWS][COLS] = {
  {'1', '2', '3'},
  {'4', '5', '6'},
  {'7', '8', '9'},
  {'*', '0', '#'}
};

byte rowPins[ROWS] = {11, 10, 9, 8};
byte colPins[COLS] = {7, 6, 5};

Keypad teclado = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

char codigoIngresado[5] = "";
byte codigoLen = 0;
unsigned long ultimaTecla = 0;

void setup() {
  Serial.begin(115200);
  Serial1.begin(9600);

  Serial.println();
  Serial.println("=== URBAPARK MEGA FINAL ===");
  Serial.println("Serial1 activo para Wemos a 9600 baudios.");

  for (int i = 0; i < 4; i++) {
    configurarPlaza(plazas[i]);
  }

  pinMode(trigFuera, OUTPUT);
  pinMode(echoFuera, INPUT);
  pinMode(trigDentro, OUTPUT);
  pinMode(echoDentro, INPUT);

  barrera.attach(pinBarrera);
  barrera.write(SERVO_CERRADO);

  tiempoInicioSistema = millis();

  for (int i = 0; i < 15; i++) {
    for (int p = 0; p < 4; p++) {
      medirDistancia(plazas[p].trig, plazas[p].echo);
      delay(20);
    }
  }

  for (int i = 0; i < 4; i++) {
    aplicarLeds(i);
  }

  Serial.println("Mega listo.");
}

void loop() {
  leerWemos();

  if (!sistemaListo) {
    if (millis() - tiempoInicioSistema >= TIEMPO_CALENTAMIENTO) {
      sistemaListo = true;
      Serial.println("Sistema estabilizado.");
    } else {
      for (int i = 0; i < 4; i++) {
        medirDistancia(plazas[i].trig, plazas[i].echo);
      }
      delay(50);
      return;
    }
  }

  for (int i = 0; i < 4; i++) {
    procesarSensorPlaza(i);
  }

  leerTeclado();
  controlarBarrera();

  delay(30);
}

void configurarPlaza(PlazaHardware &plaza) {
  pinMode(plaza.trig, OUTPUT);
  pinMode(plaza.echo, INPUT);
  pinMode(plaza.verde, OUTPUT);
  pinMode(plaza.amarillo, OUTPUT);
  pinMode(plaza.rojo, OUTPUT);
}

void procesarSensorPlaza(int index) {
  PlazaHardware &plaza = plazas[index];

  int distancia = medirDistancia(plaza.trig, plaza.echo);
  bool hayAuto = (distancia > 0 && distancia <= DIST_AUTO);

  if (hayAuto != plaza.ocupacionPendiente) {
    plaza.ocupacionPendiente = hayAuto;
    plaza.inicioPendiente = millis();
    plaza.confirmaciones = 1;
  } else {
    plaza.confirmaciones++;
  }

  bool confirmado = plaza.confirmaciones >= CONFIRMACIONES_REQUERIDAS &&
                    millis() - plaza.inicioPendiente >= TIEMPO_MINIMO_CONFIRMACION;

  if (confirmado && hayAuto != plaza.ocupacionConfirmada) {
    plaza.ocupacionConfirmada = hayAuto;
    enviarSensorAWemos(index, hayAuto);
    aplicarLeds(index);
  }
}

void enviarSensorAWemos(int index, bool ocupado) {
  Serial1.print(F("SENSOR:"));
  Serial1.print(index + 1);
  Serial1.print(F(":"));
  Serial1.println(ocupado ? F("1") : F("0"));

  Serial.print("Sensor plaza ");
  Serial.print(index + 1);
  Serial.print(" -> ");
  Serial.println(ocupado ? "ocupada" : "libre");
}

void aplicarLeds(int index) {
  PlazaHardware &plaza = plazas[index];

  bool rojo = plaza.ocupacionConfirmada || plaza.estadoFirestore == "ocupada";
  bool amarillo = !rojo && plaza.estadoFirestore == "reservada";
  bool verde = !rojo && !amarillo;

  digitalWrite(plaza.verde, verde ? HIGH : LOW);
  digitalWrite(plaza.amarillo, amarillo ? HIGH : LOW);
  digitalWrite(plaza.rojo, rojo ? HIGH : LOW);
}

int medirDistancia(int trig, int echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(2);

  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);

  long duracion = pulseIn(echo, HIGH, 30000);
  if (duracion == 0) return 999;

  return duracion * 0.034 / 2;
}

void leerTeclado() {
  char tecla = teclado.getKey();

  if (tecla) {
    if (tecla == '*') {
      limpiarCodigo();
      Serial.println("Codigo borrado.");
      return;
    }

    if (tecla == '#') {
      if (codigoLen == 4) {
        Serial1.print(F("CODE:"));
        Serial1.println(codigoIngresado);

        Serial.print("Codigo enviado al Wemos: ");
        Serial.println(codigoIngresado);
      } else {
        Serial.println("Codigo incompleto.");
      }

      limpiarCodigo();
      return;
    }

    if (tecla >= '0' && tecla <= '9' && codigoLen < 4) {
      codigoIngresado[codigoLen] = tecla;
      codigoLen++;
      codigoIngresado[codigoLen] = '\0';
      ultimaTecla = millis();

      Serial.print("Tecla: ");
      Serial.println(tecla);
    }
  }

  if (codigoLen > 0 && millis() - ultimaTecla > TIMEOUT_CODIGO) {
    limpiarCodigo();
    Serial.println("Codigo borrado por timeout.");
  }
}

void limpiarCodigo() {
  codigoLen = 0;
  codigoIngresado[0] = '\0';
}

void leerWemos() {
  while (Serial1.available()) {
    String dato = Serial1.readStringUntil('\n');
    dato.trim();

    if (dato.length() == 0) continue;

    Serial.print("Wemos -> Mega: ");
    Serial.println(dato);

    if (dato.startsWith("FS:")) {
      procesarEstadoFirestore(dato);
    } else if (dato.startsWith("OPEN:")) {
      abrirBarrera();
    } else if (dato.startsWith("DENY")) {
      Serial.print("Motivo rechazo: ");
      Serial.println(dato);
      parpadearRechazo();
    }
  }
}

void procesarEstadoFirestore(String dato) {
  int sep1 = dato.indexOf(':');
  int sep2 = dato.indexOf(':', sep1 + 1);

  if (sep1 == -1 || sep2 == -1) return;

  int plazaNum = dato.substring(sep1 + 1, sep2).toInt();
  String estado = dato.substring(sep2 + 1);

  if (plazaNum < 1 || plazaNum > 4) return;

  if (estado != "libre" && estado != "reservada" && estado != "ocupada") {
    return;
  }

  int index = plazaNum - 1;
  plazas[index].estadoFirestore = estado;

  if (estado == "libre") {
    plazas[index].ocupacionConfirmada = false;
    plazas[index].ocupacionPendiente = false;
    plazas[index].confirmaciones = 0;
  }

  aplicarLeds(index);

  Serial.print("Plaza ");
  Serial.print(plazaNum);
  Serial.print(" actualizada desde Firestore: ");
  Serial.println(estado);
}

void parpadearRechazo() {
  Serial.println("Codigo rechazado.");

  for (int r = 0; r < 2; r++) {
    for (int i = 0; i < 4; i++) {
      digitalWrite(plazas[i].rojo, HIGH);
      digitalWrite(plazas[i].verde, LOW);
      digitalWrite(plazas[i].amarillo, LOW);
    }

    delay(120);

    for (int i = 0; i < 4; i++) {
      aplicarLeds(i);
    }

    delay(120);
  }
}

void controlarBarrera() {
  if (!barreraAbierta) return;

  if (millis() - tiempoAperturaBarrera > TIMEOUT_BARRERA) {
    cerrarBarrera();
    return;
  }

  int dFuera = medirDistancia(trigFuera, echoFuera);
  int dDentro = medirDistancia(trigDentro, echoDentro);

  bool zonaLibre = (dFuera > DIST_BARRERA && dDentro > DIST_BARRERA);

  if (zonaLibre) {
    if (!autoCompletoPaso) {
      autoCompletoPaso = true;
      tiempoPasoCompleto = millis();
    } else if (millis() - tiempoPasoCompleto > 300) {
      cerrarBarrera();
    }
  } else {
    autoCompletoPaso = false;
  }
}

void abrirBarrera() {
  if (barreraAbierta) return;

  Serial.println("Abriendo barrera...");

  for (int pos = SERVO_CERRADO; pos <= SERVO_ABIERTO; pos += SERVO_PASO) {
    barrera.write(pos);
    delay(SERVO_DEMORA);
  }

  barrera.write(SERVO_ABIERTO);

  barreraAbierta = true;
  tiempoAperturaBarrera = millis();
  autoCompletoPaso = false;
}

void cerrarBarrera() {
  Serial.println("Cerrando barrera...");

  for (int pos = SERVO_ABIERTO; pos >= SERVO_CERRADO; pos -= SERVO_PASO) {
    barrera.write(pos);
    delay(SERVO_DEMORA);
  }

  barrera.write(SERVO_CERRADO);

  barreraAbierta = false;
  autoCompletoPaso = false;
}
