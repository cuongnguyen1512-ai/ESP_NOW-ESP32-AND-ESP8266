#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DHT.h>
#include <SHT31.h>
#include <Wire.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <espnow.h>

#define ESP_NOW_CHANNEL 1
#define BUTTON 0
#define SENSOR_PACKET_MAGIC 0xA5

const char* ssid = "ESP8266_config";
const char* password = "12345678";
const int gpioList[] = {0, 2};
const int GPIO_COUNT = sizeof(gpioList)/sizeof(gpioList[0]);
bool gpioConfigured = false;
bool registrationComplete = false;
unsigned long lastRegisterAttempt = 0;
const unsigned long REGISTER_RETRY_MS = 2000;
volatile bool master_register = false;
volatile bool connected = false;
bool ledState = false;
String type = "";
int sensor = 0;
DHT* dht11 = nullptr;
DHT* dhtAM = nullptr;
unsigned long last_press = 0;
unsigned long debounce_time = 500;
unsigned long lastSend = 0;
const unsigned long sendInterval = 5000;
uint8_t esp32MAC[6];
uint8_t broadcastAddress[] ={0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

ESP8266WebServer server(80);

struct Config {
  int gpioMode[10];
  int shtSDA;
  int shtSCL;
  int dht11Pin;
  int dht12SDA;
  int dht12SCL;
  int amPin;
  int ledPin = -1;
} config;

typedef enum {
  DEVICE_DHT11,
  DEVICE_AM2303,
  DEVICE_SHT3X,
  DEVICE_LED
} DeviceType;

typedef struct {
  uint8_t mac[6];        // địa chỉ MAC của ESP8266
  DeviceType type;       // loại thiết bị
  float temperature;     // dữ liệu cảm biến
  float humidity;        // dữ liệu cảm biến
  bool ledState;         // trạng thái LED 
} PeerDevice;

typedef struct __attribute__((packed)) {
  uint8_t magic; 
  uint8_t mac[6];
  uint8_t type;
  float temperature;
  float humidity;
  bool ledState;
} SensorPacket;


void initLED(){
  if(type != "DEVICE_LED"){
    return;
  }
  if(config.ledPin < 0){
    return;
  }
  pinMode(config.ledPin,OUTPUT);
  digitalWrite(config.ledPin,LOW);
  ledState=false;
  Serial.println("Init LED OK");
}

void initDHT11() {
  if (config.dht11Pin >= 0 && type == "DEVICE_DHT11") {
    dht11 = new DHT(config.dht11Pin, DHT11);
    dht11->begin();
    delay(2000);
    Serial.println("Khởi tạo DHT11 tại GPIO" + String(config.dht11Pin));
  }
}

void initGPIO() {
  for (int i = 0; i < GPIO_COUNT; i++) {
    pinMode(gpioList[i], config.gpioMode[i]);
  }
}

SHT31 sht31 = SHT31();
void initSHT() {
  if (config.shtSDA >= 0 && config.shtSCL >= 0 && config.shtSDA != config.shtSCL && type == "DEVICE_SHT3X") {
    Wire.begin(config.shtSDA, config.shtSCL);
    if (sht31.begin()) {
      Serial.println("Khởi tạo SHT3x thành công");
    } else {
      Serial.println("Không tìm thấy SHT3x trên bus I2C!");
    }
  }
}

void initAM2303() {
  if (config.amPin >= 0 && type == "DEVICE_AM2303") {
    dhtAM = new DHT(config.amPin, DHT22);
    dhtAM->begin();
    delay(2000);
    Serial.println("Khởi tạo AM2303 tại GPIO" + String(config.amPin));
  }
}

void initAll() {
  initGPIO();
  initLED();
  initDHT11();
  initSHT();
  initAM2303();
}

void LED_ON(){
  if(type != "DEVICE_LED"){
    return;
  }
  digitalWrite(config.ledPin,HIGH);
  ledState=true;
}

void LED_OFF(){
  if(type != "DEVICE_LED"){
      return;
  }
  digitalWrite(config.ledPin,LOW);
  ledState=false;
}

void LED_TOGGLE(){
  if(ledState){
    LED_OFF();
  }
  else{
    LED_ON();
  }
}

void sendRegistrationBroadcast() {
  if (type == "DEVICE_DHT11") {
    const char msg[] = "REGISTER_DHT11";
    esp_now_send(broadcastAddress, (uint8_t *)msg, sizeof(msg));
  } else if (type == "DEVICE_AM2303") {
    const char msg[] = "REGISTER_AM2303";
    esp_now_send(broadcastAddress, (uint8_t *)msg, sizeof(msg));
  } else if (type == "DEVICE_SHT3X") {
    const char msg[] = "REGISTER_SHT3X";
    esp_now_send(broadcastAddress, (uint8_t *)msg, sizeof(msg));
  } else if (type == "DEVICE_LED") {
    const char msg[] = "REGISTER_LED";
    esp_now_send(broadcastAddress, (uint8_t *)msg, sizeof(msg));
  }
}

void button_to_send_broadcast() {
  if (master_register || gpioConfigured) return;  // Đã nhận ACK từ ESP32 thì không gửi lại.
  if (digitalRead(BUTTON) == LOW) {
    unsigned long now = millis();
    if (now - last_press >= debounce_time) {
      last_press = now;
      sendRegistrationBroadcast();
      Serial.println("Da gui broadcast dang ky bang nut BOOT.");
    }
  }
}

void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len){
  if (len > 249){
    len = 249;
  }
  char msg[250];
  memcpy(msg, incomingData, len); 
  msg[len] = '\0';
    
  Serial.printf("Receive from: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("Message: %s\n", msg);

  if (strcmp(msg, "REGISTER_NEW_ESP8266_DONE") == 0){
    memcpy(esp32MAC, mac, 6);
    if (!esp_now_is_peer_exist(esp32MAC)){
      if (esp_now_add_peer(esp32MAC, ESP_NOW_ROLE_COMBO, ESP_NOW_CHANNEL, NULL, 0) != 0){
        Serial.println("Khong the them ESP32 vao peer list.");
        return;
      }
    }
    master_register = true;
    registrationComplete = true;
    if (!gpioConfigured) {
      initAll();              
      gpioConfigured = true;
    }
    Serial.printf("Da luu MAC ESP32: %02X:%02X:%02X:%02X:%02X:%02X\\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
  else if(strcmp(msg,"CONNECT")==0){
    connected=true;
    Serial.println("Gateway Connected");
  }
  else if(strcmp(msg,"DISCONNECT")==0){
    connected=false;
    Serial.println("Gateway Disconnected");
  }
  else if(strcmp(msg,"LED:ON")==0){
    LED_ON();
  }
  else if(strcmp(msg,"LED:OFF")==0){
    LED_OFF();
  }
  else if(strcmp(msg,"LED:TOGGLE")==0){
    LED_TOGGLE();
  }
}

void OnDataSent(uint8_t *mac, uint8_t status) {
  Serial.println(status == 0 ? "[ESP8266 SEND: SUCCESS]" : "[ESP8266 SEND: FAILED!]");
}

void saveConfig(String json) {
  StaticJsonDocument<2048> doc;
  DeserializationError error = deserializeJson(doc, json);
  if (error) {
    Serial.println("Lỗi parse JSON!");
    return;
  }
  File file = LittleFS.open("/config.json", "w");
  if (!file) {
    Serial.println("Không mở được file để ghi!");
    return;
  }
  serializeJsonPretty(doc, file);
  file.close();
  Serial.println("Đã lưu cấu hình vào /config.json");
}
/*
void loadConfig() {
  File file = LittleFS.open("/config.json", "r");
  if (!file) {
    Serial.println("Không tìm thấy file config!");
    return;
  }
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    Serial.println("Lỗi đọc JSON!");
    return;
  }
  int i = 0;
  for (JsonObject obj : doc["digital"].as<JsonArray>()){
    int pin = obj["pin"];
    String mode = obj["mode"];
    if(mode == "OUTPUT"){
      config.gpioMode[i++] = OUTPUT;
      config.ledPin = pin;
      type = "DEVICE_LED";
    }
    else if(mode == "INPUT"){
    config.gpioMode[i++] = INPUT;
    }

    if (mode == "DHT11_IN"){
      config.dht11Pin = pin;
      sensor = 1;
      type = "DEVICE_DHT11";
    }
    else if (mode == "AM2303_IN"){
      config.amPin = pin;
      sensor = 2;
      type = "DEVICE_AM2303";
    }
    else if (mode == "SHT3x_SDA"){
      config.shtSDA = pin;
      sensor = 3;
      type = "DEVICE_SHT3X";
    }
    else if (mode == "SHT3x_SCL"){
      config.shtSCL = pin;
      sensor = 3;
      type = "DEVICE_SHT3X";
    }
  }
}
*/

void loadConfig() {
  File file = LittleFS.open("/config.json", "r");
  if (!file) {
    Serial.println("Không tìm thấy file config!");
    return;
  }

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    Serial.println("Lỗi đọc JSON!");
    return;
  }

  int i = 0;
  for (JsonObject obj : doc["digital"].as<JsonArray>()){
      int pin = obj["pin"];
      String mode = obj["mode"];

      if (mode == "OUTPUT") config.gpioMode[i] = OUTPUT;
      else if (mode == "INPUT") config.gpioMode[i] = INPUT;
      else config.gpioMode[i] = INPUT;   

      if (mode == "OUTPUT") { config.ledPin = pin; type = "DEVICE_LED"; }
      else if (mode == "DHT11_IN") { config.dht11Pin = pin; sensor = 1; type = "DEVICE_DHT11"; }
      else if (mode == "AM2303_IN") { config.amPin = pin; sensor = 2; type = "DEVICE_AM2303"; }
      else if (mode == "SHT3x_SDA") { config.shtSDA = pin; sensor = 3; type = "DEVICE_SHT3X"; }
      else if (mode == "SHT3x_SCL") { config.shtSCL = pin; sensor = 3; type = "DEVICE_SHT3X"; }

      i++;
  }
}

SensorPacket LED_Device(){
  SensorPacket dev;
  dev.magic = SENSOR_PACKET_MAGIC;
  WiFi.macAddress(dev.mac);
  dev.type = (uint8_t)DEVICE_LED;
  dev.temperature=0;
  dev.humidity=0;
  dev.ledState=ledState;
  return dev;
}

SensorPacket DHT11_sensor() {
  SensorPacket dev;
  dev.magic = SENSOR_PACKET_MAGIC;
  WiFi.macAddress(dev.mac);   
  dev.type = (uint8_t)DEVICE_DHT11;
  dev.ledState = false;

  if (!dht11) {
    dev.temperature = NAN;
    dev.humidity = NAN;
    return dev;
  }

  float h = dht11->readHumidity();
  float t = dht11->readTemperature();
  if (!isnan(h) && !isnan(t)){
    dev.temperature = t;
    dev.humidity = h;
    Serial.println("DHT11 -> Nhiệt độ: " + String(t) + "°C, Độ ẩm: " + String(h) + "%");
  } 
  else{
    dev.temperature = NAN;
    dev.humidity = NAN;
    Serial.println("Lỗi đọc DHT11!");
  }
  return dev;
}

SensorPacket AM2303_sensor(){
  SensorPacket dev;
  dev.magic = SENSOR_PACKET_MAGIC;
  WiFi.macAddress(dev.mac);
  dev.type = (uint8_t)DEVICE_AM2303;
  dev.ledState = false;

  if (!dhtAM){
    dev.temperature = NAN;
    dev.humidity = NAN;
    return dev;
  }

  float h = dhtAM->readHumidity();
  float t = dhtAM->readTemperature();
  if (!isnan(h) && !isnan(t)){
    dev.temperature = t;
    dev.humidity = h;
    Serial.println("AM2303 -> Nhiệt độ: " + String(t) + "°C, Độ ẩm: " + String(h) + "%");
  } 
  else{
    dev.temperature = NAN;
    dev.humidity = NAN;
    Serial.println("Lỗi đọc AM2303!");
  }
  return dev;
}

SensorPacket SHT3x_sensor(){
  SensorPacket dev;
  dev.magic = SENSOR_PACKET_MAGIC;
  WiFi.macAddress(dev.mac);
  dev.type = (uint8_t)DEVICE_SHT3X;
  dev.ledState = false;

  if (sht31.read()){
    float temp = sht31.getTemperature();
    float hum  = sht31.getHumidity();
    dev.temperature = temp;
    dev.humidity = hum;
    Serial.println("SHT3x -> Nhiệt độ: " + String(temp) + "°C, Độ ẩm: " + String(hum) + "%");
  } 
  else{
    dev.temperature = NAN;
    dev.humidity = NAN;
    Serial.println("Lỗi đọc dữ liệu từ SHT3x!");
  }
  return dev;
}

void sendData(SensorPacket data) {
  uint8_t result = esp_now_send(esp32MAC, (uint8_t*)&data, sizeof(data));
  if (result == 0){
    Serial.println("Gửi dữ liệu thành công!");
  } 
  else{
    Serial.println("Gửi dữ liệu thất bại! Mã lỗi: " + String(result));
  }
}

String getConfigPage() {
  return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
 <meta charset="UTF-8">
 <title>GPIO Config</title>
 <style>
    body { font-family: 'Segoe UI', Tahoma, sans-serif; background: #f4f6f9; }
    header { background: #4a90e2; color: white; padding: 15px; font-size: 20px; font-weight: bold; }
    .container { max-width: 600px; margin: 20px auto; background: #fff; padding: 20px; border-radius: 8px; }
    table { width: 100%; border-collapse: collapse; }
    th, td { padding: 10px; border-bottom: 1px solid #ddd; }
    select { padding: 6px; border: 1px solid #ccc; border-radius: 4px; }
    button.save { margin: 20px auto; padding: 12px 24px; background: #4a90e2; color: white; border: none; border-radius: 6px; }
    button.save:hover { background: #357ABD; }
 </style>
</head>
<body>
  <header>ESP8266 GPIO Configuration</header>
  <div class="container">
    <h3>GPIO Configuration</h3>
    <table>
      <tr><th>GPIO</th><th>Chức năng</th></tr>
      <tbody id="gpioTable"></tbody>
    </table>
    <button class="save" onclick="saveConfig()">💾 Save Configuration</button>
  </div>

  <script>
    const gpioList = [0,2];
    function populateGpioTable() {
      const tbody = document.getElementById('gpioTable');
      tbody.innerHTML = "";
      gpioList.forEach(pin => {
        const row = document.createElement('tr');
        row.innerHTML = `
          <td>GPIO${pin}</td>
          <td>
            <select id="mode${pin}">
              <option value="NONE">NONE</option>
              <option value="INPUT">INPUT</option>
              <option value="OUTPUT">OUTPUT</option>
              <option value="DHT11_IN">DHT11 IN</option>
              <option value="AM2303_IN">AM2303 IN</option>
              <option value="SHT3x_SCL">SHT3x SCL</option>
              <option value="SHT3x_SDA">SHT3x SDA</option>
            </select>
          </td>`;
        tbody.appendChild(row);
      });
    }
    function initPage() {
      populateGpioTable();
      fetch('/load')
        .then(r => r.json())
        .then(cfg => {
          cfg.digital.forEach(obj => {
            document.getElementById('mode' + obj.pin).value = obj.mode;
          });
        })
        .catch(err => console.log("Không có config mặc định"));
    }
    function saveConfig() {
      const config = {
        digital: gpioList.map(pin => ({
          pin: pin,
          mode: document.getElementById('mode' + pin).value
        }))
      };
      fetch('/save', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(config)
      }).then(r => r.json()).then(data => alert("Đã lưu cấu hình!"));
    }
    window.onload = initPage;
  </script>
</body>
</html>
)rawliteral";
}

void setup() {
  Serial.begin(57600);
  pinMode(BUTTON, INPUT_PULLUP);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ssid, password);
  Serial.println(WiFi.softAPIP());
  wifi_set_channel(ESP_NOW_CHANNEL);
  Serial.printf("ESP8266 MAC: %s\n", WiFi.macAddress().c_str());

  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW INIT: FAILED!");
    return;
  }

  Serial.println("ESP-NOW INIT: OK");
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_add_peer( broadcastAddress, ESP_NOW_ROLE_COMBO, ESP_NOW_CHANNEL, NULL, 0);

  esp_now_register_recv_cb(OnDataRecv);
  esp_now_register_send_cb(OnDataSent);

  if (!LittleFS.begin()) {
    Serial.println("Không thể khởi động LittleFS!");
    return;
  }

  loadConfig();

  server.on("/", HTTP_GET, [](){ server.send(200, "text/html", getConfigPage());});
  server.on("/save", HTTP_POST, [](){
    if(server.hasArg("plain")){
      String json = server.arg("plain");
      saveConfig(json);
      loadConfig();
      initAll();
      server.send(200, "application/json", "{\"status\":\"ok\"}");
    } 
    else {
      server.send(400, "application/json", "{\"error\":\"missing body\"}");
    }
  });

  server.on("/load", HTTP_GET, [](){
    File file = LittleFS.open("/config.json", "r");
    if (!file) {
      server.send(404, "application/json", "{\"error\":\"no config\"}");
      return;
    }
    String json = file.readString();
    file.close();
    server.send(200, "application/json", json);
  });
  server.begin();
}

void loop() {
  server.handleClient();
  button_to_send_broadcast();
  if(master_register && connected){
    if (millis() - lastSend >= sendInterval) {
      SensorPacket data;
      if(type == "DEVICE_DHT11"){ 
        data = DHT11_sensor();
      }
      else if(type == "DEVICE_AM2303"){
        data = AM2303_sensor();
      }
      else if(type == "DEVICE_SHT3X"){
        data = SHT3x_sensor();
      }
      else if(type == "DEVICE_LED"){
        data=LED_Device();
      }
      sendData(data);
      lastSend = millis();
    }
  }
}