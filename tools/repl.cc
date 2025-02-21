#ifdef _MSC_VER
/* avoid silly complaints about fopen */
#  define _CRT_SECURE_NO_WARNINGS 1
/* work around clang bug with std::function (needed by linenoise) */
#  if defined(__clang__) && !defined(_HAS_STATIC_RTTI)
#    define _HAS_STATIC_RTTI 0
#  endif
#endif

#include <signal.h>

#include <cmath>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <optional>
#include <memory>
#include <iterator>
#include <string>

#include <cubescript/cubescript.hh>

namespace cs = cubescript;

std::string_view version = "CubeScript 0.0.1";

/* util */

#if defined(_WIN32)
#include <io.h>
static bool stdin_is_tty() {
    return _isatty(_fileno(stdin));
}
#else
#include <unistd.h>
static bool stdin_is_tty() {
    return isatty(0);
}
#endif

/* line editing support */

inline std::string_view get_complete_cmd(std::string_view buf) {
    std::string_view not_allowed = "\"/;()[] \t\r\n\0";
    auto found = buf.find_first_of(not_allowed);
    while (found != buf.npos) {
        buf = buf.substr(found + 1, buf.size() - found - 1);
        found = buf.find_first_of(not_allowed);
    }
    return buf;
}

inline std::string_view get_arg_type(char arg) {
    switch (arg) {
        case 'i':
            return "int";
        case 'f':
            return "float";
        case 'a':
            return "any";
        case 'c':
            return "cond";
        case 'N':
            return "numargs";
        case 's':
            return "str";
        case 'b':
            return "block";
        case 'r':
            return "ident";
        case '$':
            return "self";
    }
    return "illegal";
}

inline void fill_cmd_args(std::string &writer, std::string_view args) {
    bool variadic = false;
    int nrep = 0;
    if ((args.size() >= 3) && (args.substr(args.size() - 3) == "...")) {
        variadic = true;
        args.remove_suffix(3);
        if (!args.empty() && isdigit(args.back())) {
            nrep = args.back() - '0';
            args.remove_suffix(1);
        }
    }
    if (args.empty()) {
        if (variadic) {
            writer += "...";
        }
        return;
    }
    int norep = int(args.size()) - nrep;
    if (norep > 0) {
        for (int i = 0; i < norep; ++i) {
            if (i != 0) {
                writer += ", ";
            }
            writer += get_arg_type(args.front());
            args.remove_prefix(1);
        }
    }
    if (variadic) {
        if (norep > 0) {
            writer += ", ";
        }
        if (!args.empty()) {
            if (args.size() > 1) {
                writer += '{';
            }
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (i) {
                    writer += ", ";
                }
                writer += get_arg_type(args[i]);
            }
            if (args.size() > 1) {
                writer += '}';
            }
        }
        writer += "...";
    }
}

inline cs::command *get_hint_cmd(cs::state &cs, std::string_view buf) {
    std::string_view nextchars = "([;";
    auto lp = buf.find_first_of(nextchars);
    if (lp != buf.npos) {
        cs::command *cmd = get_hint_cmd(cs, buf.substr(1, buf.size() - 1));
        if (cmd) {
            return cmd;
        }
    }
    std::size_t nsp = 0;
    for (auto c: buf) {
        if (!isspace(c)) {
            break;
        }
        ++nsp;
    }
    buf.remove_prefix(nsp);
    std::string_view spaces = " \t\r\n";
    auto p = buf.find_first_of(spaces);
    if (p != buf.npos) {
        buf = buf.substr(0, p);
    }
    if (!buf.empty()) {
        auto cmd = cs.get_ident(buf);
        if (cmd && (cmd->get().type() == cs::ident_type::COMMAND)) {
            return static_cast<cs::command *>(&cmd->get());
        }
    }
    return nullptr;
}

#include "edit_linenoise.hh"
#include "edit_fallback.hh"

/* usage */

void print_usage(std::string_view progname, bool err) {
    std::fprintf(
        err ? stderr : stdout,
        "Usage: %s [options] [file]\n"
        "Options:\n"
        "  -e str  call string \"str\"\n"
        "  -i      enter interactive mode after the above\n"
        "  -v      show version information\n"
        "  -h      show this message\n"
        "  --      stop handling options\n"
        "  -       execute stdin and stop handling options"
        "\n",
        progname.data()
    );
}

void print_version() {
    printf("%s\n", version.data());
}

