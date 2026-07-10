# BerryBasiC Language Reference

BerryBasiC is a small, BBC-flavoured BASIC interpreter that runs on a Raspberry Pi 4 (and in a QEMU emulator on your desktop). It is a *tokenless* interpreter: you type a program as numbered lines, and it runs the source directly. If you have ever used BBC BASIC, Acorn BASIC, or one of the 8‑bit home‑computer BASICs, most of this will feel familiar; if you are new to BASIC entirely, this manual is written to be read start to finish.

At a glance, BerryBasiC supports:

- Line‑numbered programs **and** direct‑mode commands typed at the prompt
- Floating‑point, integer (`%`) and string (`$`) variables, plus arrays of up to three dimensions
- Named procedures (`PROC`) and functions (`FN`), local variables, and recursion
- Structured control flow: `IF`/`THEN`/`ELSE`/`ENDIF`, `FOR`, `REPEAT`, `WHILE`, `CASE`
- `DATA`/`READ`/`RESTORE` for built‑in constant tables
- Reusable code libraries via `IMPORT`
- Console I/O, a USB keyboard and mouse, and a centisecond clock
- Files on the SD card, opened as channels for byte‑ or record‑level I/O, plus a full set of directory commands
- Graphics: BBC‑style `PLOT`/`MOVE`/`DRAW`, a high‑level shape library, truecolour (24‑bit) drawing, sprites, and PNG/JPEG/BMP image loading and saving
- `VDU` commands for fine screen control, user‑defined characters, viewports and palettes
- Reserved memory blocks read and written with the `?` / `!` / `$` indirection operators
- **Native "seeds"** — compiled AArch64 machine code, loaded from the card and called from BASIC, for the parts of a job that need full native speed

Throughout this manual, code you type or that appears in a program is shown in a monospaced block:

```basic
10 PRINT "HELLO, WORLD"
20 GOTO 10
```

Text the machine prints back is shown as plain output:

```text
HELLO, WORLD
HELLO, WORLD
...
```

---

# Getting Started

## The prompt

When BerryBasiC starts it prints a banner and then a `>` prompt:

```text
BerryBasic (C) 2026 fritzone

>
```

The `>` is where you type. Everything you enter is one of two things:

- **A numbered line** — it is *stored* as part of your program and not run yet.
- **Anything else** — it is a *direct‑mode command*, run immediately.

## Direct mode

Type a statement with no line number and press Return, and it runs at once:

```basic
>PRINT 2 + 2
4
>PRINT "Hi "; "there"
Hi there
```

Direct mode is handy for quick sums, for inspecting variables, and for the commands that manage your program (`LIST`, `RUN`, `SAVE`, `NEW`, and so on). Most statements work in direct mode exactly as they do in a program; the few that only make sense inside a running program (such as `FOR`/`NEXT` spanning several lines, or `RETURN`) will say so.

## Writing a program

Give a line a number and it is remembered instead of run:

```basic
>10 PRINT "HELLO"
>20 PRINT "AGAIN"
>RUN
HELLO
AGAIN
>
```

Lines are always kept sorted by number, however you type them, so you can enter them in any order and insert new ones later by choosing a number in between (this is why programs are traditionally numbered 10, 20, 30 — it leaves room to squeeze `15` in). `LIST` shows the program in order; `RUN` executes it from the lowest line number.

## Editing and deleting lines

- **Replace a line:** type it again with the same number. The new text replaces the old.
- **Delete a line:** type its number alone and press Return.
- **Edit a line in place:** `EDIT 20` recalls line 20 into the input so you can change it (see *Program Control Commands*).

```basic
>20 PRINT "CHANGED"      : REM replaces line 20
>20                      : REM deletes line 20
```

## A first program

```basic
10 REM My first BerryBasiC program
20 INPUT "What is your name"; NAME$
30 PRINT "Hello, "; NAME$; "!"
40 FOR I = 1 TO 3
50   PRINT "Beep "; I
60 NEXT I
70 END
```

Type `RUN` to run it, `LIST` to see it, and `SAVE "HELLO"` to keep it on the card (it becomes `HELLO.BAS`). `LOAD "HELLO"` brings it back.

---

# Program Structure

A program is a list of numbered lines. Each line holds one or more statements.

```basic
10 PRINT "HELLO"
20 GOTO 10
```

## Several statements on one line

A colon `:` separates statements, so a single line can do several things:

```basic
10 A = 10 : B = 20 : PRINT A + B
```

Execution runs left to right along the line, then moves to the next line.

## Comments

`REM` (remark) turns the rest of the line into a comment — the interpreter ignores everything after it:

```basic
10 REM This whole line is a note
20 A = 5 : REM ...and so is this, after the colon
```

Because `REM` swallows the rest of the line, put it last.

## Line length and program size

A single line may be up to 128 characters. A program may hold up to 8192 lines. (See *Appendix B: Limits* for the full list.)

---

# Data Types

There are exactly two kinds of value in BerryBasiC: **numbers** and **strings**. Every variable holds one or the other, and its *name* decides which.

A variable name starts with a letter and may contain letters and digits (and the underscore `_`). A trailing `%` or `$` is part of the name — and it counts towards the length. Names are up to 8 characters including that suffix, so keep to seven letters plus a suffix to be safe. Names are not case‑sensitive: `Count`, `COUNT` and `count` are the same variable (they are all folded to upper case internally).

| Name ends in… | Holds | Example |
|---------------|-------|---------|
| *(nothing)*   | a floating‑point number | `RADIUS`, `X`, `TOTAL` |
| `%`           | a whole number (integer) | `COUNT%`, `I%` |
| `$`           | text (a string) | `NAME$`, `LINE$` |

A variable springs into existence the first time you assign to it; there is no separate "declare" step (arrays are the exception — see *Arrays*). A numeric variable that has never been assigned reads as `0`; an unassigned string reads as `""`.

## Floating point (the default)

A plain name, with no suffix, is a floating‑point variable. Numbers are held as **double‑precision** IEEE‑754 floating point, so both fractions and large magnitudes are kept accurately (about 15–16 significant decimal digits internally).

```basic
A = 12.5
B = 3.14159
RADIUS = 2.5
AREA = PI * RADIUS * RADIUS
```

## Integer variables (`%`)

A name ending in `%` holds a whole number. Whenever you assign to it, the value is **truncated towards zero** — the fractional part is simply dropped (this is truncation, not rounding):

```basic
I% = 12.9
PRINT I%
```

```text
12
```

Negative values truncate towards zero as well, so `I% = -12.9` gives `-12`. Integer variables make good loop counters and array indices, they run a touch faster, and they document intent ("this is a count"). Internally they behave as 32‑bit integers, which matters for the bitwise operators.

## String variables (`$`)

A name ending in `$` holds text. A string may be from 0 up to **255** characters long. The empty string is written `""`.

```basic
NAME$ = "BERRY"
EMPTY$ = ""
```

Join strings with `+`, and take them apart with the string functions (`LEFT$`, `RIGHT$`, `MID$`, `LEN`, `INSTR`, …), all described under *String Functions*.

```basic
FULL$ = "BERRY" + " " + "PI"      : REM "BERRY PI"
```

## Type mismatches

Numbers and strings never mix silently. Trying to add a number to a string, assign one to the other, or compare across types raises `Type mismatch: numbers and text can't be mixed`. Convert explicitly with `STR$` (number → text) and `VAL` (text → number) when you need to cross the divide.

---

# How Numbers Are Displayed

When `PRINT` (or `STR$`) turns a number into text, BerryBasiC uses these rules:

- A **whole number** prints with no decimal point: `PRINT 42` → `42`.
- Otherwise up to **9 significant digits** are shown, with trailing zeros trimmed: `PRINT 1/8` → `0.125`, `PRINT 2/3` → `0.666666667`.
- **Very large or very small** magnitudes switch to scientific "E" notation with a signed, two‑digit exponent: `PRINT 1e12` → `1E+12`, `PRINT 1.5e-9` → `1.5E-09`. The switch happens for exponents of about `+9` and above, or `−5` and below.
- Special values print as `INF` (magnitude beyond the double range) and `NAN` (not‑a‑number, e.g. `0/0` situations).

The value stored is always the full double precision; only its *printed* form is rounded to nine figures. If you need a particular layout (fixed decimals, right‑aligned columns, and so on), format it yourself with the string functions or lay it out with `PRINT`'s `,` fields, `TAB` and `SPC`.

---

# Constants

## PI

`PI` is the ratio of a circle's circumference to its diameter, 3.14159265358979…

```basic
PRINT PI
```

Use it with the trigonometric functions, which work in **radians** — a full turn is `2 * PI`:

```basic
PRINT SIN(PI / 2)        : REM 1
```

## TRUE

`TRUE` is the value a comparison gives when it holds. It is `-1` — every bit set — which is what makes the logical operators double as bitwise ones (see *Operators*).

```basic
PRINT TRUE               : REM -1
IF TRUE THEN PRINT "YES"
```

## FALSE

`FALSE` is the value a comparison gives when it does **not** hold. It is `0`.

```basic
PRINT FALSE              : REM 0
```

`IF` treats **any non‑zero number** as true, so `TRUE` and `FALSE` are conveniences rather than the only truth values — `IF 5 THEN …` runs, and so does `IF A$ <> "" THEN …`.

---

# Numeric Literals

You can write numbers in three ways.

## Decimal

Ordinary decimal, with or without a fractional part:

```basic
A = 123
B = 12.34
C = .5                   : REM a leading dot is fine (= 0.5)
```

## Scientific (E) notation

A number may carry an exponent, written with `E` (or lower‑case `e`) followed by an optional sign and the power of ten. This is the compact way to write very large or very small values:

```basic
A = 2E3                  : REM 2000
B = 1.5E-4               : REM 0.00015
C = 6.022E23             : REM Avogadro's number
```

The `E` form is only recognised when digits actually follow it, so a variable such as `E` or `EXIT` is never mistaken for an exponent.

## Hexadecimal

An ampersand `&` introduces a base‑16 (hexadecimal) constant, BBC‑style. The digits `A`–`F` may be upper or lower case:

```basic
A = &FF
PRINT A
```

```text
255
```

Hex is convenient for bit masks, colours and memory addresses:

```basic
MASK = &00FF00           : REM the green byte of an RGB value
```

---

# Assignment

Assignment copies the value on the right into the variable on the left.

The keyword `LET` is optional. These two lines mean exactly the same thing:

```basic
LET A = 10
A = 10
```

The right‑hand side may be any expression of the matching type:

```basic
TOTAL = PRICE * QTY
GREETING$ = "Hello, " + NAME$
```

Assigning to an integer (`%`) variable truncates towards zero, as described under *Data Types*. Assigning across types (a number to a `$` variable, or text to a numeric one) is an error.

Array elements and reserved‑memory locations are assigned the same way; see *Arrays* and *Memory and Indirection*.

---
# Operators

## Arithmetic

| Operator | Meaning |
|----------|---------|
| `+`      | Add (or, between two strings, join them) |
| `-`      | Subtract; as a prefix, negate |
| `*`      | Multiply |
| `/`      | Divide (always a floating‑point result) |
| `^`      | Raise to a power |

```basic
PRINT 2 ^ 8              : REM 256
PRINT 10 / 4            : REM 2.5   (division never rounds to an integer)
```

## Integer division — `DIV`

`DIV` divides and throws away the remainder, giving a whole‑number result (both operands are first truncated to integers):

```basic
PRINT 7 DIV 2            : REM 3
```

## Remainder — `MOD`

`MOD` gives the remainder that `DIV` discards:

```basic
PRINT 7 MOD 2            : REM 1
```

Dividing by zero — with `/`, `DIV` or `MOD` — raises `Division by zero`.

## Relational

The comparison operators compare two numbers, or two strings, and return `TRUE` (`-1`) or `FALSE` (`0`):

| Operator | True when… |
|----------|------------|
| `=`  | the operands are equal |
| `<>` | they differ |
| `<`  | the left is less than the right |
| `>`  | the left is greater than the right |
| `<=` | the left is less than or equal |
| `>=` | the left is greater than or equal |

```basic
IF A <> 0 THEN PRINT "NONZERO"
IF NAME$ = "BERRY" THEN PRINT "Hi Berry"
```

Strings compare **character by character** by code value (so uppercase letters sort before lowercase, digits before letters), and a shorter string sorts before a longer one that begins with it (`"CAT" < "CATS"`).

## Logical / bitwise — `AND` `OR` `EOR` `NOT`

