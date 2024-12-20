/*
   Timer relay terjadwal dengan modul RTC DS1307
   MCU: ESP-01
   Wifi mode AP dan STA
   SDA dan SCL di 2, 0
   output relay di GPIO3 (RX)
   Library: uRTClib, uEEPROMlib, NTPClient untuk sync time dari web
   modifikasi library: hapus WIRE_DELAY ketika read eeprom
   I2C address: DS1307 = 0x68, AT24C32 = 0x50
   kapasitas eeprom = 32 kbit = 8 kbyte (0x0000 - 0x0fff)
   16 byte pertama untuk setting umum
   record jadwal mulai dari 0x10, diawali njadwal lalu +4 byte baru mulai entri jadwal sebanyak (max 256) x 5 btye
   kapasitas jadwal = 256 x 5 byte = 1280 byte = address 0x500
   kelompok jadwal berikutnya di offset +0x600
*/

#include<ESP8266WiFi.h>
#include<WiFiClient.h>
#include<WiFiUdp.h>
#include<ESP8266WebServer.h>
#include<EEPROM.h>
#include<time.h>
#include"uRTCLib.h"
#include"uEEPROMLib.h"
#include"NTPClient.h"
//#include"webpages.cpp"
//pin LED (2) tidak boleh dipakai yang lain selain Wire (SDA)
//#define LED1 2
#define SDAPIN 2
#define SCLPIN 0
#define RELAYOUTPIN 3
//todo: atur supaya ini bisa diganti lewat interface web
//const char hotspot_ssid[32] = "STALOYSIUS_BN_WIFI"; //lab BN
//const char hotspot_password[64] = "Aloysius2030";
char hotspot_ssid[32] = "WIFI GUEST"; //lab BN
char hotspot_password[64] = "aloysius321";
//char hotspot_ssid[32] = "LABFISIKA-WIFI"; //lab SA
//char hotspot_password[32] = "aloysius";
const char *ssid = "ESP-WIFI3b";
const char *password = "aloysius";
ESP8266WebServer server(80);
//int led1state = 0, led2state = 0;
//int tled = 1000;
uRTCLib rtc(0x68);
uEEPROMLib ee(0x50); //chip EEPROM di modul RTC
#define ADRJADWAL_OFFSETNEXT 0x600
#define ADR_JADWAL 0x14
#define ADR_NJADWAL 0x10
#define ADR_JADWALYANGMANA 0x02
#define MAGIC 0x58
WiFiUDP ntpUDP;
const long zonawaktu = 25200; //7 x 3600
NTPClient timeClient(ntpUDP, "pool.ntp.org", zonawaktu, 600000);
bool timeclientstarted = false;
int sudahupdateweb = 0, timefirstupdatesuccess=0;
long tlasttryntpupdate=0;
struct wifisettings_t {
  byte magic;
  char ssid[32];
  char password[64];
};
wifisettings_t wifisettings;

struct jadwal_t {
  byte hari, jam, menit;
  uint16_t durasi;
};
jadwal_t jadwal[256];


//===========================web pages================================
const char index_head[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
<title>Main page</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
</head><body>
Waktu sistem:
)rawliteral";

const char index_part2[] PROGMEM = R"rawliteral(
<br><h1>Menu:</h1><br>
<form action="/setjam">
Set jam (hhmm): <input type="text" name="j">
<input type="submit" value="Set">
</form><br><br>
<form action="/settanggal">
Set tanggal (ddmmyy): <input type="text" name="t">
Hari (1=Senin): <input type="text" name="h">
<input type="submit" value="Set">
</form><br><br>
<a href="/setalarm">Pengaturan jadwal</a><br>
<form action="/">
Test output selama <input type="text" name="test"> milisekon
<input type="submit" value="Test sekarang">
</form><br><br>
<a href="/setwifi">Pengaturan Wifi</a><br>
</body></html>
)rawliteral";
/*
const char readrtc_head[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
<title>Waktu sistem</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
</head><body>
)rawliteral";
*/

const char htmlend[] PROGMEM = "</body></html>";

const char setjam_head[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
<title>Set Jam</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
</head><body>
)rawliteral";

