static jl_value_t *intersect_union(jl_value_t *x, jl_uniontype_t *u, jl_stenv_t *e, int8_t R)
{
    jl_value_t *a=NULL, *b=NULL;
    JL_GC_PUSH2(&a, &b);
    a = R ? intersect(x, u->a) : intersect(u->a, x);
    b = R ? intersect(x, u->b) : intersect(u->b, x);
    jl_value_t *I = simple_join(a, b);
    JL_GC_POP();
    return I;
}

static int intersect_ufirst(jl_value_t *x, jl_value_t *y, jl_stenv_t *e)
{
    if (jl_is_uniontype(x) && jl_is_typevar(y))
        return intersect_union(y, (jl_uniontype_t*)x, e, 0, &e->Lunions, 0);
    if (jl_is_typevar(x) && jl_is_uniontype(y))
        return intersect_union(x, (jl_uniontype_t*)y, e, 1, &e->Runions, 0));
    return subtype(x, y, e, 0);
}

static int var_lt(jl_tvar_t *b, jl_value_t *a, jl_stenv_t *e, int param)
{
    jl_varbinding_t *bb = lookup(e, b);
    if (bb == NULL)
        return subtype_ufirst(b->ub, a, e);
    record_var_occurrence(bb, e, param);
    if (!bb->right)  // check ∀b . b<:a
        return subtype_ufirst(bb->ub, a, e);
    if (!((bb->lb == jl_bottom_type && !jl_is_type(a) && !jl_is_typevar(a)) || subtype_ufirst(bb->lb, a, e)))
        return 0;
    // for contravariance we would need to compute a meet here, but
    // because of invariance bb.ub ⊓ a == a here always. however for this
    // to work we need to compute issub(left,right) before issub(right,left),
    // since otherwise the issub(a, bb.ub) check in var_gt becomes vacuous.
    bb->ub = a;  // meet(bb->ub, a)
    return 1;
}

static int var_gt(jl_tvar_t *b, jl_value_t *a, jl_stenv_t *e, int param)
{
    jl_varbinding_t *bb = lookup(e, b);
    if (bb == NULL)
        return subtype_ufirst(a, b->lb, e);
    record_var_occurrence(bb, e, param);
    if (!bb->right)  // check ∀b . b>:a
        return subtype_ufirst(a, bb->lb, e);
    if (!((bb->ub == (jl_value_t*)jl_any_type && !jl_is_type(a) && !jl_is_typevar(a)) || subtype_ufirst(a, bb->ub, e)))
        return 0;
    bb->lb = simple_join(bb->lb, a);
    return 1;
}

static int subtype_unionall(jl_value_t *t, jl_unionall_t *u, jl_stenv_t *e, int8_t R, int param)
{
    jl_varbinding_t *btemp = e->vars;
    // if the var for this unionall (based on identity) already appears somewhere
    // in the environment, rename to get a fresh var.
    // TODO: might need to look inside types in btemp->lb and btemp->ub
    while (btemp != NULL) {
        if (btemp->var == u->var || btemp->lb == (jl_value_t*)u->var ||
            btemp->ub == (jl_value_t*)u->var) {
            u = rename_unionall(u);
            break;
        }
        btemp = btemp->prev;
    }
    jl_varbinding_t vb = { u->var, u->var->lb, u->var->ub, R, NULL, 0, 0, 0, e->invdepth, e->vars };
    JL_GC_PUSH2(&u, &vb.lb);
    e->vars = &vb;
    int ans;
    if (R) {
        e->envidx++;
        ans = subtype(t, u->body, e, param);
        e->envidx--;
        // fill variable values into `envout` up to `envsz`
        if (e->envidx < e->envsz) {
            jl_value_t *val;
            if (vb.lb == vb.ub)
                val = vb.lb;
            else if (vb.lb != jl_bottom_type)
                // TODO: for now return the least solution, which is what
                // method parameters expect.
                val = vb.lb;
            else if (vb.lb == u->var->lb && vb.ub == u->var->ub)
                val = (jl_value_t*)u->var;
            else
                val = (jl_value_t*)jl_new_typevar(u->var->name, vb.lb, vb.ub);
            e->envout[e->envidx] = val;
        }
    }
    else {
        ans = subtype(u->body, t, e, param);
    }

    // handle the "diagonal dispatch" rule, which says that a type var occurring more
    // than once, and only in covariant position, is constrained to concrete types. E.g.
    //  ( Tuple{Int, Int}    <: Tuple{T, T} where T) but
    // !( Tuple{Int, String} <: Tuple{T, T} where T)
    // Then check concreteness by checking that the lower bound is not an abstract type.
    if (ans && (vb.concrete || (!vb.occurs_inv && vb.occurs_cov > 1))) {
        if (jl_is_typevar(vb.lb)) {
            jl_tvar_t *v = (jl_tvar_t*)vb.lb;
            jl_varbinding_t *vlb = lookup(e, v);
            if (vlb)
                vlb->concrete = 1;
            else  // TODO handle multiple variables in vb.concretevar
                ans = (v == vb.concretevar);
        }
        else if (!is_leaf_bound(vb.lb)) {
            ans = 0;
        }
        if (ans) {
            // if we occur as another var's lower bound, record the fact that we
            // were concrete so that subtype can return true for that var.
            btemp = vb.prev;
            while (btemp != NULL) {
                if (btemp->lb == (jl_value_t*)u->var)
                    btemp->concretevar = u->var;
                btemp = btemp->prev;
            }
        }
    }

    e->vars = vb.prev;
    JL_GC_POP();
    return ans;
}

