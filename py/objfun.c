#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "nlr.h"
#include "misc.h"
#include "mpconfig.h"
#include "qstr.h"
#include "obj.h"
#include "objtuple.h"
#include "runtime0.h"
#include "runtime.h"
#include "bc.h"

#if 0 // print debugging info
#define DEBUG_PRINT (1)
#else // don't print debugging info
#define DEBUG_printf(...) (void)0
#endif

/******************************************************************************/
/* native functions                                                           */

// mp_obj_fun_native_t defined in obj.h

STATIC void check_nargs(mp_obj_fun_native_t *self, int n_args, int n_kw) {
    mp_check_nargs(n_args, self->n_args_min, self->n_args_max, n_kw, self->is_kw);
}

void mp_check_nargs(int n_args, machine_uint_t n_args_min, machine_uint_t n_args_max, int n_kw, bool is_kw) {
    if (n_kw && !is_kw) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_TypeError,
                                          "function does not take keyword arguments"));
    }

    if (n_args_min == n_args_max) {
        if (n_args != n_args_min) {
            nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_TypeError,
                                                     "function takes %d positional arguments but %d were given",
                                                     n_args_min, n_args));
        }
    } else {
        if (n_args < n_args_min) {
            nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_TypeError,
                                                    "<fun name>() missing %d required positional arguments: <list of names of params>",
                                                    n_args_min - n_args));
        } else if (n_args > n_args_max) {
            nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_TypeError,
                                                     "<fun name> expected at most %d arguments, got %d",
                                                     n_args_max, n_args));
        }
    }
}

STATIC mp_obj_t fun_binary_op(int op, mp_obj_t lhs_in, mp_obj_t rhs_in) {
    switch (op) {
        case MP_BINARY_OP_EQUAL:
            // These objects can be equal only if it's the same underlying structure,
            // we don't even need to check for 2nd arg type.
            return MP_BOOL(lhs_in == rhs_in);
    }
    return NULL;
}

STATIC mp_obj_t fun_native_call(mp_obj_t self_in, uint n_args, uint n_kw, const mp_obj_t *args) {
    assert(MP_OBJ_IS_TYPE(self_in, &mp_type_fun_native));
    mp_obj_fun_native_t *self = self_in;

    // check number of arguments
    check_nargs(self, n_args, n_kw);

    if (self->is_kw) {
        // function allows keywords

        // we create a map directly from the given args array
        mp_map_t kw_args;
        mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);

        return ((mp_fun_kw_t)self->fun)(n_args, args, &kw_args);

    } else if (self->n_args_min <= 3 && self->n_args_min == self->n_args_max) {
        // function requires a fixed number of arguments

        // dispatch function call
        switch (self->n_args_min) {
            case 0:
                return ((mp_fun_0_t)self->fun)();

            case 1:
                return ((mp_fun_1_t)self->fun)(args[0]);

            case 2:
                return ((mp_fun_2_t)self->fun)(args[0], args[1]);

            case 3:
                return ((mp_fun_3_t)self->fun)(args[0], args[1], args[2]);

            default:
                assert(0);
                return mp_const_none;
        }

    } else {
        // function takes a variable number of arguments, but no keywords

        return ((mp_fun_var_t)self->fun)(n_args, args);
    }
}

const mp_obj_type_t mp_type_fun_native = {
    { &mp_type_type },
    .name = MP_QSTR_function,
    .call = fun_native_call,
    .binary_op = fun_binary_op,
};

// fun must have the correct signature for n_args fixed arguments
mp_obj_t mp_make_function_n(int n_args, void *fun) {
    mp_obj_fun_native_t *o = m_new_obj(mp_obj_fun_native_t);
    o->base.type = &mp_type_fun_native;
    o->is_kw = false;
    o->n_args_min = n_args;
    o->n_args_max = n_args;
    o->fun = fun;
    return o;
}

