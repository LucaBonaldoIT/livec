#include "livec/detail/reflection_abi.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/IndirectionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

extern char **environ;

namespace fs = std::filesystem;
namespace orc = llvm::orc;
class LiveRuntime;
static LiveRuntime *reflectionRuntime = nullptr;
static bool quietMode = false;

static void logInfo(const std::string &message) {
    if (!quietMode) std::cerr << "[LiveC] " << message << '\n';
}

static std::mutex functionVersionMutex;
static std::map<uint64_t, uint64_t> activeFunctionVersions;
static std::atomic<bool> publicationInProgress{false};

extern "C" int livec_function_is_stale(uint64_t functionId, uint64_t version) {
    if (publicationInProgress.load(std::memory_order_acquire)) return 0;
    std::lock_guard<std::mutex> lock(functionVersionMutex);
    auto active = activeFunctionVersions.find(functionId);
    return active != activeFunctionVersions.end() && active->second != version;
}

#ifndef LIVEC_CLANG_DEFAULT
#define LIVEC_CLANG_DEFAULT "clang-22"
#endif
#ifndef LIVEC_CXX_INCLUDE_DEFAULT
#define LIVEC_CXX_INCLUDE_DEFAULT ""
#endif

struct Options {
    fs::path entry;
    fs::path watchRoot;
    bool quiet = false;
    std::vector<std::string> clangArgs;
    std::vector<fs::path> dynamicLibraries;
};

struct FunctionUpdate {
    std::string stableName;
    std::string dispatchKey;
    std::string implementationName;
    std::string signature;
    std::string body;
    bool publish = true;
    bool weakODR = false;
    std::string displayName;
    std::string displaySignature;
    std::string sourceFile;
    unsigned line = 0;
    bool reflectable = true;
};

struct FunctionMetadata {
    std::string name;
    std::string signature;
    std::string sourceFile;
    unsigned line = 0;
};

struct VariableMetadata {
    std::string name;
    std::string type;
    std::string sourceFile;
    unsigned line = 0;
    size_t size = 0;
    bool isConst = false;
    bool reflectable = true;
};

struct FieldMetadata {
    std::string name;
    std::string type;
    size_t offsetBits = 0;
    size_t sizeBits = 0;
    bool isConst = false;
    bool isBitfield = false;
};

struct MethodMetadata {
    std::string name;
    std::string signature;
};

struct TypeMetadata {
    std::string id;
    std::string name;
    std::string sourceFile;
    std::string kind;
    size_t size = 0;
    size_t alignment = 0;
    std::vector<FieldMetadata> fields;
    std::vector<MethodMetadata> methods;
};

struct PersistentGlobal {
    std::string abiType;
    VariableMetadata reflection;
};

struct GlobalStage {
    std::string type;
    bool common = false;
    bool weakODR = false;
    VariableMetadata reflection;
};

struct Compilation {
    std::vector<std::unique_ptr<llvm::LLVMContext>> contexts;
    std::vector<std::unique_ptr<llvm::Module>> modules;
    std::vector<fs::path> moduleSources;
    std::vector<FunctionUpdate> functions;
    std::map<std::string, std::string> dispatchNames;
    std::map<std::string, VariableMetadata> variableMetadata;
    std::map<std::string, TypeMetadata> types;
};

static void usage(std::ostream &out) {
    out << "Usage: livec [options] main.cpp\n"
           "\n"
           "JIT-compiles C++20 project sources and watches the project directory.\n"
           "Every defined C++ function is automatically routed through reloadable\n"
           "dispatch. No application-side listener or reload annotation is needed.\n"
           "\n"
           "Options:\n"
            "  --watch-root DIR       Watch and discover C++ sources under DIR\n"
            "  --clang-arg ARG        Pass ARG to the C++ compiler (repeatable)\n"
           "  --link-library PATH    Load a native dynamic library (repeatable)\n"
           "  -q, --quiet            Suppress informational runtime messages\n"
           "  -h, --help             Show this help\n";
}

static bool parseOptions(int argc, char **argv, Options &options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            usage(std::cout);
            std::exit(0);
        } else if (arg == "--quiet" || arg == "-q") {
            options.quiet = true;
        } else if (arg == "--watch-root" && i + 1 < argc) {
            options.watchRoot = argv[++i];
        } else if (arg == "--clang-arg" && i + 1 < argc) {
            options.clangArgs.emplace_back(argv[++i]);
        } else if (arg == "--link-library" && i + 1 < argc) {
            options.dynamicLibraries.emplace_back(argv[++i]);
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "[LiveC] Unknown or incomplete option: " << arg << '\n';
            usage(std::cerr);
            return false;
        } else if (options.entry.empty()) {
            options.entry = arg;
        } else {
            std::cerr << "[LiveC] Source discovery is automatic; specify only main.cpp.\n";
            usage(std::cerr);
            return false;
        }
    }

    if (options.entry.empty()) {
        usage(std::cerr);
        return false;
    }
    return true;
}

static bool pathIsWithin(const fs::path &path, const fs::path &root) {
    auto p = path.begin();
    auto r = root.begin();
    for (; r != root.end(); ++r, ++p) {
        if (p == path.end() || *p != *r) return false;
    }
    return true;
}

static std::vector<fs::path> scanProject(const fs::path &root) {
    std::vector<fs::path> sources;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    while (!ec && it != end) {
        const fs::path path = it->path();
        if (it->is_directory(ec)) {
            const std::string name = path.filename().string();
            if ((!name.empty() && name[0] == '.') || name == "build" || name == "target") {
                it.disable_recursion_pending();
            }
        } else if (it->is_regular_file(ec) &&
                   (path.extension() == ".cpp" || path.extension() == ".cc" ||
                    path.extension() == ".cxx")) {
            sources.push_back(fs::absolute(path).lexically_normal());
        }
        it.increment(ec);
    }
    std::sort(sources.begin(), sources.end());
    return sources;
}

static std::map<fs::path, std::string> snapshotProject(const fs::path &root) {
    std::map<fs::path, std::string> snapshot;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    while (!ec && it != end) {
        const fs::path path = it->path();
        if (it->is_directory(ec)) {
            const std::string name = path.filename().string();
            if ((!name.empty() && name[0] == '.') || name == "build" || name == "target") {
                it.disable_recursion_pending();
            }
        } else if (it->is_regular_file(ec)) {
            const std::string ext = path.extension().string();
            if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
                ext == ".h" || ext == ".hpp" || ext == ".hh" ||
                ext == ".hxx" || ext == ".inc") {
                std::ifstream input(path, std::ios::binary);
                std::ostringstream contents;
                contents << input.rdbuf();
                snapshot[fs::absolute(path).lexically_normal()] = contents.str();
            }
        }
        it.increment(ec);
    }
    return snapshot;
}

static uint64_t hashString(const std::string &value) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static std::string hexHash(const std::string &value) {
    std::ostringstream out;
    out << std::hex << hashString(value);
    return out.str();
}

static std::string symbolPart(const std::string &value) {
    std::string result;
    for (unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_') {
            result.push_back(static_cast<char>(ch));
        } else {
            static constexpr char digits[] = "0123456789abcdef";
            result.push_back('_');
            result.push_back(digits[ch >> 4]);
            result.push_back(digits[ch & 0xf]);
        }
    }
    return result;
}

static std::string typeFingerprint(const llvm::Type *type,
                                   std::unordered_set<const llvm::Type *> &seen) {
    if (!seen.insert(type).second) return "rec";
    std::string result;
    llvm::raw_string_ostream out(result);
    if (type->isIntegerTy()) {
        out << "i" << type->getIntegerBitWidth();
    } else if (type->isPointerTy()) {
        out << "ptr" << type->getPointerAddressSpace();
    } else if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type)) {
        out << "array(" << array->getNumElements() << ","
            << typeFingerprint(array->getElementType(), seen) << ")";
    } else if (auto *vector = llvm::dyn_cast<llvm::VectorType>(type)) {
        auto count = vector->getElementCount();
        out << (count.isScalable() ? "scalable" : "vector") << count.getKnownMinValue()
            << "(" << typeFingerprint(vector->getElementType(), seen) << ")";
    } else if (auto *structure = llvm::dyn_cast<llvm::StructType>(type)) {
        out << "struct(" << structure->isPacked() << ",";
        if (structure->isOpaque()) {
            out << "opaque";
        } else {
            for (llvm::Type *element : structure->elements()) {
                out << typeFingerprint(element, seen) << ";";
            }
        }
        out << ")";
    } else if (auto *function = llvm::dyn_cast<llvm::FunctionType>(type)) {
        out << "fn(" << typeFingerprint(function->getReturnType(), seen) << ";";
        for (llvm::Type *param : function->params()) out << typeFingerprint(param, seen) << ";";
        out << (function->isVarArg() ? "vararg" : "fixed") << ")";
    } else {
        type->print(out);
    }
    return out.str();
}

