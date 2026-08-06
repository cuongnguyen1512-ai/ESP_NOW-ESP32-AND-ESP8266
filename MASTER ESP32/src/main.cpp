#include<WiFi.h>
#include<esp_now.h>
#include<esp_wifi.h>
#include <ESPAsyncWebServer.h>
#include<ArduinoJson.h>

#define ESP_NOW_CHANNEL 1
#define MAX_PEER_LIST 10

const char* ap_ssid = "ESP32_WEB";
const char* ap_password = "12345678";
volatile bool new_MAC_flag = false;
String wsBuffer="";
volatile bool wsFlag=false;

typedef enum{
  DEVICE_DHT11,
  DEVICE_AM2303,
  DEVICE_SHT3X,
  DEVICE_DS18B20,
  DEVICE_LED
} DeviceType;

typedef struct{
    uint8_t mac[6];
    DeviceType type;
    bool connected;      // đã connect chưa
    bool online;         // còn online không
    float temperature;
    float humidity;
    bool ledState;
    char lastBroadcast[24];
} PeerDevice;

typedef struct __attribute__((packed)){
  uint8_t mac[6];
  uint8_t type;
  float temperature;
  float humidity;
  bool ledState;
} SensorPacket;

PeerDevice peer_list[MAX_PEER_LIST];
int peer_count = 0;


AsyncWebServer server(80);
AsyncWebSocket ws("/ws");


const char index_html[] PROGMEM = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
    <meta charset="UTF-8">
    <title>ESP32 Dashboard</title>
    <style>
      body {
        margin: 0;
        font-family: Arial;
        display: flex;
        height: 100vh;
      }

      #sidebar {
        width: 260px;
        background: #2f3542;
        color: white;
        padding: 10px;
        overflow-y: auto;
      }

      #dashboard {
        flex: 1;
        background: #ecf0f1;
        padding: 15px;
        display: flex;
        flex-wrap: wrap;
        gap: 15px;
        align-content: flex-start;
      }

      .peerBtn {
        width: 100%;
        margin-bottom: 8px;
        padding: 10px;
        cursor: pointer;
        border: none;
        background: #57606f;
        color: white;
      }

      .peerBtn:hover {
        background: #747d8c;
      }

      .card {
        width: 280px;
        height: 160px;
        background: white;
        border-radius: 8px;
        padding: 10px;
        box-shadow: 0px 2px 8px rgba(0,0,0,.2);
      }
    </style>
  </head>

  <body>
    <div id="sidebar">
      <h2>ESP8266 List</h2>
      <div id="peerList"></div>
    </div>

    <div id="dashboard"></div>

    <script>
      let ws = new WebSocket("ws://" + location.host + "/ws");
      let devices = {};

      function sendLED(mac, state) {
        ws.send(JSON.stringify({
          cmd: "led",
          mac: mac,
          state: state
        }));
      }

      function addPeer(mac, broadcast) {
        const id = "btn_" + mac;
        let btn = document.getElementById(id);

        // Nếu MAC đã tồn tại, chỉ cập nhật nội dung broadcast.
        if (btn) {
          btn.textContent = mac + " -- " + broadcast;
          return;
        }

        btn = document.createElement("button");
        btn.className = "peerBtn";
        btn.id = id;
        btn.textContent = mac + " -- " + broadcast;

        btn.onclick = function () {
          ws.send(JSON.stringify({ cmd: "connect", mac: mac }));
        };

        document.getElementById("peerList").appendChild(btn);
      }

      function createCard(mac) {
        if (document.getElementById("card_" + mac)) return;

        let devType = "Unknown";
        if (devices[mac] && devices[mac].devType) devType = devices[mac].devType;

        let card = document.createElement("div");
        card.className = "card";
        card.id = "card_" + mac;

        card.innerHTML =
          "<h3>" + mac + "</h3>" +
          "<hr>" +
          "<p id='type_" + mac + "'><b>Thiết bị:</b> " + devType + "</p>" +
          "<button onclick=\"disconnectDevice('" + mac + "')\">Ngắt kết nối</button>" +
          "<div id='data_" + mac + "'>Đang chờ dữ liệu...</div>";

        document.getElementById("dashboard").appendChild(card);
      }

      function disconnectDevice(mac) {
        ws.send(JSON.stringify({ cmd: "disconnect", mac: mac }));
        const card = document.getElementById("card_" + mac);
        if (card) card.remove();
      }

      ws.onopen = function () {
        console.log("WS CONNECT");
      };

      ws.onmessage = function (event) {
        console.log(event.data);
        let obj = JSON.parse(event.data);

        if (obj.type == "peer_list") {
          obj.list.forEach(function (peer) {
            if (!devices[peer.mac]) devices[peer.mac] = {};
            devices[peer.mac].broadcast = peer.broadcast;
            addPeer(peer.mac, peer.broadcast);
          });
          return;
        }

        if (obj.type == "new_peer") {
          if (!devices[obj.mac]) devices[obj.mac] = {};
          devices[obj.mac].devType = obj.devType;
          addPeer(obj.mac, obj.broadcast || "");
          return;
        }

        if (obj.type === "connection") {
          if (obj.connected) {
            createCard(obj.mac);
          } else {
            const card = document.getElementById("card_" + obj.mac);
            if (card) card.remove();
          }
          return;
        }

        if (obj.type == "update") {
          let card = document.getElementById("card_" + obj.mac);
          // Chỉ cập nhật card khi người dùng đã kết nối node này.
          if (!card) return;

          if (obj.devType == "LED") {
            card.innerHTML =
              "<h3>" + obj.mac + "</h3>" +
              "<hr>" +
              "<p><b>Thiết bị:</b> LED</p>" +
              "<p>" + obj.data + "</p>" +
              "<button onclick=\"sendLED('" + obj.mac + "','ON')\">ON</button> " +
              "<button onclick=\"sendLED('" + obj.mac + "','OFF')\">OFF</button> " +
              "<button onclick=\"disconnectDevice('" + obj.mac + "')\">Ngắt kết nối</button>";
          } else {
            card.innerHTML =
              "<h3>" + obj.mac + "</h3>" +
              "<hr>" +
              "<p><b>Thiết bị:</b> " + obj.devType + "</p>" +
              "<p>" + obj.data + "</p>" +
              "<button onclick=\"disconnectDevice('" + obj.mac + "')\">Ngắt kết nối</button>";
          }
          return;
        }
      };
    </script>
  </body>
  </html>
)rawliteral";

