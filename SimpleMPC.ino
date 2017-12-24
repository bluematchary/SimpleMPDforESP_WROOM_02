/*
 * Nov. 2017 
 * blue-matchary,Hibernation-tech"Hibernation File Vol.2"(comic market 93頒布)
 * Simple MPD client(MPC) by ESP-WROOM-02
 *   (c) blue-matchary
参考：
・WiFiTelnetToSerial - Example Transparent UART to Telnet Server for esp8266
（ファイル-スケッチ例-ESP8266WiFi　内）
・Example: Storing struct data in RTC user rtcDataory

このプログラムはホビー（電子工作）用途で作成しております。
注意して作成しているつもりではありますが、このプログラムが正しく動作することは保証しません。
製作・使用の際は、使う方自身の責任でご使用ください。このソースコードを利用することによって生じる
不利益に対し、筆者(blue-matchary)はいかなる責任も負いません。
(修正案等は歓迎します。お手柔らかにお願いします)

セキュリティ等で重大な誤りが見受けられる場合等は特に、ご指摘頂ければ対処するつもりです。
*/
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include "wifi_ap_info.h" // APの情報を記載する
//別のファイル「wifi_ap_info.h」
//const char* ssid = "アクセスポイントの名前";
//const char* password = "アクセスポイントのパスワード";
//上記を定義

// ***************       ピン番号定義         ******************
#define swPAUSE 13
#define swPREV 12
#define swNEXT 14
#define swVOLM 4
#define swVOLP 5
#define bLEDlink 2
#define LEDstatus 15
#define LIVEKEEP 16
//動作中の、再生ボタンによるリセットを無効にする

#define LED_LINK_ON digitalWrite(bLEDlink,0)
#define LED_LINK_OFF digitalWrite(bLEDlink,1)
#define LED_status_ON digitalWrite(LEDstatus,1)
#define LED_status_OFF digitalWrite(LEDstatus,0)

// ****************        定数設定           *****************

extern const char* ssid;
extern const char* password;  

#define MPD_IP IPAddress(192,168,1,200) 
//MPDのIPアドレスを指定 ここでは192.168.1.200としている
// ****************          グローバル変数設定       ****************************
WiFiClient mpdclient;
ESP8266WiFiGenericClass ESPgen;
Ticker driveLEDstatusticker;

uint32_t milli_prev = 0;  //スイッチを押した時刻を記録
uint8_t pushed_switch_in_setup = 0;

class LEDblinkctl{
  public:
    friend void driveLEDstatus(void);
    LEDblinkctl(){ current_brightness=0;brightupdown=0;blink_period=0;blinkcnt=0; };
    void set_constant(uint8_t brightness){
      current_brightness=brightness;
      brightupdown=0;
      blink_period=0;
      blinkcnt=0;
    }
    void set_wave(void){
      current_brightness=0;
      brightupdown=1;
      blink_period=0;
      blinkcnt=0;
    }
    void set_blink(uint8_t period){	//periodで周期を指定　低いほどLEDが早く点滅します(>=1)
      current_brightness=0;
      brightupdown=0;
      blink_period=period;
      blinkcnt=0;
    }
    void set_blink(uint8_t period,uint8_t cnt_arg){//period:上記。cnt_arg:LED点滅回数
      current_brightness=0;
      brightupdown=0;
      blink_period=period;
      blinkcnt=cnt_arg;
    }
  private:
    uint8_t current_brightness = 0;
    uint8_t brightupdown = 0;  //LEDの明るさを上下させる。やめるときは0を書き込み、それ以外のときは0以外を書き込む
    uint8_t blink_period = 0;         //LEDをこの周期(値×0.1s×2)で点滅させる。やめるときは0を書き込む
    uint8_t blinkcnt = 0;     //0で無限に点滅 1以上で回数指定
};
LEDblinkctl LEDstatusctl;


// ****************          setupルーチン deep sleep復帰後はここから    *********
void setup() {
//IO過電流を避けるため、とりあえず入力ピンを先にセットアップする。ついでにoutputも
  pinMode(LIVEKEEP,OUTPUT);
  digitalWrite(LIVEKEEP,0); //再生ボタン→リセット 信号パス無効
  pinMode(swPAUSE, INPUT);
  pinMode(swPREV, INPUT);
  pinMode(swNEXT, INPUT);
  pinMode(swVOLM, INPUT);
  pinMode(swVOLP, INPUT);
  pinMode(bLEDlink, OUTPUT);
  pinMode(LEDstatus, OUTPUT);
  
  //シリアル設定 
  Serial.begin(74880);
  Serial.println("");
  
  ESPgen.setSleepMode( WIFI_LIGHT_SLEEP );  //lightスリープを設定 少しでも消費電力が下げられる
  
  delay(100); //安定させるための時間待ち

  if( wifi_connect() != 0){
    Serial.print("**** Wifi Connection Failed!!!!: "); Serial.println(ssid);
    digitalWrite(LIVEKEEP,0); //再生ボタン→リセット 有効
    link_error_blinkloop(1);
  }
  if( mpd_connect() != 0){
    Serial.println( "mpd connect failed" );
    digitalWrite(LIVEKEEP,0); //再生ボタン→リセット 有効
    link_error_blinkloop(2);
  }
  
  milli_prev = millis();

  driveLEDstatusticker.attach_ms(100,driveLEDstatus);
  //参考："ESP8266(ESP-WROOM-02)の便利ライブラリまとめ(Ticker編)"

  send_command("status");
}

