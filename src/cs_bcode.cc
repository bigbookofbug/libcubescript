#include "cs_bcode.hh"

#include "cs_util.hh"

namespace cscript {

/* public API impls */

LIBCUBESCRIPT_EXPORT cs_bcode_ref::cs_bcode_ref(cs_bcode *v): p_code(v) {
    bcode_ref(v->get_raw());
}
LIBCUBESCRIPT_EXPORT cs_bcode_ref::cs_bcode_ref(cs_bcode_ref const &v):
    p_code(v.p_code)
{
    bcode_ref(p_code->get_raw());
}

LIBCUBESCRIPT_EXPORT cs_bcode_ref::~cs_bcode_ref() {
    bcode_unref(p_code->get_raw());
}

LIBCUBESCRIPT_EXPORT cs_bcode_ref &cs_bcode_ref::operator=(
    cs_bcode_ref const &v
) {
    bcode_unref(p_code->get_raw());
    p_code = v.p_code;
    bcode_ref(p_code->get_raw());
    return *this;
}

LIBCUBESCRIPT_EXPORT cs_bcode_ref &cs_bcode_ref::operator=(cs_bcode_ref &&v) {
    bcode_unref(p_code->get_raw());
    p_code = v.p_code;
    v.p_code = nullptr;
    return *this;
}

/* private funcs */

struct bcode_hdr {
    cs_shared_state *cs; /* needed to construct the allocator */
    std::size_t asize; /* alloc size of the bytecode block */
    cs_bcode bc; /* CS_CODE_START + refcount */
};

/* returned address is the 'init' member of the header */
std::uint32_t *bcode_alloc(cs_state &cs, std::size_t sz) {
    auto a = cs_allocator<std::uint32_t>{cs};
    std::size_t hdrs = sizeof(bcode_hdr) / sizeof(std::uint32_t);
    auto p = a.allocate(sz + hdrs - 1);
    bcode_hdr *hdr = reinterpret_cast<bcode_hdr *>(p);
    hdr->cs = cs_get_sstate(cs);
    hdr->asize = sz + hdrs - 1;
    return p + hdrs - 1;
}

/* bc's address must be the 'init' member of the header */
static inline void bcode_free(std::uint32_t *bc) {
    auto *rp = bc + 1 - (sizeof(bcode_hdr) / sizeof(std::uint32_t));
    bcode_hdr *hdr = reinterpret_cast<bcode_hdr *>(rp);
    cs_allocator<std::uint32_t>{hdr->cs}.deallocate(rp, hdr->asize);
}

void bcode_incr(std::uint32_t *bc) {
    *bc += 0x100;
}

void bcode_decr(std::uint32_t *bc) {
    *bc -= 0x100;
    if (std::int32_t(*bc) < 0x100) {
        bcode_free(bc);
    }
}

void bcode_ref(std::uint32_t *code) {
    if (!code) {
        return;
    }
    if ((*code & CS_CODE_OP_MASK) == CS_CODE_START) {
        bcode_incr(code);
        return;
    }
    switch (code[-1]&CS_CODE_OP_MASK) {
        case CS_CODE_START:
            bcode_incr(&code[-1]);
            break;
        case CS_CODE_OFFSET:
            code -= std::ptrdiff_t(code[-1] >> 8);
            bcode_incr(code);
            break;
    }
}

void bcode_unref(std::uint32_t *code) {
    if (!code) {
        return;
    }
    if ((*code & CS_CODE_OP_MASK) == CS_CODE_START) {
        bcode_decr(code);
        return;
    }
    switch (code[-1]&CS_CODE_OP_MASK) {
        case CS_CODE_START:
            bcode_decr(&code[-1]);
            break;
        case CS_CODE_OFFSET:
            code -= std::ptrdiff_t(code[-1] >> 8);
            bcode_decr(code);
            break;
    }
}

/* empty fallbacks */

static std::uint32_t emptyrets[CS_VAL_ANY] = {
    CS_RET_NULL, CS_RET_INT, CS_RET_FLOAT, CS_RET_STRING
};

empty_block *bcode_init_empty(cs_shared_state *cs) {
    auto a = cs_allocator<empty_block>{cs};
    auto *p = a.allocate(CS_VAL_ANY);
    for (std::size_t i = 0; i < CS_VAL_ANY; ++i) {
        p[i].init.init = CS_CODE_START + 0x100;
        p[i].code = CS_CODE_EXIT | emptyrets[i];
    }
    return p;
}

void bcode_free_empty(cs_shared_state *cs, empty_block *empty) {
    cs_allocator<empty_block>{cs}.deallocate(empty, CS_VAL_ANY);
};

cs_bcode *bcode_get_empty(empty_block *empty, std::size_t val) {
    return &empty[val].init + 1;
}

} /* namespace cscript */
