#include <Arduino.h>
#include <ETH.h>
#include <WiFiUdp.h>
#include <MD5Builder.h>
#include <WebServer.h>
#include "esp32-hal-i2c.h"
#include "driver/i2s_std.h"
#include "es8311.h"

#define KLINGEL_PIN 21
#define PA_ENABLE_PIN 53

#define I2C_NUMMER      0
#define I2S_NUMMER      I2S_NUM_0
#define I2C_SDA_PIN     7
#define I2C_SCL_PIN     8
#define I2S_MCLK_PIN    13
#define I2S_BCLK_PIN    12
#define I2S_LRCK_PIN    10
#define I2S_DOUT_PIN    9
#define I2S_DIN_PIN     11

// Externes INMP441 Mikrofon (eigener I2S-RX)
#define MIC_I2S_NUMMER  I2S_NUM_1
#define MIC_BCLK_PIN    23
#define MIC_WS_PIN      6
#define MIC_SD_PIN      2
#define SAMPLE_RATE     16000

// ============================================================
// USER CONFIGURATION
// ============================================================
//
// Before compiling DoorP4, enter your own LAN and SIP
// configuration below.
//
// ---------- LAN configuration ----------
// Enter the IP settings for YOUR local LAN.
//
// GitHub template (intentionally commented out):
// IPAddress LOCAL_IP(xxx, xxx, xxx, xxx);   // Static IP for DoorP4
// IPAddress GATEWAY(xxx, xxx, xxx, xxx);    // Router / gateway
// IPAddress SUBNET(xxx, xxx, xxx, xxx);     // Subnet mask
// IPAddress DNS_SERVER(xxx, xxx, xxx, xxx); // DNS server


// ---------- FRITZ!Box / SIP ----------
//
// GitHub template (intentionally commented out):
// const char* SIP_SERVER = "IP_YOUR_FRITZBOX";
// const char* SIP_REALM  = "fritz.box";
// const char* SIP_USER   = "YOUR_SIP_USER";
// const char* SIP_PASS   = "YOUR_SIP_PASSWORD";


// ============================================================
// END USER CONFIGURATION
// ============================================================

const uint16_t SIP_PORT = 5060;
const uint16_t LOCAL_SIP_PORT = 5060;
const uint16_t RTP_PORT = 4000;

const char* DOORP4_VERSION = "V1.02a";

WebServer webServer(80);

const uint32_t RUF_TIMEOUT_MS = 45000;
const uint32_t VERBINDUNG_TIMEOUT_MS = 60000;

WiFiUDP sipUdp;
WiFiUDP rtpUdp;

i2s_chan_handle_t i2s_tx = NULL;
i2s_chan_handle_t i2s_rx = NULL;
i2s_chan_handle_t mic_i2s_rx = NULL;
es8311_handle_t codec = NULL;

IPAddress rtpZielIP;
uint16_t rtpZielPort = 0;
uint16_t rtpSequenz = 1;
uint32_t rtpTimestamp = 0;
uint32_t rtpSSRC = 0;
uint32_t rtpLetztesPaket = 0;
bool rtpGestartet = false;
bool rtpEmpfangGestartet = false;

bool sipGestartet = false;
bool sipRegistriert = false;
bool sipRufAktiv = false;
bool sipVerbindungAktiv = false;
bool invite401Bearbeitet = false;

uint32_t sipCSeq = 1;
uint32_t inviteCSeq = 1;
uint32_t rufStartMillis = 0;
uint32_t verbindungStartMillis = 0;

String callID;
String fromTag;

String inviteCallID;
String inviteFromTag;
String inviteBranch;
String invite401Branch;
String inviteTo;
String inviteContact;


// ------------------------------------------------------------
// MD5
// ------------------------------------------------------------

String md5(String text)
{
    MD5Builder md5;
    md5.begin();
    md5.add(text);
    md5.calculate();
    return md5.toString();
}


// ------------------------------------------------------------
// Wert aus Digest-Zeile lesen
// ------------------------------------------------------------

String getDigestValue(String text, String name)
{
    int pos = text.indexOf(name + "=");

    if (pos < 0)
        return "";

    pos += name.length() + 1;

    if (text.charAt(pos) == '"')
    {
        pos++;

        int ende = text.indexOf('"', pos);

        if (ende < 0)
            return "";

        return text.substring(pos, ende);
    }

    int ende = text.indexOf(',', pos);

    if (ende < 0)
        ende = text.length();

    String wert = text.substring(pos, ende);
    wert.trim();

    return wert;
}


// ------------------------------------------------------------
// SIP Headerwert lesen
// ------------------------------------------------------------

String getHeaderValue(String text, String name)
{
    int pos = text.indexOf(name + ":");

    if (pos < 0)
        return "";

    pos += name.length() + 1;

    int ende = text.indexOf("\r\n", pos);

    if (ende < 0)
        return "";

    String wert = text.substring(pos, ende);
    wert.trim();

    return wert;
}


// ------------------------------------------------------------
// REGISTER senden
// ------------------------------------------------------------

