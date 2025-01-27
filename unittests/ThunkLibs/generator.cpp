#include <catch2/catch.hpp>

#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/Version.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Tooling/Tooling.h>

#include <interface.h>

#include <filesystem>
#include <fstream>
#include <string_view>

#include "common.h"

/**
 * This class parses its input code and stores it alongside its AST representation.
 *
 * Use this with HasASTMatching in Catch2's CHECK_THAT/REQUIRE_THAT macros.
 */
struct SourceWithAST {
    std::string code;
    std::unique_ptr<clang::ASTUnit> ast;

    SourceWithAST(std::string_view input);
};

std::ostream& operator<<(std::ostream& os, const SourceWithAST& ast) {
    os << ast.code;

    // Additionally, change this to true to print the full AST on test failures
    const bool print_ast = false;
    if (print_ast) {
        for (auto it = ast.ast->top_level_begin(); it != ast.ast->top_level_end(); ++it) {
            // Skip header declarations
            if (!ast.ast->isInMainFileID((*it)->getBeginLoc())) {
                continue;
            }

            auto llvm_os = llvm::raw_os_ostream { os };
            (*it)->dump(llvm_os);
        }
    }
    return os;
}

struct Fixture {
    Fixture() {
        tmpdir = std::string { P_tmpdir } + "/thunkgentestXXXXXX";
        if (!mkdtemp(tmpdir.data())) {
            std::abort();
        }
        std::filesystem::create_directory(tmpdir);
        output_filenames = {
            tmpdir + "/thunkgen_guest",
            tmpdir + "/thunkgen_host",
        };
    }

    ~Fixture() {
        std::filesystem::remove_all(tmpdir);
    }

    struct GenOutput {
        SourceWithAST guest;
        SourceWithAST host;
    };

    /**
     * Runs the given given code through the thunk generator and verifies the output compiles.
     *
     * Input code with common definitions (types, functions, ...) should be specified in "prelude".
     * It will be prepended to "code" before processing and also to the generator output.
     */
    SourceWithAST run_thunkgen_guest(std::string_view prelude, std::string_view code, bool silent = false);
    SourceWithAST run_thunkgen_host(std::string_view prelude, std::string_view code, GuestABI = GuestABI::X86_64, bool silent = false);
    GenOutput run_thunkgen(std::string_view prelude, std::string_view code, bool silent = false);

    const std::string libname = "libtest";
    std::string tmpdir;
    OutputFilenames output_filenames;
};

using namespace clang::ast_matchers;

class MatchCallback : public MatchFinder::MatchCallback {
    bool success = false;

    using CheckFn = std::function<bool(const MatchFinder::MatchResult&)>;
    std::vector<CheckFn> binding_checks;

public:
    template<typename NodeType>
    void check_binding(std::string_view binding_name, bool (*check_fn)(const NodeType*)) {
        // Decorate the given check with node extraction and wrap it in a type-erased interface
        binding_checks.push_back(
            [check_fn, binding_name = std::string(binding_name)](const MatchFinder::MatchResult& result) {
                if (auto node = result.Nodes.getNodeAs<NodeType>(binding_name.c_str())) {
                    return check_fn(node);
                }
                return false;
            });
    }

    void run(const MatchFinder::MatchResult& result) override {
        success = true; // NOTE: If there are no callbacks, this signals that the match was found at all

        for (auto& binding_check : binding_checks) {
            success = success && binding_check(result);
        }
    }

    bool matched() const noexcept {
        return success;
    }
};

/**
 * This class connects the libclang AST to Catch2 test matchers, allowing for
 * code compiled via SourceWithAST objects to be pattern-matched using the
 * libclang ASTMatcher API.
 */
template<typename ClangMatcher>
class HasASTMatching : public Catch::MatcherBase<SourceWithAST> {
    ClangMatcher matcher;
    MatchCallback callback;

public:
    HasASTMatching(const ClangMatcher& matcher_) : matcher(matcher_) {

    }