`AND`, `OR`, `EOR` (exclusive‑or) and the prefix `NOT` work bit by bit on 32‑bit integers.

| Operator | Result |
|----------|--------|
| `AND` | bits set in **both** operands |
| `OR`  | bits set in **either** operand |
| `EOR` | bits set in **exactly one** operand |
| `NOT` | inverts every bit (so `NOT 0` is `-1`, and `NOT TRUE` is `0`) |

Because `TRUE` is `-1` (all bits set) and `FALSE` is `0`, the very same operators serve as logical connectives inside conditions:

```basic
PRINT 6 AND 3                       : REM 2   (bit masking)
IF A > 0 AND B > 0 THEN PRINT "BOTH POSITIVE"
IF DONE OR TIMED_OUT THEN GOTO 500
```

## Shifts and rotates

Bit shifts and rotates are written as functions and operate on 32‑bit integers:

| Function    | Meaning |
|-------------|---------|
| `SHL(x, n)` | shift `x` left by `n` bits (zeros in from the right) |
| `SHR(x, n)` | logical shift right by `n` bits (zeros in; treats `x` as unsigned) |
| `ASR(x, n)` | arithmetic shift right by `n` bits (copies the sign bit; keeps negatives negative) |
| `ROL(x, n)` | rotate left by `n` bits, within 32 bits |
| `ROR(x, n)` | rotate right by `n` bits, within 32 bits |

```basic
PRINT SHL(1, 4)          : REM 16
PRINT SHR(256, 4)        : REM 16
PRINT ASR(-8, 1)         : REM -4  (sign preserved)
PRINT ROL(1, 1)          : REM 2
```

A shift count of 32 or more gives `0` for `SHL`/`SHR`; a negative count is an error. Shifts and masks together pack and unpack fields — for example an RGB colour:

```basic
COL = SHL(R, 16) OR SHL(G, 8) OR B
R   = SHR(COL, 16) AND 255
```

## String concatenation

`+` between two strings joins them:

```basic
FULL$ = FIRST$ + " " + LAST$
```

## Indirection — `?` `!` `$`

`?`, `!` and `$` read or write memory directly — a byte, a 32‑bit word, and a CR‑terminated string respectively. They are covered fully under *Memory and Indirection*:

```basic
?addr                    : REM the byte at addr
addr!4                   : REM the 32-bit word at addr+4
$addr                    : REM the string at addr, up to a carriage return
```

## Precedence

Operators bind in the order below, from **loosest** (evaluated last) to **tightest** (evaluated first). Use parentheses whenever you want a different order — or simply for clarity.

| Level | Operators | Notes |
|-------|-----------|-------|
| 1 (loosest) | `OR`, `EOR` | bitwise/logical |
| 2 | `AND` | bitwise/logical |
| 3 | `=` `<>` `<` `>` `<=` `>=` | comparisons yield `-1`/`0` |
| 4 | `+` `-` | add/subtract, and string join |
| 5 | `*` `/` `DIV` `MOD` | |
| 6 | unary `-`, unary `+`, `NOT` | prefix operators |
| 7 | `^` | power; binds *tighter* than unary minus |
| 8 (tightest) | `?` `!` (binary form) | indirection postfix |

Two consequences worth remembering:

- Because `^` binds tighter than unary minus, `-2 ^ 2` is `-(2 ^ 2) = -4`, not `4`.
- Because binary `?` / `!` bind tightest, `BUF%?I + 1` means `(BUF%?I) + 1`. For an arithmetic *address* with the unary form, parenthesise it — `?(BUF% + 1)` — or use the binary form, `BUF%?1`.

Power is left‑associative here: `2 ^ 3 ^ 2` is `(2 ^ 3) ^ 2 = 64`.

---

# PRINT Statement

`PRINT` writes a list of items to the screen and (usually) ends with a newline. The **separator** between items decides the spacing.

## Basic output

```basic
PRINT "HELLO"
```

## Expressions

An item may be any numeric or string expression; numbers are converted to text using the rules under *How Numbers Are Displayed*:

```basic
PRINT A + B
PRINT "Area = "; PI * R * R
```

## Multiple items

A **semicolon** `;` joins items with no space at all:

```basic
PRINT "X="; X; " Y="; Y
```

A **comma** `,` moves to the next *print field*. Fields are 8 columns wide, so columns line up into a table:

```basic
PRINT A, B, C
```

## Suppressing the newline

A separator (`;` or `,`) at the very **end** of the line suppresses the closing newline, so the next `PRINT` carries on where this one stopped:

```basic
PRINT "HELLO";
PRINT "WORLD"
```

```text
HELLOWORLD
```

## Forcing a newline — `'`

A single quote (apostrophe) `'` inside the list forces a newline, a compact way to print several lines from one statement:

```basic
PRINT "Line 1" ' "Line 2" ' "Line 3"
```

## `TAB(n)`

`TAB(n)` moves the print position **forward** to column `n` (columns count from 0) by emitting spaces. If the cursor is already at or past column `n`, it does nothing — `TAB` never moves backwards and never starts a new line.

```basic
PRINT "Name"; TAB(12); "Score"
```

## `SPC(n)`

`SPC(n)` outputs exactly `n` spaces, wherever the cursor happens to be:

```basic
PRINT "A"; SPC(5); "B"
```

---

# INPUT Statement

`INPUT` reads a line typed by the user and stores it into one or more variables. It shows a `?` prompt while waiting.

## Numeric input

Reads a number. Any non‑numeric text at the field is read as `0`.

```basic
INPUT A
```

## String input

Reads text into a string variable. Spaces are kept, so the field may contain them:

```basic
INPUT NAME$
```

## A prompt string

Put a string, then `;` (or `,`), before the variable to use it as the prompt. A `? ` is always printed after your prompt text:

```basic
INPUT "NAME"; NAME$
```

The interpreter shows:

```text
NAME? 
```

## Several variables at once

You may fill several variables from one statement. On the typed line, separate the values with **commas**:

```basic
INPUT A, B, C
```

All the variables are filled from a single typed line. If the user supplies fewer values than requested, the remaining variables are left at `0` (or `""` for strings). Because commas separate fields, a single string field taken this way cannot itself contain a comma.

To read structured data back from a file with the same feel, see `INPUT#` under *File Handling*.

---

# Branching

## GOTO

`GOTO` jumps to another line and continues from there:

```basic
GOTO 100
```

`GOTO` (and `GOSUB`) may also jump to a **label** instead of a line number, which reads better and survives `RENUMBER`.

## Labels

A label is a name introduced with a leading dot, sitting on its own at the start of a line. Jump to it by name (the dot is optional in the jump):

```basic
10 GOTO main
20 .greet
30   PRINT "Hello"
40 RETURN
50 .main
60 GOSUB greet
70 END
```

Labels are never touched by `RENUMBER`, so `GOTO done` keeps working no matter how the program is renumbered.

## IF … THEN (single line)

The simplest form runs the rest of the line only when the condition holds:

```basic
IF A = 10 THEN PRINT "TEN"
```

The condition is true for any non‑zero number (and for a non‑empty string). Several things may follow `THEN`, separated by colons:

```basic
IF OK THEN PRINT "Saving" : GOSUB save : PRINT "Done"
```

`THEN` may be **omitted** in the single‑line form — `IF A = 10 PRINT "TEN"` works too — but writing `THEN` is clearer.

If what follows `THEN` is a bare **line number**, that is an implicit `GOTO` (a classic BASIC shorthand):

```basic
IF SCORE > HIGH THEN 900       : REM same as: IF SCORE > HIGH THEN GOTO 900
```

## IF … THEN … ELSE

`ELSE` supplies the alternative:

```basic
IF A = 10 THEN PRINT "TEN" ELSE PRINT "OTHER"
IF N < 0 THEN 800 ELSE 810     : REM either branch may be a line-number jump
```

## Block IF (multi‑line)

When `THEN` is the **last** thing on the line, `IF` starts a multi‑line block that runs until `ENDIF`. An optional `ELSE` (on its own) selects the alternative block. Blocks may be nested.

```basic
10 IF SCORE >= 50 THEN
20   PRINT "Pass"
30   PRINT "Well done"
40 ELSE
50   PRINT "Try again"
60 ENDIF
```

The single‑line form keeps its classic behaviour; the block form is chosen only when nothing follows `THEN` on the line. (The block form needs a stored program — it cannot be used from the direct‑mode prompt.)

## ON … GOTO

`ON` selects a target from a list by a 1‑based index:

```basic
ON N GOTO 100, 200, 300
```

| `N` | Jumps to |
|-----|----------|
| 1   | 100 |
| 2   | 200 |
| 3   | 300 |

A value outside the range (0, negative, or larger than the list) simply falls through to the next statement.

## ON … GOSUB

The same idea, but each target is called as a subroutine and control returns after the `ON` when the subroutine's `RETURN` runs:

```basic
ON CHOICE GOSUB 1000, 2000, 3000
```

---

# Subroutines

`GOSUB` and `RETURN` are the classic line‑numbered subroutine mechanism. For new code the named `PROC`/`FN` forms (see *Procedures* and *Functions*) are usually clearer, but `GOSUB` remains for compatibility and quick jobs.

## GOSUB

Jumps to a line (or label) and remembers where it came from:

```basic
10 GOSUB 100
20 PRINT "Back again"
30 END
100 PRINT "In the subroutine"
110 RETURN
```

Up to 32 `GOSUB`s may be active (nested) at once.

## RETURN

Returns to the statement immediately after the `GOSUB` that called this subroutine. A `RETURN` with no matching `GOSUB` raises `RETURN without a matching GOSUB`.

---
# FOR Loops

A `FOR` loop counts a variable from a start value to a limit, running its body once for each value. The limit is tested at the **top** of each pass, so a loop whose start is already past the limit runs zero times.

## Basic loop

The counter goes up by 1 each pass, from 1 to 10 inclusive:

```basic
FOR I = 1 TO 10
  PRINT I
NEXT
```

`NEXT` may name its variable (`NEXT I`) for clarity — helpful when loops are nested. Up to 16 `FOR` loops may be nested.

## STEP

`STEP` sets the amount added each pass:

```basic
FOR I = 0 TO 20 STEP 2
  PRINT I
NEXT
```

`STEP` may be fractional (`STEP 0.1`). Beware that fractional steps can accumulate tiny floating‑point errors over many passes.

## Negative STEP

A negative `STEP` counts down; the loop continues while the counter is at or **above** the limit:

```basic
FOR I = 10 TO 1 STEP -1
  PRINT I
NEXT
```

## Nesting

Nested loops close in reverse order (innermost first). Naming the variable on each `NEXT` makes the pairing explicit:

```basic
FOR Y = 1 TO 3
  FOR X = 1 TO 3
    PRINT X * Y;
  NEXT X
  PRINT
NEXT Y
```

---

# REPEAT Loops

## REPEAT … UNTIL

The body runs first, then the condition is tested, so a `REPEAT` loop always runs **at least once**. It repeats *until* the condition becomes true:

```basic
A = 0
REPEAT
  A = A + 1
  PRINT A
UNTIL A = 10
```

Use `WHILE` instead when the body should be skippable from the very start. Up to 16 `REPEAT` loops may be nested.

---

# WHILE Loops

## WHILE … ENDWHILE

`WHILE` tests its condition **before** each pass, so the body may run zero times. It repeats while the condition stays true:

```basic
10 N = 1
20 WHILE N <= 5
30   PRINT N
40   N = N + 1
50 ENDWHILE
```

Compared with `REPEAT … UNTIL` (tested at the bottom, always runs once), `WHILE` is the right choice when the body should be skipped entirely if the condition is false to start with. Up to 16 `WHILE` loops may be nested.

---

# Loop Control — EXIT and CONTINUE

`EXIT` and `CONTINUE` change the flow of the enclosing `FOR`, `REPEAT` or `WHILE` loop without resorting to a `GOTO`.

## EXIT

`EXIT` leaves a loop immediately, continuing after its terminator (`NEXT`, `UNTIL` or `ENDWHILE`).

```basic
10 FOR i = 1 TO 1000
20   IF a(i) = target THEN found = i : EXIT FOR
30 NEXT
40 PRINT "found at "; found
```

On its own, `EXIT` leaves the **innermost** loop, whatever its kind. You can name the kind to be explicit — `EXIT FOR`, `EXIT REPEAT`, `EXIT WHILE` — which leaves the innermost loop of that kind, breaking out of any loops nested inside it too.

## CONTINUE

`CONTINUE` skips the rest of the current pass and goes straight to the loop's next test: `FOR` advances the counter, `REPEAT` and `WHILE` re‑check their condition.