mp_obj_t mp_make_function_var(int n_args_min, mp_fun_var_t fun) {
    mp_obj_fun_native_t *o = m_new_obj(mp_obj_fun_native_t);
    o->base.type = &mp_type_fun_native;
    o->is_kw = false;
    o->n_args_min = n_args_min;
    o->n_args_max = MP_OBJ_FUN_ARGS_MAX;
    o->fun = fun;
    return o;
}

// min and max are inclusive
mp_obj_t mp_make_function_var_between(int n_args_min, int n_args_max, mp_fun_var_t fun) {
    mp_obj_fun_native_t *o = m_new_obj(mp_obj_fun_native_t);
    o->base.type = &mp_type_fun_native;
    o->is_kw = false;
    o->n_args_min = n_args_min;
    o->n_args_max = n_args_max;
    o->fun = fun;
    return o;
}

/******************************************************************************/
/* byte code functions                                                        */

typedef struct _mp_obj_fun_bc_t {
    mp_obj_base_t base;
    mp_obj_dict_t *globals; // the context within which this function was defined
    machine_uint_t n_args : 15;         // number of arguments this function takes
    machine_uint_t n_def_args : 15;     // number of default arguments
    machine_uint_t takes_var_args : 1;  // set if this function takes variable args
    machine_uint_t takes_kw_args : 1;   // set if this function takes keyword args
    const byte *bytecode;   // bytecode for the function
    qstr *args;             // argument names (needed to resolve positional args passed as keywords)
    mp_obj_t extra_args[];  // values of default args (if any), plus a slot at the end for var args and/or kw args (if it takes them)
} mp_obj_fun_bc_t;

#if DEBUG_PRINT
STATIC void dump_args(const mp_obj_t *a, int sz) {
    DEBUG_printf("%p: ", a);
    for (int i = 0; i < sz; i++) {
        DEBUG_printf("%p ", a[i]);
    }
    DEBUG_printf("\n");
}
#else
#define dump_args(...) (void)0
#endif

// If it's possible to call a function without allocating new argument array,
// this function returns true, together with pointers to 2 subarrays to be used
// as arguments. Otherwise, it returns false. It is expected that this fucntion
// will be accompanied by another, mp_obj_fun_prepare_full_args(), which will
// instead take pointer to full-length out-array, and will fill it in. Rationale
// being that a caller can try this function and if it succeeds, the function call
// can be made without allocating extra memory. Otherwise, caller can allocate memory
// and try "full" function. These functions are expected to be refactoring of
// code in fun_bc_call() and evenrually replace it.
bool mp_obj_fun_prepare_simple_args(mp_obj_t self_in, uint n_args, uint n_kw, const mp_obj_t *args,
                            uint *out_args1_len, const mp_obj_t **out_args1, uint *out_args2_len, const mp_obj_t **out_args2) {
    mp_obj_fun_bc_t *self = self_in;

    assert(n_kw == 0);
    assert(self->takes_var_args == 0);
    assert(self->takes_kw_args == 0);

    mp_obj_t *extra_args = self->extra_args + self->n_def_args;
    uint n_extra_args = 0;

    if (n_args > self->n_args) {
            goto arg_error;
    } else {
        extra_args -= self->n_args - n_args;
        n_extra_args += self->n_args - n_args;
    }
    *out_args1 = args;
    *out_args1_len = n_args;
    *out_args2 = extra_args;
    *out_args2_len = n_extra_args;
    return true;

arg_error:
    nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_TypeError, "function takes %d positional arguments but %d were given", self->n_args, n_args));
}

