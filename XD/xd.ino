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
#include <SD.h>
#include <ETH.h>
#include <IPAddress.h>
#include <WiFiUdp.h>
#include <WebServer.h>

#ifndef ETH_PHY_MDC
#define ETH_PHY_TYPE ETH_PHY_LAN8720
#if CONFIG_IDF_TARGET_ESP32
String confString = "Built for for LAN8720";
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
String confString = "Built for ESP32-P4";
#define ETH_PHY_ADDR  0
#define ETH_PHY_MDC   31
#define ETH_PHY_MDIO  52
#define ETH_PHY_POWER 51
#define ETH_CLK_MODE  EMAC_CLK_EXT_IN
#endif
#endif

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

// IP Address Configuration
IPAddress serverIP(172, 16, 170, 1);
IPAddress clientIP(172, 16, 170, 2);
IPAddress gateway(0, 0, 0, 0);
IPAddress subnetMask(255, 255, 255, 0);
IPAddress broadcast(172, 16, 170, 255);

static bool eth_connected = false;
static bool client_connected = false;

Preferences preferences;
File uploadFile;
WiFiUDP udp;
WebServer server(80);

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

// TODO: SD Card Interface
// TODO: 4-bit MMC. We really don't want to use SPI if we can avoid it since it's
// slower and has the potential to interfere with other stuff. But so far SPI is
// the only thing I can get working.

// Ethernet Event Handler
void onEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("Ethernet interface initialized.");
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
void sendDhcpResponse(const dhcpHeader &request, uint8_t dhcpType) {
  uint8_t buffer[300] = {0};
  dhcpHeader *resp = (dhcpHeader*)buffer;

  resp->op = 2;  // BOOTREPLY
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
  buffer[idx++] = 249;      // Option 249
  buffer[idx++] = 1 + 3 + 4;  // length = 8
  buffer[idx++] = 24;         // prefix length
  buffer[idx++] = 172;
  buffer[idx++] = 16;
  buffer[idx++] = 170;
  buffer[idx++] = 0;          // next-hop = 0.0.0.0
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
void handleDhcpPacket() {
  uint8_t buffer[300];
  int len = udp.read(buffer, sizeof(buffer));
  if (len < (int)sizeof(dhcpHeader)) return;

  dhcpHeader *req = (dhcpHeader*)buffer;
  if (ntohl(req->magicCookie) != 0x63825363) return;

  uint8_t *options = buffer + sizeof(dhcpHeader);
  uint8_t msgType = 0;

  for (int i = 0; i < len - (int)sizeof(dhcpHeader); ) {
    uint8_t code = options[i++];
    if (code == DHCP_OPTION_END) break;
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
    sendDhcpResponse(*req, DHCP_OFFER);
    }
  else if (msgType == DHCP_REQUEST) {
    Serial.println("Received: DHCP REQUEST");
    sendDhcpResponse(*req, DHCP_ACK);
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

// TODO: Support for uploading multiple files
void webRoot() {
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

  h2 { text-align:center; margin-bottom:15px; }

  input[type="file"] {
    margin-top: 10px;
    margin-bottom: 10px;
    width: 100%;
    }

  .btn {
    background: #1e90ff;
    color: white;
    padding: 10px 20px;
    border: none;
    border-radius: 5px;
    cursor: pointer;
    font-size: 16px;
    }

  .btn:hover { background:#0066cc; }

  .delete-btn {
    background:#ff4d4d;
    color:white;
    padding:5px 10px;
    border:none;
    border-radius:5px;
    cursor:pointer;
    }

  .delete-btn:hover {
    background:#cc0000;
    }
</style>
</head>
<body>
  <div id="upload" class="container">
    <h2>File Upload</h2>
    <form method="POST" action="/upload" enctype="multipart/form-data">
      <input type="file" name="upload" required>
      <input class="btn" type="submit" value="Upload">
    </form>
  </div>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
  }

// TODO: File Upload Handler
void handleFileUpload() { }

// Upload Confirmation
void uploadSuccess() {
  server.send(200, "text/html",
    "<html><body style='background: #2e2e2e; color: white; font-family: Arial;'>"
    "<h2>File Uploaded Successfully!</h2>"
    "<p><a href='/'>Go back</a></p>"
    "</body></html>");
  }

void setup() {
  Serial.begin(115200);
  Serial.println("---------------------------");
  Serial.println("XD - THE XFILTRATION DEVICE");
  Serial.println("---------------------------");
  Serial.print("Version ");
  Serial.println(xdver);
  Serial.println();
  Serial.println("Starting XD...");
  delay(500);

  // Clock Setup
  pinMode(0, OUTPUT);
  pinMode(ETH_PHY_POWER, OUTPUT);

  // Explicit Network Device Reset
  gpio_set_drive_capability((gpio_num_t)ETH_PHY_POWER, GPIO_DRIVE_CAP_3);
  digitalWrite(ETH_PHY_POWER, LOW);
  delay(100);
  digitalWrite(ETH_PHY_POWER, HIGH);
  delay(1000);

  Serial.println(confString);
  Network.onEvent(onEvent);

  if (ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_POWER, ETH_CLK_MODE)) {
    Serial.println("Successfully initialized ethernet.");
    }
  else { Serial.println("Failed to initialize ethernet."); }

  if (ETH.config(serverIP, gateway, subnetMask)) { Serial.println("TCP/IP configuration set successfully."); }
  else { Serial.println("TCP/IP configuration failed."); }

  // Start DHCP server
  if (udp.begin(DHCP_SERVER_PORT)) { Serial.println("DHCP Server started."); }
  else { Serial.println("Failed to start DHCP Server."); }

  // Start Webserver
  server.on("/", webRoot);
  server.begin();
  Serial.println("Webserver started.");
  }

void loop() {
  if (eth_connected) {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) { handleDhcpPacket(); }

    server.handleClient();
    }
  }