```basic
10 FOR n = 1 TO 10
20   IF n MOD 2 = 0 THEN CONTINUE FOR : REM skip the even numbers
30   PRINT n
40 NEXT
```

Like `EXIT`, a bare `CONTINUE` acts on the innermost loop, and `CONTINUE FOR` / `CONTINUE REPEAT` / `CONTINUE WHILE` name the kind.

---

# Error Handling — TRY and CATCH

Instead of letting an error stop the program, wrap the risky part in a `TRY … CATCH … ENDTRY` block. If any statement between `TRY` and `CATCH` raises an error, control jumps to the code after `CATCH`; if nothing goes wrong, the handler is skipped.

```basic
10 TRY
20   ch = OPENIN "DATA.TXT"
30   PRINT "opened channel "; ch
40 CATCH
50   PRINT "Could not open the file: "; ERR$
60 ENDTRY
70 PRINT "carrying on"
```

Errors raised inside a `PROC` or `FN` called from the `TRY` block are caught too — the whole call is unwound cleanly back to the handler, so loops and locals are restored and the program can keep running.

## ERR and ERR$

Inside (and after) a `CATCH`, two read‑only values describe what happened:

* `ERR$` — the error message as text.
* `ERR` — a numeric code: the number you passed to `RAISE`, or `0` for a built‑in error.

## RAISE

`RAISE` throws your own error, which the nearest enclosing `CATCH` will handle.

```basic
RAISE "something went wrong"        : REM message only (ERR = 0)
RAISE 404                           : REM a numeric code
RAISE 404, "not found"              : REM code and message
```

A `TRY` with no matching `CATCH` still needs an `ENDTRY`; blocks may be nested, and an inner handler catches an error without disturbing an outer one.

---

# CASE Selection

## CASE … OF … WHEN

`CASE` picks one branch by matching an expression against the values listed by each `WHEN`. A single `WHEN` may list several comma‑separated values. `OTHERWISE` catches anything that matched no `WHEN`, and `ENDCASE` closes the statement. The selector may be numeric or a string.

```basic
10 CASE DAY OF
20   WHEN 1, 7: PRINT "Weekend"
30   WHEN 6:    PRINT "Almost there"
40   OTHERWISE  PRINT "Weekday"
50 ENDCASE
```

Only the first matching `WHEN` (or `OTHERWISE`) runs; control then continues after `ENDCASE`. Each clause's statements may follow on the same line after a colon, or on the lines below until the next `WHEN` / `OTHERWISE` / `ENDCASE`. `CASE` needs a stored program (it cannot be used from the direct prompt), and up to 16 `CASE` statements may be nested.

A string example:

```basic
10 CASE CMD$ OF
20   WHEN "N", "NORTH": PROCgo_north
30   WHEN "S", "SOUTH": PROCgo_south
40   OTHERWISE PRINT "I don't understand."
50 ENDCASE
```

---

# Arrays

`DIM` reserves an array. Indices start at **0**, so `DIM A(N)` gives `N+1` elements, `A(0)` through `A(N)` (BBC BASIC semantics). Up to **3** dimensions are supported, and up to 16 arrays may exist at once. Every element starts at `0` (or `""` for a string array).

## One dimension

```basic
DIM A(10)                : REM elements A(0) .. A(10)
A(0) = 100
A(1) = A(0) + 1
PRINT A(1)
```

## Two dimensions

Bounds are given per dimension; `DIM GRID(9, 9)` is a 10×10 grid:

```basic
DIM GRID(9, 9)
GRID(2, 3) = 7
```

## Three dimensions

```basic
DIM CUBE(3, 3, 3)
CUBE(1, 1, 1) = 42
```

## String arrays

A `$` array holds a string in every element:

```basic
DIM NAME$(20)
NAME$(0) = "BERRY"
NAME$(1) = "PI"
```

An index outside the declared bounds raises `Array index out of range`, and re‑`DIM`ing an existing array raises `That array is already defined`. Arrays are cleared (along with all other variables) whenever a program is `RUN`, and by `NEW` and `LOAD`.

---

# Memory and Indirection

Beyond variables and arrays, a program can reserve a block of raw bytes and read or write it directly with the indirection operators `?`, `!` and `$`. This is how you build a buffer to hand to a native seed for fast processing, or to pack binary data.

## Reserving memory

`DIM name size` — a name **without** parentheses — reserves `size + 1` bytes and puts the address of the first byte in the variable:

```basic
DIM BUF% 255             : REM 256 bytes; BUF% now holds their address
```

Use a numeric variable (typically a `%` integer, since it holds an address). The block lives until the next `RUN`/`NEW`. Reserve several at once with commas: `DIM A% 100, B% 1000`.

## The indirection operators

| Form        | Meaning |
|-------------|---------|
| `?addr`     | the byte at `addr` (0–255) |
| `addr?n`    | the byte at `addr + n` |
| `!addr`     | the 32‑bit word at `addr` (little‑endian, signed) |
| `addr!n`    | the word at `addr + n` |
| `$addr`     | the string at `addr`, up to a carriage‑return (`&0D`) terminator |

They work on **both** sides of `=` — reading (peek) and writing (poke):

```basic
?BUF% = 65               : REM poke a byte
BUF%?1 = 66              : REM poke the next byte
PRINT ?BUF%, BUF%?1      : REM 65  66
!BUF% = &12345678        : REM poke a 32-bit word (little-endian)
PRINT BUF%?0             : REM 120  (&78, the low byte)
$BUF% = "BERRY"          : REM write text plus a CR terminator
PRINT $BUF%              : REM BERRY
```

Binary `?`/`!` bind tighter than arithmetic, so `BUF%?I + 1` is `(BUF%?I) + 1`. For an arithmetic *address* with the unary form, parenthesise it — `?(BUF% + 1)` — or just use the binary form, `BUF%?1`.

## PEEK and POKE

If you are coming from Microsoft‑style BASICs, `PEEK` and `POKE` are provided as familiar aliases for byte access. They read and write exactly the same memory as `?`, so you can mix the two freely.

| Alias           | Same as       |
| --------------- | ------------- |
| `POKE addr, b`  | `?addr = b`   |
| `PEEK(addr)`    | `?addr`       |

```basic
10 DIM BUF 16
20 POKE BUF, 65           : REM store a byte (kept modulo 256)
30 POKE BUF + 1, 66
40 PRINT PEEK(BUF); PEEK(BUF + 1)     : REM 65 66
50 PRINT CHR$(PEEK(BUF))              : REM A
```

There is no separate `PEEK`/`POKE` for words or strings — use `!` for a 32‑bit word and `$` for a string, as above.

> On this bare‑metal machine an address is a **real** memory address, not a slot in a 64 KB sandbox. `POKE`ing a made‑up address can crash the system, so poke inside memory you reserved with `DIM name size` (as here) unless you are deliberately writing to a known hardware register. Classic pokes like `POKE 53280, 0` have no meaning here.

## Passing a buffer to a seed

Because the address is a real pointer, a seed can read or write the same memory. Build the buffer in BASIC and pass its address (and length):

```basic
10 DIM B% 9
20 FOR I = 0 TO 9 : B%?I = I * I : NEXT
30 SEED H%, "BUFSUM.SED"
40 PRINT CALL(H%, B%, 10)        : REM the seed sums the 10 bytes -> 285
```

with the seed dereferencing the address it is given:

```c
SEED_EXPORT(bufsum) {
    const unsigned char *p = (const unsigned char *)(uintptr_t)(long)argv[0].num;
    int len = (int)argv[1].num, sum = 0;
    for (int i = 0; i < len; i++) sum += p[i];
    return sum;
}
```

This is the fast path for bulk data: BASIC owns the buffer, the seed crunches it natively, and both see the same bytes.

---

# DATA Processing

`DATA` holds constants inside the program; `READ` copies them into variables one after another; `RESTORE` chooses where the next `READ` starts. All the `DATA` lines in the program form one continuous list, read in line‑number order.

## DATA

Lists values to be read later. `DATA` lines may appear anywhere in the program (they are skipped during normal execution):

```basic
10 DATA 10, 20, 30
```

## READ

Takes the next item(s) from the `DATA` list, in order, and stores them. Reading past the end of the list raises `READ ran out of DATA`:

```basic
READ A, B, C
```

## String DATA

A `DATA` item may be a quoted string; read it into a string variable. The type read must match the variable's type:

```basic
10 DATA "RED", "GREEN", "BLUE"
20 READ A$, B$, C$
```

## RESTORE

Restart reading from the very first `DATA` item:

```basic
RESTORE
```

Or restart from the first item on (or after) a given line, which lets you keep several independent tables in one program:

```basic
RESTORE 100
```

A worked example — reading a table until it is exhausted:

```basic
10 FOR I = 1 TO 3
20   READ NM$, SCORE
30   PRINT NM$; " scored "; SCORE
40 NEXT
50 DATA "Ann", 40, "Ben", 55, "Cid", 33
```

---
# Procedures

A **procedure** is a named block of statements you can call by name. Procedures make a program readable and let you reuse a piece of logic without copying it.

## Definition

Define a procedure with `DEF PROC`*name* and end it with `ENDPROC`. Two spellings are accepted and are equivalent — the glued classic form and the spaced form:

```basic
10 DEF PROChello
20   PRINT "HELLO"
30 ENDPROC
```

```basic
10 DEF PROC hello
20   PRINT "HELLO"
30 END PROC
```

## Calling

Run a procedure by writing its name with the `PROC` prefix attached:

```basic
40 PROChello
```

Control returns to the statement after the call when `ENDPROC` (or `END PROC`) is reached.

## Parameters

Parameters are listed in parentheses and passed **by value**, so changing a parameter inside the procedure never affects the caller's variable:

```basic
10 DEF PROCadd(A, B)
20   PRINT A + B
30 ENDPROC
40 PROCadd(10, 20)       : REM prints 30
```

A procedure may call itself; recursion works as long as nesting stays within limits (up to 32 nested `PROC`/`FN` calls).

## LOCAL variables

`LOCAL` makes a variable private to the procedure: its previous value is saved on entry and restored on `ENDPROC`. This keeps a procedure — including a recursive one — from disturbing variables of the same name elsewhere:

```basic
10 DEF PROCtest
20   LOCAL A
30   A = 100
40   PRINT A
50 ENDPROC
```

List several after `LOCAL`, separated by commas: `LOCAL I, J, TMP$`.

---

# Functions

A function is like a procedure but **returns a value**, so it can be used inside an expression.

## Definition

Define a function with `DEF FN`*name*`(`parameters`)`. There are two equivalent styles for returning the result:

The recommended, readable style — assign to a variable whose name matches the function, then close with `END FN`:

```basic
10 DEF FN square(x)
20   square = x * x
30 END FN
```

The classic BBC style — glue the name to `FN` and return with a line beginning `=`, which both supplies the result and ends the function:

```basic
10 DEF FNsquare(x)
20 = x * x
```

Both are entirely equivalent, and a function defined either way can be **called** either way. Parameters are passed by value, and a function may use `LOCAL` and may call itself:

```basic
10 DEF FN fact(n)
20   IF n <= 1 THEN fact = 1 ELSE fact = n * fact(n - 1)
30 END FN
```

## Using a function

Call it by name, as part of an expression — its result can go anywhere a value is allowed:

```basic
40 a = square(5)
50 PRINT a
```

```text
25
```

The name alone is enough — `square(5)` — because the interpreter recognises it as a defined function. The classic `FN` forms, `FNsquare(5)` (glued) and `FN square(5)` (spaced), work too and mean exactly the same thing.

---

# Modules (IMPORT)

A **module** is an ordinary BASIC file that holds a collection of functions and procedures. `IMPORT` pulls a module into your program so you can call everything it defines — a simple way to build a library of reusable code and share it between programs.

## Writing a module

A module is just a normal `.BAS` file containing `DEF FN` / `DEF PROC` definitions. For example, save this as `MATHLIB`:

```basic
10 DEF FN gcd(a, b)
20   IF b = 0 THEN gcd = a ELSE gcd = gcd(b, a MOD b)
30 END FN
40 DEF FN lcm(a, b)
50   lcm = a * b / gcd(a, b)
60 END FN
```

## Using a module

Put `IMPORT "name"` in your program (usually near the top). Every function and procedure the module defines then becomes callable, exactly as if you had typed it into your own program:

```basic
10 IMPORT "MATHLIB"
20 PRINT "gcd = "; gcd(48, 36)
30 PRINT "lcm = "; lcm(4, 6)
```

