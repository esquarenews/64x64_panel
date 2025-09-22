#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_GFX.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// Message and rendering params
#ifndef MESSAGE
#define MESSAGE       "Music is a revelation higher than all wisdom and philosophy"
#endif
#ifndef MATRIX_ROTATION
#define MATRIX_ROTATION 2   // keep your working orientation; change 0..3 if desired
#endif
#ifndef SCROLL_SPEED
#define SCROLL_SPEED   1    // pixels per frame (1–2 is smooth)
#endif
#ifndef FRAME_DELAY_MS
#define FRAME_DELAY_MS 180   // delay in ms between frames
#endif
#ifndef BRIGHTNESS
#define BRIGHTNESS     40   // 0–255
#endif
#ifndef GAP_PIXELS
#define GAP_PIXELS     8    // gap between repeated messages
#endif


// =====================
// Panel configuration (saved default)
// =====================
#define LED_PIN       5
#define MATRIX_W      32
#define MATRIX_H      8
#define NUM_PIXELS    (MATRIX_W * MATRIX_H)

const char* ssid = "MoreHumanThanHuman";
const char* password = "neverteachapigtosing";

WebServer server(80);

String message = MESSAGE;  // global so loop() can use it
String prevMessage = message;

void handleRoot() {
  Serial.println("[HTTP] GET /");
  IPAddress staIP = WiFi.localIP();
  IPAddress apIP  = WiFi.softAPIP();
  String html = "<h3>ESP32 Matrix</h3>"
                "<p>STA IP: http://" + staIP.toString() + "/</p>"
                "<p>AP  IP: http://" + apIP.toString()  + "/</p>"
                "<form action='/set' method='get'>"
                "<input name='msg' maxlength=64>"
                "<input type='submit' value='Set Message'>"
                "</form>"
                "<p>Current message: " + message + "</p>";
  server.send(200, "text/html", html);
}