static std::string typeFingerprint(const llvm::Type *type) {
    std::unordered_set<const llvm::Type *> seen;
    return typeFingerprint(type, seen);
}

static std::string functionFingerprint(const llvm::Function &function) {
    std::string attrs;
    llvm::raw_string_ostream out(attrs);
    function.getAttributes().print(out);
    return typeFingerprint(function.getFunctionType()) + ":cc=" +
           std::to_string(function.getCallingConv()) + ":attrs=" + out.str();
}

static std::string sourcePath(const fs::path &source);

static std::string functionABIKey(const std::string &linkName,
                                  const llvm::Function &function,
                                  const fs::path &source) {
    std::string attributes;
    const auto functionAttributes = function.getAttributes();
    attributes = functionAttributes.getRetAttrs().getAsString();
    for (unsigned index = 0; index < function.arg_size(); ++index) {
        attributes += "|";
        attributes += functionAttributes.getParamAttrs(index).getAsString();
    }

    std::string key;
    if (function.hasLocalLinkage()) key = sourcePath(source) + "\n";
    key += linkName + "\n" + typeFingerprint(function.getFunctionType()) +
           "\ncc=" + std::to_string(function.getCallingConv()) + "\n" + attributes;
    return key;
}

static std::string stableFunctionName(const std::string &linkName,
                                      const std::string &dispatchKey) {
    if (linkName == "main") return "main";
    return "__livec_fn_" + hexHash(dispatchKey);
}

static std::string debugTypeName(const llvm::DIType *type, unsigned depth = 0) {
    if (!type) return "void";
    if (depth > 12) return "...";
    if (auto *basic = llvm::dyn_cast<llvm::DIBasicType>(type)) {
        if (!basic->getName().empty()) return basic->getName().str();
    }
    if (auto *derived = llvm::dyn_cast<llvm::DIDerivedType>(type)) {
        const std::string base = debugTypeName(derived->getBaseType(), depth + 1);
        switch (derived->getTag()) {
        case llvm::dwarf::DW_TAG_pointer_type: return base + " *";
        case llvm::dwarf::DW_TAG_const_type: return "const " + base;
        case llvm::dwarf::DW_TAG_volatile_type: return "volatile " + base;
        case llvm::dwarf::DW_TAG_restrict_type: return "restrict " + base;
        case llvm::dwarf::DW_TAG_typedef:
            return derived->getName().empty() ? base : derived->getName().str();
        case llvm::dwarf::DW_TAG_array_type: return base + "[]";
        case llvm::dwarf::DW_TAG_reference_type: return base + " &";
        case llvm::dwarf::DW_TAG_rvalue_reference_type: return base + " &&";
        default:
            if (!derived->getName().empty()) return derived->getName().str();
            return base;
        }
    }
    if (auto *composite = llvm::dyn_cast<llvm::DICompositeType>(type)) {
        std::string prefix;
        switch (composite->getTag()) {
        case llvm::dwarf::DW_TAG_structure_type: prefix = "struct "; break;
        case llvm::dwarf::DW_TAG_union_type: prefix = "union "; break;
        case llvm::dwarf::DW_TAG_enumeration_type: prefix = "enum "; break;
        default: break;
        }
        return prefix + (composite->getName().empty() ? "<anonymous>" : composite->getName().str());
    }
    if (!type->getName().empty()) return type->getName().str();
    return "unknown";
}

static bool debugTypeIsConst(const llvm::DIType *type) {
    while (auto *derived = llvm::dyn_cast_or_null<llvm::DIDerivedType>(type)) {
        if (derived->getTag() == llvm::dwarf::DW_TAG_const_type) return true;
        type = derived->getBaseType();
    }
    return false;
}

static std::string debugFunctionSignature(const llvm::Function &function) {
    llvm::DISubprogram *subprogram = function.getSubprogram();
    llvm::DISubroutineType *subroutine = subprogram ? subprogram->getType() : nullptr;
    if (!subroutine) return typeFingerprint(function.getFunctionType());

    llvm::DITypeRefArray types = subroutine->getTypeArray();
    std::string result = types.size() ? debugTypeName(types[0]) : "void";
    result += " (";
    bool first = true;
    const size_t argumentCount = function.arg_size();
    for (size_t index = 0; index < argumentCount; ++index) {
        if (!first) result += ", ";
        const size_t debugIndex = index + 1;
        result += debugIndex < types.size() && types[debugIndex]
            ? debugTypeName(types[debugIndex])
            : "unknown";
        first = false;
    }
    if (function.isVarArg()) {
        if (!first) result += ", ";
        result += "...";
    } else if (first) {
        result += "void";
    }
    result += ")";
    return result;
}

static std::string debugSubroutineSignature(const llvm::DISubroutineType &subroutine) {
    llvm::DITypeRefArray types = subroutine.getTypeArray();
    if (types.size() == 0) return "unknown";
    std::string result = debugTypeName(types[0]) + " (";
    bool first = true;
    for (unsigned index = 1; index < types.size(); ++index) {
        if (!types[index]) continue;
        if (!first) result += ", ";
        result += debugTypeName(types[index]);
        first = false;
    }
    if (first) result += "void";
    result += ")";
    return result;
}

static std::string sourcePath(const fs::path &source) {
    return fs::absolute(source).lexically_normal().string();
}

static std::string debugFilePath(const llvm::DIFile *file, const fs::path &fallback) {
    if (!file) return sourcePath(fallback);
    fs::path path(file->getFilename().str());
    if (path.is_relative() && !file->getDirectory().empty())
        path = fs::path(file->getDirectory().str()) / path;
    return fs::absolute(path).lexically_normal().string();
}

static std::string qualifiedScopeName(llvm::DIScope *scope) {
    std::vector<std::string> names;
    while (scope) {
        if (!scope->getName().empty()) names.push_back(scope->getName().str());
        auto *parent = scope->getScope();
        if (parent == scope) break;
        scope = parent;
    }
    std::string result;
    for (auto it = names.rbegin(); it != names.rend(); ++it) {
        if (!result.empty()) result += "::";
        result += *it;
    }
    return result;
}

static std::string qualifiedFunctionName(const llvm::DISubprogram &subprogram) {
    const std::string scope = qualifiedScopeName(subprogram.getScope());
    if (scope.empty()) return subprogram.getName().str();
    return scope + "::" + subprogram.getName().str();
}

static std::string qualifiedTypeName(const llvm::DICompositeType &type) {
    const std::string scope = qualifiedScopeName(type.getScope());
    if (scope.empty()) return type.getName().str();
    return scope + "::" + type.getName().str();
}

static std::string debugTypeKind(const llvm::DICompositeType &type) {
    switch (type.getTag()) {
    case llvm::dwarf::DW_TAG_class_type: return "class";
    case llvm::dwarf::DW_TAG_structure_type: return "struct";
    case llvm::dwarf::DW_TAG_union_type: return "union";
    case llvm::dwarf::DW_TAG_enumeration_type: return "enum";
    default: return "type";
    }
}

