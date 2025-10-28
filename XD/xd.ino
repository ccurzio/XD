// XD - The Xfiltration Device
// by Christopher R. Curzio
//
// The XD is an advanced system for performing data exfiltration on endpoints
// that are otherwise secured against connecting external mass storage devices
// (such as USB flash drives or external hard drives) as well as 
// network-to-internet exfiltration. When connected to a computer, the XD
// presents itself as an ethernet adapter connected to a self-contained
// sandboxed network. The XD runs an internal webserver which, when accessed
// using a browser on the connected computer, provides the ability to upload
// files. The files are stored on a removable Micro SD card, allowing for easy
// retrieval on a separate device.

String xdver = "0.0.1";

#include "driver/gpio.h"
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SD_MMC.h>
#include <FS.h>
#include <ETH.h>
#include <IPAddress.h>
#include <WiFiUdp.h>
#include <WebServer.h>

#ifndef ETH_PHY_MDC
#define ETH_PHY_TYPE ETH_PHY_LAN8720
#if CONFIG_IDF_TARGET_ESP32
#define ETH_PHY_TYPE  ETH_PHY_LAN8720
// This needed to be changed for WT32-ETH01 clone boards
//#define ETH_PHY_ADDR  0
#define ETH_PHY_ADDR  1
#define ETH_PHY_MDC   23
#define ETH_PHY_MDIO  18
// We hit this pin later to ensure power stability, so let's define it. 
//#define ETH_PHY_POWER -1
#define ETH_PHY_POWER 5
// This needed to be changed for WT32-ETH01 clone boards
// #define ETH_CLK_MODE  ETH_CLOCK_GPIO0_IN
#define ETH_CLK_MODE  ETH_CLOCK_GPIO0_OUT
#elif CONFIG_IDF_TARGET_ESP32P4
#define ETH_PHY_ADDR  0
#define ETH_PHY_MDC   31
#define ETH_PHY_MDIO  52
#define ETH_PHY_POWER 51
#define ETH_CLK_MODE  EMAC_CLK_EXT_IN
#endif
#endif

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5
#define DHCP_OPTION_MESSAGE_TYPE 53
#define DHCP_OPTION_SUBNET_MASK 1
#define DHCP_OPTION_ROUTER 3
#define DHCP_OPTION_LEASE_TIME 51
#define DHCP_OPTION_END 255

Preferences preferences;
File uploadFile;
WiFiUDP udp;
WebServer server(80);

// IP Address Configuration
IPAddress serverIP(172, 16, 170, 1);
IPAddress clientIP(172, 16, 170, 2);
IPAddress gateway(0, 0, 0, 0);
IPAddress subnetMask(255, 255, 255, 0);
IPAddress broadcast(172, 16, 170, 255);

static uint64_t sd_size = 0;
static uint64_t sd_used = 0;
static uint64_t sd_free = 0;
static bool sd_card = false;
static bool wifi_client = false;
static bool eth_connected = false;
static bool client_connected = false;

struct dhcpHeader {
  uint8_t op, htype, hlen, hops;
  uint32_t xid;
  uint16_t secs, flags;
  uint32_t ciaddr, yiaddr, siaddr, giaddr;
  uint8_t chaddr[16];
  uint8_t sname[64];
  uint8_t file[128];
  uint32_t magicCookie;
  };

uint32_t ipToUint(IPAddress ip) {
  return (uint32_t)ip[0] << 24 | (uint32_t)ip[1] << 16 | (uint32_t)ip[2] << 8 | (uint32_t)ip[3];
  }

// Display Setup
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Startup Progress
void startupProg(int percent) {
  int barMaxWidth = 100;
  int barHeight = 12;
  int barX = (SCREEN_WIDTH - barMaxWidth) / 2;
  int barY = 30;

  int barWidth = map(percent, 0, 100, 0, barMaxWidth);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor((SCREEN_WIDTH - (strlen("Xfiltration Device") * 6)) / 2,0);
  display.print("Xfiltration Device");
  display.setCursor((SCREEN_WIDTH - (strlen("STARTING") * 6)) / 2, barY - 10);
  display.print("STARTING");
  display.setCursor((SCREEN_WIDTH / 2) - 10, barY + barHeight + 5);
  display.drawRect(barX, barY, barMaxWidth, barHeight, SSD1306_WHITE);
  display.fillRect(barX, barY, barWidth, barHeight, SSD1306_WHITE);

  display.display();
  }

