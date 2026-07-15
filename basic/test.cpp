// Unit tests for the portable BASIC interpreter (basic.c), driven through the
// real REPL over an in-memory console (test_console.c). Each test feeds a short
// program, RUNs it, and checks what it printed.
//
//   cmake -S basic -B build/tests && cmake --build build/tests && ctest --test-dir build/tests
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <string>
#include <fstream>
#include <cstdio>
#include <filesystem>

extern "C" {
#include "basic.h"
#include "usb_hid.h"
void        tc_feed(const char *s);
void        tc_reset_output(void);
const char *tc_output(void);
}

using Catch::Matchers::ContainsSubstring;

// Feed raw REPL input and return everything printed, with the startup banner
// stripped so tests can match on program output alone.
static std::string run_raw(std::string input) {
    tc_feed(input.c_str());
    tc_reset_output();
    basic_init();
    basic_repl();                       // returns at end-of-script (con_getline -> -1)
    std::string out = tc_output();
    const std::string banner = "BerryBasic (C) 2026 fritzone\n\n";
    auto p = out.find(banner);
    if (p != std::string::npos) out.erase(p, banner.size());
    return out;
}

// Store `program` (numbered lines) then RUN it; return its output.
static std::string run(std::string program) {
    if (!program.empty() && program.back() != '\n') program += '\n';
    program += "RUN\n";
    return run_raw(program);
}

TEST_CASE("arithmetic and operator precedence") {
    REQUIRE_THAT(run("10 PRINT 6*7"),          ContainsSubstring("42"));
    REQUIRE_THAT(run("10 PRINT 2+3*4"),        ContainsSubstring("14"));
    REQUIRE_THAT(run("10 PRINT (2+3)*4"),      ContainsSubstring("20"));
    REQUIRE_THAT(run("10 PRINT 2^10"),         ContainsSubstring("1024"));
    REQUIRE_THAT(run("10 PRINT -2^2"),         ContainsSubstring("-4"));   // ^ binds tighter than unary -
    REQUIRE_THAT(run("10 PRINT 17 DIV 5"),     ContainsSubstring("3"));
    REQUIRE_THAT(run("10 PRINT 17 MOD 5"),     ContainsSubstring("2"));
}

TEST_CASE("floating point") {
    REQUIRE_THAT(run("10 PRINT 1/2"),   ContainsSubstring("0.5"));
    REQUIRE_THAT(run("10 PRINT 10/4"),  ContainsSubstring("2.5"));
    REQUIRE_THAT(run("10 PRINT SQR(2)"),ContainsSubstring("1.41421356"));
    REQUIRE_THAT(run("10 PRINT PI"),    ContainsSubstring("3.14159265"));
}

TEST_CASE("integer variables truncate toward zero") {
    REQUIRE_THAT(run("10 A%=7/2\n20 PRINT A%"),   ContainsSubstring("3"));
    REQUIRE_THAT(run("10 A%=-7/2\n20 PRINT A%"),  ContainsSubstring("-3"));
}

TEST_CASE("hex constants and bitwise ops") {
    REQUIRE_THAT(run("10 PRINT &FF"),          ContainsSubstring("255"));
    REQUIRE_THAT(run("10 PRINT &F0 AND &0F"),  ContainsSubstring("0"));
    REQUIRE_THAT(run("10 PRINT &F0 OR 5"),     ContainsSubstring("245"));
    REQUIRE_THAT(run("10 PRINT SHL(1,4)"),     ContainsSubstring("16"));
}

TEST_CASE("string functions") {
    REQUIRE_THAT(run("10 PRINT LEN(\"hello\")"),           ContainsSubstring("5"));
    REQUIRE_THAT(run("10 PRINT LEFT$(\"hello\",2)"),       ContainsSubstring("he"));
    REQUIRE_THAT(run("10 PRINT RIGHT$(\"hello\",2)"),      ContainsSubstring("lo"));
    REQUIRE_THAT(run("10 PRINT MID$(\"hello\",2,3)"),      ContainsSubstring("ell"));
    REQUIRE_THAT(run("10 PRINT \"ab\"+\"cd\""),            ContainsSubstring("abcd"));
    REQUIRE_THAT(run("10 PRINT CHR$(65)"),                 ContainsSubstring("A"));
    REQUIRE_THAT(run("10 PRINT ASC(\"A\")"),               ContainsSubstring("65"));
    REQUIRE_THAT(run("10 PRINT INSTR(\"hello\",\"ll\")"),  ContainsSubstring("3"));
    REQUIRE_THAT(run("10 PRINT VAL(\"3.5\")+1"),           ContainsSubstring("4.5"));
}

TEST_CASE("relational and logical operators (TRUE = -1)") {
    REQUIRE_THAT(run("10 PRINT 3 < 5"),          ContainsSubstring("-1"));
    REQUIRE_THAT(run("10 PRINT 5 < 3"),          ContainsSubstring("0"));
    REQUIRE_THAT(run("10 PRINT (1=1) AND (2=2)"),ContainsSubstring("-1"));
    REQUIRE_THAT(run("10 PRINT NOT 0"),          ContainsSubstring("-1"));
}

TEST_CASE("IF / THEN / ELSE") {
    REQUIRE_THAT(run("10 IF 1 THEN PRINT \"yes\" ELSE PRINT \"no\""), ContainsSubstring("yes"));
    REQUIRE_THAT(run("10 IF 0 THEN PRINT \"yes\" ELSE PRINT \"no\""), ContainsSubstring("no"));
}

TEST_CASE("FOR / NEXT loop") {
    std::string out = run("10 FOR I=1 TO 5\n20 PRINT I;\n30 NEXT");
    REQUIRE_THAT(out, ContainsSubstring("12345"));
}

TEST_CASE("STEP and negative STEP") {
    REQUIRE_THAT(run("10 FOR I=0 TO 10 STEP 2\n20 PRINT I;\n30 NEXT"), ContainsSubstring("0246810"));
    REQUIRE_THAT(run("10 FOR I=3 TO 1 STEP -1\n20 PRINT I;\n30 NEXT"), ContainsSubstring("321"));
}

TEST_CASE("REPEAT / UNTIL and WHILE / ENDWHILE") {
    REQUIRE_THAT(run("10 I=1\n20 REPEAT\n30 PRINT I;\n40 I=I+1\n50 UNTIL I>3"), ContainsSubstring("123"));
    REQUIRE_THAT(run("10 I=1\n20 WHILE I<=3\n30 PRINT I;\n40 I=I+1\n50 ENDWHILE"), ContainsSubstring("123"));
}

TEST_CASE("arrays") {
    std::string out = run("10 DIM A(5)\n20 A(2)=99\n30 A(3)=A(2)+1\n40 PRINT A(3)");
    REQUIRE_THAT(out, ContainsSubstring("100"));
}

TEST_CASE("GOSUB / RETURN and GOTO") {
    std::string out = run("10 GOSUB 100\n20 PRINT \"back\"\n30 END\n100 PRINT \"sub\"\n110 RETURN");
    REQUIRE_THAT(out, ContainsSubstring("sub"));
    REQUIRE_THAT(out, ContainsSubstring("back"));
}

TEST_CASE("DATA / READ / RESTORE") {
    std::string out = run("10 READ A,B,C\n20 PRINT A+B+C\n30 DATA 10,20,30");
    REQUIRE_THAT(out, ContainsSubstring("60"));
}

TEST_CASE("new-style functions: DEF fn / assign name / END fn") {
    std::string out = run(
        "10 PRINT square(12)\n"
        "20 END\n"
        "30 DEF fn square(x)\n"
        "40 square = x*x\n"
        "50 END fn");
    REQUIRE_THAT(out, ContainsSubstring("144"));
}

TEST_CASE("recursive function") {
    std::string out = run(
        "10 PRINT fact(5)\n"
        "20 END\n"
        "30 DEF fn fact(n)\n"
        "40 IF n<=1 THEN fact=1 ELSE fact=n*fact(n-1)\n"
        "50 END fn");
    REQUIRE_THAT(out, ContainsSubstring("120"));
}

TEST_CASE("string function and LOCAL") {
    std::string out = run(
        "10 n=99\n"
        "20 PRINT greet$(\"Bo\")\n"
        "30 PRINT n\n"
        "40 END\n"
        "50 DEF fn greet$(who$)\n"
        "60 LOCAL n\n"
        "70 n=1\n"
        "80 greet$ = \"Hi \"+who$\n"
        "90 END fn");
    REQUIRE_THAT(out, ContainsSubstring("Hi Bo"));
    REQUIRE_THAT(out, ContainsSubstring("99"));   // LOCAL n did not clobber the outer n
}

TEST_CASE("classic FN form still works") {
    std::string out = run("10 PRINT FNsq(9)\n20 END\n30 DEF FNsq(x)\n40 = x*x");
    REQUIRE_THAT(out, ContainsSubstring("81"));
}

TEST_CASE("procedures with parameters") {
    std::string out = run("10 PROCadd(3,4)\n20 END\n30 DEF PROCadd(a,b)\n40 PRINT a+b\n50 ENDPROC");
    REQUIRE_THAT(out, ContainsSubstring("7"));
}

TEST_CASE("modules: IMPORT exposes functions and keeps its own line numbers") {
    {
        std::ofstream f("TESTMOD.BAS");
        f << "10 DEF fn triple(x)\n"
             "20 triple = x*3\n"
             "30 END fn\n";
    }
    // Main and module both use lines 10-30; main's GOTO must stay in main.
    std::string out = run(
        "10 IMPORT \"TESTMOD\"\n"
        "20 PRINT triple(4)\n"
        "30 GOTO 50\n"
        "40 PRINT \"BAD\"\n"
        "50 PRINT \"OK\"");
    std::remove("TESTMOD.BAS");
    REQUIRE_THAT(out, ContainsSubstring("12"));
    REQUIRE_THAT(out, ContainsSubstring("OK"));
    REQUIRE_THAT(out, !ContainsSubstring("BAD"));
}

