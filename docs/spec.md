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

`unfold a.b.c` は、コンパイル前に `a.b.c` の中身を今いる struct の中へ、**パスを保ったまま**展開する。
展開結果は `struct a { struct b { struct c { ... } } }` の形になる。

### ファイルの探し方

- 検索ディレクトリ(ルート)を順に見て、`a.b.c` を次の順で探す。最初に見つかったファイルを使う
  1. `<root>/a.gale`
  2. `<root>/a/b.gale`
  3. `<root>/a/b/c.gale`
- ファイルが見つかったら、残りのパスはそのファイルの中の struct か func を指す
  - 例: `unfold lib.math.add` は `lib/math.gale` の中の `add` だけを取り込む
- ファイル全体を指したときは、ファイルが「パスの最後の名前の struct」になる
- ディレクトリそのもの(`unfold lib`)はまだ展開できない
- ルートは、`gale expand` では「そのファイルのあるディレクトリ」と `-I` で指定したディレクトリ(この順)

### 展開とマージ

- 展開結果は、今いる struct にマージされる。同名の struct は合流する
  - 同名の func やフィールドが重複したらエラー。func と struct が同名でもエラー
- 展開するファイルの中にある `unfold` は、先にそのファイルの中で展開される
  (展開結果はそのファイルの struct の中に入る)
- 同じものを同じ struct に2回 unfold しても、2回目は無視される
- 循環 unfold(a → b → a)や、自分自身の unfold はエラー(展開の連鎖を表示する)。展開の深さは 64 まで
- `unfold` は struct の中にも書ける(その struct の中に展開される)
- 複数の枝をまとめて書く `unfold std.{console, math}` は検討中

### export

- 展開で作られる struct(`a`、`b`、ファイル全体の `c`)は、`unfold` に `export` が付いているときだけ公開になる
  - unfold しただけでは再公開されない。再公開したいときは `export unfold`
- 元のファイルの中の `export` の有無は、そのまま引き継ぐ

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

## 16. 文法(パーサの実装に対応)

```
file     = { member }                       ※ファイル直下にフィールドは置けない
member   = field | item
field    = IDENT ":" type
item     = [ "export" ] ( unfold | func | struct )
unfold   = "unfold" path
func     = "func" IDENT "(" [ param { "," param } [ "," ] ] ")" [ "->" type ] "=" expr
param    = IDENT ":" type
struct   = "struct" IDENT "=" "{" { member } "}"
path     = IDENT { "." IDENT }

type     = prefix [ "!" ]                   ※ "!" は同じ行になければならない
prefix   = "*" prefix | "?" prefix | "[" "]" prefix | "[" expr "]" prefix | path

expr     = binary [ "=" expr ]              ※代入は右結合
binary   = cast { binop cast }              ※二項演算子は左結合
cast     = unary { "as" prefix }
unary    = ( "-" | "!" | "~" | "*" | "&" | "try" ) unary | postfix
postfix  = primary { "(" [ expr { "," expr } [ "," ] ] ")" | "[" expr "]" | "." IDENT | structlit }
primary  = INT | FLOAT | STRING | "true" | "false" | IDENT | "(" expr ")" | block | if | while
structlit= "{" [ IDENT ":" expr { "," IDENT ":" expr } [ "," ] ] "}"   ※path の直後のみ
block    = "{" { stmt } [ expr ] "}"        ※最後の式(`;` なし)がブロックの値
stmt     = ( "let" | "var" ) IDENT [ ":" type ] "=" expr
         | "defer" expr | "return" [ expr ] | expr | ";"
if       = "if" expr block [ "else" ( if | block ) ]
while    = "while" expr block
```

二項演算子の優先順位(低い順):

| 優先順位 | 演算子 |
|---|---|
| 1 | `\|\|` |
| 2 | `&&` |
| 3 | `==` `!=` `<` `<=` `>` `>=` |
| 4 | `\|` |
| 5 | `^` |
| 6 | `&` |
| 7 | `<<` `>>` |
| 8 | `+` `-` |
| 9 | `*` `/` `%` |

`as` は二項演算子より強く、単項演算子より弱い(`-a as u8` は `(-a) as u8`)。

### 改行と区切り

- メンバ(フィールド・項目)と文は、`,` か `;` か**改行**で区切る。`}` の直前は区切り不要
- 二項演算子・`=`・`as`・後置の `(` `[` `.` は、**左の式と同じ行になければならない**。
  行末に演算子を置いて次の行へ続けるのはよいが、行頭に演算子を置くと新しい文として扱われる
  (`a` の次の行の `-b` や `(b)` は、前の行の続きにならない)
- `else` は前の `}` と別の行に置いてもよい
- `return` は、同じ行に値がなければ値なしの `return` になる

### struct リテラルと条件式

- `Name { field: value }` は、`Name` が名前のパス(`a.b.C`)で、`{` の次が `}` か `名前 :` のときだけ struct リテラル
- `if` と `while` の条件式の中では struct リテラルは無効(`if x { ... }` の `{` が本体になる)。
  条件の中でリテラルを使うときは括弧で囲む: `if (P { x: 1 }) == q { ... }`

### 注意点

- `-> T!` の後ろの `=` とは空白を空ける。`T!=` は `!=` として読まれるため、エラーになる
- 戻り値型を省略すると `void`
- ネストは 256 段、`a+b+c...` や `a.b.c...` の連なりは 10000 個まで

### 構文木のダンプ

`gale parse <file>` は、構文木を S 式で表示する(テストでも使う)。

```
(unfold std.console)
(export func main () void (block (tail (call (. (. std console) println) "Hello, World!"))))
```
