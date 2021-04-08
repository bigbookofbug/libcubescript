#ifndef LIBCUBESCRIPT_GEN_HH
#define LIBCUBESCRIPT_GEN_HH

#include <cstdint>
#include <string_view>
#include <utility>

#include <cubescript/cubescript.hh>

#include "cs_std.hh"
#include "cs_thread.hh"

namespace cubescript {

struct gen_state {
    thread_state &ts;
    valbuf<std::uint32_t> code;

    gen_state() = delete;
    gen_state(thread_state &tsr):
        ts{tsr}, code{tsr.istate}
    {}

    void gen_val_null();

    void gen_val_integer(integer_type v = 0);
    void gen_val_integer(std::string_view v);

    void gen_val_float(float_type v = 0);
    void gen_val_float(std::string_view v);

    void gen_val_string(std::string_view v = std::string_view{});

    void gen_val_ident();
    void gen_val_ident(ident &i);
    void gen_val_ident(std::string_view v);

    void gen_val(
        int val_type, std::string_view v = std::string_view{},
        std::size_t line = 0
    );

    void gen_block();
    std::pair<std::size_t, std::string_view> gen_block(
        std::string_view v, std::size_t line,
        int ret_type = BC_RET_NULL, int term = '\0'
    );
};

} /* namespace cubescript */

#endif /* LIBCUBESCRIPT_GEN_HH */
