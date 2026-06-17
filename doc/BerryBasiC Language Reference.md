# BerryBasiC Language Reference

## Introduction

BerryBasiC is a BASIC interpreter running on a RaspberryPi4 supporting:

· Line-numbered programs

· Direct mode commands

· Numeric, integer, and string variables

· Arrays

· Procedures and functions

· Structured loops

· DATA/READ processing

· Console I/O

· Graphics primitives

· BBC BASIC–style VDU commands



# Program Structure

Programs consist of numbered lines.

10 PRINT "HELLO"
20 GOTO 10

Multiple statements may appear on the same line separated by colons.

10 A=10 : B=20 : PRINT A+B

Comments are introduced with REM.

10 REM This is a comment



# Data Types

There are two kinds of value: **numbers** and **strings**. A variable's name
decides which it holds. A name is up to 8 characters, starts with a letter, and
may contain letters and digits. The trailing `%` or `$` counts as part of the
name and as part of that 8-character limit.

## Floating Point

The default numeric type. Numbers are held as double-precision floating point,
so fractions and large values are kept accurately.

    A = 12.5
    B = 3.14159

A plain name (no suffix) is a floating-point variable:

    RADIUS = 2.5
    AREA = PI * RADIUS * RADIUS



## Integer Variables

A name ending in `%` holds a whole number. Any value assigned to it is truncated
towards zero, dropping the fractional part.

    COUNT% = 10
    I% = 123

Truncation happens on assignment:

    I% = 12.9
    PRINT I%

Output:

    12

Integer variables are handy as loop counters and array indices, and make the
intent ("this is a count") clear.



## String Variables

A name ending in `$` holds text. Strings may be up to 255 characters long and
may be empty (`""`).

    NAME$ = "BERRY"
    EMPTY$ = ""

Use `+` to join strings and the string functions (`LEFT$`, `MID$`, `LEN`, ...)
to take them apart.



# Constants

## PI

The ratio of a circle's circumference to its diameter, 3.14159265...

    PRINT PI

Use it with the trigonometric functions, which work in radians (a full turn is
`2 * PI`):

    PRINT SIN(PI / 2)



## TRUE

The result a comparison gives when it holds. Its value is `-1` (every bit set),
which makes it work cleanly with the bitwise operators.

    -1

Example:

    IF TRUE THEN PRINT "YES"



## FALSE

The result a comparison gives when it does not hold. Its value is `0`.

    0

Any non-zero number is treated as true by `IF`, so `TRUE` and `FALSE` are
conveniences, not the only truth values.



# Numeric Literals

## Decimal

A = 123
B = 12.34

## Hexadecimal

BBC BASIC style:

A = &FF
PRINT A

Output:

255



# Assignment

Explicit:

LET A = 10

Implicit:

A = 10

Both forms are equivalent.



# Operators

## Arithmetic

| Operator | Meaning |
|----------|---------|
| `+`      | Add |
| `-`      | Subtract (or, as a prefix, negate) |
| `*`      | Multiply |
| `/`      | Divide (floating-point result) |
| `^`      | Raise to a power |

Operators follow the usual precedence: `^` first, then `*` and `/`, then `+` and
`-`. Use parentheses to group where you need a different order.

Example:

    PRINT 2^8



## Integer Division

`DIV` divides and discards any remainder, giving a whole-number result.

    PRINT 7 DIV 2

Result:

    3



## Modulus

`MOD` gives the remainder left after `DIV`.

    PRINT 7 MOD 2

Result:

    1



## Relational

=
<>
<
\>
<=
\>=

Example:

IF A<>0 THEN PRINT "NONZERO"



## Logical / Bitwise

`AND`, `OR`, `EOR` (exclusive-or) and `NOT` operate bit by bit on 32-bit
integers. Because `TRUE` is -1 (all bits set) and `FALSE` is 0, the same
operators serve as logical connectives in conditions.

| Operator | Meaning |
|----------|---------|
| `AND`    | Bits set in both operands |
| `OR`     | Bits set in either operand |
| `EOR`    | Bits set in exactly one operand |
| `NOT`    | Inverts every bit (so `NOT 0` is -1) |

As bit masks:

    PRINT 6 AND 3        : REM 2

As conditions:

    IF A>0 AND B>0 THEN PRINT "BOTH POSITIVE"



## Shift and Rotate

Bitwise shifts and rotates operate on 32-bit integers, written as functions:

| Function    | Meaning                                             |
| ----------- | --------------------------------------------------- |
| SHL(x, n)   | Shift x left by n bits (zeros in from the right)    |
| SHR(x, n)   | Logical shift right by n bits (zeros in, unsigned)  |
| ASR(x, n)   | Arithmetic shift right by n bits (keeps the sign)   |
| ROL(x, n)   | Rotate left by n bits within 32 bits                |
| ROR(x, n)   | Rotate right by n bits within 32 bits               |

