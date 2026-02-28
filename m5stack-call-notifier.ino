#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiClient.h>
#include <M5StickCPlus2.h>
#include <SSLClient.h>
#include "trust_anchors.h"  // api.line.meのルート証明書情報
#include "arduino_secrets.h"  // シークレット

/**
  固定電話機に着信またはインターホンが押されたとき、LINEへ通知する
  デバイス:
    M5StickC Plus2
  設置場所:
    電話機スピーカ付近に設置
  ロジック:
    一定期間で発生した最大音量と平均音量を基にしきい値を超えた場合、
    着信またはインターホンが押されたと判断しLINEへ通知する
  詳細ロジック:
    WiFi未接続の場合は接続案内を行う
    デバイスの内蔵マイクを通して録音する(録音の長さ: RECORD_LENGTH)
    波形の描画
      - 録音データを基に画面上へ音の波形を描画
    最大音量と平均音量の算出
      - 録音データを基に最大音量と最大音量と平均音量を計算
      - 最大音量と平均音量を履歴データへ追加(ただし、検知中は除く)
        (履歴は直近HISTORY_LENGTH処理回数分を保持する)
      - 履歴データを基に直近の最大音量と平均音量を算出(但し検知中の場合は、0とする)
    検知の終了判定
      条件: 検知してからALERT_RESEND_INTERVAL時間経過した場合
      true: LEDライトOffと再検知有効化
    検知の判定
      条件:
        1. 検知からTHRESHOLD_DETECT_INTERVALミリ秒経過
        2. "最大音量と平均音量のしきい値以上" Or "ボタンAを押下(デバッグ用)"
      true: LEDライトOnとLINEメッセージを通知
*/

//録音設定
//static constexpr const size_t RECORD_SAMPLERATE = 16000;  // 録音サンプリングレート
//static constexpr const size_t RECORD_LENGTH = 240;        // 録音数
#define RECORD_SAMPLERATE 16000  // 録音サンプリングレート
#define RECORD_LENGTH     240    // 録音数
#define HISTORY_LENGTH 350       // 履歴データの件数

//検知設定
static constexpr const int16_t THRESHOLD_MAX = 19000;     // 最大音量しきい値
static constexpr const int32_t THRESHOLD_AVE = 3000;      // 平均音量しきい値
static constexpr const int32_t THRESHOLD_DETECT_INTERVAL = 30000; //再検知までのしきい値(ミリ秒)
static constexpr const unsigned long ALERT_RESEND_INTERVAL = 5000;    // LINEメッセージ通知後、再度通知するまでの期間(ミリ秒)
const char* ALERT_MESSAGE = "インターホンまたは固定電話着信";

// LINE Messaging API設定
const char* LINE_MESSAGING_API_HOST = "api.line.me";
const char* LINE_MESSAGING_API_ENDPOINT = "/v2/bot/message/broadcast";  // 全ユーザに通知
// const char* LINE_MESSAGING_API_ENDPOINT = "/v2/bot/message/push"; // 特定ユーザorグループ宛に通知
const char* LINE_MESSAGING_API_TOKEN = LINE_MES_API_CHANNEL_ACCESS_TOKEN;  //aruino_secret.hに記載

static bool detected = false;           //検知結果
int16_t* rec_data;                      // 録音データ
unsigned long lastDetectedMillis = 0;   // 最後に更新した時間(ミリ秒)
int history_index = 0;                  //履歴のインデックス
int16_t history_max[HISTORY_LENGTH];    //履歴データ(直近の最大音量)
int32_t history_ave[HISTORY_LENGTH];    //履歴データ(直近の平均音量)

//WiFi
static String WiFiAPSsid = "M5S_AP";  // WiFi設定用APのSSID
static String WiFiAPPassword = "";      // WiFi設定用APのパスワード(ランダム)
static String wifiSsid = "";
static String wifiPassword = "";
static bool WiFiConnected = false;
WiFiServer server(80);

#define SSLCLIENT_RANDOM_ANALOG_PIN 36 // SSLClientの乱数生成に使用するアナログピン
static WiFiClient wifi_client;
static SSLClient client(wifi_client, TAs, (size_t)TAs_NUM, SSLCLIENT_RANDOM_ANALOG_PIN);

