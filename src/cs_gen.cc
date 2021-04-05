#include <cubescript/cubescript.hh>
#include "cs_gen.hh"
#include "cs_std.hh"
#include "cs_parser.hh"

#include <ctype.h>

#include <limits>
#include <iterator>

namespace cubescript {

std::string_view codegen_state::get_str() {
    size_t nl;
    char const *beg = source;
    source = parse_string(
        *ts.pstate, std::string_view{source, std::size_t(send - source)}, nl
    );
    current_line += nl - 1;
    auto ret = std::string_view{beg, std::size_t(source - beg)};
    return ret.substr(1, ret.size() - 2);
}

charbuf codegen_state::get_str_dup() {
    charbuf buf{ts};
    unescape_string(std::back_inserter(buf), get_str());
    return buf;
}

std::string_view codegen_state::read_macro_name() {
    char const *op = source;
    char c = current();
    if (!isalpha(c) && (c != '_')) {
        return std::string_view{};
    }
    for (; isalnum(c) || (c == '_'); c = current()) {
        next_char();
    }
    return std::string_view{op, std::size_t(source - op)};
}

char codegen_state::skip_until(std::string_view chars) {
    char c = current();
    while (c && (chars.find(c) == std::string_view::npos)) {
        next_char();
        c = current();
    }
    return c;
}

char codegen_state::skip_until(char cf) {
    char c = current();
    while (c && (c != cf)) {
        next_char();
        c = current();
    }
    return c;
}

static bool is_hspace(char c) {
    return (c == ' ') || (c == '\t') || (c == '\r');
}

void codegen_state::skip_comments() {
    for (;;) {
        for (char c = current(); is_hspace(c); c = current()) {
            next_char();
        }
        if (current() == '\\') {
            char c = current(1);
            if ((c != '\r') && (c != '\n')) {
                throw error{*ts.pstate, "invalid line break"};
            }
            /* skip backslash */
            next_char();
            /* skip CR or LF */
            next_char();
            /* when CR, try also skipping LF; covers \r, \n, \r\n */
            if ((c == '\r') && (current(1) == '\n')) {
                next_char();
            }
            /* skip whitespace on new line */
            continue;
        }
        if ((current() != '/') || (current(1) != '/')) {
            return;
        }
        while (current() != '\n') {
            next_char();
        }
    }
}

std::string_view codegen_state::get_word() {
    char const *beg = source;
    source = parse_word(
        *ts.pstate, std::string_view{source, std::size_t(send - source)}
    );
    return std::string_view{beg, std::size_t(source - beg)};
}

static inline int ret_code(int type, int def = 0) {
    if (type >= VAL_ANY) {
        return def;
    }
    return type << BC_INST_RET;
}

static void compilestatements(
    codegen_state &gs, int rettype, int brak = '\0'
);
static inline std::pair<std::string_view, size_t> compileblock(
    codegen_state &gs, std::string_view p, size_t line,
    int rettype = BC_RET_NULL, int brak = '\0'
);

void codegen_state::gen_int(std::string_view word) {
    gen_int(parse_int(word));
}

void codegen_state::gen_float(std::string_view word) {
    gen_float(parse_float(word));
}

void codegen_state::gen_value(int wordtype, std::string_view word, int line) {
    switch (wordtype) {
        case VAL_ANY:
            if (!word.empty()) {
                gen_str(word);
            } else {
                gen_null();
            }
            break;
        case VAL_STRING:
            gen_str(word);
            break;
        case VAL_FLOAT:
            gen_float(word);
            break;
        case VAL_INT:
            gen_int(word);
            break;
        case VAL_COND:
            if (!word.empty()) {
                compileblock(*this, word, line);
            } else {
                gen_null();
            }
            break;
        case VAL_CODE:
            compileblock(*this, word, line);
            break;
        case VAL_IDENT:
            gen_ident(word);
            break;
        default:
            break;
    }
}

static inline void compileblock(codegen_state &gs) {
    gs.code.push_back(BC_INST_EMPTY);
}

static inline std::pair<std::string_view, size_t> compileblock(
    codegen_state &gs, std::string_view p, size_t line, int rettype, int brak
) {
    size_t start = gs.code.size();
    gs.code.push_back(BC_INST_BLOCK);
    gs.code.push_back(BC_INST_OFFSET | std::uint32_t((start + 2) << 8));
    size_t retline = line;
    if (!p.empty()) {
        char const *op = gs.source, *oe = gs.send;
        size_t oldline = gs.current_line;
        gs.source = p.data();
        gs.send = p.data() + p.size();
        gs.current_line = line;
        compilestatements(gs, VAL_ANY, brak);
        p = std::string_view{gs.source, std::size_t(gs.send - gs.source)};
        retline = gs.current_line;
        gs.source = op;
        gs.send = oe;
        gs.current_line = oldline;
    }
    if (gs.code.size() > start + 2) {
        gs.code.push_back(BC_INST_EXIT | rettype);
        gs.code[start] |= uint32_t(gs.code.size() - (start + 1)) << 8;
    } else {
        gs.code.resize(start);
        gs.code.push_back(BC_INST_EMPTY | rettype);
    }
    return std::make_pair(p, retline);
}

static inline void compileunescapestr(codegen_state &gs) {
    auto str = gs.get_str();
    gs.code.push_back(BC_INST_VAL | BC_RET_STRING);
    gs.code.reserve(
        gs.code.size() + str.size() / sizeof(uint32_t) + 1
    );
    size_t bufs = (gs.code.capacity() - gs.code.size()) * sizeof(uint32_t);
    auto alloc = std_allocator<char>{gs.ts.istate};
    auto *buf = alloc.allocate(bufs + 1);
    char *wbuf = unescape_string(&buf[0], str);
    memset(
        &buf[wbuf - buf], 0,
        sizeof(uint32_t) - (wbuf - buf) % sizeof(uint32_t)
    );
    gs.code.back() |= (wbuf - buf) << 8;
    uint32_t *ubuf = reinterpret_cast<uint32_t *>(buf);
    gs.code.append(ubuf, ubuf + ((wbuf - buf) / sizeof(uint32_t) + 1));
    alloc.deallocate(buf, bufs + 1);
}

static bool compilearg(
    codegen_state &gs, int wordtype, charbuf *word = nullptr
);

static void compilelookup(codegen_state &gs, int ltype) {
    charbuf lookup{gs.ts};
    gs.next_char();
    switch (gs.current()) {
        case '(':
        case '[':
            if (!compilearg(gs, VAL_STRING)) {
                goto invalid;
            }
            break;
        case '$':
            compilelookup(gs, VAL_STRING);
            break;
        case '\"':
            lookup = gs.get_str_dup();
            lookup.push_back('\0');
            goto lookupid;
        default: {
            lookup.append(gs.get_word());
            if (lookup.empty()) goto invalid;
            lookup.push_back('\0');
lookupid:
            ident *id = gs.ts.istate->new_ident(
                *gs.ts.pstate, lookup.str_term(), IDENT_FLAG_UNKNOWN
            );
            if (id) {
                switch (id->get_type()) {
                    case ident_type::IVAR:
                        gs.code.push_back(
                            BC_INST_IVAR | ret_code(ltype, BC_RET_INT) |
                                (id->get_index() << 8)
                        );
                        switch (ltype) {
                            case VAL_POP:
                                gs.code.pop_back();
                                break;
                            case VAL_CODE:
                                gs.code.push_back(BC_INST_COMPILE);
                                break;
                            case VAL_IDENT:
                                gs.code.push_back(BC_INST_IDENT_U);
                                break;
                        }
                        return;
                    case ident_type::FVAR:
                        gs.code.push_back(
                            BC_INST_FVAR | ret_code(ltype, BC_RET_FLOAT) |
                                (id->get_index() << 8)
                        );
                        switch (ltype) {
                            case VAL_POP:
                                gs.code.pop_back();
                                break;
                            case VAL_CODE:
                                gs.code.push_back(BC_INST_COMPILE);
                                break;
                            case VAL_IDENT:
                                gs.code.push_back(BC_INST_IDENT_U);
                                break;
                        }
                        return;
                    case ident_type::SVAR:
                        switch (ltype) {
                            case VAL_POP:
                                return;
                            default:
                                gs.code.push_back(
                                    BC_INST_SVAR | ret_code(ltype, BC_RET_STRING) |
                                        (id->get_index() << 8)
                                );
                                break;
                        }
                        goto done;
                    case ident_type::ALIAS:
                        switch (ltype) {
                            case VAL_POP:
                                return;
                            case VAL_COND:
                                gs.code.push_back(
                                    BC_INST_LOOKUP | (id->get_index() << 8)
                                );
                                break;
                            default:
                                gs.code.push_back(
                                    BC_INST_LOOKUP |
                                    ret_code(ltype, BC_RET_STRING) |
                                    (id->get_index() << 8)
                                );
                                break;
                        }
                        goto done;
                    case ident_type::COMMAND: {
                        std::uint32_t comtype = BC_INST_COM, numargs = 0;
                        auto fmt = static_cast<command_impl *>(id)->get_args();
                        for (char c: fmt) {
                            switch (c) {
                                case 's':
                                    gs.gen_str(std::string_view{});
                                    numargs++;
                                    break;
                                case 'i':
                                    gs.gen_int();
                                    numargs++;
                                    break;
                                case 'b':
                                    gs.gen_int(std::numeric_limits<integer_type>::min());
                                    numargs++;
                                    break;
                                case 'f':
                                    gs.gen_float();
                                    numargs++;
                                    break;
                                case 'F':
                                    gs.code.push_back(BC_INST_DUP | BC_RET_FLOAT);
                                    numargs++;
                                    break;
                                case 'E':
                                case 't':
                                    gs.gen_null();
                                    numargs++;
                                    break;
                                case 'e':
                                    compileblock(gs);
                                    numargs++;
                                    break;
                                case 'r':
                                    gs.gen_ident();
                                    numargs++;
                                    break;
                                case '$':
                                    gs.gen_ident(id);
                                    numargs++;
                                    break;
                                case 'N':
                                    gs.gen_int(-1);
                                    numargs++;
                                    break;
                                case 'C':
                                    comtype = BC_INST_COM_C;
                                    goto compilecomv;
                                case 'V':
                                    comtype = BC_INST_COM_V;
                                    goto compilecomv;
                                case '1':
                                case '2':
                                case '3':
                                case '4':
                                    break;
                            }
                        }
                        gs.code.push_back(
                            comtype | ret_code(ltype) | (id->get_index() << 8)
                        );
                        gs.code.push_back(
                            BC_INST_RESULT_ARG | ret_code(ltype)
                        );
                        goto done;
        compilecomv:
                        gs.code.push_back(
                            comtype | ret_code(ltype) | (id->get_index() << 8)
                        );
                        gs.code.push_back(numargs);
                        gs.code.push_back(
                            BC_INST_RESULT_ARG | ret_code(ltype)
                        );
                        goto done;
                    }
                    default:
                        goto invalid;
                }
            }
            gs.gen_str(lookup.str_term());
            break;
        }
    }
    switch (ltype) {
        case VAL_COND:
            gs.code.push_back(BC_INST_LOOKUP_U);
            break;
        default:
            gs.code.push_back(BC_INST_LOOKUP_U | ret_code(ltype));
            break;
    }
done:
    switch (ltype) {
        case VAL_POP:
            gs.code.push_back(BC_INST_POP);
            break;
        case VAL_CODE:
            gs.code.push_back(BC_INST_COMPILE);
            break;
        case VAL_COND:
            gs.code.push_back(BC_INST_COND);
            break;
        case VAL_IDENT:
            gs.code.push_back(BC_INST_IDENT_U);
            break;
    }
    return;
invalid:
    switch (ltype) {
        case VAL_POP:
            break;
        case VAL_NULL:
        case VAL_ANY:
        case VAL_WORD:
        case VAL_COND:
            gs.gen_null();
            break;
        default:
            gs.gen_value(ltype);
            break;
    }
}

static bool compileblockstr(codegen_state &gs, char const *str, char const *send) {
    std::size_t startc = gs.code.size();
    gs.code.push_back(BC_INST_VAL | BC_RET_STRING);
    gs.code.reserve(gs.code.size() + (send - str) / sizeof(uint32_t) + 1);
    auto alloc = std_allocator<char>{gs.ts.istate};
    auto asz = ((send - str) / sizeof(uint32_t) + 1) * sizeof(uint32_t);
    char *buf = alloc.allocate(asz);
    std::size_t len = 0;
    while (str < send) {
        std::string_view chrs{"\r/\"@]"};
        char const *orig = str;
        str = std::find_first_of(str, send, chrs.begin(), chrs.end());
        memcpy(&buf[len], orig, str - orig);
        len += (str - orig);
        if (str == send) {
            goto done;
        }
        switch (*str) {
            case '\r':
                ++str;
                break;
            case '\"': {
                char const *start = str;
                str = parse_string(
                    *gs.ts.pstate, std::string_view{str, send}
                );
                memcpy(&buf[len], start, std::size_t(str - start));
                len += (str - start);
                break;
            }
            case '/':
                if (((str + 1) != send) && str[1] == '/') {
                    str = std::find(str, send, '\n');
                } else {
                    buf[len++] = *str++;
                }
                break;
            case '@':
            case ']':
                if (str < send) {
                    buf[len++] = *str++;
                } else {
                    goto done;
                }
                break;
        }
    }
done:
    memset(&buf[len], '\0', sizeof(uint32_t) - len % sizeof(uint32_t));
    uint32_t *ubuf = reinterpret_cast<uint32_t *>(buf);
    gs.code.append(ubuf, ubuf + (len / sizeof(uint32_t) + 1));
    gs.code[startc] |= len << 8;
    alloc.deallocate(buf, asz);
    return true;
}

static bool compileblocksub(codegen_state &gs) {
    charbuf lookup{gs.ts};
    switch (gs.current()) {
        case '(':
            if (!compilearg(gs, VAL_ANY)) {
                return false;
            }
            break;
        case '[':
            if (!compilearg(gs, VAL_STRING)) {
                return false;
            }
            gs.code.push_back(BC_INST_LOOKUP_U);
            break;
        case '\"':
            lookup = gs.get_str_dup();
            lookup.push_back('\0');
            goto lookupid;
        default: {
            lookup.append(gs.read_macro_name());
            if (lookup.empty()) {
                return false;
            }
            lookup.push_back('\0');
lookupid:
            ident *id = gs.ts.istate->new_ident(
                *gs.ts.pstate, lookup.str_term(), IDENT_FLAG_UNKNOWN
            );
            if (id) {
                switch (id->get_type()) {
                    case ident_type::IVAR:
                        gs.code.push_back(BC_INST_IVAR | (id->get_index() << 8));
                        goto done;
                    case ident_type::FVAR:
                        gs.code.push_back(BC_INST_FVAR | (id->get_index() << 8));
                        goto done;
                    case ident_type::SVAR:
                        gs.code.push_back(BC_INST_SVAR | (id->get_index() << 8));
                        goto done;
                    case ident_type::ALIAS:
                        gs.code.push_back(
                            BC_INST_LOOKUP | (id->get_index() << 8)
                        );
                        goto done;
                    default:
                        break;
                }
            }
            gs.gen_str(lookup.str_term());
            gs.code.push_back(BC_INST_LOOKUP_U);
done:
            break;
        }
    }
    return true;
}

static void compileblockmain(codegen_state &gs, int wordtype) {
    char const *start = gs.source;
    size_t curline = gs.current_line;
    int concs = 0;
    for (int brak = 1; brak;) {
        switch (gs.skip_until("@\"/[]")) {
            case '\0':
                throw error{*gs.ts.pstate, "missing \"]\""};
                return;
            case '\"':
                gs.get_str();
                break;
            case '/':
                gs.next_char();
                if (gs.current() == '/') {
                    gs.skip_until('\n');
                }
                break;
            case '[':
                gs.next_char();
                brak++;
                break;
            case ']':
                gs.next_char();
                brak--;
                break;
            case '@': {
                char const *esc = gs.source;
                int level = 0;
                while (gs.current() == '@') {
                    ++level;
                    gs.next_char();
                }
                if (brak > level) {
                    continue;
                } else if (brak < level) {
                    throw error{*gs.ts.pstate, "too many @s"};
                    return;
                }
                if (compileblockstr(gs, start, esc)) {
                    concs++;
                }
                if (compileblocksub(gs)) {
                    concs++;
                }
                if (concs) {
                    start = gs.source;
                    curline = gs.current_line;
                }
                break;
            }
            default:
                gs.next_char();
                break;
        }
    }
    if (gs.source - 1 > start) {
        if (!concs) {
            switch (wordtype) {
                case VAL_POP:
                    return;
                case VAL_CODE:
                case VAL_COND: {
                    auto ret = compileblock(gs, std::string_view{
                        start, std::size_t(gs.send - start)
                    }, curline, BC_RET_NULL, ']');
                    gs.source = ret.first.data();
                    gs.send = ret.first.data() + ret.first.size();
                    gs.current_line = ret.second;
                    return;
                }
                case VAL_IDENT:
                    gs.gen_ident(std::string_view{
                        start, std::size_t((gs.source - 1) - start)
                    });
                    return;
            }
        }
        compileblockstr(gs, start, gs.source - 1);
        if (concs > 1) {
            concs++;
        }
    }
    if (concs) {
        gs.code.push_back(BC_INST_CONC_W | ret_code(wordtype) | (concs << 8));
    }
    switch (wordtype) {
        case VAL_POP:
            if (concs || gs.source - 1 > start) {
                gs.code.push_back(BC_INST_POP);
            }
            break;
        case VAL_COND:
            if (!concs && gs.source - 1 <= start) {
                gs.gen_null();
            } else {
                gs.code.push_back(BC_INST_COND);
            }
            break;
        case VAL_CODE:
            if (!concs && gs.source - 1 <= start) {
                compileblock(gs);
            } else {
                gs.code.push_back(BC_INST_COMPILE);
            }
            break;
        case VAL_IDENT:
            if (!concs && gs.source - 1 <= start) {
                gs.gen_ident();
            } else {
                gs.code.push_back(BC_INST_IDENT_U);
            }
            break;
        case VAL_STRING:
        case VAL_NULL:
        case VAL_ANY:
        case VAL_WORD:
            if (!concs && gs.source - 1 <= start) {
                gs.gen_str();
            }
            break;
        default:
            if (!concs) {
                if (gs.source - 1 <= start) {
                    gs.gen_value(wordtype);
                } else {
                    gs.code.push_back(BC_INST_FORCE | (wordtype << BC_INST_RET));
                }
            }
            break;
    }
}

static bool compilearg(
    codegen_state &gs, int wordtype, charbuf *word
) {
    gs.skip_comments();
    switch (gs.current()) {
        case '\"':
            switch (wordtype) {
                case VAL_POP:
                    gs.get_str();
                    break;
                case VAL_COND: {
                    size_t line = gs.current_line;
                    auto s = gs.get_str_dup();
                    if (!s.empty()) {
                        s.push_back('\0');
                        compileblock(gs, s.str_term(), line);
                    } else {
                        gs.gen_null();
                    }
                    break;
                }
                case VAL_CODE: {
                    auto s = gs.get_str_dup();
                    s.push_back('\0');
                    compileblock(gs, s.str_term(), gs.current_line);
                    break;
                }
                case VAL_WORD:
                    if (word) {
                        *word = gs.get_str_dup();
                    }
                    break;
                case VAL_ANY:
                case VAL_STRING:
                    compileunescapestr(gs);
                    break;
                default: {
                    int line = int(gs.current_line);
                    auto s = gs.get_str_dup();
                    s.push_back('\0');
                    gs.gen_value(wordtype, s.str_term(), line);
                    break;
                }
            }
            return true;
        case '$':
            compilelookup(gs, wordtype);
            return true;
        case '(': {
            gs.next_char();
            std::size_t start = gs.code.size();
            compilestatements(gs, VAL_ANY, ')');
            if (gs.code.size() > start) {
                gs.code.push_back(BC_INST_RESULT_ARG | ret_code(wordtype));
            } else {
                gs.gen_value(wordtype);
                return true;
            }
            switch (wordtype) {
                case VAL_POP:
                    gs.code.push_back(BC_INST_POP);
                    break;
                case VAL_COND:
                    gs.code.push_back(BC_INST_COND);
                    break;
                case VAL_CODE:
                    gs.code.push_back(BC_INST_COMPILE);
                    break;
                case VAL_IDENT:
                    gs.code.push_back(BC_INST_IDENT_U);
                    break;
            }
            return true;
        }
        case '[':
            gs.next_char();
            compileblockmain(gs, wordtype);
            return true;
        default:
            switch (wordtype) {
                case VAL_POP: {
                    return !gs.get_word().empty();
                }
                case VAL_COND: {
                    size_t line = gs.current_line;
                    auto s = gs.get_word();
                    if (s.empty()) {
                        return false;
                    }
                    compileblock(gs, s, line);
                    return true;
                }
                case VAL_CODE: {
                    size_t line = gs.current_line;
                    auto s = gs.get_word();
                    if (s.empty()) {
                        return false;
                    }
                    compileblock(gs, s, line);
                    return true;
                }
                case VAL_WORD: {
                    auto w = gs.get_word();
                    if (word) {
                        word->clear();
                        word->append(w);
                    }
                    return !w.empty();
                }
                default: {
                    int line = int(gs.current_line);
                    auto s = gs.get_word();
                    if (s.empty()) {
                        return false;
                    }
                    gs.gen_value(wordtype, s, line);
                    return true;
                }
            }
    }
}

static void compile_cmd(
    codegen_state &gs, command_impl *id, ident *self, bool &more, int rettype,
    std::uint32_t limit = 0
) {
    std::uint32_t comtype = BC_INST_COM, numargs = 0, numcargs = 0, fakeargs = 0;
    bool rep = false;
    auto fmt = id->get_args();
    for (auto it = fmt.begin(); it != fmt.end(); ++it) {
        switch (*it) {
            case 's': /* string */
                if (more && (!limit || (numcargs < limit))) {
                    more = compilearg(gs, VAL_STRING);
                }
                if (!more || (limit && (numcargs >= limit))) {
                    if (rep) {
                        break;
                    }
                    gs.gen_str(std::string_view{});
                    fakeargs++;
                } else if ((it + 1) == fmt.end()) {
                    int numconc = 1;
                    for (;;) {
                        more = compilearg(gs, VAL_STRING);
                        if (!more) {
                            break;
                        }
                        numconc++;
                    }
                    if (numconc > 1) {
                        gs.code.push_back(BC_INST_CONC | BC_RET_STRING | (numconc << 8));
                    }
                }
                numargs++;
                numcargs++;
                break;
            case 'i': /* integer */
                if (more && (!limit || (numcargs < limit))) {
                    more = compilearg(gs, VAL_INT);
                }
                if (!more || (limit && (numcargs >= limit))) {
                    if (rep) {
                        break;
                    }
                    gs.gen_int();
                    fakeargs++;
                }
                numargs++;
                numcargs++;
                break;
            case 'b': /* integer, INT_MIN default */
                if (more && (!limit || (numcargs < limit))) {
                    more = compilearg(gs, VAL_INT);
                }
                if (!more || (limit && (numcargs >= limit))) {
                    if (rep) {
                        break;
                    }
                    gs.gen_int(std::numeric_limits<integer_type>::min());
                    fakeargs++;
                }
                numargs++;
                numcargs++;
                break;
            case 'f': /* float */
                if (more && (!limit || (numcargs < limit))) {
                    more = compilearg(gs, VAL_FLOAT);
                }
                if (!more || (limit && (numcargs >= limit))) {
                    if (rep) {
                        break;
                    }
                    gs.gen_float();
                    fakeargs++;
                }
                numargs++;
                numcargs++;
                break;
            case 'F': /* float, prev-argument default */
                if (more && (!limit || (numcargs < limit))) {
                    more = compilearg(gs, VAL_FLOAT);
                }
                if (!more || (limit && (numcargs >= limit))) {
                    if (rep) {
                        break;
                    }
                    gs.code.push_back(BC_INST_DUP | BC_RET_FLOAT);
                    fakeargs++;
                }
                numargs++;
                numcargs++;
                break;
            case 't': /* any arg */
                if (more && (!limit || (numcargs < limit))) {
                    more = compilearg(gs, VAL_ANY);
                }
                if (!more || (limit && (numcargs >= limit))) {
                    if (rep) {
                        break;
                    }
                    gs.gen_null();
                    fakeargs++;
                }
                numargs++;
                numcargs++;
                break;
            case 'E': /* condition */
                if (more && (!limit || (numcargs < limit))) {
                    more = compilearg(gs, VAL_COND);
                }
                if (!more || (limit && (numcargs >= limit))) {
                    if (rep) {
                        break;
                    }
                    gs.gen_null();
                    fakeargs++;
                }
                numargs++;
                numcargs++;
                break;
            case 'e': /* code */
                if (more && (!limit || (numcargs < limit))) {
                    more = compilearg(gs, VAL_CODE);
                }
                if (!more || (limit && (numcargs >= limit))) {
                    if (rep) {
                        break;
                    }
                    compileblock(gs);
                    fakeargs++;
                }
                numargs++;
                numcargs++;
                break;
            case 'r': /* ident */
                if (more && (!limit || (numcargs < limit))) {
                    more = compilearg(gs, VAL_IDENT);
                }
                if (!more || (limit && (numcargs >= limit))) {
                    if (rep) {
                        break;
                    }
                    gs.gen_ident();
                    fakeargs++;
                }
                numargs++;
                numcargs++;
                break;
            case '$': /* self */
                gs.gen_ident(self);
                numargs++;
                break;
            case 'N': /* number of arguments */
                gs.gen_int(numargs - fakeargs);
                numargs++;
                break;
            case 'C': /* concatenated string */
                comtype = BC_INST_COM_C;
                if (more && (!limit || (numcargs < limit))) {
                    for (;;) {
                        more = compilearg(gs, VAL_ANY);
                        if (!more || (limit && (numcargs >= limit))) {
                            break;
                        }
                        numargs++;
                        numcargs++;
                    }
                }
                goto compilecomv;
            case 'V': /* varargs */
                comtype = BC_INST_COM_V;
                if (more && (!limit || (numcargs < limit))) {
                    for(;;) {
                        more = compilearg(gs, VAL_ANY);
                        if (!more || (limit && (numcargs >= limit))) {
                            break;
                        }
                        numargs++;
                        numcargs++;
                    }
                }
                goto compilecomv;
            case '1': /* vararg repetition */
            case '2':
            case '3':
            case '4':
                if (more && (!limit || (numcargs < limit))) {
                    int numrep = *it - '0' + 1;
                    it -= numrep;
                    rep = true;
                }
                break;
        }
    }
    gs.code.push_back(comtype | ret_code(rettype) | (id->get_index() << 8));
    return;
compilecomv:
    gs.code.push_back(
        comtype | ret_code(rettype) | (id->get_index() << 8)
    );
    gs.code.push_back(numargs);
}

static void compile_alias(codegen_state &gs, alias *id, bool &more) {
    std::uint32_t numargs = 0;
    for (;;) {
        more = compilearg(gs, VAL_ANY);
        if (!more) {
            break;
        }
        ++numargs;
    }
    gs.code.push_back(
        BC_INST_CALL | (id->get_index() << 8)
    );
    gs.code.push_back(numargs);
}

static void compile_local(codegen_state &gs, bool &more) {
    std::uint32_t numargs = 0;
    if (more) {
        for (;;) {
            more = compilearg(gs, VAL_IDENT);
            if (!more) {
                break;
            }
            numargs++;
        }
    }
    gs.code.push_back(BC_INST_LOCAL | (numargs << 8));
}

static void compile_do(
    codegen_state &gs, bool &more, int rettype, int opcode
) {
    if (more) {
        more = compilearg(gs, VAL_CODE);
    }
    gs.code.push_back((more ? opcode : BC_INST_NULL) | ret_code(rettype));
}

static void compile_if(
    codegen_state &gs, ident *id, bool &more, int rettype
) {
    if (more) {
        more = compilearg(gs, VAL_ANY);
    }
    if (!more) {
        gs.code.push_back(BC_INST_NULL | ret_code(rettype));
    } else {
        std::size_t start1 = gs.code.size();
        more = compilearg(gs, VAL_CODE);
        if (!more) {
            gs.code.push_back(BC_INST_POP);
            gs.code.push_back(BC_INST_NULL | ret_code(rettype));
        } else {
            std::size_t start2 = gs.code.size();
            more = compilearg(gs, VAL_CODE);
            std::uint32_t inst1 = gs.code[start1];
            std::uint32_t op1 = inst1 & ~BC_INST_RET_MASK;
            auto len1 = std::uint32_t(start2 - (start1 + 1));
            if (!more) {
                if (op1 == (BC_INST_BLOCK | (len1 << 8))) {
                    gs.code[start1] = (len1 << 8) | BC_INST_JUMP_B | BC_INST_FLAG_FALSE;
                    gs.code[start1 + 1] = BC_INST_ENTER_RESULT;
                    gs.code[start1 + len1] = (
                        gs.code[start1 + len1] & ~BC_INST_RET_MASK
                    ) | ret_code(rettype);
                    return;
                }
                compileblock(gs);
            } else {
                std::uint32_t inst2 = gs.code[start2];
                std::uint32_t op2 = inst2 & ~BC_INST_RET_MASK;
                auto len2 = std::uint32_t(gs.code.size() - (start2 + 1));
                if (op2 == (BC_INST_BLOCK | (len2 << 8))) {
                    if (op1 == (BC_INST_BLOCK | (len1 << 8))) {
                        gs.code[start1] = (std::uint32_t(start2 - start1) << 8)
                            | BC_INST_JUMP_B | BC_INST_FLAG_FALSE;
                        gs.code[start1 + 1] = BC_INST_ENTER_RESULT;
                        gs.code[start1 + len1] = (
                            gs.code[start1 + len1] & ~BC_INST_RET_MASK
                        ) | ret_code(rettype);
                        gs.code[start2] = (len2 << 8) | BC_INST_JUMP;
                        gs.code[start2 + 1] = BC_INST_ENTER_RESULT;
                        gs.code[start2 + len2] = (
                            gs.code[start2 + len2] & ~BC_INST_RET_MASK
                        ) | ret_code(rettype);
                        return;
                    } else if (op1 == (BC_INST_EMPTY | (len1 << 8))) {
                        gs.code[start1] = BC_INST_NULL | (inst2 & BC_INST_RET_MASK);
                        gs.code[start2] = (len2 << 8) | BC_INST_JUMP_B | BC_INST_FLAG_TRUE;
                        gs.code[start2 + 1] = BC_INST_ENTER_RESULT;
                        gs.code[start2 + len2] = (
                            gs.code[start2 + len2] & ~BC_INST_RET_MASK
                        ) | ret_code(rettype);
                        return;
                    }
                }
            }
            gs.code.push_back(BC_INST_COM | ret_code(rettype) | (id->get_index() << 8));
        }
    }
}

static void compile_and_or(
    codegen_state &gs, ident *id, bool &more, int rettype
) {
    std::uint32_t numargs = 0;
    if (more) {
        more = compilearg(gs, VAL_COND);
    }
    if (!more) {
        gs.code.push_back(
            ((id->get_raw_type() == ID_AND)
                ? BC_INST_TRUE : BC_INST_FALSE) | ret_code(rettype)
        );
    } else {
        numargs++;
        std::size_t start = gs.code.size(), end = start;
        for (;;) {
            more = compilearg(gs, VAL_COND);
            if (!more) {
                break;
            }
            numargs++;
            if ((gs.code[end] & ~BC_INST_RET_MASK) != (
                BC_INST_BLOCK | (uint32_t(gs.code.size() - (end + 1)) << 8)
            )) {
                break;
            }
            end = gs.code.size();
        }
        if (more) {
            for (;;) {
                more = compilearg(gs, VAL_COND);
                if (!more) {
                    break;
                }
                numargs++;
            }
            gs.code.push_back(
                BC_INST_COM_V | ret_code(rettype) | (id->get_index() << 8)
            );
            gs.code.push_back(numargs);
        } else {
            std::uint32_t op = (id->get_raw_type() == ID_AND)
                ? (BC_INST_JUMP_RESULT | BC_INST_FLAG_FALSE)
                : (BC_INST_JUMP_RESULT | BC_INST_FLAG_TRUE);
            gs.code.push_back(op);
            end = gs.code.size();
            while ((start + 1) < end) {
                uint32_t len = gs.code[start] >> 8;
                gs.code[start] = std::uint32_t((end - (start + 1)) << 8) | op;
                gs.code[start + 1] = BC_INST_ENTER;
                gs.code[start + len] = (
                    gs.code[start + len] & ~BC_INST_RET_MASK
                ) | ret_code(rettype);
                start += len + 1;
            }
        }
    }
}

static void compilestatements(codegen_state &gs, int rettype, int brak) {
    charbuf idname{gs.ts};
    for (;;) {
        gs.skip_comments();
        idname.clear();
        size_t curline = gs.current_line;
        bool more = compilearg(gs, VAL_WORD, &idname);
        if (!more) {
            goto endstatement;
        }
        gs.skip_comments();
        if (gs.current() == '=') {
            switch (gs.current(1)) {
                case '/':
                    if (gs.current(2) != '/') {
                        break;
                    }
                    [[fallthrough]];
                case ';':
                case ' ':
                case '\t':
                case '\r':
                case '\n':
                case '\0':
                    gs.next_char();
                    if (!idname.empty()) {
                        idname.push_back('\0');
                        ident *id = gs.ts.istate->new_ident(
                            *gs.ts.pstate, idname.str_term(), IDENT_FLAG_UNKNOWN
                        );
                        if (id) {
                            switch (id->get_type()) {
                                case ident_type::ALIAS:
                                    more = compilearg(gs, VAL_ANY);
                                    if (!more) {
                                        gs.gen_str();
                                    }
                                    gs.code.push_back(
                                        BC_INST_ALIAS | (id->get_index() << 8)
                                    );
                                    goto endstatement;
                                case ident_type::IVAR: {
                                    auto *hid = gs.ts.istate->cmd_ivar;
                                    compile_cmd(
                                        gs, static_cast<command_impl *>(hid),
                                        id, more, rettype, 1
                                    );
                                    goto endstatement;
                                }
                                case ident_type::FVAR: {
                                    auto *hid = gs.ts.istate->cmd_fvar;
                                    compile_cmd(
                                        gs, static_cast<command_impl *>(hid),
                                        id, more, rettype, 1
                                    );
                                    goto endstatement;
                                }
                                case ident_type::SVAR: {
                                    auto *hid = gs.ts.istate->cmd_svar;
                                    compile_cmd(
                                        gs, static_cast<command_impl *>(hid),
                                        id, more, rettype, 1
                                    );
                                    goto endstatement;
                                }
                                default:
                                    break;
                            }
                        }
                        gs.gen_str(idname.str_term());
                    }
                    more = compilearg(gs, VAL_ANY);
                    if (!more) {
                        gs.gen_str();
                    }
                    gs.code.push_back(BC_INST_ALIAS_U);
                    goto endstatement;
            }
        }
        if (idname.empty()) {
noid:
            std::uint32_t numargs = 0;
            for (;;) {
                more = compilearg(gs, VAL_ANY);
                if (!more) {
                    break;
                }
                ++numargs;
            }
            gs.code.push_back(BC_INST_CALL_U | (numargs << 8));
        } else {
            idname.push_back('\0');
            ident *id = gs.ts.pstate->get_ident(idname.str_term());
            if (!id) {
                if (is_valid_name(idname.str_term())) {
                    gs.gen_str(idname.str_term());
                    goto noid;
                }
                switch (rettype) {
                    case VAL_ANY: {
                        std::string_view end = idname.str_term();
                        integer_type val = parse_int(end, &end);
                        if (!end.empty()) {
                            gs.gen_str(idname.str_term());
                        } else {
                            gs.gen_int(val);
                        }
                        break;
                    }
                    default:
                        gs.gen_value(rettype, idname.str_term(), int(curline));
                        break;
                }
                gs.code.push_back(BC_INST_RESULT);
            } else {
                switch (id->get_raw_type()) {
                    case ID_ALIAS:
                        compile_alias(
                            gs, static_cast<alias *>(id), more
                        );
                        break;
                    case ID_COMMAND:
                        compile_cmd(
                            gs, static_cast<command_impl *>(id), id, more,
                            rettype
                        );
                        break;
                    case ID_LOCAL:
                        compile_local(gs, more);
                        break;
                    case ID_DO:
                        compile_do(gs, more, rettype, BC_INST_DO);
                        break;
                    case ID_DOARGS:
                        compile_do(gs, more, rettype, BC_INST_DO_ARGS);
                        break;
                    case ID_IF:
                        compile_if(gs, id, more, rettype);
                        break;
                    case ID_BREAK:
                        gs.code.push_back(BC_INST_BREAK | BC_INST_FLAG_FALSE);
                        break;
                    case ID_CONTINUE:
                        gs.code.push_back(BC_INST_BREAK | BC_INST_FLAG_TRUE);
                        break;
                    case ID_RESULT:
                        if (more) {
                            more = compilearg(gs, VAL_ANY);
                        }
                        gs.code.push_back(
                            (more ? BC_INST_RESULT : BC_INST_NULL) |
                                ret_code(rettype)
                        );
                        break;
                    case ID_NOT:
                        if (more) {
                            more = compilearg(gs, VAL_ANY);
                        }
                        gs.code.push_back(
                            (more ? BC_INST_NOT : BC_INST_TRUE) | ret_code(rettype)
                        );
                        break;
                    case ID_AND:
                    case ID_OR:
                        compile_and_or(gs, id, more, rettype);
                        break;
                    case ID_IVAR: {
                        auto *hid = gs.ts.istate->cmd_ivar;
                        compile_cmd(
                            gs, static_cast<command_impl *>(hid),
                            id, more, rettype
                        );
                        break;
                    }
                    case ID_FVAR: {
                        auto *hid = gs.ts.istate->cmd_fvar;
                        compile_cmd(
                            gs, static_cast<command_impl *>(hid),
                            id, more, rettype
                        );
                        break;
                    }
                    case ID_SVAR: {
                        auto *hid = gs.ts.istate->cmd_svar;
                        compile_cmd(
                            gs, static_cast<command_impl *>(hid),
                            id, more, rettype
                        );
                        break;
                    }
                }
            }
        }
endstatement:
        if (more) {
            while (compilearg(gs, VAL_POP));
        }
        switch (gs.skip_until(")];/\n")) {
            case '\0':
                if (gs.current() != brak) {
                    throw error{*gs.ts.pstate, "missing \"%c\"", char(brak)};
                    return;
                }
                return;
            case ')':
            case ']':
                if (gs.current() == brak) {
                    gs.next_char();
                    return;
                }
                throw error{*gs.ts.pstate, "unexpected \"%c\"", gs.current()};
                return;
            case '/':
                gs.next_char();
                if (gs.current() == '/') {
                    gs.skip_until('\n');
                }
                goto endstatement;
            default:
                gs.next_char();
                break;
        }
    }
}

void codegen_state::gen_main(std::string_view s, int ret_type) {
    source = s.data();
    send = s.data() + s.size();
    code.push_back(BC_INST_START);
    compilestatements(*this, VAL_ANY);
    code.push_back(BC_INST_EXIT | ((ret_type < VAL_ANY) ? (ret_type << BC_INST_RET) : 0));
}

} /* namespace cubescript */