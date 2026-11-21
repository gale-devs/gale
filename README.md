# Gale

小さく、明示的で、式中心のシステムプログラミング言語。

- ファイルもディレクトリも struct。`unfold` でパスを保ったまま必要な枝だけを展開する
- 式中心(ブロック・`if` も値を返す)
- 手動メモリ管理(アロケータを明示的に渡す、`defer` で解放)
- コンパイラは C 製。Gale → 独自アセンブリ → 機械語 → ELF / PE を直接出力する(リンカ・外部アセンブラ・libc 不要)
- 最小バイナリを重視。最終目標はセルフホスティング

```
unfold std.console

export func main() -> void = {
    std.console.println("Hello, World!")
}
```

言語仕様の草案は [docs/spec.md](docs/spec.md) を参照。

## ビルド

必要なもの: C99 対応のコンパイラ(gcc / clang / MSVC)。

- Linux / MSYS2(MinGW-w64): GNU make
- Windows(MSVC): Developer Command Prompt で nmake

```
make          # build/gale (Windows は build/gale.exe) を作る
make test     # 各段階のテストと、サンプルの字句解析・構文解析・型検査を実行
make clean
```

MSVC の場合:

```
nmake /nologo /f Makefile.msvc
nmake /nologo /f Makefile.msvc test
nmake /nologo /f Makefile.msvc clean
```

## 使い方

```
gale lex examples/hello.gale     # トークン列を表示
gale parse examples/hello.gale   # 構文木(S式)を表示
gale expand -I lib app.gale      # unfold を展開した構文木を表示
gale check -I lib app.gale       # unfold を展開して型検査
gale asm app.gale                # 生成した独自アセンブリを表示
gale build app.gale -o app       # 実行ファイルを作る(Linux は ELF、Windows は PE)
gale build --target windows app.gale -o app.exe
gale --version
```

## 状況

| 段階 | 状態 |
|------|------|
| 字句解析 | 実装済み |
| 構文解析 | 実装済み(AST と `gale parse`) |
| `unfold` 展開 | 実装済み(`gale expand`) |
| 型検査 | 実装済み(`gale check`、仕様は [spec §17](docs/spec.md)) |
| 独自アセンブリ | 実装済み(x86-64、`gale asm`) |
| 機械語・ELF / PE 出力 | 実装済み(`gale build`、仕様は [spec §18](docs/spec.md)) |
| 標準ライブラリ・小数・文字列 | 未着手 |
| セルフホスティング | 未着手 |

## CI

GitHub Actions で push / PR ごとに、次の環境でビルドとテストを実行する。

- Linux (ubuntu-latest): gcc と clang
- Windows (windows-latest): MSYS2 / MinGW-w64 gcc と、MSVC (nmake)

`make test` は、コンパイルしたプログラムを各 OS で実際に実行して、終了コードを確かめる(Linux では ELF、Windows では PE)。