static cs::state *scs = nullptr;
static void do_sigint(int n) {
    /* in case another SIGINT happens, terminate normally */
    signal(n, SIG_DFL);
    scs->call_hook([](cs::state &css) {
        css.call_hook(nullptr);
        throw cs::error{css, "<execution interrupted>"};
    });
}

static bool do_cat_file(cs::state &cs, std::string_view fname,
                        cs::any_value &ret) {
  FILE *f = std::fopen(fname.data(), "rb");
  char *str = NULL;
  if (f) {
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    str = static_cast<char*>(std::malloc(len));
    memset(str, '\0', strlen(str));
    if (str) {
      std::fread(str, 1, len, f);
      printf("%s", str);

      ret = cs.compile(
		       std::string_view{str, std::size_t(len)}, fname
		       ).call(cs);
      free(str);
      return true;
    }
      return false;
  }
  return false;
}

static bool do_exec_file(
    cs::state &cs, std::string_view fname, cs::any_value &ret
) {
    FILE *f = std::fopen(fname.data(), "rb");
    if (!f) {
        return false;
    }

    std::fseek(f, 0, SEEK_END);
    auto len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    auto buf = std::make_unique<char[]>(len + 1);
    if (!buf) {
        std::fclose(f);
        return false;
    }

    if (std::fread(buf.get(), 1, len, f) != std::size_t(len)) {
        std::fclose(f);
        return false;
    }

    buf[len] = '\0';

    ret = cs.compile(
        std::string_view{buf.get(), std::size_t(len)}, fname
    ).call(cs);
    return true;
}

static bool do_call(cs::state &cs, std::string_view line, bool file = false) {
    cs::any_value ret{};
    scs = &cs;
    signal(SIGINT, do_sigint);
    try {
        if (file) {
            if (!do_exec_file(cs, line, ret)) {
                std::fprintf(stderr, "cannot read file: %s\n", line.data());
            }
        } else {
            ret = cs.compile(line).call(cs);
        }
    } catch (cs::error const &e) {
        signal(SIGINT, SIG_DFL);
        scs = nullptr;
        std::string_view terr = e.what();
        auto col = terr.find(':');
        bool is_lnum = false;
        if (col != terr.npos) {
            auto pre = terr.substr(0, col);
            auto it = std::find_if(
                pre.begin(), pre.end(),
                [](auto c) { return !isdigit(c); }
            );
            is_lnum = (it == pre.end());
            terr = terr.substr(col + 2, terr.size() - col - 2);
        }
        if (!file && ((terr == "missing \"]\"") || (terr == "missing \")\""))) {
            return true;
        }
        std::printf(
            "%s%s\n", !is_lnum ? "stdin: " : "stdin:", e.what().data()
        );
        std::size_t pindex = 1;
        for (auto &nd: e.stack()) {
            std::printf("  ");
            if ((nd.index == 1) && (pindex > 2)) {
                std::printf("..");
            }
            pindex = nd.index;
            std::printf("%zu) %s\n", nd.index, nd.id.name().data());
        }
        return false;
    }
    signal(SIGINT, SIG_DFL);
    scs = nullptr;
    if (ret.type() != cs::value_type::NONE) {
        std::printf("%s\n", std::string_view{ret.get_string(cs)}.data());
    }
    return false;
}

static void do_tty(cs::state &cs) {
    auto &prompt = cs.new_var("PROMPT", "> ");
    auto &prompt2 = cs.new_var("PROMPT2", ">> ");

    bool do_exit = false;
    cs.new_command("quit", "", [&do_exit](auto &, auto, auto &) {
        do_exit = true;
    });

    std::printf("%s (REPL mode)\n", version.data());
    for (;;) {
        auto line = read_line(cs, prompt);
        if (!line) {
            return;
        }
        auto lv = std::move(line.value());
        if (lv.empty()) {
            continue;
        }
        while ((lv.back() == '\\') || do_call(cs, lv)) {
            bool bsl = (lv.back() == '\\');
            if (bsl) {
                lv.resize(lv.size() - 1);
            }
            auto line2 = read_line(cs, prompt2);
            if (!line2) {
                return;
            }
            if (!bsl || (line2.value() == "\\")) {
                lv += '\n';
            }
            lv += line2.value();
        }
        add_history(cs, lv);
        if (do_exit) {
            return;
        }
    }
}