const char settanggal_head[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
<title>Set Tanggal</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
</head><body>
)rawliteral";

const char setalarm_head[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
<title>Set Jadwal</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
</head><body>
<h1>Jadwal:</h1><br>
)rawliteral";

const char setwifi_head[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
<title>Set Wifi</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
</head><body>
)rawliteral";

const char setalarm_menu[] PROGMEM = R"rawliteral(
<form action="/setalarm">
<label for="hari">Hari (1=Senin):</label>
<input type="text" id="hari" placeholder="1" name="h"><br>
<label for="jam">Jam (hhmm)    :</label>
<input type="text" id="jam" placeholder="1200" name="j"><br>
<label for="dur">Durasi (detik):</label>
<input type="text" id="dur" placeholder="5" name="d"><input type="submit" value="Tambah">
</form><br>
<form action="/setalarm">
Hapus (nomor): <input type="text" name="del">
<input type="submit" value="Hapus">
</form><br>
<a href="/clearall"><input type=button value="Hapus semua"></a><br><br>
<a href="/duplicate"><input type=button value="Salin dari hari 1 untuk semua hari"></a><br><br>
Pilih kelompok jadwal:<br> <a href="/setalarm?group=0"><input type=button value="kelompok 1"></a><br>
<a href="/setalarm?group=1"><input type=button value="kelompok 2"></a><br>
<a href="/setalarm?group=2"><input type=button value="kelompok 3"></a><br><br>
)rawliteral";

const char setalarm_clearall[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
<title>Hapus Semua Jadwal</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
</head><body>
<h1>Yakin menghapus semua jadwal?</h1><br>
<a href="/setalarm?clear=1"><input type=button value="Yakin"></a>
<a href="/setalarm"><input type=button value="Batalkan"></a>
</body></html>
)rawliteral";

const char setalarm_duplicate[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
<title>Menyalin Jadwal</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
</head><body>
<h1>Akan menyalin semua jadwal dari hari 1 ke hari 2-5. Yakin?</h1><br>
<a href="/setalarm?duplicate=1"><input type=button value="Yakin"></a>
<a href="/setalarm"><input type=button value="Batalkan"></a>
</body></html>
)rawliteral";

//parameter: s, p
const char setwifi_page[] PROGMEM = R"rawliteral(
<br><h1>Pengaturan Wifi tujuan</h1><br>
<form action="/setwifi">
<label for="ss">SSID     :</label>
<input type="text" id="ss" name="s"><br>
<label for="pw">Password:</label>
<input type="text" id="pw" name="p"><br>
<input type="submit" value="Simpan">
</form><br>
)rawliteral";


const char backlink[] PROGMEM = R"rawliteral(
<a href="/"><input type=button value="Kembali"></a>
)rawliteral";


