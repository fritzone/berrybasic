# BerryBasiC Language Reference

## Introduction

BerryBasiC is a BBC BASIC–inspired interpreter supporting:

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



## IF THEN

IF A=10 THEN PRINT "TEN"



## IF THEN ELSE

IF A=10 THEN PRINT "TEN" ELSE PRINT "OTHER"



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

Output raw VDU codes.

VDU 65

Outputs:

A

Special case:

VDU 12

Clears the screen.



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

Display stored program.

LIST



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

 

 