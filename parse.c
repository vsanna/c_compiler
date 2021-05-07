#include "9cc.h"

Token* token;
char* user_input;
LVar* locals[100];
LVar* globals[100];  // TODO: 配列でなくてよい
int cur_scope_depth = 0;
StringToken* strings;
Type* structs[100];  // TODO: at most 100 struct defs
int struct_def_index = 0;

/**************************
 * node builder
 * *************************/
Node* new_node(NodeKind kind) {
    Node* node = calloc(1, sizeof(Node));
    node->kind = kind;
    return node;
}

Node* new_binary(NodeKind kind, Node* lhs, Node* rhs) {
    Node* node = new_node(kind);
    node->lhs = lhs;
    node->rhs = rhs;
    return node;
};

// 数字のnodeを作る特殊
Node* new_num(int val) {
    Node* node = new_node(ND_NUM);
    node->val = val;
    return node;
}

// ND_STRINGのnodeを返す
Node* new_string(StringToken* str) {
    Node* node = new_node(ND_STRING);
    node->string = str;
    return node;
}

// 最大stmt数(仮)
// TODO: 100行まで対応
Node* code[100];

/**************************
 * parse functions
 * *************************/
// program = (func_def | define_variable ";" | "struct" truct_def ";")*
// programをまずは関数の束とする
void program() {
    int i = 0;
    while (!at_eof()) {
        Define* def = read_define_head();
        if (def == NULL) {
            error("def is NULL\n");
            print_token(token);
        }
        if (consume("(")) {
            // 関数定義
            code[i] = func_def(def);
            // } else if (consume_kind(TK_STRUCT)) {
            //     // struct 定義. これはcompilerじにのみ保持する情報なのでnodeは返さない
            //     structs[struct_def_index] = define_struct();
            //     struct_def_index++;
            //     continue;
        } else {
            // global変数の定義
            Node* gvar = define_variable(def, globals);
            // TODO: ここでkind付け替えは良くない。後で直す
            gvar->kind = ND_GVAR_DEF;
            code[i] = gvar;
            expect(";");
        }
        i++;
    }
    code[i] = NULL;
}

// 各structごとにlocalsを設けてあげる
// NOTE: struct自体は何かをアセンブラに吐き出すわけではない(compilerだけが持つ情報)なのでnode不要
// define_struct = "struct" tag(ident)? "{" (define_variable ";")* "}" alias(ident)? ";"
//                          ^start
Type* define_struct() {
    if (!consume_kind(TK_STRUCT)) {
        return NULL;
    }

    expect("{");

    Type* struct_type = calloc(1, sizeof(Type));
    struct_type->ty = STRUCT;

    Member* members = calloc(1, sizeof(Member));

    // TODO: tag名に対応
    // Token* name = consume_kind(TK_IDENT);

    while (!consume("}")) {
        // 型情報を取得し
        Define* def = read_define_head();
        read_define_suffix(def);
        expect(";");
        // membersにつめる
        //// name
        Member* m = calloc(1, sizeof(Member));
        m->name = calloc(100, sizeof(char));
        memcpy(m->name, def->ident->str, def->ident->len);

        //// type
        m->ty = def->type;

        // TODO: 配列でうまく行かない?
        int relative_offset = get_type_size(def->type);
        if (struct_type->members) {
            m->offset = members->offset + relative_offset;
        } else {
            m->offset = relative_offset;
        }
        m->next = struct_type->members;
        struct_type->members = m;
    }

    expect(";");
    return struct_type;
}

