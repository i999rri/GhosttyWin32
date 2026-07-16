# リリース手順

GhosttyWin32 のリリースは GitHub Actions で MSIX をビルド・自己署名 → GitHub Releases にアップロードする。
正式な CA / OSS Foundation 署名が取得できるまで、Scoop チャネルは停止中 ([#46](https://github.com/i999rri/GhosttyWin32/issues/46))、配布は手動インストールのみ。

## ブランチとリリースの全体像

### ブランチの役割

| ブランチ | 役割 |
|---|---|
| `feature/*`, `fix/*`, `ci/*` | 個別の作業ブランチ。`pull_request` で `ci.yml` が compile-only ビルド検証する。merge までユーザには届かない。 |
| `dev` | **デフォルトブランチ**。PR の merge 先。ここに乗っている = 次のリリースに含める意思表示。push のたびに `dev-build.yml` が走り、`dev-build` Release が上書きされる。 |
| `main` | リリース履歴。正式版のタグはここに打つ。`dev` から **release PR** 経由で反映する。 |

### リリースの単位 = タグ

タグを打った瞬間がリリースイベント。**タグ名・PR タイトル・GitHub Release** の 3者が一致する三位一体。

| リリースイベント | タグ起点 | タグ例 |
|---|---|---|
| RC (リハーサル / テスター向け) | **`dev` HEAD** | `v0.3.0-rc1`, `v0.3.0-rc2` |
| 正式版 | **`main` HEAD** (release PR マージ後) | `v0.3.0` |

非対称な理由:
- RC = dev のスナップショットを試すだけ。main を触らないので気軽に何度でも切れる。
- 正式版 = main を進めるリリース。release PR の review を通す。

`release.yml` はタグ名から自動分岐 (`-` 付きは pre-release、無しは production)。タグがどのブランチで打たれたかは workflow 側は気にしない。

### リリース計画

事前に「v0.3.0 には機能 A, B, C を入れる」と硬く計画しない。代わりに:

- 各機能は feature branch で開発
- 進捗が良ければ PR で **`dev` にマージ** (= 「次のリリースに含める」と意思表示)
- ある程度たまったタイミングで RC → 正式版

「途中で機能を抜きたい」場合は、該当 feature の squash commit を `git revert` する (1 PR = 1 squash commit 運用なので 1 commit で剥がせる)。

### RC を出す判断

- ✅ dev に変更がある程度積まれた、大きな新機能を入れた直後はリハーサル兼ドッグフードで RC
- ❌ 小さい修正リリース (v0.3.1 等) は RC スキップして正式版直行も可

### RC2 以降を出す典型ケース

RC1 と RC2 (以降) は **機能セットを変えない**。RC2 にしていいのは:

- ✅ クリティカルバグ修正 (クラッシュ、データ破壊、起動不能)
- ✅ パイプライン問題の修正 (署名エラー、MSIX 構造問題)
- ✅ 依存の重大バグ修正 (RC1 後に発覚した security fix 等)
- ✅ パフォーマンス退行の修正

RC2 で **やってはいけない** こと:

- ❌ 新機能の追加 (= 機能セット変わる、リハーサルにならない)
- ❌ 大きいリファクタ (バグ持ち込みリスク)
- ❌ 「ついで」の修正

軽微な不具合は **「known issue として release notes に書いて正式版で出荷」** の判断もある。RC が 4 回以上続いたら「リリース計画ミスった」「機能凍結が緩かった」のサイン。

### dev → main の release PR

正式版を出す時の手順:

```powershell
# 1. release PR を作成 (タイトルはタグと一致させる)
gh pr create --base main --head dev --title "Release v0.3.0" --body "(変更点サマリ)"

# 2. レビュー → squash merge

# 3. ローカルの main を更新してタグ
git checkout main
git pull
git tag v0.3.0
git push origin v0.3.0
```

タグ push で `release.yml` が走って GitHub Release 完成。

### release PR を開いた後の運用ルール

**release PR は最終確認のフェーズ**。一度開いたら基本的に内容を変更しない:

- ✅ そのままレビュー → マージ → タグ → リリース
- ❌ 「dev に新しい修正が入ったから release branch に取り込む」みたいな後付けは NG (RC で検証してない変更が混ざる)

リリース後にバグが見つかった場合は **次バージョンの patch bump** で対応:

```
v0.3.0 公開 → バグ報告
   ↓
dev に fix
   ↓
v0.3.1-rc1 タグ (任意、軽微な修正なら RC スキップ可)
   ↓
release PR (release/v0.3.1)
   ↓
merge → 自動 tag v0.3.1 → 公開
```

このルールにより:
- release PR の中身 = リリースされる中身 が常に一致
- 各バージョンが「その時点で released な内容」と 1:1 で対応 (バージョン番号で何が入ってるか辿れる)
- 「PR 開いた後に dev が動いて branch を更新する」みたいな複雑な運用が発生しない

なお release PR open 中も **dev は普通に動かして OK**。release branch は dev の snapshot を取った時点で固定されるので、後から dev に入った修正の影響を受けない。次の patch bump (例: 0.3.1) のための fix を dev に積んだり、`v0.3.1-rc1` タグを並行で切ったりも自由。リリースサイクル間に depend がないので、v0.3.0 リリース直後に v0.3.1 をすぐ出せる構成にできる。

### semver の bump 指針

| 変更内容 | bump | 例 |
|---|---|---|
| バグ修正のみ | patch | 0.3.0 → 0.3.1 |
| 新機能 (互換性維持) | minor | 0.3.0 → 0.4.0 |
| 互換性破壊 | major | 0.3.0 → 1.0.0 |

`0.x.y` 期間は major bump 控えめ、minor で機能追加、patch で修正、が慣例。

---

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

3 層構成で目的別にビルドが走る。加えて PR 検証用に CI が走る。

| 種別 | トリガー | Environment | Release タグ | 用途 |
|---|---|---|---|---|
| **Production** | `v0.3.0` のような hyphen 無しタグ push | `release` (manual approval) | そのタグ | 正式リリース |
| **Pre-release** | `v0.3.0-rc1`, `v0.3.0-beta` など hyphen 付きタグ | `dev-release` (auto) | そのタグ (pre-release マーク) | リリース候補、テスター向け |
| **Dev build** | `dev` ブランチへの push | `dev-release` (auto) | `dev-build` (上書き) | 開発中の最新を試す |
| **CI** | 全 PR (`pull_request` イベント) | (なし) | (Release 作らない) | コンパイル検証のみ、PFX 不要 |

ワークフロー:
- `.github/workflows/release.yml` ← Production と Pre-release (タグ push がトリガー)
- `.github/workflows/dev-build.yml` ← Dev build (dev ブランチ push がトリガー)
- `.github/workflows/ci.yml` ← CI (PR 検証、composite を sign='false' で呼ぶ)
- `.github/actions/build-signed-msix/action.yml` ← 上 3 つが共有する composite action (zig build → ghostty.dll → NuGet restore → manifest patch → 署名 (任意) → MSIX)

### Production リリース

dev → main の release PR を経由する (詳細は冒頭「dev → main の release PR」セクション参照)。
PR がマージされた後、main HEAD にタグを打って push:

```powershell
git checkout main
git pull
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

main を更新する必要なし。**dev の HEAD にタグを直接打つ**:

```powershell
git checkout dev
git pull
git tag v0.3.0-rc1
git push origin v0.3.0-rc1
```

挙動: Production と同じビルドパスだが、自動承認 + GitHub Releases で **Pre-release マーク** 付き。
タグ名 / バージョン番号は同じ仕組み (`v0.3.0-rc1` → MSIX manifest は `0.3.0.0`、リリース名は `v0.3.0-rc1`)。

**MSIX 成果物のファイル名は production と RC で異なる** (assets 一覧で見分けられるように):

| タグ | 成果物ファイル名 |
|---|---|
| `v0.3.0` (production) | `Ghostty-0.3.0-x64.msix` |
| `v0.3.0-rc1` (RC) | `Ghostty-v0.3.0-rc1-x64.msix` |

(Scoop チャネル復帰時は、autoupdate URL pattern で `Ghostty-$version-x64.msix` (production) / `Ghostty-v$version-x64.msix` (RC) を吸収させる。)

RC2 以降が必要になった場合は、dev に修正コミットを積んでから新タグ:

```powershell
git checkout dev
git pull   # 修正コミットを取り込む
git tag v0.3.0-rc2
git push origin v0.3.0-rc2
```

何を入れる/入れないかの判断基準は冒頭「RC2 以降を出す典型ケース」を参照。

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

### `0x80073cfb` — 「パッケージ化されていないバージョンを既にインストールしています」
Visual Studio で F5 / Ctrl+F5 デバッグ実行すると `Add-AppxPackage -Register` 経由で
loose-files 登録される。これが残っている状態で MSIX を入れようとすると
同じ Identity の「unpackaged」と「packaged」が衝突してこのエラーになる。

```powershell
# 既存の登録を確認 (パッケージ Name または Publisher で)
Get-AppxPackage | Where-Object { $_.Publisher -like "*i999rri*" }

# 削除
Get-AppxPackage -Name "<NameFromAbove>" | Remove-AppxPackage

# それでも残るなら -PreserveApplicationData 付きや -AllUsers (admin) で
Get-AppxPackage -Name "<NameFromAbove>" -AllUsers | Remove-AppxPackage -AllUsers
```

削除後に MSIX を再インストール。リリーステスト前に `Get-AppxPackage` で
loose 登録が残ってないか確認するとハマらない。
