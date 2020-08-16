// power strip controller for wemos d1 mini
// (c) 2020 linux-works labs


#include <Arduino.h>

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <EEPROM.h>

#include <lwip/init.h>



// program options
#define USE_SCC  // serial-control class (ascii over uart from remote sensor)




// Set these to your desired credentials.
const char *wifi_ssid = "ac-outlet-ap-01";   // we are an AP (access point)
const char *wifi_password = "12345678";      // yes, you should pick a better one ;)


// Set the IP Address you want for your AP
IPAddress apIP(192, 168, 4, 1);


// we are reachable over port 80, normal web interface
ESP8266WebServer server(80);


#ifdef STATIC_LEASE
// List of mac address for static lease
uint8 mac1[6] = { 0x84, 0xF3, 0xEB, 0x64, 0x71, 0x66 };
#endif

String title_str = "ESP8266 Wemos-d1-Mini WIFI Power Switch AP";


int new_sta_count = 0, old_sta_count = 0;

String result;
char wifiClientMac[32];
unsigned char number_client;
struct station_info *stat_info;
String client_ip_str;
char client_ip_chars[33];
int phase = 0;
unsigned int new_sta_ts = 0;
char cmd[1024];


/*
   serial control class-instance
*/
#ifdef USE_SCC
String inputString = "";         // a String to hold incoming data
bool stringComplete = false;  // whether the string is complete
char user_str[32];
#endif




void handle_root (void)
{
  int i = 1;

  number_client = wifi_softap_get_station_num();

  result = "{ \"ssid\": \"" + String(wifi_ssid) + "\", \"pw\": \"" + String(wifi_password) + "\" }";

  stat_info = wifi_softap_get_station_info();
  while (stat_info != NULL) {
    result += "\n{ \"client\": \"" + String(i++) + "\", \"ip\": \"";

    client_ip_str = IPAddress(stat_info->ip).toString();
    client_ip_str.toCharArray(client_ip_chars, 32);   // save in pure char array, for later

    result += client_ip_str + "\", \"mac\": \"";

    sprintf(wifiClientMac, "%02X:%02X:%02X:%02X:%02X:%02X", MAC2STR(stat_info->bssid));
    result += wifiClientMac;

    result += "\" }";

    stat_info = STAILQ_NEXT(stat_info, next);  // get next in our list
  }


  //server.send(200, "text/json", result);

  Serial.println(result);  // debug

  return;
}




void setup (void)
{
  delay(1000);

  Serial.begin(9600);

  Serial.println("\n\n# ");
  Serial.println(title_str);



  // reserve 200 bytes for the inputString (Serial buffer)
  inputString.reserve(200);

  bzero(user_str, 32);




  // Disable the WiFi persistence to avoid any re-configuration that may erase static lease when starting softAP
  WiFi.persistent(true); //(false);


  // start us up as an AP, with dhcpd support, and our addr set to 192.168.4.1 on our own wifi_ssid network
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

  // set lease time to 1min
  struct dhcps_lease dhcp_lease;
  if (!wifi_softap_set_dhcps_lease_time(1)) {
    DEBUG_WIFI("[APConfig] wifi_softap_set_dhcps_lease_time failed!\n");
  }


#ifdef STATIC_LEASE
  Serial.print("# "); Serial.print("Adding static lease...");
  wifi_softap_add_dhcps_lease(mac1);  // always 192.168.4.100
#endif


  // You can remove the password parameter if you want the AP to be open
  WiFi.softAP(wifi_ssid, wifi_password);


  // show our configured wifi_ssid
  IPAddress myIP = WiFi.softAPIP();
  Serial.print("# "); Serial.print("AP wifi_ssid: ["); Serial.print(wifi_ssid); Serial.println("]");
  Serial.print("# "); Serial.print("AP wifi_pass: ["); Serial.print(wifi_password); Serial.println("]");
  Serial.print("# "); Serial.print("My AP (webserver) IP: ["); Serial.print(myIP); Serial.println("]");


  // add url handlers here
  //server.on("/", handle_root);
  //server.on("/help", handle_root);


  // start webserver
  server.begin();

  return;
}




bool send_web_command (char *web_cmd, char *ret_str)
{
  bool ret_val = false;   // assume failure for all cases unless we override
  WiFiClient client;
  HTTPClient http;
  char big_buff[1024];


  strcpy(ret_str, "");   // init


  // if we got a blank ip_addr, try our default
  if (strlen(client_ip_chars) == 0) {
    strcpy(client_ip_chars, "192.168.4.100");
  }


  // form the full url command
  sprintf(cmd, "http://%s/cm?cmnd=%s", client_ip_chars, web_cmd);
  //Serial.print("# get: ["); Serial.print(cmd); Serial.println("]");  // debug

  if (http.begin(client, cmd)) {

    // start connection and send HTTP header
    int httpCode = http.GET();

    // httpCode will be negative on error
    if (httpCode > 0) {
      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
        String payload = http.getString();

        bzero(big_buff, 1024);
        payload.toCharArray(big_buff, 1023);
        strcpy(ret_str, big_buff);   // TODO: strncpy()
        ret_val = true;   // success
      }

    } else {
      Serial.printf("# [HTTP] GET error: [%s]\n", http.errorToString(httpCode).c_str());
    }

    http.end();

  } else {
    Serial.printf("# [HTTP] Unable to connect\n");
  }

  return ret_val;
}