String macToString(const uint8_t *mac){
  char str[18];
  sprintf(str,"%02X:%02X:%02X:%02X:%02X:%02X", mac[0],mac[1],mac[2], mac[3],mac[4],mac[5]);
  return String(str);
}

int findPeer(String mac){
  for(int i=0;i<peer_count;i++){
    if(macToString(peer_list[i].mac)==mac){
      return i;
    }
  }
  return -1;
}

int findPeerByMac(const uint8_t *mac){
  for (int i = 0; i < peer_count; ++i){
    if (memcmp(peer_list[i].mac, mac, 6) == 0){
      return i;
    }
  }
  return -1;
}

void sendTextToPeer(int index, const char *text) {
  esp_now_send(peer_list[index].mac, (const uint8_t *)text, strlen(text) + 1);
}

void sendConnectionState(int index) {
  String json = "{\"type\":\"connection\",\"mac\":\"";
  json += macToString(peer_list[index].mac);
  json += "\",\"connected\":";
  json += peer_list[index].connected ? "true}" : "false}";
  ws.textAll(json);
}

void connectPeer(String mac) {
  int index = findPeer(mac);
  if (index < 0){
    return;
  }
  sendTextToPeer(index, "CONNECT");
  peer_list[index].connected = true;
  sendConnectionState(index);
}

void disconnectPeer(String mac) {
  int index = findPeer(mac);
  if (index < 0){
    return;
  }
  sendTextToPeer(index, "DISCONNECT");
  peer_list[index].connected = false;
  sendConnectionState(index);
}

