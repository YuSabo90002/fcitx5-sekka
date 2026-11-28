# fcitx5-sekka

An input method addon for using the [Sekka](https://github.com/kiyoka/sekka) input style in
[fcitx5](https://github.com/fcitx/fcitx5).

The conversion engine is [libsekka](https://github.com/YuSabo90002/libsekka).

[日本語版 / Japanese version](README.md)

## Usage

There is no mode-switching key. The **position of the uppercase letters** in the romaji
input decides what kind of conversion happens.

| Keystrokes | Result |
|---|---|
| `kanji` + Ctrl-J | かんじ (stays hiragana) |
| `Kanji` + Ctrl-J | 漢字 |
| `kanJi` + Ctrl-J | 感じ (with okurigana) |
| `OkonaU` + Ctrl-J | 行う |
| `KAnji` + Ctrl-J | 漢字 (rescues a Shift held too long; same as `Kanji`) |

When the input starts with an uppercase letter and the part before the second uppercase letter
(the stem) has not produced a single complete kana yet (consonants only; a lone `n` also counts
as a pending consonant), that second uppercase letter is treated as lowercase. This is the same
rule as SKK (`SHIneru` is the same as `Shineru`, `NAka` as `Naka`, `KAnJi` as `KanJi`). If the
stem already has a kana, the letter starts okurigana as before (`AU` → 合う), and an input that
starts in lowercase such as `kAnji` is unchanged. The typed letters are not rewritten, so
Ctrl-L during reselection or committing the romaji as it is gives `KAnji`. There is no setting
for it.

Ctrl-J means three different things depending on the state:

- with a romaji buffer: convert and place the first candidate in the preedit (not committed yet)
- with the first candidate placed: start reselection (this is when the candidate window appears)
- during reselection: advance to the next candidate

The key assignments during reselection follow upstream: Ctrl-A kanji / Ctrl-U hiragana /
Ctrl-I and Ctrl-K katakana / Ctrl-L half-width alphabet / Ctrl-E full-width alphabet, Enter
to confirm and Esc to cancel. A candidate can also be committed by clicking it. Numeric
selection by digit keys does not exist upstream and is not implemented (so no numeric labels
are shown on the candidates either).

The commit is deferred until the next key that is not the trigger. That is the design that
makes "reselect right after committing" work without having to take back text already sent to
the application.

As of 0.5.0, `wi` / `we` produce うぃ / うぇ (they produced ゐ / ゑ up to 0.4.0). Type ゐ / ゑ
with `yi` (or `wyi`) / `wye`. Words learned or registered up to 0.4.0 under a reading that
contains ゐ / ゑ show up when you type that reading this way, not with `wi` / `we`.

Typing `/` in the middle of romaji commits the romaji typed so far as it is, without
converting it, and leaves only the `/` in the input. If you then convert the romaji typed
after it with Ctrl-J, the `/` disappears and only the part after it is converted
(`Linux/Nihongo` + Ctrl-J → Linux日本語). If you commit with any key other than Ctrl-J, the `/`
comes out as a character (`and/or` + space → and/or). A `/` typed with no romaji pending comes
out as it always did. It exists to separate Latin text from Japanese, and there is no setting
for it.

Prefixes and suffixes are typed with `>`, as in SKK. Typing `chou>` and pressing Ctrl-J shows
the candidates for the prefix reading ちょう> (the first candidate is 超). Typing the romaji of
the next word commits 超, and the following word is converted with another Ctrl-J (the prefix
and the word after it are two separate conversions). For a suffix, type `>` while a converted
word is displayed: after `Nihon` Ctrl-J shows 日本, typing `>teki` commits 日本 at the `>`, and
Ctrl-J then shows the candidates for the suffix reading >てき (the first candidate is 的), giving
日本的. Pressing Ctrl-J again cycles through the prefix or suffix candidates. Without Ctrl-J, a
`>` is committed as a character as it always was (`a>b` + space → a>b). The full-width ＞ is not
treated as the marker. Suffixes with okurigana and registering prefixes or suffixes with Ctrl-R
are not supported.

For 30 seconds after Ctrl-J, each time you type romaji, the first candidate that Ctrl-J would
give right now is shown on the top line of the input panel in the form `[日本語]` (`Nihongo` →
[日本語], `nihongo` → [にほんご]). It is not shown while the first candidate is displayed or
during reselection. The 30 seconds start over each time you press Ctrl-J. The guide disappears
when the 30 seconds run out, when you press Ctrl-G, or when the focus moves away, and does not
appear again until the next Ctrl-J. Ctrl-G's other effects (committing the romaji as it is,
cancelling a reselection) are unchanged. There is no setting for it.

Pressing Ctrl-R during a conversion (while the first candidate is displayed or during
reselection) starts word registration. The reading is the romaji you typed, turned into kana,
and is shown at the application's input position. The word to register is built with Sekka's
own conversion (romaji input, Ctrl-J, reselection) and is shown on the top line of the input
panel. Pressing Ctrl-R again while building it registers that part as well (there is no limit
on the depth). Enter stores the (reading, word) pair in the user dictionary and commits the
word to the application; the registered word becomes the first candidate the next time you
convert that reading. Cancelling with Esc (or Ctrl-G) returns to the state right before
Ctrl-R, and moving the focus away ends registration without committing anything. Ctrl-R does
nothing for a conversion with okurigana.

Inside the word being registered, ← and → move the cursor one character at a time, and Home and
End move it to the start and the end (the same keys on the keypad work too). Typed kana and
conversion results are inserted at the cursor; BackSpace deletes the character before the
cursor and Delete the character after it. While romaji is half-typed, or while a conversion is
displayed or being reselected, these cursor keys first commit at the cursor what typing the
next character would commit (during reselection, the selected candidate), and then move
(Delete deletes the character after the cursor). For example, pressing ← right after
converting 語 commits 語 and moves the cursor before it, so converting 日本 next gives 日本語.
Outside registration these keys go to the application as before.

While Sekka is active, a printable key typed with Alt held down is committed as the character
without Alt (it does not reach the editor's key bindings). If a preedit, a first candidate or
a reselection is still pending, the character is appended to it and committed together. Keys
that do not produce a character, such as Enter, Esc, BackSpace, Tab and the arrows, and Alt
combined with Ctrl or Super are forwarded to the application as before.

## Dependencies

- fcitx5 5.1.13 or later (the dictionaries are located through the new `StandardPaths` API; `FCITX_ADDON_FACTORY_V2` also needs 5.1.12. The 5.1.2 of Ubuntu 24.04 is too old)
- libsekka (the `sekka` pkg-config module)
- CMake 3.13 or later and a C++20 compiler
- gettext
- GoogleTest (only with `-DENABLE_TEST=ON`)
- Notifications (optional): fcitx5's notifications addon, used to report a dictionary load
  failure

## Building

Install libsekka first, from the root of the libsekka repository (checkout):

```sh
cargo cinstall --release --prefix=/usr --libdir=lib
```

Then build the addon, from the root of the fcitx5-sekka repository (checkout):

```sh
cmake -S . -B build -DENABLE_TEST=ON
cmake --build build
(cd build && ctest)
sudo cmake --install build
```

## The master dictionary

No dictionary is generated by default. Only with an explicit `-DSEKKA_BUILD_DICT=ON` does the
build fetch SKK-JISYO.L (pinned to a fixed commit and verified by SHA256), generate
`master-dict.db` and install it into `${FCITX_INSTALL_PKGDATADIR}/sekka/` with mode 0444.

```sh
cmake -S . -B build -DSEKKA_BUILD_DICT=ON -DSEKKA_DICT_TOOL=/path/to/sekka-dict-tool
cmake --build build
```

In an offline environment, pass `-DSEKKA_SKKJISYO_SOURCE=<path to a fetched SKK-JISYO.L>`.

The output has to be mode 0444. The master dictionary is mmapped, so the addon refuses to
load it from a path writable by the running user (to avoid SIGBUS from a rewrite while
mapped).

## Dictionary search

When the "Dictionary path" setting is empty (the default), Sekka looks for
`sekka/master-dict.db` in the fcitx5 data directories in the following order and uses the
first one it can load.

1. `$XDG_DATA_HOME/fcitx5/` (`~/.local/share/fcitx5/` when `XDG_DATA_HOME` is not set)
2. `fcitx5/` under each directory listed in `$XDG_DATA_DIRS`, in order
3. fcitx5's own installed data directory (for example `/usr/share/fcitx5/`)

- To use your own dictionary, place it at `$XDG_DATA_HOME/fcitx5/sekka/master-dict.db`; it
  takes priority over the system one.
- The dictionary file must not be writable by the running user (mode 0444, as
  `sekka-dict-tool` produces it). A candidate that is writable, corrupt or unreadable is
  reported in a notification and skipped, and the next candidate is tried (see "The master
  dictionary" for why a writable dictionary is refused).
- When no dictionary is found anywhere, a notification lists the searched locations (the
  first three and the number of the rest). Input keeps working without a dictionary; text is
  committed as hiragana.
- A path entered in "Dictionary path" is used as it is, without searching. If that path
  cannot be loaded, Sekka does not fall back to a dictionary the search would find (so that a
  typo does not go unnoticed).
- If the 0.1.0 default `/usr/share/fcitx5/sekka/master-dict.db` is still in the configuration,
  it is treated the same as empty and the search is used. The configuration file is not
  rewritten.
- On systems without `/usr/share`, such as NixOS, no configuration is needed as long as the
  `share` directory of the prefix where fcitx5-sekka is installed is listed in
  `XDG_DATA_DIRS`.

## Configuration

Editable from fcitx5's configuration tool (`fcitx5-configtool`).

| Item | Default |
|---|---|
| Dictionary path | empty (searched automatically; see "Dictionary search") |
| User dictionary path | empty (`$XDG_DATA_HOME/fcitx5/sekka/user-dict.db`, or `~/.local/share/fcitx5/sekka/user-dict.db` when `XDG_DATA_HOME` is not set) |
| Conversion key | `Control+j` |

A change to the dictionary path takes effect without restarting fcitx5. Clearing it switches
back to the search.

## License

GPL-3.0-or-later; see [LICENSE](LICENSE).