TEST_CASE("errors are reported, not fatal") {
    REQUIRE_THAT(run("10 GOTO 999"),      ContainsSubstring("No such line"));
    REQUIRE_THAT(run("10 PRINT 1+\"a\""), ContainsSubstring("mismatch"));
}

TEST_CASE("immediate-mode expressions") {
    REQUIRE_THAT(run_raw("PRINT 2+2\n"),        ContainsSubstring("4"));
    REQUIRE_THAT(run_raw("PRINT \"hi\"\n"),     ContainsSubstring("hi"));
}

TEST_CASE("INPUT reads from the stream") {
    // The value on the line after RUN is consumed by INPUT.
    std::string out = run_raw("10 INPUT A\n20 PRINT A*2\nRUN\n21\n");
    REQUIRE_THAT(out, ContainsSubstring("42"));
}

TEST_CASE("CASE / OF / WHEN / OTHERWISE") {
    std::string p =
        "10 x=2\n"
        "20 CASE x OF\n"
        "30 WHEN 1: PRINT \"one\"\n"
        "40 WHEN 2: PRINT \"two\"\n"
        "50 OTHERWISE PRINT \"other\"\n"
        "60 ENDCASE\n";
    REQUIRE_THAT(run(p), ContainsSubstring("two"));
}

TEST_CASE("ON x GOSUB") {
    std::string p =
        "10 ON 2 GOSUB 100,200\n"
        "20 END\n"
        "100 PRINT \"a\":RETURN\n"
        "200 PRINT \"b\":RETURN\n";
    REQUIRE_THAT(run(p), ContainsSubstring("b"));
}

TEST_CASE("more numeric functions") {
    REQUIRE_THAT(run("10 PRINT ABS(-5)"),      ContainsSubstring("5"));
    REQUIRE_THAT(run("10 PRINT INT(3.9)"),     ContainsSubstring("3"));
    REQUIRE_THAT(run("10 PRINT SGN(-3)"),      ContainsSubstring("-1"));
    REQUIRE_THAT(run("10 PRINT INT(SIN(0))"),  ContainsSubstring("0"));
}

TEST_CASE("more string functions") {
    REQUIRE_THAT(run("10 PRINT STR$(42)"),           ContainsSubstring("42"));
    REQUIRE_THAT(run("10 PRINT STRING$(3,\"ab\")"),  ContainsSubstring("ababab"));
    REQUIRE_THAT(run("10 PRINT LEFT$(\"hi\",9)"),    ContainsSubstring("hi"));   // count clamps
}

TEST_CASE("multi-dimensional arrays and nested loops") {
    std::string p =
        "10 DIM M(2,2)\n"
        "20 FOR I=0 TO 2\n"
        "30 FOR J=0 TO 2\n"
        "40 M(I,J)=I*10+J\n"
        "50 NEXT\n"
        "60 NEXT\n"
        "70 PRINT M(2,1)\n";
    REQUIRE_THAT(run(p), ContainsSubstring("21"));
}

TEST_CASE("LIST reproduces stored lines but not module lines") {
    // Prime a module, IMPORT it, run, then LIST: LIST shows only the main lines.
    {
        std::ofstream f("LMOD.BAS");
        f << "10 DEF fn one()\n20 one=1\n30 END fn\n";
    }
    std::string out = run_raw(
        "10 IMPORT \"LMOD\"\n"
        "20 PRINT one()\n"
        "RUN\n"
        "LIST\n");
    std::remove("LMOD.BAS");
    REQUIRE_THAT(out, ContainsSubstring("1"));
    REQUIRE_THAT(out, ContainsSubstring("IMPORT"));   // main line is listed
    REQUIRE_THAT(out, !ContainsSubstring("DEF fn one"));  // module line is not
}

// ---------------------------------------------------------------------------
// The tests below drive parts of the interpreter whose backend is stubbed on
// the host (graphics, sprites) or that touch the filesystem (files, dirs). The
// statement *parsing* still runs in basic.c, which is what we're covering.
// ---------------------------------------------------------------------------

TEST_CASE("graphics statements parse and run") {
    std::string p =
        "10 MODE 0\n"
        "20 GCOL 255,0,0\n"
        "30 GCOL 0,5\n"
        "40 COLOUR 5,255,140,0\n"
        "50 COLOUR 3\n"
        "60 MOVE 0,0\n"
        "70 DRAW 100,100\n"
        "80 LINE 0,0,100,100\n"
        "90 RECTANGLE 10,10,20,20\n"
        "100 RECTANGLE FILL 10,10,20,20\n"
        "110 CIRCLE 50,50,10\n"
        "120 CIRCLE FILL 50,50,10\n"
        "130 ELLIPSE 50,50,20,10\n"
        "140 ELLIPSE FILL 50,50,20,10\n"
        "150 FILL 5,5\n"
        "160 PLOT 69,10,10\n"
        "170 CLG\n"
        "180 PRINT \"C=\";POINT(5,5)\n"
        "190 PRINT \"GFX-OK\"\n";
    REQUIRE_THAT(run(p), ContainsSubstring("GFX-OK"));
}

TEST_CASE("sprites: GGET / GPUT parse and run") {
    std::string p =
        "10 DIM S% 80000\n"
        "20 GGET S%,10,10,50,50\n"
        "30 GPUT S%,60,60\n"
        "40 PRINT \"SPR-OK\"\n";
    REQUIRE_THAT(run(p), ContainsSubstring("SPR-OK"));
}

TEST_CASE("memory reservation and ?/!/$ indirection") {
    std::string p =
        "10 DIM B% 255\n"
        "20 ?B%=65\n"
        "30 B%?1=66\n"
        "40 PRINT \"a\";?B%;\"b\";B%?1\n"
        "50 !B%=305419896\n"          // 0x12345678
        "60 PRINT \"lo\";B%?0\n"
        "70 $B%=\"HI\"\n"
        "80 PRINT $B%\n";
    std::string out = run(p);
    REQUIRE_THAT(out, ContainsSubstring("a65b66"));
    REQUIRE_THAT(out, ContainsSubstring("lo120"));   // low byte of 0x12345678 = 0x78 = 120
    REQUIRE_THAT(out, ContainsSubstring("HI"));
}

TEST_CASE("labels: GOTO by name") {
    std::string p =
        "10 GOTO skip\n"
        "20 PRINT \"BAD\"\n"
        "30 .skip\n"
        "40 PRINT \"GOOD\"\n";
    std::string out = run(p);
    REQUIRE_THAT(out, ContainsSubstring("GOOD"));
    REQUIRE_THAT(out, !ContainsSubstring("BAD"));
}

TEST_CASE("string comparison operators") {
    REQUIRE_THAT(run("10 IF \"abc\" < \"abd\" THEN PRINT \"LT\""), ContainsSubstring("LT"));
    REQUIRE_THAT(run("10 IF \"xy\" = \"xy\" THEN PRINT \"EQ\""),   ContainsSubstring("EQ"));
    REQUIRE_THAT(run("10 IF \"b\" > \"a\" THEN PRINT \"GT\""),     ContainsSubstring("GT"));
}

TEST_CASE("VDU and RESTORE") {
    REQUIRE_THAT(run("10 VDU 65,66,67"), ContainsSubstring("ABC"));
    std::string p =
        "10 READ A\n20 READ B\n30 RESTORE\n40 READ C\n"
        "50 PRINT A;B;C\n60 DATA 5,6\n";
    REQUIRE_THAT(run(p), ContainsSubstring("565"));   // A=5 B=6, RESTORE, C=5 again
}

TEST_CASE("MOUSE and RND run") {
    REQUIRE_THAT(run("10 MOUSE X,Y,B\n20 PRINT \"M\";X\n"),        ContainsSubstring("M0"));
    REQUIRE_THAT(run("10 X=RND(-1)\n20 A=RND(1)\n30 PRINT \"R\""), ContainsSubstring("R"));
}

TEST_CASE("multi-line block IF / ELSE / ENDIF") {
    std::string p =
        "10 IF 0 THEN\n"
        "20 PRINT \"BAD\"\n"
        "30 ELSE\n"
        "40 PRINT \"GOOD\"\n"
        "50 ENDIF\n";
    std::string out = run(p);
    REQUIRE_THAT(out, ContainsSubstring("GOOD"));
    REQUIRE_THAT(out, !ContainsSubstring("BAD"));
}

TEST_CASE("CASE OTHERWISE branch") {
    std::string p =
        "10 x=9\n"
        "20 CASE x OF\n"
        "30 WHEN 1: PRINT \"one\"\n"
        "40 OTHERWISE PRINT \"other\"\n"
        "50 ENDCASE\n";
    REQUIRE_THAT(run(p), ContainsSubstring("other"));
}

TEST_CASE("byte-level file I/O") {
    std::string p =
        "10 C=OPENOUT(\"BIO.DAT\")\n"
        "20 BPUT#C,65\n"
        "30 BPUT#C,66\n"
        "40 BPUT#C,\"CD\"\n"
        "50 PRINT \"len\";EXT#C\n"
        "60 CLOSE#C\n"
        "70 C=OPENIN(\"BIO.DAT\")\n"
        "80 REPEAT\n"
        "90 PRINT \"b\";BGET#C\n"
        "100 UNTIL EOF#C\n"
        "110 PTR#C=1\n"
        "120 PRINT \"at\";PTR#C\n"
        "130 CLOSE#C\n";
    std::string out = run(p);
    std::remove("BIO.DAT");
    REQUIRE_THAT(out, ContainsSubstring("len4"));
    REQUIRE_THAT(out, ContainsSubstring("b65"));
    REQUIRE_THAT(out, ContainsSubstring("b68"));   // 'D'
    REQUIRE_THAT(out, ContainsSubstring("at1"));
}