void sipRegister(String authHeader = "")
{
    IPAddress ip = ETH.localIP();

    String localIP =
        String(ip[0]) + "." +
        String(ip[1]) + "." +
        String(ip[2]) + "." +
        String(ip[3]);

    String branch = "z9hG4bK-" + String(random(10000000, 99999999));

    String sip;

    sip += "REGISTER sip:";
    sip += SIP_REALM;
    sip += " SIP/2.0\r\n";

    sip += "Via: SIP/2.0/UDP ";
    sip += localIP;
    sip += ":";
    sip += String(LOCAL_SIP_PORT);
    sip += ";branch=";
    sip += branch;
    sip += ";rport\r\n";

    sip += "Max-Forwards: 70\r\n";

    sip += "From: <sip:";
    sip += SIP_USER;
    sip += "@";
    sip += SIP_REALM;
    sip += ">;tag=";
    sip += fromTag;
    sip += "\r\n";

    sip += "To: <sip:";
    sip += SIP_USER;
    sip += "@";
    sip += SIP_REALM;
    sip += ">\r\n";

    sip += "Call-ID: ";
    sip += callID;
    sip += "\r\n";

    sip += "CSeq: ";
    sip += String(sipCSeq);
    sip += " REGISTER\r\n";

    sip += "Contact: <sip:";
    sip += SIP_USER;
    sip += "@";
    sip += localIP;
    sip += ":";
    sip += String(LOCAL_SIP_PORT);
    sip += ">\r\n";

    sip += "Expires: 600\r\n";

    if (authHeader.length() > 0)
    {
        sip += "Authorization: ";
        sip += authHeader;
        sip += "\r\n";
    }

    sip += "Content-Length: 0\r\n";
    sip += "\r\n";


    Serial.println();
    Serial.println("SIP REGISTER wird gesendet...");


    sipUdp.beginPacket(SIP_SERVER, SIP_PORT);
    sipUdp.print(sip);
    sipUdp.endPacket();
}


// ------------------------------------------------------------
// 401 Digest beantworten
// ------------------------------------------------------------

void sipDigestRegister(String antwort)
{
    int pos = antwort.indexOf("WWW-Authenticate:");

    if (pos < 0)
    {
        Serial.println("SIP: Kein WWW-Authenticate gefunden");
        return;
    }

    int ende = antwort.indexOf("\r\n", pos);

    String digest = antwort.substring(pos, ende);


    String realm = getDigestValue(digest, "realm");
    String nonce = getDigestValue(digest, "nonce");
    String qop   = getDigestValue(digest, "qop");


    Serial.print("SIP Realm: ");
    Serial.println(realm);


    String uri = "sip:" + String(SIP_REALM);

    String ha1 =
        md5(String(SIP_USER) + ":" +
            realm + ":" +
            String(SIP_PASS));

    String ha2 =
        md5("REGISTER:" + uri);


    String response;
    String auth;


    if (qop.length() > 0)
    {
        String nc = "00000001";
        String cnonce = String(random(10000000, 99999999), HEX);

        response =
            md5(ha1 + ":" +
                nonce + ":" +
                nc + ":" +
                cnonce + ":" +
                "auth:" +
                ha2);

        auth =
            "Digest username=\"" + String(SIP_USER) +
            "\", realm=\"" + realm +
            "\", nonce=\"" + nonce +
            "\", uri=\"" + uri +
            "\", response=\"" + response +
            "\", algorithm=MD5" +
            ", qop=auth" +
            ", nc=" + nc +
            ", cnonce=\"" + cnonce + "\"";
    }
    else
    {
        response =
            md5(ha1 + ":" +
                nonce + ":" +
                ha2);

        auth =
            "Digest username=\"" + String(SIP_USER) +
            "\", realm=\"" + realm +
            "\", nonce=\"" + nonce +
            "\", uri=\"" + uri +
            "\", response=\"" + response +
            "\", algorithm=MD5";
    }


    sipCSeq++;

    sipRegister(auth);
}


// ------------------------------------------------------------
// INVITE senden
// Ziel: Klingeltasten-Rufnummer 11
// ------------------------------------------------------------

void sipInvite(String authHeader = "")
{
    IPAddress ip = ETH.localIP();

    String localIP =
        String(ip[0]) + "." +
        String(ip[1]) + "." +
        String(ip[2]) + "." +
        String(ip[3]);

    String uri = "sip:11@" + String(SIP_REALM);
    inviteBranch = "z9hG4bK-" + String(random(10000000, 99999999));

    if (inviteCSeq == 1)
        invite401Branch = inviteBranch;

    String sdp;

    sdp += "v=0\r\n";
    sdp += "o=DoorP4 0 0 IN IP4 ";
    sdp += localIP;
    sdp += "\r\n";
    sdp += "s=DoorP4\r\n";
    sdp += "c=IN IP4 ";
    sdp += localIP;
    sdp += "\r\n";
    sdp += "t=0 0\r\n";
    sdp += "m=audio ";
    sdp += String(RTP_PORT);
    sdp += " RTP/AVP 8 0\r\n";
    sdp += "a=rtpmap:8 PCMA/8000\r\n";
    sdp += "a=rtpmap:0 PCMU/8000\r\n";
    sdp += "a=sendrecv\r\n";

    String sip;

    sip += "INVITE ";
    sip += uri;
    sip += " SIP/2.0\r\n";

    sip += "Via: SIP/2.0/UDP ";
    sip += localIP;
    sip += ":";
    sip += String(LOCAL_SIP_PORT);
    sip += ";branch=";
    sip += inviteBranch;
    sip += ";rport\r\n";

    sip += "Max-Forwards: 70\r\n";

    sip += "From: <sip:";
    sip += SIP_USER;
    sip += "@";
    sip += SIP_REALM;
    sip += ">;tag=";
    sip += inviteFromTag;
    sip += "\r\n";

    sip += "To: <";
    sip += uri;
    sip += ">\r\n";

    sip += "Call-ID: ";
    sip += inviteCallID;
    sip += "\r\n";

    sip += "CSeq: ";
    sip += String(inviteCSeq);
    sip += " INVITE\r\n";

    sip += "Contact: <sip:";
    sip += SIP_USER;
    sip += "@";
    sip += localIP;
    sip += ":";
    sip += String(LOCAL_SIP_PORT);
    sip += ">\r\n";

    sip += "Allow: INVITE, ACK, CANCEL, BYE\r\n";
    sip += "Content-Type: application/sdp\r\n";

    if (authHeader.length() > 0)
    {
        sip += "Authorization: ";
        sip += authHeader;
        sip += "\r\n";
    }

    sip += "Content-Length: ";
    sip += String(sdp.length());
    sip += "\r\n";
    sip += "\r\n";
    sip += sdp;


    Serial.println();
    Serial.println("SIP INVITE an Klingeltaste 11 wird gesendet...");
    Serial.print("TX INVITE | CSeq: ");
    Serial.print(inviteCSeq);
    Serial.print(" | Call-ID: ");
    Serial.println(inviteCallID);


    sipUdp.beginPacket(SIP_SERVER, SIP_PORT);
    sipUdp.print(sip);
    sipUdp.endPacket();
}