STATIC mp_obj_t fun_bc_call(mp_obj_t self_in, uint n_args, uint n_kw, const mp_obj_t *args) {
    DEBUG_printf("Input n_args: %d, n_kw: %d\n", n_args, n_kw);
    DEBUG_printf("Input pos args: ");
    dump_args(args, n_args);
    DEBUG_printf("Input kw args: ");
    dump_args(args + n_args, n_kw * 2);
    mp_obj_fun_bc_t *self = self_in;
    DEBUG_printf("Func n_def_args: %d\n", self->n_def_args);

    const mp_obj_t *kwargs = args + n_args;
    mp_obj_t *extra_args = self->extra_args + self->n_def_args;
    uint n_extra_args = 0;


    // check positional arguments

    if (n_args > self->n_args) {
        // given more than enough arguments
        if (!self->takes_var_args) {
            nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_TypeError,
                "function takes %d positional arguments but %d were given", self->n_args, n_args));
        }
        // put extra arguments in varargs tuple
        *extra_args = mp_obj_new_tuple(n_args - self->n_args, args + self->n_args);
        n_extra_args = 1;
        n_args = self->n_args;
    } else {
        if (self->takes_var_args) {
            DEBUG_printf("passing empty tuple as *args\n");
            *extra_args = mp_const_empty_tuple;
            n_extra_args = 1;
        }
        // Apply processing and check below only if we don't have kwargs,
        // otherwise, kw handling code below has own extensive checks.
        if (n_kw == 0) {
            if (n_args >= self->n_args - self->n_def_args) {
                // given enough arguments, but may need to use some default arguments
                extra_args -= self->n_args - n_args;
                n_extra_args += self->n_args - n_args;
            } else {
                nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_TypeError,
                    "function takes at least %d positional arguments but %d were given",
                    self->n_args - self->n_def_args, n_args));
            }
        }
    }

    // check keyword arguments

    if (n_kw != 0) {
        // We cannot use dynamically-sized array here, because GCC indeed
        // deallocates it on leaving defining scope (unlike most static stack allocs).
        // So, we have 2 choices: allocate it unconditionally at the top of function
        // (wastes stack), or use alloca which is guaranteed to dealloc on func exit.
        //mp_obj_t flat_args[self->n_args];
        mp_obj_t *flat_args = alloca(self->n_args * sizeof(mp_obj_t));
        for (int i = self->n_args - 1; i >= 0; i--) {
            flat_args[i] = MP_OBJ_NULL;
        }
        memcpy(flat_args, args, sizeof(*args) * n_args);
        DEBUG_printf("Initial args: ");
        dump_args(flat_args, self->n_args);

        mp_obj_t dict = MP_OBJ_NULL;
        if (self->takes_kw_args) {
            dict = mp_obj_new_dict(n_kw); // TODO: better go conservative with 0?
        }
        for (uint i = 0; i < n_kw; i++) {
            qstr arg_name = MP_OBJ_QSTR_VALUE(kwargs[2 * i]);
            for (uint j = 0; j < self->n_args; j++) {
                if (arg_name == self->args[j]) {
                    if (flat_args[j] != MP_OBJ_NULL) {
                        nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_TypeError,
                            "function got multiple values for argument '%s'", qstr_str(arg_name)));
                    }
                    flat_args[j] = kwargs[2 * i + 1];
                    goto continue2;
                }
            }
            // Didn't find name match with positional args
            if (!self->takes_kw_args) {
                nlr_raise(mp_obj_new_exception_msg(&mp_type_TypeError, "function does not take keyword arguments"));
            }
            mp_obj_dict_store(dict, kwargs[2 * i], kwargs[2 * i + 1]);