TEST_CASE("record I/O: PRINT# and INPUT#") {
    std::string p =
        "10 C=OPENOUT(\"REC.DAT\")\n"
        "20 PRINT#C, 42, \"hello\"\n"
        "30 CLOSE#C\n"
        "40 C=OPENIN(\"REC.DAT\")\n"
        "50 INPUT#C, N, S$\n"
        "60 CLOSE#C\n"
        "70 PRINT N; S$\n";
    std::string out = run(p);
    std::remove("REC.DAT");
    REQUIRE_THAT(out, ContainsSubstring("42"));
    REQUIRE_THAT(out, ContainsSubstring("hello"));
}

TEST_CASE("SAVE / LOAD round trip") {
    std::string out = run_raw(
        "10 PRINT 7*6\n"
        "SAVE \"RT\"\n"
        "NEW\n"
        "LOAD \"RT\"\n"
        "RUN\n");
    std::remove("RT.BAS");
    REQUIRE_THAT(out, ContainsSubstring("42"));
}

TEST_CASE("file errors are reported") {
    REQUIRE_THAT(run_raw("LOAD \"NOSUCH\"\n"),     ContainsSubstring("not found"));
    REQUIRE_THAT(run_raw("DELETE \"NOSUCH.XYZ\"\n"),ContainsSubstring("not found"));
}

TEST_CASE("RENUMBER renumbers stored lines") {
    std::string out = run_raw(
        "10 PRINT 1\n"
        "15 PRINT 2\n"
        "RENUMBER\n"
        "LIST\n");
    REQUIRE_THAT(out, ContainsSubstring("20 PRINT 2"));   // 15 -> 20
}

TEST_CASE("AUTO line numbering") {
    std::string out = run_raw(
        "AUTO 100,5\n"
        "PRINT 111\n"
        "\n"                        // empty entry leaves AUTO mode
        "LIST\n");
    REQUIRE_THAT(out, ContainsSubstring("100 PRINT 111"));
}

TEST_CASE("directory create / change / remove") {
    namespace fs = std::filesystem;
    fs::path saved = fs::current_path();
    fs::remove_all("TDX");                       // clean any leftover
    std::string out = run_raw(
        "MKDIR \"TDX\"\n"
        "CD \"TDX\"\n"
        "PWD\n"
        "CD \"..\"\n"
        "RMDIR \"TDX\"\n"
        "PRINT \"DIR-OK\"\n");
    fs::current_path(saved);                     // CD moved the process CWD; restore it
    REQUIRE_THAT(out, ContainsSubstring("DIR-OK"));
}

TEST_CASE("trig and transcendental functions") {
    REQUIRE_THAT(run("10 PRINT COS(0)"),                ContainsSubstring("1"));
    REQUIRE_THAT(run("10 PRINT TAN(0)"),                ContainsSubstring("0"));
    REQUIRE_THAT(run("10 PRINT EXP(0)"),                ContainsSubstring("1"));
    REQUIRE_THAT(run("10 PRINT ATN(1)"),                ContainsSubstring("0.785"));
    REQUIRE_THAT(run("10 PRINT INT(LOG(EXP(1))+0.5)"),  ContainsSubstring("1"));
    REQUIRE_THAT(run("10 PRINT DEG(PI)"),               ContainsSubstring("180"));
    REQUIRE_THAT(run("10 PRINT INT(RAD(180)*100)"),     ContainsSubstring("314"));
    REQUIRE_THAT(run("10 PRINT ASN(0)"),                ContainsSubstring("0"));
    REQUIRE_THAT(run("10 PRINT ACS(1)"),                ContainsSubstring("0"));
}

TEST_CASE("word indirection read and packed GCOL") {
    REQUIRE_THAT(run("10 DIM B% 16\n20 !B%=1000\n30 PRINT !B%"), ContainsSubstring("1000"));
    REQUIRE_THAT(run("10 C=RGB(10,20,30)\n20 GCOL C\n30 PRINT \"PK\""), ContainsSubstring("PK"));
}

TEST_CASE("garbage collector runs under heavy string churn") {
    std::string p =
        "10 FOR I=1 TO 400\n"
        "20 A$=STRING$(200,\"x\")\n"
        "30 B$=A$+\"y\"\n"
        "40 NEXT\n"
        "50 PRINT \"GC-\";LEN(B$)\n";
    REQUIRE_THAT(run(p), ContainsSubstring("GC-201"));
}

TEST_CASE("syntax errors name the offending token") {
    REQUIRE_THAT(run("10 PRINT )"),   ContainsSubstring("Expected"));
    REQUIRE_THAT(run("10 A="),        ContainsSubstring("Expected"));
    REQUIRE_THAT(run("10 NEXT"),  ContainsSubstring("NEXT"));
    REQUIRE_THAT(run("10 CLS 9"), ContainsSubstring("Unexpected"));  // trailing junk after CLS
}

TEST_CASE("RENUMBER remaps GOTO targets") {
    std::string out = run_raw(
        "5 GOTO 15\n"
        "15 PRINT \"Y\"\n"
        "RENUMBER\n"
        "LIST\n");
    REQUIRE_THAT(out, ContainsSubstring("GOTO 20"));   // 5->10, 15->20; GOTO 15 -> GOTO 20
}

TEST_CASE("EDIT recalls a stored line") {
    std::string out = run_raw(
        "10 PRINT 1\n"
        "EDIT 10\n"
        "\n"                 // accept the recalled line unchanged
        "LIST\n");
    REQUIRE_THAT(out, ContainsSubstring("PRINT 1"));
}

TEST_CASE("block IF taking the THEN branch") {
    std::string p =
        "10 IF 1 THEN\n"
        "20 PRINT \"TAKEN\"\n"
        "30 ENDIF\n"
        "40 PRINT \"AFTER\"\n";
    std::string out = run(p);
    REQUIRE_THAT(out, ContainsSubstring("TAKEN"));
    REQUIRE_THAT(out, ContainsSubstring("AFTER"));
}

TEST_CASE("block IF: skip past ELSE, and skip a nested block IF") {
    // THEN taken -> the ELSE branch must be skipped over at run time.
    std::string o1 = run(
        "10 IF 1 THEN\n"
        "20 PRINT \"T\"\n"
        "30 ELSE\n"
        "40 PRINT \"E\"\n"
        "50 ENDIF\n");
    REQUIRE_THAT(o1, ContainsSubstring("T"));
    REQUIRE_THAT(o1, !ContainsSubstring("E"));
    // Condition false: the skipped THEN branch itself contains a nested block IF.
    std::string o2 = run(
        "10 IF 0 THEN\n"
        "20 IF 1 THEN\n"
        "30 PRINT \"X\"\n"
        "40 ENDIF\n"
        "50 ELSE\n"
        "60 PRINT \"ELSE\"\n"
        "70 ENDIF\n");
    REQUIRE_THAT(o2, ContainsSubstring("ELSE"));
    REQUIRE_THAT(o2, !ContainsSubstring("X"));
}

TEST_CASE("expect() errors name the token, and glued func+number") {
    REQUIRE_THAT(run("10 PRINT (1+2"), ContainsSubstring("Expected"));  // missing ')'
    REQUIRE_THAT(run("10 PRINT SQR9"), ContainsSubstring("3"));         // SQR9 == SQR 9
}

TEST_CASE("SAVESPRITE writes an image file") {
    namespace fs = std::filesystem;
    std::string p =
        "10 DIM S% 100\n"
        "20 !S%=1\n"                 // width  = 1
        "30 S%!4=1\n"                // height = 1
        "40 S%!8=&FF804020\n"        // one RGBA pixel
        "50 SAVESPRITE S%,\"SPR.BMP\"\n"
        "60 PRINT \"SS-OK\"\n";
    std::string out = run(p);
    bool made = fs::exists("SPR.BMP");
    std::remove("SPR.BMP");
    REQUIRE_THAT(out, ContainsSubstring("SS-OK"));
    REQUIRE(made);
}

TEST_CASE("directory enumeration reports name, size, date, time") {
    { std::ofstream f("ENUMME.DAT"); f << "hi"; }
    std::string p =
        "10 IF DIROPEN(\".\") THEN 30\n"
        "20 PRINT \"OF\":END\n"
        "30 IF DIRNEXT=0 THEN 60\n"
        "40 IF DIRNAME$=\"ENUMME.DAT\" THEN PRINT \"SZ\";DIRSIZE;\"@\";DIRDATE$;\"/\";DIRTIME$\n"
        "50 GOTO 30\n"
        "60 PRINT \"ENUM-OK\"\n";
    std::string out = run(p);
    std::remove("ENUMME.DAT");
    REQUIRE_THAT(out, ContainsSubstring("ENUM-OK"));
    REQUIRE_THAT(out, ContainsSubstring("SZ2@"));   // the 2-byte file was found
}

TEST_CASE("CAT shows a coloured type icon, a size and a summary") {
    { std::ofstream f("CATME.BAS"); f << "10 PRINT 1\n"; }
    std::string out = run_raw("CAT\n");             // no paging on the host (con_rows=0)
    std::remove("CATME.BAS");
    REQUIRE_THAT(out, ContainsSubstring("[BAS] CATME.BAS"));   // icon + name
    REQUIRE_THAT(out, ContainsSubstring("total"));            // footer summary line
}

TEST_CASE("CAT SIMPLE lists plain names without icons") {
    { std::ofstream f("CATME2.BAS"); f << "x\n"; }
    std::string out = run_raw("CAT SIMPLE\n");
    std::remove("CATME2.BAS");
    REQUIRE_THAT(out, ContainsSubstring("CATME2.BAS"));
    REQUIRE_THAT(out, !ContainsSubstring("[BAS] CATME2.BAS"));  // plain, not the rich form
}

