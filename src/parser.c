#include "parser.h"
#include "lexer.h"
#include "syntax.h"
#include "mem_pool.h"
#include "utils.h"

#include <string.h>
#include <stdbool.h>
#include <assert.h>

static inline void skip_token(Parser* parser) {
    parser->prev_end = parser->ahead.range.end;
    parser->ahead = lex(parser->lexer);
}

Parser make_parser(MemPool* mem_pool, Lexer* lexer) {
    Parser parser = {
        .mem_pool = mem_pool,
        .lexer = lexer
    };
    skip_token(&parser);
    return parser;
}

static inline char* extract_str(MemPool* mem_pool, const char* file_data, size_t begin, size_t end) {
    size_t len = end < begin ? 0 : end - begin;
    char* str = mem_pool_alloc(mem_pool, len + 1);
    memcpy(str, file_data + begin, len);
    str[len] = 0;
    return str;
}

static inline const char* extract_default_val(MemPool* mem_pool, const char* info, const SourceRange* range) {
    const char* default_begin = strstr(info, "[default:");
    if (!default_begin)
        return NULL;
    default_begin += 9;
    default_begin += strspn(default_begin, " \t");
    const char* default_end = strchr(default_begin, ']');
    if (!default_end)
    {
        default_end = strpbrk(default_begin, " \t");
        if (!default_end)
            default_end = default_begin + strlen(default_begin);
        error_at(range, "unterminated default value specifier");
    }
    size_t len = default_end - default_begin;
    char* default_val = mem_pool_alloc(mem_pool, len + 1);
    memcpy(default_val, default_begin, len);
    default_val[len] = 0;
    return default_val;
}

static inline Syntax* make_syntax(Parser* parser, const SourcePos* begin, const Syntax* syntax) {
    Syntax* new_syntax = mem_pool_alloc(parser->mem_pool, sizeof(Syntax));
    memcpy(new_syntax, syntax, sizeof(Syntax));
    new_syntax->range.file_name = parser->lexer->file_name;
    new_syntax->range.begin = *begin;
    new_syntax->range.end = parser->prev_end;
    return new_syntax;
}

static inline bool accept_token(Parser* parser, TokenTag tag) {
    if (parser->ahead.tag == tag) {
        skip_token(parser);
        return true;
    }
    return false;
}

static inline void error_on_token(Parser* parser, const char* context) {
    if (parser->ahead.tag == TOKEN_NL || parser->ahead.tag == TOKEN_END) {
        error_at(&parser->ahead.range, "expected %s, but got %s",
            context, get_token_tag_name(parser->ahead.tag));
    } else {
        error_at(&parser->ahead.range, "expected %s, but got '%.*s'",
            context,
            get_source_range_len(&parser->ahead.range),
            get_source_range_str(&parser->ahead.range, parser->lexer->file_data));
    }
    skip_token(parser);
}

static inline bool expect_token(Parser* parser, TokenTag tag) {
    if (!accept_token(parser, tag)) {
        error_on_token(parser, get_token_tag_name(tag));
        return false;
    }
    return true;
}

static inline void eat_token(Parser* parser, TokenTag tag) {
    assert(parser->ahead.tag == tag);
    skip_token(parser);
}

static void skip_non_opt_lines(Parser* parser, SourcePos* end) {
    while (parser->ahead.tag != TOKEN_END) {
        *end = parser->ahead.range.begin;
        while (accept_token(parser, TOKEN_NL));
        if (parser->ahead.tag == TOKEN_LOPT || parser->ahead.tag == TOKEN_SOPT)
            break;
        skip_line(parser->lexer);
        skip_token(parser);
    }
}

static Syntax* parse_error(Parser* parser, const char* context) {
    SourcePos begin = parser->ahead.range.begin;
    error_on_token(parser, context);
    skip_token(parser);
    return make_syntax(parser, &begin, &(Syntax) { .tag = SYNTAX_ERROR });
}

static const char* parse_ident(Parser* parser) {
    const char* ident = extract_str(parser->mem_pool,
        parser->lexer->file_data,
        parser->ahead.range.begin.bytes,
        parser->ahead.range.end.bytes);
    expect_token(parser, TOKEN_IDENT);
    return ident;
}

