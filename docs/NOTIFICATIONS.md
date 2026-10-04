# Command-finished notifications

ghostty can tell you when a long command ends, which is what you want while a build runs in a pane you are not looking at. The setting is upstream's; what the notification looks like is this app's. Neither fires on its own here yet, for a reason worth knowing before you go looking for the bug.

<details>
<summary>日本語</summary>

長く走ったコマンドが終わったとき、ghostty はそれを知らせられる。見ていないペインでビルドを回しているときに欲しいのがこれ。設定は上流のもので、通知の見え方はこのアプリのもの。ただし今のところ、どちらも自分からは発火しない。バグを探し始める前に知っておく価値のある理由がある。

</details>

## The settings

Three upstream keys decide whether a notification happens. `notify-on-command-finish` is `never` by default, so nothing happens until it is set.

| | |
| --- | --- |
| `notify-on-command-finish` | `never` (default), `unfocused` or `always`. `unfocused` skips a command that ends in the pane you are looking at. |
| `notify-on-command-finish-action` | What to do: `bell`, `notify`, or both. |
| `notify-on-command-finish-after` | How long the command must have run to be worth it. `5s` by default. |

<details>
<summary>日本語</summary>

通知するかどうかは上流の 3 つのキーが決める。`notify-on-command-finish` の既定は `never` なので、設定するまで何も起きない。

| | |
| --- | --- |
| `notify-on-command-finish` | `never` (既定)、`unfocused`、`always`。`unfocused` は、いま見ているペインで終わったコマンドを飛ばす。 |
| `notify-on-command-finish-action` | 何をするか。`bell`、`notify`、または両方。 |
| `notify-on-command-finish-after` | 通知に値するとみなす実行時間の下限。既定は `5s`。 |

</details>

## What you get

`bell` beeps. `notify` raises a Windows toast, and clicking it selects the tab and pane the command ran in.

The toast's title comes from the exit code: `Command Succeeded` for zero, `Command Failed` for anything else, and `Command Finished` when the shell reported no code at all. Its body reads like `Finished in 6s (exit 2)`.

"Focused", for `unfocused`, means both at once: the pane is this window's active one, and this window is in the foreground. A command that ends in a pane you can see needs no announcement; one in a background window does.

<details>
<summary>日本語</summary>

`bell` は鳴る。`notify` は Windows のトーストを出し、クリックするとそのコマンドが走っていたタブとペインが選ばれる。

トーストのタイトルは終了コードで決まる。0 なら `Command Succeeded`、それ以外なら `Command Failed`、シェルがコードを報告しなかった場合は `Command Finished`。本文は `Finished in 6s (exit 2)` のような形。

`unfocused` でいう「焦点がある」は 2 つ同時に満たす場合を指す。そのペインがこのウインドウのアクティブなペインであり、かつこのウインドウが前面にあること。見えているペインで終わったコマンドは知らせる必要がなく、背面のウインドウのものは必要になる。

</details>

## Why it does not fire on its own

ghostty learns that a command began and ended from the `OSC 133` marks a shell writes: `OSC 133;C` at the start, `OSC 133;D;<exit>` at the end. Nothing else tells it. Those marks come from shell integration, which this port does not run — there is no resources directory for it to inject from — so no shell here writes them, and ghostty never sees a command at all.

So the settings above are in effect and still nothing happens. The notification is not broken; it is never asked for. A plain `sleep 6` cannot test it, and reading a passing check that says otherwise is how [#175](https://github.com/i999rri/GhosttyWin32/pull/175) went wrong.

The marks themselves are ordinary terminal output. Any program can write them, which is what the script below does.

<details>
<summary>日本語</summary>

ghostty がコマンドの開始と終了を知る手段は、シェルが書く `OSC 133` の mark だけ。開始が `OSC 133;C`、終了が `OSC 133;D;<exit>`。これ以外に伝える経路はない。この mark は shell integration が出すもので、この port では shell integration が動いていない (差し込む元になる resources ディレクトリがない)。つまりここではどのシェルも mark を書かず、ghostty はコマンドの存在を知らない。

だから上の設定を入れても何も起きない。通知が壊れているのではなく、誰も頼んでいない。素の `sleep 6` では試せず、それで通ったことになっていたチェックを読んで [#175](https://github.com/i999rri/GhosttyWin32/pull/175) を間違えた。

mark そのものは普通の端末出力にすぎない。どのプログラムでも書けるので、下のスクリプトがそれをやる。

</details>

## Checking it yourself

`scripts/verify/command-finished.ps1` writes the marks by hand, so the host side can be exercised without shell integration. Put these in the config and reload with `ctrl+shift+,`:

```
notify-on-command-finish = unfocused
notify-on-command-finish-action = bell,notify
```

Then, in a pane:

```powershell
.\scripts\verify\command-finished.ps1
# alt-tab away within 6 s -> bell + toast "Command Failed / Finished in 6s (exit 2)"

.\scripts\verify\command-finished.ps1 -ExitCode 0   # -> "Command Succeeded"
.\scripts\verify\command-finished.ps1 -Seconds 3    # under the 5s threshold -> nothing, which is the pass
```

Staying focused the whole time also gets nothing, and that too is a pass with `unfocused`. The script's own header repeats all of this; `Get-Help` prints it.

<details>
<summary>日本語</summary>

`scripts/verify/command-finished.ps1` は mark を手で書くので、shell integration なしで host 側を試せる。config にこれを入れて `ctrl+shift+,` で reload する。

```
notify-on-command-finish = unfocused
notify-on-command-finish-action = bell,notify
```

そのうえでペインの中で:

```powershell
.\scripts\verify\command-finished.ps1
# 6 秒以内に alt-tab する → bell と "Command Failed / Finished in 6s (exit 2)" のトースト

.\scripts\verify\command-finished.ps1 -ExitCode 0   # → "Command Succeeded"
.\scripts\verify\command-finished.ps1 -Seconds 3    # しきい値 5s 未満 → 何も出ないのが正解
```

ずっと焦点を当てたままにした場合も何も出ない。`unfocused` ではそれも正解。同じ内容はスクリプトの先頭にも書いてあり、`Get-Help` で読める。

</details>
