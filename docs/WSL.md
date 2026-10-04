# The WSL bridge

A WSL tab can run on a real Linux pty instead of on Windows' pseudo console, so the bytes a program writes reach the terminal unchanged. It is off by default and needs one binary installed inside the distribution. For the steps alone see Step 4 of [the install guide](INSTALL.md); this page is why it exists and how to tell whether it is working.

<details>
<summary>日本語</summary>

WSL のタブは、Windows の擬似コンソールではなく本物の Linux pty の上で動かせる。そうすると、プログラムが書いたバイト列がそのままターミナルに届く。既定では無効で、distribution の中にバイナリを 1 つ入れる必要がある。手順だけなら[インストール手順](INSTALL.md)の Step 4 を見てほしい。このページは、なぜそれが要るのかと、効いているかの確かめ方。

</details>

## What ConPTY does to the stream

A Windows console program does not read and write raw bytes; it talks to the console API. A terminal that runs one therefore has to put a pseudo console in between, and on Windows that is ConPTY — conhost running without a window. ConPTY parses what the program writes, keeps a screen buffer of its own, and emits VT built from that buffer.

That is a re-rendering, not a hand-off, and `wsl.exe` is an ordinary Windows console program. A WSL session driven through ConPTY therefore never delivers what the program in the distribution actually wrote. Sequences conhost implements itself are answered by conhost; ones it does not understand are dropped. OSC 52, the clipboard sequence, is of the first kind: conhost answers it, the terminal never sees it, and `clipboard-read` and `clipboard-write` have no effect in such a tab. [docs/CLIPBOARD.md](CLIPBOARD.md) covers that in full.

<details>
<summary>日本語</summary>

Windows のコンソールプログラムは生のバイト列を読み書きせず、コンソール API を使う。そうしたプログラムを動かすターミナルは擬似コンソールを間に挟む必要があり、Windows ではそれが ConPTY になる。ウインドウを持たない conhost のことだ。ConPTY はプログラムの出力を解釈し、自前の画面バッファを保ち、そのバッファから組み立て直した VT を出す。

これは受け渡しではなく再描画で、`wsl.exe` もただの Windows のコンソールプログラムだ。だから ConPTY 経由の WSL セッションでは、distribution の中のプログラムが実際に書いたものは届かない。conhost が自分で実装している列は conhost が応答し、理解しない列は捨てられる。クリップボードの列である OSC 52 は前者で、conhost が応答してしまうためターミナルには届かず、そうしたタブでは `clipboard-read` も `clipboard-write` も効かない。詳しくは [docs/CLIPBOARD.md](CLIPBOARD.md) に書いた。

</details>

## How the bridge avoids it

A pty can only be created by the Linux kernel, so the terminal has to be built inside the distribution. Three parts share the work:

- **The app** creates no ConPTY for this surface at all. It starts `wsl.exe` with plain redirected pipes.
- **`wsl.exe`**, with its stdio redirected, relays bytes between those pipes and the process it runs in the distribution. It emulates nothing.
- **`ghostty-wsl-bridge`** runs inside the distribution. It opens a real Linux pty, starts your shell on the slave side, and relays between the pty and its own stdio.

The terminal the shell sees is a Linux pty, and Windows only carries bytes. Both directions are framed, because the pipes have no out-of-band channel and a window resize has to travel somehow.

<details>
<summary>日本語</summary>

pty を作れるのは Linux カーネルだけなので、端末は distribution の中で用意するしかない。仕事は 3 つに分かれている。

- **アプリ**は、このサーフェスについては ConPTY を作らない。リダイレクトしたただのパイプを渡して `wsl.exe` を起動する。
- **`wsl.exe`** は stdio がリダイレクトされているので、そのパイプと distribution 内のプロセスの間でバイト列を中継するだけになる。何もエミュレートしない。
- **`ghostty-wsl-bridge`** が distribution の中で動く。本物の Linux pty を開き、slave 側でシェルを起こし、pty と自分の stdio の間を中継する。