Examples:

    PRINT SHL(1, 4)        : REM 16
    PRINT SHR(256, 4)      : REM 16
    PRINT ASR(-8, 1)       : REM -4  (sign preserved)
    PRINT ROL(1, 1)        : REM 2

Shift counts of 32 or more give 0 for SHL/SHR; a negative count is an error.
Combine with AND/OR to pack and unpack fields, e.g. an RGB colour:

    COL = SHL(R,16) OR SHL(G,8) OR B
    R   = SHR(COL,16) AND 255



## String Concatenation

FULL$ = FIRST$ + LAST$



# PRINT Statement

`PRINT` writes a list of items to the screen, followed by a newline. The
separator between items decides the spacing.

## Basic Output

    PRINT "HELLO"



## Expressions

Items may be any numeric or string expression; numbers are converted to text:

    PRINT A+B



## Multiple Items

A comma between items moves to the next print field. Fields are 8 columns wide,
so columns line up in a table:

    PRINT A, B, C

A semicolon joins items with no space at all:

    PRINT "X="; X; " Y="; Y



## Suppress Newline

A separator (`;` or `,`) at the very end of the line suppresses the closing
newline, so the next `PRINT` continues on the same line:

    PRINT "HELLO";
    PRINT "WORLD"

Output:

    HELLOWORLD

A single quote `'` inside the list forces a newline, which is a compact way to
print several lines from one statement:

    PRINT "Line 1" ' "Line 2"



## TAB

`TAB(n)` moves the print position to column `n` (counting from 0) by emitting
spaces. If the position is already at or past `n`, it does nothing.

    PRINT TAB(10); "value"



## SPC

`SPC(n)` outputs `n` spaces, wherever the print position happens to be.

    PRINT "A"; SPC(5); "B"



# INPUT Statement

`INPUT` reads a line typed by the user and stores it into one or more variables.
By default it shows a `?` prompt.

## Numeric Input

Reads a number. Non-numeric text is read as 0.

    INPUT A



## String Input

Reads text into a string variable. The whole field is taken literally, so it may
contain spaces.

    INPUT NAME$



## Prompt String

A string followed by `;` is printed as the prompt, with a `? ` added after it:

    INPUT "NAME"; NAME$

The interpreter prints:

    NAME? 



## Multiple Variables

Several variables may be filled from one statement; values are separated by
commas on the input line.

    INPUT A,B,C

All the variables are filled from a single typed line. If the user supplies
fewer values than requested, the remaining variables are left as 0 (or empty
text for string variables).



# Branching

## GOTO

GOTO 100

GOTO and GOSUB may also jump to a **label** instead of a line number, so the
target survives RENUMBER and the program reads more clearly.



## Labels

A label is a name introduced with a leading dot, on its own at the start of a
line. Jump to it from GOTO or GOSUB by name (the dot is optional in the jump):

    10 GOTO main
    20 .greet
    30 PRINT "Hello"
    40 RETURN
    50 .main
    60 GOSUB greet
    70 END

Labels are never altered by RENUMBER, so `GOTO done` keeps working however the
program is renumbered.



## IF THEN

IF A=10 THEN PRINT "TEN"



## IF THEN ELSE

IF A=10 THEN PRINT "TEN" ELSE PRINT "OTHER"



## Block IF (multi-line)

When THEN is the last thing on the line, IF starts a multi-line block that runs
until ENDIF. An optional ELSE selects the alternative block. Blocks may be
nested.

    10 IF SCORE>=50 THEN
    20   PRINT "Pass"
    30   PRINT "Well done"
    40 ELSE
    50   PRINT "Try again"
    60 ENDIF

Single-line `IF ... THEN ...` (with statements after THEN) keeps its classic
behaviour; the block form is used only when nothing follows THEN on the line.



## ON GOTO

ON N GOTO 100,200,300

Selection is 1-based.

| N    | Target |
| ---- | ------ |
| 1    | 100    |
| 2    | 200    |
| 3    | 300    |

Values outside the range fall through.



## ON GOSUB

ON N GOSUB 100,200,300



# Subroutines

`GOSUB` and `RETURN` are the classic line-numbered subroutine mechanism. For new
code, named `PROC`/`FN` (below) are usually clearer, but `GOSUB` remains for
compatibility.

## GOSUB

Jumps to a line (or label) and remembers where it came from:

    10 GOSUB 100
    20 END
    100 PRINT "SUBROUTINE"
    110 RETURN

