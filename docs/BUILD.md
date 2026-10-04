# Building from source

Two builds, in this order: `zig` produces the libghostty DLL, then Visual Studio produces the app around it. The second does not run the first, so a change inside the submodule is invisible until you repeat step 2.

<details>
<summary>日本語</summary>

ビルドは 2 段で、この順に行う。`zig` が libghostty の DLL を作り、そのあと Visual Studio がそれを囲むアプリを作る。2 番目は 1 番目を呼ばないので、submodule の中を変えても step 2 をやり直すまで反映されない。

</details>

## 1. Prerequisites

- Visual Studio 2022 (17.10 or newer) with **Desktop development with C++** and **Universal Windows Platform development**
- Windows SDK 10.0.22621.0 or newer
- Zig 0.16.0
- Windows App SDK — comes in through the projects' NuGet packages

<details>
<summary>日本語</summary>

- Visual Studio 2022 (17.10 以降)。**C++ によるデスクトップ開発** と **ユニバーサル Windows プラットフォーム開発** のワークロード
- Windows SDK 10.0.22621.0 以降
- Zig 0.16.0
- Windows App SDK — プロジェクトの NuGet パッケージ経由で入る

</details>

## 2. Build ghostty.dll

The fork lives as a submodule under `external/ghostty/`, pinned to a commit of its `windows-port` branch. On a fresh clone:

```bash
git submodule update --init --recursive
```

Then:

```bash
cd external/ghostty
zig build -Doptimize=ReleaseSafe -Drenderer=directx
```

`-Drenderer=directx` is not optional here, and the Zig version is exact. The result is `external/ghostty/zig-out/lib/ghostty-internal.dll`, which `App/App.vcxproj` deploys as `ghostty.dll`, together with the import library beside it. Both the app and the tests read the C API from `external/ghostty/include`, so the submodule pin is the single source of truth for the header and the binary at once.

**After the pin moves**, this step is required and `git submodule update --init` is not the way to take it: it re-clones and leaves a tree that rebuilds from scratch. Check the recorded commit out instead, then build.

```bash
git -C external/ghostty fetch origin windows-port
git -C external/ghostty checkout <the commit the parent records>
```

<details>
<summary>日本語</summary>

fork は `external/ghostty/` の submodule で、`windows-port` ブランチのあるコミットに固定してある。clone 直後は:

```bash
git submodule update --init --recursive
```

そのあと:

```bash
cd external/ghostty
zig build -Doptimize=ReleaseSafe -Drenderer=directx
```

`-Drenderer=directx` は省略できず、Zig のバージョンもこの値でないといけない。できるのは `external/ghostty/zig-out/lib/ghostty-internal.dll` で、`App/App.vcxproj` がこれを `ghostty.dll` としてデプロイする。import ライブラリも隣に出る。アプリとテストはどちらも C API を `external/ghostty/include` から読むので、ヘッダとバイナリの出どころが submodule の pin 1 つに揃う。

**pin が動いたあと**はこの step が必須で、そのときに `git submodule update --init` を使ってはいけない。再 clone になり、ゼロからビルドし直す tree が残る。記録されているコミットを checkout してからビルドする。

```bash
git -C external/ghostty fetch origin windows-port
git -C external/ghostty checkout <親リポジトリが記録しているコミット>
```

</details>

## 3. Build the app

Open `GhosttyWin32.slnx`, pick **Release | x64**, and build `App`. F5 deploys it as a packaged MSIX into the local appx registry; a plain build leaves its artifacts under `x64/Release/App/`.

The other configurations: **Debug | x64** for the debugger, and **ASan | x64** for AddressSanitizer — see [docs/ASAN.md](ASAN.md).

<details>
<summary>日本語</summary>

`GhosttyWin32.slnx` を開き、**Release | x64** を選んで `App` をビルドする。F5 ならパッケージ版 MSIX としてローカルの appx レジストリに配置され、普通のビルドなら成果物は `x64/Release/App/` に出る。

他の構成はデバッガ用の **Debug | x64** と、AddressSanitizer 用の **ASan | x64**。後者は [docs/ASAN.md](ASAN.md) を参照。

</details>

## Trying another ghostty branch

Switch inside the submodule and repeat step 2. The parent repository only notices when you `git add external/ghostty`, so experimenting costs nothing; `git submodule update --recursive` snaps back to the recorded pin.

```bash
cd external/ghostty
git switch <branch>
zig build -Doptimize=ReleaseSafe -Drenderer=directx
```

<details>
<summary>日本語</summary>

submodule の中でブランチを切り替えて step 2 をやり直す。親リポジトリが気づくのは `git add external/ghostty` したときだけなので、試すのは自由。`git submodule update --recursive` で記録された pin に戻る。

```bash
cd external/ghostty
git switch <ブランチ>
zig build -Doptimize=ReleaseSafe -Drenderer=directx
```

</details>

## Running the tests

The `Tests` project is a gtest binary that links the same `Core` static library the app does, so it covers the parts that do not need WinRT running. Build and run it from Test Explorer, or from the build output directly.

<details>
<summary>日本語</summary>

`Tests` プロジェクトは gtest のバイナリで、アプリと同じ `Core` スタティックライブラリをリンクする。WinRT を動かさずに済む部分を担当する。テストエクスプローラーから、あるいはビルド出力を直接実行する。

</details>
