# magolor-rt5682-windows

**日本語** | [English](README.en.md)

Windows で **Acer Chromebook R753T(ボード名 "Magolor"、Intel Jasper Lake)** のヘッドホン端子(ヘッドホン出力・ヘッドセットマイク)を使うための、Realtek RT5682 コーデック用の非公式ドライバ `jslcodec` です。

**単体では音は出ません。** DSP 側のドライバ [sskrc/magolor-audio-windows](https://github.com/sskrc/magolor-audio-windows)(`jslaudio`)と組み合わせて使います。`jslaudio` が DSP の SSP0 経由で音声データとクロックを送り、`jslcodec` が RT5682 を設定します。2つは別々のデバイスに入る独立したドライバで、ソースコードは共有していません。

## AI による作成について

このリポジトリのドライバ、スクリプト、ドキュメントは、**AI(Anthropic の Claude。Claude Code を使用)が作成**しました。作者は、方針の指示、実機でのインストール、動作確認を担当しています。
コードは実機で動作を確認していますが、人間による網羅的なコードレビューは行っていません。カーネルドライバであることを踏まえて、利用や改変の際は十分に注意してください。

## 状態

| 機能 | 状態 |
|---|---|
| ヘッドホン出力(Windows の「Headphones」) | ✅ |
| ヘッドセットマイク(4極、Windows の「Headset Microphone」) | ✅ |
| ジャックの抜き差しの検出、ヘッドホン/ヘッドセットの判定 | ✅(0.5秒ごとのポーリング) |
| 差し込み時の出力先の自動切り替え(Windows へのジャック状態の通知) | ❌ 未対応。手動で切り替えてください |
| ヘッドセットのボタン | ❌ 未対応 |

動作確認機: Acer Chromebook R753T(Magolor、Celeron N4500)、MrChromebox UEFI ファームウェア、Windows 11 Pro(build 26200)。

## ⚠ 注意

- **非公式・実験的なカーネルドライバ**です。不具合があるとブルースクリーンやデータ損失の可能性があります。自己責任で使ってください。
- 自己署名(テスト署名)のため、Windows の **テスト署名モード** が必要です(手順は magolor-audio-windows の README を参照)。
- 対象は `ACPI\10EC5682` です。クロック(MCLK 24 MHz)やジャック検出の設定は Magolor 向けなので、**他機種では動かないか、誤動作する可能性があります**。
- Realtek、Intel、Google、Acer とは無関係です。

## 使い方

ビルド環境(LLVM、WDK / SDK のヘッダとライブラリ)は magolor-audio-windows と同じです。テスト署名の準備も同じ証明書を使います。

1. 先に magolor-audio-windows の `jslaudio` をインストールします。
2. **ビルド**(`pkg\` にドライバパッケージができます)
   ```
   powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -LlvmBin <LLVMのbin> -WdkRoot <WDKのc> -SdkRoot <SDKのc>
   ```
3. **インストール(管理者)**
   ```
   powershell -ExecutionPolicy Bypass -File scripts\install.ps1
   ```
   デバイスマネージャーの「システム デバイス」に **JSL RT5682 Headset Codec** が正常な状態で表示されれば成功です。
4. ヘッドホンを差し、Windows の出力先を **Headphones**、入力を **Headset Microphone** に切り替えます。

ログは `C:\Windows\Temp\jslcodec.log` に出ます(読むには管理者権限が必要です)。`install.ps1 -LogPath <ファイル>` を付けると、指定したファイルに出力します。

アンインストールは、`pnputil /enum-drivers` で元の名前が `jslcodec.inf` のパッケージ(`oemNN.inf`)を探し、管理者で `pnputil /delete-driver oemNN.inf /uninstall /force` を実行します。

## 仕組み

- I2C(I2C4、アドレス 0x1A)には、Windows の SPB フレームワーク経由でアクセスします。
- 起動時に、Linux の rt5682 ドライバと同じ手順で初期化します。デバイス ID の確認、ヘッドホンのキャリブレーション、Realtek 指定のレジスタパッチ、各種初期設定を行います。
- クロックは Linux の Intel 向けボードドライバ(sof_rt5682)と同じ設定です。PLL1 で MCLK 24 MHz から約 24.576 MHz のシステムクロックを作り、DAC / ADC のフィルタには I2S1 の ASRC を使います。I2S は 2スロット、24bit です。
- 0.5秒ごとにジャックの状態(JD1)を読みます。差し込まれるとコンボジャックの種類を判定し、ヘッドホンの経路に電源を入れます。ヘッドセットならマイクの経路にも電源を入れます。抜かれると電源を切ります。

## ライセンス

**GPL-2.0-only** です([LICENSE](LICENSE))。

レジスタの初期化手順、キャリブレーション、ジャック検出、電源投入の順序などは、Linux カーネルの次のドライバから移植しています。

- `sound/soc/codecs/rt5682.c`、`rt5682-i2c.c`、`rt5682.h`(Copyright 2018 Realtek Semiconductor Corp.、作者 Bard Liao、GPL-2.0-only)
- `sound/soc/intel/boards/sof_rt5682.c`(Intel 向けボードドライバ。クロック設定を参照)

このため、DSP 側の magolor-audio-windows(MIT)とは別のリポジトリにしています。