int main(int argc, char **argv) {
    cs::state gcs;
    cs::std_init_all(gcs);

    /* this is how you can override a setter for variables; fvar and svar
     * work equivalently - in this case we want to allow multiple values
     * to be set, but you may also not be using standard i/o and so on
     */
    gcs.new_command("//ivar", "$iii#", [](auto &css, auto args, auto &) {
        auto &iv = static_cast<cs::builtin_var &>(args[0].get_ident(css));
        auto nargs = args[4].get_integer();
        if (nargs <= 1) {
            auto val = iv.value().get_integer();
            if ((val >= 0) && (val < 0xFFFFFF)) {
                std::printf(
                    "%s = %d (0x%.6X: %d, %d, %d)\n",
                    iv.name().data(), val, val,
                    (val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF
                );
            } else {
                std::printf("%s = %d\n", iv.name().data(), val);
            }
            return;
        }
        cs::any_value nv;
        if (nargs == 2) {
            nv = args[1];
        } else if (nargs == 3) {
            nv = (args[1].get_integer() << 8) | (args[2].get_integer() << 16);
        } else {
            nv = (
                args[1].get_integer() | (args[2].get_integer() << 8) |
                (args[3].get_integer() << 16)
            );
        }
        iv.set_value(css, nv);
    });

    gcs.new_command("//var_changed", "$aa", [](auto &css, auto args, auto &) {
        std::printf(
            "changed var trigger: %s (was: '%s', now: '%s')\n",
            args[0].get_ident(css).name().data(),
            args[1].get_string(css).data(),
            args[2].get_string(css).data()
        );
    });

    gcs.new_command("cat", "s", [](auto &css, auto args, auto &) {
      auto file = args[0].get_string(css);
      cs::any_value val{};
      bool ret = do_cat_file(css, file, val);
      if (!ret) {
        char buf[4096];
        std::snprintf(buf, sizeof(buf), "could not read file \"%s\"",
                      file.data());
	throw cs::error(css,buf);
      }
    });

    gcs.new_command("exec", "s", [](auto &css, auto args, auto &) {
        auto file = args[0].get_string(css);
        cs::any_value val{};
        bool ret = do_exec_file(css, file, val);
        if (!ret) {
            char buf[4096];
            std::snprintf(
                buf, sizeof(buf), "could not execute file \"%s\"", file.data()
            );
            throw cs::error(css, buf);
        }
    });

    gcs.new_command("echo", "...", [](auto &css, auto args, auto &) {
        std::printf("%s\n", cs::concat_values(css, args, " ").data());
    });

    int firstarg = 0;
    bool has_inter = false, has_ver = false, has_help = false;
    char const *has_str = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-') {
            firstarg = i;
            goto endargs;
        }
        switch (argv[i][1]) {
            case '-':
                if (argv[i][2] != '\0') {
                    firstarg = -1;
                    goto endargs;
                }
                firstarg = (argv[i + 1] != nullptr) ? (i + 1) : 0;
                goto endargs;
            case '\0':
                firstarg = i;
                goto endargs;
            case 'i':
                if (argv[i][2] != '\0') {
                    firstarg = -1;
                    goto endargs;
                }
                has_inter = true;
                break;
            case 'v':
                if (argv[i][2] != '\0') {
                    firstarg = -1;
                    goto endargs;
                }
                has_ver = true;
                break;
            case 'h':
                if (argv[i][2] != '\0') {
                    firstarg = -1;
                    goto endargs;
                }
                has_help = true;
                break;
            case 'e':
                if (argv[i][2] == '\0') {
                    ++i;
                    if (!argv[i]) {
                        firstarg = -1;
                        goto endargs;
                    } else {
                        has_str = argv[i];
                    }
                } else {
                    has_str = argv[i] + 2;
                }
                break;
            default:
                firstarg = -1;
                goto endargs;
        }
    }
endargs:
    if (firstarg < 0) {
        print_usage(argv[0], true);
        return 1;
    }
    if (has_ver && !has_inter) {
        print_version();
    }
    if (has_help) {
        print_usage(argv[0], false);
        return 0;
    }
    if (has_str) {
        do_call(gcs, has_str);
    }
    if (firstarg) {
        do_call(gcs, argv[firstarg], true);
    }
    if (!firstarg && !has_str && !has_ver) {
        if (stdin_is_tty()) {
            init_lineedit(gcs, argv[0]);
            do_tty(gcs);
            return 0;
        } else {
            std::string str;
            for (int c = '\0'; (c = std::fgetc(stdin)) != EOF;) {
                str += char(c);
            }
            do_call(gcs, str);
        }
    }
    if (has_inter) {
        if (stdin_is_tty()) {
            init_lineedit(gcs, argv[0]);
            do_tty(gcs);
        }
        return 0;
    }
}
