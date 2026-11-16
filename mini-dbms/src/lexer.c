#include "lexer.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "util.h"

static const char *KEYWORDS[] = {
    "SELECT", "INSERT", "INTO", "VALUES", "FROM", "WHERE", "LIMIT", NULL,
};

void lexer_init(Lexer *lx, const char *src) {
    lx->src = src;
    lx->pos = 0;
    lx->line = 1;
    lx->line_start = 0;
}

/* ---------- 커서 조작: 이 세 함수로 모든 걸 만든다 ---------- */

static char peek(const Lexer *lx) { return lx->src[lx->pos]; }

static char peek_next(const Lexer *lx) {
    return lx->src[lx->pos] == '\0' ? '\0' : lx->src[lx->pos + 1];
}

static char advance(Lexer *lx) {
    char c = lx->src[lx->pos];
    if (c == '\0') return c;
    lx->pos++;
    if (c == '\n') {
        lx->line++;
        lx->line_start = lx->pos;
    }
    return c;
}

static Token make_token(const Lexer *lx) {
    Token t;
    t.type = TOK_EOF;
    t.text[0] = '\0';
    t.line = lx->line;
    t.col = (int)(lx->pos - lx->line_start) + 1;
    return t;
}

static Token error_token(Token t, const char *msg) {
    t.type = TOK_ERROR;
    snprintf(t.text, sizeof t.text, "%s", msg);
    return t;
}

/* 공백과 SQL 주석(-- 부터 줄 끝까지)을 건너뛴다 */
static void skip_whitespace_and_comments(Lexer *lx) {
    for (;;) {
        char c = peek(lx);
        if (isspace((unsigned char)c)) {
            advance(lx);
        } else if (c == '-' && peek_next(lx) == '-') {
            while (peek(lx) != '\n' && peek(lx) != '\0') advance(lx);
        } else {
            break;
        }
    }
}

/* 키워드 또는 식별자: 단어를 "끝까지 다 읽은 뒤" 키워드 사전과 비교한다.
 * 그래서 selected 는 select + ed 로 잘리지 않고 식별자가 된다. */
static Token read_word(Lexer *lx, Token t) {
    size_t start = lx->pos;
    while (isalnum((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
    size_t len = lx->pos - start;
    if (len >= TOKEN_TEXT_MAX) return error_token(t, "identifier too long");
    memcpy(t.text, lx->src + start, len);
    t.text[len] = '\0';

    t.type = TOK_IDENT;
    for (int i = 0; KEYWORDS[i]; i++) {
        if (str_ieq(t.text, KEYWORDS[i])) {
            t.type = TOK_KEYWORD;
            strcpy(t.text, KEYWORDS[i]);  /* select → SELECT 로 정규화 */
            break;
        }
    }
    return t;
}

static Token read_number(Lexer *lx, Token t) {
    size_t start = lx->pos;
    if (peek(lx) == '-') advance(lx);
    while (isdigit((unsigned char)peek(lx))) advance(lx);
    if (isalpha((unsigned char)peek(lx)) || peek(lx) == '_') {  /* 12abc 같은 입력 */
        while (isalnum((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
        return error_token(t, "invalid number literal");
    }
    size_t len = lx->pos - start;
    if (len >= TOKEN_TEXT_MAX) return error_token(t, "number literal too long");
    memcpy(t.text, lx->src + start, len);
    t.text[len] = '\0';
    t.type = TOK_INT;
    return t;
}

/* 문자열 리터럴: 여는 ' 다음부터 닫는 ' 까지는 쉼표·공백·세미콜론 모두 "그냥 글자"다.
 * 문자열 안의 따옴표는 SQL 표준대로 '' 두 개로 쓴다:  'O''Brien' → O'Brien */
static Token read_string(Lexer *lx, Token t) {
    size_t n = 0;
    int too_long = 0;
    advance(lx);  /* 여는 따옴표 */
    for (;;) {
        char c = peek(lx);
        if (c == '\0') return error_token(t, "unterminated string literal (missing closing quote)");
        advance(lx);
        if (c == '\'') {
            if (peek(lx) == '\'') advance(lx);  /* '' → ' 한 글자 */
            else break;                          /* 진짜 닫는 따옴표 */
        }
        if (n + 1 >= TOKEN_TEXT_MAX) {  /* 너무 길면 표시만 하고 끝까지 읽어 위치를 맞춘다 */
            too_long = 1;
            continue;
        }
        t.text[n++] = c;
    }
    if (too_long) return error_token(t, "string literal too long (max 255 bytes)");
    t.text[n] = '\0';
    t.type = TOK_STRING;
    return t;
}

Token lexer_next(Lexer *lx) {
    skip_whitespace_and_comments(lx);
    Token t = make_token(lx);
    char c = peek(lx);

    if (c == '\0') return t;  /* TOK_EOF */
    if (isalpha((unsigned char)c) || c == '_') return read_word(lx, t);
    if (isdigit((unsigned char)c) || (c == '-' && isdigit((unsigned char)peek_next(lx))))
        return read_number(lx, t);
    if (c == '\'') return read_string(lx, t);

    /* 나머지는 1~2 글자짜리 기호 */
    advance(lx);
    t.text[0] = c;
    t.text[1] = '\0';
    switch (c) {
    case '(': t.type = TOK_LPAREN; return t;
    case ')': t.type = TOK_RPAREN; return t;
    case ',': t.type = TOK_COMMA; return t;
    case ';': t.type = TOK_SEMICOLON; return t;
    case '*': t.type = TOK_STAR; return t;
    case '=': t.type = TOK_EQ; return t;
    case '!':
        if (peek(lx) == '=') {
            advance(lx);
            t.type = TOK_NE;
            strcpy(t.text, "!=");
            return t;
        }
        break;
    case '<':
        if (peek(lx) == '=') { advance(lx); t.type = TOK_LE; strcpy(t.text, "<="); return t; }
        if (peek(lx) == '>') { advance(lx); t.type = TOK_NE; strcpy(t.text, "<>"); return t; }
        t.type = TOK_LT;
        return t;
    case '>':
        if (peek(lx) == '=') { advance(lx); t.type = TOK_GE; strcpy(t.text, ">="); return t; }
        t.type = TOK_GT;
        return t;
    default:
        break;
    }

    char msg[64];
    if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x80)
        snprintf(msg, sizeof msg, "unexpected character (byte 0x%02X)", (unsigned char)c);
    else
        snprintf(msg, sizeof msg, "unexpected character '%c'", c);
    return error_token(t, msg);
}

const char *token_type_name(TokenType t) {
    switch (t) {
    case TOK_KEYWORD: return "keyword";
    case TOK_IDENT: return "identifier";
    case TOK_INT: return "integer";
    case TOK_STRING: return "string";
    case TOK_LPAREN: return "'('";
    case TOK_RPAREN: return "')'";
    case TOK_COMMA: return "','";
    case TOK_SEMICOLON: return "';'";
    case TOK_STAR: return "'*'";
    case TOK_EQ: return "'='";
    case TOK_NE: return "'!='";
    case TOK_LT: return "'<'";
    case TOK_LE: return "'<='";
    case TOK_GT: return "'>'";
    case TOK_GE: return "'>='";
    case TOK_EOF: return "end of input";
    case TOK_ERROR: return "error";
    }
    return "?";
}