// --- Sound: the portable queued/background tone player ----------------------
// These reach past PRINT into the engine itself (sound_pump / sound_cur_freq /
// sound_queued from basic.h). run() leaves the enqueued notes un-pumped (the
// program's last statement queues them after the loop's final pump), so a test
// pumps explicitly. test_console's con_micros advances 10 ms per call, so a
// handful of pumps is enough to expire a short note.

TEST_CASE("SOUND and TONE parse and enqueue") {
    REQUIRE_THAT(run("10 SOUND 1,-15,53,20"), !ContainsSubstring("rror"));
    REQUIRE(sound_queued() >= 1);
    REQUIRE_THAT(run("10 TONE 440,100"),      !ContainsSubstring("rror"));
    REQUIRE(sound_queued() >= 1);
}

TEST_CASE("BBC pitch maps to the right frequency") {
    run("10 SOUND 1,-15,89,20");     // pitch 89 = A above middle C
    sound_pump();                    // start the queued note
    int f = sound_cur_freq();
    REQUIRE(f >= 438);               // 440 Hz, allow rounding slack
    REQUIRE(f <= 442);

    run("10 SOUND 1,-15,53,20");     // pitch 53 = middle C (~261.6 Hz)
    sound_pump();
    f = sound_cur_freq();
    REQUIRE(f >= 259);
    REQUIRE(f <= 264);
}

TEST_CASE("TONE plays the exact frequency on channel 0") {
    run("10 TONE 1000,20");
    sound_pump();
    REQUIRE(sound_cur_freq() == 1000);
    REQUIRE(sound_cur_vol()  == 15);   // default volume
    run("10 TONE 500,20,7");
    sound_pump();
    REQUIRE(sound_cur_freq() == 500);
    REQUIRE(sound_cur_vol()  == 7);
}

TEST_CASE("a note stops after its duration elapses") {
    run("10 TONE 800,10");             // 10 ms; test_console adds 10 ms per pump
    sound_pump();                      // t~=+: note starts
    REQUIRE(sound_cur_freq() == 800);
    for (int i = 0; i < 5; i++) sound_pump();   // advance well past 10 ms
    REQUIRE(sound_cur_freq() == 0);    // silent again
}

TEST_CASE("notes on one channel play back to back") {
    // 100 ms notes; test_console advances 10 ms per pump, so the first survives
    // several pumps before the second takes over.
    run("10 TONE 400,100\n20 TONE 600,100");
    REQUIRE(sound_queued() >= 2);
    sound_pump();
    REQUIRE(sound_cur_freq() == 400);           // first note still playing
    for (int i = 0; i < 15; i++) sound_pump();  // let the first expire, start the second
    REQUIRE(sound_cur_freq() == 600);           // second note
}

TEST_CASE("lower-numbered channel is the audible one") {
    run("10 SOUND 3,-15,89,20\n20 SOUND 1,-15,53,20");
    sound_pump();
    // Both channels are sounding; channel 1 (middle C) wins over channel 3 (A).
    int f = sound_cur_freq();
    REQUIRE(f >= 259);
    REQUIRE(f <= 264);
}

TEST_CASE("SOUND OFF flushes the queue and silences") {
    run("10 SOUND 1,-15,89,20\n20 SOUND 1,-15,53,20\n30 SOUND OFF");
    REQUIRE(sound_queued() == 0);
    sound_pump();
    REQUIRE(sound_cur_freq() == 0);
}

TEST_CASE("a fresh RUN silences leftover sound") {
    run("10 TONE 700,20");
    sound_pump();
    REQUIRE(sound_cur_freq() == 700);
    run("10 PRINT 1");                 // a new RUN resets the sound engine
    REQUIRE(sound_queued() == 0);
    REQUIRE(sound_cur_freq() == 0);
}

// --- SCREEN: runtime resolution switch with restore-on-finish ---------------
// The in-memory console backend (test_console.c) tracks a current resolution and
// mirrors the kernel's clamp/restore, so these exercise the interpreter's SCREEN
// statement, the SCREENW/SCREENH functions, and the auto-restore at end of RUN.

TEST_CASE("SCREEN sets the resolution and SCREENW/SCREENH read it back") {
    REQUIRE_THAT(run("10 SCREEN 640,480\n20 PRINT SCREENW;\"x\";SCREENH"),
                 ContainsSubstring("640x480"));
}

TEST_CASE("startup resolution is reported before any SCREEN") {
    REQUIRE_THAT(run("10 PRINT SCREENW;\"x\";SCREENH"),
                 ContainsSubstring("1280x1024"));
}

TEST_CASE("SCREEN resolution is restored when the program finishes") {
    // The program switches to 320x240; after RUN returns, an immediate query must
    // see the startup resolution again.
    std::string out = run_raw(
        "10 SCREEN 320,240\n"
        "20 PRINT \"IN \";SCREENW;\"x\";SCREENH\n"
        "RUN\n"
        "PRINT \"OUT \";SCREENW;\"x\";SCREENH\n");
    REQUIRE_THAT(out, ContainsSubstring("IN 320x240"));
    REQUIRE_THAT(out, ContainsSubstring("OUT 1280x1024"));
}

TEST_CASE("bare SCREEN restores the startup resolution mid-program") {
    REQUIRE_THAT(run("10 SCREEN 640,480\n20 SCREEN\n30 PRINT SCREENW;\"x\";SCREENH"),
                 ContainsSubstring("1280x1024"));
}

TEST_CASE("SCREEN clamps out-of-range sizes") {
    REQUIRE_THAT(run("10 SCREEN 5000,4000\n20 PRINT SCREENW;\"x\";SCREENH"),
                 ContainsSubstring("1920x1080"));
    REQUIRE_THAT(run("10 SCREEN 8,8\n20 PRINT SCREENW;\"x\";SCREENH"),
                 ContainsSubstring("64x64"));
}

// --- Loop control: EXIT / CONTINUE ------------------------------------------

TEST_CASE("EXIT FOR leaves the loop early") {
    REQUIRE_THAT(run("10 FOR i=1 TO 9\n20 IF i=4 THEN EXIT FOR\n30 PRINT i;\n40 NEXT\n50 PRINT \"|\""),
                 ContainsSubstring("123|"));
}

TEST_CASE("CONTINUE FOR skips to the next iteration") {
    REQUIRE_THAT(run("10 FOR i=1 TO 6\n20 IF i MOD 2=0 THEN CONTINUE FOR\n30 PRINT i;\n40 NEXT"),
                 ContainsSubstring("135"));
}

TEST_CASE("EXIT REPEAT and EXIT WHILE") {
    REQUIRE_THAT(run("10 i=0\n20 REPEAT\n30 i=i+1\n40 IF i=3 THEN EXIT REPEAT\n50 PRINT i;\n60 UNTIL i>9"),
                 ContainsSubstring("12"));
    REQUIRE_THAT(run("10 i=0\n20 WHILE 1\n30 i=i+1\n40 IF i=3 THEN EXIT WHILE\n50 PRINT i;\n60 ENDWHILE\n70 PRINT \"|\""),
                 ContainsSubstring("12|"));
}

TEST_CASE("bare EXIT breaks only the innermost loop") {
    // The WHILE is broken each pass; the FOR keeps going.
    std::string out = run(
        "10 FOR i=1 TO 3\n"
        "20 WHILE 1\n"
        "30 EXIT\n"
        "40 ENDWHILE\n"
        "50 PRINT i;\n"
        "60 NEXT");
    REQUIRE_THAT(out, ContainsSubstring("123"));
}

TEST_CASE("CONTINUE re-tests a WHILE condition") {
    REQUIRE_THAT(run("10 i=0\n20 WHILE i<5\n30 i=i+1\n40 IF i=3 THEN CONTINUE WHILE\n50 PRINT i;\n60 ENDWHILE"),
                 ContainsSubstring("1245"));
}

TEST_CASE("EXIT outside any loop is an error") {
    REQUIRE_THAT(run("10 EXIT"), ContainsSubstring("not inside a loop"));
}

// --- Structured error handling: TRY / CATCH / RAISE / ERR / ERR$ -------------

TEST_CASE("TRY catches a built-in runtime error") {
    std::string out = run(
        "10 TRY\n"
        "20 x = 1/0\n"
        "30 PRINT \"unreached\"\n"
        "40 CATCH\n"
        "50 PRINT \"caught:\";ERR$\n"
        "60 ENDTRY\n"
        "70 PRINT \"after\"");
    REQUIRE_THAT(out, ContainsSubstring("caught:Division by zero"));
    REQUIRE_THAT(out, ContainsSubstring("after"));
    REQUIRE_THAT(out, !ContainsSubstring("unreached"));
}

TEST_CASE("a successful TRY block skips the handler") {
    std::string out = run("10 TRY\n20 PRINT \"ok\"\n30 CATCH\n40 PRINT \"BAD\"\n50 ENDTRY\n60 PRINT \"done\"");
    REQUIRE_THAT(out, ContainsSubstring("ok"));
    REQUIRE_THAT(out, ContainsSubstring("done"));
    REQUIRE_THAT(out, !ContainsSubstring("BAD"));
}

TEST_CASE("RAISE sets ERR and ERR$") {
    REQUIRE_THAT(run("10 TRY\n20 RAISE 404,\"missing\"\n30 CATCH\n40 PRINT ERR;\":\";ERR$\n50 ENDTRY"),
                 ContainsSubstring("404:missing"));
    REQUIRE_THAT(run("10 TRY\n20 RAISE \"oops\"\n30 CATCH\n40 PRINT ERR$\n50 ENDTRY"),
                 ContainsSubstring("oops"));
}