    template<typename NodeT>
    HasASTMatching& check_binding(std::string_view binding_name, bool (*check_fn)(const NodeT*)) {
        callback.check_binding(binding_name, check_fn);
        return *this;
    }

    bool match(const SourceWithAST& code) const override {
        MatchCallback result = callback;
        clang::ast_matchers::MatchFinder finder;
        finder.addMatcher(matcher, &result);
        finder.matchAST(code.ast->getASTContext());
        return result.matched();
    }

    std::string describe() const override {
        std::ostringstream ss;
        ss << "should compile and match the given AST pattern";
        return ss.str();
    }
};

HasASTMatching<DeclarationMatcher> matches(const DeclarationMatcher& matcher_) {
    return HasASTMatching<DeclarationMatcher>(matcher_);
}

HasASTMatching<StatementMatcher> matches(const StatementMatcher& matcher_) {
    return HasASTMatching<StatementMatcher>(matcher_);
}

/**
 * Catch matcher that checks if a tested C++ source defines a function with the given name
 */
class DefinesPublicFunction : public HasASTMatching<DeclarationMatcher> {
    std::string function_name;

public:
    DefinesPublicFunction(std::string_view name) : HasASTMatching(functionDecl(hasName(name))), function_name(name) {
    }

    std::string describe() const override {
        std::ostringstream ss;
        ss << "should define and export a function called \"" + function_name + "\"";
        return ss.str();
    }
};

SourceWithAST::SourceWithAST(std::string_view input) : code(input) {
    // Call run_tool with a ToolAction that assigns this->ast

    struct ToolAction : clang::tooling::ToolAction {
        std::unique_ptr<clang::ASTUnit>& ast;

        ToolAction(std::unique_ptr<clang::ASTUnit>& ast_) : ast(ast_) { }

        bool runInvocation(std::shared_ptr<clang::CompilerInvocation> invocation,
                           clang::FileManager* files,
                           std::shared_ptr<clang::PCHContainerOperations> pch,
                           clang::DiagnosticConsumer *diag_consumer) override {
            auto diagnostics = clang::CompilerInstance::createDiagnostics(&invocation->getDiagnosticOpts(), diag_consumer, false);
            ast = clang::ASTUnit::LoadFromCompilerInvocation(invocation, std::move(pch), std::move(diagnostics), files);
            return (ast != nullptr);
        }
    } tool_action { ast };

    run_tool(tool_action, code);
}

/**
 * Generates guest thunk library code from the given input
 */
SourceWithAST Fixture::run_thunkgen_guest(std::string_view prelude, std::string_view code, bool silent) {
    const std::string full_code = std::string { prelude } + std::string { code };

    // These tests don't deal with data layout differences, so just run data
    // layout analysis with host configuration
    auto data_layout_analysis_factory = std::make_unique<AnalyzeDataLayoutActionFactory>();
    run_tool(*data_layout_analysis_factory, full_code, silent);
    auto& data_layout = data_layout_analysis_factory->GetDataLayout();

    run_tool(std::make_unique<GenerateThunkLibsActionFactory>(libname, output_filenames, data_layout), full_code, silent);

    std::string result =
        "#include <cstdint>\n"
        "#define MAKE_THUNK(lib, name, hash) extern \"C\" int fexthunks_##lib##_##name(void*);\n"
        "template<typename>\n"
        "struct callback_thunk_defined;\n"
        "#define MAKE_CALLBACK_THUNK(name, sig, hash) template<> struct callback_thunk_defined<sig> {};\n"
        "#define FEX_PACKFN_LINKAGE\n"
        "template<typename Target>\n"
        "Target *MakeHostTrampolineForGuestFunction(uint8_t HostPacker[32], void (*)(uintptr_t, void*), Target*);\n"
        "template<typename Target>\n"
        "Target *AllocateHostTrampolineForGuestFunction(Target*);\n";
    const auto& filename = output_filenames.guest;
    {
        std::ifstream file(filename);
        const auto current_size = result.size();
        const auto new_data_size = std::filesystem::file_size(filename);
        result.resize(result.size() + new_data_size);
        file.read(result.data() + current_size, result.size());
    }
    return SourceWithAST { std::string { prelude } + result };
}

