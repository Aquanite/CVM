#include "cvm.h"
#include "cclib.h"

#include <cc/bytecode.h>
#include <cc/diagnostics.h>
#include <cc/loader.h>

#include <ffi.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <process.h>
#include <BaseTsd.h>
#ifndef ssize_t
typedef SSIZE_T ssize_t;
#endif
#ifndef strdup
#define strdup _strdup
#endif
#ifndef access
#define access _access
#endif
#ifndef unlink
#define unlink _unlink
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef X_OK
#define X_OK 0
#endif
#else
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if defined(_WIN32)
#ifndef RTLD_NOW
#define RTLD_NOW 0
#endif
#ifndef RTLD_GLOBAL
#define RTLD_GLOBAL 0
#endif

static void *dlopen(const char *path, int mode) {
    (void)mode;
    if (!path) {
        return (void *)GetModuleHandleA(NULL);
    }
    return (void *)LoadLibraryA(path);
}

static void *dlsym(void *handle, const char *name) {
    if (!handle || !name) {
        return NULL;
    }
    return (void *)GetProcAddress((HMODULE)handle, name);
}

static int dlclose(void *handle) {
    HMODULE self = GetModuleHandleA(NULL);
    if (!handle || (HMODULE)handle == self) {
        return 0;
    }
    return FreeLibrary((HMODULE)handle) ? 0 : -1;
}

static const char *dlerror(void) {
    static char msg[256];
    DWORD err = GetLastError();
    if (!err) {
        return "unknown dynamic loader error";
    }
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL,
                             err,
                             0,
                             msg,
                             (DWORD)sizeof(msg),
                             NULL);
    if (n == 0) {
        snprintf(msg, sizeof(msg), "windows error %lu", (unsigned long)err);
    }
    return msg;
}
#endif

typedef struct VMValue {
    uint64_t bits;
    CCValueType type;
    int is_unsigned;
} VMValue;

typedef struct LabelMap {
    const char *name;
    size_t index;
} LabelMap;

typedef struct GlobalSlot {
    const char *name;
    VMValue value;
    int owns_ptr;
} GlobalSlot;

typedef struct RuntimeModule {
    const char *name;
    CCModule module;
    GlobalSlot *globals;
    size_t global_count;
    uint8_t *ccbin_data;
    size_t ccbin_size;
} RuntimeModule;

typedef struct FuncAlias {
    char *alias;
    const CCFunction *target;
    RuntimeModule *owner;
} FuncAlias;

typedef struct ImportHandle {
    void *handle;
    char *path;
    int owned;
} ImportHandle;

typedef struct JitProfileEntry {
    RuntimeModule *owner;
    const CCFunction *fn;
    uint64_t calls;
    uint64_t loop_backedges;
    int compiled;
    int native_blocked;
} JitProfileEntry;

typedef struct JitNativeEntry {
    RuntimeModule *owner;
    const CCFunction *fn;
    void *fn_ptr;
} JitNativeEntry;

typedef struct CVM {
    RuntimeModule *mods;
    size_t mod_count;
    FuncAlias *aliases;
    size_t alias_count;
    ImportHandle *imports;
    size_t import_count;
    int verbose;
    int jit_mode;
    int jit_attempted;
    const char *jit_cclib_path;
    const CVMOptions *jit_options;
    char **jit_compiled_functions;
    size_t jit_compiled_count;
    JitProfileEntry *jit_profile_entries;
    size_t jit_profile_count;
    JitNativeEntry *jit_native_entries;
    size_t jit_native_count;
    uint64_t jit_hot_threshold;
    uint64_t jit_loop_hot_threshold;
    int jit_profile_report;
    int uses_runtime_profiling_controls;
} CVM;

static int symbol_equivalent(const char *a, const char *b);
static int rewrite_jit_asm_symbols(const char *asm_path);
static int run_jit_from_cclib(const char *cclib_path, const CVMOptions *options, int *exit_code);
static int jit_function_seen(CVM *vm, const char *name);
static int jit_mark_function(CVM *vm, const char *name);
static void jit_unmark_function(CVM *vm, const char *name);
static int jit_compile_function_once(CVM *vm, RuntimeModule *owner, const CCFunction *fn);
static JitProfileEntry *jit_profile_get(CVM *vm, RuntimeModule *owner, const CCFunction *fn, int create);
static JitNativeEntry *jit_native_get(CVM *vm, RuntimeModule *owner, const CCFunction *fn, int create);
static int jit_call_native(const CCFunction *fn, void *fn_ptr, VMValue *argv, size_t argc, VMValue *out_ret);
static int jit_function_can_native_compile(const CCFunction *fn);
static void jit_maybe_compile_hot(CVM *vm, RuntimeModule *owner, const CCFunction *fn,
                                  JitProfileEntry *entry, const char *reason);
static void cvm_print_jit_profile_report(const CVM *vm);
static const CCFunction *find_function_by_ptr(CVM *vm, uintptr_t fn_ptr, RuntimeModule **owner);
static int vm_uses_runtime_profiling_controls(CVM *vm);

static uint64_t f64_to_bits(double v) {
    union {
        double f;
        uint64_t u;
    } cvt;
    cvt.f = v;
    return cvt.u;
}

static double bits_to_f64(uint64_t v) {
    union {
        double f;
        uint64_t u;
    } cvt;
    cvt.u = v;
    return cvt.f;
}

static int64_t as_i64(VMValue v) {
    if (v.type == CC_TYPE_F64) {
        return (int64_t)bits_to_f64(v.bits);
    }
    return (int64_t)v.bits;
}

static uint64_t as_u64(VMValue v) {
    if (v.type == CC_TYPE_F64) {
        return (uint64_t)bits_to_f64(v.bits);
    }
    return v.bits;
}

static uint64_t value_to_u64_for_type(VMValue v, CCValueType ty) {
    if (ty == CC_TYPE_F64 || ty == CC_TYPE_F32) {
        return (uint64_t)bits_to_f64(v.bits);
    }
    return as_u64(v);
}

static size_t vm_type_size(CCValueType ty) {
    size_t s = cc_value_type_size(ty);
    return s ? s : 1;
}

static int vm_type_is_unsigned(CCValueType ty) {
    switch (ty) {
    case CC_TYPE_U8:
    case CC_TYPE_U16:
    case CC_TYPE_U32:
    case CC_TYPE_U64:
    case CC_TYPE_PTR:
        return 1;
    default:
        return 0;
    }
}

static unsigned vm_type_bit_width(CCValueType ty) {
    switch (ty) {
    case CC_TYPE_I1:
        return 1;
    case CC_TYPE_I8:
    case CC_TYPE_U8:
        return 8;
    case CC_TYPE_I16:
    case CC_TYPE_U16:
        return 16;
    case CC_TYPE_I32:
    case CC_TYPE_U32:
    case CC_TYPE_F32:
        return 32;
    case CC_TYPE_I64:
    case CC_TYPE_U64:
    case CC_TYPE_PTR:
    case CC_TYPE_F64:
        return 64;
    default:
        return 0;
    }
}

static uint64_t vm_mask_for_bits(unsigned bits) {
    if (bits == 0) {
        return 0;
    }
    if (bits >= 64) {
        return ~0ULL;
    }
    return (1ULL << bits) - 1ULL;
}

static int64_t vm_sign_extend_bits(uint64_t value, unsigned bits) {
    if (bits == 0) {
        return 0;
    }
    if (bits >= 64) {
        return (int64_t)value;
    }
    uint64_t mask = vm_mask_for_bits(bits);
    uint64_t sign_bit = 1ULL << (bits - 1);
    value &= mask;
    if (value & sign_bit) {
        value |= ~mask;
    }
    return (int64_t)value;
}

static double vm_value_as_double(VMValue in, CCValueType from_type) {
    if (from_type == CC_TYPE_F64) {
        if (in.type == CC_TYPE_F64) {
            return bits_to_f64(in.bits);
        }
        union {
            uint64_t u;
            double d;
        } cvt;
        cvt.u = as_u64(in);
        return cvt.d;
    }
    if (from_type == CC_TYPE_F32) {
        if (in.type == CC_TYPE_F64) {
            return bits_to_f64(in.bits);
        }
        union {
            uint32_t u;
            float f;
        } cvt;
        cvt.u = (uint32_t)as_u64(in);
        return (double)cvt.f;
    }
    if (in.type == CC_TYPE_F64) {
        return bits_to_f64(in.bits);
    }
    return (double)as_i64(in);
}

static VMValue load_indirect_value(uintptr_t addr, CCValueType ty, int is_unsigned) {
    VMValue out;
    memset(&out, 0, sizeof(out));
    out.type = ty;
    out.is_unsigned = is_unsigned;

    switch (ty) {
    case CC_TYPE_I1:
    case CC_TYPE_U8:
        out.bits = (uint64_t)(*(uint8_t *)addr);
        break;
    case CC_TYPE_I8:
        out.bits = (uint64_t)(int64_t)(*(int8_t *)addr);
        break;
    case CC_TYPE_I16:
        out.bits = (uint64_t)(int64_t)(*(int16_t *)addr);
        break;
    case CC_TYPE_U16:
        out.bits = (uint64_t)(*(uint16_t *)addr);
        break;
    case CC_TYPE_I32:
        out.bits = (uint64_t)(int64_t)(*(int32_t *)addr);
        break;
    case CC_TYPE_U32:
        out.bits = (uint64_t)(*(uint32_t *)addr);
        break;
    case CC_TYPE_I64:
    case CC_TYPE_U64:
    case CC_TYPE_PTR:
        out.bits = *(uint64_t *)addr;
        break;
    case CC_TYPE_F32: {
        float f = *(float *)addr;
        out.type = CC_TYPE_F64;
        out.bits = f64_to_bits((double)f);
        break;
    }
    case CC_TYPE_F64:
        out.bits = f64_to_bits(*(double *)addr);
        out.type = CC_TYPE_F64;
        break;
    default:
        out.bits = 0;
        break;
    }

    return out;
}

static void store_indirect_value(uintptr_t addr, CCValueType ty, VMValue in) {
    switch (ty) {
    case CC_TYPE_I1:
    case CC_TYPE_U8:
        *(uint8_t *)addr = (uint8_t)(value_to_u64_for_type(in, ty) & 0xffu);
        break;
    case CC_TYPE_I8:
        *(int8_t *)addr = (int8_t)value_to_u64_for_type(in, ty);
        break;
    case CC_TYPE_I16:
        *(int16_t *)addr = (int16_t)value_to_u64_for_type(in, ty);
        break;
    case CC_TYPE_U16:
        *(uint16_t *)addr = (uint16_t)value_to_u64_for_type(in, ty);
        break;
    case CC_TYPE_I32:
        *(int32_t *)addr = (int32_t)value_to_u64_for_type(in, ty);
        break;
    case CC_TYPE_U32:
        *(uint32_t *)addr = (uint32_t)value_to_u64_for_type(in, ty);
        break;
    case CC_TYPE_I64:
    case CC_TYPE_U64:
    case CC_TYPE_PTR:
        *(uint64_t *)addr = (uint64_t)value_to_u64_for_type(in, ty);
        break;
    case CC_TYPE_F32: {
        float f = (in.type == CC_TYPE_F64)
                      ? (float)bits_to_f64(in.bits)
                      : (float)(int64_t)as_i64(in);
        *(float *)addr = f;
        break;
    }
    case CC_TYPE_F64: {
        double d = (in.type == CC_TYPE_F64)
                       ? bits_to_f64(in.bits)
                       : (double)(int64_t)as_i64(in);
        *(double *)addr = d;
        break;
    }
    default:
        break;
    }
}

static void free_runtime(CVM *vm) {
    if (!vm) {
        return;
    }
    if (vm->mods) {
        for (size_t i = 0; i < vm->mod_count; ++i) {
            RuntimeModule *m = &vm->mods[i];
            if (m->globals) {
                for (size_t g = 0; g < m->global_count; ++g) {
                    if (m->globals[g].owns_ptr && m->globals[g].value.type == CC_TYPE_PTR && m->globals[g].value.bits != 0) {
                        free((void *)(uintptr_t)m->globals[g].value.bits);
                    }
                }
                free(m->globals);
            }
            cc_module_free(&m->module);
            free((char *)m->name);
            free(m->ccbin_data);
        }
        free(vm->mods);
    }
    if (vm->aliases) {
        for (size_t i = 0; i < vm->alias_count; ++i) {
            free(vm->aliases[i].alias);
        }
        free(vm->aliases);
    }
    if (vm->jit_compiled_functions) {
        for (size_t i = 0; i < vm->jit_compiled_count; ++i) {
            free(vm->jit_compiled_functions[i]);
        }
        free(vm->jit_compiled_functions);
    }
    free(vm->jit_profile_entries);
    free(vm->jit_native_entries);
    if (vm->imports) {
        for (size_t i = 0; i < vm->import_count; ++i) {
            if (vm->imports[i].owned && vm->imports[i].handle) {
                dlclose(vm->imports[i].handle);
            }
            free(vm->imports[i].path);
        }
        free(vm->imports);
    }
    memset(vm, 0, sizeof(*vm));
}

static int add_import_handle(CVM *vm, const char *path, int owned, void *handle) {
    size_t next = vm->import_count + 1;
    ImportHandle *grown = (ImportHandle *)realloc(vm->imports, next * sizeof(ImportHandle));
    if (!grown) {
        return -1;
    }
    vm->imports = grown;
    vm->imports[vm->import_count].handle = handle;
    vm->imports[vm->import_count].path = path ? strdup(path) : NULL;
    vm->imports[vm->import_count].owned = owned;
    vm->import_count = next;
    return 0;
}

static int add_default_c_runtime_import(CVM *vm) {
#if defined(__APPLE__)
    const char *candidates[] = {
        "/usr/lib/libSystem.B.dylib",
        "libSystem.B.dylib"
    };
#elif defined(__linux__)
    const char *candidates[] = {
        "libc.so.6",
        "libc.so"
    };
#elif defined(_WIN32)
    const char *candidates[] = {
        "ucrtbase.dll",
        "msvcrt.dll"
    };
#else
    const char *candidates[] = {
        "libc.so"
    };
#endif

    for (size_t i = 0; i < (sizeof(candidates) / sizeof(candidates[0])); ++i) {
        void *h = dlopen(candidates[i], RTLD_NOW | RTLD_GLOBAL);
        if (!h) {
            continue;
        }
        if (add_import_handle(vm, candidates[i], 1, h) != 0) {
            dlclose(h);
            return -1;
        }
        return 0;
    }
    return 0;
}

static void *resolve_extern_symbol(CVM *vm, const char *name) {
    if (!name || !*name) {
        return NULL;
    }
    for (size_t i = 0; i < vm->import_count; ++i) {
        void *sym = dlsym(vm->imports[i].handle, name);
        if (sym) {
            return sym;
        }
    }

#ifdef __APPLE__
    char underscored[512];
    if (snprintf(underscored, sizeof(underscored), "_%s", name) > 0) {
        for (size_t i = 0; i < vm->import_count; ++i) {
            void *sym = dlsym(vm->imports[i].handle, underscored);
            if (sym) {
                return sym;
            }
        }
    }
#endif

    return NULL;
}