String makePeerListJson() {
  String json = "{\"type\":\"peer_list\",\"list\":[";
  for (int i = 0; i < peer_count; i++){
    if (i > 0) json += ",";
    json += "{\"mac\":\"";
    json += macToString(peer_list[i].mac);
    json += "\",\"broadcast\":\"";
    json += peer_list[i].lastBroadcast;
    json += "\"}";
  }
  json += "]}";
  return json;
}

void sendPeerList() {
  ws.textAll(makePeerListJson());
}

void sendNewPeer(const uint8_t *mac, DeviceType type){
  String json="{\"type\":\"new_peer\",\"mac\":\"";
  json+=macToString(mac);
  json+="\",\"devType\":\"";

  if(type == DEVICE_DHT11){
    json += "DHT11";
  }
  else if(type == DEVICE_AM2303){
    json += "AM2303";
  }
  else if(type == DEVICE_SHT3X){
    json += "SHT3X";
  }
  else if(type == DEVICE_DS18B20){
    json += "DS18B20";
  }
  else if(type == DEVICE_LED){
    json += "LED";
  }
  json+="\"}";
  ws.textAll(json);
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len){
  Serial.printf("WS event=%d, client=%u\n", (int)type, client->id());
  if (type == WS_EVT_CONNECT) {
    Serial.println("WEB CONNECT");
    String json = makePeerListJson();
    Serial.println("Send to web: " + json);
    client->text(json);
    return;
  }
  if (type == WS_EVT_DISCONNECT) {
    Serial.println("WEB DISCONNECT");
    return;
  }
  else if(type==WS_EVT_DATA){
    AwsFrameInfo *info=(AwsFrameInfo*)arg;
    if(info->opcode==WS_TEXT){
      data[len]=0;
      StaticJsonDocument<256> doc;
      if(deserializeJson(doc,(char*)data)){
        return;
      }
      String cmd=doc["cmd"];
      String mac=doc["mac"];
      String state=doc["state"];
      if(cmd=="connect"){
        connectPeer(mac);
      }
      if(cmd=="disconnect"){
        disconnectPeer(mac);
      }
      if(cmd=="led"){
        int index=findPeer(mac);
        if(index>=0){
          String message;
          if(state=="ON"){
            message="LED:ON";
          }
          else{
            message="LED:OFF";
          }
          esp_now_send(peer_list[index].mac,(uint8_t*)message.c_str(),message.length()+1);
        }
      }
    }
  }   
}

void initWebSocket(){
    ws.onEvent(onWsEvent);
}

void initWebServer(){
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){ request->send_P(200,"text/html",index_html); });
  server.addHandler(&ws);
  server.begin();
  Serial.println("WEB START");
}

void print_peer_list(){
  Serial.println("========== Peer List ==========");
  for (int i = 0; i < peer_count; i++){
    Serial.printf("%2d : %02X:%02X:%02X:%02X:%02X:%02X\n", i + 1, peer_list[i].mac[0], peer_list[i].mac[1], peer_list[i].mac[2], peer_list[i].mac[3], peer_list[i].mac[4], peer_list[i].mac[5]);
  }
  Serial.println("===============================");
}

bool add_new_peer(const uint8_t *mac, DeviceType type){
  if(peer_count >= MAX_PEER_LIST){
    return false;
  }
  if(esp_now_is_peer_exist(mac)){
    return false;
  }
  esp_now_peer_info_t peerInfo{};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = ESP_NOW_CHANNEL;
  peerInfo.encrypt = false;

  if(esp_now_add_peer(&peerInfo) == ESP_OK){
    memcpy(peer_list[peer_count].mac, mac, 6);
    peer_list[peer_count].type = type;
    peer_list[peer_count].temperature = 0;
    peer_list[peer_count].humidity = 0;
    peer_list[peer_count].ledState = false;
    peer_list[peer_count].connected=false;
    peer_list[peer_count].online=true;
    strncpy(peer_list[peer_count].lastBroadcast,"UNKNOWN", sizeof(peer_list[peer_count].lastBroadcast));
    peer_count++;
    return true;
  }
  return false;
}