//==============================end webpage==================
//=================variabel-variabel======================
int njadwal = 0, jadwalyangmana=0;//, baseaddress=0x10;//, nextjam = 0, nextmin = 0, nextdurasi = 0;
long waktusekarang,waktujadwal;
volatile long t1=0,tnow=0,tlastrefresh=0,outputduration=0;
long looptime=0, looptime1=0, looptime2=0;
volatile int outputstate=0;
char strbuf[50];
byte b=0,j=0,m=0,h=0,tanggal=0,bulan=0,tahun=0,hari=0,jam=0,menit=0,detik=0;
uint16_t d=0,durasi=0;
//==================fungsi-fungsi==========================
void relayon(){
  digitalWrite(RELAYOUTPIN,HIGH);
}
void relayoff(){
  digitalWrite(RELAYOUTPIN, LOW);
}
void outputenable(long t_ms){
  //aktifkan flag yang akan dimatikan lewat timer
  t1=millis();
  outputduration=t_ms;
  relayon();
  outputstate=1;
}
String readrtcstring(){
  rtc.refresh();
  sprintf(strbuf,"%02d/%02d/%02d %02d:%02d:%02d hari=%d", rtc.day(),rtc.month(),rtc.year(),rtc.hour(),rtc.minute(),rtc.second(),rtc.dayOfWeek());
  return String(strbuf);
}
//=======================fungsi handle request==================
void handleroot() {
  //fungsi yang dipanggil kalau ada permintaan koneksi ke "/" dari client
  //ada argumen: test=...(Detik)
  //digitalWrite(LED1,LOW);
  Serial.println("Request handleroot");
  String s = String(server.arg("test"));
  if(s!=""){
    int t = s.toInt();
    //TODO: test output di sini
    if(t>0){
      outputenable(t);
    }
  }
  String message=String(index_head);
  message += readrtcstring();

  message += "<br>Wifi: ";
  if(WiFi.status()==WL_CONNECTED){
    message += String("terhubung ke ");
  }
  else{
    message += String("tidak terhubung. SSID yang dituju: ");
  }
  message += String(hotspot_ssid) + String("<br>");
  message += String(index_part2);
  //Serial.println(message);
  server.send(200, "text/html", message);
  //digitalWrite(LED1,HIGH);
}
/*
void handlertc(){
  //tampilkan waktu di RTC dan link kembali ke menu
  String message=readrtc_head;
  message += "[angka jam dari RTC]\n";
  message += backlink;
  message += htmlend;
  server.send(200, "text/html", message);
}
*/
void handlesetjam(){
  //tampilan OK dan link kembali ke menu
  String s = String(server.arg("j")); //format hhmm
  //byte jam=0,menit=0;
  jam = (s[0]-'0')*10 + s[1]-'0';
  menit = (s[2]-'0')*10 + s[3]-'0';
  String message = String(setjam_head);
  if(jam<24 && menit < 60){
    //TODO: update RTC di sini

    //rtc.set(second, minute, hour, day, tanggal, bulan, tahun(2))
    rtc.refresh();
    hari=rtc.dayOfWeek();
    tanggal=rtc.day();
    bulan=rtc.month();
    tahun=rtc.year();
    rtc.set(0,menit,jam,hari,tanggal,bulan,tahun);
    message+="<h1>Set jam OK!</h1><br>";
  }
  else{
    message+="<h1>Set jam Error!</h1><br>";
  }
  message+=String(backlink);
  message+=String(htmlend);
  server.send(200, "text/html", message);
}
void handlesettanggal(){
  //tampilan OK dan link kembail ke menu
  String arg_s = String(server.arg("t")); //format ddmmyy
  String arg_h = String(server.arg("h")); //hari, 1=senin
  //byte bulan=0,tanggal=0,tahun=0,hari=0;
  tanggal = (arg_s[0]-'0')*10 + arg_s[1]-'0';
  bulan = (arg_s[2]-'0')*10 + arg_s[3]-'0';
  tahun = (arg_s[4]-'0')*10 + arg_s[5]-'0';
  hari = arg_h[0]-'0';
  String message = String(settanggal_head);
  if(tanggal<32 && bulan < 13 && hari<8){
    //TODO: update RTC di sini
    rtc.refresh();
    byte jam=rtc.hour();
    byte menit=rtc.minute();
    byte detik=rtc.second();
    rtc.set(detik,menit,jam,hari,tanggal,bulan,tahun);
    message+="<h1>Set tanggal OK!</h1><br>";
  }
  else{
    message+="<h1>Set tanggal Error!</h1><br>";
  }
  message+=String(backlink);
  message+=String(htmlend);
  server.send(200, "text/html", message);
}
int bacasemuajadwal(int n){ //n = jadwal keberapa yang dibaca (0 atau 1)
  //mengisi array jadwal_t jadwal[256] dengan hasil bacaan eeprom.
  int adr=ADR_JADWAL+ADRJADWAL_OFFSETNEXT*n;
  //if(n==1) adr=ADR_JADWAL2;
  ee.eeprom_read(0,&b);
  if(b==MAGIC){
    //jumlah entri jadwal
    ee.eeprom_read(ADR_NJADWAL+ADRJADWAL_OFFSETNEXT*n,&njadwal);
    if(njadwal>255 || njadwal<0) {
      njadwal=0;
      return 0;
    }
    //baca entri jadwal
    for(int i=0;i<njadwal;i++){
      //baca jadwal ke-i
      jadwal[i].hari=ee.eeprom_read(adr);
      adr++;
      jadwal[i].jam=ee.eeprom_read(adr);
      adr++;
      jadwal[i].menit=ee.eeprom_read(adr);
      adr++;
      ee.eeprom_read(adr,&(jadwal[i].durasi));
      adr+=2;
    }
    return 1;
  }
  return 0;
}

