# amd-vram-replay

[English](README.md) | 日本語

Windows と AMD Radeon 用の小さなインスタントリプレイ録画ツールです。リプレイの録画データを **VRAM（グラフィックボードのメモリ）** に置いておきます。

画面は AMD のドライバーそのもの（AMF DisplayCapture）から取り込み、使えないときは DXGI Desktop Duplication に切り替えます。取り込んだ画面は AMD の AMF エンコーダーで GPU 上で圧縮し（FFmpeg 経由）、圧縮したデータはグラフィックボード上の大きな `ID3D11Buffer` のリングに入れます。圧縮前の画面がメインメモリを通ることはなく、通るのは小さな圧縮済みデータだけです。ホットキーを押すと直近 *N* 秒を読み戻し、再圧縮せずに MP4 に書き出します。

考え方は ShadowPlay や Adrenalin のインスタントリプレイと同じで、メーカーのアプリの代わりに FFmpeg のライブラリで作っています。

画面の表示は日本語です。

## できること

- **AMD ドライバーで取り込み**: ディスプレイドライバーから画面を直接エンコーダーに渡します（FFmpeg の `vsrc_amf` 経由の AMF DisplayCapture。Adrenalin と同じ経路）。使えないときは Desktop Duplication を使います（`--capture=dda` で強制できます）。
- **インスタントリプレイ**: 直近 5〜180 秒（初期値 30 秒）を残しておき、`Ctrl+Alt+Shift+F10` で保存します。
  - ファイル名は手前にあるゲームの名前になります。例: `Overwatch_2026-11-25_07-46-47.mp4`
- **手動録画**: `Ctrl+Alt+Shift+F11` で録画の開始と停止。録りながらディスクに書き込みます。
  - フラグメント MP4 なので、途中で落ちても再生できるファイルが残ります。
- **スクリーンショット**: `Ctrl+Alt+Shift+F1` で今の画面を PNG で保存します。
- **画面の通知**: リプレイの保存、録画の開始・停止、スクリーンショットのときに、右上に小さなパネルが出ます。
  - パネルは録画に映りません。
  - ボーダーレスのゲームの上には出ますが、排他フルスクリーンの上には出ません。
- **音声**:
  - デスクトップの音（WASAPI ループバック）とマイクを混ぜて AAC にします。
  - 既定のデバイスが変わると追いかけます。マイクは自分で選ぶこともできます。
  - マイクは最大 +30 dB までブーストでき、ソフトリミッターつきです。
- **クリップエディター（`VramEdit`）**: Edge のアプリウィンドウで開く、手元だけで動く小さな Web 画面です。
  - ゲームごとに絞り込めるギャラリー、再生、フィルムストリップのタイムラインでのトリム・分割・カット。
  - 劣化なし（ストリームコピー）か、フレーム単位で正確な GPU 書き出し。
  - 設定画面にはライブのミキサー（縦のピークメーターと、すぐ反映されるフェーダー）があります。
- **ゲームの邪魔をしない**:
  - 取り込みとエンコードは GPU の優先度を「通常より低い」で動かします。
  - タスクトレイに常駐し、ログオン時に起動できます（タスクスケジューラ）。VRAM のリングは長さと画質に応じて約 60 MB〜1.9 GB です（初期値の 30 秒・25 Mbps で 336 MB）。

## 動作環境

- Windows 10 2004 以降（Windows 11 推奨）。
- AMF（`h264_amf`）が使える AMD の GPU。`--codec=` で他のエンコーダーも選べますが、試していません。
- ビルドに必要なもの:
  - [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw)（UCRT）。
  - ヘッダーとインポートライブラリつきの FFmpeg 共有ビルド（FFmpeg 9.1、Gyan.dev の "full shared" で確かめています）。
  - どちらも winget で入れられます:

    ```
    winget install MartinStorsjo.LLVM-MinGW.UCRT Gyan.FFmpeg.Shared
    ```

## ビルド

```sh
sh build.sh
```

`bin/VramReplay.exe`（録画ツール）と `bin/VramEdit.exe`（エディター）ができ、`editor.html` と FFmpeg の DLL がその横にコピーされます。`build.sh` は winget のインストール先を自動で探します。Windows は動いている exe を書き換えられないので、ビルドの前に両方を終了してください。

