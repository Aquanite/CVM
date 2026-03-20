#include "cclib.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CCLIB_MAGIC "CCLIB"
#define CCLIB_VERSION_MIN 1u
#define CCLIB_VERSION_MAX 2u

static int read_u8(FILE *in, uint8_t *value) {
    int ch = fgetc(in);
    if (ch == EOF) {
        return 0;
    }
    *value = (uint8_t)ch;
    return 1;
}

static int read_u32(FILE *in, uint32_t *value) {
    uint8_t buf[4];
    if (fread(buf, 1, sizeof(buf), in) != sizeof(buf)) {
        return 0;
    }
    *value = (uint32_t)buf[0] |
             ((uint32_t)buf[1] << 8) |
             ((uint32_t)buf[2] << 16) |
             ((uint32_t)buf[3] << 24);
    return 1;
}

static int read_bytes(FILE *in, uint8_t **data, uint32_t size) {
    if (size == 0) {
        *data = NULL;
        return 1;
    }
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) {
        return 0;
    }
    if (fread(buf, 1, size, in) != size) {
        free(buf);
        return 0;
    }
    *data = buf;
    return 1;
}

static int read_string(FILE *in, char **out) {
    uint8_t present = 0;
    if (!read_u8(in, &present)) {
        return 0;
    }
    if (!present) {
        *out = NULL;
        return 1;
    }

    uint32_t len = 0;
    if (!read_u32(in, &len)) {
        return 0;
    }

    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        return 0;
    }
    if (len > 0 && fread(buf, 1, len, in) != len) {
        free(buf);
        return 0;
    }
    buf[len] = '\0';
    *out = buf;
    return 1;
}

static void free_string_array(char **arr, uint32_t count) {
    if (!arr) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        free(arr[i]);
    }
    free(arr);
}

static void free_enum_values(CclibEnumValue *values, uint32_t count) {
    if (!values) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        free(values[i].name);
    }
    free(values);
}

static int read_function(FILE *in, CclibFunction *fn) {
    memset(fn, 0, sizeof(*fn));
    if (!read_string(in, &fn->name)) {
        return 0;
    }
    if (!read_string(in, &fn->backend_name)) {
        return 0;
    }
    if (!read_string(in, &fn->return_type)) {
        return 0;
    }

    uint32_t param_count = 0;
    if (!read_u32(in, &param_count)) {
        return 0;
    }
    if (param_count > 0) {
        fn->param_types = (char **)calloc(param_count, sizeof(char *));
        if (!fn->param_types) {
            return 0;
        }
        for (uint32_t i = 0; i < param_count; ++i) {
            if (!read_string(in, &fn->param_types[i])) {
                return 0;
            }
        }
    }
    fn->param_count = param_count;

    if (!read_u8(in, &fn->is_varargs)) {
        return 0;
    }
    if (!read_u8(in, &fn->is_noreturn)) {
        return 0;
    }
    if (!read_u8(in, &fn->is_exposed)) {
        return 0;
    }

    return 1;
}

static int read_struct(FILE *in, CclibStruct *st) {
    memset(st, 0, sizeof(*st));
    if (!read_string(in, &st->name)) {
        return 0;
    }

    uint32_t count = 0;
    if (!read_u32(in, &count)) {
        return 0;
    }
    st->field_count = count;

    if (count > 0) {
        st->field_names = (char **)calloc(count, sizeof(char *));
        st->field_types = (char **)calloc(count, sizeof(char *));
        st->field_defaults = (char **)calloc(count, sizeof(char *));
        st->field_offsets = (uint32_t *)calloc(count, sizeof(uint32_t));
        if (!st->field_names || !st->field_types || !st->field_defaults || !st->field_offsets) {
            return 0;
        }

        for (uint32_t i = 0; i < count; ++i) {
            if (!read_string(in, &st->field_names[i])) {
                return 0;
            }
            if (!read_string(in, &st->field_types[i])) {
                return 0;
            }
            if (!read_string(in, &st->field_defaults[i])) {
                return 0;
            }
            if (!read_u32(in, &st->field_offsets[i])) {
                return 0;
            }
        }
    }

    if (!read_u32(in, &st->size_bytes)) {
        return 0;
    }
    if (!read_u8(in, &st->is_exposed)) {
        return 0;
    }

    return 1;
}

static int read_enum(FILE *in, CclibEnum *en) {
    memset(en, 0, sizeof(*en));
    if (!read_string(in, &en->name)) {
        return 0;
    }

    uint32_t count = 0;
    if (!read_u32(in, &count)) {
        return 0;
    }

    if (count > 0) {
        en->values = (CclibEnumValue *)calloc(count, sizeof(CclibEnumValue));
        if (!en->values) {
            return 0;
        }

        for (uint32_t i = 0; i < count; ++i) {
            if (!read_string(in, &en->values[i].name)) {
                return 0;
            }
            uint32_t raw = 0;
            if (!read_u32(in, &raw)) {
                return 0;
            }
            en->values[i].value = (int32_t)raw;
        }
    }

    en->value_count = count;
    if (!read_u8(in, &en->is_exposed)) {
        return 0;
    }
    return 1;
}

static int read_global(FILE *in, CclibGlobal *gl) {
    memset(gl, 0, sizeof(*gl));
    if (!read_string(in, &gl->name)) {
        return 0;
    }
    if (!read_string(in, &gl->type_spec)) {
        return 0;
    }
    if (!read_u8(in, &gl->is_const)) {
        return 0;
    }
    return 1;
}