As with `LOAD`, a name with no extension gets `.BAS` added, so `IMPORT "MATHLIB"` reads `MATHLIB.BAS`.

## Line numbers don't clash

A module keeps its **own line‑number space**. The module above uses lines 10–60, and the program that imports it *also* uses 10–30 — that is completely fine. `GOTO`, `GOSUB`, `RESTORE` and labels inside a module only ever see that module's own lines, and the same in your main program. You never have to renumber a module to avoid overlapping with the code that imports it.

## Notes

- Modules may import other modules; imports are followed automatically. A module is loaded only once even if several modules ask for it. Up to 16 modules may be imported.
- Imported lines are not part of your program: `LIST` and `SAVE` show only the code you typed, and imports are resolved fresh each time you `RUN`.
- Keep to functions and procedures in a module. `IMPORT` itself does nothing at run time — it is handled once, before the program starts.

---

# String Functions

Character positions in strings are **1‑based**: the first character is position 1. Length and count arguments are clamped to the string, so asking for more characters than exist simply returns as many as there are (never an error).

## LEN

Number of characters in a string.

```basic
PRINT LEN("HELLO")       : REM 5
```

## ASC

Character code of the first character. `ASC("")` raises `Invalid argument`.

```basic
PRINT ASC("A")           : REM 65
```

## CHR$

The one‑character string for a character code (0–255).

```basic
PRINT CHR$(65)           : REM A
```

## STR$

Converts a number to its printed text form (the same form `PRINT` would use).

```basic
PRINT STR$(123)          : REM 123
```

## VAL

Reads a number from the front of a string, stopping at the first character that can't be part of one; leading non‑numeric text gives `0`. It accepts a leading sign, a decimal point, and `E`‑notation (it does **not** read `&`‑hex).

```basic
PRINT VAL("123")         : REM 123
PRINT VAL("3.5 apples")  : REM 3.5
PRINT VAL("-2.5E3")      : REM -2500
```

## LEFT$

The leftmost `n` characters. If `n` exceeds the length the whole string is returned; if `n` is 0 or less, the empty string.

```basic
PRINT LEFT$("HELLO", 3)  : REM HEL
```

## RIGHT$

The rightmost `n` characters, clamped the same way.

```basic
PRINT RIGHT$("HELLO", 2) : REM LO
```

## MID$

`MID$(s, start)` returns from position `start` to the end; `MID$(s, start, n)` returns at most `n` characters from that position. `start` is 1‑based.

```basic
PRINT MID$("HELLO", 2, 3): REM ELL
PRINT MID$("HELLO", 3)   : REM LLO
```

## STRING$

A string made of `n` copies of another string.

```basic
PRINT STRING$(10, "*")   : REM **********
```

## INSTR

Position of the first occurrence of the second string within the first, or `0` if it is not found. An optional third argument gives the 1‑based position to start searching from.

```basic
PRINT INSTR("HELLO", "LL")       : REM 3
PRINT INSTR("ABABAB", "AB", 2)   : REM 3
```

## UPPER$ / LOWER$

Return the string converted to upper or lower case.

```basic
PRINT UPPER$("Hello")      : REM HELLO
PRINT LOWER$("Hello")      : REM hello
```

## TRIM$

Return the string with leading and trailing whitespace removed.

```basic
PRINT "["; TRIM$("  hi  "); "]"    : REM [hi]
```

## REPLACE$

`REPLACE$(text$, find$, with$)` returns `text$` with **every** occurrence of `find$` replaced by `with$`. If `with$` is empty the matches are deleted.

```basic
PRINT REPLACE$("a,b,c", ",", " / ")     : REM a / b / c
PRINT REPLACE$("mississippi", "s", "")  : REM miiippi
```

## CONTAINS / STARTSWITH / ENDSWITH

Tests that return `TRUE` (‑1) or `FALSE` (0).

```basic
IF STARTSWITH(name$, "Dr ") THEN PRINT "a doctor"
IF ENDSWITH(file$, ".BAS")  THEN PRINT "a program"
IF CONTAINS(line$, "ERROR") THEN PRINT "problem found"
```

## SPLIT

`SPLIT(text$, sep$, parts$())` breaks `text$` at every occurrence of the separator `sep$`, stores the pieces in the string array `parts$()`, and returns how many pieces were stored (starting at index 0).

```basic
10 n = SPLIT("apple,banana,cherry", ",", fruit$())
20 FOR i = 0 TO n - 1
30   PRINT i; ": "; fruit$(i)
40 NEXT
```

Empty fields are kept (so `"a,,c"` yields three pieces), and an empty separator splits into individual characters. The array is created automatically if it does not exist; if you `DIM` it yourself, `SPLIT` fills up to its size and returns how many it stored.

## JOIN$

`JOIN$(parts$(), sep$ [, count])` is the inverse of `SPLIT`: it joins the array elements into one string with `sep$` between them. An optional `count` joins just the first `count` elements (handy with the value `SPLIT` returned).

```basic
10 n = SPLIT("one two three", " ", w$())
20 PRINT JOIN$(w$(), "-", n)          : REM one-two-three
```

---

# Mathematical Functions

The trigonometric functions work in **radians**, not degrees; use `RAD` and `DEG` to convert. These single‑argument functions allow the parentheses to be omitted, BBC‑style, so `SQR 2` and `SQR(2)` are the same, and the function binds tighter than the surrounding operators. (A single‑argument function may even be glued to a numeric literal — `SQR3` means `SQR 3` — though a space reads better.)

## ABS

Absolute value (drops the sign).

```basic
PRINT ABS(-5)            : REM 5
```

## INT

The largest whole number **not greater than** X — it rounds towards minus infinity (so it is not the same as truncation for negatives).

```basic
PRINT INT(3.7)           : REM 3
PRINT INT(-3.2)          : REM -4
```

## SGN

The sign of X: `-1`, `0`, or `1`.

```basic
PRINT SGN(-42)           : REM -1
```

## SQR

Square root. X must not be negative.

```basic
PRINT SQR(9)             : REM 3
```

## SIN / COS / TAN

Sine, cosine and tangent of X (X in radians).

```basic
PRINT SIN(PI / 2)        : REM 1
PRINT COS(0)             : REM 1
```

## ATN / ASN / ACS

Arctangent, arcsine and arccosine, each returning an angle in radians. For `ASN` and `ACS`, X must be between −1 and 1.

```basic
PRINT ATN(1)             : REM 0.785398... (PI/4)
```

## LOG / EXP

`LOG` is the natural logarithm (base e); X must be greater than 0. `EXP` is its inverse, e raised to the power X.

```basic
PRINT LOG(EXP(1))        : REM 1
```

(For a base‑10 or other‑base logarithm, divide: `LOG(x) / LOG(10)`.)

## DEG / RAD

Convert between radians and degrees.

```basic
PRINT DEG(PI)            : REM 180
PRINT RAD(180)           : REM 3.14159...
```

---

# Random Numbers

## RND

`RND` returns pseudo‑random numbers. Its behaviour depends on the argument:

| Call      | Result |
|-----------|--------|
| `RND(1)`  | a floating‑point value in the range 0 (inclusive) to 1 (exclusive) |
| `RND(n)`  | a whole number from 1 to `n`, for `n` greater than 1 |
| `RND(0)`  | repeats the last value returned by `RND(1)` |
| `RND(-n)` | seeds the generator from `n` and returns `-n`, giving a repeatable sequence |

```basic
PRINT RND(100)           : REM a whole number from 1 to 100
PRINT RND(6)             : REM a dice roll, 1 to 6
```

Seed with a negative argument when you want the *same* sequence every run — useful while debugging:

```basic
X = RND(-1)              : REM fix the seed
PRINT RND(6)             : REM the same "roll" each run
```

---

# Keyboard Functions

## GET

Waits for a key press and returns its character code. The program pauses until a key is pressed.

```basic
A = GET
```

## GET$

Like `GET`, but returns the key as a single‑character string.

```basic
A$ = GET$
```

## INKEY

`INKEY(n)` waits up to `n` centiseconds (hundredths of a second) for a key. It returns the character code if a key arrives in time, or `-1` if none does.

```basic
K = INKEY(100)           : REM wait up to 1 second
```

## INKEY$

Like `INKEY`, but returns a single‑character string, or the empty string `""` on timeout.

```basic
K$ = INKEY$(100)
```

A common "wait, but not forever" loop:

```basic
10 PRINT "Press a key..."
20 REPEAT : K = INKEY(10) : UNTIL K <> -1
30 PRINT "You pressed code "; K
```

---

# Cursor Functions

## POS

The current text cursor column (X position), counting from 0.

```basic
PRINT POS
```

## VPOS

The current text cursor row (Y position), counting from 0.

```basic
PRINT VPOS
```

---

# Mouse

A USB mouse (plugged into a USB‑A port on real hardware, or supplied with `-device usb-mouse` under QEMU) drives an on‑screen pointer. Position is reported in **raw framebuffer pixels** with the origin at the **top‑left** corner: X runs 0 to screen‑width−1, Y runs 0 to screen‑height−1. The pointer starts at the centre of the screen and is clamped to the screen edges.

The **button value** is a bitmask:

| Bit | Value | Button |
|-----|-------|--------|
| 0   | 1     | Left   |
| 1   | 2     | Right  |
| 2   | 4     | Middle |

So a value of `3` means left+right are held together. Test a single button with `AND` — for example `IF MOUSEB AND 1 THEN …` for the left button.

If no mouse is present, the position reads back as `0,0` and the buttons as `0`.

When a mouse is connected the system draws an **arrow pointer** on screen and moves it automatically — including at the `>` editor prompt and while a program waits at `GET`/`INKEY`. You do not have to draw the pointer yourself; reading `MOUSEX`/`MOUSEY`/`MOUSEB` simply tells you where it is.

> Under QEMU (`make run`), click inside the window once to let it capture the mouse (a relative USB mouse only sends movement while the window has grabbed the pointer); press `Ctrl`+`Alt`+`G` to release it.

## MOUSEX / MOUSEY / MOUSEB

Three parenthesis‑free value functions, each reading one component of the pointer, for use inside an expression:

```basic
PLOT 69, MOUSEX, MOUSEY          : REM plot a point under the pointer
IF MOUSEB AND 1 THEN PROCclick   : REM act on the left button
```

## MOUSE

The `MOUSE` statement reads all three at once into three numeric variables — X, Y, then the button bitmask:

```basic
MOUSE X%, Y%, B%
PRINT "pointer at "; X%; ","; Y%; "  buttons="; B%
```

Reading the mouse (via either form) also polls the hardware, so call it in your main loop to keep the pointer up to date. See `examples/mouse.bas` for a small drawing demo.

---

# Time

`TIME` is a centisecond (hundredth‑of‑a‑second) counter. Read it as a value:

```basic
PRINT TIME
```

Assign to it to reset or set the counter, typically to time an interval:

```basic
TIME = 0
REM ... do some work ...
PRINT "Took "; TIME; " centiseconds"
```

---
# Screen Control

## CLS

Clears the text area and moves the cursor to the top‑left.

```basic
CLS
```

## COLOUR / COLOR

Sets the text foreground colour for subsequent `PRINT` output. Both spellings are accepted. The colour is a logical colour number (0–7 in the default palette). To set the *background*, use `VDU 17, 128 + c` (see the *VDU* section, where the palette is also described). The *Graphics Library* covers the four‑argument form of `COLOUR` that redefines a palette slot, and graphics colours (`GCOL`).

```basic
COLOUR 2                 : REM green text
COLOR 2                  : REM same thing (both spellings work)
```

---

# VDU

The `VDU` statement sends a list of byte values to the VDU driver, which controls the text and graphics screen. Codes 32 to 255 are printable characters; codes 0 to 31 and 127 are control codes that act on the screen, some of which consume the values that follow as parameters.

```basic
VDU 65, 66, 67
```

```text
ABC
```

## Sending 16‑bit values

A value followed by a **semicolon** `;` is sent as a 16‑bit word — two bytes, least‑significant first. A value followed by a **comma** `,` (or by nothing) sends a single byte (its least‑significant byte). Screen coordinates are 16‑bit, so they are written with semicolons:

```basic
VDU 25, 5, 640; 512;
```

This is the same as `PLOT 5, 640, 512` (draw a line to 640,512).

## VDU code summary