/** 関数定義 **/
void sendMessage(const String& content);
String getQueryString(String queryStr, String key);
bool wifiSetup(bool retry);
bool WiFiLoop();

//========================================================================================

void setup() {
  Serial.begin(115200);
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.delay(500);  // Serial出力用のウエイト

  // 画面上にログを表示する
  M5.setLogDisplayIndex(0);

  // 画面を横にする
  M5.Display.setRotation(1);
  M5.Lcd.setTextSize(1);
  //M5.Lcd.fillScreen(BLACK);

  //WIFI接続
  WiFiConnected = wifiSetup(true);

  // 録音用メモリ確保
  rec_data = (typeof(rec_data))heap_caps_malloc(RECORD_LENGTH * sizeof(int16_t), MALLOC_CAP_8BIT);
  memset(rec_data, 0, RECORD_LENGTH * sizeof(int16_t));

  // マイク開始
  M5.Mic.begin();
}

void loop() {
  /* ----- 初期化処理 ----- */
  int16_t VolumeMax = 0; // 最大値
  int32_t VolumeAve = 0; // 平均値
  int16_t max = 0;
  int32_t ave = 0;
  int32_t sum = 0;

  M5.delay(1);
  M5.update();

  //WiFi接続確認
  if (!WiFiLoop()) return;

  //現在の時間(経過時間)
  unsigned long currentMillis = millis();

  //初期表示
  M5.Display.startWrite();
  M5.Display.setCursor(0, 0);
  M5.Log.printf("Connected: " + WiFi.localIP());

  //マイクが無効の場合は処理できない
  if (!M5.Mic.isEnabled()) {
    M5.Log.printf("Error: Mic is disabled");
    M5.Display.endWrite();
    return;
  }

  // 録音できているか確認
  if (!M5.Mic.record(rec_data, RECORD_LENGTH, RECORD_SAMPLERATE)) {
    M5.Log.printf("Error: Record Missing");
    M5.Display.endWrite();
    return;
  }

  /* ----- 波形描画処理 ----- */

  // 画面横幅より長いデータは横幅までに制限
  int32_t w = M5.Display.width();
  if (w > RECORD_LENGTH - 1) {
    w = RECORD_LENGTH - 1;
  }
  // 画面上に音の波形を描画
  for (int32_t x = 0; x < w; ++x) {
    // 前回座標を保存
    static int16_t prev_y[RECORD_LENGTH];
    static int16_t prev_h[RECORD_LENGTH];

    // 前回の線を消す
    M5.Display.writeFastVLine(x, prev_y[x], prev_h[x], TFT_BLACK);

    // 音の大きさをshiftで小さくして、画面上の高さに変換
    static constexpr int shift = 7;
    int32_t y1 = (rec_data[x] >> shift);
    int32_t y2 = (rec_data[x + 1] >> shift);
    if (y1 > y2) {
      int32_t tmp = y1;
      y1 = y2;
      y2 = tmp;
    }

    // 画面中央からの相対座標に変換
    int32_t y = (M5.Display.height() >> 1) + y1;
    int32_t h = (M5.Display.height() >> 1) + y2 + 1 - y;
    prev_y[x] = y;
    prev_h[x] = h;

    // 描画
    M5.Display.writeFastVLine(x, y, h, TFT_WHITE);
  }


  /* ----- 最大音量と平均音量の算出 ----- */

  // 最大音量と平均音量を計算
  for (int i = 0; i < RECORD_LENGTH; i++) {
    // 最大音量確認
    if (max < abs(rec_data[i])) {
      max = abs(rec_data[i]);
    }
    // 音量の合計を計算し平均値を求める
    sum += abs(rec_data[i]);
  }
  ave = sum / RECORD_LENGTH;  // 平均音量を計算

  //表示(現在の最大音量・平均音量・記録した履歴テーブルのインデックス)
  M5.Log.printf("MAX=%05d,AVE=%05d\nidx=%05d\n", max, ave, history_index);

  //最大音量と平均値を履歴データへ記録(但し検知中の場合は、0とする)
  history_max[history_index] = (detected ? 0 : max);
  history_ave[history_index] = (detected ? 0 : ave);
  history_index = (history_index + 1) % HISTORY_LENGTH;  // 履歴の循環
  
  //履歴データを基に直近の最大音量と平均音量を算出(但し検知中の場合は、0とする)
  VolumeMax = 0;
  VolumeAve = 0;
  sum = 0;
  if (!detected){
    for (int i = 0; i < HISTORY_LENGTH; i++) {
      max = history_max[i];
      sum += history_ave[i];
      VolumeMax = VolumeMax < abs(max) ? abs(max) : VolumeMax;
    }
    VolumeAve = sum / HISTORY_LENGTH;
  }

  /* ----- 判定 ----- */

  // 表示(判定に使用した最大音量と平均音量)
  M5.Log.printf("THMAX=%05d,THAVE=%05d", VolumeMax, VolumeAve);

  // 描画終わり
  M5.Display.endWrite();

  // 検知の終了判定 (終了時、LEDライトOffと再検知有効化)
  // 条件: 検知してからALERT_RESEND_INTERVAL時間経過した場合
  if (ALERT_RESEND_INTERVAL < (currentMillis - lastDetectedMillis)) {
    //規定時間経過後のため、カウントリセット
    M5.Power.setLed(0);  // 0:OFF 1-255:明るさ指定
    detected = false;
  }

  // 着信またはインターホン押下の判定 (検知時、LEDライトOnとLINEメッセージを通知)
  // 条件:
  //    1. 検知からTHRESHOLD_DETECT_INTERVALミリ秒経過
  //    2. "最大音量と平均音量のしきい値以上" Or "ボタンAを押下(デバッグ用)"
  if (THRESHOLD_DETECT_INTERVAL < (currentMillis - lastDetectedMillis) && ((THRESHOLD_MAX <= VolumeMax && THRESHOLD_AVE <= VolumeAve)  || M5.BtnA.wasClicked())) {
    M5.Log.printf("\nNotify!!!");
    M5.Power.setLed(200);  // 0:OFF 1-255:明るさ指定
    sendMessage(String(ALERT_MESSAGE) + "(max:" + String(VolumeMax) + "/ave:" + String(VolumeAve) + ")");
    lastDetectedMillis = currentMillis;
    detected = true;
  }
}