Up to 32 GOSUBs may be nested (active at once).



## RETURN

Returns to the statement immediately after the `GOSUB` that called this
subroutine. A `RETURN` with no matching `GOSUB` is an error.



# FOR Loops

A `FOR` loop counts a variable from a start value to a limit, running the body
once for each value. The limit is tested at the top of each pass, so a loop
whose start is already past the limit runs zero times.

## Basic Loop

The counter goes up by 1 each time, from 1 to 10 inclusive:

    FOR I=1 TO 10
    PRINT I
    NEXT

`NEXT` may name its variable (`NEXT I`) for clarity; this also helps when loops
are nested. Up to 16 `FOR` loops may be nested.



## STEP

`STEP` sets the amount added each pass:

    FOR I=0 TO 20 STEP 2
    PRINT I
    NEXT



## Negative STEP

A negative `STEP` counts down; the loop continues while the counter is at or
above the limit:

    FOR I=10 TO 1 STEP -1
    PRINT I
    NEXT

Nested loops close in reverse order. Naming the variable on `NEXT` makes the
pairing explicit:

    FOR Y=1 TO 3
      FOR X=1 TO 3
        PRINT X*Y
      NEXT X
    NEXT Y



# REPEAT Loops

## REPEAT UNTIL

The body runs first, then the condition is tested, so a `REPEAT` loop always
runs at least once. It repeats until the condition becomes true.

    REPEAT
    A=A+1
    UNTIL A=10

Condition is tested at the end. Use `WHILE` instead when the body should be
skippable from the start.



# WHILE Loops

## WHILE ENDWHILE

WHILE tests its condition **before** each pass, so the body may run zero times.
The loop repeats until the condition is false. Loops may be nested.

    10 N=1
    20 WHILE N<=5
    30   PRINT N
    40   N=N+1
    50 ENDWHILE

Compared with REPEAT...UNTIL (tested at the bottom, always runs once), WHILE is
the right choice when the body should be skipped entirely if the condition is
false to start with.



# CASE Selection

## CASE OF WHEN

CASE selects one branch by matching an expression against the values listed by
each WHEN. A WHEN may list several comma-separated values. OTHERWISE catches
anything that did not match, and ENDCASE closes the statement. The selector may
be numeric or a string.

    10 CASE DAY OF
    20   WHEN 1,7: PRINT "Weekend"
    30   WHEN 6:   PRINT "Almost there"
    40   OTHERWISE PRINT "Weekday"
    50 ENDCASE

Only the first matching WHEN (or OTHERWISE) runs; control then continues after
ENDCASE. Each clause's statements may follow on the same line after a colon, or
on the lines below until the next WHEN/OTHERWISE/ENDCASE.



# Arrays

`DIM` reserves an array. Indices start at 0, so `DIM A(N)` gives `N+1` elements,
`A(0)` through `A(N)` (BBC BASIC semantics). Up to 3 dimensions are supported.
All elements start at 0 (or empty text for string arrays).

## One Dimension

    DIM A(10)

Creates indices:

    A(0) .. A(10)

Assign and read elements by index:

    A(0) = 100
    A(1) = A(0) + 1
    PRINT A(1)



## Two Dimensions

The bounds are given per dimension; `GRID(9,9)` is a 10x10 grid:

    DIM GRID(9,9)
    GRID(2,3) = 7



## Three Dimensions

    DIM CUBE(3,3,3)



## String Arrays

A `$` array holds a string in every element:

    DIM NAME$(20)
    NAME$(0) = "BERRY"



# DATA Processing

`DATA` holds constants inside the program; `READ` copies them into variables one
after another, and `RESTORE` chooses where the next `READ` starts. All the
`DATA` lines in the program form one continuous list, read in line order.

## DATA

Lists values to be read later. `DATA` lines may appear anywhere in the program.

    10 DATA 10,20,30



## READ

Takes the next item(s) from the `DATA` list, in order, and stores them. Reading
past the end of the list is an error.

    READ A,B,C



## String DATA

A `DATA` item may be a quoted string; read it into a string variable. The type
read must match the variable.

    10 DATA "RED","GREEN","BLUE"
    20 READ A$,B$,C$



## RESTORE

Restart from first DATA item:

    RESTORE

Restart from the first item on (or after) a given line, which lets you keep
several independent tables in one program:

    RESTORE 100



# Procedures

## Definition

A procedure is a named block of statements. It is defined with `DEF PROC`name
and ends at `ENDPROC`:

    DEF PROCHELLO
    PRINT "HELLO"
    ENDPROC



## Call

Run a procedure by writing its name (the `PROC` prefix is part of the name):

    PROCHELLO