// ------------------------------------------------------------
// Finale Fehlerantwort auf INVITE mit ACK bestaetigen
// ------------------------------------------------------------

void sipAckFehler(String antwort)
{
    IPAddress ip = ETH.localIP();

    String localIP =
        String(ip[0]) + "." +
        String(ip[1]) + "." +
        String(ip[2]) + "." +
        String(ip[3]);

    String uri = "sip:11@" + String(SIP_REALM);
    String to = getHeaderValue(antwort, "To");
    String cseq = getHeaderValue(antwort, "CSeq");

    int leerzeichen = cseq.indexOf(' ');
    String cseqNummer = (leerzeichen > 0) ? cseq.substring(0, leerzeichen) : cseq;

    String sip;

    sip += "ACK ";
    sip += uri;
    sip += " SIP/2.0\r\n";

    sip += "Via: SIP/2.0/UDP ";
    sip += localIP;
    sip += ":";
    sip += String(LOCAL_SIP_PORT);
    sip += ";branch=";
    sip += invite401Branch;
    sip += ";rport\r\n";

    sip += "Max-Forwards: 70\r\n";

    sip += "From: <sip:";
    sip += SIP_USER;
    sip += "@";
    sip += SIP_REALM;
    sip += ">;tag=";
    sip += inviteFromTag;
    sip += "\r\n";

    sip += "To: ";
    sip += to;
    sip += "\r\n";

    sip += "Call-ID: ";
    sip += inviteCallID;
    sip += "\r\n";

    sip += "CSeq: ";
    sip += cseqNummer;
    sip += " ACK\r\n";

    sip += "Content-Length: 0\r\n";
    sip += "\r\n";

    Serial.print("TX ACK 401 | CSeq: ");
    Serial.print(cseqNummer);
    Serial.print(" | Call-ID: ");
    Serial.println(inviteCallID);

    sipUdp.beginPacket(SIP_SERVER, SIP_PORT);
    sipUdp.print(sip);
    sipUdp.endPacket();
}


// ------------------------------------------------------------
// 407 Proxy-Digest fuer INVITE beantworten
// ------------------------------------------------------------

void sipDigestInvite(String antwort)
{
    bool proxyAuth = antwort.indexOf("Proxy-Authenticate:") >= 0;

    int pos;

    if (proxyAuth)
        pos = antwort.indexOf("Proxy-Authenticate:");
    else
        pos = antwort.indexOf("WWW-Authenticate:");

    if (pos < 0)
    {
        Serial.println("SIP INVITE: Keine Authentifizierungs-Challenge gefunden");
        sipRufAktiv = false;
        return;
    }

    int ende = antwort.indexOf("\r\n", pos);

    String digest = antwort.substring(pos, ende);

    String realm = getDigestValue(digest, "realm");
    String nonce = getDigestValue(digest, "nonce");
    String qop   = getDigestValue(digest, "qop");

    String uri = "sip:11@" + String(SIP_REALM);

    String ha1 =
        md5(String(SIP_USER) + ":" +
            realm + ":" +
            String(SIP_PASS));

    String ha2 =
        md5("INVITE:" + uri);

    String response;
    String auth;

    if (qop.length() > 0)
    {
        String nc = "00000001";
        String cnonce = String(random(10000000, 99999999), HEX);

        response =
            md5(ha1 + ":" +
                nonce + ":" +
                nc + ":" +
                cnonce + ":" +
                "auth:" +
                ha2);

        auth =
            "Digest username=\"" + String(SIP_USER) +
            "\", realm=\"" + realm +
            "\", nonce=\"" + nonce +
            "\", uri=\"" + uri +
            "\", response=\"" + response +
            "\", algorithm=MD5" +
            ", qop=auth" +
            ", nc=" + nc +
            ", cnonce=\"" + cnonce + "\"";
    }
    else
    {
        response =
            md5(ha1 + ":" +
                nonce + ":" +
                ha2);

        auth =
            "Digest username=\"" + String(SIP_USER) +
            "\", realm=\"" + realm +
            "\", nonce=\"" + nonce +
            "\", uri=\"" + uri +
            "\", response=\"" + response +
            "\", algorithm=MD5";
    }

    inviteCSeq++;

    sipInvite(auth);
}


// ------------------------------------------------------------
// Angenommenen Ruf mit ACK bestaetigen
// ------------------------------------------------------------

void sipAck()
{
    IPAddress ip = ETH.localIP();

    String localIP =
        String(ip[0]) + "." +
        String(ip[1]) + "." +
        String(ip[2]) + "." +
        String(ip[3]);

    String uri = "sip:11@" + String(SIP_REALM);
    String branch = "z9hG4bK-" + String(random(10000000, 99999999));

    String sip;

    sip += "ACK ";
    sip += uri;
    sip += " SIP/2.0\r\n";

    sip += "Via: SIP/2.0/UDP ";
    sip += localIP;
    sip += ":";
    sip += String(LOCAL_SIP_PORT);
    sip += ";branch=";
    sip += branch;
    sip += ";rport\r\n";

    sip += "Max-Forwards: 70\r\n";

    sip += "From: <sip:";
    sip += SIP_USER;
    sip += "@";
    sip += SIP_REALM;
    sip += ">;tag=";
    sip += inviteFromTag;
    sip += "\r\n";

    sip += "To: ";
    sip += inviteTo;
    sip += "\r\n";

    sip += "Call-ID: ";
    sip += inviteCallID;
    sip += "\r\n";

    sip += "CSeq: ";
    sip += String(inviteCSeq);
    sip += " ACK\r\n";

    sip += "Content-Length: 0\r\n";
    sip += "\r\n";

    Serial.println("SIP RUF: ACK wird gesendet");
    Serial.print("TX ACK    | CSeq: ");
    Serial.print(inviteCSeq);
    Serial.print(" | Call-ID: ");
    Serial.println(inviteCallID);

    sipUdp.beginPacket(SIP_SERVER, SIP_PORT);
    sipUdp.print(sip);
    sipUdp.endPacket();
}