bool save_new_peer(const uint8_t *mac, DeviceType type){
  if(peer_count >= MAX_PEER_LIST){
    return false;
  }
  if(esp_now_is_peer_exist(mac)){
    return false;
  }
  memcpy(peer_list[peer_count].mac, mac, 6);
  peer_list[peer_count].type = type;
  peer_list[peer_count].temperature = 0;
  peer_list[peer_count].humidity = 0;
  peer_list[peer_count].ledState = false;
  peer_list[peer_count].connected=false;
  peer_list[peer_count].online=true;
  peer_count++;
  return true;
}

void sendUpdateToWeb(const uint8_t *mac, const char *msg){
  String json="{\"type\":\"update\",\"mac\":\"";
  json+=macToString(mac);
  json+="\",\"data\":\"";
  json+=msg;
  json+="\"}";
  ws.textAll(json);
}

void update_peer_data(const uint8_t *mac, const char *msg){
  for(int i=0; i<peer_count; i++){
    if(memcmp(peer_list[i].mac, mac, 6) == 0){
      if(peer_list[i].type == DEVICE_DHT11){
        float t,h;
        sscanf(msg, "TEMP:%f,HUM:%f", &t, &h);
        peer_list[i].temperature = t;
        peer_list[i].humidity = h;
      }
      else if(peer_list[i].type == DEVICE_LED){
        peer_list[i].ledState = (strcmp(msg,"LED:ON")==0);
      }
      sendUpdateToWeb(mac, msg);
      break;
    }
  }
}