static uint64_t call_c_u64_runtime(void *fn, uint64_t *argv, size_t argc, int is_varargs) {
    if (!fn) {
        return 0;
    }

    ffi_cif cif;
    ffi_status st;
    ffi_type **arg_types = NULL;
    void **arg_values = NULL;
    uint64_t *arg_storage = NULL;
    uint64_t ret = 0;

    if (argc > 0) {
        arg_types = (ffi_type **)calloc(argc, sizeof(ffi_type *));
        arg_values = (void **)calloc(argc, sizeof(void *));
        arg_storage = (uint64_t *)calloc(argc, sizeof(uint64_t));
        if (!arg_types || !arg_values || !arg_storage) {
            free(arg_types);
            free(arg_values);
            free(arg_storage);
            return 0;
        }

        for (size_t i = 0; i < argc; ++i) {
            arg_types[i] = &ffi_type_uint64;
            arg_storage[i] = argv ? argv[i] : 0;
            arg_values[i] = &arg_storage[i];
        }
    }

    if (is_varargs && argc > 0) {
        st = ffi_prep_cif_var(&cif,
                              FFI_DEFAULT_ABI,
                              1,
                              (unsigned int)argc,
                              &ffi_type_uint64,
                              arg_types);
    } else {
        st = ffi_prep_cif(&cif,
                          FFI_DEFAULT_ABI,
                          (unsigned int)argc,
                          &ffi_type_uint64,
                          arg_types);
    }
    if (st != FFI_OK) {
        free(arg_types);
        free(arg_values);
        free(arg_storage);
        return 0;
    }

    ffi_call(&cif, FFI_FN(fn), &ret, arg_values);

    free(arg_types);
    free(arg_values);
    free(arg_storage);
    return ret;
}

static uint64_t call_c_u64(void *fn, uint64_t *argv, size_t argc) {
    return call_c_u64_runtime(fn, argv, argc, 0);
}

static uint64_t call_c_u64_varargs(void *fn, uint64_t *argv, size_t argc) {
    return call_c_u64_runtime(fn, argv, argc, 1);
}

static uint64_t *vm_make_call_args(VMValue *argv, size_t argc) {
    if (argc == 0) {
        return NULL;
    }

    uint64_t *call_args = (uint64_t *)calloc(argc, sizeof(uint64_t));
    if (!call_args) {
        return NULL;
    }
    for (size_t i = 0; i < argc; ++i) {
        call_args[i] = argv[i].bits;
    }
    return call_args;
}

static int append_text(char **buf, size_t *len, size_t *cap, const char *text) {
    if (!buf || !len || !cap || !text) {
        return -1;
    }
    size_t n = strlen(text);
    if (*len + n + 1 > *cap) {
        size_t next = (*cap == 0) ? 128 : *cap;
        while (*len + n + 1 > next) {
            next *= 2;
        }
        char *grown = (char *)realloc(*buf, next);
        if (!grown) {
            return -1;
        }
        *buf = grown;
        *cap = next;
    }
    memcpy(*buf + *len, text, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

static char *format_builtin_varargs(VMValue *argv, size_t argc) {
    if (!argv || argc == 0) {
        return NULL;
    }
    const char *fmt = (const char *)(uintptr_t)argv[0].bits;
    if (!fmt) {
        return NULL;
    }

    char *out = NULL;
    size_t out_len = 0;
    size_t out_cap = 0;
    size_t argi = 1;

    for (size_t i = 0; fmt[i] != '\0'; ++i) {
        if (fmt[i] != '%') {
            char tmp[2] = {fmt[i], '\0'};
            if (append_text(&out, &out_len, &out_cap, tmp) != 0) {
                free(out);
                return NULL;
            }
            continue;
        }

        if (fmt[i + 1] == '%') {
            if (append_text(&out, &out_len, &out_cap, "%") != 0) {
                free(out);
                return NULL;
            }
            ++i;
            continue;
        }

        char spec = fmt[i + 1];
        if (spec == '\0') {
            break;
        }
        ++i;

        if (argi >= argc) {
            char fallback[3] = {'%', spec, '\0'};
            if (append_text(&out, &out_len, &out_cap, fallback) != 0) {
                free(out);
                return NULL;
            }
            continue;
        }

        char tmp[128];
        tmp[0] = '\0';
        VMValue av = argv[argi++];
        switch (spec) {
        case 'd':
        case 'i':
            snprintf(tmp, sizeof(tmp), "%lld", (long long)as_i64(av));
            break;
        case 'u':
            snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)as_u64(av));
            break;
        case 'x':
            snprintf(tmp, sizeof(tmp), "%llx", (unsigned long long)as_u64(av));
            break;
        case 'X':
            snprintf(tmp, sizeof(tmp), "%llX", (unsigned long long)as_u64(av));
            break;
        case 'p':
            snprintf(tmp, sizeof(tmp), "%p", (void *)(uintptr_t)as_u64(av));
            break;
        case 'c':
            snprintf(tmp, sizeof(tmp), "%c", (int)(as_u64(av) & 0xff));
            break;
        case 's': {
            const char *s = (const char *)(uintptr_t)as_u64(av);
            if (!s) {
                s = "(null)";
            }
            if (append_text(&out, &out_len, &out_cap, s) != 0) {
                free(out);
                return NULL;
            }
            continue;
        }
        default: {
            char fallback[3] = {'%', spec, '\0'};
            if (append_text(&out, &out_len, &out_cap, fallback) != 0) {
                free(out);
                return NULL;
            }
            continue;
        }
        }

        if (append_text(&out, &out_len, &out_cap, tmp) != 0) {
            free(out);
            return NULL;
        }
    }

    if (!out) {
        out = strdup("");
    }
    return out;
}

static int call_builtin_symbol(CVM *vm, const char *symbol, VMValue *argv, size_t argc, uint64_t *out_ret) {
    if (!symbol || !out_ret) {
        return 0;
    }

    if ((symbol_equivalent(symbol, "Std_Profiling_IsHotPath_ptr_to_void") ||
         symbol_equivalent(symbol, "Std.Profiling.IsHotPath")) &&
        argc >= 1 && vm) {
        if (vm->jit_mode == 0) {
            *out_ret = 0;
            return 1;
        }
        uintptr_t fn_ptr = (uintptr_t)as_u64(argv[0]);
        RuntimeModule *owner = NULL;
        const CCFunction *fn = find_function_by_ptr(vm, fn_ptr, &owner);
        if (!fn || !owner) {
            *out_ret = 0;
            return 1;
        }

        JitProfileEntry *entry = jit_profile_get(vm, owner, fn, 0);
        if (entry) {
            int hot = 0;
            if (entry->compiled || jit_function_seen(vm, fn->name)) {
                hot = 1;
            }
            if (!hot && vm->jit_hot_threshold > 0 && entry->calls >= vm->jit_hot_threshold) {
                hot = 1;
            }
            if (!hot && vm->jit_loop_hot_threshold > 0 && entry->loop_backedges >= vm->jit_loop_hot_threshold) {
                hot = 1;
            }
            *out_ret = hot ? 1u : 0u;
        } else {
            *out_ret = jit_function_seen(vm, fn->name) ? 1u : 0u;
        }
        return 1;
    }

    if ((symbol_equivalent(symbol, "Std_Profiling_ForceHot_ptr_to_void") ||
         symbol_equivalent(symbol, "Std.Profiling.ForceHot")) &&
        argc >= 1 && vm) {
        if (vm->jit_mode == 0) {
            *out_ret = 1;
            return 1;
        }
        uintptr_t fn_ptr = (uintptr_t)as_u64(argv[0]);
        RuntimeModule *owner = NULL;
        const CCFunction *fn = find_function_by_ptr(vm, fn_ptr, &owner);
        if (!fn || !owner) {
            *out_ret = 0;
            return 1;
        }

        JitProfileEntry *entry = jit_profile_get(vm, owner, fn, 1);
        if (!entry) {
            *out_ret = 0;
            return 1;
        }

        if (!jit_function_can_native_compile(fn)) {
            entry->native_blocked = 1;
            *out_ret = 0;
            return 1;
        }

        if (!jit_function_seen(vm, fn->name)) {
            if (vm->jit_mode == 2 && vm->jit_options) {
                if (jit_compile_function_once(vm, owner, fn) != 0) {
                    entry->native_blocked = 1;
                    *out_ret = 0;
                    return 1;
                }
            } else if (jit_mark_function(vm, fn->name) != 0) {
                *out_ret = 0;
                return 1;
            }
        }

        entry->compiled = 1;
        *out_ret = 1;
        return 1;
    }

    if ((symbol_equivalent(symbol, "Std_Profiling_ForceCold_ptr_to_void") ||
         symbol_equivalent(symbol, "Std.Profiling.ForceCold")) &&
        argc >= 1 && vm) {
        if (vm->jit_mode == 0) {
            *out_ret = 1;
            return 1;
        }
        uintptr_t fn_ptr = (uintptr_t)as_u64(argv[0]);
        RuntimeModule *owner = NULL;
        const CCFunction *fn = find_function_by_ptr(vm, fn_ptr, &owner);
        if (!fn || !owner) {
            *out_ret = 0;
            return 1;
        }

        JitProfileEntry *entry = jit_profile_get(vm, owner, fn, 1);
        if (!entry) {
            *out_ret = 0;
            return 1;
        }

        entry->calls = 0;
        entry->loop_backedges = 0;
        entry->compiled = 0;
        entry->native_blocked = 0;
        jit_unmark_function(vm, fn->name);

        *out_ret = 1;
        return 1;
    }

    if (symbol_equivalent(symbol, "Std_IO_printnl_ptr_to_char") ||
        symbol_equivalent(symbol, "Std_IO_printnl_ptr_to_char_varargs")) {
        uint64_t written = 0;
        if (argc >= 1) {
            char *formatted = NULL;
            const char *s = NULL;
            if (symbol_equivalent(symbol, "Std_IO_printnl_ptr_to_char_varargs") || argc > 1) {
                formatted = format_builtin_varargs(argv, argc);
                s = formatted;
            } else {
                s = (const char *)(uintptr_t)argv[0].bits;
            }
            if (s) {
                fputs(s, stdout);
                written += (uint64_t)strlen(s);
                if (strchr(s, '\n') == NULL) {
                    fputc('\n', stdout);
                    written += 1;
                }
            }
            free(formatted);
            fflush(stdout);
        }
        *out_ret = written;
        return 1;
    }

    if (symbol_equivalent(symbol, "Std_IO_print_ptr_to_char") ||
        symbol_equivalent(symbol, "Std_IO_print_ptr_to_char_varargs")) {
        uint64_t written = 0;
        if (argc >= 1) {
            char *formatted = NULL;
            const char *s = NULL;
            if (symbol_equivalent(symbol, "Std_IO_print_ptr_to_char_varargs") || argc > 1) {
                formatted = format_builtin_varargs(argv, argc);
                s = formatted;
            } else {
                s = (const char *)(uintptr_t)argv[0].bits;
            }
            if (s) {
                fputs(s, stdout);
                written += (uint64_t)strlen(s);
            }
            free(formatted);
            fflush(stdout);
        }
        *out_ret = written;
        return 1;
    }

    if (symbol_equivalent(symbol, "fwrite") && argc >= 4) {
        const void *buf = (const void *)(uintptr_t)argv[0].bits;
        size_t size = (size_t)as_u64(argv[1]);
        size_t count = (size_t)as_u64(argv[2]);
        uintptr_t handle_bits = (uintptr_t)as_u64(argv[3]);

        if (handle_bits <= 2u) {
            size_t total = size * count;
            ssize_t written = write((int)handle_bits, buf, total);
            if (written <= 0 || size == 0) {
                *out_ret = 0;
            } else {
                *out_ret = (uint64_t)((size_t)written / size);
            }
            return 1;
        }

        size_t written_items = fwrite(buf, size, count, (FILE *)handle_bits);
        *out_ret = (uint64_t)written_items;
        return 1;
    }

    if (symbol_equivalent(symbol, "read") && argc >= 3) {
        int fd = (int)as_i64(argv[0]);
        void *buf = (void *)(uintptr_t)argv[1].bits;
        size_t count = (size_t)as_u64(argv[2]);
        ssize_t rc = read(fd, buf, count);
        if (rc < 0) {
            rc = 0;
        }
        *out_ret = (uint64_t)(int64_t)rc;
        return 1;
    }

    if (symbol_equivalent(symbol, "write") && argc >= 3) {
        int fd = (int)as_i64(argv[0]);
        const void *buf = (const void *)(uintptr_t)argv[1].bits;
        size_t count = (size_t)as_u64(argv[2]);
        ssize_t rc = write(fd, buf, count);
        *out_ret = (uint64_t)(int64_t)rc;
        return 1;
    }

    if (symbol_equivalent(symbol, "Std_String_strlen_ptr_to_char") && argc >= 1) {
        const char *s = (const char *)(uintptr_t)argv[0].bits;
        *out_ret = (uint64_t)(s ? strlen(s) : 0u);
        return 1;
    }

    if (symbol_equivalent(symbol, "Std_String_strcmp_ptr_to_char_ptr_to_char") && argc >= 2) {
        const char *a = (const char *)(uintptr_t)argv[0].bits;
        const char *b = (const char *)(uintptr_t)argv[1].bits;
        if (!a) a = "";
        if (!b) b = "";
        *out_ret = (uint64_t)(int64_t)strcmp(a, b);
        return 1;
    }

    return 0;
}

