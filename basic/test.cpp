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
