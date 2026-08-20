# Windows key interceptions

Some key combinations never reach this app on Windows: the keystroke is consumed by the OS or the UI framework before WinUI raises `KeyDown`, so a ghostty `keybind` bound to it silently does nothing. This is not a ghostty bug — the same combos are dead in every application on the same machine. This page lists the known cases and how to diagnose a new one.

<details>
<summary>日本語</summary>

Windows では、一部のキーコンボがこのアプリに届く前に OS や UI フレームワークに消費される。WinUI の `KeyDown` 自体が発生しないので、そこに割り当てた ghostty の `keybind` は黙って無反応になる。ghostty のバグではない — 同じコンボは同じマシンの他のアプリでも死んでいる。このページは既知のケースと、新しいケースの診断手順をまとめたもの。

</details>

## TSF input-language hotkeys (issue #83)

**Symptom:** a binding like `keybind = ctrl+shift+zero=reset_window_size` never fires, while `ctrl+shift+one` works fine. Instrumenting `TerminalKeyDown` shows Ctrl and Shift arriving but no KeyDown for the digit at all.

**Cause:** Windows lets the user assign `Ctrl+Shift+<digit>` (and similar) sequences as *input language hot keys* — direct-switch shortcuts to a specific keyboard layout/IME. Assigned combos are consumed by the Text Services Framework before any application sees them. The assignments live per-user in the registry under `HKCU\Control Panel\Input Method\Hot Keys`; subkey IDs in the `0x00000100`–`0x0000011F` range are the direct-switch slots (`IME_HOTKEY_DSWITCH_FIRST`..`DSWITCH_LAST` in the Windows SDK's `imm.h`). On the machine where #83 was filed, subkey `00000104` held Virtual Key `0x30` ('0') with modifiers Ctrl+Shift — exactly the dead combo.

**Fix (per-user, no admin needed):** Settings → Time & Language → Typing → Advanced keyboard settings → **Input language hot keys** → select the "Switch to ..." entry holding the combo → **Change Key Sequence** → unassign (or move it). A sign-out may be needed before it takes effect.

**Verified end-to-end (2026-08-21):** with the assignment present, the digit's KeyDown never arrives (Notepad is equally dead); after unassigning, the registry entry disappears and the ghostty keybind fires. Which combos are affected varies per machine with this setting.

<details>
<summary>日本語</summary>

**症状:** `keybind = ctrl+shift+zero=reset_window_size` のような割り当てが一切発火しない。一方 `ctrl+shift+one` は普通に動く。`TerminalKeyDown` を計測すると Ctrl と Shift は届くのに、数字キーの KeyDown だけが存在しない。

**原因:** Windows には `Ctrl+Shift+<数字>` 等を「入力言語のホットキー」(特定のレイアウト / IME への直接切替) として割り当てる機能があり、割り当て済みのコンボは Text Services Framework がアプリより先に消費する。割り当ての実体はユーザー単位でレジストリ `HKCU\Control Panel\Input Method\Hot Keys` にあり、サブキー ID `0x00000100`〜`0x0000011F` が直接切替スロット (Windows SDK `imm.h` の `IME_HOTKEY_DSWITCH_FIRST`..`DSWITCH_LAST`)。#83 を起票したマシンではサブキー `00000104` が Virtual Key `0x30` ('0') + Ctrl+Shift を保持していた — 死んでいたコンボそのもの。

**対処 (ユーザー単位、管理者権限不要):** 設定 → 時刻と言語 → 入力 → キーボードの詳細設定 → **入力言語のホットキー** → コンボを保持している「〜に切り替え」エントリを選択 → **キー シーケンスの変更** → 割り当てなしにする (か別のキーへ)。反映にサインアウトが必要な場合がある。

**実証済み (2026-08-21):** 割り当てがある間は数字の KeyDown が届かず (メモ帳でも同様)、解除するとレジストリからエントリが消えて ghostty の keybind が発火する。どのコンボが影響を受けるかはこの設定次第でマシンごとに異なる。

</details>

## Other observed interceptions

Seen while wiring actions on the development machine (one data point each — listed so future keybind choices can route around them):

- `ctrl+shift+alt+f` — eaten by WinUI's Alt-menu accelerator handling; never reaches the terminal.
- `ctrl+shift+backslash` — never reaches `action_cb`; interception point not yet identified.

<details>
<summary>日本語</summary>

開発機で action の配線中に確認したもの (それぞれ 1 事例。今後の keybind 選びで回避できるよう記録):

- `ctrl+shift+alt+f` — WinUI の Alt メニューアクセラレータ処理に食われ、terminal に届かない。
- `ctrl+shift+backslash` — `action_cb` に届かない。横取り箇所は未特定。

</details>

## Diagnosing a dead keybind

1. **Try the combo in Notepad.** If it does nothing there either, the interception is system-level — ghostty cannot receive it, and the fix is on the Windows side.
2. **Dump the input-method hotkey table** and look for the combo's virtual key:

   ```powershell
   Get-ChildItem 'HKCU:\Control Panel\Input Method\Hot Keys' | ForEach-Object {
     $p = $_ | Get-ItemProperty
     '{0}  vk=0x{1:X2}  mod=0x{2:X2}' -f $_.PSChildName, $p.'Virtual Key'[0], $p.'Key Modifiers'[0]
   }
   ```

   Modifier bits: `0x01` Alt, `0x02` Ctrl, `0x04` Shift. An entry in the `000001xx` range matching your combo is the TSF case above.
3. **If Notepad receives it but ghostty doesn't**, the interception is inside the WinUI layer — instrument `TerminalKeyDown::toRawKeyPress` with `OutputDebugStringA` and add the combo to the list above once identified.

<details>
<summary>日本語</summary>

1. **そのコンボをメモ帳で試す。** メモ帳でも無反応なら横取りはシステムレベル — ghostty には受信自体が不可能で、直すのは Windows 側の設定。
2. **入力メソッドのホットキー表をダンプ**して、コンボの virtual key を探す (上の PowerShell)。modifier のビットは `0x01` Alt / `0x02` Ctrl / `0x04` Shift。`000001xx` 帯に一致エントリがあれば上記の TSF ケース。
3. **メモ帳には届くのに ghostty に届かない**なら横取りは WinUI 層 — `TerminalKeyDown::toRawKeyPress` に `OutputDebugStringA` を仕込んで計測し、特定できたら上のリストに追記する。

</details>