TEST_CASE("an error inside a PROC is caught by an enclosing TRY") {
    std::string out = run(
        "10 TRY\n"
        "20 PROCboom\n"
        "30 PRINT \"unreached\"\n"
        "40 CATCH\n"
        "50 PRINT \"caught:\";ERR$\n"
        "60 ENDTRY\n"
        "70 PRINT \"ok\"\n"
        "80 END\n"
        "90 DEF PROCboom\n"
        "100 y = 1/0\n"
        "110 ENDPROC");
    REQUIRE_THAT(out, ContainsSubstring("caught:Division by zero"));
    REQUIRE_THAT(out, ContainsSubstring("ok"));
    REQUIRE_THAT(out, !ContainsSubstring("unreached"));
}

TEST_CASE("nested TRY: the inner handler catches, the outer is untouched") {
    std::string out = run(
        "10 TRY\n"
        "20 TRY\n"
        "30 RAISE \"inner\"\n"
        "40 CATCH\n"
        "50 PRINT \"in:\";ERR$\n"
        "60 ENDTRY\n"
        "70 PRINT \"resumed\"\n"
        "80 CATCH\n"
        "90 PRINT \"OUTER-BAD\"\n"
        "100 ENDTRY");
    REQUIRE_THAT(out, ContainsSubstring("in:inner"));
    REQUIRE_THAT(out, ContainsSubstring("resumed"));
    REQUIRE_THAT(out, !ContainsSubstring("OUTER-BAD"));
}

TEST_CASE("TRY recovers loop state: a loop still works after catching") {
    std::string out = run(
        "10 TRY\n"
        "20 FOR i=1 TO 3\n"
        "30 x = 1/0\n"
        "40 NEXT\n"
        "50 CATCH\n"
        "60 PRINT \"caught\"\n"
        "70 ENDTRY\n"
        "80 FOR j=1 TO 3\n"
        "90 PRINT j;\n"
        "100 NEXT");
    REQUIRE_THAT(out, ContainsSubstring("caught"));
    REQUIRE_THAT(out, ContainsSubstring("123"));   // FOR stack was restored cleanly
}

// --- Modern string library --------------------------------------------------

TEST_CASE("UPPER$ / LOWER$ / TRIM$") {
    REQUIRE_THAT(run("10 PRINT UPPER$(\"Hello, World\")"), ContainsSubstring("HELLO, WORLD"));
    REQUIRE_THAT(run("10 PRINT LOWER$(\"Hello, World\")"), ContainsSubstring("hello, world"));
    REQUIRE_THAT(run("10 PRINT \"[\"+TRIM$(\"   hi there   \")+\"]\""), ContainsSubstring("[hi there]"));
    REQUIRE_THAT(run("10 PRINT \"[\"+TRIM$(\"\")+\"]\""), ContainsSubstring("[]"));
}

TEST_CASE("REPLACE$ replaces every occurrence") {
    REQUIRE_THAT(run("10 PRINT REPLACE$(\"a,b,c\",\",\",\"-\")"), ContainsSubstring("a-b-c"));
    REQUIRE_THAT(run("10 PRINT REPLACE$(\"aaa\",\"a\",\"xy\")"),  ContainsSubstring("xyxyxy"));
    REQUIRE_THAT(run("10 PRINT REPLACE$(\"hello\",\"l\",\"\")"),  ContainsSubstring("heo"));
    REQUIRE_THAT(run("10 PRINT REPLACE$(\"hello\",\"z\",\"Q\")"), ContainsSubstring("hello"));
}

TEST_CASE("CONTAINS / STARTSWITH / ENDSWITH return TRUE or FALSE") {
    REQUIRE_THAT(run("10 PRINT CONTAINS(\"hello\",\"ell\")"),     ContainsSubstring("-1"));
    REQUIRE_THAT(run("10 PRINT CONTAINS(\"hello\",\"xyz\")"),     ContainsSubstring("0"));
    REQUIRE_THAT(run("10 PRINT STARTSWITH(\"hello\",\"he\")"),    ContainsSubstring("-1"));
    REQUIRE_THAT(run("10 PRINT STARTSWITH(\"hello\",\"lo\")"),    ContainsSubstring("0"));
    REQUIRE_THAT(run("10 PRINT ENDSWITH(\"hello\",\"lo\")"),      ContainsSubstring("-1"));
    REQUIRE_THAT(run("10 PRINT ENDSWITH(\"hello\",\"he\")"),      ContainsSubstring("0"));
}

TEST_CASE("SPLIT fills a string array and returns the count") {
    std::string out = run(
        "10 n = SPLIT(\"apple,banana,cherry\", \",\", f$())\n"
        "20 PRINT \"n=\"; n\n"
        "30 FOR i = 0 TO n-1 : PRINT f$(i); \"|\"; : NEXT");
    REQUIRE_THAT(out, ContainsSubstring("n=3"));
    REQUIRE_THAT(out, ContainsSubstring("apple|banana|cherry|"));
}

TEST_CASE("SPLIT keeps empty fields, including a trailing one") {
    std::string out = run(
        "10 n = SPLIT(\"a,,c,\", \",\", p$())\n"
        "20 PRINT \"n=\"; n\n"
        "30 FOR i = 0 TO n-1 : PRINT \"[\"; p$(i); \"]\"; : NEXT");
    REQUIRE_THAT(out, ContainsSubstring("n=4"));
    REQUIRE_THAT(out, ContainsSubstring("[a][][c][]"));
}

TEST_CASE("SPLIT with an empty separator yields one piece per character") {
    REQUIRE_THAT(run("10 n = SPLIT(\"abc\", \"\", c$())\n20 PRINT n; c$(0); c$(1); c$(2)"),
                 ContainsSubstring("3abc"));
}

TEST_CASE("JOIN$ is the inverse of SPLIT") {
    REQUIRE_THAT(run("10 n = SPLIT(\"one two three\", \" \", w$())\n20 PRINT JOIN$(w$(), \"-\", n)"),
                 ContainsSubstring("one-two-three"));
    REQUIRE_THAT(run("10 n = SPLIT(\"a:b:c\", \":\", w$())\n20 PRINT JOIN$(w$(), \", \", n)"),
                 ContainsSubstring("a, b, c"));
}

TEST_CASE("string helpers compose in one expression") {
    // A tiny CSV-ish pipeline: trim, upper-case, test membership.
    std::string out = run(
        "10 n = SPLIT(\"  red , green , blue \", \",\", c$())\n"
        "20 FOR i = 0 TO n-1 : c$(i) = UPPER$(TRIM$(c$(i))) : NEXT\n"
        "30 PRINT JOIN$(c$(), \"/\", n)\n"
        "40 PRINT CONTAINS(JOIN$(c$(), \",\", n), \"GREEN\")");
    REQUIRE_THAT(out, ContainsSubstring("RED/GREEN/BLUE"));
    REQUIRE_THAT(out, ContainsSubstring("-1"));
}

// --- PEEK / POKE (familiar aliases over the ?/! indirection) -----------------

TEST_CASE("POKE writes a byte and PEEK reads it back") {
    REQUIRE_THAT(run("10 DIM b 8\n20 POKE b, 65\n30 POKE b+1, 66\n40 PRINT PEEK(b); PEEK(b+1)"),
                 ContainsSubstring("6566"));
}

TEST_CASE("POKE truncates to a byte") {
    REQUIRE_THAT(run("10 DIM b 8\n20 POKE b, 300\n30 PRINT PEEK(b)"),
                 ContainsSubstring("44"));   // 300 AND 255
}

TEST_CASE("PEEK/POKE and ? indirection share the same memory") {
    REQUIRE_THAT(run("10 DIM b 8\n20 POKE b, 7\n30 PRINT b?0"),   ContainsSubstring("7"));
    REQUIRE_THAT(run("10 DIM b 8\n20 b?0 = 9\n30 PRINT PEEK(b)"), ContainsSubstring("9"));
}

TEST_CASE("PEEK composes inside expressions") {
    REQUIRE_THAT(run("10 DIM b 8\n20 POKE b, 40\n30 PRINT PEEK(b) + 2"), ContainsSubstring("42"));
}

// --- GPIO (host build reports the hardware as unavailable) -------------------
// On the host there is no 40-pin header, so every GPIO verb must raise the guard
// rather than a syntax error. These confirm each verb parses and reaches the
// guard; the register-level behaviour is exercised on real hardware / QEMU.

TEST_CASE("GPIO verbs raise the host guard, not a syntax error") {
    const char *guard = "GPIO needs the Pi";
    REQUIRE_THAT(run("10 PINMODE 17, OUTPUT"),          ContainsSubstring(guard));
    REQUIRE_THAT(run("10 PINMODE 27, INPUT PULLUP"),    ContainsSubstring(guard));
    REQUIRE_THAT(run("10 PINMODE 2, ALT 0"),            ContainsSubstring(guard));
    REQUIRE_THAT(run("10 PIN 17, 1"),                   ContainsSubstring(guard));
    REQUIRE_THAT(run("10 PRINT PIN(27)"),               ContainsSubstring(guard));
    REQUIRE_THAT(run("10 PINSET 1"),                    ContainsSubstring(guard));
    REQUIRE_THAT(run("10 PINCLR 1"),                    ContainsSubstring(guard));
    REQUIRE_THAT(run("10 PRINT PINS"),                  ContainsSubstring(guard));
    REQUIRE_THAT(run("10 PRINT PINWAIT(27, 1, 0)"),     ContainsSubstring(guard));
}

// --- EVAL / EXEC (dynamic evaluation and execution) -------------------------
// The interpreter re-lexes source on the fly, so EVAL parses a string as an
// expression and EXEC runs one as a statement, both in the current context.

TEST_CASE("EVAL computes a numeric expression with variables") {
    REQUIRE_THAT(run("10 X=5\n20 PRINT EVAL(\"2*X+1\")"), ContainsSubstring("11"));
}

