#include <stdlib.h>
#include <assert.h>

#include "nlr.h"
#include "misc.h"
#include "mpconfig.h"
#include "qstr.h"
#include "obj.h"
#include "runtime.h"

#if MICROPY_ENABLE_PROPERTY

typedef struct _mp_obj_property_t {
    mp_obj_base_t base;
    mp_obj_t proxy[3]; // getter, setter, deleter
} mp_obj_property_t;

STATIC mp_obj_t property_make_new(mp_obj_t type_in, uint n_args, uint n_kw, const mp_obj_t *args) {
    // TODO check n_kw == 0

    mp_obj_property_t *o = m_new_obj(mp_obj_property_t);
    o->base.type = &mp_type_property;
    if (n_args >= 5) {
        nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_TypeError, "property takes at most 4 arguments"));
    }
    if (n_args >= 4) {
        // doc ignored
    }
    if (n_args >= 3) {
        o->proxy[2] = args[2];
    } else {
        o->proxy[2] = mp_const_none;
    }
    if (n_args >= 2) {
        o->proxy[1] = args[1];
    } else {
        o->proxy[1] = mp_const_none;
    }
    if (n_args >= 1) {
        o->proxy[0] = args[0];
    } else {
        o->proxy[0] = mp_const_none;
    }
    return o;
}

STATIC mp_obj_t property_getter(mp_obj_t self_in, mp_obj_t getter) {
    mp_obj_property_t *p2 = m_new_obj(mp_obj_property_t);
    *p2 = *(mp_obj_property_t*)self_in;
    p2->proxy[0] = getter;
    return p2;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(property_getter_obj, property_getter);

STATIC mp_obj_t property_setter(mp_obj_t self_in, mp_obj_t setter) {
    mp_obj_property_t *p2 = m_new_obj(mp_obj_property_t);
    *p2 = *(mp_obj_property_t*)self_in;
    p2->proxy[1] = setter;
    return p2;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(property_setter_obj, property_setter);

STATIC mp_obj_t property_deleter(mp_obj_t self_in, mp_obj_t deleter) {
    mp_obj_property_t *p2 = m_new_obj(mp_obj_property_t);
    *p2 = *(mp_obj_property_t*)self_in;
    p2->proxy[2] = deleter;
    return p2;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_2(property_deleter_obj, property_deleter);

STATIC const mp_map_elem_t property_locals_dict_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR_getter), (mp_obj_t)&property_getter_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_setter), (mp_obj_t)&property_setter_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_deleter), (mp_obj_t)&property_deleter_obj },
};

STATIC MP_DEFINE_CONST_DICT(property_locals_dict, property_locals_dict_table);

const mp_obj_type_t mp_type_property = {
    { &mp_type_type },
    .name = MP_QSTR_property,
    .make_new = property_make_new,
    .locals_dict = (mp_obj_t)&property_locals_dict,
};

const mp_obj_t *mp_obj_property_get(mp_obj_t self_in) {
    assert(MP_OBJ_IS_TYPE(self_in, &mp_type_property));
    mp_obj_property_t *self = self_in;
    return self->proxy;
}

#endif // MICROPY_ENABLE_PROPERTY
