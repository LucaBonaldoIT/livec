#ifndef LIVEC_REFLECTION_HPP
#define LIVEC_REFLECTION_HPP

#include <livec/detail/reflection_abi.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace livec {

template <class Signature>
struct FunctionRef;

struct Function {
    std::string name;
    std::string signature;
    std::string source_file;
    unsigned line = 0;
    std::uint64_t version = 0;
    void (*entry_point)(void) = nullptr;

    explicit operator bool() const noexcept { return entry_point != nullptr; }

    template <class Signature>
    FunctionRef<Signature> as() const;
};

template <class R, class... Args>
struct FunctionRef<R(Args...)> : Function {
    R operator()(Args... args) const {
        assert(entry_point && "calling an empty LiveC function handle");
        using Pointer = R (*)(Args...);
        auto target = reinterpret_cast<Pointer>(entry_point);
        if constexpr (std::is_void_v<R>) {
            target(std::forward<Args>(args)...);
        } else {
            return target(std::forward<Args>(args)...);
        }
    }
};

template <class Signature>
FunctionRef<Signature> Function::as() const {
    FunctionRef<Signature> result;
    static_cast<Function &>(result) = *this;
    return result;
}

struct Variable {
    std::string name;
    std::string type;
    std::string source_file;
    unsigned line = 0;
    std::size_t size = 0;
    bool is_const = false;
    void *address = nullptr;

    template <class T>
    T &as() const { return *static_cast<T *>(address); }
};

struct Field {
    std::string name;
    std::string type;
    std::size_t offset_bits = 0;
    std::size_t size_bits = 0;
    bool is_const = false;
    bool is_bitfield = false;
};

struct Type {
    std::string name;
    std::string source_file;
    std::string kind;
    std::size_t size = 0;
    std::size_t alignment = 0;
    std::vector<Field> fields;
    std::vector<Function> methods;
};

namespace detail {
inline Function toFunction(const livec_function_info &info) {
    return {info.name ? info.name : "", info.signature ? info.signature : "",
            info.source_file ? info.source_file : "", info.line,
            info.version, info.entry_point};
}

inline Variable toVariable(const livec_variable_info &info) {
    return {info.name ? info.name : "", info.type ? info.type : "",
            info.source_file ? info.source_file : "", info.line,
            info.size, info.is_const != 0, info.address};
}

inline std::string_view typeName(const char *name) { return name; }

template <class T>
std::string typeName() {
    using U = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (std::is_same_v<U, void>) return "void";
    else if constexpr (std::is_same_v<U, bool>) return "bool";
    else if constexpr (std::is_same_v<U, char>) return "char";
    else if constexpr (std::is_same_v<U, signed char>) return "signed char";
    else if constexpr (std::is_same_v<U, unsigned char>) return "unsigned char";
    else if constexpr (std::is_same_v<U, short>) return "short";
    else if constexpr (std::is_same_v<U, unsigned short>) return "unsigned short";
    else if constexpr (std::is_same_v<U, int>) return "int";
    else if constexpr (std::is_same_v<U, unsigned int>) return "unsigned int";
    else if constexpr (std::is_same_v<U, long>) return "long";
    else if constexpr (std::is_same_v<U, unsigned long>) return "unsigned long";
    else if constexpr (std::is_same_v<U, long long>) return "long long";
    else if constexpr (std::is_same_v<U, unsigned long long>) return "unsigned long long";
    else if constexpr (std::is_same_v<U, float>) return "float";
    else if constexpr (std::is_same_v<U, double>) return "double";
    else if constexpr (std::is_same_v<U, long double>) return "long double";
    else if constexpr (std::is_pointer_v<U>) return typeName<std::remove_pointer_t<U>>() + " *";
    else return typeid(U).name();
}

template <class Signature>
struct SignatureName;

template <class R, class... Args>
struct SignatureName<R(Args...)> {
    static std::string get() {
        std::string result = typeName<R>() + " (";
        bool first = true;
        ((result += (first ? (first = false, "") : ", ") + typeName<Args>()), ...);
        if constexpr (sizeof...(Args) == 0) result += "void";
        result += ")";
        return result;
    }
};

inline Function lookupFunction(std::string_view name, std::string_view signature,
                               std::string_view source) {
    const std::string n(name), s(signature), f(source);
    livec_function_info info{};
    if (!livec_reflect_function_find_signature(n.c_str(), s.empty() ? nullptr : s.c_str(),
                                                f.empty() ? nullptr : f.c_str(), &info)) return {};
    return toFunction(info);
}
} // namespace detail

inline Function function(std::string_view name, std::string_view signature = {},
                         std::string_view source_file = {}) {
    return detail::lookupFunction(name, signature, source_file);
}

inline Variable variable(std::string_view name, std::string_view source_file = {}) {
    const std::string n(name), file(source_file);
    livec_variable_info info{};
    return livec_reflect_variable_find(n.c_str(), file.empty() ? nullptr : file.c_str(), &info)
        ? detail::toVariable(info) : Variable{};
}

inline Type type(std::string_view name, std::string_view source_file = {}) {
    const std::string n(name), file(source_file);
    livec_type_info info{};
    if (!livec_reflect_type_find(n.c_str(), file.empty() ? nullptr : file.c_str(), &info)) return {};
    Type result;
    result.name = info.name ? info.name : "";
    result.source_file = info.source_file ? info.source_file : "";
    result.kind = info.kind ? info.kind : "";
    result.size = info.size;
    result.alignment = info.alignment;
    const std::string id = info.id ? info.id : "";
    const std::size_t fieldCount = livec_reflect_type_field_count(id.c_str());
    for (std::size_t index = 0; index < fieldCount; ++index) {
        livec_field_info field{};
        if (!livec_reflect_type_field_at(id.c_str(), index, &field)) continue;
        result.fields.push_back({field.name ? field.name : "", field.type ? field.type : "",
                                 field.offset_bits, field.size_bits, field.is_const != 0,
                                 field.is_bitfield != 0});
    }
    const std::size_t methodCount = livec_reflect_type_method_count(id.c_str());
    for (std::size_t index = 0; index < methodCount; ++index) {
        livec_function_info method{};
        if (livec_reflect_type_method_at(id.c_str(), index, &method))
            result.methods.push_back(detail::toFunction(method));
    }
    return result;
}

inline std::vector<Function> functions() {
    std::vector<Function> result;
    const std::size_t count = livec_reflect_function_count();
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        livec_function_info info{};
        if (livec_reflect_function_at(index, &info)) result.push_back(detail::toFunction(info));
    }
    return result;
}

inline std::vector<Variable> variables() {
    std::vector<Variable> result;
    const std::size_t count = livec_reflect_variable_count();
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        livec_variable_info info{};
        if (livec_reflect_variable_at(index, &info)) result.push_back(detail::toVariable(info));
    }
    return result;
}

inline std::vector<Type> types() {
    std::vector<Type> result;
    const std::size_t count = livec_reflect_type_count();
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        livec_type_info info{};
        if (!livec_reflect_type_at(index, &info)) continue;
        result.push_back(type(info.name ? info.name : "", info.source_file ? info.source_file : ""));
    }
    return result;
}

template <class Signature>
FunctionRef<Signature> function(std::string_view name, std::string_view source_file = {}) {
    return detail::lookupFunction(name, detail::SignatureName<Signature>::get(), source_file)
        .template as<Signature>();
}

} // namespace livec

#endif