Control returns to the statement after the call when `ENDPROC` is reached.



## Parameters

Parameters are listed in parentheses and passed by value, so changing a
parameter inside the procedure does not affect the caller's variable:

    DEF PROCADD(A,B)
    PRINT A+B
    ENDPROC
    
    PROCADD(10,20)

A procedure may call itself; recursion works as long as the nesting stays within
limits.



## LOCAL Variables

`LOCAL` makes a variable private to the procedure: its previous value is saved on
entry and restored on `ENDPROC`. This keeps a procedure (including a recursive
one) from disturbing variables of the same name elsewhere.

    DEF PROCTEST
    LOCAL A
    A=100
    ENDPROC

LOCAL variables are restored on exit.



# Functions

A function is like a procedure but returns a value. It is defined with
`DEF FN`name and returns its result with a line beginning `=`.

## Definition

    DEF FNSQUARE(X)
    = X*X

The `=` line both supplies the result and ends the function. Functions take
by-value parameters and may use `LOCAL`, exactly like procedures.



## Usage

A function call is part of an expression — use its result anywhere a value is
allowed:

    PRINT FNSQUARE(5)

Output:

    25



# String Functions

Character positions in strings are 1-based: the first character is position 1.
The length and count arguments are clamped to the string, so asking for more
characters than exist simply returns as many as there are (never an error).

## LEN

Number of characters in a string.

    PRINT LEN("HELLO")

Result:

    5



## ASC

Character code of the first character. `ASC("")` is an error.

    PRINT ASC("A")

Result:

    65



## CHR$

The one-character string for a character code (0-255).

    PRINT CHR$(65)

Result:

    A



## STR$

Converts a number to its printed text form.

    PRINT STR$(123)

Result:

    123



## VAL

Reads a number from the front of a string; leading non-numeric text gives 0.

    PRINT VAL("123")

Result:

    123



## LEFT$

The leftmost `n` characters. If `n` exceeds the length, the whole string is
returned; if `n` is 0 or less, the empty string.

    PRINT LEFT$("HELLO",3)

Result:

    HEL



## RIGHT$

The rightmost `n` characters, clamped the same way as `LEFT$`.

    PRINT RIGHT$("HELLO",2)

Result:

    LO



## MID$

`MID$(s, start)` returns from position `start` to the end; `MID$(s, start, n)`
returns at most `n` characters from that position. `start` is 1-based.

    PRINT MID$("HELLO",2,3)

Result:

    ELL



## STRING$

A string made of `n` copies of another string.

    PRINT STRING$(10,"*")

Result:

    **********



## INSTR

Position of the first occurrence of the second string within the first, or 0 if
not found. An optional third argument gives the 1-based position to start
searching from.

    PRINT INSTR("HELLO","LL")

Result:

    3



# Mathematical Functions

The trigonometric functions work in **radians**, not degrees; use `RAD` and
`DEG` to convert. These single-argument functions allow the parentheses to be
omitted, BBC-style, so `SQR 2` and `SQR(2)` are the same, and the function binds
tighter than the surrounding operators.

## ABS

Absolute value (drops the sign).

    PRINT ABS(-5)        : REM 5

## INT

Largest whole number not greater than X (rounds towards minus infinity).

    PRINT INT(3.7)       : REM 3
    PRINT INT(-3.2)      : REM -4

## SGN

Sign of X: `-1`, `0`, or `1`.

    PRINT SGN(-42)       : REM -1

## SQR

Square root. X must not be negative.

    PRINT SQR(9)         : REM 3

## SIN

Sine of X (X in radians).

    PRINT SIN(PI / 2)    : REM 1

## COS

Cosine of X (X in radians).

## TAN

Tangent of X (X in radians).

## ATN

Arctangent: the angle (in radians) whose tangent is X.

## ASN

Arcsine: the angle (in radians) whose sine is X. X must be between -1 and 1.

## ACS

Arccosine: the angle (in radians) whose cosine is X. X must be between -1 and 1.

## LOG

Natural logarithm (base e). X must be greater than 0.

    PRINT LOG(EXP(1))    : REM 1

## EXP

e raised to the power X (the inverse of `LOG`).

## DEG

Convert radians to degrees.

    PRINT DEG(PI)        : REM 180

## RAD

Convert degrees to radians.

    PRINT RAD(180)       : REM 3.14159...



# Random Numbers

## RND

`RND` returns pseudo-random numbers. Its behaviour depends on the argument:

| Call      | Result |
|-----------|--------|
| `RND(1)`  | A floating-point value in the range 0 (inclusive) to 1 (exclusive) |
| `RND(n)`  | A whole number from 1 to `n`, for `n` greater than 1 |
| `RND(0)`  | Repeats the last value returned by `RND(1)` |
| `RND(-n)` | Seeds the generator from `n` and returns `-n`, giving a repeatable sequence |

    PRINT RND(100)