// Message Display Function
void dispMessage(const char* title, const char* firstLine, const char* secondLine) {
  if (title != nullptr && firstLine != nullptr && secondLine != nullptr) {
    int titleX = (SCREEN_WIDTH - (strlen(title) * 6)) / 2;
    int line1X = (SCREEN_WIDTH - (strlen(firstLine) * 6)) / 2;
    int line2X = (SCREEN_WIDTH - (strlen(secondLine) * 6)) / 2;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(titleX, 0);
    display.print(title);
    display.setCursor(line1X, 25);
    display.print(firstLine);
    display.setCursor(line2X, 40);
    display.print(secondLine);
    display.display();
    }
  }

// SD Card Initialization
void SDInit() {
  Serial.println("Scanning SD card...");
  // Using "false" for 4-bit MMC. Set to "true" for 1-bit. (4-bit is faster)
  if (!SD_MMC.begin("/sdcard", false)) {
    Serial.println("Failure mounting SD card.");
    sd_card = false;
    dispMessage("Critical Error","Could not start XD","No SD Card");
    return;
    }
  Serial.println("Successfully mounted SD card.");

  sd_size = SD_MMC.cardSize();
  sd_used = SD_MMC.usedBytes();
  sd_free = sd_size - sd_used;

  Serial.print("  Card size: ");
  Serial.print(sd_size / (1024 * 1024));
  Serial.println("MB");
  Serial.print("  Free space: ");
  Serial.print(sd_free / (1024 * 1024));
  Serial.println("MB");

  float percentUsed = (float)sd_used / (float)sd_size * 100.0;
  Serial.printf("  Storage Used: %.2f%%\n", percentUsed);

  if (percentUsed >= 100) {
    sd_card = false;
    Serial.println("XD startup failed: SD card is full.");
    dispMessage("Critical Error","Could not start XD","SD Card is Full");
    }
  else { sd_card = true; }
  }

// TODO: SD Card Format Function
bool SDFormat() {
  if (!sd_card) { Serial.println("SD card not detected. Cannot format."); }
  else {
    Serial.println("Preparing SD card for format...");
    SD_MMC.end();
    sd_card = false;
    Serial.println("Closed SD card session.");
    Serial.println("Formatting SD card...");
    
    // FORMAT CODE GOES HERE

    if (1 == 0) {
      // REPLACE "1 == 0" WITH A VALID SUCCESS CHECK
      Serial.println("SD card format failed. Error: ");
      return false;
      }
    }
  Serial.println("SD card format complete. Remounting...");
  SDInit();
  return sd_card;
  }

// Ethernet Event Handler
void onEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("Initializing ethernet interface...");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("Ethernet link established.");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.println("IP configuration set successfully:");
      Serial.println(ETH);
      eth_connected = true;
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      Serial.println("IP configuration was lost.");
      eth_connected = false;
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("Ethernet link disconnected.");
      eth_connected = false;
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("Ethernet interface down.");
      eth_connected = false;
      break;
    default: break;
    }
  }

// DHCP Response Function
void sendDHCPResponse(const dhcpHeader &request, uint8_t dhcpType) {
  uint8_t buffer[300] = {0};
  dhcpHeader *resp = (dhcpHeader*)buffer;

  resp->op = 2;
  resp->htype = 1;
  resp->hlen = 6;
  resp->xid = request.xid;
  resp->flags = request.flags;
  resp->yiaddr = htonl(ipToUint(clientIP));
  resp->siaddr = htonl(ipToUint(serverIP));
  memcpy(resp->chaddr, request.chaddr, 16);
  resp->magicCookie = htonl(0x63825363);

  int idx = sizeof(dhcpHeader);

  // DHCP Message Type
  buffer[idx++] = DHCP_OPTION_MESSAGE_TYPE;
  buffer[idx++] = 1;
  buffer[idx++] = dhcpType;

  // Subnet Mask
  buffer[idx++] = DHCP_OPTION_SUBNET_MASK;
  buffer[idx++] = 4;
  for (int i = 0; i < 4; i++) buffer[idx++] = subnetMask[i];

  // Default Gateway
  // We hold this back to not kill routing on the connected computer
  //buffer[idx++] = DHCP_OPTION_ROUTER;
  //buffer[idx++] = 4;
  //for (int i = 0; i < 4; i++) buffer[idx++] = serverIP[i];

  // Lease Time (3600 Seconds)
  buffer[idx++] = DHCP_OPTION_LEASE_TIME;
  buffer[idx++] = 4;
  uint32_t lease = htonl(3600);
  memcpy(&buffer[idx], &lease, 4);
  idx += 4;

  // Server Identifier
  buffer[idx++] = 54;
  buffer[idx++] = 4;
  for (int i = 0; i < 4; i++) buffer[idx++] = serverIP[i];

  // Static Route (to not interfere with other connections)
  buffer[idx++] = 121;
  buffer[idx++] = 1 + 3 + 4;
  buffer[idx++] = 24;
  buffer[idx++] = 172;
  buffer[idx++] = 16;
  buffer[idx++] = 170;
  buffer[idx++] = 0;
  buffer[idx++] = 0;
  buffer[idx++] = 0;
  buffer[idx++] = 0;

  // Static Route (Windows)
  buffer[idx++] = 249;
  buffer[idx++] = 1 + 3 + 4; 
  buffer[idx++] = 24;
  buffer[idx++] = 172;
  buffer[idx++] = 16;
  buffer[idx++] = 170;
  buffer[idx++] = 0;
  buffer[idx++] = 0;
  buffer[idx++] = 0;
  buffer[idx++] = 0;

  buffer[idx++] = DHCP_OPTION_END;

  // Send Response
  udp.beginPacket(IPAddress(255, 255, 255, 255), DHCP_CLIENT_PORT);
  udp.write(buffer, idx);
  udp.endPacket();

  Serial.printf("Sent: DHCP %s\n", (dhcpType == DHCP_OFFER) ? "OFFER" : "ACK");
  }