/**
 * Generates host thunk library code from the given input
 */
SourceWithAST Fixture::run_thunkgen_host(std::string_view prelude, std::string_view code, GuestABI guest_abi, bool silent) {
    const std::string full_code = std::string { prelude } + std::string { code };

    // These tests don't deal with data layout differences, so just run data
    // layout analysis with host configuration
    auto data_layout_analysis_factory = std::make_unique<AnalyzeDataLayoutActionFactory>();
    run_tool(*data_layout_analysis_factory, full_code, silent, guest_abi);
    auto& data_layout = data_layout_analysis_factory->GetDataLayout();

    run_tool(std::make_unique<GenerateThunkLibsActionFactory>(libname, output_filenames, data_layout), full_code, silent);

    std::string result =
        "#include <array>\n"
        "#include <cstdint>\n"
        "#include <cstring>\n"
        "#include <dlfcn.h>\n"
        "#include <type_traits>\n"
        "template<typename Fn>\n"
        "struct function_traits;\n"
        "template<typename Result, typename Arg>\n"
        "struct function_traits<Result(*)(Arg)> {\n"
        "    using result_t = Result;\n"
        "    using arg_t = Arg;\n"
        "};\n"
        "template<auto Fn>\n"
        "static typename function_traits<decltype(Fn)>::result_t\n"
        "fexfn_type_erased_unpack(void* argsv) {\n"
        "    using args_t = typename function_traits<decltype(Fn)>::arg_t;\n"
        "    return Fn(reinterpret_cast<args_t>(argsv));\n"
        "}\n"
        "#define LOAD_INTERNAL_GUESTPTR_VIA_CUSTOM_ABI(arg)\n"
        "struct GuestcallInfo {\n"
        "  uintptr_t HostPacker;\n"
        "  void (*CallCallback)(uintptr_t, uintptr_t, void*);\n"
        "  uintptr_t GuestUnpacker;\n"
        "  uintptr_t GuestTarget;\n"
        "};\n"
        "struct ParameterAnnotations {};\n"
        "template<typename>\n"
        "struct GuestWrapperForHostFunction {\n"
        "  template<ParameterAnnotations...> static void Call(void*);\n"
        "};\n"
        "struct ExportEntry { uint8_t* sha256; void(*fn)(void *); };\n"
        "void *dlsym_default(void* handle, const char* symbol);\n"
        "template<typename T>\n"
        "struct guest_layout {\n"
        "  T data;\n"
        "};\n"
        "\n"
        "template<typename T>\n"
        "struct guest_layout<T*> {\n"
        "#ifdef IS_32BIT_THUNK\n"
        "  using type = uint32_t;\n"
        "#else\n"
        "  using type = uint64_t;\n"
        "#endif\n"
        "  type data;\n"
        "};\n"
        "\n"
        "template<typename T>\n"
        "struct host_layout {\n"
        "  T data;\n"
        "\n"
        "  host_layout(const guest_layout<T>& from);\n"
        "};\n"
        "\n"
        "template<typename T> guest_layout<T> to_guest(const host_layout<T>& from) requires(!std::is_pointer_v<T>);\n"
        "template<typename T> guest_layout<T*> to_guest(const host_layout<T*>& from);\n"
        "template<typename F> void FinalizeHostTrampolineForGuestFunction(F*);\n"
        "template<typename F> void FinalizeHostTrampolineForGuestFunction(guest_layout<F*>);\n"
        "template<typename T> const host_layout<T>& to_host_layout(const T& t);\n";

    auto& filename = output_filenames.host;
    {
        std::ifstream file(filename);
        const auto prelude_size = result.size();
        const auto new_data_size = std::filesystem::file_size(filename);
        result.resize(result.size() + new_data_size);
        file.read(result.data() + prelude_size, result.size());

        // Force all functions to be non-static, since having to define them
        // would add a lot of noise to simple tests.
        while (true) {
            auto pos = result.find("static ", prelude_size);
            if (pos == std::string::npos) {
                break;
            }
            result.replace(pos, 6, "      "); // Replace "static" with 6 spaces (avoiding reallocation)
        }
    }
    return SourceWithAST { std::string { prelude } + result };
}

