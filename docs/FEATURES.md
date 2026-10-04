# What GhosttyWin32 can do

Everything below works today. What does not is in [Not yet](#not-yet) at the end, with the issue tracking it. Config keys are upstream ghostty's unless marked otherwise; the full list is in the [upstream reference](https://ghostty.org/docs/config).

<details>
<summary>日本語</summary>

ここに書いてあるものは今動く。動かないものは末尾の [Not yet](#not-yet) に、追っている issue と一緒に置いてある。config キーは印のあるものを除いて upstream ghostty のもので、全一覧は[ドキュメント](https://ghostty.org/docs/config)にある。

</details>

## Terminal

| Feature | Notes |
| --- | --- |
| Terminal emulation | libghostty's own: the VT parser, screen and scrollback are upstream's code, not a reimplementation |
| Any shell | `command` — pwsh, cmd.exe, or anything else. Without it, the Windows default shell |
| CJK and emoji | System font fallback for glyphs the chosen font lacks, and surrogate pairs for the supplementary planes |
| IME | Japanese, Chinese and Korean, with the composition shown in the pane as you type |
| Exit closes the pane | When the shell process ends, so does its pane — and its tab, if it was the last one |

<details>
<summary>日本語</summary>

| 機能 | 補足 |
| --- | --- |
| 端末エミュレーション | libghostty そのもの。VT パーサ、画面、スクロールバックは upstream のコードで、作り直してはいない |
| 任意のシェル | `command` で pwsh、cmd.exe、その他なんでも。指定しなければ Windows の既定シェル |
| CJK と絵文字 | 選んだフォントに無い字はシステムフォントで補い、補助面はサロゲートペアで扱う |
| IME | 日本語・中国語・韓国語。変換中の文字列はペインの中に表示される |
| 終了でペインが閉じる | シェルのプロセスが終わるとそのペインも閉じ、最後の 1 枚だったらタブも閉じる |

</details>

## Windows, tabs and splits

| Feature | Notes |
| --- | --- |
| Multiple windows | `Ctrl+Shift+N`. Each window has its own tab strip and pane tree |
| Tabs | WinUI 3 `TabView`: new, close, reorder, go-to, from both gestures and keybinds |
| Tear a tab out | Drag it off the strip to open it in a new window at the drop point |
| Merge a tab in | Drag it onto another window's strip. Surfaces, swap chains and the split tree stay live across the move |
| Splits | Horizontal and vertical, with focus navigation, resize, equalize and zoom |
| Per-pane isolation | Every pane is a full ghostty surface with its own D3D11 device, behind its own SEH guard, so a driver fault in one pane does not take the others down |
| Undo a close | `Ctrl+Shift+T` brings back the tab you just closed, with its scrollback |

<details>
<summary>日本語</summary>

| 機能 | 補足 |
| --- | --- |
| 複数ウインドウ | `Ctrl+Shift+N`。ウインドウごとにタブ列とペインの木を持つ |
| タブ | WinUI 3 の `TabView`。新規・閉じる・並べ替え・移動を、操作とキーバインドの両方から |
| タブを引き剥がす | タブ列の外にドラッグすると、落とした位置に新しいウインドウで開く |
| タブを取り込む | 別のウインドウのタブ列にドラッグする。surface・swap chain・split の木は生きたまま移る |
| split | 水平・垂直。焦点の移動、サイズ変更、均等化、ズーム |
| ペインごとの隔離 | どのペインも完全な ghostty surface で、自分の D3D11 device と SEH guard を持つ。1 つのペインでドライバが落ちても他は巻き込まれない |
| 閉じたタブを戻す | `Ctrl+Shift+T` で直前に閉じたタブがスクロールバックごと戻る |

</details>

## Input

| Feature | Notes |
| --- | --- |
| Keyboard | Scan code and text are forwarded separately, so keybinds hold across non-Latin layouts, dead keys and AltGr |
| Mouse | Left, middle and right click, drag, and the wheel |
| Selection | Drag to select, `Ctrl+C`, `Ctrl+V`, right-click to copy. The selection clears after a copy |
| Cursor shape | Follows the terminal: a text cursor over text, a hand over a link |
| Links | Ctrl+click opens the URL in the default browser |
| Hidden while typing | The pointer disappears while you type and comes back when you move it |

<details>
<summary>日本語</summary>

| 機能 | 補足 |
| --- | --- |
| キーボード | スキャンコードと文字を別々に渡すので、ラテン以外の配列、デッドキー、AltGr でもキーバインドが崩れない |
| マウス | 左・中・右クリック、ドラッグ、ホイール |
| 選択 | ドラッグで選択、`Ctrl+C`、`Ctrl+V`、右クリックでコピー。コピー後に選択は解除される |
| カーソルの形 | 端末に従う。文字の上では文字カーソル、リンクの上では手 |
| リンク | Ctrl+クリックで既定のブラウザが開く |
| 打鍵中は隠れる | 文字を打つあいだポインタが消え、動かすと戻る |

</details>

## Appearance

| Feature | Config and notes |
| --- | --- |
| Title bar | Custom, with the terminal's background colour carried into it on Windows 11 |
| Window decorations | `window-decoration`, plus a per-window override from the `toggle_window_decorations` keybind |
| Fullscreen and maximize | `toggle_fullscreen`, `toggle_maximize` |
| Backdrop | Mica and Acrylic, when running as an installed package |
| Background | `background-opacity`, `background-image` with `background-image-opacity` and `background-image-fit` |
| Themes | `theme`, from the config or from `%LOCALAPPDATA%\ghostty\themes\` |
| Colours | The 16 ANSI colours and the cursor colour, from the config |
| HiDPI | Per-monitor DPI aware, correct at first paint and across a move between monitors or into an RDP session |

<details>
<summary>日本語</summary>

| 機能 | config と補足 |
| --- | --- |
| タイトルバー | 独自のもの。Windows 11 では端末の背景色がタイトルバーにも反映される |
| ウインドウ装飾 | `window-decoration`。加えて `toggle_window_decorations` のキーバインドでウインドウごとに上書きできる |
| 全画面・最大化 | `toggle_fullscreen`、`toggle_maximize` |
| 背景の質感 | Mica と Acrylic。インストールしたパッケージとして動いているとき |
| 背景 | `background-opacity`、`background-image` と `background-image-opacity` / `background-image-fit` |
| テーマ | `theme`。config か `%LOCALAPPDATA%\ghostty\themes\` から |
| 色 | ANSI 16 色とカーソル色を config から |
| HiDPI | モニタごとの DPI に対応。最初の描画から正しく、モニタ間の移動や RDP に入っても崩れない |

</details>

## Rendering

| Feature | Notes |
| --- | --- |
| DirectX 11 | libghostty's own DirectX renderer, with no external dependency. The swap chain lives in a `SwapChainPanel` |
| Cadence | The focused pane polls every 4 ms; the rest are event-driven, woken by output, a blink, a mailbox message or a 1 s safety net |

<details>
<summary>日本語</summary>

| 機能 | 補足 |
| --- | --- |
| DirectX 11 | libghostty の DirectX レンダラをそのまま使う。外部依存はなし。swap chain は `SwapChainPanel` の中にある |
| 描画の間隔 | 焦点のあるペインは 4 ms ごとに見る。それ以外は出力・点滅・mailbox・1 秒の保険で起こされるイベント駆動 |

</details>

## WSL

A WSL pane can run on a real Linux pty instead of on Windows' pseudo console, which is what makes the bytes a program writes arrive as it wrote them. It is off by default and needs a binary installed inside the distribution. See [docs/WSL.md](WSL.md).

| Feature | Notes |
| --- | --- |
| `wsl-bridge` | Fork-only config key, `false` by default. With it on, a WSL tab gets a real Linux pty |
| `wsl` in a pane | Typing `wsl` at a pwsh or cmd prompt opens WSL in the same pane, and `exit` brings the shell back with WSL's exit code. Needs the `wsl` shim installed as well as the bridge's helper |
| Options carried | `--distribution` / `-d`, `--cd`, `--user` / `-u`, and a lone `~`. Any other line runs the real `wsl.exe` as typed |
| A failed session | The pane stays up with the reason on it, rather than vanishing back to the shell |

<details>
<summary>日本語</summary>

WSL のペインは、Windows の擬似コンソールではなく本物の Linux pty の上で動かせる。プログラムが書いたバイト列がそのまま届くのはそのため。既定では無効で、distro の中にバイナリを置く必要がある。[docs/WSL.md](WSL.md) を参照。

| 機能 | 補足 |
| --- | --- |
| `wsl-bridge` | fork だけの config キー。既定は `false`。有効にすると WSL のタブが本物の Linux pty を得る |
| ペインの中の `wsl` | pwsh や cmd のプロンプトで `wsl` と打つと同じペインで WSL が開き、`exit` で WSL の終了コードと一緒にシェルが戻る。bridge の helper に加えて `wsl` の shim の設置も必要 |
| 運ばれるオプション | `--distribution` / `-d`、`--cd`、`--user` / `-u`、単独の `~`。それ以外の行は打たれたまま本物の `wsl.exe` が動かす |
| 失敗したセッション | ペインが理由を表示したまま残る。シェルに戻って消えてしまうことはない |

</details>

## Notifications

| Feature | Notes |
| --- | --- |
| Desktop notifications | From the terminal (OSC 9 / OSC 777) and from ghostty itself, as Windows toasts. Clicking one selects the pane it came from |
| Command finished | `notify-on-command-finish`, for a command that ran longer than `notify-on-command-finish-after`. Needs the OSC 133 marks, which nothing writes here yet: [docs/NOTIFICATIONS.md](NOTIFICATIONS.md) |
| Bell | `RING_BELL` flashes the window and plays the system sound |

<details>
<summary>日本語</summary>

| 機能 | 補足 |
| --- | --- |
| デスクトップ通知 | 端末から (OSC 9 / OSC 777) と ghostty 自身から、Windows のトーストとして出る。クリックすると出どころのペインが選ばれる |
| コマンドの終了 | `notify-on-command-finish`。`notify-on-command-finish-after` より長く走ったコマンドが対象。OSC 133 の mark が必要で、ここではまだ誰も書かない: [docs/NOTIFICATIONS.md](NOTIFICATIONS.md) |
| ベル | `RING_BELL` でウインドウが光り、システム音が鳴る |

</details>

## libghostty actions

libghostty asks the host to do things through actions. 65 of the 69 it defines are wired up. The four that are not:

| Action | What it would do |
| --- | --- |
| `SET_WINDOW_TITLE` | The window's own title, as distinct from a surface's. `SET_TITLE` is wired, so tab titles follow the terminal |
| `MOVE_TAB_TO_NEW_WINDOW` | Tearing a tab out works from the mouse; the action is not routed to it |
| `SELECTION_CHANGED` | Nothing in the host listens for a selection changing yet |
| `EXPORT_TERMINAL_IO` | Writing the terminal's I/O to a file |

<details>
<summary>日本語</summary>

libghostty は action という形で host に仕事を頼む。定義されている 69 個のうち 65 個が繋がっている。繋がっていない 4 個:

| action | 何をするものか |
| --- | --- |
| `SET_WINDOW_TITLE` | surface のものとは別の、ウインドウ自体のタイトル。`SET_TITLE` は繋がっているので、タブのタイトルは端末に追従する |
| `MOVE_TAB_TO_NEW_WINDOW` | タブの引き剥がしはマウスからはできるが、この action からは繋がっていない |
| `SELECTION_CHANGED` | 選択範囲の変化を host 側で待っているものが、まだ無い |
| `EXPORT_TERMINAL_IO` | 端末の入出力をファイルに書き出すもの |

</details>

## Not yet

| Missing | Where it stands |
| --- | --- |
| Command palette, tab overview, quick terminal | The overlay UIs have no host side yet ([#205](https://github.com/i999rri/GhosttyWin32/issues/205), [#153](https://github.com/i999rri/GhosttyWin32/issues/153)) |
| Terminal inspector | Not on the DirectX renderer ([#152](https://github.com/i999rri/GhosttyWin32/issues/152)) |
| Automatic tab titles from the running program | Needs Windows foreground-process info ([#199](https://github.com/i999rri/GhosttyWin32/issues/199)) |
| Clipboard settings in a ConPTY pane | conhost answers OSC 52 itself, so the settings never reach the terminal ([#226](https://github.com/i999rri/GhosttyWin32/issues/226), [docs/CLIPBOARD.md](CLIPBOARD.md)) |
| Paste protection | Confirmed without asking, for want of a dialog ([#225](https://github.com/i999rri/GhosttyWin32/issues/225)) |
| Split or new tab from an in-place WSL pane | Opens the default shell rather than WSL ([#220](https://github.com/i999rri/GhosttyWin32/issues/220)) |

<details>
<summary>日本語</summary>

| まだ無いもの | 状況 |
| --- | --- |
| コマンドパレット、タブ一覧、クイックターミナル | overlay の UI はまだ host 側が無い ([#205](https://github.com/i999rri/GhosttyWin32/issues/205)、[#153](https://github.com/i999rri/GhosttyWin32/issues/153)) |
| ターミナルインスペクタ | DirectX レンダラ側に無い ([#152](https://github.com/i999rri/GhosttyWin32/issues/152)) |
| 動いているプログラムからタブ名を決める | Windows の前景プロセス情報が必要 ([#199](https://github.com/i999rri/GhosttyWin32/issues/199)) |
| ConPTY のペインでのクリップボード設定 | conhost が OSC 52 に自分で応答するので、設定が端末まで届かない ([#226](https://github.com/i999rri/GhosttyWin32/issues/226)、[docs/CLIPBOARD.md](CLIPBOARD.md)) |
| paste protection | 確認ダイアログが無いため、聞かずに承認している ([#225](https://github.com/i999rri/GhosttyWin32/issues/225)) |
| in-place の WSL ペインからの split と新規タブ | WSL ではなく既定のシェルで開く ([#220](https://github.com/i999rri/GhosttyWin32/issues/220)) |

</details>
