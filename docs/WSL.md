# The WSL bridge

A WSL tab can run on a real Linux pty instead of on Windows' pseudo console, so the bytes a program writes reach the terminal unchanged. It is off by default and needs one binary installed inside the distribution. For the steps alone see Step 4 of [the install guide](INSTALL.md); this page is why it exists and how to tell whether it is working.

<details>
<summary>日本語</summary>

WSL のタブは、Windows の擬似コンソールではなく本物の Linux pty の上で動かせる。そうすると、プログラムが書いたバイト列がそのままターミナルに届く。既定では無効で、distro の中にバイナリを 1 つ入れる必要がある。手順だけなら[インストール手順](INSTALL.md)の Step 4 を見てほしい。このページは、なぜそれが要るのかと、効いているかの確かめ方。

</details>

## What ConPTY does to the stream

A Windows console program does not read and write raw bytes; it talks to the console API. A terminal that runs one therefore has to put a pseudo console in between, and on Windows that is ConPTY — conhost running without a window. ConPTY parses what the program writes, keeps a screen buffer of its own, and emits VT built from that buffer.

That is a re-rendering, not a hand-off, and `wsl.exe` is an ordinary Windows console program. A WSL session driven through ConPTY therefore never delivers what the program in the distribution actually wrote. Sequences conhost implements itself are answered by conhost; ones it does not understand are dropped. OSC 52, the clipboard sequence, is of the first kind: conhost answers it, the terminal never sees it, and `clipboard-read` and `clipboard-write` have no effect in such a tab. [docs/CLIPBOARD.md](CLIPBOARD.md) covers that in full.

<details>
<summary>日本語</summary>

Windows のコンソールプログラムは生のバイト列を読み書きせず、コンソール API を使う。そうしたプログラムを動かすターミナルは擬似コンソールを間に挟む必要があり、Windows ではそれが ConPTY になる。ウインドウを持たない conhost のことだ。ConPTY はプログラムの出力を解釈し、自前の画面バッファを保ち、そのバッファから組み立て直した VT を出す。

これは受け渡しではなく再描画で、`wsl.exe` もただの Windows のコンソールプログラムだ。だから ConPTY 経由の WSL セッションでは、distro の中のプログラムが実際に書いたものは届かない。conhost が自分で実装している列は conhost が応答し、理解しない列は捨てられる。クリップボードの列である OSC 52 は前者で、conhost が応答してしまうためターミナルには届かず、そうしたタブでは `clipboard-read` も `clipboard-write` も効かない。詳しくは [docs/CLIPBOARD.md](CLIPBOARD.md) に書いた。

</details>

## How the bridge avoids it

A pty can only be created by the Linux kernel, so the terminal has to be built inside the distribution. Three parts share the work:

- **The app** creates no ConPTY for this surface at all. It starts `wsl.exe` with plain redirected pipes.
- **`wsl.exe`**, with its stdio redirected, relays bytes between those pipes and the process it runs in the distribution. It emulates nothing.
- **`ghostty-wsl-bridge`** runs inside the distribution. It opens a real Linux pty, starts your shell on the slave side, and relays between the pty and its own stdio.

The terminal the shell sees is a Linux pty, and Windows only carries bytes. Both directions are framed, because the pipes have no out-of-band channel and a window resize has to travel somehow.

<details>
<summary>日本語</summary>

pty を作れるのは Linux カーネルだけなので、端末は distro の中で用意するしかない。仕事は 3 つに分かれている。

- **アプリ**は、このサーフェスについては ConPTY を作らない。リダイレクトしたただのパイプを渡して `wsl.exe` を起動する。
- **`wsl.exe`** は stdio がリダイレクトされているので、そのパイプと distro 内のプロセスの間でバイト列を中継するだけになる。何もエミュレートしない。
- **`ghostty-wsl-bridge`** が distro の中で動く。本物の Linux pty を開き、slave 側でシェルを起こし、pty と自分の stdio の間を中継する。

シェルから見える端末は Linux の pty で、Windows 側はバイト列を運ぶだけになる。パイプには帯域外のチャンネルがなく、ウインドウのサイズ変更も伝える必要があるため、どちらの向きもフレームに包んである。

</details>

## Why the binary is yours to install

It is not in the package, and it cannot be. A file inside an installed MSIX is read-only for you; DrvFs derives Unix permissions from the Windows ACL, and a file you cannot write arrives without the execute bit. A copy shipped that way is readable from the distribution and refused by `exec` before a byte of it is read.

Nor is it named by a path. The app runs it by name and lets the distribution's `PATH` find it, the same way the shell finds any other command — so there is nothing to point at it, and `wsl-bridge` is the only setting involved.

