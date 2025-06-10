# m5stack-call-notifier

M5StickC Plus2 を使い、固定電話やインターホンの着信を LINE に通知する IoT デバイスです。

## 概要

- M5StickC Plus2 の内蔵マイクで音量を監視し、しきい値を超えた場合に LINE へ通知します。
- WiFi 未設定時は AP モードで Web UI から WiFi 設定が可能です。
- 通知には LINE Messaging API を利用します。

## 機能

- 音量波形のリアルタイム表示
- 最大音量・平均音量の履歴管理
- 着信検知時の LED 点灯と LINE 通知
- WiFi 設定 Web ページ自動生成（AP モード）

## 必要なもの

- [M5StickC Plus2](https://ssci.to/9350)
- Arduino IDE または PlatformIO
- LINE Messaging API チャネルアクセストークン

## セットアップ

1. このリポジトリをクローン
2. `arduino_secrets.h.example` を `arduino_secrets.h` にリネームし、LINE アクセストークンを記入
3. 必要なライブラリをインストール
   - M5StickCPlus2
   - WiFi
   - SSLClient など
4. M5StickC Plus2 に書き込み

## WiFi 設定

WiFi 未設定時は自動で AP モードになり、画面の QR コードまたは `http://192.168.4.1/` から Web UI で設定できます。

## LINE 通知設定

`arduino_secrets.h` に以下のように記載してください。

```c
#ifndef ARDUINO_SECRETS_H
#define ARDUINO_SECRETS_H

// LINE Messaging API  チャンネルアクセストークン
#define LINE_MES_API_CHANNEL_ACCESS_TOKEN "secret"

#endif
```

## trush_anchors.h の生成

- 下記サイトで生成する
  - https://openslab-osu.github.io/bearssl-certificate-utility/