Fixture::GenOutput Fixture::run_thunkgen(std::string_view prelude, std::string_view code, bool silent) {
    return { run_thunkgen_guest(prelude, code, silent),
             run_thunkgen_host(prelude, code, GuestABI::X86_64, silent) };
}

#if CLANG_VERSION_MAJOR <= 15
// Old clang versions require an explicit "struct" prefix
#define CLANG_STRUCT_PREFIX "struct "
#define asStructString(name) asString(CLANG_STRUCT_PREFIX name)
#else
#define CLANG_STRUCT_PREFIX
#define asStructString(name) asString(name)
#endif

TEST_CASE_METHOD(Fixture, "Trivial") {
    const auto output = run_thunkgen("",
        "#include <thunks_common.h>\n"
        "void func();\n"
        "template<auto> struct fex_gen_config {};\n"
        "template<> struct fex_gen_config<func> {};\n");

    // Guest code
    CHECK_THAT(output.guest, DefinesPublicFunction("func"));

    CHECK_THAT(output.guest,
        matches(functionDecl(
            hasName("fexfn_pack_func"),
            returns(asString("void")),
            parameterCountIs(0)
        )));

    // Host code
    CHECK_THAT(output.host,
        matches(varDecl(
            hasName("exports"),
            hasType(constantArrayType(hasElementType(asStructString("ExportEntry")), hasSize(2))),
            hasInitializer(initListExpr(hasInit(0, expr()),
                                        hasInit(1, initListExpr(hasInit(0, implicitCastExpr()), hasInit(1, implicitCastExpr())))))
            // TODO: check null termination
            )));
}

// Unknown annotations trigger an error
TEST_CASE_METHOD(Fixture, "UnknownAnnotation") {
    REQUIRE_THROWS(run_thunkgen("void func();\n",
        "struct invalid_annotation {};\n"
        "template<auto> struct fex_gen_config {};\n"
        "template<> struct fex_gen_config<func> : invalid_annotation {};\n", true));

    REQUIRE_THROWS(run_thunkgen("void func();\n",
        "template<auto> struct fex_gen_config {};\n"
        "template<> struct fex_gen_config<func> { int invalid_field_annotation; };\n", true));
}

TEST_CASE_METHOD(Fixture, "VersionedLibrary") {
    const auto output = run_thunkgen_host("",
        "template<auto> struct fex_gen_config { int version = 123; };\n");

    CHECK_THAT(output,
        matches(callExpr(
            callee(functionDecl(hasName("dlopen"))),
            hasArgument(0, stringLiteral().bind("libname"))
            ))
        .check_binding("libname", +[](const clang::StringLiteral* lit) {
            return lit->getString().endswith(".so.123");
        }));
}

TEST_CASE_METHOD(Fixture, "FunctionPointerViaType") {
    const auto output = run_thunkgen("",
        "template<typename> struct fex_gen_type {};\n"
        "template<> struct fex_gen_type<int(char, char)> {};\n");

    // Guest should apply MAKE_CALLBACK_THUNK to this signature
    CHECK_THAT(output.guest,
        matches(classTemplateSpecializationDecl(
            // Should have signature matching input function
            hasName("callback_thunk_defined"),
            hasTemplateArgument(0, refersToType(asString("int (char, char)")))
        )));

    // Host should export the unpacking function for callback arguments
    CHECK_THAT(output.host,
        matches(varDecl(
            hasName("exports"),
            hasType(constantArrayType(hasElementType(asStructString("ExportEntry")), hasSize(2))),
            hasInitializer(hasDescendant(declRefExpr(to(cxxMethodDecl(hasName("Call"), ofClass(hasName("GuestWrapperForHostFunction"))).bind("funcptr")))))
            )).check_binding("funcptr", +[](const clang::CXXMethodDecl* decl) {
                auto parent = llvm::cast<clang::ClassTemplateSpecializationDecl>(decl->getParent());
                return parent->getTemplateArgs().get(0).getAsType().getAsString() == "int (char, char)";
            }));
}