Returns a random value (here, a whole number from 1 to 100). Seed with a
negative argument when you want the same sequence every run:

    X = RND(-1)          : REM fix the seed
    PRINT RND(6)         : REM same "dice roll" each run



# Keyboard Functions

## GET

Waits for a key press and returns its character code. The program pauses until a
key is pressed.

    A = GET



## GET$

Like `GET`, but returns the key as a single-character string.

    A$ = GET$



## INKEY

`INKEY(n)` waits up to `n` centiseconds (hundredths of a second) for a key. It
returns the character code if a key is pressed in time, or `-1` if none is.

    K = INKEY(100)       : REM wait up to 1 second



## INKEY$

Like `INKEY`, but returns a single-character string, or the empty string `""` on
timeout.

    K$ = INKEY$(100)



# Cursor Functions

## POS

The current text cursor column (X position), counting from 0.

    PRINT POS



## VPOS

The current text cursor row (Y position), counting from 0.

    PRINT VPOS



# Time

`TIME` is a centisecond (hundredth-of-a-second) counter. Read it as a value:

    PRINT TIME

Assign to it to reset or set the counter, typically to time an interval:

    TIME = 0
    REM ... do some work ...
    PRINT "Took"; TIME; "centiseconds"



# Screen Control

## CLS

Clears the text area and moves the cursor to the top-left.

    CLS



## COLOUR / COLOR

Sets the text foreground colour for subsequent `PRINT` output. Both spellings
are accepted. The colour is a logical colour number (0-7 in the default
palette); see the VDU section for the palette and for graphics colours (`GCOL`).

    COLOUR 2

or

    COLOR 2



# VDU

The VDU statement sends a list of byte values to the VDU driver, which controls
the text and graphics screen. Codes 32 to 255 are printable characters; codes
0 to 31 and 127 are control codes that act on the screen, some of which use the
values that follow as parameters.

VDU 65,66,67

Outputs:

ABC

## Sending 16-bit values

A value followed by a semicolon ( ; ) is sent as a 16-bit word - two bytes,
least significant first. A value followed by a comma ( , ), or by nothing, sends
a single byte (its least significant byte). Screen coordinates are 16-bit, so
they are written with semicolons:

VDU 25,5,640;512;

This is the same as PLOT 5,640,512 (draw a line to 640,512).

## VDU code summary

| Code | Params | Meaning |
|------|--------|---------|
| 0    | -      | Null - does nothing |
| 4    | -      | Write text at the text cursor (the default) |
| 5    | -      | Write text at the graphics cursor |
| 6    | -      | Enable output to the screen |
| 7    | -      | Bell (no sound on this hardware) |
| 8    | -      | Move the text cursor back one character |
| 9    | -      | Move the text cursor forward one character |
| 10   | -      | Move the text cursor down one line |
| 11   | -      | Move the text cursor up one line |
| 12   | -      | Clear the text area (same as CLS) |
| 13   | -      | Move the text cursor to the start of the line |
| 16   | -      | Clear the graphics area (same as CLG) |
| 17   | 1      | Define a text colour (same as COLOUR) |
| 18   | 2      | Define a graphics colour (same as GCOL) |
| 19   | 5      | Set an entry in the colour palette |
| 20   | -      | Restore the default colours and palette |
| 21   | -      | Disable output to the screen |
| 22   | 1      | Select the screen mode (same as MODE) |
| 23   | 9      | Define a character, or control the cursor / scrolling |
| 24   | 8      | Define a graphics viewport |
| 25   | 5      | Plot (same as PLOT) |
| 26   | -      | Restore the default viewports and graphics origin |
| 27   | 1      | Send the next value to the screen as a literal character |
| 28   | 4      | Define a text viewport |
| 29   | 4      | Set the graphics origin (same as ORIGIN) |
| 30   | -      | Home the text cursor to the top left |
| 31   | 2      | Move the text cursor (same as TAB(x,y)) |
| 127  | -      | Backspace and delete |

Codes 1, 2 and 3 (printer) and 14 and 15 (auto-paging) are accepted but have no
effect on this hardware.

## Colours and palette

VDU 17,c          : set the text foreground to logical colour c (0 to 7)

VDU 17,128+c      : set the text background to logical colour c

VDU 18,action,c   : set the graphics colour and plot action (same as GCOL)

VDU 19,l,16,r,g,b : set logical colour l to an RGB value (r, g, b each 0 to 255)