// func_def = type-annotation ident "(" ("int" ident ("," "int" ident)*)? ")"
//                                       ^start
// block.
// type-annotation ident までは読み込み済み
Node* func_def(Define* def) {
    // TODO: 本当? 呼び出しごとでは?
    cur_scope_depth++;
    Node* node;

    // 関数定義
    node = new_node(ND_FUNC_DEF);
    node->funcname = calloc(100, sizeof(char));
    memcpy(node->funcname, def->ident->str, def->ident->len);
    node->args = calloc(10, sizeof(char*));
    node->block = calloc(100, sizeof(Node));

    // function args
    for (int i = 0; !consume(")"); i++) {
        if (i != 0) {
            expect(",");
        }

        Define* arg_def = read_define_head();
        if (arg_def == NULL) {
            char* name[100] = {0};
            memcpy(name, token->str, token->len);
            error("invalid token comes here. %d", name);
        }

        Node* variable_node = define_variable(arg_def, locals);
        node->args[i] = variable_node;
    }

    // function body
    node->lhs = block();

    return node;
}

// stmt = expr ";"
//        | "return" expr ";"
//        | "if" "(" expr ")" stmt ("else" stmt)?
//        | "while" "(" expr ")" stmt
//        | "for" "(" expr? ";" expr? ";" expr? ")" stmt
//        | define_variable ";"
//        | block
//        consume_kindココでしてるからそれもdefineにもってく
Node* stmt() {
    Node* node;

    if (consume_kind(TK_RETURN)) {
        node = new_node(ND_RETURN);
        node->lhs = expr();  // lhsだけ持つとする
        expect(";");
        return node;
    }

    if (consume_kind(TK_IF)) {
        /*
        # elseなし
        if -lhs- cond
           -rhs- main

        # elseあり
        if -lhs- cond
           -rhs- else -lhs- main
                      -rhs- alt(stmt)
        */
        node = new_node(ND_IF);
        expect("(");
        node->lhs = expr();  // lhsをcondとする
        expect(")");
        node->rhs = stmt();
        if (consume_kind(TK_ELSE)) {
            Node* els = new_node(ND_ELSE);
            els->lhs = node->rhs;
            els->rhs = stmt();
            node->rhs = els;
        }
        return node;
    }

    if (consume_kind(TK_WHILE)) {
        Node* node = new_node(ND_WHILE);
        expect("(");
        node->lhs = expr();
        expect(")");
        node->rhs = stmt();
        return node;
    }

    if (consume_kind(TK_FOR)) {
        // leftは初期化と終了条件を、
        // rightは後処理とstmtを保持
        Node* node = new_node(ND_FOR);
        Node* left = new_node(ND_FOR_LEFT);
        Node* right = new_node(ND_FOR_RIGHT);
        node->lhs = left;
        node->rhs = right;

        expect("(");
        if (!consume(";")) {
            left->lhs = expr();
            expect(";");
        }

        if (!consume(";")) {
            left->rhs = expr();
            expect(";");
        }

        if (!consume(")")) {
            right->lhs = expr();
            expect(")");
        }

        right->rhs = stmt();
        return node;
    }

    Node* block_node = block();
    if (block_node != NULL) {
        return block_node;
    }

    Define* def = read_define_head();
    if (def != NULL) {
        Node* node = define_variable(def, locals);
        // NOTE: ND_ASSIGNに変換
        node = local_variable_init(node);
        expect(";");
        return node;
    }

    node = expr();
    expect(";");
    return node;
}

// block = "{" stmt* "}"
Node* block() {
    Node* node = NULL;
    if (consume("{")) {
        node = new_node(ND_BLOCK);
        // TODO: 100行まで対応
        node->block = calloc(100, sizeof(Node));
        for (int i = 0; !consume("}"); i++) {
            node->block[i] = stmt();
        }
    }
    return node;
}

// expr = assign
Node* expr() { return assign(); }

// assign = equality ("=" equality)?
Node* assign() {
    Node* node = equality();

    if (consume("=")) {
        node = new_binary(ND_ASSIGN, node, equality());
    }

    return node;
}

// equality = relational ("==" relational | "!=" relational)*
Node* equality() {
    Node* node = relational();

    for (;;) {
        if (consume("==")) {
            node = new_binary(ND_EQ, node, relational());
        } else if (consume("!=")) {
            node = new_binary(ND_NE, node, relational());
        } else {
            return node;
        }
    }
}

