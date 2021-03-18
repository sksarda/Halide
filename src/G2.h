#include "AbstractGenerator.h"

#include <array>
#include <iostream>
#include <sstream>

namespace Halide {

inline std::ostream &operator<<(std::ostream &stream, const std::vector<Type> &v) {
    stream << "{";
    const char *comma = "";
    for (const Type &t : v) {
        stream << t << comma;
        comma = ",";
    }
    stream << "}";
    return stream;
}

namespace Internal {

// ----------------------------------------------

// Strip the class from a method type
template<typename T>
struct remove_class {};
template<typename C, typename R, typename... A>
struct remove_class<R (C::*)(A...)> { typedef R type(A...); };
template<typename C, typename R, typename... A>
struct remove_class<R (C::*)(A...) const> { typedef R type(A...); };

template<typename F>
struct strip_function_object {
    using type = typename remove_class<decltype(&F::operator())>::type;
};

// Extracts the function signature from a function, function pointer or lambda.
template<typename Function, typename F = typename std::remove_reference<Function>::type>
using function_signature = std::conditional<
    std::is_function<F>::value,
    F,
    typename std::conditional<
        std::is_pointer<F>::value || std::is_member_pointer<F>::value,
        typename std::remove_pointer<F>::type,
        typename strip_function_object<F>::type>::type>;

template<typename T, typename T0 = typename std::remove_reference<T>::type>
using is_lambda = std::integral_constant<bool, !std::is_function<T0>::value && !std::is_pointer<T0>::value && !std::is_member_pointer<T0>::value>;

// ---------------------------------------

struct SingleArg {
    enum class Kind {
        Unknown,
        Constant,
        Expression,
        Function,
        Buffer,
    };

    std::string name;
    Kind kind = Kind::Unknown;
    std::vector<Type> types;
    int dimensions = -1;
    std::string default_value;  // only when kind == Constant

    explicit SingleArg(const std::string &n, Kind k, const std::vector<Type> &t, int d, const std::string &s = "")
        : name(n), kind(k), types(t), dimensions(d), default_value(s) {
    }

    // Combine the inferred type info with the explicitly-annotated type info
    // to produce an ArgInfo. All information must be specified in at least one
    // of the two. It's ok for info to be specified in both places iff they
    // agree.
    static SingleArg match(const SingleArg &annotated, const SingleArg &inferred, bool skip_default_value = true) {
        user_assert(!annotated.name.empty())
            << "Unable to resolve signature for Generator: all arguments must have an explicit name specified.";

        return SingleArg{
            get_matching_value(annotated.name, inferred.name, annotated.name, "name"),
            get_matching_value(annotated.kind, inferred.kind, annotated.name, "kind"),
            get_matching_value(annotated.types, inferred.types, annotated.name, "types"),
            get_matching_value(annotated.dimensions, inferred.dimensions, annotated.name, "dimensions"),
            skip_default_value ?
                require_both_empty(annotated.default_value, inferred.default_value) :
                get_matching_value(annotated.default_value, inferred.default_value, annotated.name, "default_value")};
    }

private:
    template<typename T>
    static bool is_specified(const T &t);

    template<typename T>
    static T get_matching_value(const T &annotated, const T &inferred, const std::string &name, const char *field);