/** サブ関数 ====================================================================== **/

void sendMessage(const String& content) {
  if (!client.connect(LINE_MESSAGING_API_HOST, 443)) {
    Serial.println("Connection to LINE failed!");
    return;
  }

  // HTTPリクエストの作成
  String message = "{";
  // message += "\"to\":\"USER_ID\","; // 特定の宛先に通知する場合、LINEユーザーIDを指定
  message += "\"messages\":[{\"type\":\"text\",\"text\":\"" + content + "\"}]";
  message += "}";

  client.println("POST " + String(LINE_MESSAGING_API_ENDPOINT) + " HTTP/1.1");
  client.println("Host: " + String(LINE_MESSAGING_API_HOST));
  client.println("Authorization: Bearer " + String(LINE_MESSAGING_API_TOKEN));
  client.println("Content-Type: application/json");
  client.println("Content-Length: " + String(message.length()));
  client.println();
  client.println(message);

  // レスポンスの読み取り
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;  // HTTPヘッダーの終わり
  }
  // レスポンスボディを読み捨てる（SSL clientの内部状態を整合させる）
  while (client.available()) {
    client.read();
  }
  // SSL接続を閉じる（メモリリーク防止）
  client.stop();
  Serial.println("Message sent!");
}

String getQueryString(String queryStr, String key) {
  int start = queryStr.indexOf(key + "=");
  if (start == -1) {
    return "";
  }
  start += key.length() + 1;  // "="の次の文字からスタート
  int end = queryStr.indexOf("&", start);
  if (end == -1) {
    end = queryStr.length();
  }

  return queryStr.substring(start, end);
}