TEST_CASE("EVAL honours precedence and functions") {
    REQUIRE_THAT(run("10 PRINT EVAL(\"SQR(9)+3*2\")"), ContainsSubstring("9"));
}

TEST_CASE("EVAL returns a string when the expression is a string") {
    REQUIRE_THAT(run("10 A$=\"foo\"\n20 B$=\"bar\"\n30 PRINT EVAL(\"A$+B$\")"),
                 ContainsSubstring("foobar"));
}

TEST_CASE("EVAL composes inside a larger expression") {
    REQUIRE_THAT(run("10 X=10\n20 PRINT EVAL(\"X*2\")+100"), ContainsSubstring("120"));
}

TEST_CASE("EVAL nests (an EVAL inside an EVAL)") {
    REQUIRE_THAT(run("10 X=3\n20 I$=\"X\"\n30 PRINT EVAL(\"EVAL(I$)*10\")"),
                 ContainsSubstring("30"));
}

TEST_CASE("a bad EVAL string raises, and TRY can catch it") {
    REQUIRE_THAT(run("10 TRY\n20 PRINT EVAL(\"1/\")\n30 CATCH\n40 PRINT \"caught\"\n50 ENDTRY"),
                 ContainsSubstring("caught"));
}

TEST_CASE("EXEC runs a statement in the current context") {
    REQUIRE_THAT(run("10 EXEC \"Y = 7*6\"\n20 PRINT Y"), ContainsSubstring("42"));
}

TEST_CASE("EXEC can be assembled dynamically for dispatch") {
    REQUIRE_THAT(run("10 N$=\"Z\" : V$=\"99\"\n20 EXEC N$+\" = \"+V$\n30 PRINT Z"),
                 ContainsSubstring("99"));
}

TEST_CASE("EXEC runs multiple colon-separated statements") {
    REQUIRE_THAT(run("10 EXEC \"A=1 : B=2 : PRINT A+B\""), ContainsSubstring("3"));
}

TEST_CASE("a statement after EXEC still runs") {
    REQUIRE_THAT(run("10 EXEC \"PRINT 1\" : PRINT \"after\""), ContainsSubstring("after"));
}

TEST_CASE("EXEC GOTO branches the program") {
    REQUIRE_THAT(run("10 EXEC \"GOTO 40\"\n20 PRINT \"skip\"\n30 END\n40 PRINT \"jumped\""),
                 ContainsSubstring("jumped"));
}

// --- Keyboard layouts (HID decode + the KEYBOARD statement) -----------------
// Validate the layout tables directly through the shared decoder, then the BASIC
// surface. mod bits: 0x02 = Shift, 0x40 = AltGr (right Alt).
static char K(uint8_t kc, uint8_t mod = 0) { return hid_to_ascii(kc, mod); }

TEST_CASE("US layout: number-row shifts and symbols") {
    REQUIRE(hid_set_layout("US") == 1);
    REQUIRE(K(0x1F) == '2');
    REQUIRE(K(0x1F, 0x02) == '@');       // shift-2 = @ on US
    REQUIRE(K(0x34, 0x02) == '"');       // shift-' = "
    REQUIRE(K(0x64, 0x40) == 0);         // US has no AltGr layer
}

TEST_CASE("Norwegian layout: the three extra letters") {
    REQUIRE(hid_set_layout("no") == 1);  // case-insensitive
    REQUIRE((unsigned char)K(0x2F) == 0xE5);          // å
    REQUIRE((unsigned char)K(0x2F, 0x02) == 0xC5);    // Å
    REQUIRE((unsigned char)K(0x33) == 0xF8);          // ø
    REQUIRE((unsigned char)K(0x33, 0x02) == 0xD8);    // Ø
    REQUIRE((unsigned char)K(0x34) == 0xE6);          // æ
    REQUIRE((unsigned char)K(0x34, 0x02) == 0xC6);    // Æ
}

TEST_CASE("Norwegian layout: AltGr programming symbols") {
    REQUIRE(hid_set_layout("NO") == 1);
    REQUIRE(K(0x1F, 0x40) == '@');       // AltGr-2 = @
    REQUIRE(K(0x21, 0x40) == '$');       // AltGr-4 = $
    REQUIRE(K(0x24, 0x40) == '{');       // AltGr-7 = {
    REQUIRE(K(0x25, 0x40) == '[');       // AltGr-8 = [
    REQUIRE(K(0x26, 0x40) == ']');       // AltGr-9 = ]
    REQUIRE(K(0x27, 0x40) == '}');       // AltGr-0 = }
    REQUIRE(K(0x2D, 0x40) == '\\');      // AltGr-+ = backslash
    REQUIRE(K(0x30, 0x40) == '~');       // AltGr-¨ = tilde
    REQUIRE(K(0x64, 0x40) == '|');       // AltGr-< = pipe
}

TEST_CASE("Swedish and German pick the right national letters") {
    REQUIRE(hid_set_layout("SE") == 1);
    REQUIRE((unsigned char)K(0x33) == 0xF6);   // ö
    REQUIRE((unsigned char)K(0x34) == 0xE4);   // ä
    REQUIRE(hid_set_layout("DE") == 1);
    REQUIRE(K(0x1C) == 'z');                    // QWERTZ: Y position types z
    REQUIRE(K(0x1D) == 'y');
    REQUIRE(K(0x14, 0x40) == '@');              // AltGr-Q = @
    REQUIRE((unsigned char)K(0x2D) == 0xDF);    // ß
}

TEST_CASE("an unknown layout code is rejected and keeps the current one") {
    REQUIRE(hid_set_layout("US") == 1);
    REQUIRE(hid_set_layout("ZZ") == 0);
    REQUIRE(std::string(hid_layout_code()) == "US");
}

TEST_CASE("KEYBOARD statement sets the layout; KEYBOARD$ reads it back") {
    REQUIRE_THAT(run("10 KEYBOARD \"NO\"\n20 PRINT KEYBOARD$"), ContainsSubstring("NO"));
}

TEST_CASE("KEYBOARD with an unknown layout raises an error") {
    REQUIRE_THAT(run("10 KEYBOARD \"ZZ\""), ContainsSubstring("Unknown keyboard layout"));
}

// --- Events (ON TIMER / ON MOUSE) and WAIT -----------------------------------
// ON PIN needs GPIO (host stub can't fire it); ON MOUSE needs a moving mouse
// (host con_mouse is static). Timer events use the host clock, so they fire.

// The test console's clock advances a fixed step per read, so a timer fires on
// every poll; these assert "the handler ran" via a flag rather than an exact
// count (which is clock-rate dependent and validated on real hardware/QEMU).

TEST_CASE("ON TIMER runs a handler PROC") {
    REQUIRE_THAT(run("10 F=0\n20 ON TIMER 1 PROC t\n30 REPEAT\n35 UNTIL F=1\n"
                     "40 PRINT \"fired\"\n50 END\n60 DEF PROC t\n70 F=1\n80 ENDPROC"),
                 ContainsSubstring("fired"));
}

TEST_CASE("a handler sees and updates global state") {
    REQUIRE_THAT(run("10 N=0\n20 ON TIMER 1 PROC b\n30 REPEAT\n35 UNTIL N<>0\n"
                     "40 PRINT \"final=\";N\n50 END\n60 DEF PROC b\n70 N=42\n80 ENDPROC"),
                 ContainsSubstring("final=42"));
}

TEST_CASE("ON TIMER OFF stops the handler firing") {
    // Fire once, cancel, then confirm a guard counter stays at zero.
    REQUIRE_THAT(run("10 F=0\n20 G=0\n30 ON TIMER 1 PROC t\n40 REPEAT\n45 UNTIL F=1\n"
                     "50 ON TIMER OFF\n60 G=0\n70 FOR I=1 TO 100000:NEXT\n"
                     "80 IF G=0 THEN PRINT \"stopped\" ELSE PRINT \"leak\"\n90 END\n"
                     "100 DEF PROC t\n110 F=1\n120 G=G+1\n130 ENDPROC"),
                 ContainsSubstring("stopped"));
}

TEST_CASE("ON MOUSE registers and cancels without error") {
    REQUIRE_THAT(run("10 ON MOUSE PROC m\n20 ON MOUSE OFF\n30 PRINT \"ok\"\n"
                     "40 END\n50 DEF PROC m\n60 ENDPROC"),
                 ContainsSubstring("ok"));
}

TEST_CASE("ON KEY registers and cancels without error") {
    // Firing needs real key input (the test console has none); this checks parse.
    REQUIRE_THAT(run("10 ON KEY PROC k\n20 ON KEY OFF\n30 PRINT \"ok\"\n"
                     "40 END\n50 DEF PROC k\n60 ENDPROC"),
                 ContainsSubstring("ok"));
}

TEST_CASE("WAIT executes and lets time advance") {
    // Exact 60 Hz pacing needs a real clock; here just confirm WAIT runs and TIME moves on.
    std::string out = run("10 T=TIME\n20 FOR F=1 TO 10:WAIT:NEXT\n"
                          "30 IF TIME-T>0 THEN PRINT \"waited\" ELSE PRINT \"stuck\"\n40 END");
    REQUIRE_THAT(out, ContainsSubstring("waited"));
}

TEST_CASE("DELAY pauses for at least the requested centiseconds") {
    std::string out = run("10 T=TIME\n20 DELAY 10\n"
                          "30 IF TIME-T>=10 THEN PRINT \"delayed\" ELSE PRINT \"short\"\n40 END");
    REQUIRE_THAT(out, ContainsSubstring("delayed"));
}

TEST_CASE("DELAY with a non-positive value returns immediately") {
    REQUIRE_THAT(run("10 DELAY 0\n20 DELAY -5\n30 PRINT \"ok\"\n40 END"),
                 ContainsSubstring("ok"));
}