int cclib_read(const char *path, CclibFile *out_lib) {
    if (!path || !out_lib) {
        return EINVAL;
    }

    memset(out_lib, 0, sizeof(*out_lib));

    FILE *in = fopen(path, "rb");
    if (!in) {
        return errno ? errno : EIO;
    }

    char magic[6] = {0};
    if (fread(magic, 1, 5, in) != 5 || memcmp(magic, CCLIB_MAGIC, 5) != 0) {
        fclose(in);
        return EINVAL;
    }

    uint32_t version = 0;
    if (!read_u32(in, &version)) {
        fclose(in);
        return EIO;
    }
    if (version < CCLIB_VERSION_MIN || version > CCLIB_VERSION_MAX) {
        fclose(in);
        return EINVAL;
    }
    out_lib->format_version = version;

    uint32_t module_count = 0;
    if (!read_u32(in, &module_count)) {
        fclose(in);
        return EIO;
    }
    out_lib->module_count = module_count;

    if (module_count > 0) {
        out_lib->modules = (CclibModule *)calloc(module_count, sizeof(CclibModule));
        if (!out_lib->modules) {
            fclose(in);
            return ENOMEM;
        }
    }

    for (uint32_t mi = 0; mi < module_count; ++mi) {
        CclibModule *mod = &out_lib->modules[mi];
        memset(mod, 0, sizeof(*mod));

        if (!read_string(in, &mod->module_name)) {
            fclose(in);
            return EIO;
        }

        if (!read_u32(in, &mod->function_count)) {
            fclose(in);
            return EIO;
        }
        if (mod->function_count > 0) {
            mod->functions = (CclibFunction *)calloc(mod->function_count, sizeof(CclibFunction));
            if (!mod->functions) {
                fclose(in);
                return ENOMEM;
            }
            for (uint32_t i = 0; i < mod->function_count; ++i) {
                if (!read_function(in, &mod->functions[i])) {
                    fclose(in);
                    return EIO;
                }
            }
        }

        if (!read_u32(in, &mod->struct_count)) {
            fclose(in);
            return EIO;
        }
        if (mod->struct_count > 0) {
            mod->structs = (CclibStruct *)calloc(mod->struct_count, sizeof(CclibStruct));
            if (!mod->structs) {
                fclose(in);
                return ENOMEM;
            }
            for (uint32_t i = 0; i < mod->struct_count; ++i) {
                if (!read_struct(in, &mod->structs[i])) {
                    fclose(in);
                    return EIO;
                }
            }
        }

        if (!read_u32(in, &mod->enum_count)) {
            fclose(in);
            return EIO;
        }
        if (mod->enum_count > 0) {
            mod->enums = (CclibEnum *)calloc(mod->enum_count, sizeof(CclibEnum));
            if (!mod->enums) {
                fclose(in);
                return ENOMEM;
            }
            for (uint32_t i = 0; i < mod->enum_count; ++i) {
                if (!read_enum(in, &mod->enums[i])) {
                    fclose(in);
                    return EIO;
                }
            }
        }

        if (!read_u32(in, &mod->global_count)) {
            fclose(in);
            return EIO;
        }
        if (mod->global_count > 0) {
            mod->globals = (CclibGlobal *)calloc(mod->global_count, sizeof(CclibGlobal));
            if (!mod->globals) {
                fclose(in);
                return ENOMEM;
            }
            for (uint32_t i = 0; i < mod->global_count; ++i) {
                if (!read_global(in, &mod->globals[i])) {
                    fclose(in);
                    return EIO;
                }
            }
        }

        if (!read_u32(in, &mod->ccbin_size)) {
            fclose(in);
            return EIO;
        }
        if (!read_bytes(in, &mod->ccbin_data, mod->ccbin_size)) {
            fclose(in);
            return EIO;
        }
    }

    fclose(in);
    return 0;
}

void cclib_free(CclibFile *lib) {
    if (!lib || !lib->modules) {
        return;
    }

    for (uint32_t mi = 0; mi < lib->module_count; ++mi) {
        CclibModule *mod = &lib->modules[mi];
        free(mod->module_name);

        if (mod->functions) {
            for (uint32_t i = 0; i < mod->function_count; ++i) {
                CclibFunction *fn = &mod->functions[i];
                free(fn->name);
                free(fn->backend_name);
                free(fn->return_type);
                free_string_array(fn->param_types, fn->param_count);
            }
            free(mod->functions);
        }

        if (mod->structs) {
            for (uint32_t i = 0; i < mod->struct_count; ++i) {
                CclibStruct *st = &mod->structs[i];
                free(st->name);
                free_string_array(st->field_names, st->field_count);
                free_string_array(st->field_types, st->field_count);
                free_string_array(st->field_defaults, st->field_count);
                free(st->field_offsets);
            }
            free(mod->structs);
        }

        if (mod->enums) {
            for (uint32_t i = 0; i < mod->enum_count; ++i) {
                CclibEnum *en = &mod->enums[i];
                free(en->name);
                free_enum_values(en->values, en->value_count);
            }
            free(mod->enums);
        }

        if (mod->globals) {
            for (uint32_t i = 0; i < mod->global_count; ++i) {
                free(mod->globals[i].name);
                free(mod->globals[i].type_spec);
            }
            free(mod->globals);
        }

        free(mod->ccbin_data);
    }

    free(lib->modules);
    memset(lib, 0, sizeof(*lib));
}