// relational = add ("<" add | ">" add | "<=" add | ">=" add)*
Node* relational() {
    Node* node = add();

    for (;;) {
        // TODO: こっち先でよいの?
        // -> OK.
        // なぜならconsumeが見ているのは次のtokenなのですでに<か<=かは考慮済みなので
        if (consume("<")) {
            node = new_binary(ND_LT, node, add());
        } else if (consume("<=")) {
            node = new_binary(ND_LE, node, add());
        } else if (consume(">")) {
            node = new_binary(ND_LT, add(), node);
        } else if (consume(">=")) {
            node = new_binary(ND_LE, add(), node);
        } else {
            return node;
        }
    }
}

// add = mul("*" mul | "/" mul)*
Node* add() {
    Node* node = mul();

    for (;;) {
        if (consume("+")) {
            if (node->type != NULL && node->type->ty != INT) {
                int size_of_type = get_type_size(node->type->ptr_to);
                node = new_binary(
                    ND_ADD, node,
                    new_binary(ND_MUL, mul(), new_num(size_of_type)));
            } else {
                node = new_binary(ND_ADD, node, mul());
            }
        } else if (consume("-")) {
            if (node->type != NULL && node->type->ty != INT) {
                int size_of_type = get_type_size(node->type->ptr_to);
                node = new_binary(
                    ND_SUB, node,
                    new_binary(ND_MUL, mul(), new_num(size_of_type)));
            } else {
                node = new_binary(ND_SUB, node, mul());
            }
        } else {
            return node;
        }
    }
}

// mul = unary("*" unary| "/" unary)*
Node* mul() {
    Node* node = unary();
    for (;;) {
        if (consume("*")) {
            node = new_binary(ND_MUL, node, unary());
        } else if (consume("/")) {
            node = new_binary(ND_DIV, node, unary());
        } else {
            return node;
        }
    }
}

// unary = ("sizeof" | "+"" | "-" | "*" | "&")? unary | primary
Node* unary() {
    if (consume("+")) {
        return unary();
    }
    if (consume("-")) {
        return new_binary(ND_SUB, new_num(0), unary());
    }
    if (consume("*")) {
        // TODO: unaryの部分はpointer型でなかったらエラーにするべき
        return new_binary(ND_DEREF, unary(), NULL);
    }
    if (consume("&")) {
        return new_binary(ND_ADDR, unary(), NULL);
    }

    // sizeof x: xのサイズを返す.
    if (consume_kind(TK_SIZEOF)) {
        // "sizeof" unaryにおけるunaryの末尾までtokenをすすめる
        Node* node = unary();
        int size;
        if (node->kind == ND_NUM) {
            size = 4;
        } else {
            Type* t = get_type(node);
            size = get_type_size(t);
        }
        return new_num(size);
    }

    return primary();
}

// primary = num
//           | ident ("(" (expr ",")* ")")?
//           | "(" expr ")"
//           | '""string'"'
// TODO: primaryにexpr入れたくない
Node* primary() {
    // 次のトークンが ( なら ( expr ) のハズ
    if (consume("(")) {
        Node* node = expr();
        expect(")");
        return node;
    }

    // ident
    Token* tok = consume_kind(TK_IDENT);
    if (tok) {
        // 関数呼び出しの場合
        if (consume("(")) {
            // node->blockに引数となるexprを詰める
            Node* node = new_node(ND_FUNC_CALL);
            node->funcname = calloc(100, sizeof(char));
            memcpy(node->funcname, tok->str, tok->len);
            // TODO: 引数とりあえず10こまで。
            node->block = calloc(10, sizeof(Node));
            for (int i = 0; !consume(")"); i++) {
                if (i != 0) {
                    expect(",");
                }
                node->block[i] = expr();
            }

            return node;
        }

        // 変数呼び出しの場合
        return variable(tok);
    }

    // string
    if (tok = consume_kind(TK_STRING)) {
        // TODO: builder 関数を整理したい
        StringToken* s = calloc(1, sizeof(StringToken));
        s->value = calloc(100, sizeof(char));
        memcpy(s->value, tok->str, tok->len);
        if (strings) {
            s->index = strings->index + 1;
        } else {
            s->index = 0;
        }
        s->next = strings;
        Node* node = new_string(s);
        strings = s;
        return node;
    }

    // そうでなければnumのハズ
    return new_num(expect_number());
}