// --- Double buffering (BUFFER ON/OFF, FLIP) ----------------------------------
// The host console has no framebuffer, so the visible effect is validated in
// QEMU; here we check the surface parses, runs, and drives the standard loop.

TEST_CASE("BUFFER ON / FLIP / BUFFER OFF parse and run") {
    REQUIRE_THAT(run("10 BUFFER ON\n20 CLG\n30 CIRCLE FILL 640,512,100\n"
                     "40 FLIP\n50 BUFFER OFF\n60 PRINT \"ok\"\n70 END"),
                 ContainsSubstring("ok"));
}

TEST_CASE("the standard WAIT/CLG/FLIP frame loop runs to completion") {
    REQUIRE_THAT(run("10 BUFFER ON\n20 FOR F=1 TO 3\n30 WAIT:CLG:CIRCLE FILL F*100,512,60:FLIP\n"
                     "40 NEXT\n50 BUFFER OFF\n60 PRINT \"done\"\n70 END"),
                 ContainsSubstring("done"));
}

TEST_CASE("FLIP with buffering off is a harmless no-op") {
    REQUIRE_THAT(run("10 FLIP\n20 PRINT \"ok\"\n30 END"), ContainsSubstring("ok"));
}

TEST_CASE("BUFFER without ON or OFF raises an error") {
    REQUIRE_THAT(run("10 BUFFER 3"), ContainsSubstring("Expected ON or OFF"));
}

// --- Transformed blits (GPUT scale/angle, GTINT) -----------------------------
// The host sprite calls are no-ops (no framebuffer); these check the surface
// parses and runs. The pixel results are validated in QEMU.

TEST_CASE("GPUT accepts the plain and the scaled/rotated forms") {
    REQUIRE_THAT(run("10 DIM S 40000\n20 GPUT S,100,100\n30 GPUT S,100,100,2.0,45\n"
                     "40 PRINT \"ok\"\n50 END"),
                 ContainsSubstring("ok"));
}

TEST_CASE("GTINT sets and clears the tint without error") {
    REQUIRE_THAT(run("10 DIM S 40000\n20 GTINT 255,0,0,128\n30 GPUT S,10,10,1,0\n"
                     "40 GTINT OFF\n50 PRINT \"ok\"\n60 END"),
                 ContainsSubstring("ok"));
}

TEST_CASE("GTINT with too few arguments raises an error") {
    REQUIRE_THAT(run("10 GTINT 255,0,0"), ContainsSubstring("Expected ','"));
}

// --- Render-to-sprite (NEWSPRITE / SPRITETARGET) -----------------------------
// Host sprite ops are no-ops; the pixel results are validated in QEMU. Here we
// check the surface parses/runs and the size guard fires.

TEST_CASE("NEWSPRITE / SPRITETARGET redirect and restore drawing") {
    REQUIRE_THAT(run("10 DIM B 40000\n20 NEWSPRITE B,96,96\n30 SPRITETARGET B\n"
                     "40 CLG:CIRCLE FILL 48,48,46\n50 SPRITETARGET OFF\n"
                     "60 GPUT B,100,100\n70 PRINT \"ok\"\n80 END"),
                 ContainsSubstring("ok"));
}

TEST_CASE("NEWSPRITE with a non-positive size raises an error") {
    REQUIRE_THAT(run("10 DIM B 40000\n20 NEWSPRITE B,0,96"),
                 ContainsSubstring("Bad sprite size"));
}

// --- Tilemap -----------------------------------------------------------------
// Host con_tilemap is a no-op; the scrolling render is validated in QEMU. Here
// we check the eight-argument surface parses and runs with a built map.

TEST_CASE("TILEMAP parses and runs with a DIM map") {
    REQUIRE_THAT(run("10 DIM SH 20000\n20 NEWSPRITE SH,96,32\n30 DIM M 64\n"
                     "40 M!0=1:M!4=2:M!8=3:M!12=0\n"
                     "50 TILEMAP SH,M,4,1,32,32,0,0\n60 PRINT \"ok\"\n70 END"),
                 ContainsSubstring("ok"));
}

TEST_CASE("TILEMAP with too few arguments raises an error") {
    REQUIRE_THAT(run("10 TILEMAP 1,2,3,4"), ContainsSubstring("Expected ','"));
}

// --- Collections: dictionary, list, binary tree ------------------------------
// These run on the host (the interpreter and its heap are platform-independent).

TEST_CASE("dictionary stores, updates, reads and removes string/number values") {
    REQUIRE_THAT(run("10 D=NEWDICT\n20 DICTSET D,\"name\",\"Ada\"\n30 DICTSET D,\"born\",1815\n"
                     "40 DICTSET D,\"born\",1816\n"     // update
                     "50 PRINT DICTGET$(D,\"name\");\"/\";DICTGET(D,\"born\");\"/\";SIZE(D)\n60 END"),
                 ContainsSubstring("Ada/1816/2"));
}

TEST_CASE("DICTHAS and DICTDEL and DICTKEY$ report membership and order") {
    REQUIRE_THAT(run("10 D=NEWDICT\n20 DICTSET D,\"a\",1\n30 DICTSET D,\"b\",2\n"
                     "40 DICTDEL D,\"a\"\n"
                     "50 PRINT DICTHAS(D,\"a\");\"/\";DICTHAS(D,\"b\");\"/\";DICTKEY$(D,0)\n60 END"),
                 ContainsSubstring("0/-1/b"));
}

TEST_CASE("a missing dictionary key reads as 0 or empty") {
    REQUIRE_THAT(run("10 D=NEWDICT\n20 PRINT \"[\";DICTGET$(D,\"x\");\"]\";DICTGET(D,\"x\")\n30 END"),
                 ContainsSubstring("[]0"));
}

TEST_CASE("list push/pop behaves as a stack and reports its length") {
    REQUIRE_THAT(run("10 L=NEWLIST\n20 PUSH L,10:PUSH L,20:PUSH L,30\n"
                     "30 PRINT POP(L);\"/\";POP(L);\"/\";SIZE(L)\n40 END"),
                 ContainsSubstring("30/20/1"));
}

TEST_CASE("list index get/set, insert and delete") {
    REQUIRE_THAT(run("10 L=NEWLIST\n20 PUSH L,1:PUSH L,2:PUSH L,3\n30 LISTINS L,1,99\n"
                     "40 LISTSET L,0,7\n50 LISTDEL L,3\n"
                     "60 PRINT LISTGET(L,0);LISTGET(L,1);LISTGET(L,2);\"/\";SIZE(L)\n70 END"),
                 ContainsSubstring("7992/3"));
}

TEST_CASE("a list can hold strings too") {
    REQUIRE_THAT(run("10 L=NEWLIST\n20 PUSH L,\"hi\"\n30 PRINT POP$(L)\n40 END"),
                 ContainsSubstring("hi"));
}

TEST_CASE("binary tree keeps keys sorted for in-order iteration") {
    REQUIRE_THAT(run("10 T=NEWTREE\n20 TREESET T,50,0:TREESET T,20,0:TREESET T,80,0\n"
                     "30 TREESET T,10,0:TREESET T,30,0\n40 S$=\"\"\n"
                     "50 FOR I=0 TO SIZE(T)-1:S$=S$+STR$(TREEKEY(T,I))+\" \":NEXT\n"
                     "60 PRINT S$\n70 END"),
                 ContainsSubstring("10 20 30 50 80"));
}

TEST_CASE("tree lookup, membership, min/max and delete keep the order") {
    REQUIRE_THAT(run("10 T=NEWTREE\n20 TREESET T,50,\"x\":TREESET T,20,\"y\":TREESET T,80,\"z\"\n"
                     "30 TREEDEL T,20\n"
                     "40 PRINT TREEGET$(T,50);\"/\";TREEHAS(T,20);\"/\";TREEMIN(T);\"/\";TREEMAX(T)\n50 END"),
                 ContainsSubstring("x/0/50/80"));
}

TEST_CASE("collection type mismatches and empties raise clear errors") {
    REQUIRE_THAT(run("10 D=NEWDICT\n20 PUSH D,1"), ContainsSubstring("Not a list"));
    REQUIRE_THAT(run("10 L=NEWLIST\n20 PRINT POP(L)"), ContainsSubstring("List is empty"));
    REQUIRE_THAT(run("10 PRINT SIZE(999)"), ContainsSubstring("Not a collection"));
    REQUIRE_THAT(run("10 D=NEWDICT\n20 DICTSET D,\"x\",\"t\"\n30 PRINT DICTGET(D,\"x\")"),
                 ContainsSubstring("Type mismatch"));
}

TEST_CASE("a collection error can be caught with TRY/CATCH") {
    REQUIRE_THAT(run("10 L=NEWLIST\n20 TRY\n30 PRINT POP(L)\n40 CATCH\n50 PRINT \"caught\"\n60 ENDTRY\n70 END"),
                 ContainsSubstring("caught"));
}

TEST_CASE("collection string values survive garbage collection") {
    REQUIRE_THAT(run("10 D=NEWDICT\n20 DICTSET D,\"k\",\"keepme\"\n"
                     "30 FOR I=1 TO 500:A$=STR$(I)+\"padding\":NEXT\n"
                     "40 PRINT DICTGET$(D,\"k\")\n50 END"),
                 ContainsSubstring("keepme"));
}

// --- I2C ---------------------------------------------------------------------
// The bus is real-hardware only (QEMU doesn't model the BSC), so on the host it
// reports unavailable; here we check the words parse and raise the guard error.
// Actual bus traffic is validated on a real Pi.

