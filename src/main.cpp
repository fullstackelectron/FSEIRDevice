#include <Arduino.h>
#include <FSEWifiManager.h>
#include "FSEOTA.h"
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <IRsend.h>
#include "FSERestAPI.h"
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>


enum Mode{
	READ,
	PULLING,
	WIFI,
	DEVICE_FAIL
};

std::unique_ptr<ESP8266WebServer> server;
std::unique_ptr<DNSServer>        dnsServer;

FSEOTA ota;
FSEWifiManager fseWifiManager;
FSERestAPI restApi;

#define OTA_FILE "/jrareas/firmware/main/ir_code_sucker/firmware.bin"
#define OTA_HOST "raw.githubusercontent.com"
#define OTA_PORT 443
#define OTA_FINGERPRINT "02 AC 5C 26 6A 0B 40 9B 8F 0B 79 F2 AE 46 25 77"
#define BACKEND_HOST_PARAM "backend_host"
#define DEVICE_ID_PARAM "DEVICE_ID"
#define APP_NAME "CLIMAPP"
#define DNS_PORT 53



#define kRecvPin 14
#ifdef V1
#define IR_LED_PIN 13
#else
#define IR_LED_PIN 4
#endif

#ifdef V1
#define BEEP_PIN 15
#else
#define BEEP_PIN 13
#endif

#ifndef V1
#define SEND_BTN_PIN 15
#endif

#ifdef V1
#define FUNC_BUTTON 12
#else
#define FUNC_BUTTON 5
#endif
//#define TURN_OFF_OTA_BTN_PIN 5
#define LED_BUILTIN 2

#define VERSION "1.0.6" // change to keep track of changes


#define HOSTNAME "irsuccker"
const uint16_t kCaptureBufferSize = 1024;
const uint8_t kTimeout = 50;
const uint16_t kFrequency = 38000;

Mode device_mode = READ;
IRsend irsend(IR_LED_PIN);
IRrecv irrecv(kRecvPin, kCaptureBufferSize, kTimeout, false);
// Somewhere to store the captured message.
decode_results results;

void beep(int times, bool short_beep = true);
void handleButton(int button);
void registerDevice();
void openAP();
void handlePost();
void blink(int interval);
void handlePullingCommands();
void handleRead();
void handleFail();

ICACHE_RAM_ATTR void sendLastSaved();
bool mode_read = true;
bool send_last_code = false;
const int time_read_blink = 100;
uint16_t *raw_array;
uint16_t length;

String device_id;
const char* ac_id;
DynamicJsonDocument doc(1024);

// This section of code runs only once at start-up.
void setup() {
	irrecv.enableIRIn();
	irsend.begin();
	pinMode(LED_BUILTIN, OUTPUT);
	pinMode(BEEP_PIN, OUTPUT);
#ifndef V1
	pinMode(SEND_BTN_PIN, INPUT);
	attachInterrupt(SEND_BTN_PIN, sendLastSaved, RISING);
#endif
	pinMode(FUNC_BUTTON,INPUT_PULLUP);
	handleButton(FUNC_BUTTON);
	Serial.begin(115200);

	while (!Serial)  // Wait for the serial connection to be establised.
		delay(50);

	Serial.println("Serial is up");

//	if (!fseWifiManager.has_wifi_settings()) {
//		if (!fseWifiManager.begin()) {
//			beep(1);
//			ESP.reset();
//		}
//	}

	if(!fseWifiManager.has_wifi_settings()) {
		device_mode = WIFI;
		openAP();
	} else {
		device_id = fseWifiManager.getByKey(DEVICE_ID_PARAM);
		Serial.print("Device ID:");
		Serial.println(device_id);

		restApi.setHost(fseWifiManager.getByKey(BACKEND_HOST_PARAM));

		String getMe = restApi.getReq(String("/api/devices/" + device_id));
		Serial.print("Me information:");
		Serial.println(getMe);
		DeserializationError error = deserializeJson(doc, getMe.c_str());

		if (!error) {
			ac_id = doc["acModelId"];
			Serial.print("AC Model id:");
			Serial.println(ac_id);
		} else {
			device_mode = DEVICE_FAIL;
			Serial.print(F("deserializeJson() failed: "));
			Serial.println(error.f_str());
		}
		WiFi.hostname(HOSTNAME);
		Serial.print("IP address:\t");
		Serial.println(WiFi.localIP());
		Serial.print("Running Version: ");
		Serial.println(VERSION);

		beep(2);
		ota.begin(HOSTNAME);
		MDNS.addService("esp", "tcp", 8266);
	}
}

