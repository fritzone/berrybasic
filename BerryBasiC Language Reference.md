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

## Floating Point

Default numeric type.

A = 12.5
B = 3.14159



## Integer Variables

Variables ending with %.

COUNT% = 10
I% = 123

Assignments are automatically truncated to integers.

I% = 12.9
PRINT I%

Output:

12



## String Variables

Variables ending with $.

NAME$ = "BERRY"



# Constants

## PI

PRINT PI



## TRUE

Value:

-1

Example:

IF TRUE THEN PRINT "YES"



## FALSE

Value:

0



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

+
\-
*
/
^

Example:

PRINT 2^8



## Integer Division

DIV

Example:

PRINT 7 DIV 2

Result:

3



## Modulus

MOD

Example:

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

AND
OR
EOR
NOT

Example:

PRINT 6 AND 3



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

## Basic Output

PRINT "HELLO"



## Expressions

PRINT A+B



## Multiple Items

PRINT A, B, C



## Suppress Newline

PRINT "HELLO";
PRINT "WORLD"



## TAB

PRINT TAB(10);

Moves output position.



## SPC

PRINT SPC(5);

Outputs spaces.



# INPUT Statement

## Numeric Input

INPUT A



## String Input

INPUT NAME$



## Prompt String

INPUT "NAME"; NAME$

The interpreter prints:

NAME? 



## Multiple Variables

INPUT A,B,C

Input is comma-separated.



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

## GOSUB

10 GOSUB 100
20 END

100 PRINT "SUBROUTINE"
110 RETURN



## RETURN

Returns to the statement following the GOSUB.



# FOR Loops

## Basic Loop

FOR I=1 TO 10
PRINT I
NEXT



## STEP

FOR I=0 TO 20 STEP 2
PRINT I
NEXT



## Negative STEP

FOR I=10 TO 1 STEP -1
PRINT I
NEXT



# REPEAT Loops

## REPEAT UNTIL

REPEAT
A=A+1
UNTIL A=10

Condition is tested at the end.



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

BerryBasiC supports up to 3 dimensions.

## One Dimension

DIM A(10)

Creates indices:

A(0) .. A(10)

BBC BASIC semantics.



## Two Dimensions

DIM GRID(9,9)



## Three Dimensions

DIM CUBE(3,3,3)



## String Arrays

DIM NAME$(20)



# DATA Processing

## DATA

10 DATA 10,20,30



## READ

READ A,B,C



## String DATA

10 DATA "RED","GREEN","BLUE"
20 READ A$,B$,C$



## RESTORE

Restart from first DATA item:

RESTORE

Restart from a specific line:

RESTORE 100



# Procedures

## Definition

DEF PROCHELLO
PRINT "HELLO"
ENDPROC



## Call

PROCHELLO



## Parameters

DEF PROCADD(A,B)
PRINT A+B
ENDPROC

PROCADD(10,20)



## LOCAL Variables

DEF PROCTEST
LOCAL A
A=100
ENDPROC

LOCAL variables are restored on exit.



# Functions

## Definition

DEF FNSQUARE(X)
= X*X



## Usage

PRINT FNSQUARE(5)

Output:

25



# String Functions

## LEN

PRINT LEN("HELLO")

Result:

5



## ASC

PRINT ASC("A")

Result:

65



## CHR$

PRINT CHR$(65)

Result:

A



## STR$

PRINT STR$(123)



## VAL

PRINT VAL("123")



## LEFT$

PRINT LEFT$("HELLO",3)

Result:

HEL



## RIGHT$

PRINT RIGHT$("HELLO",2)

Result:

LO



## MID$

PRINT MID$("HELLO",2,3)

Result:

ELL



## STRING$

PRINT STRING$(10,"*")

Result:

**********



## INSTR

PRINT INSTR("HELLO","LL")

Returns match position.



# Mathematical Functions

## ABS

ABS(X)

## INT

INT(X)

## SGN

SGN(X)

## SQR

SQR(X)

## SIN

SIN(X)

## COS

COS(X)

## TAN

TAN(X)

## ATN

ATN(X)

## ASN

ASN(X)

## ACS

ACS(X)

## LOG

LOG(X)

## EXP

EXP(X)

## DEG

Convert radians to degrees.

DEG(X)

## RAD

Convert degrees to radians.

RAD(X)



# Random Numbers

## RND

PRINT RND(100)

Returns a random value.



# Keyboard Functions

## GET

A = GET

Returns character code.



## GET$

A$ = GET$

Returns a single-character string.



## INKEY

K = INKEY(100)



## INKEY$

K$ = INKEY$(100)



# Cursor Functions

## POS

Current X position.

PRINT POS



## VPOS

Current Y position.

PRINT VPOS



# Time

Read current centisecond timer:

PRINT TIME

Set timer:

TIME = 0



# Screen Control

## CLS

Clear text screen.

CLS



## COLOUR / COLOR

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

 

 