int bacajadwalyangmana(){
  ee.eeprom_read(0,&b);
  if(b==MAGIC){
    int u;
    ee.eeprom_read(ADR_JADWALYANGMANA,&u);
    if(u>=0 && u<=2){
      return u;
    }
  }
  ee.eeprom_write(0,MAGIC);
  ee.eeprom_write(ADR_JADWALYANGMANA,(int)0);
  ee.eeprom_write(ADR_NJADWAL,(int)0);
  //njadwal=0;
  return 0;
}
String displayjadwal(){ //baca dari ram, return string untuk ditampilkan di web
  if(njadwal>0){
    String res;
    for(int i=0;i<njadwal;i++){
      sprintf(strbuf,"%d: h=%d %02d:%02d durasi=%d<br>",i+1,jadwal[i].hari,jadwal[i].jam,jadwal[i].menit,jadwal[i].durasi);
      res+=String(strbuf);
    }
  return res;
  }
  else{
    return String("Tidak ada jadwal.");
  }
}
void tambahjadwal(byte hari_, byte jam_, byte menit_, uint16_t durasi_){
  //tulis magic number kalau belum sesuai
  //byte b;
  ee.eeprom_read(0,&b);
  if(b!=MAGIC){
    njadwal=0; //kalau belum ada magic number artinya belum pernah ditulis, masukkan jadwal pertama
    jadwalyangmana=0;
    ee.eeprom_write(0,MAGIC);
    ee.eeprom_write(ADR_JADWALYANGMANA, 0);
    ee.eeprom_write(ADR_NJADWAL, 0);
  }
  //address write = njadwal * 5 + 0x10
  //ee.eeprom_read(1,&njadwal);
  jadwal[njadwal].hari=hari_;
  jadwal[njadwal].jam=jam_;
  jadwal[njadwal].menit=menit_;
  jadwal[njadwal].durasi=durasi_;
  int ad=njadwal*5 + ADR_JADWAL+jadwalyangmana*ADRJADWAL_OFFSETNEXT;
  ee.eeprom_write(ad,hari_);
  ad++;
  ee.eeprom_write(ad,jam_);
  ad++;
  ee.eeprom_write(ad,menit_);
  ad++;
  ee.eeprom_write(ad,durasi_);
  //increment dan simpan di adress 1
  njadwal++;
  ee.eeprom_write(ADR_NJADWAL+jadwalyangmana*ADRJADWAL_OFFSETNEXT,njadwal); //jumlah jadwal yang tersimpan (int)
}