void openAP() {
	Serial.println("Configuring Access Point");
	WiFi.mode(WIFI_AP);
	WiFi.softAP(APP_NAME);
	Serial.println(WiFi.softAPIP());

	dnsServer.reset(new DNSServer());
	server.reset(new ESP8266WebServer(80));
	dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
	String host = String(HOSTNAME) + ".local";

	dnsServer->start(DNS_PORT, HOSTNAME, WiFi.softAPIP());

	server->on("/wifi", HTTP_POST, handlePost);
	server->onNotFound([]() {
	    String message = "Hello World!\n\n";
	    message += "URI: ";
	    message += server->uri();

	    server->send(200, "text/plain", message);
	  });
	server->begin();
}

void handlePost() {
	const char *payload = server->arg("plain").c_str();
	DeserializationError error = deserializeJson(doc, payload);

	// Test if parsing succeeds.
	if (error) {
		Serial.print(F("deserializeJson() failed: "));
		Serial.println(error.f_str());
	return;
	}
	fseWifiManager.addParameter(DEVICE_ID_PARAM, "Device ID", doc["DEVICE_ID"],120);
	fseWifiManager.addParameter(BACKEND_HOST_PARAM, "API Host",doc["API_HOST"], 120);

	fseWifiManager.connectWifi(doc["SSID"], doc["PASS"]);
	if (WiFi.status() == WL_CONNECTED) {
		fseWifiManager.saveConfigToSPIFFS();
		const char* ssid = doc["SSID"];
		const char* pass = doc["PASS"];
		Serial.print("SSID:");
		Serial.println(ssid);
		Serial.print("PASS:");
		Serial.println(pass);
		Serial.println("ESP Will reset now.");
		server->send(200, "application/json", payload);
		beep(1);
	} else {
		Serial.println("Fail to connect. ESP Will reset now");
		server->send(500, "application/json", "{\"error\":\"Couldnt connect to WiFi\"}");
	}
	ESP.reset();
}

void beep(int times, bool short_beep) {
	for (int i=times; i > 0; i--) {
		digitalWrite(BEEP_PIN, HIGH);
		if (short_beep) {
			delay(10);
		} else {
			delay(100);
		}
		digitalWrite(BEEP_PIN, LOW);
		delay(100);
	}
}

void saveCode(decode_results results) {
	length = getCorrectedRawLength(&results);
	raw_array = resultToRawArray(&results);
	String codeStr= "";
	for (int i=0; i < length; i++) {
		codeStr += raw_array[i];
		if(i<length - 1) {
			codeStr += ":";
		}
	}
	Serial.print("SEquence is:");
	Serial.println(codeStr);
	String body = "{\"infraredCode\": \"" + codeStr + "\"}";
	String path = "/api/acModels/" + String(ac_id) +"/infraredCodes";
//	restApi.postReq(path, body);
}

void sendCommand(String command) {
	const char * del = ":";
	int count = 0;
	int from = 0;
	while (true) {
		from = command.indexOf(del, from);
		count++;
		if (from == -1) {
			break;
		} else {
			from++;
		}
	}
	Serial.print("Count = ");
	Serial.println(count);

	uint16_t *data = new uint16_t[count];
	int index = 0;
	while (index < count) {
		from = command.indexOf(del, 0);
		if (from == -1) {
			data[index] = command.substring(0).toInt();
		} else {
			data[index] = command.substring(0, from).toInt();
		}

		command.remove(0,from + 1);
		from = 0;
		index++;
	}

	irsend.sendRaw(data, count, kFrequency);
	irrecv.resume();
}