    template<typename T>
    static T require_both_empty(const T &annotated, const T &inferred);
};

// ---------------------------------------

inline std::ostream &operator<<(std::ostream &stream, SingleArg::Kind k) {
    static const char *const kinds[] = {
        "Unknown",
        "Constant",
        "Expression",
        "Function",
        "Buffer",
    };
    stream << kinds[(int)k];
    return stream;
}

inline std::ostream &operator<<(std::ostream &stream, const SingleArg &a) {
    stream << "SingleArg{" << a.name << "," << a.kind << "," << a.types << "," << a.dimensions << "," << a.default_value << "}";
    return stream;
}

inline std::ostream &operator<<(std::ostream &stream, IOKind k) {
    static const char *const kinds[] = {
        "Scalar",
        "Function",
        "Buffer",
    };
    stream << kinds[(int)k];
    return stream;
}

inline std::ostream &operator<<(std::ostream &stream, const AbstractGenerator::ArgInfo &a) {
    stream << "ArgInfo{" << a.name << "," << a.kind << "," << a.types << "," << a.dimensions << "}";
    return stream;
}

// ---------------------------------------

template<typename T>
/*static*/ T SingleArg::get_matching_value(const T &annotated, const T &inferred, const std::string &name, const char *field) {
    const bool a_spec = is_specified(annotated);
    const bool i_spec = is_specified(inferred);

    user_assert(a_spec || i_spec)
        << "Unable to resolve signature for Generator argument '" << name << "': "
        << "There is no explicitly-specified or inferred value for field '" << field << "'.";

    if (a_spec) {
        if (i_spec) {
            user_assert(annotated == inferred)
                    << "Unable to resolve signature for Generator argument '" << name << "': "
                    << "The explicitly-specified value for field '" << field
                    << "' was '" << annotated
                    << "', which does not match the inferred value '" << inferred << "'.";
        }
        return annotated;
    } else {
        return inferred;
    }
}

template<typename T>
/*static*/ T SingleArg::require_both_empty(const T &annotated, const T &inferred) {
    const bool a_spec = is_specified(annotated);
    const bool i_spec = is_specified(inferred);

    internal_assert(!a_spec && !i_spec);
    return annotated;
}

template<>
inline bool SingleArg::is_specified(const std::string &n) {
    return !n.empty();
}

template<>
inline bool SingleArg::is_specified(const SingleArg::Kind &k) {
    return k != SingleArg::Kind::Unknown;
}

template<>
inline bool SingleArg::is_specified(const std::vector<Type> &t) {
    return !t.empty();
}

template<>
inline bool SingleArg::is_specified(const int &d) {
    return d >= 0;
}

template<typename T>
struct SingleArgInferrer {
    inline SingleArg operator()() {
        const Type t = type_of<T>();
        // TODO: also allow std::string
        if (t.is_scalar() && (t.is_int() || t.is_uint() || t.is_float())) {
            return SingleArg{"", SingleArg::Kind::Constant, {t}, 0};
        }
        return SingleArg{"", SingleArg::Kind::Unknown, {}, -1};
    }
};

template<>
inline SingleArg SingleArgInferrer<Func>::operator()() {
    return SingleArg{"", SingleArg::Kind::Function, {}, -1};  // TODO: can't tell type or dims, must put in input()
}

template<>
inline SingleArg SingleArgInferrer<Expr>::operator()() {
    return SingleArg{"", SingleArg::Kind::Expression, {}, 0};  // TODO: can't tell type (but dims are always 0)
}

// ---------------------------------------

using ArgInfo = AbstractGenerator::ArgInfo;

class ArgInfoDetector {
public:
    // Movable but not copyable
    ArgInfoDetector() = delete;
    void operator=(const ArgInfoDetector &) = delete;
    void operator=(ArgInfoDetector &&) = delete;
    ArgInfoDetector(const ArgInfoDetector &) = default;
    ArgInfoDetector(ArgInfoDetector &&) = default;

    struct InputOrConstant : public SingleArg {
        explicit InputOrConstant(const std::string &n, SingleArg::Kind k, const std::vector<Type> &t, int d, const std::string &s = "")
            : SingleArg(n, k, t, d, s) {
        }
    };

    struct Constant : public InputOrConstant {
        explicit Constant(const std::string &n, const std::string &s)
            : InputOrConstant(n, SingleArg::Kind::Constant, std::vector<Type>{}, 0, s) {
        }
    };

    struct Input : public InputOrConstant {
        explicit Input(const std::string &n, const std::vector<Type> &t, int d)
            : InputOrConstant(n, SingleArg::Kind::Unknown, t, d) {
        }

        // explicit Input(const std::string &n)
        //     : Input(n, std::vector<Type>{}, -1) {
        // }
        explicit Input(const std::string &n, const std::vector<Type> &t)
            : Input(n, t, -1) {
        }
        // explicit Input(const std::string &n, int d)
        //     : Input(n, std::vector<Type>{}, d) {
        // }
        explicit Input(const std::string &n, const Type &t)
            : Input(n, std::vector<Type>{t}, -1) {
        }
        explicit Input(const std::string &n, const Type &t, int d)
            : Input(n, std::vector<Type>{t}, d) {
        }
    };

    struct Output : public SingleArg {
        explicit Output(const std::string &n, const std::vector<Type> &t, int d)
            : SingleArg(n, SingleArg::Kind::Unknown, t, d) {
        }