static TypeMetadata describeType(const llvm::DICompositeType &type,
                                 const fs::path &fallbackSource,
                                 const llvm::Module &module) {
    TypeMetadata result;
    result.name = qualifiedTypeName(type);
    result.sourceFile = debugFilePath(type.getFile(), fallbackSource);
    result.kind = debugTypeKind(type);
    result.size = static_cast<size_t>((type.getSizeInBits() + 7) / 8);
    result.alignment = static_cast<size_t>((type.getAlignInBits() + 7) / 8);
    for (llvm::StructType *structure : module.getIdentifiedStructTypes()) {
        if (!structure->hasName() || structure->isOpaque()) continue;
        const std::string llvmName = structure->getName().str();
        const std::string debugName = type.getName().str();
        if (llvmName.find(debugName) == std::string::npos) continue;
        result.size = module.getDataLayout().getTypeAllocSize(structure).getFixedValue();
        result.alignment = module.getDataLayout().getABITypeAlign(structure).value();
        break;
    }
    result.id = type.getIdentifier().empty()
        ? result.sourceFile + "::" + result.name
        : type.getIdentifier().str();

    for (llvm::DINode *element : type.getElements()) {
        if (auto *field = llvm::dyn_cast<llvm::DIDerivedType>(element)) {
            const unsigned tag = field->getTag();
            if (tag != llvm::dwarf::DW_TAG_member && tag != llvm::dwarf::DW_TAG_inheritance) continue;
            const bool isInheritance = tag == llvm::dwarf::DW_TAG_inheritance;
            result.fields.push_back({
                isInheritance ? "<base>" : field->getName().str(),
                debugTypeName(field->getBaseType()),
                static_cast<size_t>(field->getOffsetInBits()),
                static_cast<size_t>(field->getSizeInBits()),
                debugTypeIsConst(field->getBaseType()),
                field->isBitField()
            });
        } else if (auto *method = llvm::dyn_cast<llvm::DISubprogram>(element)) {
            const std::string signature = method->getType()
                ? debugSubroutineSignature(*method->getType()) : "unknown";
            result.methods.push_back({qualifiedFunctionName(*method), signature});
        }
    }
    return result;
}

static bool sameTypeLayout(const TypeMetadata &left, const TypeMetadata &right) {
    if (left.size != right.size || left.alignment != right.alignment ||
        left.fields.size() != right.fields.size()) return false;
    for (size_t index = 0; index < left.fields.size(); ++index) {
        const auto &a = left.fields[index];
        const auto &b = right.fields[index];
        if (a.name != b.name || a.type != b.type || a.offsetBits != b.offsetBits ||
            a.sizeBits != b.sizeBits || a.isConst != b.isConst ||
            a.isBitfield != b.isBitfield) return false;
    }
    return true;
}

static bool isODRWeak(const llvm::GlobalValue &value) {
    return value.hasWeakODRLinkage() || value.hasLinkOnceODRLinkage() ||
           value.hasAvailableExternallyLinkage();
}

static bool collectDebugType(llvm::DIType *type, const fs::path &fallbackSource,
                             const llvm::Module &module,
                             const fs::path &root, std::map<std::string, TypeMetadata> &types,
                             std::set<const llvm::DIType *> &visited) {
    if (!type || !visited.insert(type).second) return true;
    if (auto *composite = llvm::dyn_cast<llvm::DICompositeType>(type)) {
        if (!composite->isForwardDecl() &&
            (!composite->getName().empty() || !composite->getIdentifier().empty())) {
            TypeMetadata metadata = describeType(*composite, fallbackSource, module);
            if (pathIsWithin(fs::path(metadata.sourceFile), root)) {
                auto [existing, inserted] = types.emplace(metadata.id, metadata);
                if (!inserted && !sameTypeLayout(existing->second, metadata)) {
                    std::cerr << "[LiveC] Conflicting debug layouts for " << metadata.name
                              << " (" << metadata.id << ") size " << existing->second.size
                              << " vs " << metadata.size << ".\n";
                    return false;
                }
            }
        }
        for (llvm::DINode *element : composite->getElements()) {
            if (auto *member = llvm::dyn_cast<llvm::DIDerivedType>(element)) {
                if (!collectDebugType(member->getBaseType(), fallbackSource, module, root, types, visited)) return false;
            } else if (auto *method = llvm::dyn_cast<llvm::DISubprogram>(element)) {
                if (auto *subroutine = method->getType()) {
                    for (llvm::DIType *parameterType : subroutine->getTypeArray()) {
                        if (!collectDebugType(parameterType, fallbackSource, module, root, types, visited)) return false;
                    }
                }
            }
        }
    } else if (auto *derived = llvm::dyn_cast<llvm::DIDerivedType>(type)) {
        return collectDebugType(derived->getBaseType(), fallbackSource, module, root, types, visited);
    } else if (auto *subroutine = llvm::dyn_cast<llvm::DISubroutineType>(type)) {
        for (llvm::DIType *elementType : subroutine->getTypeArray()) {
            if (!collectDebugType(elementType, fallbackSource, module, root, types, visited)) return false;
        }
    }
    return true;
}

static VariableMetadata describeGlobal(const llvm::GlobalVariable &global,
                                       const fs::path &source,
                                       const fs::path &projectRoot,
                                       llvm::DataLayout const &layout) {
    VariableMetadata result;
    result.name = global.getName().str();
    result.type = typeFingerprint(global.getValueType());
    result.sourceFile = sourcePath(source);
    result.isConst = global.isConstant();
    result.reflectable = false;
    if (global.getValueType()->isSized()) {
        result.size = layout.getTypeAllocSize(global.getValueType()).getFixedValue();
    }

    llvm::SmallVector<llvm::DIGlobalVariableExpression *, 1> debugInfo;
    global.getDebugInfo(debugInfo);
    if (!debugInfo.empty()) {
        if (llvm::DIGlobalVariable *variable = debugInfo.front()->getVariable()) {
            if (!variable->getName().empty()) result.name = variable->getName().str();
            result.line = variable->getLine();
            result.sourceFile = debugFilePath(variable->getFile(), source);
            const bool compilerGenerated = variable->getName().starts_with("_vtable$") ||
                llvm::StringRef(global.getName()).starts_with("_ZTV") ||
                llvm::StringRef(global.getName()).starts_with("_ZTI") ||
                llvm::StringRef(global.getName()).starts_with("_ZTS");
            result.reflectable = !compilerGenerated &&
                pathIsWithin(fs::path(result.sourceFile), projectRoot);
            if (llvm::DIType *debugType = variable->getType()) {
                result.type = debugTypeName(debugType);
                result.isConst = result.isConst || debugTypeIsConst(debugType);
            }
            if (auto bits = variable->getSizeInBits()) result.size = (*bits + 7) / 8;
        }
    }
    return result;
}