VDU 19,l,p,0,0,0  : set logical colour l to default physical colour p (0 to 7)

VDU 20            : restore the default eight colours and palette

## Cursor and viewports

VDU 31,x,y        : move the text cursor to column x, row y (same as TAB(x,y))

VDU 30            : home the text cursor to the top-left of the text viewport

VDU 28,l,b,r,t    : define a text viewport in character cells (left, bottom,
                    right, top)

VDU 24,l;b;r;t;   : define a graphics viewport in graphics coordinates

VDU 26            : restore the full-screen viewports and reset the origin

VDU 29,x;y;       : set the graphics origin (same as ORIGIN x,y)

A text viewport restricts where text is printed and scrolled; a graphics
viewport clips all plotting (and CLG) to a rectangle.

## VDU 5 - text at the graphics cursor

VDU 5 makes all character output - including PRINT - appear at the graphics
cursor instead of the text cursor. Characters are drawn in the current graphics
foreground colour, using the current GCOL plot action, with a transparent
background, and are clipped to the graphics viewport. VDU 4 returns to normal
text output.

MOVE 200,400

VDU 5

PRINT "LABEL"

VDU 4

While in VDU 5 mode the codes 8, 9, 10, 11 move the graphics cursor by one
character, VDU 13 returns it to the left of the graphics viewport, VDU 30 homes
it to the top-left, and VDU 127 backspaces and erases using the graphics
background colour.

## VDU 23 - user-defined characters and control

Define a character (code 32 to 255) from eight rows of eight pixels, top to
bottom. Each row is one byte; a set bit is a lit pixel, with the most significant
bit on the left:

VDU 23,240,126,129,165,129,165,153,129,126

PRINT CHR$(240)

This defines character 240 as a smiley face and then prints it.

Other VDU 23 sub-functions:

VDU 23,1,0;0;0;0;0;   : hide the text cursor (caret)

VDU 23,1,1;0;0;0;0;   : show the text cursor

VDU 23,7,m,d,0;0;0;   : scroll the text viewport one cell
                        (d: 0 = right, 1 = left, 2 = down, 3 = up)

The remaining VDU 23 sub-functions (cursor appearance, cursor-movement flags,
MODE 7 extensions, user-defined screen modes and line thickness) are accepted
but have no effect on this fixed-resolution display.



# Graphics

## MODE

MODE 1

Select graphics mode.



## GCOL

GCOL 1

or

GCOL action,colour

Example:

GCOL 0,3



## PLOT

PLOT code,x,y

Example:

PLOT 69,100,100



## MOVE

Equivalent to:

PLOT 4,x,y

Usage:

MOVE 100,100



## DRAW

Equivalent to:

PLOT 5,x,y

Usage:

DRAW 200,200



## CLG

Clear graphics screen.

CLG



## POINT

Read pixel colour/value.

C = POINT(X,Y)



# Program Control Commands

## RUN

Execute program.

RUN

RUN clears variables before execution.



## LIST

Display the stored program. Keywords are shown in UPPERCASE; variable names,
strings and REM comments are shown as you typed them.

LIST

List a single line, a range, from a line, or up to a line:

LIST 100

LIST 100,200

LIST 100,

LIST ,200



## AUTO

Enter automatic line-numbering. After AUTO, each line you type is given the next
number automatically, so you can just type the program. Press Return on an empty
line (i.e. without adding anything after the offered number) to leave AUTO.

AUTO

or, choosing the first number and the step:

AUTO 100,10

The default is AUTO 10,10 (start at 10, step 10).



## RENUMBER

Renumber the whole program, and fix up every line-number reference (the targets
of GOTO, GOSUB, RESTORE, THEN, ELSE and the lists of ON ... GOTO / ON ... GOSUB)
so the program still runs correctly.

RENUMBER

or, choosing the first number and the step:

RENUMBER 100,10

The default is RENUMBER 10,10. References to a line that does not exist are left
unchanged.



## EDIT

Recall a program line into the input line so you can edit it instead of retyping
it. The line appears ready to edit; press Return to store the changes.

EDIT 150



## Line editing

While typing a line (at the prompt, during AUTO, or after EDIT) the following
keys are available:

| Key            | Action |
|----------------|--------|
| Left / Right   | move the cursor within the line |
| Home / End     | jump to the start / end of the line |
| Backspace      | delete the character before the cursor |
| Delete         | delete the character at the cursor |
| Up / Down      | recall previous / next commands from the history |



## NEW

Delete current program.

NEW

Also clears variables and control stacks.



## STOP

Terminate execution.

STOP



## END

Terminate execution immediately.

END



# Native Seeds