        // explicit Output(const std::string &n)
        //     : Output(n, std::vector<Type>{}, -1) {
        // }
        explicit Output(const std::string &n, const std::vector<Type> &t)
            : Output(n, t, -1) {
        }
        // explicit Output(const std::string &n, int d)
        //     : Output(n, std::vector<Type>{}, d) {
        // }
        explicit Output(const std::string &n, const Type &t)
            : Output(n, std::vector<Type>{t}, -1) {
        }
        explicit Output(const std::string &n, const Type &t, int d)
            : Output(n, std::vector<Type>{t}, d) {
        }
    };

    // Construct an ArgInfoDetector from an ordinary function
    template<typename ReturnType, typename... Args>
    ArgInfoDetector(ReturnType (*f)(Args...), const std::vector<InputOrConstant> &inputs, const Output &output){
std::cerr << "CTOR #1\n";
        initialize(f, f, inputs, output);
    }

    // Construct an ArgInfoDetector from a lambda function (possibly with internal state)
    template<typename Func, typename... Inputs,
             typename std::enable_if<is_lambda<Func>::value>::type * = nullptr>
    ArgInfoDetector(Func &&f, const std::vector<InputOrConstant> &inputs, const Output &output){
std::cerr << "CTOR #2\n";
        initialize(std::forward<Func>(f), (typename function_signature<Func>::type *)nullptr, inputs, output);
    }

    std::vector<Constant> constants() const {
        return constants_;
    }
    std::vector<ArgInfo> inputs() const {
        return inputs_;
    }
    std::vector<ArgInfo> outputs() const {
        return outputs_;
    }

    void inspect() const {
        for (const auto &a : constants_) {
            std::cout << "  constant: " << a << "\n";
        }
        for (const auto &a : inputs_) {
            std::cout << "  in: " << a << "\n";
        }
        for (const auto &a : outputs_) {
            std::cout << "  out: " << a << "\n";
        }
    }

protected:
    std::vector<Constant> constants_;
    std::vector<ArgInfo> inputs_;
    std::vector<ArgInfo> outputs_;

    IOKind to_iokind(SingleArg::Kind k) {
        std::ostringstream oss;
        oss << k;
        std::cerr << oss.str();
        switch (k) {
        default:
            internal_error << "Unhandled SingleArg::Kind: " << k;
        case SingleArg::Kind::Expression:
            return IOKind::Scalar;
        case SingleArg::Kind::Function:
            return IOKind::Function;
        case SingleArg::Kind::Buffer:
            return IOKind::Buffer;
        }
    }

    ArgInfo to_arginfo(const SingleArg &a) {
        return ArgInfo{
            a.name,
            to_iokind(a.kind),
            a.types,
            a.dimensions,
        };
    }

    template<typename Func, typename ReturnType, typename... Args>
    void initialize(Func &&f, ReturnType (*)(Args...), const std::vector<InputOrConstant> &inputs, const Output &output) {
        user_assert(sizeof...(Args) == inputs.size()) << "The number of argument annotations does not match the number of function arguments";

        const std::array<SingleArg, sizeof...(Args)> inferred_arg_types = {SingleArgInferrer<typename std::decay<Args>::type>()()...};
        internal_assert(inferred_arg_types.size() == inputs.size());

        for (size_t i = 0; i < inputs.size(); ++i) {
            const bool is_constant = (inferred_arg_types[i].kind == SingleArg::Kind::Constant);
            const bool skip_default_value = !is_constant;
            const SingleArg matched = SingleArg::match(inputs[i], inferred_arg_types[i], skip_default_value);
            if (inferred_arg_types[i].kind == SingleArg::Kind::Constant) {
                constants_.emplace_back(matched.name, matched.default_value);
                constants_.back().types = matched.types;
            } else {
                inputs_.push_back(to_arginfo(matched));
            }
        }

        // TODO: handle Halide::Tuple here
        const SingleArg inferred_ret_type = SingleArgInferrer<typename std::decay<ReturnType>::type>()();
        user_assert(inferred_ret_type.kind != SingleArg::Kind::Constant)
            << "Outputs must be Func, Expr, or Buffer, but the type seen was " << inferred_ret_type.types << ".";
        outputs_.push_back(to_arginfo(SingleArg::match(output, inferred_ret_type)));
    }
};

#if 1
class G2Generator : public AbstractGenerator {
    const TargetInfo target_info_;
    const std::string name_;
    const std::vector<ArgInfo> inputs_, outputs_;
    std::map<std::string, std::string> generatorparams_;

    Pipeline pipeline_;