continue2:;
        }
        DEBUG_printf("Args with kws flattened: ");
        dump_args(flat_args, self->n_args);

        // Now fill in defaults
        mp_obj_t *d = &flat_args[self->n_args - 1];
        mp_obj_t *s = &self->extra_args[self->n_def_args - 1];
        for (int i = self->n_def_args; i > 0; i--, d--, s--) {
            if (*d == MP_OBJ_NULL) {
                *d = *s;
            }
        }
        DEBUG_printf("Args after filling defaults: ");
        dump_args(flat_args, self->n_args);

        // Now check that all mandatory args specified
        while (d >= flat_args) {
            if (*d-- == MP_OBJ_NULL) {
                nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_TypeError,
                    "function missing required positional argument #%d", d - flat_args));
            }
        }

        args = flat_args;
        n_args = self->n_args;

        if (self->takes_kw_args) {
            extra_args[n_extra_args] = dict;
            n_extra_args += 1;
        }
    } else {
        // no keyword arguments given
        if (self->takes_kw_args) {
            extra_args[n_extra_args] = mp_obj_new_dict(0);
            n_extra_args += 1;
        }
    }

    mp_obj_dict_t *old_globals = mp_globals_get();
    mp_globals_set(self->globals);
    mp_obj_t result;
    DEBUG_printf("Calling: args=%p, n_args=%d, extra_args=%p, n_extra_args=%d\n", args, n_args, extra_args, n_extra_args);
    dump_args(args, n_args);
    dump_args(extra_args, n_extra_args);
    mp_vm_return_kind_t vm_return_kind = mp_execute_byte_code(self->bytecode, args, n_args, extra_args, n_extra_args, &result);
    mp_globals_set(old_globals);

    if (vm_return_kind == MP_VM_RETURN_NORMAL) {
        return result;
    } else { // MP_VM_RETURN_EXCEPTION
        nlr_raise(result);
    }
}

const mp_obj_type_t mp_type_fun_bc = {
    { &mp_type_type },
    .name = MP_QSTR_function,
    .call = fun_bc_call,
    .binary_op = fun_binary_op,
};

mp_obj_t mp_obj_new_fun_bc(uint scope_flags, qstr *args, uint n_args, mp_obj_t def_args_in, const byte *code) {
    uint n_def_args = 0;
    uint n_extra_args = 0;
    mp_obj_tuple_t *def_args = def_args_in;
    if (def_args != MP_OBJ_NULL) {
        assert(MP_OBJ_IS_TYPE(def_args, &mp_type_tuple));
        n_def_args = def_args->len;
        n_extra_args = def_args->len;
    }
    if ((scope_flags & MP_SCOPE_FLAG_VARARGS) != 0) {
        n_extra_args += 1;
    }
    if ((scope_flags & MP_SCOPE_FLAG_VARKEYWORDS) != 0) {
        n_extra_args += 1;
    }
    mp_obj_fun_bc_t *o = m_new_obj_var(mp_obj_fun_bc_t, mp_obj_t, n_extra_args);
    o->base.type = &mp_type_fun_bc;
    o->globals = mp_globals_get();
    o->args = args;
    o->n_args = n_args;
    o->n_def_args = n_def_args;
    o->takes_var_args = (scope_flags & MP_SCOPE_FLAG_VARARGS) != 0;
    o->takes_kw_args = (scope_flags & MP_SCOPE_FLAG_VARKEYWORDS) != 0;
    o->bytecode = code;
    if (def_args != MP_OBJ_NULL) {
        memcpy(o->extra_args, def_args->items, n_def_args * sizeof(mp_obj_t));
    }
    return o;
}

void mp_obj_fun_bc_get(mp_obj_t self_in, int *n_args, const byte **code) {
    assert(MP_OBJ_IS_TYPE(self_in, &mp_type_fun_bc));
    mp_obj_fun_bc_t *self = self_in;
    *n_args = self->n_args;
    *code = self->bytecode;
}

/******************************************************************************/
/* inline assembler functions                                                 */

typedef struct _mp_obj_fun_asm_t {
    mp_obj_base_t base;
    int n_args;
    void *fun;
} mp_obj_fun_asm_t;

typedef machine_uint_t (*inline_asm_fun_0_t)();
typedef machine_uint_t (*inline_asm_fun_1_t)(machine_uint_t);
typedef machine_uint_t (*inline_asm_fun_2_t)(machine_uint_t, machine_uint_t);
typedef machine_uint_t (*inline_asm_fun_3_t)(machine_uint_t, machine_uint_t, machine_uint_t);

