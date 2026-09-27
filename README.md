# fcitx5-sekka

[fcitx5](https://github.com/fcitx/fcitx5) で [Sekka](https://github.com/kiyoka/sekka)
（石火）の入力方式を使うためのインプットメソッドアドオン。

変換エンジンは [libsekka](https://github.com/YuSabo90002/libsekka) を用いる。

[English version](README.en.md)

## 操作

モード切替のキーは無い。ローマ字入力の**大文字の位置**が変換の種類を決める。

| 打鍵 | 結果 |
|---|---|
| `kanji` + Ctrl-J | かんじ（ひらがなのまま） |
| `Kanji` + Ctrl-J | 漢字 |
| `kanJi` + Ctrl-J | 感じ（送り仮名付き） |
| `OkonaU` + Ctrl-J | 行う |

Ctrl-J は状態によって3つの意味を持つ。

- ローマ字バッファがあるとき: 変換して第1候補をプリエディットに置く（まだ確定しない）
- 第1候補が置かれている状態: 選び直しを開始する（ここで初めて候補ウィンドウが出る）
- 選び直し中: 次候補へ進む

選び直し中のキー割り当ては本家に準拠する。Ctrl-A 漢字 / Ctrl-U ひらがな / Ctrl-I・Ctrl-K
カタカナ / Ctrl-L 半角英字 / Ctrl-E 全角英字、Enter 確定、Esc 取消。候補のクリックでも
確定できる。数字キーによる番号選択は本家に無いため実装していない（候補に番号ラベルも
出さない）。

確定はトリガー以外の次のキーまで遅延する。これは「確定直後の選び直し」を、アプリへ
送った文字列を取り返さずに成立させるための設計である。

## 依存

- fcitx5 5.1.13 以降（辞書を新しい `StandardPaths` API で探すため。`FCITX_ADDON_FACTORY_V2` にも 5.1.12 以降が要る。Ubuntu 24.04 の 5.1.2 では足りない）
- libsekka（pkg-config の `sekka`）
- CMake 3.13 以降、C++20 コンパイラ
- gettext
- GoogleTest（`-DENABLE_TEST=ON` のときのみ）
- 通知（任意）: fcitx5 の notifications アドオン。辞書の読み込みに失敗したときの通知に使う

## ビルド

libsekka を先にインストールする。

```sh
cd libsekka
cargo cinstall --release --prefix=/usr --libdir=lib
```

続いてアドオンをビルドする。

```sh
cmake -S . -B build -DENABLE_TEST=ON
cmake --build build
cd build && ctest
sudo cmake --install build
```

## マスター辞書

既定では辞書を生成しない。`-DSEKKA_BUILD_DICT=ON` を明示したときだけ、SKK-JISYO.L
（固定コミット＋SHA256 で検証）を取得して `master-dict.db` を生成し、
`${FCITX_INSTALL_PKGDATADIR}/sekka/` へ 0444 でインストールする。

```sh
cmake -S . -B build -DSEKKA_BUILD_DICT=ON -DSEKKA_DICT_TOOL=/path/to/sekka-dict-tool
cmake --build build
```

オフライン環境では `-DSEKKA_SKKJISYO_SOURCE=<取得済みSKK-JISYO.Lのパス>` を指定する。

生成物は 0444 である必要がある。マスター辞書は mmap されるため、実行ユーザーから
書き込み可能なパスに置かれているとアドオンがロードを拒否する（mmap 中の書き換えによる
SIGBUS を避けるため）。

## 辞書の探索

設定の「辞書パス」が空のとき（既定）、Sekka は `sekka/master-dict.db` を fcitx5 のデータ
ディレクトリから次の順に探し、最初に読み込めた1つを使う。

1. `$XDG_DATA_HOME/fcitx5/`（未設定なら `~/.local/share/fcitx5/`）
2. `$XDG_DATA_DIRS` に並ぶ各ディレクトリの `fcitx5/`（先頭から順に）
3. fcitx5 自身のインストール先のデータディレクトリ（例: `/usr/share/fcitx5/`）

- 自分で用意した辞書を使うときは `$XDG_DATA_HOME/fcitx5/sekka/master-dict.db` に置く。
  システムの辞書より優先される。
- 辞書ファイルは実行ユーザーから書き込めない状態（`sekka-dict-tool` の出力どおり 0444）で
  置く。書き込み可能・壊れている・読めない辞書は、理由を通知したうえで飛ばし、次の候補を
  試す（書き込み可能な辞書を拒否する理由は「マスター辞書」の節の mmap と SIGBUS の説明を
  参照）。
- どこにも見つからないときは、探した場所を並べた通知が出る（多いときは先頭3件と残りの
  件数）。辞書が無くても入力は止まらず、ひらがなのまま確定できる。
- 「辞書パス」にパスを書くと、探索せずにそのパスだけを使う。そのパスが読めなくても、
  探索で見つかる別の辞書へはフォールバックしない（打ち間違いに気づけるようにするため）。
- v1.0 の既定値 `/usr/share/fcitx5/sekka/master-dict.db` が設定に残っている場合は、
  空と同じに扱って探索する。設定ファイルは書き換えない。
- NixOS など `/usr/share` を持たない環境でも、fcitx5-sekka を入れた prefix の `share` が
  `XDG_DATA_DIRS` に載っていれば設定は要らない。

## 設定

fcitx5 の設定ツール（`fcitx5-configtool`）から変更できる。

| 項目 | 既定値 |
|---|---|
| 辞書パス | 空（自動で探す。「辞書の探索」を参照） |
| ユーザー辞書パス | 空（`$XDG_DATA_HOME/fcitx5/sekka/user-dict.db`、未設定なら `~/.local/share/fcitx5/sekka/user-dict.db`） |
| 変換キー | `Control+j` |

辞書パスの変更は fcitx5 を再起動せずに反映される。空に戻すと探索に切り替わる。

## ライセンス

GPL-3.0-or-later。詳細は [LICENSE](LICENSE)。