    static std::map<std::string, std::string> init_generatorparams(const std::vector<ArgInfoDetector::Constant> &constants) {
        std::map<std::string, std::string> result;
        for (const auto &c : constants) {
            result[c.name] = c.default_value;
        }
        return result;
    }

public:
    explicit G2Generator(const GeneratorContext &context, const std::string &name, const ArgInfoDetector &detector)
        : target_info_{context.get_target(), context.get_auto_schedule(), context.get_machine_params()},
          name_(name),
          inputs_(detector.inputs()),
          outputs_(detector.outputs()),
          generatorparams_(init_generatorparams(detector.constants())) {
    }

    std::string get_name() override {
        return name_;
    }

    TargetInfo get_target_info() override {
        return target_info_;
    }

    std::vector<ArgInfo> get_input_arginfos() override {
        return inputs_;
    }

    std::vector<ArgInfo> get_output_arginfos() override {
        return outputs_;
    }

    std::vector<std::string> get_generatorparam_names() override {
        std::vector<std::string> v;
        for (const auto &c : generatorparams_) {
            v.push_back(c.first);
        }
        return v;
    }

    void set_generatorparam_value(const std::string &name, const std::string &value) override {
        user_assert(!pipeline_.defined())
            << "set_generatorparam_value() must be called before build_pipeline().";
        user_assert(generatorparams_.count(name) == 1) << "Unknown Constant: " << name;
        generatorparams_[name] = value;
    }

    void set_generatorparam_value(const std::string &name, const LoopLevel &value) override {
        user_assert(!pipeline_.defined())
            << "set_generatorparam_value() must be called before build_pipeline().";
        user_assert(generatorparams_.count(name) == 1) << "Unknown Constant: " << name;
        user_assert(false) << "This Generator has no LoopLevel constants.";
    }

    void bind_input(const std::string &name, const std::vector<Parameter> &v) override {
        user_assert(!pipeline_.defined())
            << "bind_input() must be called before build_pipeline().";
        internal_error << "Unimplemented: " << __FILE__ << ":" << __LINE__;
    }

    void bind_input(const std::string &name, const std::vector<Func> &v) override {
        user_assert(!pipeline_.defined())
            << "bind_input() must be called before build_pipeline().";
        internal_error << "Unimplemented: " << __FILE__ << ":" << __LINE__;
    }

    void bind_input(const std::string &name, const std::vector<Expr> &v) override {
        user_assert(!pipeline_.defined())
            << "bind_input() must be called before build_pipeline().";
        internal_error << "Unimplemented: " << __FILE__ << ":" << __LINE__;
    }

    Pipeline build_pipeline() override {
        user_assert(!pipeline_.defined())
            << "build_pipeline() may not be called twice.";

        // const int scaling = string_to_int(generatorparams_.at("scaling"));

        // Var x, y;
        // output_(x, y) = input_(x, y) * scaling + offset_;
        // output_.compute_root();

        // pipeline_ = output_;
        user_assert(pipeline_.defined())
            << "build_pipeline() did not build a Pipeline!";
        return pipeline_;
    }

    std::vector<Parameter> get_parameters_for_input(const std::string &name) override {
        user_assert(pipeline_.defined())
            << "get_parameters_for_input() must be called after build_pipeline().";
        // if (name == "input") {
        //     return {input_.parameter()};
        // }
        // if (name == "offset") {
        //     return {offset_.parameter()};
        // }
        // user_assert(false) << "Unknown input: " << name;
        return {};
    }

    std::vector<Func> get_funcs_for_output(const std::string &name) override {
        user_assert(pipeline_.defined())
            << "get_funcs_for_output() must be called after build_pipeline().";
        // if (name == "output") {
        //     return {output_};
        // }
        // internal_assert(false) << "Unknown output: " << name;
        return {};
    }

    ExternsMap get_external_code_map() override {
        user_assert(pipeline_.defined())
            << "get_external_code_map() must be called after build_pipeline().";
        // TODO: not supported now; how necessary and/or doable is this?
        return {};
    }

    bool emit_cpp_stub(const std::string & /*stub_file_path*/) override {
        // not supported
        return false;
    }
};

class G2GeneratorFactory {
    const std::string name_;
    const ArgInfoDetector detector_;
public:
    explicit G2GeneratorFactory(const std::string name, ArgInfoDetector &&detector) : name_(name), detector_(std::move(detector)) {
    }

    std::unique_ptr<AbstractGenerator> operator()(const GeneratorContext &context) {
        return std::unique_ptr<AbstractGenerator>(new G2Generator(context, name_, detector_));
    }
};

#endif

}  // namespace Internal
}  // namespace Halide