void handlesetalarm(){
  //tampilkan semua jadwal yang ada, input add, clear, delete, kembali ke menu
  String message=String(setalarm_head);
  //cek mungkin ada args
  //args:j, h, d, del, clear, duplicate, group
  String sj = String(server.arg("j")); //jam 0-23
  String sh = String(server.arg("h")); //hari 1-7
  String sd = String(server.arg("d")); //durasi dalam detik
  String sdel = String(server.arg("del")); //index yang mau dihapus
  String sclear = String(server.arg("clear")); //1 = clear all
  String sduplicate = String(server.arg("duplicate")); //1 = duplikat hari 1 ke hari 2-5
  String sgroup = String(server.arg("group")); //nilainya 0-2
  String strjadwal="";
  //int jam=0, menit=0, hari=0, durasi=0;
  int adaarg=0;
  if(sj!=""){
    Serial.print("arg j:");Serial.println(sj);
    jam=(sj[0]-'0')*10 + (sj[1]-'0');
    menit=(sj[2]-'0')*10 + (sj[3]-'0');
    adaarg++;
  }
  if(sh!=""){
    Serial.print("arg h:");Serial.println(sh);
    hari=sh.toInt();
    adaarg++;
  }
  if(sd!=""){
    Serial.print("arg d:");Serial.println(sd);
    durasi=sd.toInt();
    adaarg++;
  }
  if(adaarg){
    if(jam>=0&&jam<24&&menit>=0&&menit<60&&hari>0&&hari<8&&durasi>0&&durasi<=3600){
      tambahjadwal(hari,jam,menit,durasi);
      strjadwal=F("Berhasil menambah jadwal<br>");
    }
    else{
      strjadwal=F("Error dalam input jadwal!<br>");
    }
  }
  if(sdel!=""){
    int idel = sdel.toInt();
    if(idel<=njadwal){
      //hapus jadwal ke-i dengan cara geser semua sesudahnya yang di eeprom
      int ad=(idel-1)*5 + ADR_JADWAL+jadwalyangmana*ADRJADWAL_OFFSETNEXT;
      byte hh,jj,mm;
      uint16_t dd;
      for(int i=idel-1;i<njadwal-1;i++){
        ee.eeprom_read(ad+5,&hh);
        ee.eeprom_write(ad,hh);
        ad++;
        ee.eeprom_read(ad+5,&jj);
        ee.eeprom_write(ad,jj);
        ad++;
        ee.eeprom_read(ad+5,&mm);
        ee.eeprom_write(ad,mm);
        ad++;
        ee.eeprom_read(ad+5,&dd);
        ee.eeprom_write(ad,dd);
        jadwal[i]=jadwal[i+1];
      }
      //kurangi satu dari njadwal
      ee.eeprom_read(ADR_NJADWAL+jadwalyangmana*ADRJADWAL_OFFSETNEXT,&njadwal);
      njadwal--;
      ee.eeprom_write(ADR_NJADWAL+jadwalyangmana*ADRJADWAL_OFFSETNEXT,njadwal);
      
      strjadwal=F("Sukses menghapus satu jadwal<br>");
    }
    else{
      strjadwal=F("Error dalam menghapus jadwal!<br>");
    }
  }
  if(sclear!=""){
    Serial.print("arg clear:");Serial.println(sclear);
    if(sclear.toInt()==1){
      //TODO: hapus semua jadwal di eeprom
      ee.eeprom_write(ADR_NJADWAL+jadwalyangmana*ADRJADWAL_OFFSETNEXT,0);
      njadwal=0;
      strjadwal=F("Berhasil menghapus semua jadwal.<br>");
    }
  }
  if(sduplicate!=""){
    Serial.print("arg duplicate:");Serial.println(sduplicate);
    if(sduplicate.toInt()==1){
      byte hh,jj,mm;
      uint16_t dd;
      int njadwals=njadwal; //simpan ini karena mungkin berubah selama prosesnya
      int nduplicate=0;
      for(int h=2;h<=5;h++){ //hari target
         for(int i=0;i<njadwals;i++){
          if(jadwal[i].hari==1){//baca dari ram
            tambahjadwal(h,jadwal[i].jam,jadwal[i].menit,jadwal[i].durasi);
            nduplicate++;
          }
        }
      }
      //bacasemuajadwal();
      sprintf(strbuf, "Berhasil menambahkan %d jadwal.<br>", nduplicate);
      strjadwal=String(strbuf);
    }
  }
  if(sgroup!=""){
    int u=sgroup.toInt();
    
    if(u<0 || u>2) {
      u=0;
      strjadwal+=F("<br>Error dalam memilih kelompok jadwal<br>");
    }
    else{
      if(u!=jadwalyangmana){ //save jadwal yang mana
        bacasemuajadwal(u);
        jadwalyangmana=u;
        ee.eeprom_write(ADR_JADWALYANGMANA,jadwalyangmana);
      }
    }
  }
      strjadwal+=F("<br>Kelompok jadwal:");
      strjadwal+=String(jadwalyangmana+1);
      strjadwal+=F("<br>");
  message+=strjadwal;
  message+=displayjadwal();
  message+=String(setalarm_menu);
  message+=String(backlink);
  message+=String(htmlend);
  server.send(200, "text/html", message);
}
void handleclearall(){
    server.send(200, "text/html", setalarm_clearall);
}
void handleduplicate(){
    server.send(200, "text/html", setalarm_duplicate);
}
void handlesetwifi(){
  //arg: s, p (ssid, password)
  String ss = String(server.arg("s")); //ssid
  String sp = String(server.arg("p")); //password
  String message = String(setwifi_head);

  //baca parameter SSID dan password
  if(ss!=""){ //request mengandung parameter
    strcpy(wifisettings.ssid, ss.c_str());
    strcpy(wifisettings.password, sp.c_str());
  
  message += String("Set Wifi OK!<br> Mengganti Wifi SSID tujuan menjadi: ");
  message += ss;
  message += String("<br>");
  message += String(backlink);
  message += String(htmlend);
  server.send(200,"text/html", message);
  changewifi();
  savewifi();
  }
  else{ //request tidak mengandung parameter
   message += String(setwifi_page);
   message += String(backlink);
   message += String(htmlend);
   server.send(200,"text/html", message);
  }
}
//===================================================