// DHCP Packet Handler
void handleDHCPPacket() {
  uint8_t buffer[300];
  int len = udp.read(buffer, sizeof(buffer));
  if (len < (int)sizeof(dhcpHeader)) { return; }

  dhcpHeader *req = (dhcpHeader*)buffer;
  if (ntohl(req->magicCookie) != 0x63825363) { return; }

  uint8_t *options = buffer + sizeof(dhcpHeader);
  uint8_t msgType = 0;

  for (int i = 0; i < len - (int)sizeof(dhcpHeader);) {
    uint8_t code = options[i++];
    if (code == DHCP_OPTION_END) { break; }
    uint8_t optlen = options[i++];
    if (code == DHCP_OPTION_MESSAGE_TYPE) {
      msgType = options[i];
      break;
      }
    i += optlen;
    }

  // TODO: Serial output acknowledging client connection
  if (msgType == DHCP_DISCOVER) {
    Serial.println("Received: DHCP DISCOVER");
    sendDHCPResponse(*req, DHCP_OFFER);
    }
  else if (msgType == DHCP_REQUEST) {
    Serial.println("Received: DHCP REQUEST");
    sendDHCPResponse(*req, DHCP_ACK);
    }
  }

// WebRoot Handler
// Probably need to do something to change the title text on the page to something more
// innocuous-looking. Host-based IPSes have the potential to have signatures which can 
// then detect stuff like XD, which we don't want. Of course it's also entirely possible
// those same HIPS solutions can also block HTTP file upload functionality, but hey, one
// step at a time.