void handleSet() {
  Serial.println("[HTTP] GET /set");
  if (server.hasArg("msg")) {
    message = server.arg("msg");
    Serial.println("New message: " + message);
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleHealth() {
  server.send(200, "text/plain", String("OK ") + String(millis()));
}

void handleNotFound() {
  String uri = server.uri();
  Serial.print("404: "); Serial.println(uri);
  server.send(404, "text/plain", "Not found: " + uri);
}

// =====================
// NeoPixel strip
// =====================
Adafruit_NeoPixel strip(NUM_PIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

// =====================
// Coordinate helpers
// =====================
static inline void rotateXY(int16_t &x, int16_t &y) {
  int16_t nx, ny;
  switch (MATRIX_ROTATION) {
    default:
    case 0: return;                      // 0°
    case 1: nx = y; ny = MATRIX_W - 1 - x; x = nx; y = ny; return;          // 90°
    case 2: x = MATRIX_W - 1 - x; y = MATRIX_H - 1 - y; return;             // 180°
    case 3: nx = MATRIX_H - 1 - y; ny = x; x = nx; y = ny; return;          // 270°
  }
}

// Saved mapping: column-serpentine, first pixel bottom-left
static inline uint16_t XY(uint16_t x, uint16_t y) {
  if (x % 2 == 0) {
    return x * MATRIX_H + y;                    // even column: top->bottom
  } else {
    return x * MATRIX_H + (MATRIX_H - 1 - y);   // odd column: bottom->top
  }
}

static inline uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// =====================
// Minimal GFX surface using our XY mapping
// =====================
class MatrixGFX : public Adafruit_GFX {
public:
  MatrixGFX(uint16_t w, uint16_t h) : Adafruit_GFX(w, h) {}

  void drawPixel(int16_t x, int16_t y, uint16_t c565) override {
    int16_t rx = x, ry = y;              // rotate first
    rotateXY(rx, ry);
    if (rx < 0 || ry < 0 || rx >= width() || ry >= height()) return;

    uint8_t r5 = (c565 >> 11) & 0x1F;
    uint8_t g6 = (c565 >> 5)  & 0x3F;
    uint8_t b5 =  c565        & 0x1F;
    uint8_t r = (r5 * 255 + 15) / 31;
    uint8_t g = (g6 * 255 + 31) / 63;
    uint8_t b = (b5 * 255 + 15) / 31;
    strip.setPixelColor(XY((uint16_t)rx, (uint16_t)ry), strip.Color(r, g, b));
  }

  void fillScreen(uint16_t c565) override {
    uint8_t r5 = (c565 >> 11) & 0x1F;
    uint8_t g6 = (c565 >> 5)  & 0x3F;
    uint8_t b5 =  c565        & 0x1F;
    uint8_t r = (r5 * 255 + 15) / 31;
    uint8_t g = (g6 * 255 + 31) / 63;
    uint8_t b = (b5 * 255 + 15) / 31;
    uint32_t c = strip.Color(r, g, b);
    for (uint16_t i = 0; i < NUM_PIXELS; i++) strip.setPixelColor(i, c);
  }
};

MatrixGFX gfx(MATRIX_W, MATRIX_H);

// Precomputed text metrics
static int16_t text_x1, text_y1; static uint16_t text_w, text_h;
static int scrollX;   // current x offset for the leftmost text instance
static unsigned long lastFrameMs = 0;
static unsigned long lastBeat = 0;

void setup() {
  Serial.begin(115200);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("esp32-matrix");
  WiFi.begin(ssid, password);
while (WiFi.status() != WL_CONNECTED) {
  delay(500);
  Serial.print(".");
}
Serial.println("\n============================");
Serial.println(" WiFi connected");
Serial.print(" IP: http://"); Serial.println(WiFi.localIP());
Serial.println("============================\n");

  // Start a fallback Access Point so you can connect directly if STA HTTP is blocked
  const char* apSsid = "esp32-matrix";
  const char* apPass = "matrix123";    // 8+ chars required
  bool apOK = WiFi.softAP(apSsid, apPass);
  IPAddress apIP = WiFi.softAPIP();
  Serial.print("AP mode "); Serial.print(apOK ? "started" : "failed");
  Serial.print(" — SSID: "); Serial.print(apSsid);
  Serial.print("  PASS: "); Serial.print(apPass);
  Serial.print("  IP: http://"); Serial.println(apIP);

  server.on("/", handleRoot);
server.on("/set", handleSet);
server.on("/health", handleHealth);
  server.onNotFound(handleNotFound);
server.begin();
  Serial.println("HTTP server started on port 80");

  if (MDNS.begin("esp32-matrix")) {
    Serial.println("mDNS started: http://esp32-matrix.local/");
  } else {
    Serial.println("mDNS start failed");
  }
  MDNS.addService("http", "tcp", 80);
  // Note: mDNS will work on STA; many clients don’t resolve .local on AP mode

  strip.begin();
  strip.setBrightness(BRIGHTNESS);
  strip.show();

  gfx.setTextWrap(false);
  gfx.setTextSize(1);
  gfx.setTextColor(color565(255, 255, 255)); // white text

  // Measure once
  gfx.getTextBounds(message.c_str(), 0, 0, &text_x1, &text_y1, &text_w, &text_h);

  // Start the scroll just off the right edge
  scrollX = MATRIX_W;

  delay(5000);
  Serial.println("WiFi connected");
Serial.print("ESP32 IP Address: http://");
Serial.println(WiFi.localIP());
}

void loop() {
  server.handleClient();
  yield();

  unsigned long now = millis();
  if (now - lastBeat > 5000) {
    lastBeat = now;
    Serial.println("[loop] HTTP servicing OK");
  }

  if (message != prevMessage) {
    prevMessage = message;
    gfx.getTextBounds(message.c_str(), 0, 0, &text_x1, &text_y1, &text_w, &text_h);
    scrollX = MATRIX_W;  // restart scroll from the right edge
  }

  // Render at most once per FRAME_DELAY_MS; otherwise just keep servicing HTTP
  if (now - lastFrameMs < FRAME_DELAY_MS) return;
  lastFrameMs = now;

  // Red background each frame
  gfx.fillScreen(color565(0, 0, 150));

  // Baseline: vertically center using bounds offsets
  int cy = (MATRIX_H - (int)text_h) / 2 - text_y1;

  // Draw first copy
  gfx.setCursor(scrollX - text_x1, cy);
  gfx.print(message.c_str());

  // Draw wrapped copy to create seamless scrolling
  int totalSpan = (int)text_w + GAP_PIXELS;
  int nextX = scrollX + totalSpan;
  if (nextX - text_x1 < MATRIX_W) {
    gfx.setCursor(nextX - text_x1, cy);
    gfx.print(message.c_str());
  }

  strip.show();
  yield();

  // Advance scroll
  scrollX -= SCROLL_SPEED;
  if (scrollX <= -totalSpan) {
    scrollX += totalSpan;
  }
}