シェルから見える端末は Linux の pty で、Windows 側はバイト列を運ぶだけになる。パイプには帯域外のチャンネルがなく、ウインドウのサイズ変更も伝える必要があるため、どちらの向きもフレームに包んである。

</details>

## Why the binaries are yours to install

Two pieces are, and for the same reason: the package is the one place neither of them can be run from.

`ghostty-wsl-bridge` runs inside the distribution, and that is the unit: on WSL 2 every distribution shares one lightweight utility VM and one kernel, but each has its own mount namespace, so `/usr/local/bin` is a different directory in each one.[^distros] Installing the binary in one does not install it in another, which is why `wsl -d NAME` is asked about separately and why the message that it is missing names the distribution.

A file inside an installed MSIX is read-only for you; DrvFs derives Unix permissions from the Windows ACL, and a file you cannot write arrives without the execute bit. A copy shipped that way is readable from the distribution and refused by `exec` before a byte of it is read.

The `wsl` shim runs on Windows, as a child of your shell, and fares no better. `C:\Program Files\WindowsApps` refuses even a read of its own permissions, and starting a program under it answers access denied whatever the file's own entry grants. The host cannot put a runnable copy anywhere for you either -- a directory it writes to is a directory it has to keep in step with every upgrade, and an executable it drops into your profile unasked is not the sort of thing a terminal should do.

