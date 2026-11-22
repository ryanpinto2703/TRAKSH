#include <Arduino.h>
/*
 * ESP32-S3: HC-SR04 distance + rover drive (generic dual H-bridge, e.g. L298N / TB6612 style).
 * Joins ESP32-CAM-Local with fixed IP 192.168.4.50.
 *
 *   GET /distance                    -> {"distance_cm":42.3} or {"distance_cm":null}
 *   GET /drive?cmd=F|B|L|R|S&speed=N -> drives; N is 0-200 (capped)
 *
 * Safety built in:
 *   - Watchdog: motors stop if no /drive command arrives for 500 ms.
 *   - Obstacle guard: forward is blocked (and stopped mid-drive) below OBSTACLE_CM.
 *   - "S" always stops, and is accepted at any time.
 *
 * Wiring:
 *   HC-SR04: VCC 5V, GND GND, TRIG GPIO4, ECHO GPIO5 via 1k to GPIO5 + 2k from GPIO5 to GND.
 *   Motors:  LEFT IN1=GPIO10 IN2=GPIO11 EN=GPIO14 (PWM)
 *            RIGHT IN3=GPIO12 IN4=GPIO13 EN=GPIO15 (PWM)
 *   Motor driver and S3 must share GND. Motors run from their own battery, not the S3's 5V.
 *   Remove the EN jumpers on L298N if you use PWM on EN pins.
 */
#include <WiFi.h>
#include <WebServer.h>

static const char *WIFI_SSID = "ESP32-CAM-Local";
static const char *WIFI_PASS = "camera123";
static const IPAddress LOCAL_IP(192, 168, 4, 50);
static const IPAddress GATEWAY(192, 168, 4, 1);
static const IPAddress SUBNET(255, 255, 255, 0);

static const int TRIG_PIN = 4;
static const int ECHO_PIN = 5;
static const int LEFT_IN1 = 10, LEFT_IN2 = 11, LEFT_EN = 14;
static const int RIGHT_IN3 = 12, RIGHT_IN4 = 13, RIGHT_EN = 15;

static const int MAX_SPEED = 200;
static const float OBSTACLE_CM = 30.0f;
static const unsigned long WATCHDOG_MS = 500;
static const unsigned long ECHO_TIMEOUT_US = 30000;
static const int WINDOW = 5;
static const unsigned long SAMPLE_MS = 60;

WebServer server(80);
float window[WINDOW];
int filled = 0;
int head = 0;
float latestCm = -1;
unsigned long lastCmdMs = 0;
int leftCmd = 0, rightCmd = 0;

float readRawCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long us = pulseIn(ECHO_PIN, HIGH, ECHO_TIMEOUT_US);
  if (us == 0) return -1;
  float cm = us * 0.0343f / 2.0f;
  return (cm >= 2.0f && cm <= 400.0f) ? cm : -1;
}

float medianOfWindow() {
  float tmp[WINDOW];
  for (int i = 0; i < filled; i++) tmp[i] = window[i];
  for (int i = 1; i < filled; i++) {
    float v = tmp[i];
    int j = i - 1;
    while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; }
    tmp[j + 1] = v;
  }
  return tmp[filled / 2];
}

void setMotor(int in1, int in2, int en, int v) {
  if (v > 0) { digitalWrite(in1, HIGH); digitalWrite(in2, LOW); }
  else if (v < 0) { digitalWrite(in1, LOW); digitalWrite(in2, HIGH); }
  else { digitalWrite(in1, LOW); digitalWrite(in2, LOW); }
  analogWrite(en, constrain(abs(v), 0, 255));
}

void applyDrive(int l, int r) {
  leftCmd = l;
  rightCmd = r;
  setMotor(LEFT_IN1, LEFT_IN2, LEFT_EN, l);
  setMotor(RIGHT_IN3, RIGHT_IN4, RIGHT_EN, r);
}

bool obstacleAhead() {
  return latestCm > 0 && latestCm < OBSTACLE_CM;
}

void handleDistance() {
  String body = (latestCm > 0)
    ? "{\"distance_cm\":" + String(latestCm, 1) + "}"
    : "{\"distance_cm\":null}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void handleDrive() {
  String cmd = server.arg("cmd");
  cmd.toUpperCase();
  int speed = server.hasArg("speed") ? server.arg("speed").toInt() : 150;
  speed = constrain(speed, 0, MAX_SPEED);

  int l = 0, r = 0;
  if (cmd == "F") { l = speed; r = speed; }
  else if (cmd == "B") { l = -speed; r = -speed; }
  else if (cmd == "L") { l = -speed; r = speed; }
  else if (cmd == "R") { l = speed; r = -speed; }
  else { cmd = "S"; }

  String note = "";
  if (cmd == "F" && obstacleAhead()) { l = 0; r = 0; note = "obstacle"; }

  lastCmdMs = millis();
  applyDrive(l, r);

  String body = "{\"cmd\":\"" + cmd + "\",\"left\":" + String(l) + ",\"right\":" + String(r) +
                ",\"blocked\":\"" + note + "\"}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);
  pinMode(LEFT_IN1, OUTPUT); pinMode(LEFT_IN2, OUTPUT); pinMode(LEFT_EN, OUTPUT);
  pinMode(RIGHT_IN3, OUTPUT); pinMode(RIGHT_IN4, OUTPUT); pinMode(RIGHT_EN, OUTPUT);
  applyDrive(0, 0);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.config(LOCAL_IP, GATEWAY, SUBNET);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Joining ");
  Serial.print(WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/distance and /drive");
  } else {
    Serial.println("Wi-Fi not connected; motors stay off until a valid connection.");
  }

  server.on("/distance", HTTP_GET, handleDistance);
  server.on("/drive", HTTP_GET, handleDrive);
  server.begin();
}

void loop() {
  server.handleClient();

  static unsigned long lastSample = 0;
  if (millis() - lastSample >= SAMPLE_MS) {
    lastSample = millis();
    float d = readRawCm();
    if (d > 0) {
      window[head] = d;
      head = (head + 1) % WINDOW;
      if (filled < WINDOW) filled++;
      latestCm = medianOfWindow();
    } else {
      latestCm = -1;
    }
  }

  // Watchdog: stop if commands stop arriving.
  if ((leftCmd != 0 || rightCmd != 0) && millis() - lastCmdMs > WATCHDOG_MS) {
    applyDrive(0, 0);
  }
  // Obstacle guard while moving forward.
  if (leftCmd > 0 && rightCmd > 0 && obstacleAhead()) {
    applyDrive(0, 0);
  }
}