Interpreting BASIC is convenient but not fast. A **seed** is a small piece of compiled native (AArch64) code that a program can load from the storage card and call directly, for the parts of a job that are too slow to interpret, like some image filters, simulations, tight numeric loops. It is the modern equivalent of the
machine-code `CALL`/`USR` of classic BASICs.

A seed is an ordinary C function, cross-compiled and turned into a `.SED` file on your Linux machine (see *Writing a seed* below), then copied onto the card. At run time the program loads it with `SEED` and calls it with `CALL` (for a number) or `CALL$` (for a string).

## SEED

Loads a `.SED` file and stores a *handle* (a small number) in a numeric variable:

    SEED H%, "FILT.SED"

The handle is how later `CALL`s refer to this seed. Up to 4 seeds may be loaded at once. The file is searched on the storage card; `.SED` is assumed if you give no extension. A fresh `RUN` reloads the program's seeds, so the `SEED` lines normally live in the program itself.

## CALL

Calls a loaded seed and returns its numeric result. The first operand is the handle; any further operands are passed to the seed as arguments:

    R = CALL(H%, 40, 2)        : REM -> 42, if the seed adds its two arguments

As a statement (when you only want the side effect and not the result):

    CALL H%, X, Y

Arguments may be numbers or strings, mixed freely. Up to 16 may be passed.

## CALL$

Like `CALL`, but returns a string — the text the seed produced with its `set_return_str` service. If the seed returns no string, the result is empty.

    NAME$ = CALL$(H%, "berry pi")    : REM e.g. -> "BERRY PI"

## What a seed can do

A seed never links against the interpreter; instead it is handed a small set of *services* it may call back into. Through these it can print, read the keyboard, read and write BASIC variables and arrays, and return a string:

| Service          | Purpose                                              |
| ---------------- | ---------------------------------------------------- |
| `putc`, `puts`   | Write characters to the screen                       |
| `getkey`, `inkey`| Read a key (blocking, or with a timeout)             |
| `get_num`, `set_num` | Read / write a numeric BASIC variable            |
| `get_str`, `set_str` | Read / write a string BASIC variable             |
| `num_array`      | Get a direct pointer to a numeric array's storage    |
| `set_return_str` | Provide the string that `CALL$` returns              |
| `time_cs`        | Centiseconds since power-on                          |
| `alloc`, `free`  | Allocate / release working memory from the seed heap |
| `realloc`, `alloc_aligned` | Resize a block / allocate with a given alignment |

Variable and array names are passed exactly as BASIC stores them: upper case, including the `$` or `%` suffix, e.g. `"X"`, `"N%"`, `"A$"`.

The `num_array` service is the real prize for heavy work: it hands the seed a direct pointer to the array's numbers, so a thousand-element array can be crunched at full native speed with no copying. (String data is always copied in and out, because the interpreter is free to move strings around in memory.)

String *arguments* are snapshots: they are valid for the duration of the call. A seed that wants to keep one must copy it.

## Working memory

For tasks that need scratch space — a sorted copy, a lookup table, an image
buffer — a seed allocates from its own heap with the **ordinary C functions**
`malloc`, `calloc` and `free`:

    double *tmp = malloc(n * sizeof(double));
    if (!tmp) return 0;                 // heap exhausted
    ... use tmp ...
    free(tmp);

These read like normal C, but they draw from the *seed heap* rather than any
system allocator (under the hood they route to the services). `malloc` returns a
16-byte-aligned block (suitable for doubles and NEON), or 0 if the heap is full;
`calloc` also zeroes it. The seed heap is separate from BASIC's own variable and
array storage, is 2 MB by default (raise `SEED_HEAP_SIZE` in the interpreter if
you need more), and is wiped clean at every `RUN`/`NEW` — so a seed that forgets
to `free` leaks only within the current run, never permanently. Memory does
persist between calls within a run, so a seed may allocate a table on its first
call and reuse it on later ones.

The full standard allocation set is provided, all backed by the same seed heap:

| Function | Notes |
| -------- | ----- |
| `malloc(size)`                          | uninitialised block, 16-byte aligned |
| `calloc(nmemb, size)`                   | zeroed; checks `nmemb*size` overflow |
| `realloc(ptr, size)`                    | grow/shrink, preserving contents; `realloc(0,n)`=malloc, `realloc(p,0)`=free |
| `reallocarray(ptr, nmemb, size)`        | `realloc` with an overflow-checked size |
| `free(ptr)`                             | release a block (`free(0)` is a no-op) |
| `aligned_alloc(align, size)`            | block aligned to a power-of-two `align` (C11) |
| `memalign(align, size)`                 | legacy spelling of `aligned_alloc` |
| `posix_memalign(&ptr, align, size)`     | POSIX form; returns 0 / `EINVAL` / `ENOMEM` |
| `free_sized(ptr, size)`, `free_aligned_sized(ptr, align, size)` | C23 sized frees (the size hint is ignored) |