bool wifiSetup(bool retry) {
  int i = 0;

  //記憶している接続情報を試す(60*0.5sec=)
  if(retry){
    M5.Lcd.println();
    M5.Lcd.println("WfFi Connecting ");
    WiFi.begin();
    while (WiFi.status() != WL_CONNECTED && i < 60) {
      delay(500);
      M5.Lcd.print(".");
      Serial.print(".");
      i++;
    }
  }

  //接続成功
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected\r\nIP address: ");
    Serial.println(WiFi.localIP());
    WiFiConnected = true;
    return true;
  }

  /* ----- WiFi接続失敗 ----- */

  //WiFiパスワード生成
  randomSeed(analogRead(0));
  for (int i = 0; i < 8; i++) {
    WiFiAPPassword += random(10);
  }

  //WiFi アクセスポイントの作成
  M5.Lcd.setCursor(0, 0, 1);
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.println("Configuring access point...");
  WiFi.softAP(WiFiAPSsid.c_str(), WiFiAPPassword.c_str());
  IPAddress myIP = WiFi.softAPIP();
  Serial.println();
  Serial.print("Configuring access point...");
  Serial.print("AP IP address: ");
  Serial.println(myIP);
  Serial.println(WiFiAPSsid);
  Serial.println(WiFiAPPassword);

  //WebServerの起動
  server.begin();
  Serial.println("Web server started");
  delay(10000);
  M5.Lcd.println("SSID:" + WiFiAPSsid);
  M5.Lcd.println("PASS:" + WiFiAPPassword);
  M5.Lcd.println("http://192.168.4.1/");
  // QR Code
  //WIFI:S:<SSID>;T:<WPA|WEP|nopass>;P:<password>;;
  M5.Lcd.qrcode("WIFI:S:" + WiFiAPSsid + ";T:WPA;P:" + WiFiAPPassword + ";;", 2, 35, 90, 2);
  return false;
}