// The repeating section of the code
void loop() {
//	sendCommand("9368:4748:618:1816:592:616:616:618:560:650:614:618:590:646:584:624:592:644:560:648:592:1838:590:1846:566:1844:590:1844:564:1844:590:1846:564:1844:588:1844:564:1844:586:648:564:642:590:644:562:648:590:642:590:644:564:644:590:644:588:1844:564:1844:590:1846:564:1846:588:1844:564:1846:590:1846:564:1846:586:650:562:646:590:644:562:646:588:644:588:644:562:646:590:644:586:1846:562:1846:586:1848:564:1846:586:1850:562:1846:562");
//	delay(3000);
//	return;
	if (device_mode == WIFI) {
		dnsServer->processNextRequest();
		server->handleClient();
		digitalWrite(LED_BUILTIN, HIGH);
	} else if(device_mode == READ){
		handleRead();
	} else if (device_mode == PULLING){ //pulling
		handlePullingCommands();
	} else { //fail
		handleFail();
	}

	if (send_last_code) {
		device_mode = PULLING;
		send_last_code = false;
		sendCommand("9368:4748:618:1816:592:616:616:618:560:650:614:618:590:646:584:624:592:644:560:648:592:1838:590:1846:566:1844:590:1844:564:1844:590:1846:564:1844:588:1844:564:1844:586:648:564:642:590:644:562:648:590:642:590:644:564:644:590:644:588:1844:564:1844:590:1846:564:1846:588:1844:564:1846:590:1846:564:1846:586:650:562:646:590:644:562:646:588:644:588:644:562:646:590:644:586:1846:562:1846:586:1848:564:1846:586:1850:562:1846:562");
		device_mode = READ;
	}
	handleButton(FUNC_BUTTON);
	yield();
}
void handleFail() {
	blink(50);
}
void blink(int interval) {
	static unsigned long int last = millis();
	if ( millis() > interval + last) {
		if (digitalRead(LED_BUILTIN) == HIGH) {
			digitalWrite(LED_BUILTIN, LOW);
		} else {
			digitalWrite(LED_BUILTIN, HIGH);
		}
		last = millis();
	}
}

void handlePullingCommands() {
	blink(100);
}

void handleRead() {
	blink(300);
	if (irrecv.decode(&results)) {
		for (int i=0;i<5;i++) {
			digitalWrite(LED_BUILTIN, HIGH);
			delay(30);
			digitalWrite(LED_BUILTIN, LOW);
			delay(30);
		}
		saveCode(results);
		raw_array = resultToRawArray(&results);
		// Find out how many elements are in the array.
		length = getCorrectedRawLength(&results);

		serialPrintUint64(results.value, HEX);
		Serial.println("");
		irrecv.resume();
	}
}

ICACHE_RAM_ATTR void modeToggle() {
	Serial.println("Toggle Mode");
	mode_read = !mode_read;
}

ICACHE_RAM_ATTR void sendLastSaved() {
	Serial.println("Send Last Saved");

	send_last_code = true;
}

void handleButton(int button){
	int start = millis();
	bool action_requested = false;
	int blinkOn=false;
	int secs = 0;
	if(digitalRead(button) == LOW) {
		digitalWrite(LED_BUILTIN, HIGH);
	}
	while (digitalRead(button) == LOW) {
		if (!blinkOn && millis() > start + 1000) {
			digitalWrite(LED_BUILTIN, LOW);
			blinkOn = true;
			secs++;
		}
		if (millis() > start + 1050) {
			digitalWrite(LED_BUILTIN, HIGH);
			start = millis();
			blinkOn = false;
		}
		action_requested = true;
		ota.handle();
		yield();
	}
	if (!action_requested) {
		return;
	}
	if (secs >= 1 && secs < 3) {
		if(device_mode == READ) {
			device_mode = PULLING;
		} else {
			device_mode = READ;
		}
	}
	if (secs >= 3 && secs < 5) {
		//update OTA
		ota.otaUpdateWeb(OTA_HOST, OTA_PORT,OTA_FILE, OTA_FINGERPRINT);
	} else if( secs >= 5 && secs < 10) {
		Serial.println("Reseting...");
		ESP.reset();
	} else if (secs >= 10) {
		Serial.println("Reseting factory");
		fseWifiManager.resetSettings();
		delay(100);
		ESP.reset();
	}
}