static int subtype_tuple(jl_datatype_t *xd, jl_datatype_t *yd, jl_stenv_t *e)
{
    size_t lx = jl_nparams(xd), ly = jl_nparams(yd);
    if (lx == 0 && ly == 0)
        return 1;
    if (ly == 0)
        return 0;
    size_t i=0, j=0;
    int vx=0, vy=0;
    while (i < lx) {
        if (j >= ly) return 0;
        jl_value_t *xi = jl_tparam(xd, i), *yi = jl_tparam(yd, j);
        if (jl_is_vararg_type(xi)) vx = 1;
        if (jl_is_vararg_type(yi)) vy = 1;
        if (vx && !vy)
            return 0;
        if (!vx && vy) {
            if (!subtype(xi, jl_unwrap_vararg(yi), e, 1))
                return 0;
        }
        else {
            if (!subtype(xi, yi, e, 1))
                return 0;
        }
        i++;
        if (j < ly-1 || !vy)
            j++;
    }
    // TODO: handle Vararg with explicit integer length parameter
    vy = vy || (j < ly && jl_is_vararg_type(jl_tparam(yd,j)));
    if (vy && !vx && lx+1 >= ly) {
        jl_tvar_t *va_p1=NULL, *va_p2=NULL;
        jl_value_t *tail = unwrap_2_unionall(jl_tparam(yd,ly-1), &va_p1, &va_p2);
        assert(jl_is_datatype(tail));
        // in Tuple{...,tn} <: Tuple{...,Vararg{T,N}}, check (lx+1-ly) <: N
        jl_value_t *N = jl_tparam1(tail);
        // only do the check if N is free in the tuple type's last parameter
        if (N != (jl_value_t*)va_p1 && N != (jl_value_t*)va_p2) {
            if (!subtype(jl_box_long(lx+1-ly), N, e, 1))
                return 0;
        }
    }
    /*
      Tuple{A^n...,Vararg{T,N}} ∩ Tuple{Vararg{S,M}} =
        Tuple{(A∩S)^n...,Vararg{T∩S,N}} plus N = M-n
    */
    return (lx==ly && vx==vy) || (vy && (lx >= (vx ? ly : (ly-1))));
}