So both are downloaded from the release and put where they are wanted, one `curl` each — [docs/INSTALL.md](INSTALL.md#step-4-wsl-bridge-optional--wsl-bridge-任意) has the lines. Neither is named by a path: the bridge is started by name on the distribution's `PATH`, and the shim is looked for in one place, `%LOCALAPPDATA%\ghostty\bin\`, beside the config.

Taking them from the release rather than out of the installed package is deliberate. Whether a shim and a host can work together is settled by the wire format the shim reports, not by which build produced the file, and copying the app's own copy would be asserting the opposite.

Nor does the host check what is there. With nothing installed, the shell finds System32's `wsl.exe` and that is the right answer, so there is nothing to detect. A shim from another build says which wire format it speaks when it asks for a pane, and the host answers that in the pane rather than refusing a line for a reason you cannot see. So the only thing that makes a reinstall necessary is that format changing -- not a release, and not a rebuild. For the bridge's helper the host can only ask the distribution, which it does before offering to swap a pane.

<details>
<summary>日本語</summary>

2 つあって、理由は同じ。どちらもパッケージの中からは実行できない。

`ghostty-wsl-bridge` は distribution の中で動く。そしてその単位が distribution であることに意味がある。WSL 2 ではすべての distribution が 1 つの軽量 utility VM とカーネルを共有するが、mount namespace は各自が持つので、`/usr/local/bin` は distribution ごとに別のディレクトリになる[^distros]。1 つに入れても別の 1 つには入っていない。だから `wsl -d NAME` は distribution ごとに確かめられるし、無いときのメッセージは distribution の名前を出す。

インストールされた MSIX の中のファイルは、ユーザーから見て読み取り専用になる。DrvFs は Unix の権限を Windows の ACL から導くので、書き込めないファイルには実行ビットが付かない。その形で同梱したコピーは、distribution から読めはしても、1 バイトも読まれないうちに `exec` に拒否される。

`wsl` の shim は Windows 側で、シェルの子として動くが、こちらも同じだ。`C:\Program Files\WindowsApps` は自分の権限を読むことすら拒否するし、その下のプログラムを起動すると、ファイル自身の権限が何を許していてもアクセス拒否が返る。host が代わりに実行できる場所へコピーしておくこともしない。コピー先を持つと、アプリを更新するたびにそのコピーも入れ替え続けることになる。それに、頼まれてもいない実行ファイルをユーザーのプロファイルに書くのは、ターミナルがやることではない。

そこで両方とも release からダウンロードして、必要な場所に置く。それぞれ `curl` 1 行で、コマンドは [docs/INSTALL.md](INSTALL.md#step-4-wsl-bridge-optional--wsl-bridge-任意) にある。どちらもパスで指定はしない。bridge は distribution の `PATH` から名前で起動し、shim は config の隣の `%LOCALAPPDATA%\ghostty\bin\` という 1 箇所だけを見る。

インストール済みのパッケージの中から取り出すのではなく release から取るのは意図的だ。shim と host が一緒に動けるかを決めるのは、shim が送ってくる形式の番号であって、そのファイルがどのビルドから出たかではない。アプリ自身のコピーを持ってこさせると、同じビルドのファイルでなければ動かない、と言っているのと同じになる。

置かれているものを host が点検することもしない。何も置かれていなければシェルは System32 の `wsl.exe` を見つけ、それが正しい結果なので、確かめる必要がない。別のビルドの shim は、ペインを頼むときに自分が対応している形式の番号を送ってくる。host が知らない番号だったときは、そのペインにそう書く。黙って断れば、打った人には理由が見えないからだ。だから置き直しが必要になるのは形式が変わったときだけで、リリースごとでも、ビルドし直すごとでもない。bridge の helper については distribution に尋ねるしかないので、ペインの差し替えを申し出る前に尋ねる。

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

`scripts/verify/wsl-exec-bit.ps1` は、前の節の前提を確かめる。パッケージの中のファイルは distribution から実行できず、自分が所有するファイルは実行できる、という前提だ。将来の Windows や WSL がこのページを間違いにしたときのために置いてある。

</details>

## Limits

It lives in one distribution, so a second distribution needs its own copy. The two halves speak a frame protocol with no version in it — unknown frame types are skipped, so a small difference survives, but replacing the binary with the one from the new release is worth doing after an upgrade.

On v0.8.2 a tab reaches the bridge only when the tab's own command is `wsl`, which means `command = wsl` in the config. A `wsl` typed inside a pwsh tab runs under that tab's ConPTY and is unaffected.

Because the app starts `wsl.exe` itself, no shell is involved, and a `function wsl` or `Set-Alias wsl` in a PowerShell profile has no say — nothing reads the profile here. Write the same arguments on the config line, which takes `wsl.exe`'s own:

```ini
command = wsl --cd ~ -d Ubuntu
```

`--cd` is where the session starts: `~` the Linux home, a leading `/` an absolute Linux path, anything else an absolute Windows path. Without it a session starts wherever the Windows working directory translates to, under `/mnt/c`. A lone `~`, as in `wsl ~`, means the same as `--cd ~`.

<details>
<summary>日本語</summary>

バイナリは 1 つの distribution の中にあるので、別の distribution を使うならそちらにも要る。2 つの半分が話すフレーム protocol には版が入っていない。知らない種類のフレームは読み飛ばす作りなので多少の差は耐えるが、アプリを更新したら新しい release のバイナリに置き換えておくのがよい。

v0.8.2 では、タブ自身のコマンドが `wsl` のときだけ bridge に入る。つまり config の `command = wsl` だ。pwsh のタブの中で `wsl` と打った場合は、そのタブの ConPTY の下で動くので関係ない。

アプリが `wsl.exe` を自分で起動するため、シェルは関与しない。PowerShell の profile に `function wsl` や `Set-Alias wsl` を書いていても効かない。ここでは profile を誰も読まないからだ。同じ引数は config の行に書く。そちらは `wsl.exe` 自身のオプションを取れる (上の例)。

`--cd` はセッションの開始位置で、`~` なら Linux のホーム、先頭が `/` なら Linux の絶対パス、それ以外は Windows の絶対パス。指定しなければ、Windows の作業ディレクトリを変換した先、つまり `/mnt/c` の下で始まる。`wsl ~` のような単独の `~` も `--cd ~` と同じ意味になる。

</details>

[^distros]: [Comparing WSL versions](https://learn.microsoft.com/en-us/windows/wsl/compare-versions) — distributions running under WSL 2 share the network namespace, device tree, CPU, kernel, memory and `/init`, and have their own PID, mount, user and cgroup namespaces and their own init process.