// Parameter is a function pointer
TEST_CASE_METHOD(Fixture, "FunctionPointerParameter") {
    const auto output = run_thunkgen("",
        "void func(int (*funcptr)(char, char));\n"
        "template<auto> struct fex_gen_config {};\n"
        "template<> struct fex_gen_config<func> {};\n");

    CHECK_THAT(output.guest,
        matches(functionDecl(
            // Should have signature matching input function
            hasName("fexfn_pack_func"),
            returns(asString("void")),
            parameterCountIs(1),
            hasParameter(0, hasType(asString("int (*)(char, char)")))
        )));

    // Host packing function should call FinalizeHostTrampolineForGuestFunction on the argument
    CHECK_THAT(output.host,
        matches(functionDecl(
            hasName("fexfn_unpack_libtest_func"),
            hasDescendant(callExpr(callee(functionDecl(hasName("FinalizeHostTrampolineForGuestFunction"))), hasArgument(0, expr().bind("funcptr"))))
        )).check_binding("funcptr", +[](const clang::Expr* funcptr) {
            // Check that the argument type matches the function pointer
            return funcptr->getType().getAsString() == "guest_layout<int (*)(char, char)>";
        }));

    // Host should export the unpacking function for function pointer arguments
    CHECK_THAT(output.host,
        matches(varDecl(
            hasName("exports"),
            hasType(constantArrayType(hasElementType(asStructString("ExportEntry")), hasSize(3))),
            hasInitializer(hasDescendant(declRefExpr(to(cxxMethodDecl(hasName("Call"), ofClass(hasName("GuestWrapperForHostFunction")))))))
            )));
}

TEST_CASE_METHOD(Fixture, "MultipleParameters") {
    const std::string prelude =
        "struct TestStruct { int member; };\n";

    auto output = run_thunkgen(prelude,
        "void func(int arg, char, unsigned long, TestStruct);\n"
        "template<auto> struct fex_gen_config {};\n"
        "template<> struct fex_gen_config<func> {};\n");

    // Guest code
    CHECK_THAT(output.guest, DefinesPublicFunction("func"));

    CHECK_THAT(output.guest,
        matches(functionDecl(
            hasName("fexfn_pack_func"),
            returns(asString("void")),
            parameterCountIs(4),
            hasParameter(0, hasType(asString("int"))),
            hasParameter(1, hasType(asString("char"))),
            hasParameter(2, hasType(asString("unsigned long"))),
            hasParameter(3, hasType(asStructString("TestStruct")))
        )));

    // Host code
    CHECK_THAT(output.host,
        matches(varDecl(
            hasName("exports"),
            hasType(constantArrayType(hasElementType(asStructString("ExportEntry")), hasSize(2))),
            hasInitializer(initListExpr(hasInit(0, expr()),
                                        hasInit(1, initListExpr(hasInit(0, implicitCastExpr()), hasInit(1, implicitCastExpr())))))
            // TODO: check null termination
            )));

    CHECK_THAT(output.host,
        matches(functionDecl(
            hasName("fexfn_unpack_libtest_func"),
            // Packed argument struct should contain all parameters
            parameterCountIs(1),
            hasParameter(0, hasType(pointerType(pointee(hasUnqualifiedDesugaredType(
                recordType(hasDeclaration(decl(
                    has(fieldDecl(hasType(asString("guest_layout<int>")))),
                    has(fieldDecl(hasType(asString("guest_layout<char>")))),
                    has(fieldDecl(hasType(asString("guest_layout<unsigned long>")))),
                    has(fieldDecl(hasType(asString("guest_layout<" CLANG_STRUCT_PREFIX "TestStruct>"))))
                    ))))))))
            )));
}