// stmtの一つ. 変数を宣言する.
// ND_LVARを返しつつ、LVarを作ってlocalsに追加する
// TODO: これは意味解析も一緒にやっちゃってないか? それはいいの?
// TODO: scopeが異なる変数が見つかった場合はエラーではないが,
// 同一scopeで見つかったらエラーみたいなことをするにはまたロジックを考える必要がある
// TODO: block切って同名/別型変数を宣言したときに既存言語でエラーになるかどうか
// NOTE: 型宣言の最初にいる状態で呼ばれる. *とidentの処理を行う
// define_variable = "int" "*"* ident ("=" 初期化式)
//                                    ^start/end
//                 | "int" "*"* ident "[" number "]" ("=" 初期化式)
//                                    ^start        ^end
Node* define_variable(Define* def, LVar** varlist) {
    if (def == NULL) {
        error("invalid definition of function or variable");
    }

    read_define_suffix(def);

    Type* type = def->type;
    Token* tok = def->ident;

    int size = get_type_size(type);

    Node* init = NULL;
    if (consume("=")) {
        /*
        TODO: なぜassignにせずにinitをもたせる形にしたのか...
        なぜならアセンブラとして吐き出したいコードがぜんぜん違うから。
        - assign: 左辺のアドレスをpush -> 右辺を評価 -> 右辺の値をpush -> 左辺のアドレスに右辺の値をmov
        - globalのinit: .data領域に書き込み
        - localのinit: 初期化式でのみ使える表現がある. よってだめ.

        > 初期化式は一見ただの代入式のように見えますが、実際は初期化式と代入式は文法的にかなり異なっていて、
        > 初期化式だけに許された特別な書き方というものがいくつかあります。
        > ここでその特別な書き方をきちんと把握しておきましょう。
        */
        if (consume("{")) {
            // TODO 最大100要素まで
            init = calloc(1, sizeof(Node));
            init->block = calloc(100, sizeof(Node));
            int i = 0;
            for (; !consume("}"); i++) {
                if (i != 0) {
                    expect(",");
                }

                init->block[i] = expr();
            }
            // 初期化子の数のほうがサイズよりも大きい場合はそちらでうわがく
            if (type->array_size < i) {
                type->array_size = i;
            }

            // int a[5] = {5} のとき
            // この時点で i = 1
            // array_size = 5

            // 不足分の要素を埋める
            for (; i < type->array_size; i++) {
                init->block[i] = new_num(0);
            }
        } else {
            init = expr();
            if (init->kind == ND_STRING) {
                int len = strlen(init->string->value) + 1;
                if (type->array_size < len) {
                    type->array_size = len;
                }
            }
        }
    }

    if (type->ty == ARRAY) {
        if (type->array_size == 0) {
            error("array size is not defined.\n");
        }
        size = get_type_size(type);
    }

    Node* node = new_node(ND_LVAR);
    LVar* var = find_variable(tok);

    node->varname = calloc(100, sizeof(char));
    memcpy(node->varname, tok->str, tok->len);
    node->varsize = size;

    // 新規変数の定義なのでlvarあってはこまる
    if (var) {
        error("redefining variable %s", node->varname);
    }

    // 新規変数なのでlvarを追加する
    var = calloc(1, sizeof(LVar));
    var->next = varlist[cur_scope_depth];
    var->name = tok->str;
    var->len = tok->len;
    var->type = type;
    var->kind = (varlist == locals) ? LOCAL : GLOBAL;
    var->init = init;

    // TODO: きれいにする
    int scope_depth = varlist == locals ? cur_scope_depth : 0;
    if (varlist[scope_depth] == NULL) {
        var->offset = size;
    } else {
        var->offset = varlist[scope_depth]->offset + size;
    }
    varlist[scope_depth] = var;

    // nodeを完成させる
    node->offset = varlist[scope_depth]->offset;
    node->type = type;
    node->kind = (var->kind == LOCAL) ? ND_LVAR : ND_GVAR;
    node->var = var;
    return node;
}

