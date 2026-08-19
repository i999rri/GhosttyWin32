# AddressSanitizer builds

The `ASan` solution configuration is Debug plus `/fsanitize=address`, for local stress verification. The bug classes that have actually bitten this port — use-after-free around config replacement (#133), heap over-reads at the Zig/C boundary (i999rri/ghostty#38) — are runtime lifetime/size bugs. Without instrumentation they only crash when the bad access happens to cross an unmapped page; under ASan the first bad access stops the debugger at the exact allocation, free, and access stacks.

<details>
<summary>日本語</summary>

`ASan` ソリューション構成は Debug + `/fsanitize=address`。ローカルの stress 検証用。この port で実際に踏んできたバグ — config 差し替え周りの use-after-free (#133)、Zig/C 境界の heap over-read (i999rri/ghostty#38) — は実行時の lifetime / サイズ契約のバグで、非計装ビルドでは不正アクセスが未マップページを跨いだときしかクラッシュしない。ASan なら最初の不正アクセスの時点で、確保・解放・アクセスそれぞれのスタック付きでデバッガが止まる。

</details>

## Using it

1. Pick **`ASan | x64`** in the VS configuration dropdown.
2. Build and run as usual — F5 (Package project) for the app, `Tests.exe` / Test Explorer for the tests.

Reports appear on stderr and in the VS Output window; under the debugger, execution breaks at the faulting access with full stacks. ASan is silent while nothing is wrong — no output means no bad access on the paths you exercised.

Outputs live in their own tree (`x64\ASan\`), fully separate from `x64\Debug\`, so switching back and forth is just the dropdown — no Rebuild discipline, and instrumented objects can never mix into a normal build. Expect roughly 2x slowdown and higher memory use, and one full build the first time.

<details>
<summary>日本語</summary>

1. VS の構成ドロップダウンで **`ASan | x64`** を選ぶ。
2. あとはいつも通りビルド・実行 — アプリは F5 (Package プロジェクト)、テストは `Tests.exe` / Test Explorer。

レポートは stderr と VS の出力ウインドウに出る。デバッガ接続中は問題のアクセスの箇所でフルスタック付きでブレークする。ASan は問題がない間は完全に沈黙する — 何も出ない = 実行した経路に不正アクセスがなかった、ということ。

成果物は専用ツリー (`x64\ASan\`) に入り `x64\Debug\` とは完全に分離されるので、切り替えはドロップダウンだけ — Rebuild の運用規律は不要で、計装済み obj が通常ビルドに混ざることは構造的に起きない。速度はおよそ 2 倍遅く、メモリも増える。初回だけフルビルドが走る。

</details>

## What it does / doesn't cover

- Covered: all heap accesses made by our own code (App, Core, Tests) — use-after-free, buffer over/under-flow, double-free.
- Not covered: `ghostty.dll` internals. The DLL is built by Zig and is not instrumented; allocations made inside it are not shadow-tracked. Host-side misuse of pointers the DLL hands out is still caught at the access site.
- Not covered: memory leaks. MSVC's ASan does not support leak detection (LSan); the D3D debug layer's live-object dump at exit is the only leak-ish signal in this build.
- Not covered: data races — that is TSan territory, which MSVC does not ship.

<details>
<summary>日本語</summary>

- カバーされる: 自前コード (App / Core / Tests) の全ヒープアクセス — use-after-free、buffer over/under-flow、double-free。
- カバーされない: `ghostty.dll` の内部。DLL は Zig ビルドで非計装なので、DLL 内部の確保は shadow 追跡されない。ただし DLL が返したポインタをホスト側で誤用した場合は、アクセスの時点で捕まる。
- カバーされない: メモリリーク。MSVC の ASan は leak 検出 (LSan) 非対応。このビルドでリークらしき兆候が見えるのは終了時の D3D debug layer の live-object ダンプだけ。
- カバーされない: データ競合。それは TSan の領分で、MSVC には無い。

</details>

## How it's wired

- `Directory.Build.props` sets `EnableASAN` for the `ASan` configuration — the single place that defines what the configuration means. `Directory.Build.targets` removes `/RTC1` and forces `/Zi`, which conflict with ASan.
- The projects declare `ASan|x64` as a twin of `Debug|x64` (or-conditions on the Debug groups), so ASan inherits Debug semantics (`/MDd`, `_DEBUG`, asserts) automatically.
- The ASan runtime DLL (`clang_rt.asan_dynamic-x86_64.dll`) is deployed explicitly: as MSIX content in `App.vcxproj`, and by a copy target in `Tests.vcxproj` — the build does not put it next to the EXE on its own, and without it the EXE dies at startup (Test Explorer discovery then shows an empty list with no error).
- `Tests.vcxproj` hand-wires the Debug gtest libs because the googletest package targets pick the flavor by configuration name (`'Debug'` vs everything else) and would inject the Release ones.
- `Package.wapproj` keys the production appxmanifest on `Release` only, so ASan F5 runs use the `.Dev` identity like Debug does.
- Release and CI are untouched; the ASan configuration is never built there.

<details>
<summary>日本語</summary>

- `Directory.Build.props` が `ASan` 構成に `EnableASAN` を設定する — 構成の意味を定義する唯一の場所。`Directory.Build.targets` は ASan と衝突する `/RTC1` を外し `/Zi` を強制する。
- 各プロジェクトは `ASan|x64` を `Debug|x64` の双子として宣言 (Debug グループへの or 条件) しているので、Debug の意味論 (`/MDd`、`_DEBUG`、assert 有効) を自動で引き継ぐ。
- ASan ランタイム DLL (`clang_rt.asan_dynamic-x86_64.dll`) は明示的に配置している: `App.vcxproj` では MSIX content として、`Tests.vcxproj` では copy target で。ビルドは自動では EXE の隣に置いてくれず、ないと EXE が起動時に死ぬ (そのとき Test Explorer の discovery はエラーなしの空一覧になる)。
- `Tests.vcxproj` は Debug 版 gtest を手動配線している。googletest package の targets は構成名 (`'Debug'` かそれ以外か) で lib を選ぶため、ASan 構成には Release 版が注入されてしまうから。
- `Package.wapproj` は production appxmanifest を `Release` のみに限定したので、ASan の F5 実行は Debug と同じ `.Dev` identity を使う。
- Release と CI には一切影響しない。ASan 構成がそこでビルドされることはない。

</details>
