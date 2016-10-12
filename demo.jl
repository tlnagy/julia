using LLVM

#
# Functions
#

@inline function child(x)
    x+1
end

@inline function hacked_child(x)
    x+2
end

function parent(x)
    return child(x)
end


#
# Inference
#

f = parent
t = Tuple{Int}
tt = Base.to_tuple_type(t)

ms = Base._methods(f, tt, -1)
@assert length(ms) == 1
(sig, spvals, m) = first(ms)

# given a function and the argument tuple type (incl. the function type)
# return a tuple of the replacement function and its type, or nothing
function call_hook(f, tt)
    if f == child
        return hacked_child
    end
    return nothing
end
# alternatively, call_hook(f::typeof(child), tt) = return hacked_child
hooks = Core.Inference.InferenceHooks(call_hook)

# raise limits on inference parameters, performing a more exhaustive search
params = Core.Inference.InferenceParams(tuple_depth=32, cached=false, hooks=hooks)

(code, rettyp) = Core.Inference.typeinf_code(m, sig, spvals, params)
code === nothing && error("inference not successful")
println("Returns: $rettyp")
print(code)
println()


#
# IRgen
#

# module set-up
mod = LLVM.Module("my_module")

# irgen
# TODO
exit()
fun = get(functions(mod), "parent")

# execution
ExecutionEngine(mod) do engine
    args = [GenericValue(LLVM.Int32Type(), x)]

    res = LLVM.run(engine, fun, args)
    println(convert(Int, res))

    dispose.(args)
    dispose(res)
end

# jl_get_llvmf_defn vs jl_compile_linfo?
