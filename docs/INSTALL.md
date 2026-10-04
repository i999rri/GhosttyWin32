# Install Guide / インストール手順

Step-by-step manual install for the self-signed MSIX. Manual install is the only supported path while an established CA / OSS Foundation signature is unavailable — see [issue #46](https://github.com/i999rri/GhosttyWin32/issues/46).

<details><summary>日本語</summary>

自己署名 MSIX を手動インストールする手順。正式な CA / OSS Foundation 署名が取得できるまでは手動インストールが唯一のサポート経路 (詳細は [issue #46](https://github.com/i999rri/GhosttyWin32/issues/46))。

</details>

## Prerequisites / 前提

- Windows 11 or Windows 10 1809+
- Administrator access (one-time, only for trusting the certificate)
- Two files from a [Release](https://github.com/i999rri/GhosttyWin32/releases): `Ghostty-X.Y.Z-x64.msix` and `Ghostty.cer`

<details><summary>日本語</summary>

- Windows 11 または Windows 10 1809+
- 管理者権限 (証明書を信頼するときの一回のみ)
- [Releases](https://github.com/i999rri/GhosttyWin32/releases) からダウンロードする2ファイル: `Ghostty-X.Y.Z-x64.msix` と `Ghostty.cer`

</details>

## Step 1: Trust the certificate / 証明書を信頼する

GhosttyWin32 is signed with a self-signed publisher certificate (subject `CN=i999rri`). Windows refuses to sideload the MSIX until that certificate lives in a machine-level trust store. **One-time setup per machine.**

<details><summary>日本語</summary>

GhosttyWin32 は自己署名のパブリッシャー証明書 (subject `CN=i999rri`) で署名されている。Windows はその証明書がマシンレベルの信頼ストアに入っていない限り MSIX のサイドロードを拒否する。**マシンごとに一度だけ必要**。

</details>

The cert install wizard has three places where the wrong choice silently puts the cert in a useless store. Follow these exactly:

1. Right-click `Ghostty.cer` → **Install Certificate**
2. Select **Local Machine** (NOT *Current User*) → **Next** → accept the UAC prompt
3. Choose **Place all certificates in the following store** (NOT *Automatically select the certificate store...*)
4. Click **Browse** → select **Trusted People** → **OK**
5. **Next** → **Finish**

<details><summary>日本語</summary>

証明書インストールのウィザードには、誤った選択が無音でストア違いに繋がる箇所が3つある。下記の通り正確に:

1. `Ghostty.cer` を右クリック → 「**証明書のインストール**」
2. 「**ローカル コンピューター**」を選択 (「現在のユーザー」は不可) → 「**次へ**」 → UAC プロンプトで「はい」
3. 「**証明書をすべて次のストアに配置する**」を選ぶ (「証明書の種類に基づいて、自動的に選択する」は不可)
4. 「**参照**」 → 「**信頼されたユーザー**」を選択 → 「**OK**」
5. 「**次へ**」 → 「**完了**」

</details>

## Step 2: Verify the certificate (optional) / 証明書の確認 (任意)

Open `certlm.msc`, navigate to **Trusted People** → **Certificates**. An entry issued to `i999rri` should appear.

<details><summary>日本語</summary>

`certlm.msc` を起動し、「**信頼されたユーザー**」 → 「**証明書**」を開く。`i999rri` 宛の証明書が表示されればOK。

</details>

## Step 3: Install the MSIX / MSIX をインストール

Double-click `Ghostty-X.Y.Z-x64.msix`. Windows shows the publisher (`i999rri`), the version, and a capability list. Click **Install**.

<details><summary>日本語</summary>

`Ghostty-X.Y.Z-x64.msix` をダブルクリック。Windows がパブリッシャー (`i999rri`) とバージョン、必要な機能の一覧を表示する。「**インストール**」を押す。

</details>

If you previously installed Ghostty (stable or dev), the new MSIX upgrades it in place. Settings, configs, and themes under `%LOCALAPPDATA%\ghostty\` are preserved.

<details><summary>日本語</summary>

過去に Ghostty (stable / dev どちらでも) をインストールしている場合、新しい MSIX は in-place アップグレードされる。`%LOCALAPPDATA%\ghostty\` 配下の設定・テーマはそのまま保持される。

</details>

## Step 4: WSL bridge (optional) / WSL bridge (任意)

Skip this unless you want WSL tabs to run on a Linux pty instead of on Windows' pseudo console. [docs/WSL.md](WSL.md) explains what that buys and why the binary is not in the package.

Inside the distribution, with `<tag>` the release you installed:

```sh
curl -fsSLo /tmp/gwb https://github.com/i999rri/GhosttyWin32/releases/download/<tag>/ghostty-wsl-bridge \
  && sudo install -Dm755 /tmp/gwb /usr/local/bin/ghostty-wsl-bridge
```

`/usr/local/bin` because the `PATH` that matters is the one a session gets from `wsl.exe --exec`, not a login shell's: a directory added by `.profile` or `.zshrc` — `~/.local/bin`, typically — will not do. Anywhere on that `PATH` works, since the app starts the binary by name.

Then turn it on in `%LOCALAPPDATA%\ghostty\config` and restart the app:

```ini
wsl-bridge = true
```

With the setting on and the binary missing, a WSL tab ends with `ghostty-wsl-bridge: not found` rather than falling back — see [docs/WSL.md](WSL.md#why-the-binaries-are-yours-to-install).

<details><summary>日本語</summary>

WSL のタブを Windows の擬似コンソールではなく Linux の pty で動かしたい場合だけ必要。何が得られるのか、なぜバイナリがパッケージに入っていないのかは [docs/WSL.md](WSL.md) に書いた。

distribution の中で、`<tag>` はインストールした release (上のコマンド)。

`/usr/local/bin` なのは、ここで効く `PATH` が `wsl.exe --exec` がセッションに与えるもので、ログインシェルのものではないから。`.profile` や `.zshrc` で足したディレクトリ (たいていは `~/.local/bin`) は使えない。その `PATH` 上ならどこに置いてもよい。アプリはバイナリを名前で起動する。

そのうえで `%LOCALAPPDATA%\ghostty\config` で有効にし、アプリを再起動する。

設定を on にしたままバイナリが無い場合、WSL のタブはフォールバックせずに `ghostty-wsl-bridge: not found` で終わる。理由は [docs/WSL.md](WSL.md#why-the-binaries-are-yours-to-install) に書いた。

</details>

### The `wsl` shim / `wsl` の shim

Only needed to get the bridge for a `wsl` typed at a prompt: without the shim that session runs under ConPTY, like any other program a shell starts. A WSL *tab* goes through the bridge with `wsl-bridge` alone — this is for the other way in. One line, with `<tag>` the release you installed:

```powershell
curl.exe -fsSL --create-dirs -o "$env:LOCALAPPDATA\ghostty\bin\wsl.exe" https://github.com/i999rri/GhosttyWin32/releases/download/<tag>/wsl.exe
```

The name has to stay `wsl.exe` -- that is the whole mechanism: the host puts that directory first on the `PATH` of the shells it starts, so the shell finds this one before System32's. Nothing outside those panes is affected.

It needs replacing only when the way it talks to the app changes, not on every release: its protocol version is in the line it sends the host, and a version the host does not know is answered in that pane. With nothing installed, `wsl` in a pane is simply the real `wsl.exe`.

<details><summary>日本語</summary>

プロンプトで打った `wsl` のセッションを bridge で動かしたい場合だけ必要。shim が無いと、そのセッションはシェルが起動する他のプログラムと同じく ConPTY の下で動く。最初から WSL で開いたタブは `wsl-bridge` だけで bridge を通るので、これはもう一方の入り口のためのもの。1 行で済む。`<tag>` はインストールした release (上のコマンド)。

名前は `wsl.exe` でなければならない。host は自分が起動するシェルの `PATH` の先頭に `%LOCALAPPDATA%\ghostty\bin` を置くので、そのシェルは System32 より先にここの `wsl.exe` を 見つける。名前が違えば見つからないし、ここを見るのはそのペインのシェルだけで、ほかのシェルの `wsl` は変わらない。

置き換えが必要なのは、アプリとのやりとりの形が変わったときだけで、リリースごとではない。shim が host に送る行には、自分のプロトコルのバージョンが入っている。host が知らないバージョンなら、そのペインに「別のビルドの shim なので入れ替えてほしい」と出て、本物の `wsl.exe` が動く。何も置いていない場合も本物が動く。

</details>

### Checking both landed / 両方入ったか確かめる

```powershell
Get-Command -All wsl | Format-Table CommandType, Source      # the first Application is the shim
wsl.exe --exec /bin/sh -c 'command -v ghostty-wsl-bridge'    # prints the helper's path
```

The first line asked in this shell only says the file is in place; the `PATH` of a pane is the app's to set, and it puts that directory first.

<details><summary>日本語</summary>

1 行目をこのシェルで打つと System32 の `wsl.exe` が先に出る。このシェルの `PATH` に `%LOCALAPPDATA%\ghostty\bin` は入っていないからで、ここで分かるのはファイルが置かれたことだけだ。ペインの `PATH` はアプリが組み立て、そこではこのディレクトリが先頭に来る。

</details>

## Troubleshooting / トラブルシューティング

### Error 0x800B0109 — root certificate not trusted

> アプリ パッケージまたはバンドルの署名のルート証明書は、信頼されている必要があります。

The certificate is missing or installed in the wrong store. Common causes:

- Installed under **Current User** instead of **Local Machine** — MSIX install requires machine-level trust.
- Installed into **Personal** instead of **Trusted People** — wrong store for publisher trust.
- Selected *Automatically select the certificate store...* — Windows defaults to **Personal** for code-signing certs, which doesn't grant sideload trust.

**Fix:** open `certlm.msc` and confirm the cert appears under **Trusted People** → **Certificates**. If it's anywhere else (or missing), redo Step 1 with the exact choices listed there.

<details><summary>日本語</summary>

> アプリ パッケージまたはバンドルの署名のルート証明書は、信頼されている必要があります。

証明書が無いか、間違ったストアに入っている。よくある原因:

- 「**ローカル コンピューター**」ではなく「**現在のユーザー**」にインストールした → MSIX インストールはマシンレベルの信頼が必要
- 「**信頼されたユーザー**」ではなく「**個人**」にインストールした → ストア違い
- 「証明書の種類に基づいて、自動的に選択する」を選んだ → Windows はコード署名証明書を「**個人**」に入れる、これはサイドロード信頼を付与しない

**対処:** `certlm.msc` を開き、「**信頼されたユーザー**」 → 「**証明書**」配下に証明書があるか確認。別の場所に入っている (or 入っていない) なら Step 1 を上記の選択通りに再実行。

</details>

### Error 0x80073CF3 — package validation failed

The downloaded MSIX is corrupt or doesn't match the cert. Re-download both files from the same Release page (don't mix MSIX and `.cer` from different releases — they may use different signing keys after a cert rotation).

<details><summary>日本語</summary>

ダウンロードした MSIX が破損しているか、証明書と一致しない。同じ Release ページから両ファイルを再ダウンロードする (異なるリリース間で MSIX と `.cer` を混ぜないこと — 証明書ローテーション後は別の署名鍵が使われる場合がある)。

</details>

### Sideload disabled

> このアプリのインストールはブロックされています。 / This app installation is blocked.

Sideloading is disabled in OS settings. **Settings → Privacy & Security → For developers → Developer Mode** (or *Sideload apps* on older builds) → On.

<details><summary>日本語</summary>

> このアプリのインストールはブロックされています。

OS 設定でサイドロードが無効化されている。「**設定 → プライバシーとセキュリティ → 開発者向け → 開発者モード**」を有効化 (古いビルドでは「サイドロード」)。

</details>

### Other errors / その他のエラー

If you see a different error code, check existing [GitHub Issues](https://github.com/i999rri/GhosttyWin32/issues) or open a new one with the exact error text and your Windows build (`winver`).

<details><summary>日本語</summary>

別のエラーコードが出た場合は既存の [GitHub Issues](https://github.com/i999rri/GhosttyWin32/issues) を確認するか、エラー文と Windows ビルド番号 (`winver`) を貼って新規 issue を立てる。

</details>

## Uninstalling / アンインストール

### Removing Ghostty / Ghostty を削除

- **Settings → Apps → Installed apps** → search "Ghostty" → **⋯** → **Uninstall**

User data under `%LOCALAPPDATA%\ghostty\` is not removed automatically. Delete that directory manually if you want a clean state.

<details><summary>日本語</summary>

- 「**設定 → アプリ → インストール済みアプリ**」で「Ghostty」を検索 → 「**⋯**」 → 「**アンインストール**」

`%LOCALAPPDATA%\ghostty\` 配下のユーザーデータは自動削除されない。完全に消したい場合は手動で削除。

</details>

### Removing the certificate / 証明書を削除

Only needed if you want to revoke trust (e.g., the publisher key was rotated and you want to install a fresh `.cer`).

1. `certlm.msc` → **Trusted People** → **Certificates**
2. Right-click the `i999rri` entry → **Delete**

<details><summary>日本語</summary>

証明書削除は信頼を取り消したい場合のみ必要 (例: パブリッシャー鍵がローテーションされたので新しい `.cer` を入れ直したい)。

1. `certlm.msc` → 「**信頼されたユーザー**」 → 「**証明書**」
2. `i999rri` のエントリを右クリック → 「**削除**」

</details>