static int runProcess(const std::vector<std::string> &arguments) {
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int spawnError = posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
    if (spawnError != 0) {
        std::cerr << "[LiveC] Could not start " << arguments[0] << ": " << std::strerror(spawnError) << '\n';
        return -1;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            std::cerr << "[LiveC] Failed waiting for compiler: " << std::strerror(errno) << '\n';
            return -1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static std::string clangPath() {
    const char *configured = std::getenv("LIVEC_CLANG");
    return configured && *configured ? configured : LIVEC_CLANG_DEFAULT;
}

static std::string cxxIncludePath() {
    const char *configured = std::getenv("LIVEC_CXX_INCLUDE");
    return configured && *configured ? configured : LIVEC_CXX_INCLUDE_DEFAULT;
}

static bool compileToBitcode(const fs::path &source, const fs::path &output,
                             const fs::path &root,
                             const std::vector<std::string> &clangArgs) {
    std::vector<std::string> command = {
        clangPath(), "-x", "c++", "-std=c++20", "-O0", "-g",
        "-D_LIBCPP_NO_ABI_TAG", "-fno-use-cxa-atexit",
        "-fkeep-persistent-storage-variables", "-Xclang", "-femit-all-decls",
        "-Xclang", "-fno-eliminate-unused-debug-types",
        "-emit-llvm", "-c",
        "-I", root.string()
    };
    const std::string cxxInclude = cxxIncludePath();
    if (!cxxInclude.empty()) {
        command.push_back("-nostdinc++");
        command.push_back("-isystem");
        command.push_back(cxxInclude);
    }
    const fs::path projectInclude = root / "include";
    if (fs::is_directory(projectInclude)) {
        command.push_back("-I");
        command.push_back(projectInclude.string());
    }
    command.insert(command.end(), clangArgs.begin(), clangArgs.end());
    command.push_back(source.string());
    command.push_back("-o");
    command.push_back(output.string());
    return runProcess(command) == 0;
}

static std::string staticGlobalName(const fs::path &source, const std::string &name) {
    return "__livec_static_global_" + hexHash(source.string()) + "_" + symbolPart(name);
}

class LiveRuntime {
public:
    explicit LiveRuntime(fs::path root) : root(std::move(root)) {}

    bool initialize() {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        auto created = orc::LLJITBuilder().create();
        if (!created) return logError("creating ORC JIT", created.takeError());
        jit = std::move(*created);

        orc::SymbolMap runtimeSymbols;
        runtimeSymbols[jit->mangleAndIntern("__livec_function_is_stale")] = {
            llvm::orc::ExecutorAddr::fromPtr(&livec_function_is_stale),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable
        };
        runtimeSymbols[jit->mangleAndIntern("livec_reflect_function_count")] = {
            llvm::orc::ExecutorAddr::fromPtr(&livec_reflect_function_count),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable
        };
        runtimeSymbols[jit->mangleAndIntern("livec_reflect_variable_count")] = {
            llvm::orc::ExecutorAddr::fromPtr(&livec_reflect_variable_count),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable
        };
        runtimeSymbols[jit->mangleAndIntern("livec_reflect_function_at")] = {
            llvm::orc::ExecutorAddr::fromPtr(&livec_reflect_function_at),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable
        };
        runtimeSymbols[jit->mangleAndIntern("livec_reflect_variable_at")] = {
            llvm::orc::ExecutorAddr::fromPtr(&livec_reflect_variable_at),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable
        };
        runtimeSymbols[jit->mangleAndIntern("livec_reflect_function_find")] = {
            llvm::orc::ExecutorAddr::fromPtr(&livec_reflect_function_find),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable
        };
        runtimeSymbols[jit->mangleAndIntern("livec_reflect_function_find_signature")] = {
            llvm::orc::ExecutorAddr::fromPtr(&livec_reflect_function_find_signature),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable
        };
        runtimeSymbols[jit->mangleAndIntern("livec_reflect_variable_find")] = {
            llvm::orc::ExecutorAddr::fromPtr(&livec_reflect_variable_find),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable
        };
        runtimeSymbols[jit->mangleAndIntern("livec_reflect_type_count")] = {
            llvm::orc::ExecutorAddr::fromPtr(&livec_reflect_type_count),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable
        };
        runtimeSymbols[jit->mangleAndIntern("livec_reflect_type_at")] = {
            llvm::orc::ExecutorAddr::fromPtr(&livec_reflect_type_at),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable
        };
        runtimeSymbols[jit->mangleAndIntern("livec_reflect_type_find")] = {
            llvm::orc::ExecutorAddr::fromPtr(&livec_reflect_type_find),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable
        };
        runtimeSymbols[jit->mangleAndIntern("livec_reflect_type_field_count")] = {
            llvm::orc::ExecutorAddr::fromPtr(&livec_reflect_type_field_count),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable
        };
        runtimeSymbols[jit->mangleAndIntern("livec_reflect_type_field_at")] = {
            llvm::orc::ExecutorAddr::fromPtr(&livec_reflect_type_field_at),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable
        };
        runtimeSymbols[jit->mangleAndIntern("livec_reflect_type_method_count")] = {
            llvm::orc::ExecutorAddr::fromPtr(&livec_reflect_type_method_count),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable
        };
        runtimeSymbols[jit->mangleAndIntern("livec_reflect_type_method_at")] = {
            llvm::orc::ExecutorAddr::fromPtr(&livec_reflect_type_method_at),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable
        };
        if (auto error = jit->getMainJITDylib().define(orc::absoluteSymbols(std::move(runtimeSymbols)))) {
            return logError("registering main-loop safepoint", std::move(error));
        }

        auto generator = orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit->getDataLayout().getGlobalPrefix());
        if (!generator) return logError("installing process symbol lookup", generator.takeError());
        jit->getMainJITDylib().addGenerator(std::move(*generator));

        auto builder = orc::createLocalIndirectStubsManagerBuilder(jit->getTargetTriple());
        stubs = builder();
        if (!stubs) {
            std::cerr << "[LiveC] Could not create the native dispatch-stub manager.\n";
            return false;
        }
        for (const auto &library : dynamicLibraries) {
            auto loaded = jit->loadPlatformDynamicLibrary(library.c_str());
            if (!loaded) return logError("loading library " + library.string(), loaded.takeError());
        }
        return true;
    }

    void setLibraries(std::vector<fs::path> libraries) { dynamicLibraries = std::move(libraries); }

    bool compileAndInstall(const std::vector<fs::path> &sources) {
        if (sources.empty()) {
            std::cerr << "[LiveC] No C++ source files found under " << root << '\n';
            return false;
        }
        ++generation;
        Compilation candidate;
        std::set<std::string> definitions;
        std::map<std::string, GlobalStage> stagedGlobalDefinitions;
        std::map<std::string, std::string> stagedLayouts;

        for (const auto &source : sources) {
            const fs::path bitcode = scratchDirectory / ("unit-" + std::to_string(generation) +
                "-" + hexHash(source.string()) + ".bc");
            if (!compileToBitcode(source, bitcode, root, clangArgs)) {
                --generation;
                return false;
            }

            auto context = std::make_unique<llvm::LLVMContext>();
            llvm::SMDiagnostic diagnostic;
            auto module = llvm::parseIRFile(bitcode.string(), diagnostic, *context);
            if (!module) {
                diagnostic.print("[LiveC]", llvm::errs());
                --generation;
                return false;
            }
            module->setDataLayout(jit->getDataLayout());
            module->setTargetTriple(jit->getTargetTriple());
            if (!prepareModule(*module, source, candidate, definitions,
                               stagedGlobalDefinitions, stagedLayouts)) {
                --generation;
                return false;
            }
            candidate.contexts.push_back(std::move(context));
            candidate.modules.push_back(std::move(module));
            candidate.moduleSources.push_back(source);
        }

        rewriteFunctionDeclarations(candidate);

        for (const auto &[name, layout] : stagedLayouts) {
            std::shared_lock metadataLock(reflectionMutex);
            auto previous = typeLayouts.find(name);
            if (previous != typeLayouts.end() && previous->second != layout) {
                std::cerr << "[LiveC] Rejecting type-layout change for " << name
                          << "; existing objects retain their old C++ layout.\n";
                --generation;
                return false;
            }
        }
        for (const auto &[id, metadata] : candidate.types) {
            std::shared_lock metadataLock(reflectionMutex);
            auto previous = typeMetadata.find(id);
            if (previous != typeMetadata.end() && !sameTypeLayout(previous->second, metadata)) {
                std::cerr << "[LiveC] Rejecting C++ type layout change for " << metadata.name << ".\n";
                --generation;
                return false;
            }
        }
        for (const auto &[name, stage] : stagedGlobalDefinitions) {
            std::shared_lock metadataLock(reflectionMutex);
            auto previous = persistentGlobals.find(name);
            if (previous != persistentGlobals.end() && previous->second.abiType != stage.type) {
                std::cerr << "[LiveC] Rejecting incompatible change to persistent global " << name << ".\n";
                --generation;
                return false;
            }
        }
        if (!installNewStubs(candidate.functions)) {
            --generation;
            return false;
        }

        auto tracker = jit->getMainJITDylib().createResourceTracker();
        for (size_t i = 0; i < candidate.modules.size(); ++i) {
            orc::ThreadSafeModule threadSafeModule(
                std::move(candidate.modules[i]), std::move(candidate.contexts[i]));
            if (auto error = jit->addIRModule(tracker, std::move(threadSafeModule))) {
                logError("adding a C++ module to ORC", std::move(error));
                if (auto removalError = tracker->remove()) llvm::consumeError(std::move(removalError));
                --generation;
                return false;
            }
        }

        std::vector<std::pair<std::string, llvm::orc::ExecutorAddr>> resolved;
        resolved.reserve(candidate.functions.size());
        for (const auto &function : candidate.functions) {
            if (!function.publish) continue;
            auto address = jit->lookup(function.implementationName);
            if (!address) {
                logError("JIT-compiling " + function.stableName, address.takeError());
                if (auto removalError = tracker->remove()) llvm::consumeError(std::move(removalError));
                --generation;
                return false;
            }
            resolved.emplace_back(function.stableName, *address);
        }

        publicationInProgress.store(true, std::memory_order_release);
        for (const auto &[name, address] : resolved) {
            if (auto error = stubs->updatePointer(name, address)) {
                publicationInProgress.store(false, std::memory_order_release);
                logError("publishing " + name, std::move(error));
                if (auto removalError = tracker->remove()) llvm::consumeError(std::move(removalError));
                --generation;
                return false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(functionVersionMutex);
            for (const auto &function : candidate.functions) {
                if (function.publish) activeFunctionVersions[hashString(function.stableName)] = generation;
            }
        }
        publicationInProgress.store(false, std::memory_order_release);

        {
            std::unique_lock metadataLock(reflectionMutex);
            for (const auto &[name, global] : stagedGlobalDefinitions) {
                persistentGlobals.emplace(name, PersistentGlobal{global.type, global.reflection});
            }
            for (const auto &[name, metadata] : candidate.variableMetadata) {
                auto current = persistentGlobals.find(name);
                if (current != persistentGlobals.end()) current->second.reflection = metadata;
            }
            for (const auto &[name, layout] : stagedLayouts) typeLayouts.emplace(name, layout);
            activeFunctions.clear();
            for (const auto &function : candidate.functions) {
                functionSignatures[function.stableName] = function.signature;
                functionBodies[function.stableName] = function.body;
                functionMetadata[function.stableName] = {
                    function.displayName, function.displaySignature, function.sourceFile, function.line
                };
                if (function.reflectable) activeFunctions.insert(function.stableName);
            }
            activeTypes.clear();
            for (const auto &[id, metadata] : candidate.types) {
                typeMetadata[id] = metadata;
                activeTypes.insert(id);
            }
            for (const auto &[key, stableName] : candidate.dispatchNames)
                functionDispatchNames[key] = stableName;
        }
        if (generation == 1) {
            if (auto error = jit->initialize(jit->getMainJITDylib())) {
                return logError("running C++ static initializers", std::move(error));
            }
            initializersRan = true;
        }
        trackers.push_back(std::move(tracker));
        std::ostringstream message;
        message << "JIT installed version " << generation << " ("
                  << candidate.functions.size() << " functions, " << sources.size() << " C++ files).";
        logInfo(message.str());
        return true;
    }

    int runMain() {
        auto entry = jit->lookup("main");
        if (!entry) {
            logError("looking up C++ main", entry.takeError());
            return 2;
        }
        using MainFunction = int (*)(void);
        MainFunction main = entry->toPtr<MainFunction>();
        return main();
    }

    void shutdown() {
        if (initializersRan) {
            if (auto error = jit->deinitialize(jit->getMainJITDylib())) {
                logError("running C++ static destructors", std::move(error));
            }
            initializersRan = false;
        }
    }

    void setClangArgs(std::vector<std::string> args) { clangArgs = std::move(args); }

    size_t reflectedFunctionCount() const {
        std::shared_lock lock(reflectionMutex);
        return activeFunctions.size();
    }

    size_t reflectedVariableCount() const {
        std::shared_lock lock(reflectionMutex);
        return std::count_if(persistentGlobals.begin(), persistentGlobals.end(),
                             [](const auto &entry) { return entry.second.reflection.reflectable; });
    }

    int reflectedFunctionAt(size_t index, livec_function_info *out) {
        if (!out) return 0;
        std::shared_lock lock(reflectionMutex);
        auto item = activeFunctions.begin();
        std::advance(item, std::min(index, activeFunctions.size()));
        if (item == activeFunctions.end() || index >= activeFunctions.size()) return 0;
        auto metadata = functionMetadata.find(*item);
        if (metadata == functionMetadata.end()) return 0;
        auto stub = stubs->findStub(*item, false);
        out->name = metadata->second.name.c_str();
        out->signature = metadata->second.signature.c_str();
        out->source_file = metadata->second.sourceFile.c_str();
        out->line = metadata->second.line;
        {
            std::lock_guard versionLock(functionVersionMutex);
            auto version = activeFunctionVersions.find(hashString(*item));
            out->version = version == activeFunctionVersions.end() ? 0 : version->second;
        }
        out->entry_point = stub.getAddress().toPtr<void (*)(void)>();
        return 1;
    }

    int findReflectedFunction(const char *name, const char *sourceFile,
                              livec_function_info *out) {
        return findReflectedFunctionSignature(name, nullptr, sourceFile, out);
    }

    int findReflectedFunctionSignature(const char *name, const char *signature,
                                       const char *sourceFile, livec_function_info *out) {
        if (!name || !out) return 0;
        std::shared_lock lock(reflectionMutex);
        for (const auto &stableName : activeFunctions) {
            auto metadata = functionMetadata.find(stableName);
            if (metadata == functionMetadata.end() || metadata->second.name != name) continue;
            if (signature && metadata->second.signature != signature) continue;
            if (sourceFile && metadata->second.sourceFile != sourceFile) continue;
            return fillFunctionInfo(stableName, out);
        }
        return 0;
    }

    int reflectedVariableAt(size_t index, livec_variable_info *out) {
        if (!out) return 0;
        std::shared_lock lock(reflectionMutex);
        auto item = persistentGlobals.begin();
        for (; item != persistentGlobals.end(); ++item) {
            if (!item->second.reflection.reflectable) continue;
            if (index == 0) break;
            --index;
        }
        if (item == persistentGlobals.end()) return 0;
        auto address = jit->lookup(item->first);
        if (!address) {
            llvm::consumeError(address.takeError());
            return 0;
        }
        fillVariableInfo(item->second.reflection, address->toPtr<void *>(), out);
        return 1;
    }

    int findReflectedVariable(const char *name, const char *sourceFile,
                              livec_variable_info *out) {
        if (!name || !out) return 0;
        std::shared_lock lock(reflectionMutex);
        for (const auto &[stableName, global] : persistentGlobals) {
            const auto &metadata = global.reflection;
            if (!metadata.reflectable || metadata.name != name) continue;
            if (sourceFile && metadata.sourceFile != sourceFile) continue;
            auto address = jit->lookup(stableName);
            if (!address) {
                llvm::consumeError(address.takeError());
                return 0;
            }
            fillVariableInfo(metadata, address->toPtr<void *>(), out);
            return 1;
        }
        return 0;
    }

    size_t reflectedTypeCount() const {
        std::shared_lock lock(reflectionMutex);
        return activeTypes.size();
    }

    int reflectedTypeAt(size_t index, livec_type_info *out) const {
        if (!out) return 0;
        std::shared_lock lock(reflectionMutex);
        auto item = activeTypes.begin();
        std::advance(item, std::min(index, activeTypes.size()));
        if (item == activeTypes.end() || index >= activeTypes.size()) return 0;
        auto metadata = typeMetadata.find(*item);
        if (metadata == typeMetadata.end()) return 0;
        fillTypeInfo(*item, metadata->second, out);
        return 1;
    }

    int findReflectedType(const char *name, const char *sourceFile,
                          livec_type_info *out) const {
        if (!name || !out) return 0;
        std::shared_lock lock(reflectionMutex);
        for (const auto &id : activeTypes) {
            auto metadata = typeMetadata.find(id);
            if (metadata == typeMetadata.end() || metadata->second.name != name) continue;
            if (sourceFile && metadata->second.sourceFile != sourceFile) continue;
            fillTypeInfo(id, metadata->second, out);
            return 1;
        }
        return 0;
    }

    size_t reflectedTypeFieldCount(const char *typeId) const {
        if (!typeId) return 0;
        std::shared_lock lock(reflectionMutex);
        auto metadata = typeMetadata.find(typeId);
        return metadata == typeMetadata.end() ? 0 : metadata->second.fields.size();
    }

    int reflectedTypeFieldAt(const char *typeId, size_t index, livec_field_info *out) const {
        if (!typeId || !out) return 0;
        std::shared_lock lock(reflectionMutex);
        auto metadata = typeMetadata.find(typeId);
        if (metadata == typeMetadata.end() || index >= metadata->second.fields.size()) return 0;
        const auto &field = metadata->second.fields[index];
        out->name = field.name.c_str();
        out->type = field.type.c_str();
        out->offset_bits = field.offsetBits;
        out->size_bits = field.sizeBits;
        out->is_const = field.isConst ? 1 : 0;
        out->is_bitfield = field.isBitfield ? 1 : 0;
        return 1;
    }

    size_t reflectedTypeMethodCount(const char *typeId) const {
        if (!typeId) return 0;
        std::shared_lock lock(reflectionMutex);
        auto metadata = typeMetadata.find(typeId);
        return metadata == typeMetadata.end() ? 0 : metadata->second.methods.size();
    }

    int reflectedTypeMethodAt(const char *typeId, size_t index, livec_function_info *out) {
        if (!typeId || !out) return 0;
        std::shared_lock lock(reflectionMutex);
        auto metadata = typeMetadata.find(typeId);
        if (metadata == typeMetadata.end() || index >= metadata->second.methods.size()) return 0;
        const auto &method = metadata->second.methods[index];
        for (const auto &stableName : activeFunctions) {
            auto function = functionMetadata.find(stableName);
            if (function != functionMetadata.end() && function->second.name == method.name &&
                function->second.signature == method.signature)
                return fillFunctionInfo(stableName, out);
        }
        return 0;
    }

private:
    void rewriteFunctionDeclarations(Compilation &candidate) {
        std::map<std::string, std::string> dispatchNames;
        {
            std::shared_lock lock(reflectionMutex);
            dispatchNames = functionDispatchNames;
        }
        dispatchNames.insert(candidate.dispatchNames.begin(), candidate.dispatchNames.end());
        for (size_t moduleIndex = 0; moduleIndex < candidate.modules.size(); ++moduleIndex) {
            llvm::Module &module = *candidate.modules[moduleIndex];
            const fs::path &source = candidate.moduleSources[moduleIndex];
            for (llvm::Function &function : module) {
                if (!function.isDeclaration() || function.isIntrinsic()) continue;
                const std::string name = function.getName().str();
                if (name == "main" || llvm::StringRef(name).starts_with("__livec_fn_")) continue;
                auto dispatch = dispatchNames.find(functionABIKey(name, function, source));
                if (dispatch != dispatchNames.end()) function.setName(dispatch->second);
            }
        }
    }

    int fillFunctionInfo(const std::string &stableName, livec_function_info *out) {
        auto metadata = functionMetadata.find(stableName);
        if (metadata == functionMetadata.end()) return 0;
        auto stub = stubs->findStub(stableName, false);
        out->name = metadata->second.name.c_str();
        out->signature = metadata->second.signature.c_str();
        out->source_file = metadata->second.sourceFile.c_str();
        out->line = metadata->second.line;
        {
            std::lock_guard versionLock(functionVersionMutex);
            auto version = activeFunctionVersions.find(hashString(stableName));
            out->version = version == activeFunctionVersions.end() ? 0 : version->second;
        }
        out->entry_point = stub.getAddress().toPtr<void (*)(void)>();
        return 1;
    }

    static void fillTypeInfo(const std::string &id, const TypeMetadata &metadata,
                             livec_type_info *out) {
        out->id = id.c_str();
        out->name = metadata.name.c_str();
        out->source_file = metadata.sourceFile.c_str();
        out->kind = metadata.kind.c_str();
        out->size = metadata.size;
        out->alignment = metadata.alignment;
    }

    static void fillVariableInfo(const VariableMetadata &metadata, const void *address,
                                 livec_variable_info *out) {
        out->name = metadata.name.c_str();
        out->type = metadata.type.c_str();
        out->source_file = metadata.sourceFile.c_str();
        out->line = metadata.line;
        out->size = metadata.size;
        out->is_const = metadata.isConst ? 1 : 0;
        out->address = const_cast<void *>(address);
    }

    static bool logError(const std::string &action, llvm::Error error) {
        std::cerr << "[LiveC] Error " << action << ": " << llvm::toString(std::move(error)) << '\n';
        return false;
    }

    static void instrumentLoopSafepoints(llvm::Function &function, llvm::Module &module,
                                         llvm::Function &dispatch, uint64_t functionId,
                                         uint64_t version) {
        if (function.isVarArg() || function.hasFnAttribute(llvm::Attribute::NoReturn) ||
            function.hasFnAttribute(llvm::Attribute::Naked) || function.hasPersonalityFn()) return;
        for (llvm::BasicBlock &block : function) {
            for (llvm::Instruction &instruction : block) {
                auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                llvm::Function *callee = call ? call->getCalledFunction() : nullptr;
                if (!callee) continue;
                const llvm::StringRef name = callee->getName();
                if (name.ends_with("D0Ev") || name.ends_with("D1Ev") || name.ends_with("D2Ev")) return;
            }
        }
        llvm::DominatorTree dominators(function);
        std::vector<std::pair<llvm::BranchInst *, unsigned>> backEdges;
        for (llvm::BasicBlock &block : function) {
            auto *branch = llvm::dyn_cast<llvm::BranchInst>(block.getTerminator());
            if (!branch) continue;
            for (unsigned successor = 0; successor < branch->getNumSuccessors(); ++successor) {
                if (dominators.dominates(branch->getSuccessor(successor), &block)) {
                    backEdges.emplace_back(branch, successor);
                }
            }
        }

        if (backEdges.empty()) return;
        llvm::LLVMContext &context = module.getContext();
        auto *intType = llvm::Type::getInt32Ty(context);
        auto *int64Type = llvm::Type::getInt64Ty(context);
        auto *pollType = llvm::FunctionType::get(intType, {int64Type, int64Type}, false);
        llvm::FunctionCallee poll = module.getOrInsertFunction("__livec_function_is_stale", pollType);
        for (const auto &[branch, successorIndex] : backEdges) {
            llvm::BasicBlock *target = branch->getSuccessor(successorIndex);
            auto *pollBlock = llvm::BasicBlock::Create(context, "livec.reload.poll", &function, target);
            branch->setSuccessor(successorIndex, pollBlock);

            llvm::IRBuilder<> builder(pollBlock);
            llvm::Value *isStale = builder.CreateCall(poll, {
                llvm::ConstantInt::get(int64Type, functionId),
                llvm::ConstantInt::get(int64Type, version)
            });
            llvm::Value *shouldRestart = builder.CreateICmpNE(isStale, llvm::ConstantInt::get(intType, 0));
            auto *restartBlock = llvm::BasicBlock::Create(context, "livec.refresh.function", &function);
            builder.CreateCondBr(shouldRestart, restartBlock, target);

            llvm::IRBuilder<> restartBuilder(restartBlock);
            std::vector<llvm::Value *> arguments;
            for (llvm::Argument &argument : function.args()) arguments.push_back(&argument);
            llvm::CallInst *replacement = restartBuilder.CreateCall(
                function.getFunctionType(), &dispatch, arguments);
            replacement->setCallingConv(function.getCallingConv());
            replacement->setAttributes(function.getAttributes());
            replacement->setTailCallKind(llvm::CallInst::TCK_MustTail);
            if (function.getReturnType()->isVoidTy()) {
                restartBuilder.CreateRetVoid();
            } else {
                restartBuilder.CreateRet(replacement);
            }
        }
    }

    static std::string printFunction(const llvm::Function &function) {
        std::string result;
        llvm::raw_string_ostream out(result);
        function.print(out);
        return out.str();
    }

    bool prepareModule(llvm::Module &module, const fs::path &source,
                       Compilation &candidate, std::set<std::string> &definitions,
                       std::map<std::string, GlobalStage> &stagedGlobalDefinitions,
                       std::map<std::string, std::string> &stagedLayouts) {
        const std::string sourceKey = source.lexically_normal().string();
        const std::string sourceHash = hexHash(sourceKey);

        if (generation > 1) {
            for (const char *initializerName : {"llvm.global_ctors", "llvm.global_dtors"}) {
                if (llvm::GlobalVariable *initializers = module.getGlobalVariable(initializerName)) {
                    initializers->eraseFromParent();
                }
            }
        }

        for (llvm::StructType *structure : module.getIdentifiedStructTypes()) {
            if (structure->isOpaque() || !structure->hasName()) continue;
            const std::string identity = sourceHash + ":" + structure->getName().str();
            const std::string layout = typeFingerprint(structure);
            auto [it, inserted] = stagedLayouts.emplace(identity, layout);
            if (!inserted && it->second != layout) {
                std::cerr << "[LiveC] Conflicting record layout for " << identity << ".\n";
                return false;
            }
        }

        std::set<const llvm::DIType *> visitedDebugTypes;
        auto retainDebugType = [&](llvm::DIType *type) {
            return collectDebugType(type, source, module, root, candidate.types, visitedDebugTypes);
        };
    for (llvm::DICompileUnit *unit : module.debug_compile_units()) {
            for (llvm::DIScope *scope : unit->getRetainedTypes()) {
                if (!retainDebugType(llvm::dyn_cast_or_null<llvm::DIType>(scope))) {
                    std::cerr << "[LiveC] Conflicting C++ type layouts in " << source << ".\n";
                    return false;
                }
            }
        }
        for (llvm::Function &function : module) {
            if (llvm::DISubprogram *subprogram = function.getSubprogram()) {
                if (llvm::DISubroutineType *subroutine = subprogram->getType()) {
                    for (llvm::DIType *type : subroutine->getTypeArray()) {
                        if (!retainDebugType(type)) {
                            std::cerr << "[LiveC] Conflicting C++ type layouts in " << source << ".\n";
                            return false;
                        }
                    }
                }
            }
            for (llvm::BasicBlock &block : function) {
                for (llvm::Instruction &instruction : block) {
                    auto *debugVariable = llvm::dyn_cast<llvm::DbgVariableIntrinsic>(&instruction);
                    if (debugVariable && !retainDebugType(debugVariable->getVariable()->getType())) {
                        std::cerr << "[LiveC] Conflicting C++ type layouts in " << source << ".\n";
                        return false;
                    }
                }
            }
        }

        for (const char *usedName : {"llvm.used", "llvm.compiler.used"}) {
            if (llvm::GlobalVariable *used = module.getGlobalVariable(usedName)) {
                used->eraseFromParent();
            }
        }

        for (llvm::GlobalVariable &global : module.globals()) {
            if (!global.hasInitializer()) continue;
            const std::string originalName = global.getName().str();
            if (originalName == "llvm.global_ctors" || originalName == "llvm.global_dtors") continue;
            const bool local = global.hasLocalLinkage();
            llvm::SmallVector<llvm::DIGlobalVariableExpression *, 1> debugInfo;
            global.getDebugInfo(debugInfo);
            llvm::DIGlobalVariable *debugVariable =
                debugInfo.empty() ? nullptr : debugInfo.front()->getVariable();
            const bool hasSourceVariable = debugVariable &&
                !llvm::StringRef(originalName).starts_with(".str");
            const bool persistent = !global.isConstant() || !local || hasSourceVariable;
            if (!persistent) continue;
            if (global.isThreadLocal()) {
                std::cerr << "[LiveC] Thread-local global reload is not supported yet: "
                          << originalName << '\n';
                return false;
            }

            const std::string stableName = local
                ? staticGlobalName(source, originalName)
                : originalName;
            const std::string type = typeFingerprint(global.getValueType());
            VariableMetadata reflection = describeGlobal(global, source, root, module.getDataLayout());
            candidate.variableMetadata[stableName] = reflection;
            std::shared_lock metadataLock(reflectionMutex);
            auto existing = persistentGlobals.find(stableName);
            auto staged = stagedGlobalDefinitions.find(stableName);
            if (existing != persistentGlobals.end()) {
                if (existing->second.abiType != type) {
                    std::cerr << "[LiveC] Rejecting incompatible change to persistent global "
                              << stableName << ".\n";
                    return false;
                }
                global.setName(stableName);
                global.setInitializer(nullptr);
                global.setLinkage(llvm::GlobalValue::ExternalLinkage);
                continue;
            }
            if (staged != stagedGlobalDefinitions.end()) {
                if (staged->second.type != type) {
                    std::cerr << "[LiveC] Conflicting declarations for global " << stableName << ".\n";
                    return false;
                }
                const bool commonDuplicate = staged->second.common && global.hasCommonLinkage();
                const bool weakDuplicate = staged->second.weakODR && isODRWeak(global);
                if (!commonDuplicate && !weakDuplicate) {
                    std::cerr << "[LiveC] Multiple definitions of global " << stableName << ".\n";
                    return false;
                }
                global.setName(stableName);
                global.setInitializer(nullptr);
                global.setLinkage(llvm::GlobalValue::ExternalLinkage);
                continue;
            }

            GlobalStage newGlobal{type, global.hasCommonLinkage(), isODRWeak(global),
                                  std::move(reflection)};
            stagedGlobalDefinitions.emplace(stableName, std::move(newGlobal));
            global.setName(stableName);
            global.setLinkage(llvm::GlobalValue::ExternalLinkage);
        }

        std::vector<llvm::Function *> definitionsInModule;
        for (llvm::Function &function : module) {
            if (!function.isDeclaration() && !function.isIntrinsic()) {
                definitionsInModule.push_back(&function);
            }
        }
        for (llvm::Function *function : definitionsInModule) {
            const std::string originalName = function->getName().str();
            const std::string dispatchKey = functionABIKey(originalName, *function, source);
            const std::string stableName = stableFunctionName(originalName, dispatchKey);
            std::string displayName = originalName;
            std::string displaySignature = debugFunctionSignature(*function);
            std::string displaySource = sourcePath(source);
            bool reflectable = false;
            unsigned line = 0;
            if (llvm::DISubprogram *subprogram = function->getSubprogram()) {
                if (!subprogram->getName().empty()) displayName = qualifiedFunctionName(*subprogram);
                line = subprogram->getLine();
                displaySource = debugFilePath(subprogram->getFile(), source);
                reflectable = !subprogram->isArtificial() &&
                              pathIsWithin(fs::path(displaySource), root);
            }
            if (!reflectable) continue;
            candidate.dispatchNames[dispatchKey] = stableName;
            if (stableName == "main" &&
                (!function->getReturnType()->isIntegerTy(32) ||
                 function->getFunctionType()->getNumParams() != 0 ||
                 function->getFunctionType()->isVarArg())) {
                std::cerr << "[LiveC] Entry point must have the signature int main().\n";
                return false;
            }
            const std::string signature = functionFingerprint(*function);
            bool publish = true;
            {
                std::shared_lock metadataLock(reflectionMutex);
                auto previous = functionSignatures.find(stableName);
                if (previous != functionSignatures.end() && previous->second != signature) {
                    std::cerr << "[LiveC] Rejecting ABI/signature change to function " << stableName << ".\n";
                    return false;
                }
                auto previousBody = functionBodies.find(stableName);
                publish = previousBody == functionBodies.end() ||
                          previousBody->second != printFunction(*function);
            }
            const bool weakODR = isODRWeak(*function);
            if (!definitions.insert(stableName).second) {
                auto previous = std::find_if(candidate.functions.begin(), candidate.functions.end(),
                    [&](const FunctionUpdate &entry) { return entry.stableName == stableName; });
                if (previous == candidate.functions.end() || !weakODR || !previous->weakODR ||
                    previous->signature != signature) {
                    std::cerr << "[LiveC] Multiple non-ODR definitions of function " << stableName << ".\n";
                    return false;
                }
                llvm::FunctionType *functionType = function->getFunctionType();
                const auto callingConvention = function->getCallingConv();
                const llvm::AttributeList attributes = function->getAttributes();
                function->setName("__livec_odr_discard_" + std::to_string(generation) +
                                  "_" + hexHash(source.string() + stableName));
                function->setLinkage(llvm::GlobalValue::ExternalLinkage);
                auto *dispatchDeclaration = llvm::Function::Create(
                    functionType, llvm::GlobalValue::ExternalLinkage, stableName, module);
                dispatchDeclaration->setCallingConv(callingConvention);
                dispatchDeclaration->setAttributes(attributes);
                function->replaceAllUsesWith(dispatchDeclaration);
                function->eraseFromParent();
                continue;
            }

            const std::string implementationName = "__livec_impl_" + std::to_string(generation) +
                "_" + hexHash(stableName);
            const std::string body = printFunction(*function);
            llvm::FunctionType *functionType = function->getFunctionType();
            const auto callingConvention = function->getCallingConv();
            const llvm::AttributeList attributes = function->getAttributes();

            function->setName(implementationName);
            function->setLinkage(llvm::GlobalValue::ExternalLinkage);
            auto *dispatchDeclaration = llvm::Function::Create(
                functionType, llvm::GlobalValue::ExternalLinkage, stableName, module);
            dispatchDeclaration->setCallingConv(callingConvention);
            dispatchDeclaration->setAttributes(attributes);
            function->replaceAllUsesWith(dispatchDeclaration);
            instrumentLoopSafepoints(*function, module, *dispatchDeclaration,
                                     hashString(stableName), generation);
            candidate.functions.push_back({stableName, dispatchKey, implementationName,
                                            signature, body, publish,
                                            weakODR, displayName, displaySignature, displaySource,
                                            line, reflectable});
        }
        return true;
    }

    bool installNewStubs(const std::vector<FunctionUpdate> &functions) {
        orc::SymbolMap symbols;
        for (const auto &function : functions) {
            if (stubNames.count(function.stableName)) continue;
            const auto flags = llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable;
            if (auto error = stubs->createStub(function.stableName, llvm::orc::ExecutorAddr(), flags)) {
                return logError("creating dispatch stub for " + function.stableName, std::move(error));
            }
            auto stub = stubs->findStub(function.stableName, false);
            symbols[jit->mangleAndIntern(function.stableName)] = stub;
            stubNames.insert(function.stableName);
        }
        if (!symbols.empty()) {
            if (auto error = jit->getMainJITDylib().define(orc::absoluteSymbols(std::move(symbols)))) {
                return logError("registering dispatch symbols", std::move(error));
            }
        }
        return true;
    }

public:
    void setScratchDirectory(fs::path path) { scratchDirectory = std::move(path); }

private:
    fs::path root;
    fs::path scratchDirectory;
    std::vector<std::string> clangArgs;
    std::vector<fs::path> dynamicLibraries;
    std::unique_ptr<orc::LLJIT> jit;
    std::unique_ptr<orc::IndirectStubsManager> stubs;
    std::vector<orc::ResourceTrackerSP> trackers;
    std::set<std::string> stubNames;
    mutable std::shared_mutex reflectionMutex;
    std::set<std::string> activeFunctions;
    std::map<std::string, std::string> functionSignatures;
    std::map<std::string, std::string> functionBodies;
    std::map<std::string, std::string> functionDispatchNames;
    std::map<std::string, FunctionMetadata> functionMetadata;
    std::map<std::string, PersistentGlobal> persistentGlobals;
    std::set<std::string> activeTypes;
    std::map<std::string, TypeMetadata> typeMetadata;
    std::map<std::string, std::string> typeLayouts;
    bool initializersRan = false;
    size_t generation = 0;
};

extern "C" size_t livec_reflect_function_count(void) {
    return reflectionRuntime ? reflectionRuntime->reflectedFunctionCount() : 0;
}

extern "C" size_t livec_reflect_variable_count(void) {
    return reflectionRuntime ? reflectionRuntime->reflectedVariableCount() : 0;
}

extern "C" int livec_reflect_function_at(size_t index, livec_function_info *out) {
    return reflectionRuntime ? reflectionRuntime->reflectedFunctionAt(index, out) : 0;
}

extern "C" int livec_reflect_variable_at(size_t index, livec_variable_info *out) {
    return reflectionRuntime ? reflectionRuntime->reflectedVariableAt(index, out) : 0;
}

extern "C" int livec_reflect_function_find(const char *name, const char *sourceFile,
                                             livec_function_info *out) {
    return reflectionRuntime ? reflectionRuntime->findReflectedFunction(name, sourceFile, out) : 0;
}

extern "C" int livec_reflect_function_find_signature(const char *name, const char *signature,
                                                        const char *sourceFile,
                                                        livec_function_info *out) {
    if (!reflectionRuntime) return 0;
    return reflectionRuntime->findReflectedFunctionSignature(name, signature, sourceFile, out);
}

extern "C" int livec_reflect_variable_find(const char *name, const char *sourceFile,
                                              livec_variable_info *out) {
    return reflectionRuntime ? reflectionRuntime->findReflectedVariable(name, sourceFile, out) : 0;
}

extern "C" size_t livec_reflect_type_count(void) {
    return reflectionRuntime ? reflectionRuntime->reflectedTypeCount() : 0;
}

extern "C" int livec_reflect_type_at(size_t index, livec_type_info *out) {
    return reflectionRuntime ? reflectionRuntime->reflectedTypeAt(index, out) : 0;
}

extern "C" int livec_reflect_type_find(const char *name, const char *sourceFile,
                                          livec_type_info *out) {
    return reflectionRuntime ? reflectionRuntime->findReflectedType(name, sourceFile, out) : 0;
}

extern "C" size_t livec_reflect_type_field_count(const char *typeId) {
    return reflectionRuntime ? reflectionRuntime->reflectedTypeFieldCount(typeId) : 0;
}

extern "C" int livec_reflect_type_field_at(const char *typeId, size_t index,
                                              livec_field_info *out) {
    return reflectionRuntime ? reflectionRuntime->reflectedTypeFieldAt(typeId, index, out) : 0;
}

extern "C" size_t livec_reflect_type_method_count(const char *typeId) {
    return reflectionRuntime ? reflectionRuntime->reflectedTypeMethodCount(typeId) : 0;
}

extern "C" int livec_reflect_type_method_at(const char *typeId, size_t index,
                                               livec_function_info *out) {
    return reflectionRuntime ? reflectionRuntime->reflectedTypeMethodAt(typeId, index, out) : 0;
}

int main(int argc, char **argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) return 2;
    quietMode = options.quiet;
    std::error_code ec;
    options.entry = fs::absolute(options.entry, ec).lexically_normal();
    if (ec || !fs::is_regular_file(options.entry)) {
        std::cerr << "[LiveC] Entry source does not exist: " << options.entry << '\n';
        return 2;
    }
    fs::path root = options.watchRoot.empty()
        ? options.entry.parent_path()
        : fs::absolute(options.watchRoot, ec).lexically_normal();
    if (ec || !fs::is_directory(root)) {
        std::cerr << "[LiveC] Project root does not exist: " << root << '\n';
        return 2;
    }
    if (!pathIsWithin(options.entry, root)) {
        std::cerr << "[LiveC] Entry source must be inside the watched project root.\n";
        return 2;
    }

    char temporaryTemplate[] = "/tmp/livec-jit-XXXXXX";
    char *temporaryDirectory = mkdtemp(temporaryTemplate);
    if (!temporaryDirectory) {
        std::cerr << "[LiveC] Could not create a temporary directory: " << std::strerror(errno) << '\n';
        return 2;
    }

    LiveRuntime runtime(root);
    reflectionRuntime = &runtime;
    runtime.setScratchDirectory(temporaryDirectory);
    runtime.setClangArgs(std::move(options.clangArgs));
    runtime.setLibraries(std::move(options.dynamicLibraries));
    if (!runtime.initialize()) return 2;

    std::vector<fs::path> sources = scanProject(root);
    if (std::find(sources.begin(), sources.end(), options.entry) == sources.end()) {
        sources.push_back(options.entry);
        std::sort(sources.begin(), sources.end());
    }
    auto observed = snapshotProject(root);
    if (!runtime.compileAndInstall(sources)) return 2;

    std::atomic<bool> programFinished{false};
    int programResult = 0;
    std::thread program([&] {
        programResult = runtime.runMain();
        programFinished.store(true, std::memory_order_release);
    });
    logInfo("Program running; watching project " + root.string());

    while (!programFinished.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto current = snapshotProject(root);
        if (current == observed) continue;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        current = snapshotProject(root);
        if (current == observed) continue;

        logInfo("Source change detected; compiling project...");
        std::vector<fs::path> currentSources = scanProject(root);
        if (std::find(currentSources.begin(), currentSources.end(), options.entry) == currentSources.end()) {
            currentSources.push_back(options.entry);
            std::sort(currentSources.begin(), currentSources.end());
        }
        if (runtime.compileAndInstall(currentSources)) {
            observed = std::move(current);
        } else {
            std::cerr << "[LiveC] Compile/reload failed; previous code remains active.\n";
            observed = std::move(current);
        }
    }

    program.join();
    runtime.shutdown();
    logInfo("Program exited with status " + std::to_string(programResult));
    fs::remove_all(temporaryDirectory);
    return programResult;
}
