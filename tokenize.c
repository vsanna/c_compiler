#include "9cc.h"

Token *token;
char *user_input;
LVar *locals[100];

// 次のトークンが期待している記号のときにはトークンを一つ読み進めてtrueを返す
// それ以外にはfalseを返す
bool consume(char *op) {
    if (!check(op)) {
        return false;
    }
    advance_token();
    return true;
}

// current tokenが指定したTokenKindであれば、それを返しつつ、1つよみ進める
// それ以外はNULL
Token *consume_kind(TokenKind kind) {
    if (!check_kind(kind)) {
        return NULL;
    }
    Token *tok = token;
    advance_token();
    return tok;
}

// current tokenが指定した記号かどうかを判定. 進めはしない
bool check(char *op) {
    return (token->kind == TK_RESERVED) && (strlen(op) == token->len) &&
           (memcmp(token->str, op, token->len) == 0);
}

// current tokenが指定したTokenKindかどうかを調べる. 進めはしない.
bool check_kind(TokenKind kind) { return token->kind == kind; }

// 次のトークンが期待している記号のときにはトークンを一つ読み進める。
// それ以外にはエラーを投げる
void expect(char *op) {
    if (token->kind != TK_RESERVED || strlen(op) != token->len ||
        memcmp(token->str, op, token->len)) {
        char *tmp[100] = {0};
        memcpy(tmp, token->str, token->len);
        error_at2(token->str, "expected: '%c'\nactual: '%s'\n", *op, tmp);
    }
    advance_token();
    return;
}

// 次のトークンが数値の場合、トークンを一つ読み進めてその数値を返す
// それ以外の場合にはエラー
int expect_number() {
    if (token->kind != TK_NUM) {
        int debug;
        error_at0(token->str, "数ではありません\n");
    }
    int val = token->val;
    advance_token();
    return val;
}

// step forward by 1 token
void advance_token() {
    // for debugging
    // print_token(token);

    token = token->next;
    return;
}

bool at_eof() { return token->kind == TK_EOF; }

// 新しいトークンを作成してcurのnextにセット
// TODO: "+ 12 - hoge" が渡ってきたとき、tok->str = str
// で先頭の1文字だけ渡るのはなぜ
//   ->
//   わかった。その位置のアドレスを保持しているのみで、ちゃんとsplitしているわけではない...
Token *new_token(TokenKind kind, Token *cur, char *str, int len) {
    // callocは割り当てられたメモリをゼロクリアしてくれる
    Token *tok = calloc(1, sizeof(Token));
    tok->kind = kind;
    tok->str = str;  // charだけをsetしてる?
    tok->len = len;
    cur->next = tok;
    return tok;
}

bool startswith(char *p, char *q) { return memcmp(p, q, strlen(q)) == 0; }

bool is_alnum(char p) {
    return ('a' <= p && p <= 'z') || ('A' <= p && p <= 'Z') ||
           ('0' <= p && p <= '9') || (p == '_');
}

typedef struct ReservedWord {
    char *word;
    TokenKind kind;
} ReservedWord;

ReservedWord reserved_words[] = {
    {"return", TK_RETURN},     {"if", TK_IF},         {"else", TK_ELSE},
    {"while", TK_WHILE},       {"for", TK_FOR},       {"int", TK_TYPE},
    {"void", TK_TYPE},         {"char", TK_TYPE},     {"bool", TK_TYPE},
    {"size_t", TK_TYPE},       {"sizeof", TK_SIZEOF}, {"struct", TK_STRUCT},
    {"typedef", TK_TYPEDEF},   {"enum", TK_ENUM},     {"break", TK_BREAK},
    {"continue", TK_CONTINUE}, {"switch", TK_SWITCH}, {"case", TK_CASE},
    {"default", TK_DEFAULT},   {"", TK_EOF},
};

// 入力文字列pをトークない頭してそれを返す
// headはコードを簡単にするためのtrick.
// ポインターでlinkedlistを作るときの定石らしい.
Token *tokenize() {
    char *p = user_input;
    Token head;
    head.next = NULL;
    head.len = 0;
    head.str = NULL;
    Token *cur = &head;

    while (*p) {
        print_token(cur);

        // 空白
        if (isspace(*p)) {
            p++;
            continue;
        }

        // 行コメント
        if (startswith(p, "//")) {
            while (!startswith(p, "\n")) {
                p++;
            }
            continue;
        }

        // skip #include
        // TODO: skip all macro
        if (startswith(p, "#include")) {
            while (!startswith(p, "\n")) {
                p++;
            }
            continue;
        }

        // skip extern
        // TODO: skip all macro
        if (startswith(p, "extern")) {
            while (!startswith(p, "\n")) {
                p++;
            }
            continue;
        }

        // ブロックコメント
        if (startswith(p, "/*")) {
            char *q = strstr(p + 2, "*/");
            if (!q) {
                error_at0(p, "block comment is not closed.\n");
            }
            p = q + 2;
            continue;
        }

        // identの前に処理する
        // returnがきてなおかつnの次がtokenを構成する文字列ではない
        // p[6]: pの示すアドレス+6にある値
        bool is_broken = false;
        for (int i = 0; reserved_words[i].kind != TK_EOF; i++) {
            char *word = reserved_words[i].word;
            int len = strlen(word);
            TokenKind kind = reserved_words[i].kind;

            if (startswith(p, word) && !is_alnum(p[len])) {
                cur = new_token(kind, cur, p, len);
                p += len;
                is_broken = true;
                break;
            }
        }
        if (is_broken) {
            continue;
        }

        if (startswith(p, "==") || startswith(p, "!=") || startswith(p, "<=") ||
            startswith(p, ">=") || startswith(p, "->") || startswith(p, "+=") ||
            startswith(p, "-=") || startswith(p, "*=") || startswith(p, "/=") ||
            startswith(p, "++") || startswith(p, "--") || startswith(p, "&&") ||
            startswith(p, "||")) {
            cur = new_token(TK_RESERVED, cur, p, 2);
            p += 2;
            continue;
        }

        if (strchr(".+-*/()<>;={},&[]!~|^?:", *p)) {
            cur = new_token(TK_RESERVED, cur, p, 1);
            p++;
            continue;
        }

        // alphabet+いくつかの記号からなる文字列のまとまりは変数名
        if (('a' <= *p && *p <= 'z') || ('A' <= *p && *p <= 'Z')) {
            char *start = p;
            while (is_alnum(*p)) {
                p++;
            }
            cur = new_token(TK_IDENT, cur, start, (p - start));
            continue;
        }

        // TODO:
        // 今の箇所からどれだけの長さがdigitなのか、についてどう判断するのか
        if (isdigit(*p)) {
            cur = new_token(TK_NUM, cur, p, 0);
            char *q = p;
            // 数字分だけ前に進む
            cur->val = strtol(p, &p, 10);
            cur->len = p - q;
            continue;
        }

        if ('"' == *p) {
            p++;
            char *c = p;
            while ('"' != *c) {
                c++;
            }
            int len = c - p;
            cur = new_token(TK_STRING, cur, p, len);
            p = c;
            p += 1;
            continue;
        }

        error_at0(token->str, "トークナイズできません");
    }

    // print_token(cur);
    new_token(TK_EOF, cur, p, 0);
    return head.next;
}