// Returning a function pointer should trigger an error unless an annotation is provided
TEST_CASE_METHOD(Fixture, "ReturnFunctionPointer") {
    const std::string prelude = "using funcptr = void (*)(char, char);\n";

    REQUIRE_THROWS(run_thunkgen_guest(prelude,
        "funcptr func(int);\n"
        "template<auto> struct fex_gen_config {};\n"
        "template<> struct fex_gen_config<func> {};\n", true));

    REQUIRE_NOTHROW(run_thunkgen_guest(prelude,
        "#include <thunks_common.h>\n"
        "funcptr func(int);\n"
        "template<auto> struct fex_gen_config {};\n"
        "template<> struct fex_gen_config<func> : fexgen::returns_guest_pointer {};\n"));
}

TEST_CASE_METHOD(Fixture, "VariadicFunction") {
    const std::string prelude = "void func(int arg, ...);\n";

    const auto output = run_thunkgen_guest(prelude,
        "template<auto> struct fex_gen_config {};\n"
        "template<> struct fex_gen_config<func> {\n"
        "  using uniform_va_type = char;\n"
        "};\n");

    CHECK_THAT(output,
        matches(functionDecl(
            hasName("fexfn_pack_func_internal"),
            returns(asString("void")),
            parameterCountIs(3),
            hasParameter(0, hasType(asString("int"))),
            hasParameter(1, hasType(asString("unsigned long"))),
            hasParameter(2, hasType(pointerType(pointee(asString("char")))))
        )));
}

// Variadic functions without annotation trigger an error
TEST_CASE_METHOD(Fixture, "VariadicFunctionsWithoutAnnotation") {
    REQUIRE_THROWS(run_thunkgen_guest("void func(int arg, ...);\n",
        "template<auto> struct fex_gen_config {};\n"
        "template<> struct fex_gen_config<func> {};\n", true));
}