// Node* initializer(Type* type, int* size) {
//     Node* init = NULL;
//     if (consume("=")) {
//         /*
//         TODO: なぜassignにせずにinitをもたせる形にしたのか...
//         なぜならアセンブラとして吐き出したいコードがぜんぜん違うから。
//         - assign: 左辺のアドレスをpush -> 右辺を評価 -> 右辺の値をpush -> 左辺のアドレスに右辺の値をmov
//         - globalのinit: .data領域に書き込み
//         - localのinit: 初期化式でのみ使える表現がある. よってだめ.

//         > 初期化式は一見ただの代入式のように見えますが、実際は初期化式と代入式は文法的にかなり異なっていて、
//         > 初期化式だけに許された特別な書き方というものがいくつかあります。
//         > ここでその特別な書き方をきちんと把握しておきましょう。
//         */
//         if (consume("{")) {
//             // TODO 最大100要素まで
//             init = calloc(1, sizeof(Node));
//             init->block = calloc(100, sizeof(Node));
//             int i = 0;
//             for (; !consume("}"); i++) {
//                 if (i != 0) {
//                     expect(",");
//                 }

//                 init->block[i] = expr();
//             }
//             // 初期化子の数のほうがサイズよりも大きい場合はそちらでうわがく
//             if (type->array_size < i) {
//                 type->array_size = i;
//             }

//             // 不足分の要素を埋める
//             i++;
//             for (; i < type->array_size; i++) {
//                 init->block[i] = new_num(0);
//             }
//         } else {
//             init = expr();
//             if (init->kind == ND_STRING) {
//                 int len = type->array_size = strlen(init->string->value) + 1;
//                 if (type->array_size < len) {
//                     type->array_size = len;
//                 }
//             }
//         }
//     }

//     if (type->ty == ARRAY) {
//         if (type->array_size == 0) {
//             error("array size is not defined.\n");
//         }
//         *size *= type->array_size;
//     }

//     return init;
// }

// TODO: これは本当? 外のscopeの値をそのまま利用 or
// ウワがいてしまう
// a[1] ==> *(a+1)
// variable = ident ("[" num "]")?
//            ^start
Node* variable(Token* tok) {
    Node* node = new_node(ND_LVAR);
    LVar* lvar = find_variable(tok);
    if (lvar == NULL) {
        char name[100] = {0};
        memcpy(name, tok->str, tok->len);
        error("variable %s is not defined yet", name);
    }

    // すでに宣言済みの変数であればそのoffsetを使う
    node->offset = lvar->offset;
    node->type = lvar->type;

    // TODO: kindの付け替えはしたくない
    node->kind = (lvar->kind == LOCAL) ? ND_LVAR : ND_GVAR;
    node->varname = calloc(100, sizeof(char));
    memcpy(node->varname, tok->str, tok->len);

    // もし a[1][2]のaがnodeとして渡ってきたなら、aのtypeは PTR of PTR of INT
    // のはず

    // DEBUG
    Node* original_node = node;

    Type* tp = node->type;
    bool has_index = false;
    while (consume("[")) {
        has_index = true;
        // a[3] は *(a + 3) にする
        // DEREF -- ADD -- a
        //              -- MUL -- 3(expr)
        //                     -- sizeof(a)

        // 大きな1歩の中に小さな2歩
        // (a + 1)はあどれすで、[a + 1]が示す値もアドレス
        // int a[n][m]; にたいして、 a[1][2]を取得するには
        // DEREF( (a + sizeof(int * m: 1行のsize) * 1) + sizeof(int:
        // 1cellのサイズ) * 2 ) 最後にだけderefする
        Node* add = new_node(ND_ADD);
        add->lhs = node;  // 変数

        Node* index_val = new_num(expect_number());

        // NOTE:
        // ここのtype_sizeでみるのは要素のサイズなので常にptr_to一個先をみる
        add->rhs =
            new_binary(ND_MUL, index_val, new_num(get_type_size(tp->ptr_to)));
        tp = tp->ptr_to;
        // TODO: node(ND_LVAR)のtypeが配列のままなのにptrになっちゃってない?
        node = add;
        expect("]");
    }
    if (has_index) {
        Node* deref_node = new_node(ND_DEREF);
        deref_node->lhs = node;
        node = deref_node;
    }

    return node;
}