bool status_play = 0; //再生中か否かを保持

void loop() {
  unsigned int i;

  mpdclient_receive_handler();    //mpdからの受信文字列を解析。stop->play操作時に必須
  serialport_receive_handler();   //シリアルポートからの文字列をMPDに送信。無くても動く

  if( WiFi.status() != WL_CONNECTED ){
    wifi_disconnect_handler();        //Wifiが途切れたら呼ばれる。再接続を試行し、接続できなければエラー表示に移る
  }
  uint8_t SWpushedbit = SW_detect(true);  //スイッチ検出
  
  if(SWpushedbit){
    milli_prev = millis();
    if (SWpushedbit & 0x10) {
      send_command("pause\nstatus");    //pauseとstatusコマンドを送信。この後帰ってくる文字列はmpdclient_receive_handler()で受ける
      Serial.println("pause&status send");
    }

    if (SWpushedbit & 0x08) {
      send_command("previous");
      Serial.println("previous send");
      LEDstatusctl.set_blink(1,3);
    }

    if (SWpushedbit & 0x04) {
      send_command("next");
      Serial.println("next send");
      LEDstatusctl.set_blink(1,3);
    }

    if (SWpushedbit & 0x02) {
      send_command("volume -5");
      Serial.println("volume -5 send");
      LEDstatusctl.set_blink(1,2);
    }

    if (SWpushedbit & 0x01) {
      send_command("volume +5");
      Serial.println("volume +5 send");
      LEDstatusctl.set_blink(1,2);
    }
  }

  //時間経過に関する処理
  if(mpdclient.connected() && (millis() - milli_prev > 2000)){  //2秒後にはTCP接続を切断してしまう。繋いでいても1分以内に使うことはあまりなさそう
     mpdclient.stop();
     Serial.println("TCP stopped");
  }

  if(millis() - milli_prev > 60000){  //msで待機時間を設定 操作がなければこの時間経過後スリープ
    WiFi.disconnect(1); //WiFioff
    delay(500);
    LED_LINK_OFF;
    LED_status_OFF;
    digitalWrite(LIVEKEEP,1); //再生ボタン→リセット 有効
    ESP.deepSleep(0); //永久スリープ
  }
  delay(50);
}

void mpdclient_receive_handler(){
  while (mpdclient.available() ) {
    String line = mpdclient.readStringUntil('\n');
    
    Serial.println(line);
//  "state:" を探し、再生中/停止/一時停止を判別します 
    switch(status_search(line)){
      case 0: status_play = 1;
          send_command("play");
          Serial.println("** STOP -> PLAY **");
          LEDstatusctl.set_wave();
          break;
      case 1: status_play = 1;
          Serial.println("** PAUSE -> PLAY **");
          LEDstatusctl.set_wave();
          break;
      case 2: status_play = 0;
          Serial.println("** PLAY -> PAUSE **");
          LEDstatusctl.set_blink(4);
          break;
    }
  }
}
void serialport_receive_handler(){  //シリアルでコマンド来たらMPDに横流し
  //while (Serial.available()) {
      
	//無くても動く上、PCからならtelnetつなげば楽しめることに気づいたので削除 telnettoSerialサンプル78～89行目の処理を実装していた
  //    if ( !mpd_connect()  ) {
  //      //mpdclient.write(sbuf, len);	//上記挿入後にコメントアウト解除
  //      delay(1);
  //    } else {
  //      Serial.println("MPD connect failure...");
  //    }
  //}
}
uint8_t wifi_disconnect_handler(){
  //復帰を試す
    Serial.println("try to Wifi reconnect...");
    for(int i=0;i<3;i++){
      if( wifi_connect() == 0) return 0;
    }
    link_error_blinkloop(1); //復帰できなかった
}

uint8_t send_command(String sendstr){
  uint8_t lpcnt;
  
  if ( !mpdclient.connected()  ) {
    for(lpcnt=0;lpcnt<10;lpcnt++){
      if (mpd_connect()) {
        LED_LINK_OFF;
        LED_status_OFF;
        Serial.println( "***wifi client reconnect failed!!***" );
      } else {
        Serial.println( "wifi client reconnect ok!" );
        break;
      }
    }
    if(lpcnt == 10) return 1;
  }
  
  mpdclient.print(sendstr);
  mpdclient.print("\n");
  return 0;
}

