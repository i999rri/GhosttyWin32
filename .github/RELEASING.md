# リリース手順

GhosttyWin32 のリリースは GitHub Actions で MSIX をビルド・自己署名 → GitHub Releases にアップロードする。
Scoop / 手動どちらでもインストール可能な配布形態。

## 一回だけやるセットアップ

リポジトリオーナーが最初に一度だけやる作業。証明書の有効期限が切れたら再実行。

### 1. 自己署名証明書を生成

ローカル PowerShell（管理者でなくて良い）で:

```powershell
$cert = New-SelfSignedCertificate `
  -Type CodeSigningCert `
  -Subject "CN=i999rri" `
  -KeyAlgorithm RSA `
  -KeyLength 2048 `
  -HashAlgorithm SHA256 `
  -KeyExportPolicy Exportable `
  -KeyUsage DigitalSignature `
  -CertStoreLocation Cert:\CurrentUser\My `
  -NotAfter (Get-Date).AddYears(5) `
  -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")

$cert.Thumbprint
```

`-Subject` は `Package.appxmanifest` の `Publisher` 属性と**完全に一致**させる必要がある。
今のマニフェストは `CN=i999rri` なのでそのまま使う。変える場合は両方変える。

### 2. PFX (秘密鍵入り) をエクスポート

```powershell
$pwd = Read-Host -AsSecureString -Prompt "PFX password (記録しておく)"
Export-PfxCertificate `
  -Cert "Cert:\CurrentUser\My\$($cert.Thumbprint)" `
  -FilePath ghostty-signing.pfx `
  -Password $pwd
```

### 3. PFX を Base64 化

```powershell
$pfxBytes = [System.IO.File]::ReadAllBytes("ghostty-signing.pfx")
$pfxBase64 = [System.Convert]::ToBase64String($pfxBytes)
$pfxBase64 | Set-Clipboard
Write-Host "PFX base64 をクリップボードにコピーしました"
```

### 4. GitHub Secrets に登録

リポジトリ Settings → Secrets and variables → Actions → New repository secret:

| Secret 名 | 値 |
|---|---|
| `SIGNING_PFX_BASE64` | クリップボードに入った Base64 文字列 |
| `SIGNING_PFX_PASSWORD` | 手順 2 で入力したパスワード |

### 5. Environments を作成

リリース種別ごとに Environment を分けて、production だけ手動承認を要求する構成にする。

#### `release` (production / stable タグ用)

Settings → Environments → New environment → 名前 `release`

| 設定項目 | 値 |
|---|---|
| **Deployment branches and tags** | "Selected branches and tags" → `v*` パターンを add |
| **Required reviewers** | 自分を追加（複数メンテナならその人達も）|
| **Prevent self-review** | OFF（自分一人なら必須）|
| **Wait timer** | 不要（Required reviewers で十分）|

→ `v0.3.0` のような production タグ push 時、ジョブが「Waiting for review」で停止 → 手動承認後にビルド。

#### `dev-release` (pre-release タグ + dev ブランチ用)

Settings → Environments → New environment → 名前 `dev-release`

| 設定項目 | 値 |
|---|---|
| **Deployment branches and tags** | "Selected branches and tags" → `v*` と `dev` を add |
| **Required reviewers** | 設定しない（自動承認）|

→ `v0.3.0-rc1` のような pre-release タグ や `dev` ブランチ push 時は自動承認、即ビルド。

#### Secrets の配置

Step 4 で **Repository secrets** に登録すれば両方の Environment から自動的に参照できる。
Environment ごとに別 PFX を使いたい場合（dev/prod を別証明書にする等）のみ Environment secrets を使う。

### 6. ローカルの PFX を**安全に削除**

`ghostty-signing.pfx` はもう不要（GitHub Secrets に入っている）。クラウド同期フォルダ等から完全に消す:

```powershell
Remove-Item ghostty-signing.pfx -Force
```

GitHub Secrets を再設定する必要が出たら、手順 1 から証明書を再生成する（同じ Subject を使う限り、ユーザー側の `.cer` 信頼設定もやり直しになる点に注意）。

---

## リリース種別

3 層構成で目的別にビルドが走る。

| 種別 | トリガー | Environment | Release タグ | 用途 |
|---|---|---|---|---|
| **Production** | `v0.3.0` のような hyphen 無しタグ push | `release` (manual approval) | そのタグ | 正式リリース |
| **Pre-release** | `v0.3.0-rc1`, `v0.3.0-beta` など hyphen 付きタグ | `dev-release` (auto) | そのタグ (pre-release マーク) | リリース候補、テスター向け |
| **Dev build** | `dev` ブランチへの push | `dev-release` (auto) | `dev-build` (上書き) | 開発中の最新を試す |

ワークフロー:
- `.github/workflows/release.yml` ← Production と Pre-release を扱う（タグ push がトリガー）
- `.github/workflows/dev-build.yml` ← Dev build を扱う（dev ブランチ push がトリガー）

### Production リリース

```powershell
git tag v0.3.0
git push origin v0.3.0
```

挙動:
1. ghostty fork の `windows-port` から `ghostty.dll` をビルド
2. `Package.appxmanifest` の `Version` をタグから動的書き換え (`v0.3.0` → `0.3.0.0`)
3. **`release` Environment が承認待ち** → Actions タブ → "Review pending deployments" で承認
4. 承認後: PFX を Secrets から復元 → MSIX 署名 → PFX 削除 → Releases にアップロード
5. 成果物: `Ghostty-0.3.0-x64.msix` + `Ghostty.cer`

### Pre-release（RC / Beta）

```powershell
git tag v0.3.0-rc1
git push origin v0.3.0-rc1
```

挙動: Production と同じだが、自動承認 + GitHub Releases で **Pre-release マーク** 付き。
タグ名 / バージョン番号は同じ仕組み (`v0.3.0-rc1` → MSIX manifest は `0.3.0.0`、リリース名は `v0.3.0-rc1`)。

### Dev build

```powershell
git push origin dev
```

挙動:
1. 自動的に `windows-port` の最新 ghostty.dll をビルド
2. MSIX manifest version は `0.3.0.<run_number>` (例: `0.3.0.42`)
3. 自動承認、即ビルド
4. **`dev-build` という固定タグの Release を上書き作成**（前回の dev-build は削除される）
5. URL は固定: `https://github.com/i999rri/GhosttyWin32/releases/tag/dev-build`