// まずlocal変数を探してなければglobal変数を探す
LVar* find_variable(Token* tok) {
    for (LVar* var = locals[cur_scope_depth]; var; var = var->next) {
        // NOTE: memcmpは一致していたら0を返す
        if (var->len == tok->len && !memcmp(tok->str, var->name, var->len)) {
            var->kind = LOCAL;
            return var;
        }
    }

    // TODO: globalsを配列でなくしてこの添字を消す
    for (LVar* var = globals[0]; var; var = var->next) {
        // NOTE: memcmpは一致していたら0を返す
        if (var->len == tok->len && !memcmp(tok->str, var->name, var->len)) {
            var->kind = GLOBAL;
            return var;
        }
    }
    return NULL;
}

// 初期化の=よりもあとから.
Node* local_variable_init(Node* node) {
    if (!node->var->init) {
        return node;
    }

    // 初期化式が配列
    // int a[2] = {1, 2};
    // を
    // int a[2];
    // a[0] = 1;
    // a[1] = 2;
    // として評価する
    //
    // 初期化式の数がarray_sizeに足りてない場合はzero値いれる

    // TODO: 真下の処理と統一
    if (node->var->init->kind == ND_STRING) {
        // 文字列の場合は各charをそれぞれ代入する形にする
        // NOTE: この時点ではnode->varは用意済み. ident/type入ってる. typeがarrayの場合もsize入れ込み済み
        Node* block = new_node(ND_BLOCK);
        block->block = calloc(node->var->type->array_size, sizeof(Node));

        int len = strlen(node->var->init->string->value);
        for (int i = 0; i < node->var->type->array_size; i++) {
            // a[0] ==> *(a + 0 * size)
            Node* add = new_node(ND_ADD);
            add->lhs = node;
            if (node->type && node->type->ty != INT) {
                int size = get_type_size(node->type->ptr_to);
                add->rhs = new_num(i * size);
            }
            Node* deref = new_node(ND_DEREF);
            deref->lhs = add;

            Node* assign = new_node(ND_ASSIGN);
            assign->lhs = deref;

            if (len > i) {
                assign->rhs = new_num(node->var->init->string->value[i]);
            } else {
                assign->rhs = new_num(0);
            }

            block->block[i] = assign;
        }

        return block;
    }

    if (node->var->init->block) {
        Node* block = new_node(ND_BLOCK);
        block->block = calloc(node->var->type->array_size, sizeof(Node));

        for (int i = 0; node->var->init->block[i]; i++) {
            // a[0] ==> *(a + 0 * size)
            Node* add = new_node(ND_ADD);
            add->lhs = node;
            if (node->type && node->type->ty != INT) {
                int size = get_type_size(node->type->ptr_to);
                add->rhs = new_num(i * size);
            }
            Node* deref = new_node(ND_DEREF);
            deref->lhs = add;

            Node* assign = new_node(ND_ASSIGN);
            assign->lhs = deref;
            assign->rhs = node->var->init->block[i];

            block->block[i] = assign;
        }

        return block;
    }

    Node* assign = new_node(ND_ASSIGN);
    assign->lhs = node;
    assign->rhs = node->var->init;

    return assign;
}

