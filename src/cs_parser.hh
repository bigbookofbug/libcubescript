#ifndef LIBCUBESCRIPT_PARSER_HH
#define LIBCUBESCRIPT_PARSER_HH

#include <cstdlib>
#include <string_view>
#include <type_traits>

#include <cubescript/cubescript.hh>

#include "cs_std.hh"
#include "cs_bcode.hh"
#include "cs_ident.hh"
#include "cs_thread.hh"
#include "cs_gen.hh"

namespace cubescript {

integer_type parse_int(std::string_view input, std::string_view *end = nullptr);
float_type parse_float(std::string_view input, std::string_view *end = nullptr);

bool is_valid_name(std::string_view input);

struct parser_state {
    thread_state &ts;
    gen_state &gs;
    parser_state *prevps;
    bool parsing = true;
    char const *source, *send;
    std::size_t current_line;
    std::string_view src_name;

    parser_state() = delete;
    parser_state(thread_state &tsr, gen_state &gsr):
        ts{tsr}, gs{gsr}, prevps{tsr.cstate},
        source{}, send{}, current_line{1}, src_name{}
    {
        tsr.cstate = this;
    }

    ~parser_state() {
        done();
    }

    void done() {
        if (!parsing) {
            return;
        }
        ts.cstate = prevps;
        parsing = false;
    }

    std::string_view get_str();
    charbuf get_str_dup();

    std::string_view get_word();

    void parse_block(int ret_type, int term = '\0');
    void gen_main(std::string_view s, int ret_type = VAL_ANY);

    void next_char() {
        if (source == send) {
            return;
        }
        if (*source == '\n') {
            ++current_line;
        }
        ++source;
    }

    char current(size_t ahead = 0) {
        if (std::size_t(send - source) <= ahead) {
            return '\0';
        }
        return source[ahead];
    }

    std::string_view read_macro_name();

    char skip_until(std::string_view chars);
    char skip_until(char cf);

    void skip_comments();
};

} /* namespace cubescript */

#endif