int updatewaktuweb(){
  //if(switchtoSTA()){
  Serial.println("Update waktu dari NTP...");
  if(timeClient.update()){ //kalau update sukses
    time_t now = timeClient.getEpochTime();
    struct tm *tt = localtime(&now); //memecah jadi jam, menit, detik, tanggal dsb
    byte hari = tt->tm_wday;
    if(hari==0)hari=7; //minggu = 0 diubah jadi minggu = 7
    //localtime menghasilkan januari = 0 dan tahun sejak 1900
    //RTC mengharapkan januari = 1 dan tahun cuma dua digit
    rtc.set(tt->tm_sec, tt->tm_min, tt->tm_hour, hari, tt->tm_mday, tt->tm_mon+1, tt->tm_year % 100);
    rtc.refresh();
    //byte hari=rtc.dayOfWeek();
    //byte hari = timeClient.getDay(); //0 = minggu
    if(hari==0) hari = 7; //jadikan 7 = minggu
    //byte tanggal=rtc.day();
    //byte bulan=rtc.month();
    //byte tahun=rtc.year();
    //byte jam = timeClient.getHours();
    //byte menit = timeClient.getMinutes();
    //byte detik = timeClient.getSeconds();
    //rtc.set(detik,menit,jam,hari,tanggal,bulan,tahun);
    Serial.println(timeClient.getFormattedTime());
    //sprintf(strbuf,"%02d:%02d:%02d",timeClient.getHours(),timeClient.getMinutes(),timeClient.getSeconds());
    sprintf(strbuf,"%02d/%02d/%02d h=%d",tt->tm_mday,tt->tm_mon+1,tt->tm_year%100, hari);
    Serial.println(strbuf);
    Serial.print("rtc:");
    Serial.println(readrtcstring());
    return 1;
  }
  else{
    Serial.println("Tidak berhasil update NTP.");
    return 0;
  }
  //switchtoAP();
}
void savewifi(){
  wifisettings.magic=MAGIC;
  EEPROM.put(0,wifisettings);
  Serial.println("Setting wifi tersimpan.");
}
void changewifi(){
  WiFi.disconnect();
  //strcpy(dest, source)
  strcpy(hotspot_ssid, wifisettings.ssid);
  strcpy(hotspot_password, wifisettings.password);
  WiFi.begin(hotspot_ssid, hotspot_password);
  Serial.print("Mengganti tujuan wifi ke: ");
  Serial.println(hotspot_ssid);
}
//==================================================
void setup() {
  // put your setup code here, to run once:
  delay(1000);
  //pinMode(LED1, OUTPUT);
  pinMode(SDAPIN,OUTPUT); //pin 2, sama dengan LED
  pinMode(SCLPIN,OUTPUT);
  pinMode(RELAYOUTPIN,OUTPUT);
  relayoff();
  Serial.begin(115200,SERIAL_8N1,SERIAL_TX_ONLY); //bebaskan pin 3(RX) untuk relay
  while (!Serial);
  //digitalWrite(LED1, LOW);
  //delay(100);
  //digitalWrite(LED1, HIGH);
  delay(1000);
  Serial.println("Program start");
  //timeClient.begin();
  //updatewaktuweb();
  //WiFi.disconnect();
  EEPROM.begin(512); //EEPROM internal untuk menyimpan wifi SSID
  EEPROM.get(0,wifisettings);
  if(wifisettings.magic==MAGIC){
    strcpy(hotspot_ssid, wifisettings.ssid);
    strcpy(hotspot_password, wifisettings.password);
  }
    Serial.print("menghubungkan ke wifi: ");
    Serial.print(hotspot_ssid);
    Serial.print(" password: ");
    Serial.println(hotspot_password);
  
  //test RTC
  URTCLIB_WIRE.begin(SDAPIN, SCLPIN);
  rtc.refresh();
  Serial.print("waktu RTC:");
  Serial.print(rtc.hour());
  Serial.print(":");
  Serial.print(rtc.minute());
  Serial.print(":");
  Serial.print(rtc.second());
  Serial.print(" hari:");
  Serial.print(rtc.dayOfWeek());
  Serial.print(" tanggal:");
  Serial.print(rtc.day());
  Serial.print("/");
  Serial.print(rtc.month());
  Serial.print("/");
  Serial.print(rtc.year());
  Serial.println();
  jadwalyangmana = bacajadwalyangmana();
  bacasemuajadwal(jadwalyangmana);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(hotspot_ssid,hotspot_password); //connect ke hotspot
  if(WiFi.softAP(ssid, password)){//; //default 192.168.4.1
  Serial.println("access point ready!");
  }

  IPAddress ip = WiFi.softAPIP();
  Serial.print("server address: ");
  Serial.println(ip);
  server.on("/", handleroot);
  //server.on("/readRTC", handlertc);
  server.on("/setjam", handlesetjam);
  server.on("/settanggal", handlesettanggal);
  server.on("/setalarm", handlesetalarm);
  server.on("/clearall", handleclearall);
  server.on("/duplicate", handleduplicate);
  //server.on("/loadgroup", handleloadgroup);
  server.on("/setwifi", handlesetwifi);
  server.begin();
  Serial.println("server started");

  //digitalWrite(LED1, LOW);
  //delay(100);
  //digitalWrite(LED1, HIGH); //mati
}