TEST_CASE("I2C words raise a clear guard on the host (no bus present)") {
    REQUIRE_THAT(run("10 I2CWRITE 39, 65, 66"), ContainsSubstring("I2C needs real Pi hardware"));
    REQUIRE_THAT(run("10 DIM B 8\n20 I2CREAD 39, B, 2"), ContainsSubstring("I2C needs real Pi hardware"));
    REQUIRE_THAT(run("10 PRINT I2CPROBE(39)"), ContainsSubstring("I2C needs real Pi hardware"));
}

TEST_CASE("an I2C error can be caught with TRY/CATCH") {
    REQUIRE_THAT(run("10 TRY\n20 I2CWRITE 39, 1\n30 CATCH\n40 PRINT \"caught\"\n50 ENDTRY\n60 END"),
                 ContainsSubstring("caught"));
}

TEST_CASE("HEX$ and BIN$ render numbers in base 16 and 2") {
    REQUIRE_THAT(run("10 PRINT HEX$(255)"),        ContainsSubstring("FF"));
    REQUIRE_THAT(run("10 PRINT HEX$(255,4)"),      ContainsSubstring("00FF"));
    REQUIRE_THAT(run("10 PRINT HEX$(-1)"),         ContainsSubstring("FFFFFFFF"));   // 32-bit two's complement
    REQUIRE_THAT(run("10 PRINT BIN$(10)"),         ContainsSubstring("1010"));
    REQUIRE_THAT(run("10 PRINT BIN$(10,8)"),       ContainsSubstring("00001010"));
    REQUIRE_THAT(run("10 PRINT \"0x\"+HEX$(240)"), ContainsSubstring("0xF0"));       // composes with +
}

TEST_CASE("binary literals with % evaluate and compose") {
    REQUIRE_THAT(run("10 PRINT %1010"),            ContainsSubstring("10"));
    REQUIRE_THAT(run("10 PRINT %1100 OR %0011"),   ContainsSubstring("15"));
    REQUIRE_THAT(run("10 A%=%11110000\n20 PRINT A%;\" \";HEX$(A%)"), ContainsSubstring("240 F0"));
}

TEST_CASE("a lone %% is still an integer variable suffix, not a binary literal") {
    REQUIRE_THAT(run("10 COUNT%=5\n20 PRINT COUNT%*2"), ContainsSubstring("10"));
}

TEST_CASE("FORMAT$ renders decimals, width, zero-pad, thousands and sign") {
    REQUIRE_THAT(run("10 PRINT \"[\";FORMAT$(\"####.##\",3.14159);\"]\""), ContainsSubstring("[   3.14]"));
    REQUIRE_THAT(run("10 PRINT \"[\";FORMAT$(\"000.00\",3.1);\"]\""),      ContainsSubstring("[003.10]"));
    REQUIRE_THAT(run("10 PRINT \"[\";FORMAT$(\"#,###,###\",1234567);\"]\""),ContainsSubstring("[1,234,567]"));
    REQUIRE_THAT(run("10 PRINT \"[\";FORMAT$(\"+#0.0\",5);\"]\""),         ContainsSubstring("[+5.0]"));
    REQUIRE_THAT(run("10 PRINT \"[\";FORMAT$(\"#.##\",-2.005);\"]\""),     ContainsSubstring("[-2.01]"));  // rounds
}

TEST_CASE("FORMAT$ copies literal template text through and never truncates the integer part") {
    REQUIRE_THAT(run("10 PRINT FORMAT$(\"$#,##0.00\",1999.5)"), ContainsSubstring("$1,999.50"));
    REQUIRE_THAT(run("10 PRINT \"[\";FORMAT$(\"#,##0\",1234567);\"]\""), ContainsSubstring("[1,234,567]"));
}

TEST_CASE("FORMAT$ with no digit position raises Bad format template") {
    REQUIRE_THAT(run("10 PRINT FORMAT$(\"abc\",5)"), ContainsSubstring("Bad format template"));
}

TEST_CASE("PRINT USING applies a template to each numeric item") {
    REQUIRE_THAT(run("10 PRINT USING \"#,##0\"; 1234567"),        ContainsSubstring("1,234,567"));
    REQUIRE_THAT(run("10 PRINT USING \"######.00\"; 12.5"),       ContainsSubstring("    12.50"));
    REQUIRE_THAT(run("10 PRINT USING \"000.0\"; 7; \" done\""),   ContainsSubstring("007.0 done")); // strings pass through
}

// --- TrueType fonts ---------------------------------------------------------
// A real TTF bundled in the repo; its absolute path comes from CMake so these
// tests don't depend on the working directory. Metrics run on the host backend
// (real stb_truetype); GTEXT is a no-op there but must still parse and run.
#ifdef FONT_TTF_PATH
static std::string load_font() {
    return std::string("10 F = LOADFONT(\"") + FONT_TTF_PATH + "\")\n";
}

TEST_CASE("LOADFONT returns a handle for a real font and 0 for a missing one") {
    REQUIRE_THAT(run(load_font() + "20 PRINT \"H=\";F"),           ContainsSubstring("H=1"));
    REQUIRE_THAT(run("10 PRINT \"M=\";LOADFONT(\"nope.ttf\")"),    ContainsSubstring("M=0"));
}

TEST_CASE("FONTHEIGHT tracks FONTSIZE") {
    std::string out = run(load_font() +
        "20 FONTSIZE 48 : PRINT \"A=\";FONTHEIGHT\n"
        "30 FONTSIZE 24 : PRINT \"B=\";FONTHEIGHT\n");
    REQUIRE_THAT(out, ContainsSubstring("A=48"));
    REQUIRE_THAT(out, ContainsSubstring("B=24"));
}

TEST_CASE("TEXTWIDTH grows with text length and is zero for the empty string") {
    std::string out = run(load_font() +
        "20 FONTSIZE 32\n"
        "30 IF TEXTWIDTH(\"MMMM\") > TEXTWIDTH(\"M\") THEN PRINT \"WIDER\"\n"
        "40 IF TEXTWIDTH(\"M\") > 0 THEN PRINT \"NONZERO\"\n"
        "50 PRINT \"E=\";TEXTWIDTH(\"\")\n");
    REQUIRE_THAT(out, ContainsSubstring("WIDER"));
    REQUIRE_THAT(out, ContainsSubstring("NONZERO"));
    REQUIRE_THAT(out, ContainsSubstring("E=0"));
}

TEST_CASE("FONTSTYLE bold widens the text") {
    std::string out = run(load_font() +
        "20 FONTSIZE 40\n"
        "30 W = TEXTWIDTH(\"lll\")\n"
        "40 FONTSTYLE 1\n"
        "50 IF TEXTWIDTH(\"lll\") > W THEN PRINT \"BOLDER\"\n");
    REQUIRE_THAT(out, ContainsSubstring("BOLDER"));
}

TEST_CASE("FONT selects a loaded handle and rejects an unknown one") {
    REQUIRE_THAT(run(load_font() + "20 FONT 1 : PRINT \"OK\""),    ContainsSubstring("OK"));
    REQUIRE_THAT(run(load_font() + "20 FONT 99"),                  ContainsSubstring("No such font"));
}

TEST_CASE("GTEXT and FONTSTYLE parse and run (no-op without a framebuffer)") {
    std::string out = run(load_font() +
        "20 FONTSIZE 30 : FONTSTYLE 1,1,1\n"
        "30 GCOL 255,200,0\n"
        "40 GTEXT 100, 200, \"Hello\"\n"
        "50 PRINT \"RAN\"\n");
    REQUIRE_THAT(out, ContainsSubstring("RAN"));
}
#endif

// --- pretty LIST (colouring is a no-op in tests, so gutter + indent are seen) --
TEST_CASE("LIST right-aligns line numbers in a gutter sized to the largest") {
    std::string out = run_raw("5 PRINT 1\n100 PRINT 2\nLIST\n");
    REQUIRE_THAT(out, ContainsSubstring("  5 PRINT 1"));    // padded to width 3
    REQUIRE_THAT(out, ContainsSubstring("100 PRINT 2"));
}

TEST_CASE("LIST indents the body of a block") {
    std::string out = run_raw("10 FOR I=1 TO 2\n20 PRINT I\n30 NEXT\nLIST\n");
    REQUIRE_THAT(out, ContainsSubstring("10 FOR I"));       // opener at column 0
    REQUIRE_THAT(out, ContainsSubstring("20   PRINT I"));   // body indented one level
    REQUIRE_THAT(out, ContainsSubstring("30 NEXT"));        // closer back out
}

TEST_CASE("LIST dedents ELSE and ENDIF within a block IF") {
    std::string out = run_raw(
        "10 IF X THEN\n20 PRINT \"a\"\n30 ELSE\n40 PRINT \"b\"\n50 ENDIF\nLIST\n");
    REQUIRE_THAT(out, ContainsSubstring("20   PRINT"));     // body indented
    REQUIRE_THAT(out, ContainsSubstring("30 ELSE"));        // ELSE dedented
    REQUIRE_THAT(out, ContainsSubstring("50 ENDIF"));       // ENDIF dedented
}

TEST_CASE("LIST SIMPLE gives the plain unindented form") {
    std::string out = run_raw("10 FOR I=1 TO 2\n20 PRINT I\n30 NEXT\nLIST SIMPLE\n");
    REQUIRE_THAT(out, ContainsSubstring("20 PRINT I"));     // one space, no indent
    REQUIRE_THAT(out, !ContainsSubstring("20   PRINT I"));  // definitely not indented
}

TEST_CASE("LIST SIMPLE still honours a line range") {
    std::string out = run_raw("10 PRINT 1\n20 PRINT 2\n30 PRINT 3\nLIST SIMPLE 20,30\n");
    REQUIRE_THAT(out, ContainsSubstring("20 PRINT 2"));
    REQUIRE_THAT(out, ContainsSubstring("30 PRINT 3"));
    REQUIRE_THAT(out, !ContainsSubstring("10 PRINT 1"));
}