uint8_t SW_detect(bool rec_enable){
  static bool swPAUSEpushed = 0;
  static bool swPREVpushed = 0;
  static bool swNEXTpushed = 0;
  static bool swVOLMpushed = 0;
  static bool swVOLPpushed = 0;

  int retval=0;
  
  if( digitalRead(swPAUSE) & ~swPAUSEpushed ) retval |= 0x10;
  if( digitalRead(swPREV) & ~swPREVpushed   ) retval |= 0x08;
  if( digitalRead(swNEXT) & ~swNEXTpushed   ) retval |= 0x04;
  if( digitalRead(swVOLM) & ~swVOLMpushed   ) retval |= 0x02;
  if( digitalRead(swVOLP) & ~swVOLPpushed   ) retval |= 0x01;

  if(rec_enable){
    swPAUSEpushed = digitalRead(swPAUSE);
    swPREVpushed = digitalRead(swPREV);
    swNEXTpushed = digitalRead(swNEXT);
    swVOLMpushed = digitalRead(swVOLM);
    swVOLPpushed = digitalRead(swVOLP);
  }
  return retval;
}

uint8_t wifi_connect(void){
  // Wifiに接続 素早く点滅させる
  Serial.println("*** Wifi Access Start! ***");
  WiFi.begin(ssid, password);
  
  //10秒くらい
  int i;
  for (i=0; i < 200 ; i++){
	  if(WiFi.status() == WL_CONNECTED) break;
    if(i&0x1) LED_LINK_ON;
    else LED_LINK_OFF;
    delay(50);
  }
  //エラー
  if (i == 200) {
    LED_LINK_OFF;
    return 1;
  }
  LED_LINK_ON;
  return 0;
}

uint8_t mpd_connect(void){
  int lp;
  for(lp=0;lp<10;lp++){
    if (mpdclient.connect(MPD_IP, 6600) == 0) {
      LED_LINK_OFF;
    }
    else break;
    delay(10);
  }

  if(lp==10){
    return 1;
  }
  else {
    LED_LINK_ON;
    return 0;
  }
}

void link_error_blinkloop(uint8_t error_no){
  //3回点滅後1回早点滅でWiFiに問題、2回早点滅でソフトウェア的問題でMPD接続失敗
  while (1) {
      uint8_t cnt;
      for (cnt = 0; cnt < 3; cnt++) {
        LED_LINK_ON;
        delay(500);
        LED_LINK_OFF;
        delay(500);
      }
      delay(500);
      for (cnt = 0; cnt < error_no; cnt++) {
        LED_LINK_ON;
        delay(250);
        LED_LINK_OFF;
        delay(250);
      }
      delay(500);
  }
}

//0:stop 1:play 2:pause
uint8_t status_search(String line){
  int8_t index_OK = line.indexOf("OK");
  if(index_OK == -1){
    //OK行でない：拒否もしくはコマンド結果が帰ってきている(play後にstatusも実行するため)
    if(line.length() > 8){
      //念のため長さチェック 文字入ってなければ読み取っても意味がない
      if( line.indexOf("state") != -1 ){  //state行である
        if(line.indexOf("play") != -1 ){
          return 1;
        }
        else if(line.indexOf("stop") != -1 ){
          return 0;
        }
        else{
          return 2;
        }
      }
    }
  }
  return 255;
}

void driveLEDstatus(void){
//  uint8_t current_brightness = 0;
//    uint8_t brightupdown = 0;  //LEDの明るさを上下させる。やめるときは0を書き込み、それ以外のときは0以外を書き込む
//    uint8_t blink_period = 0;         //LEDをこの周期(値×0.1s×2)で点滅させる。やめるときは0を書き込む
//    uint8_t blinkcnt = 0;     //0で無限に点滅 1以上で回数指定
static bool LEDblink_pol;
static uint LEDblink_cnt = 0;
  
  if(LEDstatusctl.blink_period) {
    if(LEDblink_cnt > LEDstatusctl.blink_period){
      //Serial.print("d");
      LEDblink_pol = !LEDblink_pol;
      if(LEDblink_pol) LEDstatusctl.current_brightness = 255;
      else LEDstatusctl.current_brightness = 0;
      
      if(LEDblink_pol == false && LEDstatusctl.blinkcnt!=0){
        LEDstatusctl.blinkcnt--;
        if(LEDstatusctl.blinkcnt==0) LEDstatusctl.blink_period = 0;
      }
      LEDblink_cnt = 0;
    }
    else{
      LEDblink_cnt++;
    }
  }
  else if(LEDstatusctl.brightupdown){
    //Serial.print("ud");
    if(LEDstatusctl.brightupdown > 128){
      if(LEDstatusctl.current_brightness < 235) LEDstatusctl.current_brightness+= 20;
      else LEDstatusctl.brightupdown = 1;
    }
    else{
      if(LEDstatusctl.current_brightness > 20) LEDstatusctl.current_brightness-=20;
      else LEDstatusctl.brightupdown = 255;
    }
  }
  
  analogWrite(LEDstatus,LEDstatusctl.current_brightness);
  
  
  
  
}