That costs the one thing a path was buying: the app can no longer tell from the Windows side whether the binary is there, so it cannot quietly fall back to ConPTY when it is not. Turning `wsl-bridge` on without installing the binary ends the session with `ghostty-wsl-bridge: not found` instead. The default is off to make that the right answer: a session that cannot start belongs to someone who asked for it.

<details>
<summary>日本語</summary>

このバイナリはパッケージに入っていないし、入れられない。インストールされた MSIX の中のファイルは、利用者から見て読み取り専用になる。DrvFs は Unix の権限を Windows の ACL から導くので、書き込めないファイルには実行ビットが付かない。その形で同梱したコピーは、distro から読めはしても、1 バイトも読まれないうちに `exec` に拒否される。

パスで指定するものでもない。アプリは名前で起動し、distro の `PATH` に解決させる。シェルがほかのコマンドを見つけるのと同じだ。だから指し示すための設定は要らず、関わる設定は `wsl-bridge` だけになる。

その代わり、パスが買っていた唯一のものを失う。バイナリがあるかどうかを Windows 側から判断できなくなるので、無いときに黙って ConPTY へ戻ることもできない。バイナリを入れずに `wsl-bridge` を on にすると、セッションは `ghostty-wsl-bridge: not found` で終わる。既定が off なのは、それを妥当な答えにするためだ。起動できないセッションは、自分で有効にした人のものになる。

</details>

## Checking it

Four states, and what each should do. `wsl-bridge` is in `%LOCALAPPDATA%\ghostty\config`; the app has to be restarted after changing it.

| `wsl-bridge` | binary on `PATH` | what you should get |
| --- | --- | --- |
| absent | — | ConPTY |
| `false` | — | ConPTY |
| `true` | yes | the bridge |
| `true` | no | `ghostty-wsl-bridge: not found`, and the tab ends |

From a pwsh tab or Windows Terminal, one bridge process exists per WSL surface, tab or split:

```powershell
wsl.exe -- pgrep -af ghostty-wsl-bridge
```

From inside a WSL tab, `scripts/verify/wsl-bridge-routing.sh` answers which path that tab is on. It asks twice on purpose: the process tree above the shell says how the tab was started, and an OSC 52 round trip says what the stream does. A tab started through the bridge whose bytes do not survive the trip is a different problem from one that was never on the bridge.

```sh
bash scripts/verify/wsl-bridge-routing.sh
```

`scripts/verify/wsl-exec-bit.ps1` checks the premise of the section above — that a file inside the package cannot be run from the distribution and one you own can. It is worth running if a future Windows or WSL makes this page wrong.

<details>
<summary>日本語</summary>

状態は 4 つで、それぞれ何が起きるべきかが上の表。`wsl-bridge` は `%LOCALAPPDATA%\ghostty\config` にあり、変更したらアプリの再起動が要る。

pwsh のタブか Windows Terminal からは、上の `pgrep` で確認できる。bridge のプロセスは WSL のサーフェス 1 枚につき 1 本で、タブでも分割でも同じ。

WSL のタブの中からは `scripts/verify/wsl-bridge-routing.sh` が、そのタブがどちらの経路かを答える。2 つのことを別々に聞いているのは意図したもので、シェルより上のプロセスの並びは「どう起動されたか」を、OSC 52 の往復は「ストリームが何をするか」を示す。bridge 経由で起動したのにバイト列が往復しないタブは、そもそも bridge を通っていないタブとは別の問題だ。

`scripts/verify/wsl-exec-bit.ps1` は、前の節の前提を確かめる。パッケージの中のファイルは distro から実行できず、自分が所有するファイルは実行できる、という前提だ。将来の Windows や WSL がこのページを間違いにしたときのために置いてある。

</details>

## Limits

It lives in one distribution, so a second distribution needs its own copy. The two halves speak a frame protocol with no version in it — unknown frame types are skipped, so a small difference survives, but replacing the binary with the one from the new release is worth doing after an upgrade.

On v0.8.2 a tab reaches the bridge only when the tab's own command is `wsl`, which means `command = wsl` in the config. A `wsl` typed inside a pwsh tab runs under that tab's ConPTY and is unaffected.

<details>
<summary>日本語</summary>

バイナリは 1 つの distro の中にあるので、別の distro を使うならそちらにも要る。2 つの半分が話すフレーム protocol には版が入っていない。知らない種類のフレームは読み飛ばす作りなので多少の差は耐えるが、アプリを更新したら新しい release のバイナリに置き換えておくのがよい。

v0.8.2 では、タブ自身のコマンドが `wsl` のときだけ bridge に入る。つまり config の `command = wsl` だ。pwsh のタブの中で `wsl` と打った場合は、そのタブの ConPTY の下で動くので関係ない。

</details>