bool WiFiLoop() {
  int wifiStatus = WiFi.status();

  //接続済み
  if (WiFiConnected) {
    return true;
  }

  /* ----- WiFi未接続 ----- */

  //WifiSSID/Pass入手済みの場合
  if (wifiSsid.length() > 0 && wifiPassword.length() > 0) {
    if (wifiStatus != WL_CONNECTED) { //★つながってないことを確認しているのはなぜ？
      M5.Lcd.setCursor(0, 0, 1);
      M5.Lcd.fillScreen(BLACK);
      M5.Lcd.println("WiFi connecting");
      Serial.println("SSID: " + wifiSsid);
      Serial.println("Pass: " + wifiPassword);

      WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
      int i = 0;
      while (wifiStatus != WL_CONNECTED && i < 30) {
        delay(500);
        wifiStatus = WiFi.status();
        M5.Lcd.print(".");
        Serial.print(".");
        i++;
      }

      //接続成功
      if (WiFi.status() == WL_CONNECTED) {
        M5.Lcd.setCursor(0, 0, 1);
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.println("Connected!");
        delay(500);

        Serial.print("WiFi connected\r\nIP address: ");
        Serial.println(WiFi.localIP());
        delay(5000);
        M5.Lcd.println("System restart...");
        delay(5000);
        ESP.restart();
        delay(5000);

        //接続完了
        WiFiConnected = true;
        return true;
      }

      //接続失敗のため再起動
      M5.Lcd.println("");
      M5.Lcd.println("Connection fail!");
      M5.Lcd.println("System restart...");
      delay(5000);
      ESP.restart();
      delay(5000);
      
      wifiSsid = "";
      wifiPassword = "";
      WiFiConnected = false;
      wifiSetup(false);
      return false;
    }
  } else {
    //クライアントからの要求まち
    WiFiClient wifiClient = server.available();
    if (!wifiClient) {
      return false;
    }
    Serial.println("");
    Serial.println("New client");

    //クライアント接続中は維持
    while (wifiClient.connected()) {
      if (wifiClient.available()) { //受信まで待機
        String req = "";
        String res = "";
        String path = "";

        //リクエスト受信(1行分)
        req = wifiClient.readStringUntil('\r');
        Serial.println("Request(line): " + req);

        // クライアント側のIPアドレス
        IPAddress remoteIp = wifiClient.remoteIP();
        String remoteIpStr = String(remoteIp[0]) + '.' + String(remoteIp[1]) + '.' + String(remoteIp[2]) + '.' + String(remoteIp[3]);
        Serial.println("remoteIp: " + remoteIpStr);

        /*
        * HTTPヘッダ1行目で判断
        */

        if(req.startsWith("GET / HTTP/1.")){
          // 初回表示(/)の場合
          Serial.println("Response: initview");

          res = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"; //HTTP HEADER
          res += "<!DOCTYPE html>";
          res += "<html lang='ja'>";
          res += "<head>";
          res += "    <meta charset='UTF-8'>";
          res += "    <meta name='viewport' content='width=device-width, initial-scale=1.0'>";
          res += "    <title>M5Stick WiFiログインページ</title>";
          res += "    <style>";
          res += "        body {";
          res += "            font-family: Arial, sans-serif;";
          res += "            background-color: #f0f4f8;";
          res += "            margin: 0;";
          res += "            padding: 0;";
          res += "            display: flex;";
          res += "            justify-content: center;";
          res += "            align-items: center;";
          res += "            height: 100vh;";
          res += "        }";
          res += "        .container {";
          res += "            background-color: white;";
          res += "            padding: 20px;";
          res += "            border-radius: 8px;";
          res += "            box-shadow: 0 4px 10px rgba(0, 0, 0, 0.1);";
          res += "            width: 100%;";
          res += "            max-width: 400px;";
          res += "            box-sizing: border-box;";
          res += "        }";
          res += "        h1 {";
          res += "            text-align: center;";
          res += "            font-size: 24px;";
          res += "            margin-bottom: 20px;";
          res += "            color: #333;";
          res += "        }";
          res += "        .input-group {";
          res += "            margin-bottom: 15px;";
          res += "        }";
          res += "        .input-group label {";
          res += "            display: block;";
          res += "            font-size: 14px;";
          res += "            color: #666;";
          res += "            margin-bottom: 5px;";
          res += "        }";
          res += "        .input-group input {";
          res += "            width: 100%;";
          res += "            padding: 10px;";
          res += "            font-size: 16px;";
          res += "            border: 1px solid #ccc;";
          res += "            border-radius: 4px;";
          res += "            box-sizing: border-box;";
          res += "        }";
          res += "        .input-group input:focus {";
          res += "            border-color: #007bff;";
          res += "            outline: none;";
          res += "        }";
          res += "        .btn {";
          res += "            width: 100%;";
          res += "            padding: 10px;";
          res += "            background-color: #007bff;";
          res += "            color: white;";
          res += "            font-size: 16px;";
          res += "            border: none;";
          res += "            border-radius: 4px;";
          res += "            cursor: pointer;";
          res += "            transition: background-color 0.3s;";
          res += "        }";
          res += "        .btn:hover {";
          res += "            background-color: #0056b3;";
          res += "        }";
          res += "        .btn:active {";
          res += "            background-color: #003f7f;";
          res += "        }";
          res += "    </style>";
          res += "</head>";
          res += "<body>";
          res += "    <div class='container'>";
          res += "        <h1>M5Stick WiFi ログイン</h1>";
          res += "        <form action='/connect' method='POST'>";
          res += "            <div class='input-group'>";
          res += "                <label for='ssid'>SSID</label>";
          res += "                <input type='text' id='ssid' name='ssid' placeholder='SSIDを入力' required>";
          res += "            </div>";
          res += "            <div class='input-group'>";
          res += "                <label for='password'>パスワード</label>";
          res += "                <input type='password' id='password' name='password' placeholder='パスワードを入力' required>";
          res += "            </div>";
          res += "            <button type='submit' class='btn'>ログイン</button>";
          res += "        </form>";
          res += "    </div>";
          res += "</body>";
          res += "</html>";
        }else if(req.startsWith("POST /connect HTTP/1.")){
          // 接続要求の場合
          Serial.println("Response: connect");

          //データ部の受信
          char buf[257];
          int n;
          while(wifiClient.available()){
            //HTTPヘッダとデータ部の区切りまでシーク後受信
            req = wifiClient.readStringUntil('\n');
            if(req.length() == 1 && req[0] == '\r'){
              //データの受信開始
              req = "";
              while (n = wifiClient.available()) {
                if (n < 256){
                  wifiClient.readBytes(buf,n);
                  buf[n] = 0;
                } else {
                  wifiClient.readBytes(buf,256) ;
                  buf[256] = 0;
                }
                req += buf;
              }
            }
          }

          //入力値の取得
          Serial.println("POST Data: " + req);
          wifiSsid = getQueryString(req, "ssid");
          wifiPassword = getQueryString(req, "password");
          Serial.println("SSID: " + wifiSsid);
          Serial.println("Pass: " + wifiPassword);

          res = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"; //HTTP HEADER

          res += "<!DOCTYPE html>";
          res += "<html lang='ja'>";
          res += "<head>";
          res += "    <meta charset='UTF-8'>";
          res += "    <meta name='viewport' content='width=device-width, initial-scale=1.0'>";
          res += "    <title>M5Stick WiFiログインページ</title>";
          res += "    <style>";
          res += "        body {";
          res += "            font-family: Arial, sans-serif;";
          res += "            background-color: #f0f4f8;";
          res += "            margin: 0;";
          res += "            padding: 0;";
          res += "            display: flex;";
          res += "            justify-content: center;";
          res += "            align-items: center;";
          res += "            height: 100vh;";
          res += "        }";
          res += "        .container {";
          res += "            background-color: white;";
          res += "            padding: 20px;";
          res += "            border-radius: 8px;";
          res += "            box-shadow: 0 4px 10px rgba(0, 0, 0, 0.1);";
          res += "            width: 100%;";
          res += "            max-width: 400px;";
          res += "            box-sizing: border-box;";
          res += "        }";
          res += "        h1 {";
          res += "            text-align: center;";
          res += "            font-size: 24px;";
          res += "            margin-bottom: 20px;";
          res += "            color: #333;";
          res += "        }";
          res += "        .input-group {";
          res += "            margin-bottom: 15px;";
          res += "        }";
          res += "        .input-group label {";
          res += "            display: block;";
          res += "            font-size: 14px;";
          res += "            color: #666;";
          res += "            margin-bottom: 5px;";
          res += "        }";
          res += "        .input-group input {";
          res += "            width: 100%;";
          res += "            padding: 10px;";
          res += "            font-size: 16px;";
          res += "            border: 1px solid #ccc;";
          res += "            border-radius: 4px;";
          res += "            box-sizing: border-box;";
          res += "        }";
          res += "        .input-group input:focus {";
          res += "            border-color: #007bff;";
          res += "            outline: none;";
          res += "        }";
          res += "        .btn {";
          res += "            width: 100%;";
          res += "            padding: 10px;";
          res += "            background-color: #007bff;";
          res += "            color: white;";
          res += "            font-size: 16px;";
          res += "            border: none;";
          res += "            border-radius: 4px;";
          res += "            cursor: pointer;";
          res += "            transition: background-color 0.3s;";
          res += "        }";
          res += "        .btn:hover {";
          res += "            background-color: #0056b3;";
          res += "        }";
          res += "        .btn:active {";
          res += "            background-color: #003f7f;";
          res += "        }";
          res += "    </style>";
          res += "</head>";
          res += "<body>";
          res += "    <div class='container'>";
          res += "        <h1>M5Stick WiFi ログイン</h1>";
          res += "        <h4>WiFi接続中...<br>(SSID: " + wifiSsid + ")</h4>";
          res += "        <h5>connectedと表示されるまでお待ちください</h5>";
          res += "    </div>";
          res += "</body>";
          res += "</html>";
        }else{
          //上記以外の場合、エラーとする
          Serial.println("Response: 302 Found");
          res = "HTTP/1.1 302 Found\r\nLocation: http://192.168.4.1/\r\n\r\n";
        }
        //レスポンスの送信
        wifiClient.print(res);
        wifiClient.flush();
        wifiClient.stop();
      }
    }
    Serial.println("Done with client");
  }

  delay(1000);
  return false;
}