Anything `aligned_alloc`/`posix_memalign` returns can be released with the
ordinary `free`. The standard `memset`, `memcpy`, `memmove` and `memcmp` are
available too (a small runtime is linked in for you), so ordinary buffer code
just works. If you prefer, the raw `svc->alloc` / `svc->free` / `svc->realloc` /
`svc->alloc_aligned` services are still there and do exactly the same thing.

## The seed C library

Because a seed is built freestanding (there is no C library to link against),
any standard function it calls must be provided by the *seed runtime*. A useful,
OS-independent subset is — just include the familiar headers:

    #include <stdlib.h>
    #include <string.h>
    #include <ctype.h>

| Header       | Provided |
| ------------ | -------- |
| `<stdlib.h>` | `malloc` `calloc` `realloc` `reallocarray` `free` `free_sized` `free_aligned_sized` `aligned_alloc` `memalign` `posix_memalign`, `qsort` `bsearch`, `atoi` `atol` `strtol` `strtoul`, `abs` `labs`, `rand` `srand` |
| `<string.h>` | `memcpy` `memmove` `memset` `memcmp` `memchr`, `strlen` `strnlen` `strcmp` `strncmp` `strcpy` `strncpy` `strcat` `strncat` `strchr` `strrchr` `strstr` `strdup` `strndup` |
| `<ctype.h>`  | `isdigit` `isalpha` `isalnum` `isspace` `isupper` `islower` `isxdigit` `ispunct` … and `toupper` / `tolower` |

These behave exactly as in standard C; `malloc` and friends draw from the seed
heap, and `qsort`/`bsearch` take the usual comparator. For example, sorting a
BASIC array in place:

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

What is **not** provided is anything that needs an operating system — `printf`,
file I/O, `getenv`, `exit`, threads, `time`, and so on. Calling one of those is a
link error (`undefined reference to 'printf'`), which is the build telling you
the seed reached outside its sandbox. Use the services (`svc`) for I/O instead:
`svc->puts` to print, `svc->getkey`/`svc->inkey` for the keyboard, `svc->time_cs`
for timing. The library lives in `seed/include/` and `seed/runtime/`; adding a
missing pure function is just a declaration in the header and a definition in the
runtime.

## Starting a new seed

The quickest way to begin is to let the project scaffold one for you:

    make newseed NAME=blur          (or just: tools/newseed.sh)

This creates `seed/garden/blur/` containing a starter `blur.c` (an empty seed you
fill in) and a self-contained `Makefile`. From there:

    cd seed/garden/blur
    make                            builds blur.sed
    make install                    copies it where 'make sdimage' bundles it

A seed name is 1–8 characters (a letter first, then letters/digits/`_`) — short
enough for the card's 8.3 file names, and a valid C identifier because it is also
the seed's entry function. The rest of this section explains what goes inside.

## Writing a seed

A seed is a single C function marked with `SEED_EXPORT`, in a file that includes `seed.h`. It receives the services pointer, the argument list, and the argument count, and returns a number:

    #include "seed.h"
    
    SEED_EXPORT(seed)
    {
        double a = (argc > 0) ? argv[0].num : 0;
        double b = (argc > 1) ? argv[1].num : 0;
        return a + b;
    }

Each argument is tagged as a number (`argv[i].num`) or a string (`argv[i].is_str`, with `argv[i].str` / `argv[i].len`). To return a string, call `svc->set_return_str(buf, len)` and read it back with `CALL$`.

A string-uppercasing seed, for example:

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

And one that sums a numeric array passed by name:

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

And one that needs working memory — the median of an array, computed from an
allocated sorted copy so the original is left untouched:

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

## Building a seed

Put the source in `seed/examples/` and run:

    make seeds

This cross-compiles each seed for the Pi and links it into a flat `.SED` blob in `build/seeds/`. The build **requires the seed to be self-contained**: work through the arguments, your own local helper functions, the seed C library (above), and the services — but do not reach for OS-dependent functions or for global/static data outside the seed. If a seed pulls in something it cannot carry, the build stops with an error rather than producing a blob that would crash on the Pi.

The example `.SED` files are copied onto the card image by `make sdimage`, so the seed programs run on real hardware as well as in the emulator.

## A complete example

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

Output:

    40 + 2 = 42
    upper: BERRY PI
    sum of squares 0..5 = 55
    median of D() = 9
    dynarr sum 1..100 = 5050
    sorted E: 1 2 5 8 9