// Tests generation of guest_layout/host_layout wrappers and related helpers
TEST_CASE_METHOD(Fixture, "LayoutWrappers") {
    auto guest_abi = GENERATE(GuestABI::X86_32, GuestABI::X86_64);
    INFO(guest_abi);

    const auto host_layout_is_trivial =
        matches(classTemplateSpecializationDecl(
            hasName("host_layout"),
            hasAnyTemplateArgument(refersToType(asString("struct A"))),
            has(fieldDecl(hasName("data"), hasType(hasCanonicalType(asString("struct A")))))
        ));
    const auto layout_undefined = [](const char* type) {
        return matches(classTemplateSpecializationDecl(
              hasName(type),
              hasAnyTemplateArgument(refersToType(asString("struct A")))
        ).bind("layout")).check_binding("layout", +[](const clang::ClassTemplateSpecializationDecl* decl) {
            return !decl->isCompleteDefinition();
        });
    };
    const auto guest_converter_defined =
          matches(functionDecl(hasName("to_guest"),
                // Parameter is a host_layout<A> (ignoring qualifiers and references)
                hasParameter(0, hasType(references(classTemplateSpecializationDecl(hasName("host_layout"), hasAnyTemplateArgument(refersToType(asString("struct A"))))))),
                // Return value is a guest_layout<A>
                returns(asString("guest_layout<" CLANG_STRUCT_PREFIX "A>"))));
    const auto guest_converter_undefined =
          matches(functionDecl(hasName("to_guest"),
                // Parameter is a host_layout<A> (ignoring qualifiers and references)
                hasParameter(0, hasType(references(classTemplateSpecializationDecl(hasName("host_layout"), hasAnyTemplateArgument(refersToType(asString("struct A"))))))),
                isDeleted()));

    const std::string code =
        "template<typename> struct fex_gen_type {};\n"
        "template<> struct fex_gen_type<A> {};\n";

    // For fully compatible types, both guest_layout and host_layout directly
    // reference the original struct
    SECTION("Fully compatible type") {
        const char* struct_def = "struct A { int a; int b; };\n";
        const auto output = run_thunkgen_host(struct_def, code, guest_abi);
        CHECK_THAT(output,
            matches(classTemplateSpecializationDecl(
                  hasName("guest_layout"),
                  hasAnyTemplateArgument(refersToType(asString("struct A"))),
                  has(fieldDecl(hasName("data"), hasType(hasCanonicalType(asString("struct A")))))
            )));
        CHECK_THAT(output, guest_converter_defined);

        CHECK_THAT(output, host_layout_is_trivial);
    }

    // For repackable types, guest_layout explicitly lists its members
    SECTION("Repackable type") {
        const char* struct_def =
            "#ifdef HOST\n"
            "struct A { int a; int b; };\n"
            "#else\n"
            "struct A { int b; int a; };\n"
            "#endif\n";
        const auto output = run_thunkgen_host(struct_def, code, guest_abi);
        CHECK_THAT(output,
            matches(classTemplateSpecializationDecl(
                  hasName("guest_layout"),
                  hasAnyTemplateArgument(refersToType(asString("struct A"))),
                  // The member "data" exists and is defined to a struct...
                  has(fieldDecl(hasName("data"), hasType(hasCanonicalType(hasDeclaration(decl(
                      // ... the members of which also use guest_layout
                      has(fieldDecl(hasName("a"), hasType(asString("guest_layout<int>")))),
                      has(fieldDecl(hasName("b"), hasType(asString("guest_layout<int>"))))
                      ))))))
            )));
        CHECK_THAT(output, guest_converter_defined);

        CHECK_THAT(output, host_layout_is_trivial);
    }

    // For incompatible types, use of guest_layout nor host_layout should be prohibited
    SECTION("Incompatible type, unannotated") {
        const char* struct_def =
            "#ifdef HOST\n"
            "struct A { int a; int b; };\n"
            "#else\n"
            "struct A { int c; int d; };\n"
            "#endif\n";
        const auto output = run_thunkgen_host(struct_def, code, guest_abi);
        CHECK_THAT(output, layout_undefined("guest_layout"));
        CHECK_THAT(output, guest_converter_undefined);
        CHECK_THAT(output, layout_undefined("host_layout"));
    }

    // Layout wrappers can be enabled even for incompatible types using the emit_layout_wrappers annotation
    SECTION("Incompatible type, annotated") {
        // A slightly different setup is used here in order to construct a type which...
        // - has incompatible data layout (for both 32-bit and 64-bit guests)
        // - has consistently named members in struct A (which is required to emit layout wrappers)
        const char* struct_def =
            "#ifdef HOST\n"
            "struct B { int a; };\n"
            "#else\n"
            "struct B { int b; };\n"
            "#endif\n"
            "struct A { B* a; int b; };\n";
        const std::string code =
            "#include <thunks_common.h>\n"
            "template<typename> struct fex_gen_type {};\n"
            "template<> struct fex_gen_type<A> : fexgen::emit_layout_wrappers {};\n";
        const auto output = run_thunkgen_host(struct_def, code, guest_abi);
        CHECK_THAT(output,
            matches(classTemplateSpecializationDecl(
                  hasName("guest_layout"),
                  hasAnyTemplateArgument(refersToType(recordType(hasDeclaration(recordDecl(hasName("A")))))),
                  // The member "data" exists and is defined to a struct...
                  has(fieldDecl(hasName("data"), hasType(hasCanonicalType(hasDeclaration(decl(
                      // ... the members of which also use guest_layout
                      has(fieldDecl(hasName("a"), hasType(asString("guest_layout<" CLANG_STRUCT_PREFIX "B *>")))),
                      has(fieldDecl(hasName("b"), hasType(asString("guest_layout<int>"))))
                      ))))))
            )));
        CHECK_THAT(output, guest_converter_defined);

        CHECK_THAT(output, host_layout_is_trivial);
    }
}