// Yes I know it's HTTP and not HTTPS. HTTPS introduces a lot of overhead for no real
// benefit in this scenario. Since it's a completely sandboxed network there isn't much
// in the way of snooping opportunities between the USB ethernet adapter and the XD.
void webRoot() {
  Serial.println("HTTP Client: GET /");
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>XD File Upload</title>
<style>
  body {
    background:#2e2e2e;
    color:white;
    font-family: Arial;
    display: flex;
    justify-content: center;
    align-items: center;
    height: 100vh;
    margin:0;
    }

  .container {
    background: #3a3a3a;
    padding: 20px;
    border-radius: 10px;
    max-width: 400px;
    width: 90%;
    box-shadow: 0 0 20px rgba(0,0,0,0.5);
    }

  h2 { text-align:center; margin-bottom: 15px; }

  input[type="file"] {
    margin-top: 10px;
    margin-bottom: 10px;
    width: 100%;
    }

  .button {
    background: #1e90ff;
    color: white;
    padding: 10px 20px;
    border: none;
    border-radius: 5px;
    cursor: pointer;
    font-size: 16px;
    }

  .button:hover { background:#0066cc; }

  .delete-button {
    background:#ff4d4d;
    color:white;
    padding:5px 10px;
    border:none;
    border-radius:5px;
    cursor:pointer;
    }

  .delete-button:hover {
    background:#cc0000;
    }

  .button:disabled {
    background-color: #aaa;
    color: #eee;
    cursor: not-allowed;
    opacity: 0.7;
    }
  
  .button:disabled:hover { background-color: #aaa; }
  
  progress {
    width: 100%;
    height: 20px;
    margin-top: 10px;
    }
</style>
</head>
<body>
  <div id="upload" class="container">
    <h2>XD File Upload</h2>
    <form id="uploadForm">
      <input type="file" id="fileInput" name="upload" multiple required><br>
      <input id="uploadButton" class="button" type="submit" value="Upload">
      &nbsp;
      <input id="resetButton" class="button" type="reset" value="Clear" disabled>
      <input id="cancelButton" class="button" type="button" value="Cancel" hidden="hidden" disabled>
    </form>
    <progress id="progressBar" value="0" max="100"></progress>
    <p id="status" style="text-align: center;">Ready</p>
  </div>

  <script>
    const form = document.getElementById('uploadForm');
    const fileInput = document.getElementById('fileInput');
    const uploadButton = document.getElementById('uploadButton');
    const resetButton = document.getElementById('resetButton');
    const cancelButton = document.getElementById('cancelButton');
    const progressBar = document.getElementById('progressBar');
    const status = document.getElementById('status');

    if (fileInput.files.length === 0) {
      uploadButton.disabled = true;
      resetButton.disabled = true;
      cancelButton.disabled = true;
      cancelButton.hidden = true;
      }

    fileInput.addEventListener('change', () => {
      uploadButton.disabled = fileInput.files.length === 0;
      resetButton.disabled = fileInput.files.length === 0;
      });

    form.addEventListener('reset', () => {
      uploadButton.disabled = true;
      resetButton.disabled = true;
      cancelButton.disabled = true;
      cancelButton.hidden = true;
      fileInput.disabled = false;
      });

    form.addEventListener('submit', function (e) {
      e.preventDefault();
      fileInput.disabled = true;
      uploadButton.disabled = true;
      resetButton.hidden = true;
      cancelButton.hidden = false;
      cancelButton.disabled = false;

      const files = fileInput.files;
      const formData = new FormData();
      const xhr = new XMLHttpRequest();

      for (const file of files) { formData.append('upload', file); }

      xhr.open('POST', '/upload', true);
      xhr.upload.addEventListener('progress', function (event) {
        if (event.lengthComputable) {
          const percent = Math.round((event.loaded / event.total) * 100);
          progressBar.value = percent;
          status.textContent = `Uploading... ${percent}%`;
          }
        });

      xhr.addEventListener('load', function () {
        if (xhr.status === 200) {
          status.textContent = 'Upload Complete';
          fileInput.disabled = false;
          cancelButton.disabled = true;
          cancelButton.hidden = true;
          resetButton.hidden = false;
          resetButton.disabled = false;
          }
        else { status.textContent = 'Upload Failed.'; }
        });

      xhr.addEventListener('abort', () => {
        progressBar.value = 0;
        status.textContent = 'Aborted';
        fileInput.disabled = false;
        uploadButton.disabled = false;
        cancelButton.disabled = true;
        cancelButton.hidden = true;
        resetButton.hidden = false;
        });

      xhr.addEventListener('error', function () { status.textContent = 'Upload Error'; });
      xhr.send(formData);

      cancelButton.addEventListener('click', () => {
        if (xhr) { xhr.abort(); }
        });
    });
  </script>
</body>
</html>

)rawliteral";

  server.send(200, "text/html", html);
  }

// File Upload Handler
void handleFileUpload() {
  HTTPUpload& upload = server.upload();

  switch (upload.status) {
    case UPLOAD_FILE_START: {
      String filename = "/" + upload.filename;
      Serial.printf("Upload Start: %s\n", filename.c_str());
      uploadFile = SD_MMC.open(filename, FILE_WRITE);
      if (!uploadFile) {
        Serial.print("Failure opening file \"");
        Serial.print(upload.filename);
        Serial.println("\" for writing.");
        }
      break;
      }
    case UPLOAD_FILE_WRITE:
      if (uploadFile) { uploadFile.write(upload.buf, upload.currentSize); }
      break;
    case UPLOAD_FILE_END:
      if (uploadFile) {
        uploadFile.close();
        Serial.printf("Upload Success: %s (%u bytes)\n", upload.filename.c_str(), upload.totalSize);
        }
      break;
    case UPLOAD_FILE_ABORTED:
      if (uploadFile) {
        uploadFile.close();
        Serial.println("Upload operation aborted by user.");
        }
      break;
    }
  }

// Upload Confirmation
void uploadSuccess() {
  Serial.println("Upload operation complete.");
  server.send(200, "text/html",
    "<!DOCTYPE html><html>"
    "<head><title>Upload Complete</title></head>"
    "<body style='background: #2e2e2e; color: white; font-family: Arial;'>"
    "<h2>Upload Successful!</h2>"
    "<p><a href='/'>Go back</a></p>"
    "</body></html>");
  }

// TODO: Web Configuration Handler
void configRoot() {
  Serial.println("HTTP Client: GET /");
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>XD Configuration</title>
<style>
  body {
    background:#2e2e2e;
    color:white;
    font-family: Arial;
    display: flex;
    justify-content: center;
    align-items: center;
    height: 100vh;
    margin:0;
    }

  .container {
    background: #3a3a3a;
    padding: 20px;
    border-radius: 10px;
    max-width: 400px;
    width: 90%;
    box-shadow: 0 0 20px rgba(0,0,0,0.5);
    }

  h2 { text-align:center; margin-bottom:15px; }

  input[type="file"] {
    margin-top: 10px;
    margin-bottom: 10px;
    width: 100%;
    }

  .button {
    background: #1e90ff;
    color: white;
    padding: 10px 20px;
    border: none;
    border-radius: 5px;
    cursor: pointer;
    font-size: 16px;
    }

  .button:hover { background:#0066cc; }

  .delete-button {
    background:#ff4d4d;
    color:white;
    padding:5px 10px;
    border:none;
    border-radius:5px;
    cursor:pointer;
    }

  .delete-button:hover {
    background:#cc0000;
    }

  progress {
    width: 100%;
    height: 20px;
    margin-top: 10px;
    }
</style>
</head>
<body>
  <div id="config" class="container">
    <h2>XD Configuration</h2>
  </div>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
  }

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("---------------------------");
  Serial.println("XD - THE XFILTRATION DEVICE");
  Serial.println("---------------------------");
  Serial.print("Version ");
  Serial.println(xdver);
  Serial.print("Built for ");
  Serial.println(ARDUINO_BOARD);
  Serial.println();
  Serial.println("Starting XD...");
  delay(500);

  // Display Initialization
  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { for(;;); }
  startupProg(0);

  // Clock Setup
  pinMode(0, OUTPUT);
  startupProg(5);

  // Explicit Network Device Reset
  Serial.println("Configuring device hardware...");
  pinMode(ETH_PHY_POWER, OUTPUT);
  gpio_set_drive_capability((gpio_num_t)ETH_PHY_POWER, GPIO_DRIVE_CAP_3);
  digitalWrite(ETH_PHY_POWER, LOW);
  delay(100);
  digitalWrite(ETH_PHY_POWER, HIGH);
  delay(1000);
  Serial.println("Hardware configuration complete.");
  startupProg(25);
  
  // SD Card Setup
  SDInit();

  // Start network stuff if SD card is present
  if (sd_card) {
    startupProg(50);
    Network.onEvent(onEvent);

    if (ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_POWER, ETH_CLK_MODE)) {
      Serial.println("Successfully started ethernet.");
      startupProg(60);
      }
    else { Serial.println("Failed to initialize ethernet."); }

    if (ETH.config(serverIP, gateway, subnetMask)) {
      delay(500);
      Serial.println("Ethernet configuration successful.");
      startupProg(75);
      }
    else {
      delay(500);
      Serial.println("Ethernet configuration failed.");
      }

    // Start DHCP Server
    if (udp.begin(DHCP_SERVER_PORT)) {
      Serial.println("DHCP server started.");
      startupProg(90);
      }
    else { Serial.println("Failed to start DHCP server."); }

    // Start Webserver
    if (wifi_client == false) {
      server.on("/", webRoot);
      server.on("/upload", HTTP_POST, uploadSuccess, handleFileUpload);
      server.begin();
      Serial.print("Webserver started: http://");
      Serial.print(serverIP);
      Serial.println("/");
      startupProg(100);
      delay(1000);
      dispMessage("Xfiltration Device","READY",serverIP.toString().c_str());
      }
      else {
      // TODO: Configuration Controls
      server.on("/", configRoot);
      server.begin();
      Serial.print("Webserver started (Config Mode): http://");
      Serial.print(serverIP);
      Serial.println("/");
      startupProg(100);
      }
    }
  else {
    Serial.println("XD startup failed: SD card not detected.");
    }
  }

void loop() {
  if (sd_card) {
    if (eth_connected) {
      int packetSize = udp.parsePacket();
      if (packetSize > 0) { handleDHCPPacket(); }

      server.handleClient();
      }
    }
  }