static Syntax* parse_many(Parser* parser, TokenTag stop, Syntax* (*parse_one)(Parser*)) {
    Syntax* first = NULL;
    Syntax** prev = &first;
    while (parser->ahead.tag != stop) {
        Syntax* next = parse_one(parser);
        *prev = next;
        prev = &next->next;
    }
    return first;
}

static Syntax* parse_or(Parser*);

static Syntax* parse_arg(Parser* parser) {
    SourcePos begin = parser->ahead.range.begin;
    size_t skip = parser->ahead.tag == TOKEN_DELIMARG ? 1 : 0;
    const char* name = extract_str(parser->mem_pool,
        parser->lexer->file_data,
        parser->ahead.range.begin.bytes + skip,
        parser->ahead.range.end.bytes - skip);
    skip_token(parser);
    return make_syntax(parser, &begin, &(Syntax) { .tag = SYNTAX_ARG, .arg.name = name });
}

static Syntax* parse_opt(Parser* parser) {
    SourcePos begin = parser->ahead.range.begin;
    bool is_short = parser->ahead.tag == TOKEN_SOPT;
    char* name = extract_str(parser->mem_pool,
        parser->lexer->file_data,
        parser->ahead.range.begin.bytes + (is_short ? 1 : 2),
        parser->ahead.range.end.bytes);
    eat_token(parser, is_short ? TOKEN_SOPT : TOKEN_LOPT);
    char* arg = strpbrk(name, "= ");
    if (arg) {
        *(arg++) = 0;
        arg += strspn(arg, "<>");
        arg[strcspn(arg, "<>")] = 0;
    }
    return make_syntax(parser, &begin, &(Syntax) {
        .tag = SYNTAX_OPTION,
        .option = {
            .is_short = is_short,
            .name = name,
            .arg = arg
        }
    });
}

static Syntax* parse_command(Parser* parser) {
    SourcePos begin = parser->ahead.range.begin;
    const char* name = parse_ident(parser);
    return make_syntax(parser, &begin, &(Syntax) { .tag = SYNTAX_COMMAND, .command.name = name });
}

static Syntax* parse_parens(Parser* parser) {
    SourcePos begin = parser->ahead.range.begin;
    eat_token(parser, TOKEN_LPAREN);
    Syntax* elems = parse_many(parser, TOKEN_RPAREN, parse_or);
    expect_token(parser, TOKEN_RPAREN);
    return make_syntax(parser, &begin, &(Syntax) { .tag = SYNTAX_PARENS, .parens.elems = elems });
}

static Syntax* parse_brackets(Parser* parser) {
    SourcePos begin = parser->ahead.range.begin;
    eat_token(parser, TOKEN_LBRACKET);
    Syntax* elems = parse_many(parser, TOKEN_RBRACKET, parse_or);
    expect_token(parser, TOKEN_RBRACKET);
    return make_syntax(parser, &begin, &(Syntax) { .tag = SYNTAX_BRACKETS, .brackets.elems = elems });
}

static Syntax* parse_elem(Parser* parser) {
    switch (parser->ahead.tag) {
        case TOKEN_IDENT:    return parse_command(parser);
        case TOKEN_SOPT:     // fallthrough
        case TOKEN_LOPT:     return parse_opt(parser);
        case TOKEN_UPPERARG: // fallthrough
        case TOKEN_DELIMARG: return parse_arg(parser);
        case TOKEN_LPAREN:   return parse_parens(parser);
        case TOKEN_LBRACKET: return parse_brackets(parser);
        default:
            return parse_error(parser, "option or positional argument");
    }
}

static Syntax* parse_repeat(Parser* parser) {
    SourcePos begin = parser->ahead.range.begin;
    Syntax* elem = parse_elem(parser);
    if (!accept_token(parser, TOKEN_DOTS))
        return elem;
    return make_syntax(parser, &begin, &(Syntax) { .tag = SYNTAX_REPEAT, .repeat.elem = elem });
}

