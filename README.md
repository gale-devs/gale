# Gale

小さく、明示的で、式中心のシステムプログラミング言語。

- ファイルもディレクトリも struct。`unfold` でパスを保ったまま必要な枝だけを展開する
- 式中心(ブロック・`if` も値を返す)
- 手動メモリ管理(アロケータを明示的に渡す、`defer` で解放)
- コンパイラは C 製。Gale → 独自アセンブリ → ELF / PE を直接出力する(リンカ・外部アセンブラ不要)
- 最小バイナリを重視。最終目標はセルフホスティング

```
unfold std.console

export func main() -> void = {
    std.console.println("Hello, World!")
}
```

言語仕様の草案は [docs/spec.md](docs/spec.md) を参照。

## ビルド

必要なもの: C99 対応のコンパイラ(gcc / clang)と GNU make。
Windows では MSYS2 の MinGW-w64 gcc を使う。

```
make          # build/gale (Windows は build/gale.exe) を作る
make test     # 字句解析器のテストとサンプルの字句解析を実行
make clean
```

## 使い方

```
gale lex examples/hello.gale   # トークン列を表示
gale --version
```

## 状況

| 段階 | 状態 |
|------|------|
| 字句解析 | 実装済み |
| 構文解析 | 未着手 |
| `unfold` 展開 | 未着手 |
| 型検査 | 未着手 |
| IR / 独自アセンブリ | 未着手 |
| ELF / PE 出力 | 未着手 |
| セルフホスティング | 未着手 |

## CI

GitHub Actions で push / PR ごとに、次の環境でビルドとテストを実行する。

- Linux (ubuntu-latest): gcc と clang
- Windows (windows-latest): MSYS2 / MinGW-w64 gcc

ELF / PE の出力ができた段階で、生成した実行ファイルを各 OS 上で実際に動かすテストを追加する。