// ------------------------------------------------------------
// Angenommene Verbindung beenden
// ------------------------------------------------------------

void sipBye()
{
    IPAddress ip = ETH.localIP();

    String localIP =
        String(ip[0]) + "." +
        String(ip[1]) + "." +
        String(ip[2]) + "." +
        String(ip[3]);

    String uri = "sip:11@" + String(SIP_REALM);
    String branch = "z9hG4bK-" + String(random(10000000, 99999999));

    String sip;

    sip += "BYE ";
    sip += uri;
    sip += " SIP/2.0\r\n";

    sip += "Via: SIP/2.0/UDP ";
    sip += localIP;
    sip += ":";
    sip += String(LOCAL_SIP_PORT);
    sip += ";branch=";
    sip += branch;
    sip += ";rport\r\n";

    sip += "Max-Forwards: 70\r\n";

    sip += "From: <sip:";
    sip += SIP_USER;
    sip += "@";
    sip += SIP_REALM;
    sip += ">;tag=";
    sip += inviteFromTag;
    sip += "\r\n";

    sip += "To: ";
    sip += inviteTo;
    sip += "\r\n";

    sip += "Call-ID: ";
    sip += inviteCallID;
    sip += "\r\n";

    sip += "CSeq: ";
    sip += String(inviteCSeq + 1);
    sip += " BYE\r\n";

    sip += "Content-Length: 0\r\n";
    sip += "\r\n";

    Serial.println();
    Serial.println("SIP VERBINDUNG: 30 s Timeout -> BYE");
    Serial.print("TX BYE    | CSeq: ");
    Serial.print(inviteCSeq + 1);
    Serial.print(" | Call-ID: ");
    Serial.println(inviteCallID);

    sipUdp.beginPacket(SIP_SERVER, SIP_PORT);
    sipUdp.print(sip);
    sipUdp.endPacket();
}


// ------------------------------------------------------------
// Laufenden Ruf abbrechen
// ------------------------------------------------------------

void sipCancel()
{
    IPAddress ip = ETH.localIP();

    String localIP =
        String(ip[0]) + "." +
        String(ip[1]) + "." +
        String(ip[2]) + "." +
        String(ip[3]);

    String uri = "sip:11@" + String(SIP_REALM);

    String sip;

    sip += "CANCEL ";
    sip += uri;
    sip += " SIP/2.0\r\n";

    sip += "Via: SIP/2.0/UDP ";
    sip += localIP;
    sip += ":";
    sip += String(LOCAL_SIP_PORT);
    sip += ";branch=";
    sip += inviteBranch;
    sip += ";rport\r\n";

    sip += "Max-Forwards: 70\r\n";

    sip += "From: <sip:";
    sip += SIP_USER;
    sip += "@";
    sip += SIP_REALM;
    sip += ">;tag=";
    sip += inviteFromTag;
    sip += "\r\n";

    sip += "To: <";
    sip += uri;
    sip += ">\r\n";

    sip += "Call-ID: ";
    sip += inviteCallID;
    sip += "\r\n";

    sip += "CSeq: ";
    sip += String(inviteCSeq);
    sip += " CANCEL\r\n";

    sip += "Content-Length: 0\r\n";
    sip += "\r\n";

    Serial.println();
    Serial.println("SIP RUF: Timeout erreicht -> CANCEL");
    Serial.print("TX CANCEL | CSeq: ");
    Serial.print(inviteCSeq);
    Serial.print(" | Call-ID: ");
    Serial.println(inviteCallID);

    sipUdp.beginPacket(SIP_SERVER, SIP_PORT);
    sipUdp.print(sip);
    sipUdp.endPacket();
}


// ------------------------------------------------------------
// SDP / RTP Ziel aus SIP-Antwort anzeigen
// ------------------------------------------------------------

void sipRtpDebug(String antwort)
{
    int posC = antwort.indexOf("c=IN IP4 ");
    int posM = antwort.indexOf("m=audio ");

    Serial.println();
    Serial.println("========== RTP DEBUG ==========");

    if (posC >= 0)
    {
        int ende = antwort.indexOf("\r\n", posC);
        String ipText = antwort.substring(posC + 9, ende);

        Serial.print("SDP IP   : ");
        Serial.println(ipText);

        int a, b, c, d;

        if (sscanf(ipText.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) == 4)
            rtpZielIP = IPAddress(a, b, c, d);
    }
    else
    {
        Serial.println("SDP IP   : nicht gefunden");
    }

    if (posM >= 0)
    {
        int ende = antwort.indexOf("\r\n", posM);
        String zeile = antwort.substring(posM, ende);

        int startPort = 8;
        int endePort = zeile.indexOf(' ', startPort);

        Serial.print("RTP Port : ");

        if (endePort > startPort)
        {
            rtpZielPort = zeile.substring(startPort, endePort).toInt();
            Serial.println(rtpZielPort);
        }
        else
        {
            Serial.println("nicht gefunden");
        }

        Serial.print("m=audio  : ");
        Serial.println(zeile);
    }
    else
    {
        Serial.println("RTP Port : nicht gefunden");
        Serial.println("m=audio  : nicht gefunden");
    }

    Serial.println("===============================");
}