TEST_CASE_METHOD(Fixture, "StructRepacking") {
    auto guest_abi = GENERATE(GuestABI::X86_32, GuestABI::X86_64);
    INFO(guest_abi);

    // All tests use the same function, but the prelude defining its parameter type "A" varies
    const std::string code =
        "#include <thunks_common.h>\n"
        "void func(A*);\n"
        "template<auto> struct fex_gen_config {};\n"
        "template<> struct fex_gen_config<func> : fexgen::custom_host_impl {};\n";

    SECTION("Pointer to struct with consistent data layout") {
        CHECK_NOTHROW(run_thunkgen_host("struct A { int a; };\n", code, guest_abi));
    }

    SECTION("Pointer to struct with unannotated pointer member with inconsistent data layout") {
        const auto prelude =
            "#ifdef HOST\n"
            "struct B { int a; };\n"
            "#else\n"
            "struct B { int b; };\n"
            "#endif\n"
            "struct A { B* a; };\n";

        SECTION("Parameter unannotated") {
            CHECK_THROWS(run_thunkgen_host(prelude, code, guest_abi, true));
        }

        SECTION("Parameter annotated as ptr_passthrough") {
            CHECK_NOTHROW(run_thunkgen_host(prelude, code + "template<> struct fex_gen_param<func, 0, A*> : fexgen::ptr_passthrough {};\n", guest_abi));
        }
    }

    SECTION("Pointer to struct with pointer member of opaque type") {
        const auto prelude =
            "struct B;\n"
            "struct A { B* a; };\n";

        // Unannotated
        REQUIRE_THROWS_WITH(run_thunkgen_host(prelude, code, guest_abi), Catch::Contains("incomplete type"));
    }
}

TEST_CASE_METHOD(Fixture, "VoidPointerParameter") {
    auto guest_abi = GENERATE(GuestABI::X86_32, GuestABI::X86_64);
    INFO(guest_abi);

    SECTION("Unannotated") {
        const char* code =
            "#include <thunks_common.h>\n"
            "void func(void*);\n"
            "template<> struct fex_gen_config<func> {};\n";
        if (guest_abi == GuestABI::X86_32) {
            // TODO: Currently not considered an error
//            CHECK_THROWS_WITH(run_thunkgen_host("", code, guest_abi, true), Catch::Contains("unsupported parameter type", Catch::CaseSensitive::No));
        } else {
            // Pointee data is assumed to be compatible on 64-bit
            CHECK_NOTHROW(run_thunkgen_host("", code, guest_abi));
        }
    }

    SECTION("Passthrough") {
        const char* code =
            "#include <thunks_common.h>\n"
            "void func(void*);\n"
            "template<> struct fex_gen_config<func> : fexgen::custom_host_impl {};\n"
            "template<> struct fex_gen_param<func, 0, void*> : fexgen::ptr_passthrough {};\n";
        CHECK_NOTHROW(run_thunkgen_host("", code, guest_abi));
    }

    SECTION("Assumed compatible") {
        const char* code =
            "#include <thunks_common.h>\n"
            "void func(void*);\n"
            "template<> struct fex_gen_config<func> {};\n"
            "template<> struct fex_gen_param<func, 0, void*> : fexgen::assume_compatible_data_layout {};\n";
        CHECK_NOTHROW(run_thunkgen_host("", code, guest_abi));
    }

    SECTION("Unannotated in struct") {
        const char* prelude =
            "struct A { void* a; };\n";
        const char* code =
            "#include <thunks_common.h>\n"
            "void func(A*);\n"
            "template<> struct fex_gen_config<func> {};\n";
        if (guest_abi == GuestABI::X86_32) {
            CHECK_THROWS_WITH(run_thunkgen_host(prelude, code, guest_abi, true), Catch::Contains("unsupported parameter type", Catch::CaseSensitive::No));
        } else {
            CHECK_NOTHROW(run_thunkgen_host(prelude, code, guest_abi));
        }
    }
}