| Code | Params | Meaning |
|------|--------|---------|
| 0    | –      | Null — does nothing |
| 4    | –      | Write text at the text cursor (the default) |
| 5    | –      | Write text at the graphics cursor |
| 6    | –      | Enable output to the screen |
| 7    | –      | Bell (no sound on this hardware) |
| 8    | –      | Move the text cursor back one character |
| 9    | –      | Move the text cursor forward one character |
| 10   | –      | Move the text cursor down one line |
| 11   | –      | Move the text cursor up one line |
| 12   | –      | Clear the text area (same as `CLS`) |
| 13   | –      | Move the text cursor to the start of the line |
| 16   | –      | Clear the graphics area (same as `CLG`) |
| 17   | 1      | Define a text colour (same as `COLOUR`) |
| 18   | 2      | Define a graphics colour (same as `GCOL`) |
| 19   | 5      | Set an entry in the colour palette |
| 20   | –      | Restore the default colours and palette |
| 21   | –      | Disable output to the screen |
| 22   | 1      | Select the screen mode (same as `MODE`) |
| 23   | 9      | Define a character, or control the cursor / scrolling |
| 24   | 8      | Define a graphics viewport |
| 25   | 5      | Plot (same as `PLOT`) |
| 26   | –      | Restore the default viewports and graphics origin |
| 27   | 1      | Send the next value to the screen as a literal character |
| 28   | 4      | Define a text viewport |
| 29   | 4      | Set the graphics origin |
| 30   | –      | Home the text cursor to the top‑left |
| 31   | 2      | Move the text cursor to column, row |
| 127  | –      | Backspace and delete |

Codes 1, 2 and 3 (printer) and 14 and 15 (auto‑paging) are accepted but have no effect on this hardware.

## Colours and palette

```basic
VDU 17, c            : set the text foreground to logical colour c (0 to 7)
VDU 17, 128 + c      : set the text background to logical colour c
VDU 18, action, c    : set the graphics colour and plot action (same as GCOL)
VDU 19, l, 16, r, g, b : set logical colour l to an RGB value (r, g, b each 0 to 255)
VDU 19, l, p, 0, 0, 0  : set logical colour l to default physical colour p (0 to 7)
VDU 20               : restore the default eight colours and palette
```

## Cursor and viewports

```basic
VDU 31, x, y         : move the text cursor to column x, row y
VDU 30               : home the text cursor to the top-left of the text viewport
VDU 28, l, b, r, t   : define a text viewport in character cells (left, bottom, right, top)
VDU 24, l; b; r; t;  : define a graphics viewport in graphics coordinates
VDU 26               : restore the full-screen viewports and reset the origin
VDU 29, x; y;        : set the graphics origin (same as ORIGIN in other BASICs)
```

A text viewport restricts where text is printed and scrolled; a graphics viewport clips all plotting (and `CLG`) to a rectangle.

## VDU 5 — text at the graphics cursor

`VDU 5` makes all character output — including `PRINT` — appear at the graphics cursor instead of the text cursor. Characters are drawn in the current graphics foreground colour, using the current `GCOL` plot action, with a transparent background, and are clipped to the graphics viewport. `VDU 4` returns to normal text output.

```basic
MOVE 200, 400
VDU 5
PRINT "LABEL"
VDU 4
```

While in `VDU 5` mode, codes 8, 9, 10, 11 move the graphics cursor by one character, `VDU 13` returns it to the left of the graphics viewport, `VDU 30` homes it to the top‑left, and `VDU 127` backspaces and erases using the graphics background colour.

## VDU 23 — user‑defined characters and control

Define a character (code 32 to 255) from eight rows of eight pixels, top to bottom. Each row is one byte; a set bit is a lit pixel, with the most significant bit on the left:

```basic
VDU 23, 240, 126, 129, 165, 129, 165, 153, 129, 126
PRINT CHR$(240)
```

This defines character 240 as a smiley face and then prints it.

Other `VDU 23` sub‑functions:

```basic
VDU 23, 1, 0; 0; 0; 0; 0;   : hide the text cursor (caret)
VDU 23, 1, 1; 0; 0; 0; 0;   : show the text cursor
VDU 23, 7, m, d, 0; 0; 0;   : scroll the text viewport one cell
                              (d: 0 = right, 1 = left, 2 = down, 3 = up)
```

The remaining `VDU 23` sub‑functions (cursor appearance, cursor‑movement flags, MODE 7 extensions, user‑defined screen modes and line thickness) are accepted but have no effect on this fixed‑resolution display.

---

# Graphics

