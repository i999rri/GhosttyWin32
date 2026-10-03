# Clipboard on Windows

`clipboard-read` and `clipboard-write` do not mean the same thing in every pane, and a terminal-initiated read is refused outright for now. Both are consequences of Windows, not of choices made here, but neither is written down in the upstream [configuration reference](https://ghostty.org/docs/config). This page is a placeholder until these notes move somewhere better.

<details>
<summary>日本語</summary>

`clipboard-read` と `clipboard-write` は、どのペインでも同じ意味になるわけではない。またターミナル内のプログラムからの読み取りは、今のところ一律で拒否している。どちらも Windows 側の事情によるもので、このアプリの方針として選んだものではないが、上流の[設定ドキュメント](https://ghostty.org/docs/config)には書かれていない。このページは、もっと良い置き場所ができるまでの仮置き。

</details>

## Which pane you are in decides which rules apply

A pane running a Windows console program — pwsh, cmd, anything that talks to the console API — is backed by ConPTY, which is conhost running headless. conhost parses the output stream itself and answers OSC 52 without passing it on, so the settings below never reach the terminal. A write goes to the Windows clipboard whenever the window has focus, and a read is dropped with no answer at all. `clipboard-write = deny` does not stop the write, and `clipboard-read` has no effect either.

A WSL pane goes through the bridge, which owns a real pty inside the distribution, and a shell reached over ssh from such a pane is the same. There is no conhost in the way, so both settings apply as documented.

<details>
<summary>日本語</summary>

Windows のコンソールプログラム (pwsh、cmd、コンソール API を使うもの全般) が動くペインは、ConPTY が裏にいる。ConPTY の正体は、画面を持たないモードで動く conhost。conhost は出力を自分で解析して OSC 52 に自分で応答し、その先へは渡さない。そのため下記の設定はターミナルまで届かない。書き込みはウィンドウにフォーカスがあれば Windows のクリップボードに入り、読み取りは応答なしで捨てられる。`clipboard-write = deny` にしても書き込みは止まらないし、`clipboard-read` も効かない。

WSL のペインは bridge を通り、distro 内の本物の pty につながる。そのペインから ssh で入った先も同じ。あいだに conhost がいないので、どちらの設定もドキュメントどおりに効く。

</details>

## A program in the terminal cannot read the clipboard

The default `clipboard-read = ask` means libghostty asks before handing the clipboard to a program in the terminal. This app has no prompt to ask with, so it refuses instead: OSC 52 and Kitty clipboard reads get an answer with nothing in it. `clipboard-read = allow` still works and skips the question entirely.

Pasting is unaffected. Ctrl+V is something you asked for, so it goes through, including the paste that trips paste protection — which means the protection currently warns nobody ([#225](https://github.com/i999rri/GhosttyWin32/issues/225)). Both halves are waiting on the same confirmation dialog.

<details>
<summary>日本語</summary>

既定の `clipboard-read = ask` は、「ターミナル内のプログラムにクリップボードを渡す前に確認する」という意味。このアプリには確認する画面がないので、代わりに拒否している。OSC 52 と Kitty クリップボードの読み取りには、中身が空の応答が返る。`clipboard-read = allow` はこれまでどおり効き、確認そのものを省略する。

貼り付けには影響しない。Ctrl+V は利用者が自分で指示したものなので通る。paste protection に引っかかる貼り付けも通ってしまうので、現状その保護は誰にも警告できていない ([#225](https://github.com/i999rri/GhosttyWin32/issues/225))。どちらも同じ確認ダイアログ待ち。

</details>

## Checking it yourself

`scripts/verify/clipboard-osc52.sh` seeds the clipboard through the terminal, asks for it back, and reports whether the reply carries any data. Run it in a WSL pane: a pwsh pane cannot answer the question, for the reason above, and its silence looks exactly like a pass.

```sh
bash scripts/verify/clipboard-osc52.sh ask     # the default: expect no data
bash scripts/verify/clipboard-osc52.sh allow   # clipboard-read = allow: expect the marker
```

<details>
<summary>日本語</summary>

`scripts/verify/clipboard-osc52.sh` は、ターミナル経由でクリップボードに目印を入れ、それを読み返して、応答に中身が入っているかを報告する。実行するのは WSL のペイン。pwsh のペインでは上記の理由で判定できず、その沈黙は成功とまったく同じに見えてしまう。

</details>