static char symbol_norm_char(char ch) {
    if ((ch >= 'a' && ch <= 'z') ||
        (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9')) {
        return ch;
    }
    return '_';
}

static int symbol_equivalent(const char *a, const char *b) {
    if (!a || !b) {
        return 0;
    }

    size_t ia = 0;
    size_t ib = 0;
    while (a[ia] != '\0' && b[ib] != '\0') {
        if (symbol_norm_char(a[ia]) != symbol_norm_char(b[ib])) {
            return 0;
        }
        ia++;
        ib++;
    }
    return a[ia] == '\0' && b[ib] == '\0';
}

static int vm_uses_runtime_profiling_controls(CVM *vm) {
    if (!vm) {
        return 0;
    }
    for (size_t mi = 0; mi < vm->mod_count; ++mi) {
        RuntimeModule *m = &vm->mods[mi];
        for (size_t fi = 0; fi < m->module.function_count; ++fi) {
            const CCFunction *fn = &m->module.functions[fi];
            for (size_t ii = 0; ii < fn->instruction_count; ++ii) {
                const CCInstruction *ins = &fn->instructions[ii];
                if (ins->kind != CC_INSTR_CALL || !ins->data.call.symbol) {
                    continue;
                }
                if (symbol_equivalent(ins->data.call.symbol, "Std_Profiling_IsHotPath_ptr_to_void") ||
                    symbol_equivalent(ins->data.call.symbol, "Std_Profiling_ForceHot_ptr_to_void") ||
                    symbol_equivalent(ins->data.call.symbol, "Std_Profiling_ForceCold_ptr_to_void") ||
                    symbol_equivalent(ins->data.call.symbol, "Std.Profiling.IsHotPath") ||
                    symbol_equivalent(ins->data.call.symbol, "Std.Profiling.ForceHot") ||
                    symbol_equivalent(ins->data.call.symbol, "Std.Profiling.ForceCold")) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int add_func_alias(CVM *vm, const char *alias, RuntimeModule *owner, const CCFunction *target) {
    if (!vm || !alias || !*alias || !target) {
        return 0;
    }
    for (size_t i = 0; i < vm->alias_count; ++i) {
        if (strcmp(vm->aliases[i].alias, alias) == 0) {
            return 0;
        }
    }

    size_t next = vm->alias_count + 1;
    FuncAlias *grown = (FuncAlias *)realloc(vm->aliases, next * sizeof(FuncAlias));
    if (!grown) {
        return -1;
    }
    vm->aliases = grown;
    vm->aliases[vm->alias_count].alias = strdup(alias);
    if (!vm->aliases[vm->alias_count].alias) {
        return -1;
    }
    vm->aliases[vm->alias_count].target = target;
    vm->aliases[vm->alias_count].owner = owner;
    vm->alias_count = next;
    return 0;
}

static const CCFunction *find_function(CVM *vm, const char *name, RuntimeModule **owner) {
    RuntimeModule *resolved_owner = NULL;

    for (size_t ai = 0; ai < vm->alias_count; ++ai) {
        FuncAlias *a = &vm->aliases[ai];
        if (strcmp(a->alias, name) == 0 || symbol_equivalent(a->alias, name)) {
            if (owner) {
                for (size_t mi = 0; mi < vm->mod_count; ++mi) {
                    RuntimeModule *m = &vm->mods[mi];
                    const CCFunction *base = m->module.functions;
                    const CCFunction *end = base + m->module.function_count;
                    if (a->target >= base && a->target < end) {
                        resolved_owner = m;
                        break;
                    }
                }
                *owner = resolved_owner;
            }
            return a->target;
        }
    }

    for (size_t mi = 0; mi < vm->mod_count; ++mi) {
        RuntimeModule *m = &vm->mods[mi];
        for (size_t fi = 0; fi < m->module.function_count; ++fi) {
            const CCFunction *fn = &m->module.functions[fi];
            if (fn->name && (strcmp(fn->name, name) == 0 || symbol_equivalent(fn->name, name))) {
                if (owner) {
                    *owner = m;
                }
                return fn;
            }
        }
    }

    if (name && strstr(name, "_varargs") == NULL) {
        char varargs_name[1024];
        if (snprintf(varargs_name, sizeof(varargs_name), "%s_varargs", name) > 0) {
            for (size_t ai = 0; ai < vm->alias_count; ++ai) {
                FuncAlias *a = &vm->aliases[ai];
                if (strcmp(a->alias, varargs_name) == 0 || symbol_equivalent(a->alias, varargs_name)) {
                    if (owner) {
                        resolved_owner = NULL;
                        for (size_t mi = 0; mi < vm->mod_count; ++mi) {
                            RuntimeModule *m = &vm->mods[mi];
                            const CCFunction *base = m->module.functions;
                            const CCFunction *end = base + m->module.function_count;
                            if (a->target >= base && a->target < end) {
                                resolved_owner = m;
                                break;
                            }
                        }
                        *owner = resolved_owner;
                    }
                    return a->target;
                }
            }

            for (size_t mi = 0; mi < vm->mod_count; ++mi) {
                RuntimeModule *m = &vm->mods[mi];
                for (size_t fi = 0; fi < m->module.function_count; ++fi) {
                    const CCFunction *fn = &m->module.functions[fi];
                    if (fn->name && (strcmp(fn->name, varargs_name) == 0 || symbol_equivalent(fn->name, varargs_name))) {
                        if (owner) {
                            *owner = m;
                        }
                        return fn;
                    }
                }
            }
        }
    }
    return NULL;
}

static const CCFunction *find_function_exact(CVM *vm, const char *name, RuntimeModule **owner) {
    RuntimeModule *resolved_owner = NULL;
    if (!vm || !name) {
        if (owner) {
            *owner = NULL;
        }
        return NULL;
    }

    for (size_t mi = 0; mi < vm->mod_count; ++mi) {
        RuntimeModule *m = &vm->mods[mi];
        for (size_t fi = 0; fi < m->module.function_count; ++fi) {
            const CCFunction *fn = &m->module.functions[fi];
            if (fn->name && strcmp(fn->name, name) == 0) {
                resolved_owner = m;
                if (owner) {
                    *owner = resolved_owner;
                }
                return fn;
            }
        }
    }

    if (owner) {
        *owner = NULL;
    }
    return NULL;
}

static int vm_declares_extern_exact(CVM *vm, const char *name) {
    if (!vm || !name) {
        return 0;
    }
    for (size_t mi = 0; mi < vm->mod_count; ++mi) {
        RuntimeModule *m = &vm->mods[mi];
        for (size_t ei = 0; ei < m->module.extern_count; ++ei) {
            const CCExtern *ex = &m->module.externs[ei];
            if (ex->name && strcmp(ex->name, name) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

static const CCFunction *find_function_by_ptr(CVM *vm, uintptr_t fn_ptr, RuntimeModule **owner) {
    if (!vm || !fn_ptr) {
        return NULL;
    }
    for (size_t mi = 0; mi < vm->mod_count; ++mi) {
        RuntimeModule *m = &vm->mods[mi];
        for (size_t fi = 0; fi < m->module.function_count; ++fi) {
            const CCFunction *fn = &m->module.functions[fi];
            if ((uintptr_t)fn == fn_ptr) {
                if (owner) {
                    *owner = m;
                }
                return fn;
            }
        }
    }
    return NULL;
}

static GlobalSlot *find_global_slot(CVM *vm, const char *name) {
    for (size_t mi = 0; mi < vm->mod_count; ++mi) {
        RuntimeModule *m = &vm->mods[mi];
        for (size_t gi = 0; gi < m->global_count; ++gi) {
            if (m->globals[gi].name && strcmp(m->globals[gi].name, name) == 0) {
                return &m->globals[gi];
            }
        }
    }
    return NULL;
}

static int build_labels(const CCFunction *fn, LabelMap **out_labels, size_t *out_count) {
    *out_labels = NULL;
    *out_count = 0;
    for (size_t i = 0; i < fn->instruction_count; ++i) {
        if (fn->instructions[i].kind == CC_INSTR_LABEL) {
            size_t next = *out_count + 1;
            LabelMap *grown = (LabelMap *)realloc(*out_labels, next * sizeof(LabelMap));
            if (!grown) {
                free(*out_labels);
                *out_labels = NULL;
                *out_count = 0;
                return -1;
            }
            *out_labels = grown;
            (*out_labels)[*out_count].name = fn->instructions[i].data.label.name;
            (*out_labels)[*out_count].index = i;
            *out_count = next;
        }
    }
    return 0;
}

static int label_index(LabelMap *labels, size_t count, const char *name, size_t *index) {
    for (size_t i = 0; i < count; ++i) {
        if (labels[i].name && strcmp(labels[i].name, name) == 0) {
            *index = labels[i].index;
            return 0;
        }
    }
    return -1;
}

static JitProfileEntry *jit_profile_get(CVM *vm, RuntimeModule *owner, const CCFunction *fn, int create) {
    if (!vm || !owner || !fn) {
        return NULL;
    }
    for (size_t i = 0; i < vm->jit_profile_count; ++i) {
        JitProfileEntry *entry = &vm->jit_profile_entries[i];
        if (entry->owner == owner && entry->fn == fn) {
            return entry;
        }
    }
    if (!create) {
        return NULL;
    }

    size_t next = vm->jit_profile_count + 1;
    JitProfileEntry *grown = (JitProfileEntry *)realloc(vm->jit_profile_entries,
                                                        next * sizeof(JitProfileEntry));
    if (!grown) {
        return NULL;
    }
    vm->jit_profile_entries = grown;
    vm->jit_profile_entries[vm->jit_profile_count].owner = owner;
    vm->jit_profile_entries[vm->jit_profile_count].fn = fn;
    vm->jit_profile_entries[vm->jit_profile_count].calls = 0;
    vm->jit_profile_entries[vm->jit_profile_count].loop_backedges = 0;
    vm->jit_profile_entries[vm->jit_profile_count].compiled = 0;
    vm->jit_profile_entries[vm->jit_profile_count].native_blocked = 0;
    vm->jit_profile_count = next;
    return &vm->jit_profile_entries[next - 1];
}

static void jit_maybe_compile_hot(CVM *vm, RuntimeModule *owner, const CCFunction *fn,
                                  JitProfileEntry *entry, const char *reason) {
    int should_compile = 0;
    if (!vm || !owner || !fn || !entry || vm->jit_mode != 2 || !vm->jit_options) {
        return;
    }
    if (entry->native_blocked) {
        return;
    }
    if (entry->compiled || jit_function_seen(vm, fn->name)) {
        entry->compiled = 1;
        return;
    }

    if (vm->jit_hot_threshold > 0 && entry->calls >= vm->jit_hot_threshold) {
        should_compile = 1;
    }
    if (vm->jit_loop_hot_threshold > 0 && entry->loop_backedges >= vm->jit_loop_hot_threshold) {
        should_compile = 1;
    }
    if (!should_compile) {
        return;
    }

    if (!jit_function_can_native_compile(fn)) {
        entry->native_blocked = 1;
        if (vm->verbose) {
            fprintf(stderr,
                    "cvm[jit]: native compile blocked for '%s' (unsupported call/ABI pattern)\n",
                    fn->name ? fn->name : "<unnamed>");
        }
        return;
    }

    if (jit_compile_function_once(vm, owner, fn) == 0) {
        entry->compiled = 1;
        if (vm->verbose) {
            fprintf(stderr,
                    "cvm[jit]: hot-compiled '%s' after %" PRIu64 " calls / %" PRIu64 " loop backedges (%s)\n",
                    fn->name ? fn->name : "<unnamed>",
                    entry->calls,
                    entry->loop_backedges,
                    reason ? reason : "hotness");
        }
    } else if (vm->verbose) {
        entry->native_blocked = 1;
        fprintf(stderr,
                "cvm[jit]: failed hot-compile for '%s', continuing in interpreter\n",
                fn->name ? fn->name : "<unnamed>");
    } else {
        entry->native_blocked = 1;
    }
}

static int jit_function_can_native_compile(const CCFunction *fn) {
    if (!fn) {
        return 0;
    }
    if (fn->is_varargs || fn->param_count > 8) {
        return 0;
    }
    if (fn->return_type == CC_TYPE_F32 || fn->return_type == CC_TYPE_F64) {
        return 0;
    }
    for (size_t i = 0; i < fn->param_count; ++i) {
        if (fn->param_types[i] == CC_TYPE_F32 || fn->param_types[i] == CC_TYPE_F64) {
            return 0;
        }
    }
    for (size_t i = 0; i < fn->instruction_count; ++i) {
        switch (fn->instructions[i].kind) {
        case CC_INSTR_CALL:
        case CC_INSTR_CALL_INDIRECT:
        case CC_INSTR_JUMP_INDIRECT:
            return 0;
        default:
            break;
        }
    }
    return 1;
}

static int execute_function(CVM *vm, RuntimeModule *owner, const CCFunction *fn,
                            VMValue *args, size_t arg_count,
                            VMValue *out_ret, int *has_ret) {
    if (owner && fn) {
        JitProfileEntry *entry = jit_profile_get(vm, owner, fn, 1);
        if (entry) {
            entry->calls += 1;
            jit_maybe_compile_hot(vm, owner, fn, entry, "call threshold");
        }
    }

    if (vm->jit_mode == 1 && !vm->jit_attempted && vm->jit_options) {
        int jit_exit = 0;
        vm->jit_attempted = 1;
        int jit_rc = -1;
        if (vm->jit_cclib_path) {
            jit_rc = run_jit_from_cclib(vm->jit_cclib_path, vm->jit_options, &jit_exit);
        }

        if (jit_rc == 0) {
            out_ret->bits = (uint64_t)(jit_exit & 0xff);
            out_ret->type = fn->return_type;
            out_ret->is_unsigned = 0;
            *has_ret = (fn->return_type != CC_TYPE_VOID);
            return 0;
        }
        if (vm->verbose) {
            fprintf(stderr, "cvm[jit]: falling back to interpreter\n");
        }
    }

    uint8_t *local_mem = NULL;
    size_t *local_offsets = NULL;
    if (fn->local_count > 0) {
        local_offsets = (size_t *)calloc(fn->local_count, sizeof(size_t));
        if (!local_offsets) {
            return -1;
        }
        size_t total = 0;
        for (size_t i = 0; i < fn->local_count; ++i) {
            size_t sz = vm_type_size(fn->local_types[i]);
            total = (total + 7u) & ~7u;
            local_offsets[i] = total;
            total += sz;
        }
        local_mem = (uint8_t *)calloc(total ? total : 1, 1);
        if (!local_mem) {
            free(local_offsets);
            return -1;
        }
    }

    uint8_t *param_mem = NULL;
    size_t *param_offsets = NULL;
    size_t param_slot_count = fn->param_count;
    if (fn->is_varargs && arg_count > param_slot_count) {
        param_slot_count = arg_count;
    }
    if (param_slot_count > 0) {
        param_offsets = (size_t *)calloc(param_slot_count, sizeof(size_t));
        if (!param_offsets) {
            free(local_mem);
            free(local_offsets);
            return -1;
        }
        size_t total = 0;
        for (size_t i = 0; i < param_slot_count; ++i) {
            CCValueType ty = CC_TYPE_U64;
            if (i < fn->param_count) {
                ty = fn->param_types[i];
            } else if (i < arg_count) {
                ty = args[i].type;
            }
            size_t sz = vm_type_size(ty);
            total = (total + 7u) & ~7u;
            param_offsets[i] = total;
            total += sz;
        }
        param_mem = (uint8_t *)calloc(total ? total : 1, 1);
        if (!param_mem) {
            free(param_offsets);
            free(local_mem);
            free(local_offsets);
            return -1;
        }
        size_t copy_count = arg_count < param_slot_count ? arg_count : param_slot_count;
        for (size_t i = 0; i < copy_count; ++i) {
            CCValueType ty = (i < fn->param_count) ? fn->param_types[i] : args[i].type;
            store_indirect_value((uintptr_t)(param_mem + param_offsets[i]),
                                 ty, args[i]);
        }
    }

    VMValue *stack = (VMValue *)calloc(8192, sizeof(VMValue));
    if (!stack) {
        free(param_mem);
        free(param_offsets);
        free(local_mem);
        free(local_offsets);
        return -1;
    }
    size_t sp = 0;
    void **stack_alloc_ptrs = NULL;
    size_t stack_alloc_count = 0;
    size_t stack_alloc_cap = 0;

    LabelMap *labels = NULL;
    size_t label_count = 0;
    if (build_labels(fn, &labels, &label_count) != 0) {
        free(stack);
        free(param_mem);
        free(param_offsets);
        free(local_mem);
        free(local_offsets);
        return -1;
    }

    size_t current_ip = 0;
    for (size_t ip = 0; ip < fn->instruction_count; ++ip) {
        current_ip = ip;
        const CCInstruction *ins = &fn->instructions[ip];

        switch (ins->kind) {
        case CC_INSTR_CONST: {
            VMValue v;
            memset(&v, 0, sizeof(v));
            v.type = ins->data.constant.type;
            v.is_unsigned = ins->data.constant.is_unsigned;
            if (v.type == CC_TYPE_F32 || v.type == CC_TYPE_F64) {
                double f = (v.type == CC_TYPE_F32)
                               ? (double)ins->data.constant.value.f32
                               : ins->data.constant.value.f64;
                v.bits = f64_to_bits(f);
                v.type = CC_TYPE_F64;
            } else if (ins->data.constant.is_null) {
                v.bits = 0;
                v.type = CC_TYPE_PTR;
            } else {
                v.bits = ins->data.constant.is_unsigned
                             ? ins->data.constant.value.u64
                             : (uint64_t)ins->data.constant.value.i64;
            }
            stack[sp++] = v;
            break;
        }
        case CC_INSTR_CONST_STRING: {
            VMValue v;
            memset(&v, 0, sizeof(v));
            char *s = (char *)malloc(ins->data.const_string.length + 1);
            if (!s) {
                free(labels);
                free(stack);
                free(param_mem);
                free(param_offsets);
                free(local_mem);
                free(local_offsets);
                return -1;
            }
            memcpy(s, ins->data.const_string.bytes, ins->data.const_string.length);
            s[ins->data.const_string.length] = '\0';
            v.type = CC_TYPE_PTR;
            v.bits = (uint64_t)(uintptr_t)s;
            v.is_unsigned = 1;
            stack[sp++] = v;
            break;
        }
        case CC_INSTR_LOAD_PARAM: {
            uint32_t idx = ins->data.param.index;
            if (idx >= param_slot_count || !param_mem) {
                fprintf(stderr, "cvm: load_param out of range in %s\n", fn->name);
                goto fail;
            }
            VMValue v = load_indirect_value((uintptr_t)(param_mem + param_offsets[idx]),
                                            ins->data.param.type,
                                            vm_type_is_unsigned(ins->data.param.type));
            stack[sp++] = v;
            break;
        }
        case CC_INSTR_ADDR_PARAM: {
            uint32_t idx = ins->data.param.index;
            if (idx >= param_slot_count || !param_mem) {
                fprintf(stderr, "cvm: addr_param out of range in %s\n", fn->name);
                goto fail;
            }
            VMValue ptrv;
            memset(&ptrv, 0, sizeof(ptrv));
            ptrv.type = CC_TYPE_PTR;
            ptrv.is_unsigned = 1;
            ptrv.bits = (uint64_t)(uintptr_t)(param_mem + param_offsets[idx]);
            stack[sp++] = ptrv;
            break;
        }
        case CC_INSTR_LOAD_LOCAL: {
            uint32_t idx = ins->data.local.index;
            if (idx >= fn->local_count || !local_mem) {
                fprintf(stderr, "cvm: load_local out of range in %s\n", fn->name);
                goto fail;
            }
            VMValue v = load_indirect_value((uintptr_t)(local_mem + local_offsets[idx]),
                                            fn->local_types[idx],
                                            vm_type_is_unsigned(fn->local_types[idx]));
            stack[sp++] = v;
            break;
        }
        case CC_INSTR_STORE_LOCAL: {
            uint32_t idx = ins->data.local.index;
            if (idx >= fn->local_count || sp == 0 || !local_mem) {
                fprintf(stderr, "cvm: store_local invalid in %s\n", fn->name);
                goto fail;
            }
            VMValue in = stack[--sp];
            store_indirect_value((uintptr_t)(local_mem + local_offsets[idx]),
                                 fn->local_types[idx], in);
            break;
        }
        case CC_INSTR_ADDR_LOCAL: {
            uint32_t idx = ins->data.local.index;
            if (idx >= fn->local_count || !local_mem) {
                fprintf(stderr, "cvm: addr_local out of range in %s\n", fn->name);
                goto fail;
            }
            VMValue ptrv;
            memset(&ptrv, 0, sizeof(ptrv));
            ptrv.type = CC_TYPE_PTR;
            ptrv.is_unsigned = 1;
            ptrv.bits = (uint64_t)(uintptr_t)(local_mem + local_offsets[idx]);
            stack[sp++] = ptrv;
            break;
        }
        case CC_INSTR_LOAD_GLOBAL: {
            GlobalSlot *g = find_global_slot(vm, ins->data.global.symbol);
            if (!g) {
                RuntimeModule *fn_owner = NULL;
                const CCFunction *fn_ptr = find_function(vm, ins->data.global.symbol, &fn_owner);
                if (!fn_ptr) {
                    fprintf(stderr, "cvm: unknown global '%s'\n", ins->data.global.symbol);
                    goto fail;
                }
                VMValue fv;
                memset(&fv, 0, sizeof(fv));
                fv.type = CC_TYPE_PTR;
                fv.is_unsigned = 1;
                fv.bits = (uint64_t)(uintptr_t)fn_ptr;
                stack[sp++] = fv;
                break;
            }
            stack[sp++] = g->value;
            break;
        }
        case CC_INSTR_STORE_GLOBAL: {
            GlobalSlot *g = find_global_slot(vm, ins->data.global.symbol);
            if (!g || sp == 0) {
                fprintf(stderr, "cvm: invalid store_global '%s'\n", ins->data.global.symbol);
                goto fail;
            }
            g->value = stack[--sp];
            break;
        }
        case CC_INSTR_ADDR_GLOBAL: {
            GlobalSlot *g = find_global_slot(vm, ins->data.global.symbol);
            if (!g) {
                RuntimeModule *fn_owner = NULL;
                const CCFunction *fn_ptr = find_function(vm, ins->data.global.symbol, &fn_owner);
                if (!fn_ptr) {
                    fprintf(stderr, "cvm: unknown global '%s'\n", ins->data.global.symbol);
                    goto fail;
                }
                VMValue ptrv;
                memset(&ptrv, 0, sizeof(ptrv));
                ptrv.type = CC_TYPE_PTR;
                ptrv.is_unsigned = 1;
                ptrv.bits = (uint64_t)(uintptr_t)fn_ptr;
                stack[sp++] = ptrv;
                break;
            }
            VMValue ptrv;
            memset(&ptrv, 0, sizeof(ptrv));
            ptrv.type = CC_TYPE_PTR;
            ptrv.is_unsigned = 1;
            ptrv.bits = (uint64_t)(uintptr_t)&g->value.bits;
            stack[sp++] = ptrv;
            break;
        }
        case CC_INSTR_LOAD_INDIRECT: {
            if (sp < 1) {
                goto fail;
            }
            VMValue addr = stack[--sp];
            uintptr_t p = (uintptr_t)as_u64(addr);
            if (!p) {
                fprintf(stderr, "cvm: null pointer load in %s\n", fn->name);
                goto fail;
            }
            VMValue out = load_indirect_value(p, ins->data.memory.type,
                                              ins->data.memory.is_unsigned);
            stack[sp++] = out;
            break;
        }
        case CC_INSTR_STORE_INDIRECT: {
            if (sp < 2) {
                goto fail;
            }
            VMValue in = stack[--sp];
            VMValue addr = stack[--sp];
            uintptr_t p = (uintptr_t)as_u64(addr);
            if (!p) {
                fprintf(stderr, "cvm: null pointer store in %s\n", fn->name);
                goto fail;
            }
            store_indirect_value(p, ins->data.memory.type, in);
            break;
        }
        case CC_INSTR_BINOP: {
            if (sp < 2) {
                goto fail;
            }
            VMValue rhs = stack[--sp];
            VMValue lhs = stack[--sp];
            VMValue out;
            memset(&out, 0, sizeof(out));
            out.type = lhs.type;
            out.is_unsigned = ins->data.binop.is_unsigned;

            if (lhs.type == CC_TYPE_F64 || rhs.type == CC_TYPE_F64) {
                double a = bits_to_f64(lhs.bits);
                double b = bits_to_f64(rhs.bits);
                double r = 0.0;
                if (ins->data.binop.type == CC_TYPE_F32) {
                    float af = (float)a;
                    float bf = (float)b;
                    float rf = 0.0f;
                    switch (ins->data.binop.op) {
                    case CC_BINOP_ADD:
                        rf = af + bf;
                        break;
                    case CC_BINOP_SUB:
                        rf = af - bf;
                        break;
                    case CC_BINOP_MUL:
                        rf = af * bf;
                        break;
                    case CC_BINOP_DIV:
                        rf = af / bf;
                        break;
                    default:
                        fprintf(stderr, "cvm: unsupported float binop in %s\n", fn->name);
                        goto fail;
                    }
                    r = (double)rf;
                } else {
                    switch (ins->data.binop.op) {
                    case CC_BINOP_ADD:
                        r = a + b;
                        break;
                    case CC_BINOP_SUB:
                        r = a - b;
                        break;
                    case CC_BINOP_MUL:
                        r = a * b;
                        break;
                    case CC_BINOP_DIV:
                        r = a / b;
                        break;
                    default:
                        fprintf(stderr, "cvm: unsupported float binop in %s\n", fn->name);
                        goto fail;
                    }
                }
                out.type = CC_TYPE_F64;
                out.bits = f64_to_bits(r);
            } else {
                uint64_t a = as_u64(lhs);
                uint64_t b = as_u64(rhs);
                uint64_t r = 0;
                switch (ins->data.binop.op) {
                case CC_BINOP_ADD:
                    r = a + b;
                    break;
                case CC_BINOP_SUB:
                    r = a - b;
                    break;
                case CC_BINOP_MUL:
                    r = a * b;
                    break;
                case CC_BINOP_DIV:
                    r = b ? (ins->data.binop.is_unsigned ? (a / b) : (uint64_t)((int64_t)a / (int64_t)b)) : 0;
                    break;
                case CC_BINOP_MOD:
                    r = b ? (ins->data.binop.is_unsigned ? (a % b) : (uint64_t)((int64_t)a % (int64_t)b)) : 0;
                    break;
                case CC_BINOP_AND:
                    r = a & b;
                    break;
                case CC_BINOP_OR:
                    r = a | b;
                    break;
                case CC_BINOP_XOR:
                    r = a ^ b;
                    break;
                case CC_BINOP_SHL:
                    r = a << (b & 63u);
                    break;
                case CC_BINOP_SHR:
                    r = ins->data.binop.is_unsigned ? (a >> (b & 63u)) : (uint64_t)((int64_t)a >> (b & 63u));
                    break;
                }
                out.bits = r;
            }
            stack[sp++] = out;
            break;
        }
        case CC_INSTR_COMPARE: {
            if (sp < 2) {
                goto fail;
            }
            VMValue rhs = stack[--sp];
            VMValue lhs = stack[--sp];
            int result = 0;
            if (lhs.type == CC_TYPE_F64 || rhs.type == CC_TYPE_F64) {
                double a = bits_to_f64(lhs.bits);
                double b = bits_to_f64(rhs.bits);
                switch (ins->data.compare.op) {
                case CC_COMPARE_EQ:
                    result = (a == b);
                    break;
                case CC_COMPARE_NE:
                    result = (a != b);
                    break;
                case CC_COMPARE_LT:
                    result = (a < b);
                    break;
                case CC_COMPARE_LE:
                    result = (a <= b);
                    break;
                case CC_COMPARE_GT:
                    result = (a > b);
                    break;
                case CC_COMPARE_GE:
                    result = (a >= b);
                    break;
                }
            } else {
                uint64_t a = as_u64(lhs);
                uint64_t b = as_u64(rhs);
                unsigned cmp_bits = vm_type_bit_width(ins->data.compare.type);
                if (cmp_bits > 0 && cmp_bits < 64) {
                    uint64_t mask = vm_mask_for_bits(cmp_bits);
                    a &= mask;
                    b &= mask;
                }
                switch (ins->data.compare.op) {
                case CC_COMPARE_EQ:
                    result = (a == b);
                    break;
                case CC_COMPARE_NE:
                    result = (a != b);
                    break;
                case CC_COMPARE_LT:
                    if (ins->data.compare.is_unsigned) {
                        result = (a < b);
                    } else {
                        int64_t as = vm_sign_extend_bits(a, cmp_bits ? cmp_bits : 64);
                        int64_t bs = vm_sign_extend_bits(b, cmp_bits ? cmp_bits : 64);
                        result = (as < bs);
                    }
                    break;
                case CC_COMPARE_LE:
                    if (ins->data.compare.is_unsigned) {
                        result = (a <= b);
                    } else {
                        int64_t as = vm_sign_extend_bits(a, cmp_bits ? cmp_bits : 64);
                        int64_t bs = vm_sign_extend_bits(b, cmp_bits ? cmp_bits : 64);
                        result = (as <= bs);
                    }
                    break;
                case CC_COMPARE_GT:
                    if (ins->data.compare.is_unsigned) {
                        result = (a > b);
                    } else {
                        int64_t as = vm_sign_extend_bits(a, cmp_bits ? cmp_bits : 64);
                        int64_t bs = vm_sign_extend_bits(b, cmp_bits ? cmp_bits : 64);
                        result = (as > bs);
                    }
                    break;
                case CC_COMPARE_GE:
                    if (ins->data.compare.is_unsigned) {
                        result = (a >= b);
                    } else {
                        int64_t as = vm_sign_extend_bits(a, cmp_bits ? cmp_bits : 64);
                        int64_t bs = vm_sign_extend_bits(b, cmp_bits ? cmp_bits : 64);
                        result = (as >= bs);
                    }
                    break;
                }
            }
            VMValue out;
            out.bits = (uint64_t)(result ? 1 : 0);
            out.type = CC_TYPE_I1;
            out.is_unsigned = 1;
            stack[sp++] = out;
            break;
        }
        case CC_INSTR_CONVERT: {
            if (sp < 1) {
                goto fail;
            }
            VMValue in = stack[--sp];
            VMValue out;
            memset(&out, 0, sizeof(out));

            CCValueType from = ins->data.convert.from_type;
            CCValueType to = ins->data.convert.to_type;
            unsigned from_bits = vm_type_bit_width(from);
            unsigned to_bits = vm_type_bit_width(to);

            out.type = to;
            out.is_unsigned = vm_type_is_unsigned(to);

            switch (ins->data.convert.kind) {
            case CC_CONVERT_TRUNC: {
                uint64_t raw = as_u64(in);
                if (from_bits > 0 && from_bits < 64) {
                    raw &= vm_mask_for_bits(from_bits);
                }
                if (to_bits > 0 && to_bits < 64) {
                    raw &= vm_mask_for_bits(to_bits);
                }
                out.bits = raw;
                break;
            }
            case CC_CONVERT_SEXT: {
                uint64_t raw = as_u64(in);
                if (from_bits > 0 && from_bits < 64) {
                    raw &= vm_mask_for_bits(from_bits);
                }
                int64_t ext = vm_sign_extend_bits(raw, from_bits ? from_bits : 64);
                uint64_t result = (uint64_t)ext;
                if (to_bits > 0 && to_bits < 64) {
                    result &= vm_mask_for_bits(to_bits);
                }
                out.bits = result;
                out.is_unsigned = 0;
                break;
            }
            case CC_CONVERT_ZEXT: {
                uint64_t raw = as_u64(in);
                if (from_bits > 0 && from_bits < 64) {
                    raw &= vm_mask_for_bits(from_bits);
                }
                if (to_bits > 0 && to_bits < 64) {
                    raw &= vm_mask_for_bits(to_bits);
                }
                out.bits = raw;
                out.is_unsigned = 1;
                break;
            }
            case CC_CONVERT_F2I: {
                double d = vm_value_as_double(in, from);
                uint64_t result;
                if (vm_type_is_unsigned(to)) {
                    result = (uint64_t)d;
                } else {
                    int64_t s = (int64_t)d;
                    result = (uint64_t)s;
                }
                if (to_bits > 0 && to_bits < 64) {
                    result &= vm_mask_for_bits(to_bits);
                }
                out.bits = result;
                break;
            }
            case CC_CONVERT_I2F: {
                uint64_t raw = as_u64(in);
                if (from_bits > 0 && from_bits < 64) {
                    raw &= vm_mask_for_bits(from_bits);
                }
                double d;
                if (vm_type_is_unsigned(from)) {
                    d = (double)raw;
                } else {
                    d = (double)vm_sign_extend_bits(raw, from_bits ? from_bits : 64);
                }
                if (to == CC_TYPE_F32) {
                    float f = (float)d;
                    out.bits = f64_to_bits((double)f);
                } else {
                    out.bits = f64_to_bits(d);
                }
                out.type = CC_TYPE_F64;
                out.is_unsigned = 0;
                break;
            }
            case CC_CONVERT_BITCAST: {
                if (from == to) {
                    out = in;
                    break;
                }

                if (cc_value_type_is_float(from) && cc_value_type_is_float(to)) {
                    if (from == CC_TYPE_F32 && to == CC_TYPE_F64) {
                        float f = (float)vm_value_as_double(in, CC_TYPE_F32);
                        out.bits = f64_to_bits((double)f);
                        out.type = CC_TYPE_F64;
                    } else if (from == CC_TYPE_F64 && to == CC_TYPE_F32) {
                        float f = (float)vm_value_as_double(in, CC_TYPE_F64);
                        out.bits = f64_to_bits((double)f);
                        out.type = CC_TYPE_F64;
                    }
                    break;
                }

                if (cc_value_type_is_float(from) && !cc_value_type_is_float(to)) {
                    uint64_t raw = 0;
                    if (from == CC_TYPE_F32) {
                        union {
                            float f;
                            uint32_t u;
                        } f32;
                        f32.f = (float)vm_value_as_double(in, CC_TYPE_F32);
                        raw = f32.u;
                    } else {
                        union {
                            double d;
                            uint64_t u;
                        } f64;
                        f64.d = vm_value_as_double(in, CC_TYPE_F64);
                        raw = f64.u;
                    }
                    if (to_bits > 0 && to_bits < 64) {
                        raw &= vm_mask_for_bits(to_bits);
                    }
                    out.bits = raw;
                    break;
                }

                if (!cc_value_type_is_float(from) && cc_value_type_is_float(to)) {
                    uint64_t raw = as_u64(in);
                    if (from_bits > 0 && from_bits < 64) {
                        raw &= vm_mask_for_bits(from_bits);
                    }
                    if (to == CC_TYPE_F32) {
                        union {
                            uint32_t u;
                            float f;
                        } f32;
                        f32.u = (uint32_t)(raw & 0xffffffffu);
                        out.bits = f64_to_bits((double)f32.f);
                    } else {
                        union {
                            uint64_t u;
                            double d;
                        } f64;
                        f64.u = raw;
                        out.bits = f64_to_bits(f64.d);
                    }
                    out.type = CC_TYPE_F64;
                    out.is_unsigned = 0;
                    break;
                }

                {
                    uint64_t raw = as_u64(in);
                    if (from_bits > 0 && from_bits < 64) {
                        raw &= vm_mask_for_bits(from_bits);
                    }
                    if (to_bits > 0 && to_bits < 64) {
                        raw &= vm_mask_for_bits(to_bits);
                    }
                    out.bits = raw;
                }
                break;
            }
            default:
                out = in;
                out.type = to;
                out.is_unsigned = vm_type_is_unsigned(to);
                break;
            }
            stack[sp++] = out;
            break;
        }
        case CC_INSTR_UNOP: {
            if (sp < 1) {
                goto fail;
            }
            VMValue in = stack[--sp];
            VMValue out = in;
            switch (ins->data.unop.op) {
            case CC_UNOP_NEG:
                if (in.type == CC_TYPE_F64) {
                    out.type = CC_TYPE_F64;
                    out.bits = f64_to_bits(-bits_to_f64(in.bits));
                } else {
                    out.bits = (uint64_t)(-(int64_t)as_i64(in));
                }
                break;
            case CC_UNOP_NOT:
                out.type = CC_TYPE_I1;
                out.is_unsigned = 1;
                out.bits = as_u64(in) ? 0u : 1u;
                break;
            case CC_UNOP_BITNOT:
                out.bits = ~as_u64(in);
                break;
            }
            stack[sp++] = out;
            break;
        }
        case CC_INSTR_STACK_ALLOC: {
            size_t n = (size_t)ins->data.stack_alloc.size_bytes;
            if (n == 0)
                n = 1;
            void *mem = calloc(1, n);
            if (!mem) {
                goto fail;
            }
            if (stack_alloc_count == stack_alloc_cap) {
                size_t next_cap = stack_alloc_cap ? (stack_alloc_cap * 2) : 8;
                void **grown = (void **)realloc(stack_alloc_ptrs,
                                                next_cap * sizeof(void *));
                if (!grown) {
                    free(mem);
                    goto fail;
                }
                stack_alloc_ptrs = grown;
                stack_alloc_cap = next_cap;
            }
            stack_alloc_ptrs[stack_alloc_count++] = mem;
            VMValue ptrv;
            memset(&ptrv, 0, sizeof(ptrv));
            ptrv.type = CC_TYPE_PTR;
            ptrv.is_unsigned = 1;
            ptrv.bits = (uint64_t)(uintptr_t)mem;
            stack[sp++] = ptrv;
            break;
        }
        case CC_INSTR_DUP: {
            if (sp < 1) {
                goto fail;
            }
            stack[sp] = stack[sp - 1];
            ++sp;
            break;
        }
        case CC_INSTR_DROP: {
            if (sp < 1) {
                goto fail;
            }
            --sp;
            break;
        }
        case CC_INSTR_LABEL:
            break;
        case CC_INSTR_JUMP: {
            size_t target = 0;
            if (label_index(labels, label_count, ins->data.jump.target, &target) != 0) {
                fprintf(stderr, "cvm: unknown label '%s'\n", ins->data.jump.target);
                goto fail;
            }
            if (owner && fn && target <= ip) {
                JitProfileEntry *entry = jit_profile_get(vm, owner, fn, 0);
                if (entry) {
                    entry->loop_backedges += 1;
                    jit_maybe_compile_hot(vm, owner, fn, entry, "loop backedge threshold");
                }
            }
            ip = target;
            break;
        }
        case CC_INSTR_BRANCH: {
            if (sp < 1) {
                goto fail;
            }
            VMValue cond = stack[--sp];
            const char *dst = cond.bits ? ins->data.branch.true_target : ins->data.branch.false_target;
            size_t target = 0;
            if (label_index(labels, label_count, dst, &target) != 0) {
                fprintf(stderr, "cvm: unknown label '%s'\n", dst);
                goto fail;
            }
            if (owner && fn && target <= ip) {
                JitProfileEntry *entry = jit_profile_get(vm, owner, fn, 0);
                if (entry) {
                    entry->loop_backedges += 1;
                    jit_maybe_compile_hot(vm, owner, fn, entry, "loop backedge threshold");
                }
            }
            ip = target;
            break;
        }
        case CC_INSTR_TEST_NULL: {
            if (sp < 1) {
                goto fail;
            }
            VMValue in = stack[--sp];
            VMValue out;
            memset(&out, 0, sizeof(out));
            out.type = CC_TYPE_I1;
            out.is_unsigned = 1;
            out.bits = (as_u64(in) == 0) ? 1u : 0u;
            stack[sp++] = out;
            break;
        }
        case CC_INSTR_CALL: {
            size_t argc = ins->data.call.arg_count;
            if (sp < argc) {
                goto fail;
            }
            VMValue *argv = (VMValue *)calloc(argc ? argc : 1, sizeof(VMValue));
            if (!argv) {
                goto fail;
            }
            for (size_t i = 0; i < argc; ++i) {
                argv[argc - 1 - i] = stack[--sp];
            }

            uint64_t builtin_raw = 0;
            int handled_builtin = call_builtin_symbol(vm, ins->data.call.symbol, argv, argc, &builtin_raw);
            if (handled_builtin) {
                free(argv);
                if (ins->data.call.return_type != CC_TYPE_VOID) {
                    VMValue retv;
                    retv.bits = builtin_raw;
                    retv.type = ins->data.call.return_type;
                    retv.is_unsigned = 0;
                    stack[sp++] = retv;
                }
                break;
            }

            RuntimeModule *target_owner = NULL;
            const CCFunction *callee = NULL;
            int prefer_extern = 0;
            if (ins->data.call.symbol) {
                callee = find_function_exact(vm, ins->data.call.symbol, &target_owner);
                if (!callee) {
                    prefer_extern = vm_declares_extern_exact(vm, ins->data.call.symbol);
                }
            }
            if (!callee) {
                if (!prefer_extern) {
                    callee = find_function(vm, ins->data.call.symbol, &target_owner);
                }
            }
            if (callee) {
                int used_native = 0;
                if (vm->jit_mode != 0) {
                    JitNativeEntry *native = jit_native_get(vm, target_owner, callee, 0);
                    if (native && native->fn_ptr) {
                        VMValue native_ret;
                        if (jit_call_native(callee, native->fn_ptr, argv, argc, &native_ret) == 0) {
                            used_native = 1;
                            free(argv);
                            if (ins->data.call.return_type != CC_TYPE_VOID) {
                                stack[sp++] = native_ret;
                            }
                        }
                    }
                }
                if (used_native) {
                    break;
                }

                VMValue retv;
                int call_has_ret = 0;
                int rc = execute_function(vm, target_owner, callee, argv, argc, &retv, &call_has_ret);
                free(argv);
                if (rc != 0) {
                    goto fail;
                }
                if (ins->data.call.return_type != CC_TYPE_VOID) {
                    if (call_has_ret) {
                        stack[sp++] = retv;
                    } else {
                        VMValue defv;
                        memset(&defv, 0, sizeof(defv));
                        defv.type = ins->data.call.return_type;
                        defv.is_unsigned = vm_type_is_unsigned(ins->data.call.return_type);
                        stack[sp++] = defv;
                    }
                }
            } else {
                uint64_t *call_args = vm_make_call_args(argv, argc);
                if (argc > 0 && !call_args) {
                    free(argv);
                    goto fail;
                }

                uint64_t raw = 0;
                void *sym = resolve_extern_symbol(vm, ins->data.call.symbol);
                if (!sym) {
                    free(call_args);
                    free(argv);
                    fprintf(stderr, "cvm: unresolved extern '%s'\n", ins->data.call.symbol);
                    goto fail;
                }
                raw = ins->data.call.is_varargs
                          ? call_c_u64_varargs(sym, call_args, argc)
                          : call_c_u64(sym, call_args, argc);
                free(call_args);
                free(argv);
                if (ins->data.call.return_type != CC_TYPE_VOID) {
                    VMValue retv;
                    retv.bits = raw;
                    retv.type = ins->data.call.return_type;
                    retv.is_unsigned = 0;
                    stack[sp++] = retv;
                }
            }
            break;
        }
        case CC_INSTR_CALL_INDIRECT: {
            size_t argc = ins->data.call.arg_count;
            if (sp < argc + 1) {
                goto fail;
            }

            VMValue callee_ptr = stack[--sp];
            uintptr_t target_ptr = (uintptr_t)as_u64(callee_ptr);

            VMValue *argv = (VMValue *)calloc(argc ? argc : 1, sizeof(VMValue));
            if (!argv) {
                goto fail;
            }
            for (size_t i = 0; i < argc; ++i) {
                argv[argc - 1 - i] = stack[--sp];
            }

            RuntimeModule *target_owner = NULL;
            const CCFunction *callee = find_function_by_ptr(vm, target_ptr, &target_owner);
            if (callee) {
                VMValue retv;
                int call_has_ret = 0;
                int rc = execute_function(vm, target_owner, callee, argv, argc, &retv, &call_has_ret);
                free(argv);
                if (rc != 0) {
                    goto fail;
                }
                if (ins->data.call.return_type != CC_TYPE_VOID) {
                    if (call_has_ret) {
                        stack[sp++] = retv;
                    } else {
                        VMValue defv;
                        memset(&defv, 0, sizeof(defv));
                        defv.type = ins->data.call.return_type;
                        defv.is_unsigned = vm_type_is_unsigned(ins->data.call.return_type);
                        stack[sp++] = defv;
                    }
                }
            } else {
                uint64_t *call_args = vm_make_call_args(argv, argc);
                if (target_ptr == 0 || (argc > 0 && !call_args)) {
                    free(call_args);
                    free(argv);
                    fprintf(stderr, "cvm: invalid indirect call target in %s\n", fn->name);
                    goto fail;
                }
                uint64_t raw = ins->data.call.is_varargs
                                   ? call_c_u64_varargs((void *)target_ptr, call_args, argc)
                                   : call_c_u64((void *)target_ptr, call_args, argc);
                free(call_args);
                free(argv);
                if (ins->data.call.return_type != CC_TYPE_VOID) {
                    VMValue retv;
                    retv.bits = raw;
                    retv.type = ins->data.call.return_type;
                    retv.is_unsigned = 0;
                    stack[sp++] = retv;
                }
            }
            break;
        }
        case CC_INSTR_JUMP_INDIRECT:
            fprintf(stderr, "cvm: jump_indirect not yet supported in %s\n", fn->name);
            goto fail;
        case CC_INSTR_RET: {
            uintptr_t escaped_stack_alloc = 0;
            if (ins->data.ret.has_value) {
                if (sp < 1) {
                    goto fail;
                }
                *out_ret = stack[--sp];
                *has_ret = 1;
                escaped_stack_alloc = (uintptr_t)out_ret->bits;
                if (escaped_stack_alloc != 0 && stack_alloc_ptrs) {
                    for (size_t i = 0; i < stack_alloc_count; ++i) {
                        if ((uintptr_t)stack_alloc_ptrs[i] == escaped_stack_alloc) {
                            stack_alloc_ptrs[i] = NULL;
                            break;
                        }
                    }
                }
            } else {
                *has_ret = 0;
            }
            free(labels);
            free(stack);
            free(param_mem);
            free(param_offsets);
            free(local_mem);
            free(local_offsets);
            if (stack_alloc_ptrs) {
                for (size_t i = 0; i < stack_alloc_count; ++i)
                    free(stack_alloc_ptrs[i]);
                free(stack_alloc_ptrs);
            }
            return 0;
        }
        case CC_INSTR_COMMENT:
            break;
        default:
            fprintf(stderr, "cvm: unsupported instruction kind %d in %s\n", (int)ins->kind, fn->name);
            goto fail;
        }

        if (sp >= 8192) {
            fprintf(stderr, "cvm: stack overflow in %s\n", fn->name);
            goto fail;
        }
    }

    *has_ret = 0;
    free(labels);
    free(stack);
    free(param_mem);
    free(param_offsets);
    free(local_mem);
    free(local_offsets);
    if (stack_alloc_ptrs) {
        for (size_t i = 0; i < stack_alloc_count; ++i)
            free(stack_alloc_ptrs[i]);
        free(stack_alloc_ptrs);
    }
    return 0;

fail:
    if (fn) {
        int kind = -1;
        if (current_ip < fn->instruction_count) {
            kind = (int)fn->instructions[current_ip].kind;
        }
        fprintf(stderr, "cvm: fail in %s at ip=%zu kind=%d\n",
                fn->name ? fn->name : "<anon>",
                current_ip,
                kind);
    }
    free(labels);
    free(stack);
    free(param_mem);
    free(param_offsets);
    free(local_mem);
    free(local_offsets);
    if (stack_alloc_ptrs) {
        for (size_t i = 0; i < stack_alloc_count; ++i)
            free(stack_alloc_ptrs[i]);
        free(stack_alloc_ptrs);
    }
    return -1;
}

static int write_temp_file(const uint8_t *data, size_t size, char *out_path, size_t out_path_sz) {
#if defined(_WIN32)
    char temp_dir[MAX_PATH];
    DWORD dir_len = GetTempPathA((DWORD)sizeof(temp_dir), temp_dir);
    if (dir_len == 0 || dir_len >= sizeof(temp_dir)) {
        return -1;
    }

    char temp_file[MAX_PATH];
    if (!GetTempFileNameA(temp_dir, "cvm", 0, temp_file)) {
        return -1;
    }

    FILE *f = fopen(temp_file, "wb");
    if (!f) {
        DeleteFileA(temp_file);
        return -1;
    }
    if (size > 0 && fwrite(data, 1, size, f) != size) {
        fclose(f);
        DeleteFileA(temp_file);
        return -1;
    }
    fclose(f);

    if (strlen(temp_file) + 1 > out_path_sz) {
        DeleteFileA(temp_file);
        return -1;
    }
    strcpy(out_path, temp_file);
    return 0;
#else
    const char *tmpl = "/tmp/cvm_ccbin_XXXXXX";
    if (strlen(tmpl) + 1 > out_path_sz) {
        return -1;
    }
    strcpy(out_path, tmpl);
    int fd = mkstemp(out_path);
    if (fd < 0) {
        return -1;
    }
    size_t off = 0;
    while (off < size) {
        ssize_t n = write(fd, data + off, size - off);
        if (n <= 0) {
            close(fd);
            unlink(out_path);
            return -1;
        }
        off += (size_t)n;
    }
    close(fd);
    return 0;
#endif
}

static int load_runtime_modules(CVM *vm, const CclibFile *lib) {
    for (uint32_t i = 0; i < lib->module_count; ++i) {
        const CclibModule *cm = &lib->modules[i];
        if (!cm->ccbin_data || cm->ccbin_size == 0) {
            if (vm->verbose) {
                fprintf(stderr, "cvm: cclib module '%s' has no embedded ccbin payload\n",
                        cm->module_name ? cm->module_name : "<unnamed>");
            }
            continue;
        }

        RuntimeModule rm;
        memset(&rm, 0, sizeof(rm));
        rm.name = cm->module_name ? strdup(cm->module_name) : NULL;
        if (cm->module_name && !rm.name) {
            return -1;
        }
        cc_module_init(&rm.module, 0);
        rm.ccbin_data = NULL;
        rm.ccbin_size = 0;

        if (cm->ccbin_data && cm->ccbin_size > 0) {
            rm.ccbin_data = (uint8_t *)malloc(cm->ccbin_size);
            if (!rm.ccbin_data) {
                return -1;
            }
            memcpy(rm.ccbin_data, cm->ccbin_data, cm->ccbin_size);
            rm.ccbin_size = cm->ccbin_size;
        }

        char tmp_path[1024];
        if (write_temp_file(cm->ccbin_data, cm->ccbin_size, tmp_path, sizeof(tmp_path)) != 0) {
            fprintf(stderr, "cvm: failed to materialize ccbin for module %s\n", cm->module_name ? cm->module_name : "<unnamed>");
            return -1;
        }

        CCDiagnosticSink sink;
        cc_diag_init_default(&sink);
        if (!cc_load_file(tmp_path, &rm.module, &sink)) {
            unlink(tmp_path);
            fprintf(stderr, "cvm: failed loading module ccbin for %s\n", cm->module_name ? cm->module_name : "<unnamed>");
            cc_module_free(&rm.module);
            return -1;
        }
        unlink(tmp_path);

        rm.global_count = rm.module.global_count;
        if (rm.global_count > 0) {
            rm.globals = (GlobalSlot *)calloc(rm.global_count, sizeof(GlobalSlot));
            if (!rm.globals) {
                cc_module_free(&rm.module);
                return -1;
            }
            for (size_t g = 0; g < rm.global_count; ++g) {
                CCGlobal *src = &rm.module.globals[g];
                rm.globals[g].name = src->name;
                rm.globals[g].value.type = src->type;
                rm.globals[g].value.is_unsigned = 0;
                rm.globals[g].owns_ptr = 0;
                switch (src->init.kind) {
                case CC_GLOBAL_INIT_INT:
                    rm.globals[g].value.bits = src->init.payload.u64;
                    break;
                case CC_GLOBAL_INIT_FLOAT:
                    rm.globals[g].value.type = CC_TYPE_F64;
                    rm.globals[g].value.bits = f64_to_bits(src->init.payload.f64);
                    break;
                case CC_GLOBAL_INIT_STRING: {
                    char *s = (char *)malloc(src->init.payload.string.length + 1);
                    if (!s) {
                        cc_module_free(&rm.module);
                        free(rm.globals);
                        return -1;
                    }
                    memcpy(s, src->init.payload.string.data, src->init.payload.string.length);
                    s[src->init.payload.string.length] = '\0';
                    rm.globals[g].value.type = CC_TYPE_PTR;
                    rm.globals[g].value.bits = (uint64_t)(uintptr_t)s;
                    rm.globals[g].owns_ptr = 1;
                    break;
                }
                case CC_GLOBAL_INIT_BYTES:
                    rm.globals[g].value.type = CC_TYPE_PTR;
                    rm.globals[g].value.bits = (uint64_t)(uintptr_t)src->init.payload.bytes.data;
                    break;
                case CC_GLOBAL_INIT_NONE:
                default:
                    rm.globals[g].value.bits = 0;
                    break;
                }
            }
        }

        size_t next = vm->mod_count + 1;
        RuntimeModule *grown = (RuntimeModule *)realloc(vm->mods, next * sizeof(RuntimeModule));
        if (!grown) {
            cc_module_free(&rm.module);
            free(rm.globals);
            free((char *)rm.name);
            free(rm.ccbin_data);
            return -1;
        }
        vm->mods = grown;
        vm->mods[vm->mod_count] = rm;
        RuntimeModule *installed = &vm->mods[vm->mod_count];
        vm->mod_count = next;

        for (size_t fi = 0; fi < installed->module.function_count; ++fi) {
            const CCFunction *ccfn = &installed->module.functions[fi];
            if (ccfn->name) {
                if (add_func_alias(vm, ccfn->name, installed, ccfn) != 0) {
                    return -1;
                }
            }
        }

        for (uint32_t fi = 0; fi < cm->function_count; ++fi) {
            const CclibFunction *meta_fn = &cm->functions[fi];
            if (!meta_fn->backend_name || !meta_fn->name) {
                continue;
            }
            const CCFunction *matched = NULL;
            for (size_t cfi = 0; cfi < installed->module.function_count; ++cfi) {
                const CCFunction *ccfn = &installed->module.functions[cfi];
                if (!ccfn->name) {
                    continue;
                }
                if (symbol_equivalent(ccfn->name, meta_fn->backend_name) ||
                    symbol_equivalent(ccfn->name, meta_fn->name)) {
                    matched = ccfn;
                    break;
                }
            }

            if (matched) {
                if (add_func_alias(vm, meta_fn->backend_name, installed, matched) != 0) {
                    return -1;
                }
                if (add_func_alias(vm, meta_fn->name, installed, matched) != 0) {
                    return -1;
                }
            }
        }

        if (vm->verbose) {
            fprintf(stderr, "cvm: loaded module '%s' functions=%zu aliases=%zu\n",
                    installed->name ? installed->name : "<unnamed>",
                    installed->module.function_count,
                    vm->alias_count);
        }
    }

    return 0;
}

static int load_cclib_modules_from_file(CVM *vm, const char *path) {
    CclibFile lib;
    memset(&lib, 0, sizeof(lib));
    int err = cclib_read(path, &lib);
    if (err != 0) {
        return err;
    }

    int rc = load_runtime_modules(vm, &lib);
    cclib_free(&lib);
    return rc == 0 ? 0 : EIO;
}

static int file_exists(const char *path) {
    return path && access(path, R_OK) == 0;
}

static void path_dirname_from(const char *path, char *out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return;
    }
    out[0] = '\0';
    if (!path || !*path) {
        return;
    }

    const char *slash = strrchr(path, '/');
#if defined(_WIN32)
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) {
        slash = backslash;
    }
#endif
    if (!slash) {
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n == 0 || n + 1 >= out_sz) {
        return;
    }
    memcpy(out, path, n);
    out[n] = '\0';
}

static int get_executable_dir(const char *program_path, char *out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return -1;
    }
    out[0] = '\0';

#if defined(__APPLE__)
    uint32_t sz = (uint32_t)out_sz;
    if (_NSGetExecutablePath(out, &sz) == 0) {
        char dir[1024];
        path_dirname_from(out, dir, sizeof(dir));
        if (dir[0] != '\0') {
            strncpy(out, dir, out_sz - 1);
            out[out_sz - 1] = '\0';
            return 0;
        }
    }
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", out, out_sz - 1);
    if (n > 0) {
        out[n] = '\0';
        char dir[1024];
        path_dirname_from(out, dir, sizeof(dir));
        if (dir[0] != '\0') {
            strncpy(out, dir, out_sz - 1);
            out[out_sz - 1] = '\0';
            return 0;
        }
    }
#elif defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)out_sz);
    if (n > 0 && n < out_sz) {
        char dir[1024];
        path_dirname_from(out, dir, sizeof(dir));
        if (dir[0] != '\0') {
            strncpy(out, dir, out_sz - 1);
            out[out_sz - 1] = '\0';
            return 0;
        }
    }
#endif

    if (program_path && (strchr(program_path, '/') || strchr(program_path, '\\'))) {
        path_dirname_from(program_path, out, out_sz);
        if (out[0] != '\0') {
            return 0;
        }
    }
    return -1;
}

static int try_load_cclib_candidate(CVM *vm, const char *path, int verbose) {
    if (!file_exists(path)) {
        return 0;
    }
    int rc = load_cclib_modules_from_file(vm, path);
    if (rc != 0) {
        fprintf(stderr, "cvm: failed to load cclib '%s' (%d)\n", path, rc);
        return -1;
    }
    if (verbose) {
        fprintf(stderr, "cvm: loaded cclib '%s'\n", path);
    }
    return 0;
}

static int load_default_cclib_imports(CVM *vm, const CVMOptions *options) {
    char exe_dir[1024];
    char candidate[1200];

    if (get_executable_dir(options->program_path, exe_dir, sizeof(exe_dir)) == 0) {
        snprintf(candidate, sizeof(candidate), "%s/stdlib.cclib", exe_dir);
        if (try_load_cclib_candidate(vm, candidate, options->verbose) != 0) {
            return -1;
        }

        snprintf(candidate, sizeof(candidate), "%s/runtime.cclib", exe_dir);
        if (try_load_cclib_candidate(vm, candidate, options->verbose) != 0) {
            return -1;
        }
    }

    if (try_load_cclib_candidate(vm, "/usr/local/share/chance/stdlib/stdlib.cclib", options->verbose) != 0) {
        return -1;
    }
    if (try_load_cclib_candidate(vm, "/usr/local/share/chance/runtime/runtime.cclib", options->verbose) != 0) {
        return -1;
    }

    return 0;
}

static int resolve_tool_path(const char *env_name, const char *exe_name, char *out, size_t out_sz) {
    const char *env = getenv(env_name);
    if (env && *env) {
        if (access(env, X_OK) == 0) {
            snprintf(out, out_sz, "%s", env);
            return 0;
        }
        char candidate[1024];
        snprintf(candidate, sizeof(candidate), "%s/%s", env, exe_name);
        if (access(candidate, X_OK) == 0) {
            snprintf(out, out_sz, "%s", candidate);
            return 0;
        }
    }

    const char *common_prefixes[] = {
        "/usr/local/bin",
        "/opt/homebrew/bin",
        "/usr/bin"
    };

    for (size_t i = 0; i < (sizeof(common_prefixes) / sizeof(common_prefixes[0])); ++i) {
        char candidate[1024];
        snprintf(candidate, sizeof(candidate), "%s/%s", common_prefixes[i], exe_name);
        if (access(candidate, X_OK) == 0) {
            snprintf(out, out_sz, "%s", candidate);
            return 0;
        }
    }

    snprintf(out, out_sz, "%s", exe_name);
    return 0;
}

static int run_cmdv(const char *const *argv, int verbose) {
    if (!argv || !argv[0]) {
        return -1;
    }

    if (verbose) {
        fprintf(stderr, "cvm[jit]:");
        for (size_t i = 0; argv[i]; ++i) {
            fprintf(stderr, " %s", argv[i]);
        }
        fprintf(stderr, "\n");
    }

#if defined(_WIN32)
    intptr_t rc = _spawnvp(_P_WAIT, argv[0], argv);
    if (rc == -1) {
        return -1;
    }
    return rc == 0 ? 0 : -1;
#else
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }
    return -1;
#endif
}

static int run_cmd_capture_exit(const char *const *argv, int verbose, int *exit_code) {
    if (!argv || !argv[0] || !exit_code) {
        return -1;
    }

    if (verbose) {
        fprintf(stderr, "cvm[jit]:");
        for (size_t i = 0; argv[i]; ++i) {
            fprintf(stderr, " %s", argv[i]);
        }
        fprintf(stderr, "\n");
    }

#if defined(_WIN32)
    intptr_t rc = _spawnvp(_P_WAIT, argv[0], argv);
    if (rc == -1) {
        return -1;
    }
    *exit_code = (int)rc;
    return 0;
#else
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (WIFEXITED(status)) {
        *exit_code = WEXITSTATUS(status);
        return 0;
    }
    return -1;
#endif
}

static int locate_support_ccb(const CVMOptions *options, const char *name, char *out, size_t out_sz) {
    char exe_dir[1024];
    if (get_executable_dir(options->program_path, exe_dir, sizeof(exe_dir)) == 0) {
        snprintf(out, out_sz, "%s/%s", exe_dir, name);
        if (file_exists(out)) {
            return 0;
        }
    }

    if (strcmp(name, "stdlib.ccb") == 0) {
        snprintf(out, out_sz, "/usr/local/share/chance/stdlib/stdlib.ccb");
        if (file_exists(out)) {
            return 0;
        }
    } else if (strcmp(name, "runtime.ccb") == 0) {
        snprintf(out, out_sz, "/usr/local/share/chance/runtime/runtime.ccb");
        if (file_exists(out)) {
            return 0;
        }
    }

    for (int i = 0; i < options->cclib_import_count; ++i) {
        const char *cclib = options->cclib_imports[i];
        if (!cclib) {
            continue;
        }
        char candidate[1200];
        snprintf(candidate, sizeof(candidate), "%s", cclib);
        char *dot = strrchr(candidate, '.');
        if (dot && strcmp(dot, ".cclib") == 0) {
            strcpy(dot, ".ccb");
            if (file_exists(candidate) && strstr(candidate, name) != NULL) {
                snprintf(out, out_sz, "%s", candidate);
                return 0;
            }
        }
    }

    out[0] = '\0';
    return -1;
}

static void free_path_list(char **paths, size_t count) {
    if (!paths) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(paths[i]);
    }
    free(paths);
}

static int append_path_list(char ***out_paths, size_t *out_count, const char *path) {
    if (!out_paths || !out_count || !path || !*path) {
        return -1;
    }

    char *copy = strdup(path);
    if (!copy) {
        return -1;
    }

    size_t next = *out_count + 1;
    char **grown = (char **)realloc(*out_paths, next * sizeof(char *));
    if (!grown) {
        free(copy);
        return -1;
    }
    *out_paths = grown;
    (*out_paths)[*out_count] = copy;
    *out_count = next;
    return 0;
}

static void sanitize_name(const char *in, char *out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return;
    }
    if (!in) {
        out[0] = '\0';
        return;
    }
    size_t oi = 0;
    for (size_t i = 0; in[i] != '\0' && oi + 1 < out_sz; ++i) {
        char ch = in[i];
        if ((ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_') {
            out[oi++] = ch;
        } else {
            out[oi++] = '_';
        }
    }
    out[oi] = '\0';
}

static int locate_default_cclib_path(const CVMOptions *options, const char *name, char *out, size_t out_sz) {
    if (!options || !name || !out || out_sz == 0) {
        return -1;
    }

    char exe_dir[1024];
    if (get_executable_dir(options->program_path, exe_dir, sizeof(exe_dir)) == 0) {
        snprintf(out, out_sz, "%s/%s", exe_dir, name);
        if (file_exists(out)) {
            return 0;
        }
    }

    if (strcmp(name, "stdlib.cclib") == 0) {
        snprintf(out, out_sz, "/usr/local/share/chance/stdlib/stdlib.cclib");
        if (file_exists(out)) {
            return 0;
        }
    } else if (strcmp(name, "runtime.cclib") == 0) {
        snprintf(out, out_sz, "/usr/local/share/chance/runtime/runtime.cclib");
        if (file_exists(out)) {
            return 0;
        }
    }

    out[0] = '\0';
    return -1;
}

static int compile_cclib_modules_to_objects(const char *cclib_path,
                                            const char *tag,
                                            const char *temp_dir,
                                            const char *chancecodec_tool,
                                            const char *backend,
                                            const char *chs_tool,
                                            const char *chs_arch,
                                            const char *chs_format,
                                            int verbose,
                                            char ***out_paths,
                                            size_t *out_count) {
    if (!cclib_path || !tag || !temp_dir || !chancecodec_tool || !chs_tool || !chs_arch || !chs_format || !out_paths || !out_count) {
        return -1;
    }

    CclibFile lib;
    memset(&lib, 0, sizeof(lib));
    if (cclib_read(cclib_path, &lib) != 0) {
        return -1;
    }

    for (uint32_t mi = 0; mi < lib.module_count; ++mi) {
        const CclibModule *m = &lib.modules[mi];
        if (!m->ccbin_data || m->ccbin_size == 0) {
            continue;
        }

        char temp_ccbin[1024];
        if (write_temp_file(m->ccbin_data, m->ccbin_size, temp_ccbin, sizeof(temp_ccbin)) != 0) {
            cclib_free(&lib);
            return -1;
        }

        char mod_name[256];
        sanitize_name(m->module_name ? m->module_name : "mod", mod_name, sizeof(mod_name));

        char out_s[1400];
        char out_o[1400];
        snprintf(out_s, sizeof(out_s), "%s/%s_%u_%s.s", temp_dir, tag, mi, mod_name);
        snprintf(out_o, sizeof(out_o), "%s/%s_%u_%s.o", temp_dir, tag, mi, mod_name);

        const char *cc_argv[8];
        size_t cca = 0;
        cc_argv[cca++] = chancecodec_tool;
        cc_argv[cca++] = temp_ccbin;
        if (backend) {
            cc_argv[cca++] = "--backend";
            cc_argv[cca++] = backend;
        }
        cc_argv[cca++] = "--output";
        cc_argv[cca++] = out_s;
        cc_argv[cca] = NULL;
        if (run_cmdv(cc_argv, verbose) != 0) {
            unlink(temp_ccbin);
            cclib_free(&lib);
            return -1;
        }
        rewrite_jit_asm_symbols(out_s);

        const char *chs_argv[] = {
            chs_tool,
            "--arch",
            chs_arch,
            "--format",
            chs_format,
            "--output",
            out_o,
            out_s,
            NULL
        };
        if (run_cmdv(chs_argv, verbose) != 0) {
            unlink(temp_ccbin);
            cclib_free(&lib);
            return -1;
        }

        if (append_path_list(out_paths, out_count, out_o) != 0) {
            unlink(temp_ccbin);
            cclib_free(&lib);
            return -1;
        }

        unlink(temp_ccbin);
    }

    cclib_free(&lib);
    return 0;
}

static char *replace_all_alloc(const char *src, const char *from, const char *to, int *changed) {
    if (!src || !from || !to || !changed) {
        return NULL;
    }
    *changed = 0;

    size_t src_len = strlen(src);
    size_t from_len = strlen(from);
    size_t to_len = strlen(to);
    if (from_len == 0) {
        return strdup(src);
    }

    size_t count = 0;
    const char *p = src;
    while ((p = strstr(p, from)) != NULL) {
        if (strncmp(p, to, to_len) == 0) {
            p += from_len;
            continue;
        }
        count++;
        p += from_len;
    }

    if (count == 0) {
        return strdup(src);
    }

    size_t out_len = src_len + count * (to_len - from_len);
    char *out = (char *)malloc(out_len + 1);
    if (!out) {
        return NULL;
    }

    const char *cur = src;
    char *dst = out;
    while ((p = strstr(cur, from)) != NULL) {
        if (strncmp(p, to, to_len) == 0) {
            size_t n = (size_t)(p - cur) + from_len;
            memcpy(dst, cur, n);
            dst += n;
            cur = p + from_len;
            continue;
        }

        size_t n = (size_t)(p - cur);
        memcpy(dst, cur, n);
        dst += n;
        memcpy(dst, to, to_len);
        dst += to_len;
        cur = p + from_len;
        *changed = 1;
    }

    size_t tail = strlen(cur);
    memcpy(dst, cur, tail);
    dst += tail;
    *dst = '\0';
    return out;
}

static int rewrite_jit_asm_symbols(const char *asm_path) {
    if (!asm_path || !*asm_path) {
        return -1;
    }

    FILE *f = fopen(asm_path, "rb");
    if (!f) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    size_t len = (size_t)sz;
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    if (got != len) {
        free(buf);
        return -1;
    }
    buf[len] = '\0';

    int changed = 0;
    int pass_changed = 0;
    char *rewritten = replace_all_alloc(buf,
                                        "Std_IO_printnl_ptr_to_char",
                                        "Std_IO_printnl_ptr_to_char_varargs",
                                        &pass_changed);
    free(buf);
    if (!rewritten) {
        return -1;
    }
    if (pass_changed) {
        changed = 1;
    }

    if (changed) {
        FILE *out = fopen(asm_path, "wb");
        if (!out) {
            free(rewritten);
            return -1;
        }
        size_t out_len = strlen(rewritten);
        if (fwrite(rewritten, 1, out_len, out) != out_len) {
            fclose(out);
            free(rewritten);
            return -1;
        }
        fclose(out);
    }

    free(rewritten);
    return 0;
}

static int run_jit_from_ccbin_entry(const uint8_t *ccbin_data,
                                    size_t ccbin_size,
                                    const char *entry_symbol,
                                    int function_only,
                                    const CVMOptions *options,
                                    int *exit_code) {
#if defined(_WIN32)
    (void)ccbin_data;
    (void)ccbin_size;
    (void)entry_symbol;
    (void)function_only;
    (void)options;
    (void)exit_code;
    return -1;
#else
    if (!ccbin_data || ccbin_size == 0 || !entry_symbol || !*entry_symbol || !options || !exit_code) {
        return -1;
    }

    char temp_ccbin[1024];
    if (write_temp_file(ccbin_data, ccbin_size, temp_ccbin, sizeof(temp_ccbin)) != 0) {
        return -1;
    }

    char temp_dir[] = "/tmp/cvm_jit_XXXXXX";
    if (!mkdtemp(temp_dir)) {
        unlink(temp_ccbin);
        return -1;
    }

    char chancecodec_tool[1024];
    char chancec_tool[1024];
    char cld_tool[1024];
    char chs_tool[1024];
    resolve_tool_path("CHANCEC_HOME", "chancec", chancec_tool, sizeof(chancec_tool));
    resolve_tool_path("CHANCECODEC_HOME", "chancecodec", chancecodec_tool, sizeof(chancecodec_tool));
    resolve_tool_path("CLD_HOME", "cld", cld_tool, sizeof(cld_tool));
    resolve_tool_path("CHS_HOME", "chs", chs_tool, sizeof(chs_tool));
    if (options->verbose) {
        fprintf(stderr, "cvm[jit]: tools chancec=%s chancecodec=%s cld=%s chs=%s\n",
                chancec_tool,
                chancecodec_tool,
                cld_tool,
                chs_tool);
    }

    const char *backend = NULL;
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    backend = "arm64-macos";
#elif defined(__APPLE__) && defined(__x86_64__)
    backend = "x86-gas";
#elif defined(__linux__) && defined(__x86_64__)
    backend = "x86-gas";
#elif defined(__linux__) && defined(__aarch64__)
    backend = "arm64-elf";
#endif

    char main_s[1200];
    snprintf(main_s, sizeof(main_s), "%s/main.s", temp_dir);

    char function_opt[1152];
    const char *cc_argv[10];
    size_t cca = 0;
    cc_argv[cca++] = chancecodec_tool;
    cc_argv[cca++] = temp_ccbin;
    if (backend) {
        cc_argv[cca++] = "--backend";
        cc_argv[cca++] = backend;
    }
    if (function_only && entry_symbol && *entry_symbol) {
        snprintf(function_opt, sizeof(function_opt), "function=%s", entry_symbol);
        cc_argv[cca++] = "--option";
        cc_argv[cca++] = function_opt;
    }
    cc_argv[cca++] = "--output";
    cc_argv[cca++] = main_s;
    cc_argv[cca] = NULL;
    if (run_cmdv(cc_argv, options->verbose) != 0) {
        unlink(temp_ccbin);
        return -1;
    }
    rewrite_jit_asm_symbols(main_s);

    const char *chs_arch = NULL;
    const char *chs_format = NULL;
    const char *cld_target = NULL;
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    chs_arch = "arm64";
    chs_format = "macho";
    cld_target = "macos-arm64";
#elif defined(__linux__) && defined(__x86_64__)
    chs_arch = "x86_64";
    chs_format = "elf64";
    cld_target = "x86_64-elf";
#endif

    if (!chs_arch || !chs_format || !cld_target) {
        unlink(temp_ccbin);
        return -1;
    }

    char main_o[1200];
    snprintf(main_o, sizeof(main_o), "%s/main.o", temp_dir);

    const char *chs_main_argv[] = {
        chs_tool,
        "--arch",
        chs_arch,
        "--format",
        chs_format,
        "--output",
        main_o,
        main_s,
        NULL
    };
    if (run_cmdv(chs_main_argv, options->verbose) != 0) {
        unlink(temp_ccbin);
        return -1;
    }

    char jit_exe[1200];
    snprintf(jit_exe, sizeof(jit_exe), "%s/jit_exec", temp_dir);

    char entry_buf[1024];
    snprintf(entry_buf, sizeof(entry_buf), "%s", entry_symbol ? entry_symbol : "main");
#if defined(__APPLE__)
    if (entry_buf[0] != '_') {
        char prefixed[1024];
        if (snprintf(prefixed, sizeof(prefixed), "_%s", entry_buf) > 0) {
            snprintf(entry_buf, sizeof(entry_buf), "%s", prefixed);
        }
    }
#endif

    char **extra_obj_paths = NULL;
    size_t extra_obj_count = 0;

    char stdlib_cclib[1200];
    if (locate_default_cclib_path(options, "stdlib.cclib", stdlib_cclib, sizeof(stdlib_cclib)) == 0) {
        if (compile_cclib_modules_to_objects(stdlib_cclib,
                                             "jit_stdlib",
                                             temp_dir,
                                             chancecodec_tool,
                                             backend,
                                             chs_tool,
                                             chs_arch,
                                             chs_format,
                                             options->verbose,
                                             &extra_obj_paths,
                                             &extra_obj_count) != 0) {
            free_path_list(extra_obj_paths, extra_obj_count);
            unlink(temp_ccbin);
            return -1;
        }
    }

    char runtime_cclib[1200];
    if (locate_default_cclib_path(options, "runtime.cclib", runtime_cclib, sizeof(runtime_cclib)) == 0) {
        if (compile_cclib_modules_to_objects(runtime_cclib,
                                             "jit_runtime",
                                             temp_dir,
                                             chancecodec_tool,
                                             backend,
                                             chs_tool,
                                             chs_arch,
                                             chs_format,
                                             options->verbose,
                                             &extra_obj_paths,
                                             &extra_obj_count) != 0) {
            free_path_list(extra_obj_paths, extra_obj_count);
            unlink(temp_ccbin);
            return -1;
        }
    }

    for (int i = 0; i < options->cclib_import_count; ++i) {
        if (!options->cclib_imports[i]) {
            continue;
        }
        char tag[64];
        snprintf(tag, sizeof(tag), "jit_import_%d", i);
        if (compile_cclib_modules_to_objects(options->cclib_imports[i],
                                             tag,
                                             temp_dir,
                                             chancecodec_tool,
                                             backend,
                                             chs_tool,
                                             chs_arch,
                                             chs_format,
                                             options->verbose,
                                             &extra_obj_paths,
                                             &extra_obj_count) != 0) {
            free_path_list(extra_obj_paths, extra_obj_count);
            unlink(temp_ccbin);
            return -1;
        }
    }

    const char *cld_argv[512];
    size_t cla = 0;
    cld_argv[cla++] = cld_tool;
    cld_argv[cla++] = "link";
    cld_argv[cla++] = main_o;
    for (size_t oi = 0; oi < extra_obj_count; ++oi) {
        cld_argv[cla++] = extra_obj_paths[oi];
    }
    cld_argv[cla++] = "-o";
    cld_argv[cla++] = jit_exe;
    cld_argv[cla++] = "--target";
    cld_argv[cla++] = cld_target;
    cld_argv[cla++] = "--entry";
    cld_argv[cla++] = entry_buf;
    cld_argv[cla] = NULL;

    if (run_cmdv(cld_argv, options->verbose) != 0) {
        free_path_list(extra_obj_paths, extra_obj_count);
        unlink(temp_ccbin);
        return -1;
    }

    const char *run_argv[] = {
        jit_exe,
        NULL
    };
    if (run_cmd_capture_exit(run_argv, options->verbose, exit_code) != 0) {
        free_path_list(extra_obj_paths, extra_obj_count);
        unlink(temp_ccbin);
        return -1;
    }

    free_path_list(extra_obj_paths, extra_obj_count);
    unlink(temp_ccbin);
    return 0;
#endif
}

static int jit_function_seen(CVM *vm, const char *name) {
    if (!vm || !name || !*name) {
        return 0;
    }
    for (size_t i = 0; i < vm->jit_compiled_count; ++i) {
        if (strcmp(vm->jit_compiled_functions[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int jit_mark_function(CVM *vm, const char *name) {
    if (!vm || !name || !*name) {
        return -1;
    }
    size_t next = vm->jit_compiled_count + 1;
    char **grown = (char **)realloc(vm->jit_compiled_functions, next * sizeof(char *));
    if (!grown) {
        return -1;
    }
    vm->jit_compiled_functions = grown;
    vm->jit_compiled_functions[vm->jit_compiled_count] = strdup(name);
    if (!vm->jit_compiled_functions[vm->jit_compiled_count]) {
        return -1;
    }
    vm->jit_compiled_count = next;
    return 0;
}

static JitNativeEntry *jit_native_get(CVM *vm, RuntimeModule *owner, const CCFunction *fn, int create) {
    if (!vm || !owner || !fn) {
        return NULL;
    }
    for (size_t i = 0; i < vm->jit_native_count; ++i) {
        JitNativeEntry *entry = &vm->jit_native_entries[i];
        if (entry->owner == owner && entry->fn == fn) {
            return entry;
        }
    }
    if (!create) {
        return NULL;
    }

    size_t next = vm->jit_native_count + 1;
    JitNativeEntry *grown = (JitNativeEntry *)realloc(vm->jit_native_entries,
                                                      next * sizeof(JitNativeEntry));
    if (!grown) {
        return NULL;
    }
    vm->jit_native_entries = grown;
    vm->jit_native_entries[vm->jit_native_count].owner = owner;
    vm->jit_native_entries[vm->jit_native_count].fn = fn;
    vm->jit_native_entries[vm->jit_native_count].fn_ptr = NULL;
    vm->jit_native_count = next;
    return &vm->jit_native_entries[next - 1];
}

static int jit_call_native(const CCFunction *fn, void *fn_ptr, VMValue *argv, size_t argc, VMValue *out_ret) {
    uint64_t raw = 0;
    uint64_t call_args[8];

    if (!fn || !fn_ptr || !out_ret) {
        return -1;
    }
    if (argc > 8 || fn->is_varargs) {
        return -1;
    }
    if (fn->return_type == CC_TYPE_F32 || fn->return_type == CC_TYPE_F64) {
        return -1;
    }
    for (size_t i = 0; i < fn->param_count && i < argc; ++i) {
        if (fn->param_types[i] == CC_TYPE_F32 || fn->param_types[i] == CC_TYPE_F64) {
            return -1;
        }
    }

    for (size_t i = 0; i < argc; ++i) {
        call_args[i] = argv[i].bits;
    }

    raw = call_c_u64(fn_ptr, call_args, argc);
    out_ret->bits = raw;
    out_ret->type = fn->return_type;
    out_ret->is_unsigned = vm_type_is_unsigned(fn->return_type);
    return 0;
}

static void jit_unmark_function(CVM *vm, const char *name) {
    if (!vm || !name || !*name || !vm->jit_compiled_functions) {
        return;
    }
    for (size_t i = 0; i < vm->jit_compiled_count; ++i) {
        if (vm->jit_compiled_functions[i] && strcmp(vm->jit_compiled_functions[i], name) == 0) {
            free(vm->jit_compiled_functions[i]);
            for (size_t j = i + 1; j < vm->jit_compiled_count; ++j) {
                vm->jit_compiled_functions[j - 1] = vm->jit_compiled_functions[j];
            }
            vm->jit_compiled_count -= 1;
            if (vm->jit_compiled_count == 0) {
                free(vm->jit_compiled_functions);
                vm->jit_compiled_functions = NULL;
            } else {
                char **shrunk = (char **)realloc(vm->jit_compiled_functions,
                                                 vm->jit_compiled_count * sizeof(char *));
                if (shrunk) {
                    vm->jit_compiled_functions = shrunk;
                }
            }
            return;
        }
    }
}

static int jit_profile_cmp_desc(const void *lhs, const void *rhs) {
    const JitProfileEntry *a = (const JitProfileEntry *)lhs;
    const JitProfileEntry *b = (const JitProfileEntry *)rhs;
    uint64_t ha = a->calls + a->loop_backedges;
    uint64_t hb = b->calls + b->loop_backedges;
    if (ha < hb) {
        return 1;
    }
    if (ha > hb) {
        return -1;
    }
    return 0;
}

static void cvm_print_jit_profile_report(const CVM *vm) {
    if (!vm || vm->jit_profile_count == 0 || !vm->jit_profile_entries) {
        return;
    }

    JitProfileEntry *copy = (JitProfileEntry *)malloc(vm->jit_profile_count * sizeof(JitProfileEntry));
    if (!copy) {
        return;
    }
    memcpy(copy, vm->jit_profile_entries, vm->jit_profile_count * sizeof(JitProfileEntry));
    qsort(copy, vm->jit_profile_count, sizeof(JitProfileEntry), jit_profile_cmp_desc);

    fprintf(stderr,
            "cvm[jit-profiler]: %zu functions observed (hot=%" PRIu64 ", loop=%" PRIu64 ")\n",
            vm->jit_profile_count,
            vm->jit_hot_threshold,
            vm->jit_loop_hot_threshold);

    for (size_t i = 0; i < vm->jit_profile_count; ++i) {
        const JitProfileEntry *e = &copy[i];
        const char *name = (e->fn && e->fn->name) ? e->fn->name : "<unnamed>";
        fprintf(stderr,
                "  %-40s calls=%" PRIu64 " backedges=%" PRIu64 " compiled=%s\n",
                name,
                e->calls,
                e->loop_backedges,
                e->compiled ? "yes" : "no");
    }

    free(copy);
}

static int jit_compile_function_once(CVM *vm, RuntimeModule *owner, const CCFunction *fn) {
#if defined(_WIN32)
    (void)vm;
    (void)owner;
    (void)fn;
    return -1;
#else
    if (!vm || !owner || !fn || !fn->name || !owner->ccbin_data || owner->ccbin_size == 0 || !vm->jit_options) {
        return -1;
    }
    if (jit_function_seen(vm, fn->name)) {
        return 0;
    }

    char temp_ccbin[1024];
    if (write_temp_file(owner->ccbin_data, owner->ccbin_size, temp_ccbin, sizeof(temp_ccbin)) != 0) {
        return -1;
    }

    char temp_dir[] = "/tmp/cvm_jit_func_XXXXXX";
    if (!mkdtemp(temp_dir)) {
        unlink(temp_ccbin);
        return -1;
    }

    char chancecodec_tool[1024];
    char chs_tool[1024];
    resolve_tool_path("CHANCECODEC_HOME", "chancecodec", chancecodec_tool, sizeof(chancecodec_tool));
    resolve_tool_path("CHS_HOME", "chs", chs_tool, sizeof(chs_tool));

    const char *backend = NULL;
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    backend = "arm64-macos";
#elif defined(__APPLE__) && defined(__x86_64__)
    backend = "x86-gas";
#elif defined(__linux__) && defined(__x86_64__)
    backend = "x86-gas";
#elif defined(__linux__) && defined(__aarch64__)
    backend = "arm64-elf";
#endif

    const char *chs_arch = NULL;
    const char *chs_format = NULL;
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    chs_arch = "arm64";
    chs_format = "macho";
#elif defined(__linux__) && defined(__x86_64__)
    chs_arch = "x86_64";
    chs_format = "elf64";
#endif

    if (!backend || !chs_arch || !chs_format) {
        unlink(temp_ccbin);
        return -1;
    }

    char out_s[1200];
    char out_o[1200];
    char out_dylib[1200];
    char function_opt[1152];
    snprintf(out_s, sizeof(out_s), "%s/%s.s", temp_dir, fn->name);
    snprintf(out_o, sizeof(out_o), "%s/%s.o", temp_dir, fn->name);
    snprintf(out_dylib, sizeof(out_dylib), "%s/%s.dylib", temp_dir, fn->name);
    snprintf(function_opt, sizeof(function_opt), "function=%s", fn->name);

    const char *cc_argv[] = {
        chancecodec_tool,
        temp_ccbin,
        "--backend",
        backend,
        "--option",
        function_opt,
        "--output",
        out_s,
        NULL
    };
    if (run_cmdv(cc_argv, vm->jit_options->verbose) != 0) {
        unlink(temp_ccbin);
        return -1;
    }
    rewrite_jit_asm_symbols(out_s);

    const char *chs_argv[] = {
        chs_tool,
        "--arch",
        chs_arch,
        "--format",
        chs_format,
        "--output",
        out_o,
        out_s,
        NULL
    };
    if (run_cmdv(chs_argv, vm->jit_options->verbose) != 0) {
        unlink(temp_ccbin);
        return -1;
    }

#if defined(__APPLE__)
    const char *link_argv[] = {
        "cc",
        "-dynamiclib",
        "-Wl,-undefined,dynamic_lookup",
        "-o",
        out_dylib,
        out_o,
        NULL
    };
#else
    const char *link_argv[] = {
        "cc",
        "-shared",
        "-o",
        out_dylib,
        out_o,
        NULL
    };
#endif
    if (run_cmdv(link_argv, vm->jit_options->verbose) != 0) {
        unlink(temp_ccbin);
        return -1;
    }

    void *lib = dlopen(out_dylib, RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        unlink(temp_ccbin);
        return -1;
    }
    if (add_import_handle(vm, out_dylib, 1, lib) != 0) {
        dlclose(lib);
        unlink(temp_ccbin);
        return -1;
    }

    void *fn_ptr = dlsym(lib, fn->name);
    if (!fn_ptr) {
#if defined(__APPLE__)
        char underscored[1400];
        if (snprintf(underscored, sizeof(underscored), "_%s", fn->name) > 0) {
            fn_ptr = dlsym(lib, underscored);
        }
#endif
    }
    if (!fn_ptr) {
        unlink(temp_ccbin);
        return -1;
    }

    JitNativeEntry *native = jit_native_get(vm, owner, fn, 1);
    if (!native) {
        unlink(temp_ccbin);
        return -1;
    }
    native->fn_ptr = fn_ptr;

    if (jit_mark_function(vm, fn->name) != 0) {
        unlink(temp_ccbin);
        return -1;
    }
    unlink(temp_ccbin);
    return 0;
#endif
}

static int run_jit_from_cclib(const char *cclib_path, const CVMOptions *options, int *exit_code) {
    CclibFile lib;
    memset(&lib, 0, sizeof(lib));
    if (cclib_read(cclib_path, &lib) != 0) {
        return -1;
    }

    const char *entry = options->entry_name ? options->entry_name : "main";
    const CclibModule *main_mod = NULL;
    const CclibFunction *main_fn = NULL;
    for (uint32_t mi = 0; mi < lib.module_count && !main_mod; ++mi) {
        const CclibModule *m = &lib.modules[mi];
        if (!m->ccbin_data || m->ccbin_size == 0) {
            continue;
        }
        for (uint32_t fi = 0; fi < m->function_count; ++fi) {
            const CclibFunction *f = &m->functions[fi];
            if ((f->name && strcmp(f->name, entry) == 0) ||
                (f->backend_name && strcmp(f->backend_name, entry) == 0)) {
                main_mod = m;
                main_fn = f;
                break;
            }
        }
    }

    if (!main_mod && strcmp(entry, "main") == 0) {
        for (uint32_t mi = 0; mi < lib.module_count && !main_mod; ++mi) {
            const CclibModule *m = &lib.modules[mi];
            if (!m->ccbin_data || m->ccbin_size == 0) {
                continue;
            }
            for (uint32_t fi = 0; fi < m->function_count; ++fi) {
                const CclibFunction *f = &m->functions[fi];
                if (f->name && strcmp(f->name, "__cert__entry_main") == 0) {
                    main_mod = m;
                    main_fn = f;
                    break;
                }
            }
        }
    }

    if (!main_mod) {
        cclib_free(&lib);
        return -1;
    }

    const char *entry_symbol = (main_fn && main_fn->backend_name) ? main_fn->backend_name :
                               ((main_fn && main_fn->name) ? main_fn->name : entry);
    int rc = run_jit_from_ccbin_entry(main_mod->ccbin_data,
                                      main_mod->ccbin_size,
                                      entry_symbol ? entry_symbol : "main",
                                      0,
                                      options,
                                      exit_code);
    cclib_free(&lib);
    return rc;
}

int cvm_run_file(const char *cclib_path, const CVMOptions *options, int *exit_code) {
    if (!cclib_path || !options || !exit_code) {
        return 2;
    }

    CVM vm;
    memset(&vm, 0, sizeof(vm));
    vm.verbose = options->verbose;
    vm.jit_mode = options->jit_mode;
    vm.jit_attempted = 0;
    vm.jit_cclib_path = cclib_path;
    vm.jit_options = options;
    vm.jit_hot_threshold = (uint64_t)options->jit_hot_threshold;
    vm.jit_loop_hot_threshold = (uint64_t)options->jit_loop_hot_threshold;
    vm.jit_profile_report = options->jit_profile;

    if (vm.jit_mode == 1) {
        if (vm.jit_hot_threshold == 0 || vm.jit_hot_threshold > 8) {
            vm.jit_hot_threshold = 8;
        }
        if (vm.jit_loop_hot_threshold == 0 || vm.jit_loop_hot_threshold > 256) {
            vm.jit_loop_hot_threshold = 256;
        }
    }

    if (add_import_handle(&vm, "self", 0, dlopen(NULL, RTLD_NOW | RTLD_GLOBAL)) != 0) {
        return 2;
    }

    if (add_default_c_runtime_import(&vm) != 0) {
        free_runtime(&vm);
        return 2;
    }

    for (int i = 0; i < options->import_count; ++i) {
        void *h = dlopen(options->imports[i], RTLD_NOW | RTLD_GLOBAL);
        if (!h) {
            fprintf(stderr, "cvm: failed import '%s': %s\n", options->imports[i], dlerror());
            free_runtime(&vm);
            return 2;
        }
        if (add_import_handle(&vm, options->imports[i], 1, h) != 0) {
            dlclose(h);
            free_runtime(&vm);
            return 2;
        }
    }

    if (load_cclib_modules_from_file(&vm, cclib_path) != 0) {
        fprintf(stderr, "cvm: failed to read '%s'\n", cclib_path);
        free_runtime(&vm);
        return 2;
    }

    if (load_default_cclib_imports(&vm, options) != 0) {
        free_runtime(&vm);
        return 2;
    }

    for (int i = 0; i < options->cclib_import_count; ++i) {
        int rc = load_cclib_modules_from_file(&vm, options->cclib_imports[i]);
        if (rc != 0) {
            fprintf(stderr, "cvm: failed cclib import '%s' (%d)\n", options->cclib_imports[i], rc);
            free_runtime(&vm);
            return 2;
        }
    }

    if (vm.mod_count == 0) {
        fprintf(stderr, "cvm: no embedded ccbin modules found in %s\n", cclib_path);
        free_runtime(&vm);
        return 2;
    }

    vm.uses_runtime_profiling_controls = vm_uses_runtime_profiling_controls(&vm);
    if (vm.jit_mode == 1 && vm.uses_runtime_profiling_controls) {
        vm.jit_mode = 2;
        if (vm.verbose) {
            fprintf(stderr,
                    "cvm[jit]: runtime profiling controls detected; using lazy-jit so Std.Profiling can control hotness\n");
        }
    }

    const char *entry = options->entry_name ? options->entry_name : "main";
    RuntimeModule *owner = NULL;
    const CCFunction *main_fn = find_function(&vm, entry, &owner);
    if (!main_fn) {
        for (size_t mi = 0; mi < vm.mod_count && !main_fn; ++mi) {
            RuntimeModule *m = &vm.mods[mi];
            for (size_t fi = 0; fi < m->module.function_count; ++fi) {
                const char *n = m->module.functions[fi].name;
                size_t nn = n ? strlen(n) : 0;
                size_t en = strlen(entry);
                if (n && nn > en + 1 && strcmp(n + nn - en, entry) == 0 && n[nn - en - 1] == '.') {
                    main_fn = &m->module.functions[fi];
                    owner = m;
                    break;
                }
            }
        }
    }

    if (!main_fn) {
        fprintf(stderr, "cvm: entry '%s' not found in %s\n", entry, cclib_path);
        free_runtime(&vm);
        return 2;
    }

    VMValue retv;
    int has_ret = 0;
    if (execute_function(&vm, owner, main_fn, NULL, 0, &retv, &has_ret) != 0) {
        fprintf(stderr, "cvm: execution failed in %s\n", main_fn->name);
        free_runtime(&vm);
        return 1;
    }

    if (has_ret) {
        *exit_code = (int)(retv.bits & 0xff);
    } else {
        *exit_code = 0;
    }

    if (vm.jit_mode == 2 && (vm.jit_profile_report || vm.verbose)) {
        cvm_print_jit_profile_report(&vm);
    }

    free_runtime(&vm);
    return 0;
}