static Syntax* parse_or(Parser* parser) {
    SourcePos begin = parser->ahead.range.begin;
    Syntax* first_elem = parse_repeat(parser);
    Syntax** prev_elem = &first_elem->next;
    while (accept_token(parser, TOKEN_OR)) {
        (*prev_elem) = parse_repeat(parser);
        prev_elem = &(*prev_elem)->next;
    }
    if (!first_elem->next)
        return first_elem;
    return make_syntax(parser, &begin, &(Syntax) { .tag = SYNTAX_OR, .or_.elems = first_elem });
}

static Syntax* parse_usage(Parser* parser) {
    SourcePos begin = parser->ahead.range.begin;
    const char* prog = parse_ident(parser);
    Syntax* elems = parse_many(parser, TOKEN_NL, parse_or);
    expect_token(parser, TOKEN_NL);
    return make_syntax(parser, &begin, &(Syntax) {
        .tag = SYNTAX_USAGE,
        .usage = {
            .prog = prog,
            .elems = elems
        }
    });
}

static Syntax* parse_desc(Parser* parser) {
    SourcePos begin = parser->ahead.range.begin;

    Syntax* first_elem = parse_elem(parser);
    Syntax** prev_elem = &first_elem->next;
    while (
        !parser->ahead.is_separated &&
        (parser->ahead.tag == TOKEN_SOPT || parser->ahead.tag == TOKEN_LOPT))
    {
        *prev_elem = parse_opt(parser);
        prev_elem = &(*prev_elem)->next;
        accept_token(parser, TOKEN_COMMA);
    }

    const char* info = "";
    const char* default_val = NULL;
    if (parser->ahead.is_separated)
    {
        SourcePos info_begin = parser->ahead.range.begin, info_end;
        skip_line(parser->lexer);
        skip_token(parser);
        skip_non_opt_lines(parser, &info_end);
        info = extract_str(parser->mem_pool, parser->lexer->file_data, info_begin.bytes, info_end.bytes);
        default_val = extract_default_val(parser->mem_pool, info, &(SourceRange) {
            .file_name = parser->lexer->file_name,
            .begin = info_begin,
            .end = info_end
        });
    }
    else
        error_on_token(parser, "option description");

    return make_syntax(parser, &begin, &(Syntax) {
        .tag = SYNTAX_DESC,
        .desc = {
            .info = info,
            .default_val = default_val,
            .elems = first_elem
        }
    });
}

static Syntax* parse_descs(Parser* parser) {
    SourcePos end;
    Syntax* first_desc = NULL;
    Syntax** prev_desc = &first_desc;
    while (true) {
        skip_non_opt_lines(parser, &end);
        if (parser->ahead.tag == TOKEN_END)
            break;
        *prev_desc = parse_desc(parser);
        prev_desc = &(*prev_desc)->next;
    }
    return first_desc;
}

static bool locate_usage(Parser* parser, SourcePos* end) {
    while (true) {
        *end = parser->ahead.range.begin;
        while (accept_token(parser, TOKEN_NL));
        if (parser->ahead.tag == TOKEN_END)
            return false;
        if (parser->ahead.tag == TOKEN_USAGE)
            break;
        skip_line(parser->lexer);
        skip_token(parser);
    }
    eat_token(parser, TOKEN_USAGE);
    accept_token(parser, TOKEN_NL);
    return true;
}

static void check_usages(Syntax* usages) {
    for (Syntax* usage = usages; usage; usage = usage->next) {
        if (strcmp(usage->usage.prog, usages->usage.prog)) {
            error_at(&usage->range, "expected program name '%s', but got '%s'",
                usages->usage.prog, usage->usage.prog);
        }
    }
}

Syntax* parse(Parser* parser) {
    SourcePos begin = parser->ahead.range.begin;
    SourcePos info_end;
    if (!locate_usage(parser, &info_end))
        return parse_error(parser, "usage or option list");

    const char* info = extract_str(parser->mem_pool,
        parser->lexer->file_data, begin.bytes, info_end.bytes);

    Syntax* usages = parse_many(parser, TOKEN_NL, parse_usage);
    check_usages(usages);

    Syntax* descs = parse_descs(parser);
    return make_syntax(parser, &begin, &(Syntax) {
        .tag = SYNTAX_ROOT,
        .root = {
            .info = info,
            .usages = usages,
            .descs = descs
        }
    });
}