void loop() {
  tnow = millis();
  server.handleClient();
  //cek outputstate dan matikan kalau sudah waktunya
  if(outputstate){
    //t2=t;
    if(tnow-t1>=outputduration){
      relayoff();
      outputstate=0;
    }
  }
  else if(tnow-tlastrefresh>500){ //refresh rtc setiap 500ms, tapi hanya jika output tidak sedang enable
    looptime=tnow-tlastrefresh;
    looptime1=millis()-tnow;
    rtc.refresh();
    waktusekarang=86400*(long)rtc.dayOfWeek() + 3600*(long)rtc.hour() + 60*(long)rtc.minute() + rtc.second();
    tlastrefresh=tnow;  
    //cek jadwal

    for(int i=0;i<njadwal;i++){
      waktujadwal=86400*(long)jadwal[i].hari+3600*(long)jadwal[i].jam+60*(long)jadwal[i].menit;
        if(waktujadwal==waktusekarang){
          outputenable((long)1000*jadwal[i].durasi); //d dalam detik, jadikan milisekon
          break;
        }
    }
    looptime2=millis()-tnow;
    //coba koneksi ke NTP, kalau sudah connect hotspot
    if(WiFi.status() == WL_CONNECTED){
      if(!timeclientstarted){
        Serial.println("timeclient begin");
        timeClient.begin();
        timefirstupdatesuccess=updatewaktuweb();
        timeclientstarted=true;
      }
      else if(!timefirstupdatesuccess){
        if(tnow-tlasttryntpupdate>120000){
          tlasttryntpupdate=tnow;
          timefirstupdatesuccess=updatewaktuweb();
        }
      }
      else{
        //update NTP di jam 00.00
        if(rtc.hour()==0){
          if(sudahupdateweb==0){
            if(updatewaktuweb()){
              sudahupdateweb=1;
              timeClient.setUpdateInterval(3600000);
            }
          }
        }
        else{
          sudahupdateweb=0;
        }
      }
    }
    else{
      Serial.println("Waiting wifi...");
    }
  }
}
