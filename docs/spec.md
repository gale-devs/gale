# Gale 言語仕様(草案)

この文書は設計メモをまとめた草案で、実装に合わせて更新する。

## 1. 概要

- 小さく、明示的で、式中心
- 手動メモリ管理
- ファイル・ディレクトリ・struct を同じ入れ子の木として扱う

## 2. ハローワールド

```
unfold std.console

export func main() -> void = {
    std.console.println("Hello, World!")
}
```

## 3. struct とファイル

- ファイル自体が struct。struct の中に struct を入れられる
- ディレクトリも struct。`std/console.gale` は `struct std { struct console { ... } }` に当たる
- `std.console` は「`std` というファイル(ディレクトリ)struct の `console` struct」

## 4. unfold

- `unfold a.b` はコンパイル前に、`a` の `b` だけをパスを保ったまま展開する
  - 展開結果は `struct a { struct b { ... } }` の形になる
- 同名の struct はマージされる
- マージ時に同名の func やフィールドが重複したらコンパイルエラー
- 循環 unfold はエラー
- unfold は再公開されない。再公開したいときだけ `export unfold` と書く
- 複数の枝をまとめて書く `unfold std.{console, math}` は検討中

## 5. 可視性

- `export` ありは公開
- `export` なしは、その struct の内部だけから見える
- unfold しても `export` の有無は引き継ぐ

## 6. 式

- 式中心。ブロック・`if`・`match` は値を返す。ブロックの最後の式が値になる
- 関数の本体は式で、`=` で結ぶ

```
func square(x: i32) -> i32 = x * x
func abs(x: i32) -> i32 = if x < 0 { -x } else { x }
```

## 7. 型

- 整数: `i8 i16 i32 i64 isize` / `u8 u16 u32 u64 usize`
- 浮動小数: `f32 f64`
- そのほか: `bool` `void`
- ポインタ `*T`、スライス `[]T`、配列 `[N]T`、null を許す型 `?T`
- 暗黙の型変換はなし。`as` で明示する
- struct は `struct 名 = { フィールド }` と書く

## 8. 変数

- `let` は不変、`var` は可変

## 9. メモリ管理

- アロケータを明示的に渡す。隠れたグローバル確保はなし
- `defer` でスコープ終了時に実行する(逆順)
- `a.new(値)` / `a.free(p)` / `a.alloc(T, n)`

```
let a = std.mem.heap
let p = a.new(Point { x: 3.0, y: 4.0 })
defer a.free(p)
```

## 10. エラー

- 例外はなし。失敗しうる関数は `-> T!` を返し、呼び出し側は `try` で伝播する

## 11. 字句

- 空白と改行は区切りにだけ使う。各トークンには「直前に改行があったか」を記録する
- コメント: `// 行コメント`、`/* ブロックコメント */`(入れ子なし)
- 整数: `42` `1_000` `0xFF` `0b1010`
- 浮動小数: `3.14` `1e10` `2.5e-3`
- 文字列: `"..."`、エスケープは `\n \r \t \0 \\ \" \'`
- キーワード: `unfold export func struct let var if else while defer return try as true false`

## 12. パッケージとツール

- コマンドは `gale` の1本: `new / build / run / test / fmt / add`
- 設定は `gale.toml`、ロックは `gale.lock`(ハッシュ付き)
- 依存は Git の URL とタグ(またはコミット)で指定。中央レジストリは作らない

```
[package]
name = "myapp"
version = "0.1.0"

[deps]
json = { git = "https://github.com/x/json", tag = "v1.2.0" }
```

## 13. 標準ライブラリ

- `std.core`: 基本型、`?T`、スライス、数学。アロケータ不要
- `std.mem`: アロケータ(`heap` `arena` `fixed`)
- `std.*`: `console` `fs` `os` `str` `list` `map` `test`。確保が要るものはアロケータを引数で受け取る

## 14. コンパイラ

```
.gale → 字句解析 → 構文解析 → unfold展開 → 型検査
      → IR → 独自アセンブリ → エンコード → ELF / PE
```

- 実装は C
- 外部のアセンブラ・リンカは使わない。ELF と PE を直接書き出す
- 最小バイナリを重視する(libc なし、ELF はセクションヘッダなし、未使用関数は出力しない)

## 15. セルフホスティング

1. stage0: C 製のコンパイラ(小さなサブセットを実装)
2. stage1: コンパイラ自身を Gale で書き、stage0 でコンパイルする
3. stage2: stage1 で自分自身をコンパイルし、出力が一致すれば完了