void send_wifi_command (char *ac_socket_name, bool socket_on)
{
  char *on_off_str = NULL;
  bool ret_status;
  char web_cmd[256];
  char ret_str[1024];

  // convert bool on_off to string
  if (socket_on == true) {
    on_off_str = (char *)"On";
  } else {
    on_off_str = (char *)"Off";
  }


  // form the web command
  sprintf(web_cmd, "%s%%20%s", ac_socket_name, on_off_str);

  ret_status = send_web_command(web_cmd, ret_str);
  if (ret_status == true) {
    delay(2000);
    Serial.println(ret_str);   // web reply, from ac outlet device
  }

  return;
}



void get_tasmota_state (void)
{
  bool ret_status;
  char web_cmd[256];
  char ret_str[1024];


  // form the web command
  sprintf(web_cmd, "state");

  ret_status = send_web_command(web_cmd, ret_str);
  if (ret_status == true) {
    delay(2000);
    Serial.println(ret_str);   // web reply, from ac outlet device
  }

  return;
}




void get_tasmota_ports (void)
{
  bool ret_status;
  char web_cmd[256];
  char ret_str[1024];


  // form the web command
  sprintf(web_cmd, "state");

  ret_status = send_web_command(web_cmd, ret_str);
  if (ret_status == true) {
    delay(2000);
    Serial.println(ret_str);   // web reply, from ac outlet device
  }

  return;
}



void get_tasmota_power (void)
{
  bool ret_status;
  char web_cmd[256];
  char ret_str[1024];

  
  // form the web command
  sprintf(web_cmd, "status%%208");

  ret_status = send_web_command(web_cmd, ret_str);
  if (ret_status == true) {
    delay(2000);
    Serial.println(ret_str);   // web reply, from ac outlet device
  }

  return;
}



/*
  serial_event occurs whenever a new data comes in the hardware serial RX. This
  routine is run between each time loop() runs, so using delay inside loop can
  delay response. Multiple bytes of data may be available.
*/
void serial_event (void)
{
  while (Serial.available()) {
    // get the new byte
    char inChar = (char)Serial.read();

    // if the incoming character is a newline, set a flag so the main loop can do something about it
    if (inChar == '\n' || inChar == '\r') {
      stringComplete = true;
      inputString += '\0';  // do add a null, instead of newline
      return;
    }

    // add it to the inputString
    inputString += inChar;

  }

  return;
}


void print_help (void)
{
  Serial.println("# commands: up, down, p0,on, p1,off, p2,off, power, cycle, status, ports, reboot");
}



void loop (void)
{

  // give this routine some of loop()'s time
  server.handleClient();  // this is us, OUR webserver at port 80


  // service any user serial-port ascii commands to us
  serial_event();
  if (stringComplete) {
    inputString.toCharArray(user_str, 32);
    if (strlen(user_str) != 0) {


      // is this a SET or GET command?

      if (!strcmp(user_str, "p0,on")) {
        send_wifi_command((char *)"Power0", true);
      } else if (!strcmp(user_str, "p0,off")) {
        send_wifi_command((char *)"Power0", false);
      }

      else if (!strcmp(user_str, "p1,on")) {
        send_wifi_command((char *)"Power1", true);
      } else if (!strcmp(user_str, "p1,off")) {
        send_wifi_command((char *)"Power1", false);
      }

      else if (!strcmp(user_str, "p2,on")) {
        send_wifi_command((char *)"Power2", true);
      } else if (!strcmp(user_str, "p2,off")) {
        send_wifi_command((char *)"Power2", false);
      }


      // easy 'all down' or 'all up'
      if (!strcmp(user_str, "up")) {
        send_wifi_command((char *)"Power0", true);
      } else if (!strcmp(user_str, "down")) {
        send_wifi_command((char *)"Power0", false);
      }


      // automatically power-cycle both sockets
      else if ( (!strcmp(user_str, "bounce")) || (!strcmp(user_str, "cycle")) ) {
        send_wifi_command((char *)"Power0", false);
        delay(3000);
        send_wifi_command((char *)"Power0", true);
        //delay(1000);
      }


      // ask the wifi outlet switch about its general state (uptime, etc)
      else if ( (!strcmp(user_str, "state")) || (!strcmp(user_str, "status")) ) {
        get_tasmota_state();
      }


      // ask the wifi outlet switch about its port states (outlet on or off)
      else if (!strcmp(user_str, "ports")) {
        get_tasmota_ports();
      }


      // ask the wifi outlet switch about its port POWER (volts, current) values
      else if (!strcmp(user_str, "power")) {
        get_tasmota_power();
      }


      // user wants help?
      else if ( (!strcmp(user_str, "help")) || (!strcmp(user_str, "?")) )  {
        print_help();
      }


      // if we get hung, the user (pc) can reboot us.  note, this is not rebooting the power socket,
      //  but its rebooting the ESP chip
      else if (!strcmp(user_str, "reboot")) {
        ESP.restart();
      }

    }

    // reset the string data
    stringComplete = false;
    inputString = "";
  }




  // check if any stations entered or left our AP

  // set a 'timer' for 3 seconds from now, to get the dhcp-assigned ip addr and client_mac
  new_sta_count = wifi_softap_get_station_num();
  if ( (new_sta_count != old_sta_count) && (phase == 0) ) {
    new_sta_ts = millis();   // save a timestamp of when we got triggered
    phase = 1;
    old_sta_count = new_sta_count;
  }


  if ( ((millis() - new_sta_ts) > 3000) && (phase == 1) )  {
    phase = 0;  // reset it
    handle_root();  // for now, print the same thing as the web output
  }

}