BerryBasiC draws on a graphics screen using BBC‑style **logical coordinates**: x runs 0 to 1279, y runs 0 to 1023, with the origin at the **bottom‑left** (y increases upwards — the opposite of the mouse's pixel coordinates). The low‑level primitives below are the classic BBC set; the *Graphics Library* that follows adds high‑level shapes, truecolour and sprites on top of them.

## Screen resolution

Those logical coordinates are independent of the **physical** resolution: `CIRCLE 640, 512, 300` always draws in the middle of the screen whether the display is 320×240 or 1920×1080. A higher resolution just makes graphics sharper and fits more characters of text on a line; a lower one makes them chunkier.

By default the machine runs at its **startup resolution** (set at build time / in `config.txt`). A program can pick a different resolution while it runs with `SCREEN`, and the system returns to the startup resolution automatically when the program finishes.

## SCREEN

Switch the physical display resolution.

```basic
SCREEN width, height
```

`width` and `height` are in pixels (clamped to a sensible range). Switching clears the screen. Use it at the start of a program that wants a particular resolution:

```basic
10 SCREEN 320, 240      : REM chunky, fast
20 CIRCLE 640, 512, 400 : REM still addressed in logical coordinates
```

`SCREEN` on its own restores the **startup** resolution:

```basic
SCREEN
```

You normally don't need it: when a program ends (or stops with an error), the startup resolution is restored for you. If a program never calls `SCREEN`, nothing changes and the screen is not cleared.

> Because the framebuffer is genuinely re‑allocated by the GPU, `SCREEN` only takes effect on real hardware and under QEMU. On backends without a display it is a no‑op.

## SCREENW / SCREENH

Report the current physical resolution in pixels.

```basic
10 PRINT "Running at "; SCREENW; " x "; SCREENH
```

## MODE

Resets the screen, palette, viewports and graphics origin to their defaults. Unlike BBC BASIC, the mode number does **not** change the resolution — use `SCREEN` for that — so `MODE` mainly clears and resets the screen.

```basic
MODE 1
```

## GCOL — graphics colour and plot action

`GCOL` sets the colour (and, optionally, the *plot action*) used by all subsequent plotting. It has three forms:

```basic
GCOL c               : logical colour c (0-7), plot action 0 (store)
GCOL action, c       : logical colour c with an explicit plot action
GCOL r, g, b         : a 24-bit truecolour foreground (see the Graphics Library)
```

The **plot action** controls how a plotted pixel combines with what is already on the screen:

| Action | Effect |
|--------|--------|
| 0 | store the colour (overwrite) |
| 1 | OR with the existing pixel |
| 2 | AND with the existing pixel |
| 3 | EOR (exclusive‑or) with the existing pixel |
| 4 | invert the existing pixel (the colour is ignored) |

EOR (action 3) is especially useful for animation: drawing the same shape twice in EOR mode leaves the screen unchanged, so you can move a sprite without erasing the background.

```basic
GCOL 0, 3            : REM plot in logical colour 3, overwrite
GCOL 3, 1            : REM EOR mode, logical colour 1
```

## PLOT

`PLOT code, x, y` is the low‑level drawing primitive; the *code* selects what to draw and whether the coordinates are absolute or relative to the last point. `MOVE` and `DRAW` (below) are the two you will use most; a fuller set of the standard codes:

| Code | Meaning |
|------|---------|
| 4  | move to absolute (x, y) — same as `MOVE` |
| 5  | draw a line to absolute (x, y) in the foreground colour — same as `DRAW` |
| 0  | move *relative* by (x, y) |
| 1  | draw a line *relative* by (x, y) |
| 69 | plot a single point at absolute (x, y) |

```basic
PLOT 69, 100, 100    : REM a single point
PLOT 4, 0, 0         : REM move to the origin
PLOT 5, 500, 500     : REM draw a line to (500,500)
```

(The exact set of higher‑numbered codes — filled triangles, circles and so on — depends on the display driver; `LINE`, `RECTANGLE`, `CIRCLE` and friends in the *Graphics Library* give a friendlier way to reach the same results.)

## MOVE

Move the graphics cursor to (x, y) without drawing. Equivalent to `PLOT 4, x, y`.

```basic
MOVE 100, 100
```

## DRAW

Draw a line from the graphics cursor to (x, y) in the current foreground colour, then leave the cursor there. Equivalent to `PLOT 5, x, y`.

```basic
MOVE 100, 100
DRAW 200, 200
```

## CLG

Clears the graphics area (to the graphics background colour), within the current graphics viewport.

```basic
CLG
```

## POINT

`POINT(x, y)` reads back the logical colour of the pixel at (x, y) — the inverse of plotting.

```basic
C = POINT(100, 100)
```

---
# Graphics Library

The graphics library adds high‑level shape, colour and sprite statements on top of the low‑level `PLOT`/`MOVE`/`DRAW` primitives. All coordinates are BBC logical units (x in 0..1279, y in 0..1023, origin bottom‑left). Every shape is drawn in the current graphics foreground colour and honours the current `GCOL` plot action (store / OR / AND / EOR / invert).

## Truecolour (RGB)

The eight logical colours (0..7) still exist, but you can also draw in any 24‑bit colour. There are two ways.

Give `GCOL` three arguments — red, green and blue, each 0..255:

```basic
GCOL 255, 128, 0         : REM orange foreground
```

Or use the `RGB` function to pack a colour into a single value that `GCOL` accepts wherever a colour number is expected:

```basic
GCOL RGB(0, 128, 255)    : REM sky-blue foreground
C = RGB(255, 0, 255)
GCOL 3, C                : REM plot action 3 (EOR) in magenta
```

`RGB(r, g, b)` returns a tagged value that is distinct from the logical colour numbers 0..7, so `GCOL c` and `GCOL action, c` recognise it automatically.

## COLOUR l, r, g, b — redefine a palette slot

Redefine one of the eight logical colours to an arbitrary RGB value. After this, `GCOL 2` (and text `COLOUR 2`) use the new colour:

```basic
COLOUR 2, 255, 128, 0    : REM make logical colour 2 orange
```

(The single‑argument `COLOUR n` still selects a text colour — see *Screen Control*.)

## LINE

Draw a straight line between two points.

```basic
LINE x1, y1, x2, y2
LINE 100, 100, 1100, 700
```

## RECTANGLE

Draw a rectangle whose bottom‑left corner is (x, y), `w` wide and `h` high. Add `FILL` for a solid rectangle; without it only the outline is drawn.

```basic
RECTANGLE x, y, w, h
RECTANGLE FILL x, y, w, h
RECTANGLE FILL 500, 400, 200, 150
```

## CIRCLE

Draw a circle centred at (x, y) with radius `r` (outline, or solid with `FILL`). The circle is round on screen regardless of the physical aspect ratio.

```basic
CIRCLE x, y, r
CIRCLE FILL x, y, r
CIRCLE FILL 300, 500, 120
```

## ELLIPSE

Draw an ellipse centred at (x, y) with horizontal radius `rx` and vertical radius `ry` (outline, or solid with `FILL`).

```basic
ELLIPSE x, y, rx, ry
ELLIPSE FILL x, y, rx, ry
ELLIPSE FILL 640, 512, 300, 150
```

## FILL

Flood‑fill the connected region containing the point (x, y) with the current foreground colour. The region is bounded by any pixel of a different colour.

```basic
RECTANGLE 200, 150, 150, 120
GCOL 255, 0, 255
FILL 275, 210            : REM fill the inside of the outline
```

Note the two uses of the word `FILL`: as a *modifier* right after a shape keyword (`RECTANGLE FILL …`) it makes that shape solid; as a *statement* on its own (`FILL x, y`) it flood‑fills from a point.

## Sprites — GGET and GPUT

A sprite is a rectangular block of pixels captured from the screen into a reserved memory buffer (see `DIM`), so it can be stamped back elsewhere.

`GGET` captures the rectangle between the two corners (x1, y1) and (x2, y2) into the buffer at `addr`. The buffer must be large enough to hold an 8‑byte header plus 4 bytes per pixel: `width * height * 4 + 8` bytes.

```basic
GGET addr, x1, y1, x2, y2
```

`GPUT` stamps a previously captured sprite so that its top‑left corner sits at the logical point (x, y). The blit honours the current `GCOL` plot action, so an EOR sprite can be drawn and un‑drawn for flicker‑free animation.

```basic
GPUT addr, x, y
```

Example:

```basic
DIM S% 60000             : REM reserve a sprite buffer
GGET S%, 220, 440, 340, 560
GPUT S%, 800, 300
```

## Loading sprites from image files — LOADSPRITE

`LOADSPRITE` decodes an image file from the SD card into a sprite and returns its address, ready to pass to `GPUT`. Supported formats are **PNG**, **JPEG** and **BMP**. Unlike `GGET`, you do not reserve a buffer with `DIM` — the sprite is stored in a managed pool sized for the image, and the pool is emptied whenever a program is `RUN` or the variables are cleared.

```basic
addr = LOADSPRITE("filename")
```

`addr` is the sprite address, or **0** if the file is missing, unreadable, of an unsupported format, or too large for the pool. Always check for 0.

`SPRW(addr)` and `SPRH(addr)` return a sprite's width and height in pixels (read from its header), so you can centre or tile it.

The image is drawn at one screen pixel per image pixel (no scaling), with the image's top‑left corner placed at the logical point given to `GPUT`. Only images up to the screen size can be stamped.

**Transparency:** `GPUT` honours a sprite's alpha channel. Fully transparent pixels are skipped (the background shows through), fully opaque pixels are drawn normally, and partially transparent pixels (e.g. the smooth edges of a PNG cut‑out) are blended over whatever is already on screen. So a PNG of a character on a transparent background draws cleanly over your scene. Images with no alpha channel (JPEG/BMP, or an opaque PNG) are fully opaque, as are `GGET` captures.

```basic
128 cat% = LOADSPRITE("CAT.PNG")
130 IF cat% = 0 THEN PRINT "Could not load CAT.PNG" : END
140 PRINT "Loaded "; SPRW(cat%); " x "; SPRH(cat%)
150 GPUT cat%, 500, 700
```

## Saving sprites to image files — SAVESPRITE

`SAVESPRITE` writes a sprite back out to an image file on the SD card. The sprite can be one loaded with `LOADSPRITE`, or a region of the screen captured with `GGET` — so `GGET` + `SAVESPRITE` is also a way to take a **screenshot**.

```basic
SAVESPRITE addr, "filename"
```

The format is **PNG** (which preserves the alpha channel), unless the filename ends in `.bmp`, in which case a 24‑bit BMP is written. If the file cannot be written, the program stops with a `Could not save sprite` error.

```basic
200 DIM S% 200000               : REM room for the captured pixels
210 GGET S%, 100, 900, 400, 600 : REM grab a rectangle of the screen
220 SAVESPRITE S%, "SHOT.PNG"
```

Because PNG round‑trips alpha, you can load a transparent sprite and save it again without losing its transparency.

---

# Program Control Commands

These commands manage the program itself. Most are typically typed in direct mode.

## RUN

Executes the current program from its lowest line number. `RUN` first **clears all variables** (and arrays, and reserved memory), so a program always starts from a clean slate.

```basic
RUN
```

## LIST

Displays the stored program. Keywords are shown in UPPERCASE; variable names, strings and `REM` comments are shown as you typed them.

```basic
LIST
```

List a single line, a range, from a line onward, or up to a line:

```basic
LIST 100                 : REM just line 100
LIST 100, 200            : REM lines 100 to 200
LIST 100,                : REM line 100 to the end
LIST , 200               : REM the start up to line 200
```

## AUTO

Enter automatic line‑numbering. After `AUTO`, each line you type is given the next number automatically, so you can just type the program. Press Return on an offered number (without typing anything after it) to leave `AUTO`.

```basic
AUTO
```

or, choosing the first number and the step:

```basic
AUTO 100, 10
```

The default is `AUTO 10, 10` (start at 10, step 10).

## RENUMBER

Renumber the whole program, and fix up every line‑number reference — the targets of `GOTO`, `GOSUB`, `RESTORE`, `THEN`, `ELSE`, and the lists of `ON … GOTO` / `ON … GOSUB` — so the program still runs correctly.

```basic
RENUMBER
```

or, choosing the first number and the step:

```basic
RENUMBER 100, 10
```

The default is `RENUMBER 10, 10`. A reference to a line that does not exist is left unchanged. (Labels are never altered by `RENUMBER`, which is why they are the robust choice for jump targets.)

## EDIT

Recall a program line into the input so you can change it instead of retyping it. The line appears ready to edit; press Return to store the changes.

```basic
EDIT 150
```

## Line editing keys

While typing a line — at the prompt, during `AUTO`, or after `EDIT` — these keys are available:

| Key            | Action |
|----------------|--------|
| Left / Right   | move the cursor within the line |
| Home / End     | jump to the start / end of the line |
| Backspace      | delete the character before the cursor |
| Delete         | delete the character at the cursor |
| Up / Down      | recall previous / next commands from the history |

## NEW

Deletes the current program. Also clears all variables and the control stacks (and unloads any seeds).

```basic
NEW
```

## STOP

Stops a running program and prints a message noting where it stopped — useful as a deliberate breakpoint:

```basic
STOP
```

```text
STOP at line 250
```

## END

Ends the program **silently** (no message). It marks the normal, tidy finish of a program.

```basic
END
```

The difference in one line: `END` is the quiet, intended end of a run; `STOP` announces itself and the line it was on, so it reads like a breakpoint.

---

# Sound

BerryBasiC plays sound through the Raspberry Pi 4's **3.5 mm analogue jack** (a single square‑wave voice driven by the PWM hardware). Sound is **queued and plays in the background**: a `SOUND` or `TONE` statement returns immediately and the note plays while your program carries on. There are four channels, each with its own note queue; because there is one physical voice, when several channels have a note due the lowest‑numbered channel is the one you hear (the others still count down, so they are never starved).

> Sound only comes out on **real hardware**. QEMU's `raspi4b` does not emulate the PWM audio device, so under the emulator `SOUND` / `TONE` run but stay silent.

## SOUND

BBC‑style tone. Queues a note on a channel.

```basic
SOUND channel, amplitude, pitch, duration
```

| Argument | Meaning |
|----------|---------|
| `channel` | 0–3. Independent note queues. |
| `amplitude` | Loudness. BBC's `-15`…`0` and a plain `0`…`15` both work; `0` is silent, `15` (or `-15`) is loudest. |
| `pitch` | 0–255 in **quarter‑semitone** steps (4 = one semitone, 48 = one octave). `53` = middle C, `89` = the A above it (440 Hz). |
| `duration` | Length in **twentieths of a second** (`20` = one second). `-1` plays until replaced. |

```basic
10 REM a rising arpeggio on channel 1
20 SOUND 1, -15, 53, 10      : REM middle C
30 SOUND 1, -15, 69, 10      : REM E
40 SOUND 1, -15, 81, 10      : REM G
50 SOUND 1, -15, 89, 20      : REM high A, held longer
```

The four notes queue up and play one after another while the program continues past line 50.

## TONE

A direct, non‑BBC helper: play an exact frequency for a number of milliseconds. Queues on channel 0.

```basic
TONE frequency_hz, duration_ms [, volume]
```

`volume` is 0–15 and defaults to 15 (full). A `duration_ms` of `-1` plays until replaced.

```basic
10 TONE 440, 500            : REM concert A for half a second
20 TONE 880, 500, 8         : REM an octave up, quieter
```

## SOUND OFF

Silence everything immediately and empty all the queues.

```basic
SOUND OFF
```

Sound is also reset automatically whenever a program is `RUN`.

---
# Storage (SD Card)

Programs and data live on the SD card's FAT filesystem. File names may be given quoted or bare; if you give no extension, `.BAS` is assumed (so `LOAD WELCOME` opens `WELCOME.BAS`). Names are 8.3 — up to eight characters, a dot, a three‑letter extension — because the card is FAT.

## SAVE

Writes the current program to a file.

```basic
SAVE "GAME"              : REM -> GAME.BAS
SAVE "DATA.TXT"
```

## LOAD

Clears the current program and variables, then loads a program from a file.

```basic
LOAD "GAME"
```

## CAT / DIR

Lists the current directory. `CAT` and `DIR` are the same command. Subdirectories are shown with a `<DIR>` marker.

```basic
CAT
```

## DELETE

Removes a file from the card.

```basic
DELETE "OLD.BAS"
```

These commands work on a whole program (or, for `DELETE`, any file). To read and write your own **data files** byte by byte, use the file‑handling commands further below.

## Directories

The card can hold **subdirectories**, and every file command accepts a **path**. Components are separated by `/`. A path starting with `/` is absolute (from the top of the card); otherwise it is relative to the **current directory**.

| Command | Meaning |
|---------|---------|
| `MKDIR "name"` | create a directory |
| `CD "name"`    | change the current directory (`CD ".."` goes up, `CD "/"` to the top) |
| `RMDIR "name"` | remove a directory (it must be empty) |
| `PWD`          | print the current directory |

Because file commands take paths, you can read and write anywhere on the card:

```basic
MKDIR "SPRITES"
cat% = LOADSPRITE("SPRITES/CAT.PNG")
SAVE "LEVELS/LEVEL1"          : REM -> LEVELS/LEVEL1.BAS
OPENIN("/DATA/SCORES.DAT")    : REM absolute path
```

Directory and file names are still 8.3 since the card is FAT. `MKDIR`, `CD` and `RMDIR` take the name as given (no automatic `.BAS`), so quote them: `CD "GAMES"`.

## Reading a directory in a program

`CAT` prints a listing, but a program can also **walk** a directory itself and read each entry's details. Point the scanner at a directory with `DIROPEN`, then call `DIRNEXT` repeatedly; each time it returns `TRUE` (`-1`) it has stepped onto the next entry, whose fields you then read with the words below. It returns `FALSE` (`0`) when there are no more entries.

| Word | Result |
|------|--------|
| `DIROPEN("path")` | begin scanning a directory. Returns `TRUE` if it opened, `FALSE` if not. `DIROPEN(".")` or `DIROPEN("")` scans the current directory. |
| `DIRNEXT`   | step to the next entry; `TRUE` while one was read, `FALSE` at the end |
| `DIRNAME$`  | the current entry's name (e.g. `"CAT.PNG"`) |
| `DIRTYPE`   | `TRUE` if the entry is a directory, `FALSE` if it is a file |
| `DIRSIZE`   | the file's size in bytes (`0` for a directory) |
| `DIRDATE$`  | last‑modified date as `"YYYY-MM-DD"` |
| `DIRTIME$`  | last‑modified time as `"HH:MM"` |

Only one scan is active at a time, and `.` / `..` are not reported. Test `DIROPEN` directly as a condition — `IF DIROPEN(p$) THEN …` — rather than comparing it (like `OPENIN`, writing `DIROPEN(p$) = 0` does not work).

```basic
10 IF DIROPEN("/") THEN 30
20 PRINT "cannot open" : END
30 IF DIRNEXT = FALSE THEN 80
40 IF DIRTYPE THEN PRINT DIRNAME$, "<DIR>" : GOTO 30
50 PRINT DIRNAME$, DIRSIZE; " bytes  "; DIRDATE$; " "; DIRTIME$
60 GOTO 30
80 PRINT "done"
```

---

# File Handling

A program can open a file on the SD card as a **channel** and read or write its bytes directly. Writes go straight to the real FAT filesystem, so the files can be read on a PC (and by other programs). Up to four files may be open at once.

Open a file with one of three functions, each returning a channel number (a small positive integer), or **0** if it could not be opened:

| Function | Opens a file for… | If the file… |
|----------|-------------------|--------------|
| `OPENIN "name"`  | reading | doesn't exist → returns 0 |
| `OPENOUT "name"` | writing (creates a new file, or **empties** an existing one) | is created fresh |
| `OPENUP "name"`  | reading *and* writing | doesn't exist → returns 0 |

All the other file words take a channel after a `#`:

| Command | Meaning |
|---------|---------|
| `BGET# ch`         | read and return the next byte (0–255), or `-1` at end of file |
| `BPUT# ch, n`      | write the byte `n AND 255` |
| `BPUT# ch, A$`     | write every byte of the string `A$` |
| `EOF# ch`          | `TRUE` (`-1`) when the file pointer is at the end, else `FALSE` (`0`) |
| `EXT# ch`          | the length of the file in bytes |
| `PTR# ch`          | the current read/write position (0 = start) |
| `PTR# ch = n`      | move the position to byte `n` (0 to `EXT#`; `PTR# ch = EXT# ch` appends) |
| `CLOSE# ch`        | close the channel (writes are finalised here); `CLOSE# 0` closes every open channel |

Reading advances the pointer by one byte; so does writing. Seeking with `PTR#` lets you re‑read or overwrite any part of the file.

```basic
10 C = OPENOUT "SCORES.DAT"
20 FOR I = 1 TO 8 : BPUT# C, I * I : NEXT   : REM write eight bytes
30 BPUT# C, "END"                            : REM append the bytes of a string
40 CLOSE# C
50 C = OPENIN "SCORES.DAT"
60 PRINT "length = "; EXT# C
70 REPEAT : PRINT BGET# C; " "; : UNTIL EOF# C
80 PRINT
90 PTR# C = 3 : PRINT "byte 3 = "; BGET# C   : REM random access
100 CLOSE# C
```

> Always `CLOSE#` a file you have written — the final data and the file's length are committed to the card on close. `CLOSE# 0` is a quick way to close everything (for example in an error handler or at the end of a program).

## `PRINT#` and `INPUT#` (typed records)

`BGET#` / `BPUT#` work a byte at a time. When you want to store whole **numbers and strings** and get them back with their types intact, use `PRINT#` to write and `INPUT#` to read. Each value is stored as a self‑describing record, so you read them back in the same order you wrote them:

```basic
PRINT# ch, expr, expr, ...   : REM write a list of numbers and/or strings
INPUT# ch, var,  var,  ...   : REM read them back into variables
```

The variables in `INPUT#` may be simple variables or array elements, and their types must match the records in the file (a number into a numeric variable, a string into a `$` variable).

```basic
10 C = OPENOUT "SAVE.DAT"
20 PRINT# C, "Level", 7, SCORE, NAME$      : REM mix strings and numbers freely
30 CLOSE# C
40 C = OPENIN "SAVE.DAT"
50 INPUT# C, TITLE$, LVL, SC, NAME$
60 CLOSE# C
```

On disk each record is a tag byte followed by its data — a number is `&40` plus eight bytes (an IEEE‑754 double, little‑endian); a string is `&00`, a one‑byte length, then the characters. You can freely mix `BPUT#`/`BGET#` and `PRINT#`/`INPUT#` on the same file if you keep track of the layout yourself.

See `examples/fileio.bas` (byte level) and `examples/records.bas` (typed records) for complete demos.

---
# Native Seeds

Interpreting BASIC is convenient but not fast. A **seed** is a small piece of compiled native (AArch64) code that a program can load from the storage card and call directly, for the parts of a job that are too slow to interpret — some image filters, simulations, tight numeric loops. It is the modern equivalent of the machine‑code `CALL`/`USR` of classic BASICs.

A seed is an ordinary C function, cross‑compiled and turned into a `.SED` file on your Linux machine (see *Writing a seed* below), then copied onto the card. At run time the program loads it with `SEED` and calls it with `CALL` (for a number) or `CALL$` (for a string).

> Seeds run only on the Raspberry Pi (and the QEMU emulator), which is where the AArch64 code executes. On a host test build, loading or calling a seed raises `Native seeds run on the Pi, not the host build`.

## SEED

Loads a `.SED` file and stores a *handle* (a small number) in a numeric variable:

```basic
SEED H%, "FILT.SED"
```

The handle is how later `CALL`s refer to this seed. Up to **8** seeds may be loaded at once. The file is searched on the storage card; `.SED` is assumed if you give no extension. A fresh `RUN` reloads the program's seeds, so the `SEED` lines normally live in the program itself.

## CALL

Calls a loaded seed and returns its numeric result. The first operand is the handle; any further operands are passed to the seed as arguments:

```basic
R = CALL(H%, 40, 2)      : REM -> 42, if the seed adds its two arguments
```

As a statement (when you only want the side effect and not the result):

```basic
CALL H%, X, Y
```

Arguments may be numbers or strings, mixed freely. Up to 16 may be passed.

## CALL$

Like `CALL`, but returns a string — the text the seed produced with its `set_return_str` service. If the seed returns no string, the result is empty.

```basic
NAME$ = CALL$(H%, "berry pi")    : REM e.g. -> "BERRY PI"
```

## What a seed can do

A seed never links against the interpreter; instead it is handed a small set of *services* it may call back into. Through these it can print, read the keyboard, read and write BASIC variables and arrays, and return a string:

| Service          | Purpose |
|------------------|---------|
| `putc`, `puts`   | write characters to the screen |
| `getkey`, `inkey`| read a key (blocking, or with a timeout) |
| `get_num`, `set_num` | read / write a numeric BASIC variable |
| `get_str`, `set_str` | read / write a string BASIC variable |
| `num_array`      | get a direct pointer to a numeric array's storage |
| `set_return_str` | provide the string that `CALL$` returns |
| `time_cs`        | centiseconds since power‑on |
| `alloc`, `free`  | allocate / release working memory from the seed heap |
| `realloc`, `alloc_aligned` | resize a block / allocate with a given alignment |

Variable and array names are passed exactly as BASIC stores them: upper case, including the `$` or `%` suffix — e.g. `"X"`, `"N%"`, `"A$"`.

The `num_array` service is the real prize for heavy work: it hands the seed a direct pointer to the array's numbers, so a thousand‑element array can be crunched at full native speed with no copying. (String data is always copied in and out, because the interpreter is free to move strings around in memory.)

String *arguments* are snapshots: they are valid for the duration of the call. A seed that wants to keep one must copy it.

## Working memory

For tasks that need scratch space — a sorted copy, a lookup table, an image buffer — a seed allocates from its own heap with the *ordinary C functions* `malloc`, `calloc` and `free`:

```c
double *tmp = malloc(n * sizeof(double));
if (!tmp) return 0;                 // heap exhausted
... use tmp ...
free(tmp);
```

These read like normal C, but they draw from the *seed heap* rather than any system allocator (under the hood they route to the services). `malloc` returns a 16‑byte‑aligned block (suitable for doubles and NEON), or 0 if the heap is full; `calloc` also zeroes it. The seed heap is separate from BASIC's own variable and array storage, is 2 MB by default (raise `SEED_HEAP_SIZE` in the interpreter if you need more), and is wiped clean at every `RUN`/`NEW` — so a seed that forgets to `free` leaks only within the current run, never permanently. Memory does persist between calls within a run, so a seed may allocate a table on its first call and reuse it on later ones.

The full standard allocation set is provided, all backed by the same seed heap:

| Function | Notes |
|----------|-------|
| `malloc(size)`                          | uninitialised block, 16‑byte aligned |
| `calloc(nmemb, size)`                   | zeroed; checks `nmemb*size` overflow |
| `realloc(ptr, size)`                    | grow/shrink, preserving contents; `realloc(0,n)`=malloc, `realloc(p,0)`=free |
| `reallocarray(ptr, nmemb, size)`        | `realloc` with an overflow‑checked size |
| `free(ptr)`                             | release a block (`free(0)` is a no‑op) |
| `aligned_alloc(align, size)`            | block aligned to a power‑of‑two `align` (C11) |
| `memalign(align, size)`                 | legacy spelling of `aligned_alloc` |
| `posix_memalign(&ptr, align, size)`     | POSIX form; returns 0 / `EINVAL` / `ENOMEM` |
| `free_sized(ptr, size)`, `free_aligned_sized(ptr, align, size)` | C23 sized frees (the size hint is ignored) |

Anything `aligned_alloc`/`posix_memalign` returns can be released with the ordinary `free`. The standard `memset`, `memcpy`, `memmove` and `memcmp` are available too (a small runtime is linked in for you), so ordinary buffer code just works. If you prefer, the raw `svc->alloc` / `svc->free` / `svc->realloc` / `svc->alloc_aligned` services are still there and do exactly the same thing.

## The seed C library

Because a seed is built freestanding (there is no C library to link against), any standard function it calls must be provided by the *seed runtime*. A useful, OS‑independent subset is — just include the familiar headers:

```c
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
```

| Header       | Provided |
|--------------|----------|
| `<stdlib.h>` | `malloc` `calloc` `realloc` `reallocarray` `free` `free_sized` `free_aligned_sized` `aligned_alloc` `memalign` `posix_memalign`, `qsort` `bsearch`, `atoi` `atol` `strtol` `strtoul`, `abs` `labs`, `rand` `srand` |
| `<string.h>` | `memcpy` `memmove` `memset` `memcmp` `memchr`, `strlen` `strnlen` `strcmp` `strncmp` `strcpy` `strncpy` `strcat` `strncat` `strchr` `strrchr` `strstr` `strdup` `strndup` |
| `<ctype.h>`  | `isdigit` `isalpha` `isalnum` `isspace` `isupper` `islower` `isxdigit` `ispunct` … and `toupper` / `tolower` |

These behave exactly as in standard C; `malloc` and friends draw from the seed heap, and `qsort`/`bsearch` take the usual comparator. For example, sorting a BASIC array in place:

```c
#include "seed.h"
#include <stdlib.h>

static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

SEED_EXPORT(sortarr) {
    int len = 0;
    double *a = svc->num_array("E", &len);     // direct pointer to E()
    if (a && len > 0) qsort(a, len, sizeof(double), cmp);
    return len;
}
```

What is **not** provided is anything that needs an operating system — `printf`, file I/O, `getenv`, `exit`, threads, `time`, and so on. Calling one of those is a link error (`undefined reference to 'printf'`), which is the build telling you the seed reached outside its sandbox. Use the services (`svc`) for I/O instead: `svc->puts` to print, `svc->getkey`/`svc->inkey` for the keyboard, `svc->time_cs` for timing. The library lives in `seed/include/` and `seed/runtime/`; adding a missing pure function is just a declaration in the header and a definition in the runtime.

## Starting a new seed

The quickest way to begin is to let the project scaffold one for you:

```text
make newseed NAME=blur          (or just: tools/newseed.sh)
```

This creates `seed/garden/blur/` containing a starter `blur.c` (an empty seed you fill in) and a self‑contained `Makefile`. From there:

```text
cd seed/garden/blur
make                            builds blur.sed
make install                    copies it where 'make sdimage' bundles it
```

A seed name is 1–8 characters (a letter first, then letters/digits/`_`) — short enough for the card's 8.3 file names, and a valid C identifier because it is also the seed's entry function. The rest of this section explains what goes inside.

## Writing a seed

A seed is a single C function marked with `SEED_EXPORT`, in a file that includes `seed.h`. It receives the services pointer, the argument list, and the argument count, and returns a number:

```c
#include "seed.h"

SEED_EXPORT(seed)
{
    double a = (argc > 0) ? argv[0].num : 0;
    double b = (argc > 1) ? argv[1].num : 0;
    return a + b;
}
```

Each argument is tagged as a number (`argv[i].num`) or a string (`argv[i].is_str`, with `argv[i].str` / `argv[i].len`). To return a string, call `svc->set_return_str(buf, len)` and read it back with `CALL$`.

A string‑uppercasing seed, for example:

```c
#include "seed.h"

SEED_EXPORT(seed)
{
    if (argc < 1 || !argv[0].is_str) return 0;
    char buf[256];
    int n = argv[0].len;
    if (n > (int)sizeof(buf)) n = sizeof(buf);
    for (int i = 0; i < n; i++) {
        char c = argv[0].str[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        buf[i] = c;
    }
    svc->set_return_str(buf, n);
    return n;
}
```

And one that sums a numeric array passed by name:

```c
#include "seed.h"

SEED_EXPORT(seed)
{
    if (argc < 1 || !argv[0].is_str) return 0;
    char name[16]; int n = argv[0].len;
    if (n > 15) n = 15;
    for (int i = 0; i < n; i++) name[i] = argv[0].str[i];
    name[n] = 0;

    int len = 0;
    double *a = svc->num_array(name, &len);
    if (!a) return 0;
    double sum = 0;
    for (int i = 0; i < len; i++) sum += a[i];
    return sum;
}
```

And one that needs working memory — the median of an array, computed from an allocated sorted copy so the original is left untouched:

```c
#include "seed.h"

SEED_EXPORT(seed)
{
    if (argc < 1 || !argv[0].is_str) return 0;
    char name[16]; int n = argv[0].len;
    if (n > 15) n = 15;
    for (int i = 0; i < n; i++) name[i] = argv[0].str[i];
    name[n] = 0;

    int len = 0;
    double *a = svc->num_array(name, &len);
    if (!a || len <= 0) return 0;

    double *tmp = malloc(len * sizeof(double));
    if (!tmp) return 0;                 // out of heap
    for (int i = 0; i < len; i++) tmp[i] = a[i];

    for (int i = 1; i < len; i++) {     // insertion sort the copy
        double v = tmp[i]; int j = i - 1;
        while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = v;
    }

    double med = tmp[len / 2];
    free(tmp);
    return med;
}
```

## Building a seed

Put the source in `seed/examples/` and run:

```text
make seeds
```

This cross‑compiles each seed for the Pi and links it into a flat `.SED` blob in `build/seeds/`. The build **requires the seed to be self‑contained**: work through the arguments, your own local helper functions, the seed C library (above), and the services — but do not reach for OS‑dependent functions or for global/static data outside the seed. If a seed pulls in something it cannot carry, the build stops with an error rather than producing a blob that would crash on the Pi.

The example `.SED` files are copied onto the card image by `make sdimage`, so the seed programs run on real hardware as well as in the emulator.

## A complete example

```basic
10 REM Load and use several seeds
20 SEED A%, "ADD.SED"
30 PRINT "40 + 2 = "; CALL(A%, 40, 2)
40 SEED U%, "UPPER.SED"
50 PRINT "upper: "; CALL$(U%, "berry pi")
60 DIM D(5)
70 FOR I = 0 TO 5: D(I) = I * I: NEXT
80 SEED S%, "SUMARR.SED"
90 PRINT "sum of squares 0..5 = "; CALL(S%, "D")
95 SEED M%, "MEDIAN.SED"
96 PRINT "median of D() = "; CALL(M%, "D")
97 SEED Y%, "DYNARR.SED"
98 PRINT "dynarr sum 1..100 = "; CALL(Y%, 100)
102 DIM E(4)
104 E(0)=5 : E(1)=2 : E(2)=8 : E(3)=1 : E(4)=9
106 SEED Z%, "SORTARR.SED"
108 CALL Z%, "E"
110 PRINT "sorted E: "; E(0); " "; E(1); " "; E(2); " "; E(3); " "; E(4)
120 END
```

Output:

```text
40 + 2 = 42
upper: BERRY PI
sum of squares 0..5 = 55
median of D() = 9
dynarr sum 1..100 = 5050
sorted E: 1 2 5 8 9
```

---
# Appendix A: Error Messages

When something goes wrong, BerryBasiC stops the current operation and prints a message; inside a running program the offending line number is appended in brackets, e.g. `Division by zero (line 30)`. Only the first error of a statement is reported. The messages you may meet:

## Syntax and expressions

| Message | Usually means |
|---------|---------------|
| `Syntax error in expression` | the expression is malformed |
| `Expected a value or expression` | a value was needed here but something else appeared |
| `Expected a command` | the line does not begin with something runnable |
| `That keyword can't be used as a command` | a function‑only keyword was used where a statement was expected |
| `Expected a variable name` | a variable name was required (e.g. after `INPUT`, `LET`) |
| `Expected '='` | an assignment or `PTR#`/`TIME` set is missing its `=` |
| `Expected ')' to close the parameter list` | unbalanced parentheses in a `DEF` |
| `Expected a parameter name` | a `DEF`'s parameter list is malformed |
| `Unexpected text after the statement` | leftover characters after a complete statement |
| `I don't recognise that character` | an illegal character in the source |
| `Expression too complex` | the expression nests deeper than the evaluator allows |

## Types and values

| Message | Usually means |
|---------|---------------|
| `Type mismatch: numbers and text can't be mixed` | a number and a string were combined or compared |
| `Type mismatch: file record is a number` | `INPUT#` read a number into a `$` variable |
| `Type mismatch: file record is a string` | `INPUT#` read a string into a numeric variable |
| `Division by zero` | `/`, `DIV` or `MOD` by 0 |
| `Invalid argument` | e.g. `ASC("")`, or a value out of a function's allowed range |
| `Text string is too long` | a result would exceed 255 characters |

## Control flow

| Message | Usually means |
|---------|---------------|
| `NEXT without a matching FOR` | a stray `NEXT`, or mismatched loop nesting |
| `RETURN without a matching GOSUB` | a `RETURN` reached with no active `GOSUB` |
| `UNTIL without a matching REPEAT` | a stray `UNTIL` |
| `ENDWHILE without a matching WHILE` | a stray `ENDWHILE` |
| `WHEN/OTHERWISE without a matching CASE` | a `CASE` clause outside any `CASE` |
| `CASE without a matching ENDCASE` | a `CASE` block was never closed |
| `IF without a matching ENDIF` | a block `IF` was never closed |
| `ELSE without a matching IF` | a stray block `ELSE` |
| `No such line number` | a jump target that does not exist |
| `No such label` | a jump to a label that is not defined |
| `Expected a line number` / `Expected a line number or label` | a jump target was expected |
| `Expected GOTO or GOSUB after ON` | malformed `ON …` |
| `Expected OF after CASE` | malformed `CASE …` |
| `Expected TO in a FOR statement` | malformed `FOR …` |
| `Block IF can only be used inside a program` | block `IF` typed in direct mode |
| `CASE can only be used inside a program` | `CASE` typed in direct mode |
| `WHILE can only be used inside a program` | `WHILE` typed in direct mode |
| `This can only be used inside a program` | a program‑only statement used in direct mode |

## Capacity ("Out of memory" and the "Too many…" family)

| Message | Usually means |
|---------|---------------|
| `Out of memory` | out of variable space, string heap, program lines, or a stack is full |
| `Too many nested PROC calls` | recursion or nesting beyond 32 `PROC`/`FN` calls |
| `Too many nested function calls` | function‑return stack full |
| `Too many nested REPEAT loops` | more than 16 nested `REPEAT`s |
| `Too many nested WHILE loops` | more than 16 nested `WHILE`s |
| `Too many nested CASE statements` | more than 16 nested `CASE`s |
| `Too many arguments` / `Too many imported modules` / `Too many seeds loaded` | the matching limit was reached |
| `Wrong number of arguments` | a `PROC`/`FN` call's argument count doesn't match its `DEF` |

## Arrays and data

| Message | Usually means |
|---------|---------------|
| `Array index out of range` | a subscript outside the declared bounds (or a negative `DIM` size) |
| `That array is already defined` / `Already exists` | re‑`DIM`ing an existing array |
| `Reserve memory into a numeric variable` | `DIM name size` used with a `$` variable |
| `READ ran out of DATA` | more `READ`s than `DATA` items |

## Files and storage

| Message | Usually means |
|---------|---------------|
| `No storage card found` / `No such file` / `File not found` | the card or file is unavailable |
| `Expected a file name` | a name was required by a file command |
| `File is too big to load` | the program file exceeds the program store |
| `Storage card is full` / `Storage read/write error` / `Disk error` / `File write error` | a filesystem failure |
| `Directory not empty` | `RMDIR` on a non‑empty directory |
| `End of file` | read past the end of a channel |
| `Expected '#' before a file channel` | a channel operand is missing its `#` |
| `File is not a PRINT# record` | `INPUT#` on data not written by `PRINT#` |

## Seeds

| Message | Usually means |
|---------|---------------|
| `Native seeds run on the Pi, not the host build` | seeds were used on a host test build |
| `No such seed` | a `CALL` handle that isn't loaded |
| `Not a valid seed file` / `Seed needs a newer interpreter` / `Seed is too big` | the `.SED` file can't be loaded |
| `Seed handle must be a numeric variable` | `SEED` given a `$` variable for its handle |
| `Imported module not found` | `IMPORT` could not find the named `.BAS` file |

---

# Appendix B: Limits

| Thing | Limit |
|-------|-------|
| Line length | 128 characters |
| Program size | 8192 lines |
| Variable name | 8 characters, including the `%` / `$` suffix |
| String length | 255 characters |
| Variables | 512 |
| Arrays | 16, up to 3 dimensions each |
| String‑array elements (pool) | 512 |
| `GOSUB` nesting | 32 |
| `FOR` nesting | 16 |
| `REPEAT` nesting | 16 |
| `WHILE` nesting | 16 |
| `CASE` nesting | 16 |
| `PROC` / `FN` call depth | 32 |
| `LOCAL` saves (across all active calls) | 96 |
| `PROC` / `FN` definitions | 64 |
| `PROC` / `FN` parameters (arguments) | 16 |
| Imported modules | 16 |
| Open files | 4 |
| Loaded seeds | 8 |
| Seed arguments | 16 |
| Seed heap | 2 MB (default) |

---

# Appendix C: Keyword Quick Reference

Grouped by purpose. Functions are marked *(fn)* and used inside expressions; everything else is a statement or command. String‑valued words end in `$`.

**Program control:** `RUN` · `LIST` · `NEW` · `AUTO` · `RENUMBER` · `EDIT` · `END` · `STOP`

**Output & input:** `PRINT` · `INPUT` · `TAB` · `SPC` · `CLS` · `COLOUR` / `COLOR`

**Assignment & data:** `LET` · `DIM` · `DATA` · `READ` · `RESTORE`

**Control flow:** `IF` · `THEN` · `ELSE` · `ENDIF` · `FOR` · `TO` · `STEP` · `NEXT` · `REPEAT` · `UNTIL` · `WHILE` · `ENDWHILE` · `CASE` · `OF` · `WHEN` · `OTHERWISE` · `ENDCASE` · `GOTO` · `GOSUB` · `RETURN` · `ON` · `EXIT` · `CONTINUE`

**Error handling:** `TRY` · `CATCH` · `ENDTRY` · `RAISE` · `ERR` *(fn)* · `ERR$` *(fn)*

**Procedures & functions:** `DEF` · `PROC` · `FN` · `ENDPROC` · `LOCAL` · `IMPORT`

**Operators (keywords):** `DIV` · `MOD` · `AND` · `OR` · `EOR` · `NOT`

**Numeric functions *(fn)*:** `ABS` · `INT` · `SGN` · `SQR` · `SIN` · `COS` · `TAN` · `ATN` · `ASN` · `ACS` · `LOG` · `EXP` · `DEG` · `RAD` · `RND` · `PI`

**Bit functions *(fn)*:** `SHL` · `SHR` · `ASR` · `ROL` · `ROR`

**String functions *(fn)*:** `LEN` · `ASC` · `VAL` · `INSTR` · `CHR$` · `STR$` · `LEFT$` · `RIGHT$` · `MID$` · `STRING$` · `UPPER$` · `LOWER$` · `TRIM$` · `REPLACE$` · `CONTAINS` · `STARTSWITH` · `ENDSWITH` · `SPLIT` · `JOIN$`

**Constants *(fn)*:** `PI` · `TRUE` · `FALSE`

**Keyboard & cursor *(fn)*:** `GET` · `GET$` · `INKEY` · `INKEY$` · `POS` · `VPOS`

**Mouse:** `MOUSE` (statement) · `MOUSEX` · `MOUSEY` · `MOUSEB` *(fn)*

**Time:** `TIME` (read or set)

**Screen & VDU:** `VDU` · `MODE` · `SCREEN` · `SCREENW` *(fn)* · `SCREENH` *(fn)*

**Sound:** `SOUND` · `SOUND OFF` · `TONE`

**Graphics:** `GCOL` · `PLOT` · `MOVE` · `DRAW` · `CLG` · `POINT` *(fn)* · `LINE` · `RECTANGLE` · `CIRCLE` · `ELLIPSE` · `FILL` · `RGB` *(fn)*

**Sprites:** `GGET` · `GPUT` · `LOADSPRITE` *(fn)* · `SAVESPRITE` · `SPRW` *(fn)* · `SPRH` *(fn)*

**Storage & directories:** `SAVE` · `LOAD` · `CAT` · `DIR` · `DELETE` · `MKDIR` · `CD` · `RMDIR` · `PWD` · `DIROPEN` *(fn)* · `DIRNEXT` *(fn)* · `DIRNAME$` *(fn)* · `DIRTYPE` *(fn)* · `DIRSIZE` *(fn)* · `DIRDATE$` *(fn)* · `DIRTIME$` *(fn)*

**Files (channels):** `OPENIN` *(fn)* · `OPENOUT` *(fn)* · `OPENUP` *(fn)* · `BGET` *(fn)* · `BPUT` · `EOF` *(fn)* · `EXT` *(fn)* · `PTR` (read or set) · `CLOSE` · `PRINT#` · `INPUT#`

**Native seeds:** `SEED` · `CALL` · `CALL$`

**Memory & indirection:** `DIM name size` (reserve) · `?` (byte) · `!` (word) · `$` (string) · `PEEK` *(fn)* · `POKE` · `REM` (comment)

---

*End of the BerryBasiC Language Reference.*