// ------------------------------------------------------------
// I2S / ES8311 Mikrofon
// ------------------------------------------------------------

void i2sInit()
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUMMER, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx, &i2s_rx));

    i2s_std_config_t std_cfg =
    {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
        {
            .mclk = (gpio_num_t)I2S_MCLK_PIN,
            .bclk = (gpio_num_t)I2S_BCLK_PIN,
            .ws = (gpio_num_t)I2S_LRCK_PIN,
            .dout = (gpio_num_t)I2S_DOUT_PIN,
            .din = (gpio_num_t)I2S_DIN_PIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false }
        }
    };

    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx));
}

// ------------------------------------------------------------
// Externes INMP441 Mikrofon
// BCLK=GPIO23, WS=GPIO6, SD=GPIO2, L/R am Modul auf GND (LEFT)
// ------------------------------------------------------------

void inmp441Init()
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_NUMMER, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &mic_i2s_rx));

    i2s_std_config_t std_cfg =
    {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
        {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)MIC_BCLK_PIN,
            .ws = (gpio_num_t)MIC_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din = (gpio_num_t)MIC_SD_PIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false }
        }
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mic_i2s_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(mic_i2s_rx));
}

void es8311Init()
{
    ESP_ERROR_CHECK(i2cInit(I2C_NUMMER, I2C_SDA_PIN, I2C_SCL_PIN, 100000));
    codec = es8311_create(I2C_NUMMER, ES8311_ADDRRES_0);

    if (codec == NULL)
    {
        Serial.println("FEHLER: ES8311 konnte nicht erzeugt werden");
        while (1) delay(1000);
    }

    es8311_clock_config_t clock_cfg =
    {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = SAMPLE_RATE * 256,
        .sample_frequency = SAMPLE_RATE
    };

    ESP_ERROR_CHECK(es8311_init(codec, &clock_cfg, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
    ESP_ERROR_CHECK(es8311_sample_frequency_config(codec, SAMPLE_RATE * 256, SAMPLE_RATE));
    ESP_ERROR_CHECK(es8311_microphone_config(codec, false));
    ESP_ERROR_CHECK(es8311_microphone_gain_set(codec, ES8311_MIC_GAIN_42DB));

    // ES8311 DAC / Lautsprecherausgang
    ESP_ERROR_CHECK(es8311_voice_volume_set(codec, 90, NULL));
    ESP_ERROR_CHECK(es8311_voice_mute(codec, false));
}


// ------------------------------------------------------------
// G.711 A-law fuer RTP-Mikrofon
// ------------------------------------------------------------

uint8_t linearToALaw(int16_t pcm)
{
    const uint16_t segmentEnd[8] =
    {
        0x00FF, 0x01FF, 0x03FF, 0x07FF,
        0x0FFF, 0x1FFF, 0x3FFF, 0x7FFF
    };

    uint8_t mask;
    uint8_t segment;
    uint8_t aval;
    int16_t wert = pcm;

    if (wert >= 0)
    {
        mask = 0xD5;
    }
    else
    {
        mask = 0x55;
        wert = -wert - 8;

        if (wert < 0)
            wert = 0;
    }

    for (segment = 0; segment < 8; segment++)
    {
        if ((uint16_t)wert <= segmentEnd[segment])
            break;
    }

    if (segment >= 8)
        return (0x7F ^ mask);

    aval = segment << 4;

    if (segment < 2)
        aval |= (wert >> 4) & 0x0F;
    else
        aval |= (wert >> (segment + 3)) & 0x0F;

    return aval ^ mask;
}


// ------------------------------------------------------------
// G.711 A-law -> 16 Bit PCM
// ------------------------------------------------------------

int16_t aLawToLinear(uint8_t aLaw)
{
    // Referenzalgorithmus fuer G.711 A-law -> 16-Bit Linear-PCM
    aLaw ^= 0x55;

    int16_t t = (aLaw & 0x0F) << 4;
    int16_t segment = (aLaw & 0x70) >> 4;

    switch (segment)
    {
        case 0:
            t += 8;
            break;

        case 1:
            t += 0x108;
            break;

        default:
            t += 0x108;
            t <<= (segment - 1);
            break;
    }

    return (aLaw & 0x80) ? t : -t;
}


// ------------------------------------------------------------
// RTP / PCMA Mikrofon senden
// 20 ms pro Paket, 160 Samples bei 8 kHz
// ------------------------------------------------------------

void rtpMikrofonStart()
{
    if (rtpZielPort == 0 || rtpZielIP == IPAddress(0, 0, 0, 0))
    {
        Serial.println("RTP MIKROFON: Ziel fehlt");
        return;
    }

    if (!rtpGestartet)
    {
        if (!rtpUdp.begin(RTP_PORT))
        {
            Serial.println("RTP MIKROFON: UDP Port 4000 konnte nicht gestartet werden");
            return;
        }

        rtpGestartet = true;
    }

    rtpSequenz = 1;
    rtpTimestamp = 0;
    rtpSSRC = random(1, 0x7FFFFFFF);
    rtpLetztesPaket = micros();

    Serial.print("RTP MIKROFON: START -> ");
    Serial.print(rtpZielIP);
    Serial.print(":");
    Serial.println(rtpZielPort);
}


void rtpMikrofonSenden()
{
    if (!sipVerbindungAktiv || !rtpGestartet || rtpZielPort == 0)
        return;

    uint32_t jetzt = micros();

    if ((uint32_t)(jetzt - rtpLetztesPaket) < 20000)
        return;

    rtpLetztesPaket += 20000;

    uint8_t paket[172];

    paket[0] = 0x80;
    paket[1] = 8;

    paket[2] = (rtpSequenz >> 8) & 0xFF;
    paket[3] = rtpSequenz & 0xFF;

    paket[4] = (rtpTimestamp >> 24) & 0xFF;
    paket[5] = (rtpTimestamp >> 16) & 0xFF;
    paket[6] = (rtpTimestamp >> 8) & 0xFF;
    paket[7] = rtpTimestamp & 0xFF;

    paket[8]  = (rtpSSRC >> 24) & 0xFF;
    paket[9]  = (rtpSSRC >> 16) & 0xFF;
    paket[10] = (rtpSSRC >> 8) & 0xFF;
    paket[11] = rtpSSRC & 0xFF;

    // 20 ms INMP441-Audio bei 16 kHz einlesen.
    // Das INMP441 liefert 24-Bit-Audio in 32-Bit-I2S-Slots.
    // L/R ist am Modul auf GND gelegt, daher liegt das Nutzsignal im LEFT-Slot.
    int32_t audio[640];
    size_t bytesRead = 0;

    esp_err_t ret = i2s_channel_read(
        mic_i2s_rx,
        audio,
        sizeof(audio),
        &bytesRead,
        1000
    );

    if (ret != ESP_OK || bytesRead < sizeof(audio))
        return;

    // LEFT-Slot -> Mono und 16 kHz -> 8 kHz.
    // Je zwei 16-kHz-Frames entsteht ein 8-kHz-Mono-Sample.
    // Die oberen 16 Bit des linksbuendigen 24-Bit-INMP441-Samples werden verwendet.
    for (int i = 0; i < 160; i++)
    {
        int frame = i * 2;
        int32_t sample32 = audio[frame * 2];
        int16_t mono = (int16_t)(sample32 >> 15);

        paket[12 + i] = linearToALaw(mono);
    }

    rtpUdp.beginPacket(rtpZielIP, rtpZielPort);
    rtpUdp.write(paket, sizeof(paket));
    rtpUdp.endPacket();

    rtpSequenz++;
    rtpTimestamp += 160;
}



// ------------------------------------------------------------
// RTP / PCMA vom FritzFon empfangen und ueber Lautsprecher ausgeben
// ------------------------------------------------------------

void rtpAudioEmpfangen()
{
    if (!sipVerbindungAktiv || !rtpGestartet)
        return;

    int packetSize = rtpUdp.parsePacket();

    if (packetSize < 12)
        return;

    uint8_t paket[512];

    if (packetSize > (int)sizeof(paket))
    {
        while (rtpUdp.available())
            rtpUdp.read();

        return;
    }

    int gelesen = rtpUdp.read(paket, packetSize);

    if (gelesen < 12)
        return;

    uint8_t version = paket[0] >> 6;
    uint8_t csrcCount = paket[0] & 0x0F;
    bool extension = (paket[0] & 0x10) != 0;
    uint8_t payloadType = paket[1] & 0x7F;

    static uint32_t rtpRxPakete = 0;
    static uint32_t rtpRxLetzteAusgabe = 0;

    rtpRxPakete++;

    if (millis() - rtpRxLetzteAusgabe >= 1000)
    {
        rtpRxLetzteAusgabe = millis();

        Serial.print("RTP RX | Pakete: ");
        Serial.print(rtpRxPakete);
        Serial.print(" | Von: ");
        Serial.print(rtpUdp.remoteIP());
        Serial.print(":");
        Serial.print(rtpUdp.remotePort());
        Serial.print(" | PT: ");
        Serial.print(payloadType);
        Serial.print(" | Bytes: ");
        Serial.println(gelesen);
    }

    if (version != 2 || payloadType != 8)
        return;

    int headerLaenge = 12 + (csrcCount * 4);

    if (extension)
    {
        if (gelesen < headerLaenge + 4)
            return;

        uint16_t extensionWords =
            ((uint16_t)paket[headerLaenge + 2] << 8) |
            paket[headerLaenge + 3];

        headerLaenge += 4 + (extensionWords * 4);
    }

    if (headerLaenge >= gelesen)
        return;

    int payloadLaenge = gelesen - headerLaenge;

    if (payloadLaenge > 160)
        payloadLaenge = 160;

    // PCMA liefert 8 kHz.
    // Der aktuell bestaetigte Lautsprecher-I2S-Weg laeuft mit 16 kHz.
    // Deshalb jedes 8-kHz-Sample zeitlich verdoppeln und als Stereo ausgeben.
    int16_t audio[640];

    static int16_t pcmPeak = 0;

    pcmPeak = 0;

    for (int i = 0; i < payloadLaenge; i++)
    {
        int16_t sample = aLawToLinear(paket[headerLaenge + i]);

        // Waveshare Audio_Playback erzeugt bei 16-Bit-Ausgabe nur ca. +/-8000 PCM.
        // G.711 A-law kann bis fast Vollaussteuerung liefern.
        // Deshalb fuer den Lautsprecherpfad auf etwa 1/4 skalieren.
        sample = (int16_t)((int32_t)sample / 4);

        if (abs(sample) > pcmPeak)
            pcmPeak = abs(sample);

        int pos = i * 4;

        audio[pos]     = sample;
        audio[pos + 1] = sample;
        audio[pos + 2] = sample;
        audio[pos + 3] = sample;
    }

    static uint32_t pcmLetzteAusgabe = 0;

    if (millis() - pcmLetzteAusgabe >= 1000)
    {
        pcmLetzteAusgabe = millis();

        Serial.print("PCM RX | Peak: ");
        Serial.print(pcmPeak);
        Serial.print(" | Payload: ");
        Serial.println(payloadLaenge);
    }

    size_t bytesGeschrieben = 0;

    i2s_channel_write(
        i2s_tx,
        audio,
        payloadLaenge * 4 * sizeof(int16_t),
        &bytesGeschrieben,
        20
    );
}





// ------------------------------------------------------------
// Eingehendes BYE bestaetigen
// ------------------------------------------------------------

void sipByeAntwort(String anfrage)
{
    String via   = getHeaderValue(anfrage, "Via");
    String from  = getHeaderValue(anfrage, "From");
    String to    = getHeaderValue(anfrage, "To");
    String cid   = getHeaderValue(anfrage, "Call-ID");
    String cseq  = getHeaderValue(anfrage, "CSeq");

    String sip;

    sip += "SIP/2.0 200 OK\r\n";
    sip += "Via: " + via + "\r\n";
    sip += "From: " + from + "\r\n";
    sip += "To: " + to + "\r\n";
    sip += "Call-ID: " + cid + "\r\n";
    sip += "CSeq: " + cseq + "\r\n";
    sip += "Content-Length: 0\r\n";
    sip += "\r\n";

    Serial.println("SIP VERBINDUNG: BYE empfangen -> 200 OK");

    sipUdp.beginPacket(sipUdp.remoteIP(), sipUdp.remotePort());
    sipUdp.print(sip);
    sipUdp.endPacket();

    sipVerbindungAktiv = false;
}


// ------------------------------------------------------------
// SIP Debug
// ------------------------------------------------------------

void sipDebugHeader(String antwort)
{
    int endeErsteZeile = antwort.indexOf("\r\n");

    Serial.println();
    Serial.println("========== SIP DEBUG ==========");

    if (endeErsteZeile >= 0)
        Serial.print("START : "), Serial.println(antwort.substring(0, endeErsteZeile));
    else
        Serial.print("START : "), Serial.println(antwort);

    String cseq = getHeaderValue(antwort, "CSeq");
    String cid  = getHeaderValue(antwort, "Call-ID");
    String via  = getHeaderValue(antwort, "Via");
    String from = getHeaderValue(antwort, "From");
    String to   = getHeaderValue(antwort, "To");

    Serial.print("CSeq  : "), Serial.println(cseq);
    Serial.print("CallID: "), Serial.println(cid);
    Serial.print("Via   : "), Serial.println(via);
    Serial.print("From  : "), Serial.println(from);
    Serial.print("To    : "), Serial.println(to);

    if (antwort.startsWith("SIP/2.0 491"))
        Serial.println("*** 491 REQUEST PENDING ***");

    if (antwort.startsWith("INVITE "))
        Serial.println("*** EINGEHENDER INVITE ***");

    Serial.println("===============================");
}


// ------------------------------------------------------------
// SIP Antworten lesen
// ------------------------------------------------------------

void sipEmpfangen()
{
    int packetSize = sipUdp.parsePacket();

    if (!packetSize)
        return;


    String antwort;

    while (sipUdp.available())
    {
        antwort += (char)sipUdp.read();
    }


    sipDebugHeader(antwort);


    if (antwort.startsWith("BYE "))
    {
        sipByeAntwort(antwort);
        return;
    }


    Serial.println();
    Serial.println("---------- SIP Antwort ----------");

    int ersteZeile = antwort.indexOf("\r\n");

    if (ersteZeile > 0)
        Serial.println(antwort.substring(0, ersteZeile));

    Serial.println("---------------------------------");


    bool istRegister = antwort.indexOf("CSeq:") >= 0 &&
                       antwort.indexOf(" REGISTER\r\n") >= 0;

    bool istInvite = antwort.indexOf("CSeq:") >= 0 &&
                     antwort.indexOf(" INVITE\r\n") >= 0;


    if (antwort.startsWith("SIP/2.0 401"))
    {
        if (istRegister)
        {
            Serial.println("SIP REGISTER: Authentifizierung erforderlich");

            sipDigestRegister(antwort);
        }
        else if (istInvite)
        {
            sipAckFehler(antwort);

            String cseq = getHeaderValue(antwort, "CSeq");

            if (cseq.startsWith("1 ") && !invite401Bearbeitet)
            {
                invite401Bearbeitet = true;

                Serial.println("SIP INVITE: Authentifizierung erforderlich");

                sipDigestInvite(antwort);
            }
            else
            {
                Serial.println("SIP INVITE: alte 401-Wiederholung ignoriert");
            }
        }
        else
        {
            Serial.println("SIP 401: CSeq nicht zugeordnet");
        }

        return;
    }


    if (antwort.startsWith("SIP/2.0 407"))
    {
        if (istInvite)
        {
            Serial.println("SIP INVITE: Proxy-Authentifizierung erforderlich");

            sipDigestInvite(antwort);
        }
        else
        {
            Serial.println("SIP 407: CSeq nicht zugeordnet");
        }

        return;
    }


    if (antwort.startsWith("SIP/2.0 180") && istInvite)
    {
        Serial.println("*********************************");
        Serial.println("SIP RUF: TELEFONE KLINGELN");
        Serial.println("*********************************");
        return;
    }


    if (antwort.startsWith("SIP/2.0 487") && istInvite)
    {
        Serial.println("SIP INVITE: 487 Request Cancelled -> ACK");

        sipAckFehler(antwort);

        return;
    }


    if (antwort.startsWith("SIP/2.0 200"))
    {
        if (istRegister)
        {
            sipRegistriert = true;

            Serial.println();
            Serial.println("*********************************");
            Serial.println("SIP REGISTRIERT");
            Serial.println("*********************************");
        }
        else if (istInvite)
        {
            inviteTo = getHeaderValue(antwort, "To");
            inviteContact = getHeaderValue(antwort, "Contact");

            sipRtpDebug(antwort);

            Serial.println();
            Serial.println("*********************************");
            Serial.println("SIP RUF: 200 OK / ANGENOMMEN");
            Serial.println("*********************************");

            sipAck();

            sipRufAktiv = false;
            sipVerbindungAktiv = true;
            verbindungStartMillis = millis();

            rtpMikrofonStart();
        }
        else
        {
            Serial.println("SIP 200: CSeq nicht zugeordnet");
        }

        return;
    }


    if (antwort.startsWith("SIP/2.0 4") ||
        antwort.startsWith("SIP/2.0 5") ||
        antwort.startsWith("SIP/2.0 6"))
    {
        if (sipRufAktiv)
        {
            Serial.println("SIP RUF: FEHLER / ABGEWIESEN");
            sipRufAktiv = false;
        }
    }
}


// ------------------------------------------------------------
// Mini-WebIF / Systemdaten
// ------------------------------------------------------------

void webSystemdaten()
{
    String html;

    html += "<!DOCTYPE html><html><head>";
    html += "<meta charset=\"utf-8\">";
    html += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
    html += "<title>DoorP4</title>";
    html += "</head><body>";
    html += "<h1>DoorP4</h1>";
    html += "<p><b>Version:</b> ";
    html += DOORP4_VERSION;
    html += "</p>";
    html += "<p><b>IP:</b> ";
    html += ETH.localIP().toString();
    html += "</p>";
    html += "<p><b>Uptime:</b> ";
    html += String(millis() / 1000);
    html += " s</p>";
    html += "<p><b>SIP:</b> ";
    html += sipRegistriert ? "registriert" : "nicht registriert";
    html += "</p>";
    html += "</body></html>";

    webServer.send(200, "text/html; charset=utf-8", html);
}


// ------------------------------------------------------------
// SETUP
// ------------------------------------------------------------

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("DoorP4 Ethernet-/Klingel-/SIP-Ruf-Test");

    pinMode(KLINGEL_PIN, INPUT_PULLUP);

    pinMode(PA_ENABLE_PIN, OUTPUT);
    digitalWrite(PA_ENABLE_PIN, HIGH);

    randomSeed(micros());

    Serial.println("I2S / ES8311 Lautsprecher initialisieren...");
    i2sInit();
    es8311Init();
    Serial.println("I2S / ES8311 Lautsprecher OK");

    Serial.println("INMP441 Mikrofon initialisieren...");
    inmp441Init();
    Serial.println("INMP441 Mikrofon OK");

    callID = String(random(10000000, 99999999)) + "@DoorP4";
    fromTag = String(random(10000000, 99999999));

    ETH.begin();
    ETH.config(LOCAL_IP, GATEWAY, SUBNET, DNS_SERVER);

}


// ------------------------------------------------------------
// LOOP
// ------------------------------------------------------------

void loop()
{
    // --------------------------------------------------------
    // Ethernet vorhanden und DHCP fertig
    // --------------------------------------------------------

    if (ETH.linkUp() && ETH.localIP() != IPAddress(0, 0, 0, 0))
    {
        if (!sipGestartet)
        {
            Serial.print("LAN verbunden   IP: ");
            Serial.println(ETH.localIP());

            webServer.on("/", webSystemdaten);
            webServer.begin();
            Serial.print("WebIF bereit: http://");
            Serial.print(ETH.localIP());
            Serial.println("/");

            if (sipUdp.begin(LOCAL_SIP_PORT))
            {
                Serial.println("SIP UDP Port 5060 gestartet");

                sipGestartet = true;

                sipRegister();
            }
            else
            {
                Serial.println("FEHLER: SIP UDP Port konnte nicht gestartet werden");
            }
        }
    }


    if (sipGestartet)
    {
        webServer.handleClient();
    }


    // --------------------------------------------------------
    // SIP Antworten
    // --------------------------------------------------------

    if (sipGestartet)
    {
        sipEmpfangen();
    }


    // --------------------------------------------------------
    // Klingel
    // LOW-Flanke startet genau einen SIP-Ruf
    // --------------------------------------------------------

    static bool letzterKlingelzustand = HIGH;

    bool klingel = digitalRead(KLINGEL_PIN);

    if (klingel != letzterKlingelzustand)
    {
        letzterKlingelzustand = klingel;

        if (klingel == LOW)
        {
            Serial.println("Klingel: GEDRUECKT");

            if (sipRegistriert && !sipRufAktiv && !sipVerbindungAktiv)
            {
                inviteCallID = String(random(10000000, 99999999)) + "@DoorP4";
                inviteFromTag = String(random(10000000, 99999999));
                inviteCSeq = 1;
                invite401Bearbeitet = false;
                sipRufAktiv = true;
                rufStartMillis = millis();

                sipInvite();
            }
            else if (!sipRegistriert)
            {
                Serial.println("SIP RUF: noch nicht registriert");
            }
        }
        else
        {
            Serial.println("Klingel: frei");
        }
    }


    // --------------------------------------------------------
    // Ruf nach ca. 5 Klingelzyklen beenden
    // --------------------------------------------------------

    if (sipRufAktiv && (millis() - rufStartMillis >= RUF_TIMEOUT_MS))
    {
        sipCancel();
        sipRufAktiv = false;
    }


    // --------------------------------------------------------
    // RTP / PCMA Mikrofon
    // --------------------------------------------------------

    // TEST main_24:
    // Mikrofon-RTP voruebergehend aus, um eine akustische Rueckkopplung
    // P4-SPK -> P4-MIC -> FritzFon -> P4-SPK eindeutig auszuschliessen.
    rtpMikrofonSenden();
    rtpAudioEmpfangen();


    // --------------------------------------------------------
    // Angenommene Verbindung nach 30 s beenden
    // --------------------------------------------------------

    if (sipVerbindungAktiv &&
        (millis() - verbindungStartMillis >= VERBINDUNG_TIMEOUT_MS))
    {
        sipBye();
        sipVerbindungAktiv = false;
    }


    delay(10);
}