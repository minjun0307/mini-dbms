/*
 * lexer.h — [Phase 1] Tokenizer (Lexer)
 *
 * 문자열(SQL)을 의미 있는 단어(Token)로 자른다.
 *   "SELECT * FROM users;"  →  [SELECT] [*] [FROM] [users] [;] [EOF]
 *
 * 핵심 원리: 커서(pos)를 앞으로만 옮기면서, "현재 글자"를 보고 토큰 종류를 결정한다.
 * 공백 제거, 키워드 대소문자 정규화, 문자열 따옴표 처리는 모두 여기서 끝난다.
 * 그래서 Parser 는 글자가 아니라 "토큰의 종류"만 보면 된다.
 */
#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>

#define TOKEN_TEXT_MAX 256   /* 토큰 텍스트 최대 길이 → 문자열 값은 최대 255 바이트 */

typedef enum {
    TOK_KEYWORD,    /* SELECT INSERT INTO VALUES FROM WHERE LIMIT (대문자로 정규화) */
    TOK_IDENT,      /* users, name, age */
    TOK_INT,        /* 42, -7 */
    TOK_STRING,     /* 'kim' → text 에는 따옴표를 뗀 kim */
    TOK_LPAREN,     /* ( */
    TOK_RPAREN,     /* ) */
    TOK_COMMA,      /* , */
    TOK_SEMICOLON,  /* ; */
    TOK_STAR,       /* * */
    TOK_EQ,         /* = */
    TOK_NE,         /* != 또는 <> */
    TOK_LT,         /* < */
    TOK_LE,         /* <= */
    TOK_GT,         /* > */
    TOK_GE,         /* >= */
    TOK_EOF,        /* 입력 끝 */
    TOK_ERROR       /* 잘못된 입력 — text 에 에러 메시지 */
} TokenType;

typedef struct {
    TokenType type;
    char text[TOKEN_TEXT_MAX];
    int line;   /* 에러 메시지용 위치 정보 */
    int col;
} Token;

typedef struct {
    const char *src;    /* 전체 SQL 문자열 */
    size_t pos;         /* 지금 보고 있는 글자 위치 (커서) */
    int line;
    size_t line_start;  /* 현재 줄이 시작된 위치 (col 계산용) */
} Lexer;

void lexer_init(Lexer *lx, const char *src);
Token lexer_next(Lexer *lx);          /* 다음 토큰 하나를 만들어 돌려준다 */
const char *token_type_name(TokenType t);

#endif