// convert a Micro Python object to a sensible value for inline asm
STATIC machine_uint_t convert_obj_for_inline_asm(mp_obj_t obj) {
    // TODO for byte_array, pass pointer to the array
    if (MP_OBJ_IS_SMALL_INT(obj)) {
        return MP_OBJ_SMALL_INT_VALUE(obj);
    } else if (obj == mp_const_none) {
        return 0;
    } else if (obj == mp_const_false) {
        return 0;
    } else if (obj == mp_const_true) {
        return 1;
    } else if (MP_OBJ_IS_STR(obj)) {
        // pointer to the string (it's probably constant though!)
        uint l;
        return (machine_uint_t)mp_obj_str_get_data(obj, &l);
    } else {
        mp_obj_type_t *type = mp_obj_get_type(obj);
        if (0) {
#if MICROPY_ENABLE_FLOAT
        } else if (type == &mp_type_float) {
            // convert float to int (could also pass in float registers)
            return (machine_int_t)mp_obj_float_get(obj);
#endif
        } else if (type == &mp_type_tuple) {
            // pointer to start of tuple (could pass length, but then could use len(x) for that)
            uint len;
            mp_obj_t *items;
            mp_obj_tuple_get(obj, &len, &items);
            return (machine_uint_t)items;
        } else if (type == &mp_type_list) {
            // pointer to start of list (could pass length, but then could use len(x) for that)
            uint len;
            mp_obj_t *items;
            mp_obj_list_get(obj, &len, &items);
            return (machine_uint_t)items;
        } else {
            buffer_info_t bufinfo;
            if (mp_get_buffer(obj, &bufinfo)) {
                // supports the buffer protocol, return a pointer to the data
                return (machine_uint_t)bufinfo.buf;
            } else {
                // just pass along a pointer to the object
                return (machine_uint_t)obj;
            }
        }
    }
}

// convert a return value from inline asm to a sensible Micro Python object
STATIC mp_obj_t convert_val_from_inline_asm(machine_uint_t val) {
    return MP_OBJ_NEW_SMALL_INT(val);
}

STATIC mp_obj_t fun_asm_call(mp_obj_t self_in, uint n_args, uint n_kw, const mp_obj_t *args) {
    mp_obj_fun_asm_t *self = self_in;

    if (n_args != self->n_args) {
        nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_TypeError, "function takes %d positional arguments but %d were given", self->n_args, n_args));
    }
    if (n_kw != 0) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_TypeError, "function does not take keyword arguments"));
    }

    machine_uint_t ret;
    if (n_args == 0) {
        ret = ((inline_asm_fun_0_t)self->fun)();
    } else if (n_args == 1) {
        ret = ((inline_asm_fun_1_t)self->fun)(convert_obj_for_inline_asm(args[0]));
    } else if (n_args == 2) {
        ret = ((inline_asm_fun_2_t)self->fun)(convert_obj_for_inline_asm(args[0]), convert_obj_for_inline_asm(args[1]));
    } else if (n_args == 3) {
        ret = ((inline_asm_fun_3_t)self->fun)(convert_obj_for_inline_asm(args[0]), convert_obj_for_inline_asm(args[1]), convert_obj_for_inline_asm(args[2]));
    } else {
        assert(0);
        ret = 0;
    }

    return convert_val_from_inline_asm(ret);
}

STATIC const mp_obj_type_t mp_type_fun_asm = {
    { &mp_type_type },
    .name = MP_QSTR_function,
    .call = fun_asm_call,
    .binary_op = fun_binary_op,
};

mp_obj_t mp_obj_new_fun_asm(uint n_args, void *fun) {
    mp_obj_fun_asm_t *o = m_new_obj(mp_obj_fun_asm_t);
    o->base.type = &mp_type_fun_asm;
    o->n_args = n_args;
    o->fun = fun;
    return o;
}