/**************************
 * utils
 * *************************/

// TODO: *(a + 1) * b の場合、bはint, aはptrだと、なんの型がかえる?
// 型を探す.
Type* get_type(Node* node) {
    if (node == NULL) {
        return NULL;
    }

    if (node->type) {
        return node->type;
    }

    // 先に左見て、なければ右みる
    Type* t = get_type(node->lhs);
    if (!t) {
        t = get_type(node->rhs);
    }

    // DEREFの場合はとってきたtypeの指す先を返す(型のderef)
    // TODO: ココよく理解する
    if (t && node->kind == ND_DEREF) {
        t = t->ptr_to;
        if (t == NULL) {
            error("invalid dereference");
        }
        return t;
    }
    return t;
}

// 渡ってきたtypeのサイズを返す.
// INT -> 4
// PTR -> 8
// CHAR -> 1
int get_type_size(Type* type) {
    if (type == NULL) {
        error("type should be non null");
    }

    switch (type->ty) {
        case INT:
            return 4;
        case PTR:
            return 8;
        case CHAR:
            return 1;
        case ARRAY:
            return get_type_size(type->ptr_to) * type->array_size;
        default:
            error("unexpected Type->ty comes here\n");
    }
}

// type_annotation = ("int"|"char") "*"*
// assume:  current token = ("int"|"char")
// move to: current token = *の次
// TODO: type_prefixにrenameしてtype_suffixと連動させる
Type* type_annotation() {
    Token* typeToken = consume_kind(TK_TYPE);
    if (typeToken == NULL) {
        return NULL;
    }

    Type* type = calloc(1, sizeof(Type));
    int isChar = memcmp("char", typeToken->str, typeToken->len) == 0;

    char* tmp[100] = {0};
    memcpy(tmp, typeToken->str, typeToken->len);
    type->ty = isChar ? CHAR : INT;

    // ここで*を読む
    while (consume("*")) {
        Type* t = calloc(1, sizeof(Type));
        t->ty = PTR;
        t->ptr_to = type;
        type = t;
    }

    return type;
}

// 関数またはグローバル変数の定義の前半のみをreadする
// その読んだ情報をDefineをcontainerとして流用してそこに詰めて返す
// int** hoge までを読む.
// assume:  current token = int
// move to: current token = ident
Define* read_define_head() {
    // read type "*"*
    Type* type = type_annotation();
    if (type == NULL) {
        return NULL;
    }

    Node* node = NULL;
    Token* tok = consume_kind(TK_IDENT);

    if (tok == NULL) {
        error("function name(ident) should come here.");
    }

    Define* def = calloc(1, sizeof(Define));
    def->type = type;
    def->ident = tok;

    return def;
}

// Defineのtypeに配列情報を付け加える. つまり型定義のsuffix部分をみる
void read_define_suffix(Define* def) {
    if (def == NULL) {
        error("invalid definition of function or variable");
    }

    Type* type = def->type;
    Token* tok = def->ident;

    // check if it's array or not;
    while (consume("[")) {
        Type* t;
        t = calloc(1, sizeof(Type));
        t->ty = ARRAY;
        t->ptr_to = type;

        // size of array is optional.
        t->array_size = 0;
        Token* num = NULL;
        if (num = consume_kind(TK_NUM)) {
            t->array_size = num->val;
        }

        type = t;
        expect("]");
        fprintf(stderr, "[DEBUG] arary size: %ld\n", type->array_size);
    }

    def->type = type;
}