`workflow_dispatch` でも手動起動可能（Actions タブ → "Dev Build" → "Run workflow"）。

連続して dev に push した場合、走行中のビルドはキャンセルされて最新の commit のみがビルドされる
(`concurrency: cancel-in-progress`)。

---

## 証明書の更新 / 失効時

PFX が漏洩した疑い、または有効期限が近づいた場合:

1. **古い証明書を失効** （自己署名なので CRL は無いが、新しい証明書で署名し直して周知）
2. 手順 1〜8 を再実行（同じ Subject `CN=i999rri` を再利用）
3. 新しい `Ghostty.cer` を commit / push
4. 旧証明書で署名された MSIX は **再署名できない**（ユーザー側で旧 cer を信頼から外して新 cer を入れ直す必要あり）

タイミング目安:
- 5年有効で生成 → 4年経過時点で切り替えを検討
- 新リリース時には現在の証明書の有効期限を確認

---

## 運用上の注意

- ❌ ログに secret を出さない（`echo $env:PFX_PASSWORD` 等）
- ❌ サードパーティ Action は固定 SHA で pin（現状の `actions/checkout@v4` 等は GitHub 公式なので OK）
- ✅ `release` Environment は Required reviewers を有効化（production タグは手動承認必須）
- ✅ `dev-release` Environment は `v*` と `dev` ブランチに deployment 制限
- ✅ `pull_request_target` トリガーは絶対に追加しない（"pwn request" 脆弱性）
- ✅ Secret Scanning + Push Protection を有効に保つ（PFX うっかり commit ブロック）
- ✅ Secrets 漏洩疑いがあれば即座に PFX を再生成して旧 secret を削除

---

## トラブルシューティング

### `MSIX not found under AppPackages/`
msbuild がエラーを起こしている。`Build MSIX package` step のログを確認。

### `Manifest Validation: Publisher does not match`
証明書の Subject (`CN=i999rri`) と manifest の Publisher が不一致。
`Package.appxmanifest:13` の `Publisher` を確認。

### ユーザーから「インストールできない」と報告
Windows のサイドロードが無効になっているか、`Ghostty.cer` を信頼ストアに入れていない可能性。
Settings → Privacy & Security → For developers → "Developer Mode" もしくは
"Sideload apps" を有効化。