void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len){
  if(len == sizeof(SensorPacket)){
    SensorPacket dev;
    memcpy(&dev, incomingData, sizeof(dev));
    Serial.printf("Recv from %s -> Temp=%.2f, Hum=%.2f\n", macToString(mac).c_str(), dev.temperature, dev.humidity);
    // gửi JSON update lên WebSocket
    int index = findPeerByMac(mac);
    if (index < 0 || !peer_list[index].connected) return;
    String json = "{\"type\":\"update\",\"mac\":\"" + macToString(mac) + "\",";
    json += "\"devType\":\"";

    if(dev.type == DEVICE_DHT11){
      json += "DHT11";
    }
    else if(dev.type == DEVICE_AM2303){
      json += "AM2303";
    }
    else if(dev.type == DEVICE_SHT3X){
      json += "SHT3X";
    }
    else if(dev.type == DEVICE_DS18B20){
      json += "DS18B20";
    }
    else if(dev.type == DEVICE_LED){
      json += "LED";
    }
    json += "\",\"data\":\"";

    if(dev.type == DEVICE_DHT11 || dev.type == DEVICE_AM2303 || dev.type == DEVICE_SHT3X){
      json += "Nhiệt độ: " + String(dev.temperature) + "°C, Độ ẩm: " + String(dev.humidity) + "%";
    } 
    else if(dev.type == DEVICE_DS18B20){
      json += "Nhiệt độ: " + String(dev.temperature) + "°C";
    }
    else if(dev.type == DEVICE_LED){
      json += dev.ledState ? "LED:ON" : "LED:OFF";
    }
    json += "\"}";
    wsBuffer=json;
    wsFlag=true;
    } 
    else {
      if (len <= 0){
        return;
      }
      char msg[251];
      int textLen = min(len, 250);
      memcpy(msg, incomingData, textLen);
      msg[textLen] = '\0';
      
      if(strcmp(msg, "REGISTER_DHT11") == 0){
        int index = findPeerByMac(mac);
        if(index < 0 && add_new_peer(mac, DEVICE_DHT11)){
          index = peer_count - 1;
          sendPeerList();            
        }
        if(index >= 0){
          strncpy(peer_list[index].lastBroadcast, "REGISTER_DHT11", sizeof(peer_list[index].lastBroadcast));
          sendPeerList();
          sendTextToPeer(index, "REGISTER_NEW_ESP8266_DONE");
        }
      } 
      else if(strcmp(msg, "REGISTER_AM2303") == 0){
        int index = findPeerByMac(mac);
        if(index < 0 && add_new_peer(mac, DEVICE_AM2303)){
          index = peer_count - 1;
          sendPeerList();             
        }
        if(index >= 0){
          strncpy(peer_list[index].lastBroadcast, "REGISTER_AM2303", sizeof(peer_list[index].lastBroadcast));
          sendPeerList();
          sendTextToPeer(index, "REGISTER_NEW_ESP8266_DONE");
        }
      } 
      else if(strcmp(msg, "REGISTER_SHT3X") == 0){
        int index = findPeerByMac(mac);
        if (index < 0 && add_new_peer(mac, DEVICE_SHT3X)){
          index = peer_count - 1;
          sendPeerList();             
        }
        if (index >= 0){
          strncpy(peer_list[index].lastBroadcast, "REGISTER_SHT3X", sizeof(peer_list[index].lastBroadcast));
          sendPeerList();
          sendTextToPeer(index, "REGISTER_NEW_ESP8266_DONE");
        }
      } 
      else if(strcmp(msg, "REGISTER_LED") == 0){
        int index = findPeerByMac(mac);
        if(index < 0 && add_new_peer(mac, DEVICE_LED)){
          index = peer_count - 1;
          sendPeerList();             
        }
        if(index >= 0){
          strncpy(peer_list[index].lastBroadcast, "REGISTER_LED", sizeof(peer_list[index].lastBroadcast));
          sendPeerList();
          sendTextToPeer(index, "REGISTER_NEW_ESP8266_DONE");
        }
      } 
      else if(strcmp(msg, "REGISTER_DS18B20") == 0){
        int index = findPeerByMac(mac);
        if(index < 0 && add_new_peer(mac, DEVICE_DS18B20)){
          index = peer_count - 1;
          sendPeerList();
        }
        if(index >= 0){
          // Cho phep node doi loai cam bien ma khong can xoa peer cu.
          peer_list[index].type = DEVICE_DS18B20;
          strncpy(peer_list[index].lastBroadcast, "REGISTER_DS18B20", sizeof(peer_list[index].lastBroadcast) - 1);
          peer_list[index].lastBroadcast[sizeof(peer_list[index].lastBroadcast) - 1] = '\0';
          sendPeerList();
          sendTextToPeer(index, "REGISTER_NEW_ESP8266_DONE");
        }
      }
      else{
        update_peer_data(mac, msg);
    }
  }
}


void onDataSent(const uint8_t *mac, esp_now_send_status_t status){
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "[ESP32 SEND SUCCESS]" : "[ESP32 SEND FAILED]");
}

void send_command(uint8_t *mac,String cmd){
    esp_now_send(mac, (uint8_t*)cmd.c_str(), cmd.length()+1);
} 

void setup(){
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid, ap_password);
  Serial.println(WiFi.softAPIP());

  if (esp_wifi_set_channel(ESP_NOW_CHANNEL, WIFI_SECOND_CHAN_NONE) == ESP_OK){
    Serial.println("WIFI CHANNEL: 1");
  } 
  else{
    Serial.println(" SET CHANNEL FAILED!");
  }

  Serial.printf("ESP32 MAC: %s\n", WiFi.macAddress().c_str());

  if (esp_now_init() != ESP_OK){
    Serial.println("ESP-NOW INIT: FAILED!");
    return;
  }
  Serial.println("ESP-NOW INIT: OK");
  print_peer_list();

  esp_now_register_recv_cb(onDataRecv);
  esp_now_register_send_cb(onDataSent);

  initWebSocket();
  initWebServer();
}

void loop(){
  ws.cleanupClients();
  if(wsFlag){
    ws.textAll(wsBuffer);
    wsFlag=false;
  }
}