// `param` means we are currently looking at a parameter of a type constructor
// (as opposed to being outside any type constructor, or comparing variable bounds).
// this is used to record the positions where type variables occur for the
// diagonal rule (record_var_occurrence).
static int subtype(jl_value_t *x, jl_value_t *y, jl_stenv_t *e, int param)
{
    if (x == jl_ANY_flag) x = (jl_value_t*)jl_any_type;
    if (y == jl_ANY_flag) y = (jl_value_t*)jl_any_type;
    if (jl_is_typevar(x)) {
        if (jl_is_typevar(y)) {
            if (x == y) return 1;
            jl_varbinding_t *xx = lookup(e, (jl_tvar_t*)x);
            jl_varbinding_t *yy = lookup(e, (jl_tvar_t*)y);
            int xr = xx && xx->right;  // treat free variables as "forall" (left)
            int yr = yy && yy->right;
            if (xr) {
                if (yy) record_var_occurrence(yy, e, param);
                if (yr) {
                    // this is a bit odd, but seems necessary to make this case work:
                    // (UnionAll x<:T<:x Ref{Ref{T}}) == Ref{UnionAll x<:T<:x Ref{T}}
                    return subtype(yy->ub, yy->lb, e, 0);
                }
                return var_lt((jl_tvar_t*)x, y, e, param);
            }
            else if (yr) {
                if (xx) record_var_occurrence(xx, e, param);
                return var_gt((jl_tvar_t*)y, x, e, param);
            }
            jl_value_t *xub = xx ? xx->ub : ((jl_tvar_t*)x)->ub;
            jl_value_t *ylb = yy ? yy->lb : ((jl_tvar_t*)y)->lb;
            // check ∀x,y . x<:y
            // the bounds of left-side variables never change, and can only lead
            // to other left-side variables, so using || here is safe.
            return subtype(xub, y, e, param) || subtype(x, ylb, e, param);
        }
        return var_lt((jl_tvar_t*)x, y, e, param);
    }
    if (jl_is_typevar(y))
        return var_gt((jl_tvar_t*)y, x, e, param);
    if (jl_is_uniontype(y)) {
        if (x == y || x == ((jl_uniontype_t*)y)->a || x == ((jl_uniontype_t*)y)->b)
            return 1;
        if (jl_is_unionall(x))
            return subtype_unionall(y, (jl_unionall_t*)x, e, 0, param);
        return subtype_union(x, y, e, 1, &e->Runions, param);
    }
    if (jl_is_uniontype(x)) {
        if (jl_is_unionall(y))
            return subtype_unionall(x, (jl_unionall_t*)y, e, 1, param);
        return subtype_union(y, x, e, 0, &e->Lunions, param);
    }
    if (jl_is_unionall(y)) {
        if (x == y && !(e->envidx < e->envsz))
            return 1;
        return subtype_unionall(x, (jl_unionall_t*)y, e, 1, param);
    }
    if (jl_is_unionall(x))
        return subtype_unionall(y, (jl_unionall_t*)x, e, 0, param);
    if (jl_is_datatype(x) && jl_is_datatype(y)) {
        if (x == y) return 1;
        if (y == (jl_value_t*)jl_any_type) return 1;
        jl_datatype_t *xd = (jl_datatype_t*)x, *yd = (jl_datatype_t*)y;
        if (jl_is_type_type(xd) && !jl_is_typevar(jl_tparam0(xd)) && jl_typeof(jl_tparam0(xd)) == yd)
            // TODO this is not strictly correct, but we don't yet have any other way for
            // e.g. the argument `Int` to match a `::DataType` slot. Most correct would be:
            // Int isa DataType, Int isa Type{Int}, Type{Int} more specific than DataType,
            // !(Type{Int} <: DataType), !isleaftype(Type{Int}), because non-DataTypes can
            // be type-equal to `Int`.
            return 1;
        while (xd != jl_any_type && xd->name != yd->name)
            xd = xd->super;
        if (xd == (jl_value_t*)jl_any_type) return 0;
        if (jl_is_tuple_type(xd))
            return subtype_tuple(xd, yd, e);
        if (jl_is_vararg_type(xd)) {
            // Vararg: covariant in first parameter, invariant in second
            jl_value_t *xp1=jl_tparam0(xd), *xp2=jl_tparam1(xd), *yp1=jl_tparam0(yd), *yp2=jl_tparam1(yd);
            // in Vararg{T1} <: Vararg{T2}, need to check subtype twice to
            // simulate the possibility of multiple arguments, which is needed
            // to implement the diagonal rule correctly.
            if (!subtype(xp1, yp1, e, 1)) return 0;
            if (!subtype(xp1, yp1, e, 1)) return 0;
            // Vararg{T,N} <: Vararg{T2,N2}; equate N and N2
            e->invdepth++;
            int ans = subtype(xp2, yp2, e, 1) && subtype(yp2, xp2, e, 0);
            e->invdepth--;
            return ans;
        }
        size_t i, np = jl_nparams(xd);
        int ans = 1;
        e->invdepth++;
        for (i=0; i < np; i++) {
            jl_value_t *xi = jl_tparam(xd, i), *yi = jl_tparam(yd, i);
            jl_value_t *ii = intersect(xi, yi, e, 1);
            if (ii == jl_bottom_type && xi != yi)
                return ii;
            if (!(subtype(xi, ii, e, 0) && subtype(yi, ii, e, 0))) {
                ans = 0; break;
            }
        }
        e->invdepth--;
        return ans;
    }
    if (jl_is_type(y))
        return x == jl_bottom_type;
    return x == y || jl_egal(x, y);
}