## 使い方

`bin/VramReplay.exe` を起動します。タスクトレイに常駐するので、アイコンを右クリックするとメニューが出ます。

| 操作 | ホットキー |
|---|---|
| リプレイを保存 | `Ctrl+Alt+Shift+F10` |
| 録画の開始・停止 | `Ctrl+Alt+Shift+F11` |
| スクリーンショット | `Ctrl+Alt+Shift+F1` |

設定は `%LOCALAPPDATA%\VramReplay\config.ini` に保存され、エディターの設定画面から変えるのが簡単です。ログも同じフォルダーにあります。クリップは初期設定では `ビデオ\VramReplay` に保存されます。

### 操作用のパイプ

ほかの道具（Stream Deck やスクリプトなど）から、名前付きパイプ `\\.\pipe\VramReplay` で操作できます。1 回の接続で 1 つのコマンドを送り、JSON で返事が来ます。

| コマンド | 動き |
|---|---|
| `save [秒数]` | リプレイを保存（秒数を書くと直近 *N* 秒だけ） |
| `record start` / `record stop` / `record` | 手動録画の操作。`record` は開始と停止の切り替え |
| `screenshot` | 今の画面を PNG で保存 |
| `status` | 録画ツールの状態、リングの使用量、録画時間 |
| `level` | 音声のピークレベル |
| `gain <デスクトップ %> <マイク %>` | 音量をその場で変える |
| `restart` | 再起動して `config.ini` を読み直す |
| `quit` | 終了 |

PowerShell の例:

```powershell
$c = New-Object IO.Pipes.NamedPipeClientStream('.', 'VramReplay', 'InOut'); $c.Connect(2000)
$w = New-Object IO.StreamWriter($c); $w.AutoFlush = $true; $w.Write('save')
(New-Object IO.StreamReader($c)).ReadLine(); $c.Dispose()
```

## しくみ

```
AMD ドライバーで取り込み (vsrc_amf) ─────────► h264_amf (GPU で色変換とエンコード)
  └ 予備: Desktop Duplication ─► D3D11 テクスチャ ─┘
                                              │ 圧縮済みデータ
WASAPI ループバック + マイク ─► 変換・ミックス ─► AAC ─┤
                                              ▼
                              VRAM 上のリングバッファー (ID3D11Buffer)
                                              │  ホットキー
                                              ▼
                       ステージングで読み戻し ─► MP4 に書き出し (ストリームコピー)
```

- `src/video.cpp`: 取り込みのループ、フレームの間隔、AMF エンコーダー。
  - AMF はテクスチャ配列のスライスを正しくエンコードできないため、フレームごとに別のテクスチャを使います。
  - ロック画面や UAC から戻ったときはリプレイを消します。
- `src/ring.cpp`: VRAM のリング。データは途中で折り返さず、小さな索引だけをメインメモリに置きます。
- `src/saver.cpp`、`src/mux.cpp`: 読み戻しと MP4 への書き出し。
- `src/recorder.cpp`: 手動録画。書き込み用のスレッドが、キューに入ったデータのコピーをディスクに書きます。
- `src/audio.cpp`: WASAPI の取り込み、ミックス、リミッター、AAC。
- `src/overlay.cpp`: 通知パネル。クリックを素通りさせ、録画に映らないレイヤードウィンドウです。
- `src/tray.cpp`: トレイアイコン、ホットキー、クリップ名に使う手前のアプリの追跡、自動起動。
- `editor/`: エディターの HTTP サーバー（127.0.0.1 だけ）と、1 ページの画面。

## 制限

- 取り込めるモニターは 1 台だけです（GPU の最初の出力。`--monitor=N` で変えられます）。
- 排他フルスクリーンのゲームの上には通知を出せません。メーカーのオーバーレイはゲームの描画をフックして出していますが、アンチチートに引っかかるおそれがあるため、このプロジェクトではあえてしていません。
- 画面の文字は日本語だけです。

## ライセンス

このリポジトリのコードは MIT です（[LICENSE](LICENSE)）。FFmpeg は含まれていません。共有ライブラリとして読み込み、使うビルドによって LGPL か GPL になるので、バイナリを再配布するときは気をつけてください。
