//------------------------------------------------------------------------------
//! @file Compilation.h
//! @brief Central manager for compilation processes
//
// File is under the MIT license; see LICENSE for details
//------------------------------------------------------------------------------
#pragma once

#include <memory>

#include "slang/diagnostics/Diagnostics.h"
#include "slang/numeric/Time.h"
#include "slang/symbols/Scope.h"
#include "slang/symbols/Symbol.h"
#include "slang/syntax/SyntaxNode.h"
#include "slang/util/Bag.h"
#include "slang/util/BumpAllocator.h"
#include "slang/util/SafeIndexedVector.h"

namespace slang {

class AttributeSymbol;
class BindContext;
class CompilationUnitSymbol;
class Definition;
class Expression;
class GenericClassDefSymbol;
class PackageSymbol;
class PrimitiveSymbol;
class PortConnection;
class RootSymbol;
class Statement;
class SubroutineSymbol;
class SyntaxTree;
class SystemSubroutine;

struct BindDirectiveSyntax;
struct CompilationUnitSyntax;
struct DataTypeSyntax;
struct DefaultClockingReferenceSyntax;
struct DPIExportSyntax;
struct FunctionDeclarationSyntax;
struct ImplicitTypeSyntax;
struct ModuleDeclarationSyntax;
struct ScopedNameSyntax;
struct UdpDeclarationSyntax;
struct VariableDimensionSyntax;

enum class IntegralFlags : uint8_t;
enum class UnconnectedDrive;

/// Specifies which set of min:typ:max expressions should
/// be used during compilation.
enum class MinTypMax {
    /// Use the "min" delay expressions.
    Min,

    /// Use the "typical" delay expressions.
    Typ,

    /// Use the "max" delay expressions.
    Max
};

/// Contains various options that can control compilation behavior.
struct CompilationOptions {
    /// The maximum depth of nested module instances (and interfaces/programs),
    /// to detect infinite recursion.
    uint32_t maxInstanceDepth = 128;

    /// The maximum number of steps that will be taken when expanding a single
    /// generate construct, to detect infinite loops.
    uint32_t maxGenerateSteps = 131072;

    /// The maximum depth of nested function calls in constant expressions,
    /// to detect infinite recursion.
    uint32_t maxConstexprDepth = 128;

    /// The maximum number of steps to allow when evaluating a constant expressions,
    /// to detect infinite loops.
    uint32_t maxConstexprSteps = 100000;

    /// The maximum number of frames in a callstack to display in diagnostics
    /// before abbreviating them.
    uint32_t maxConstexprBacktrace = 10;

    /// The maximum number of iterations to try to resolve defparams before
    /// giving up due to potentially cyclic dependencies in parameter values.
    uint32_t maxDefParamSteps = 128;

    /// The maximum number of errors that can be found before we short circuit
    /// the tree walking process.
    uint32_t errorLimit = 64;

    /// The maximum number of times we'll attempt to do typo correction before
    /// giving up. This is to prevent very slow compilation times if the
    /// source text is hopelessly broken.
    uint32_t typoCorrectionLimit = 32;

    /// Specifies which set of min:typ:max expressions should
    /// be used during compilation.
    MinTypMax minTypMax = MinTypMax::Typ;

    /// If true, allow hierarchical names in constant expressions.
    bool allowHierarchicalConst = false;

    /// If true, allow all integral types to convert implicitly to enum types.
    bool relaxEnumConversions = false;

    /// Signals driven by an always_comb are normally not allowed to be driven
    /// by any other process. Setting this option allows initial blocks to
    /// also drive such signals.
    bool allowDupInitialDrivers = false;

    /// If true, perform strict checking of variable drivers, which currently
    /// means not taking into account procedural for loop unrolling.
    bool strictDriverChecking = false;

    /// If true, compile in "linting" mode where we suppress errors that could
    /// be caused by not having an elaborated design.
    bool lintMode = false;

    /// If true, suppress warnings about unused code elements. This is intended
    /// for tests; for end users, they can use warning flags to control output.
    bool suppressUnused = true;

    /// If non-empty, specifies the list of modules that should serve as the
    /// top modules in the design. If empty, this will be automatically determined
    /// based on which modules are unreferenced elsewhere.
    flat_hash_set<string_view> topModules;

    /// A list of parameters to override, of the form &lt;name>=&lt;value> -- note that
    /// for now at least this only applies to parameters in top-level modules.
    std::vector<std::string> paramOverrides;
};

/// A node in a tree representing specific parameters to override. These are
/// assembled from defparam values and command-line specified overrides.
struct ParamOverrideNode {
    /// A map of parameters in the current scope to override.
    flat_hash_map<std::string, ConstantValue> overrides;

    /// A map of child scopes that also contain overrides.
    flat_hash_map<std::string, ParamOverrideNode> childNodes;
};

/// A centralized location for creating and caching symbols. This includes
/// creating symbols from syntax nodes as well as fabricating them synthetically.
/// Common symbols such as built in types are exposed here as well.
class Compilation : public BumpAllocator {
public:
    explicit Compilation(const Bag& options = {});
    Compilation(const Compilation& other) = delete;
    Compilation(Compilation&& other) = delete;
    ~Compilation();

    /// Gets the set of options used to construct the compilation.
    const CompilationOptions& getOptions() const { return options; }

    /// Adds a syntax tree to the compilation. If the compilation has already been finalized
    /// by calling @a getRoot this call will throw an exception.
    void addSyntaxTree(std::shared_ptr<SyntaxTree> tree);

    /// Gets the set of syntax trees that have been added to the compilation.
    span<const std::shared_ptr<SyntaxTree>> getSyntaxTrees() const;

    /// Gets the compilation unit for the given syntax node. The compilation unit must have
    /// already been added to the compilation previously via a call to @a addSyntaxTree
    const CompilationUnitSymbol* getCompilationUnit(const CompilationUnitSyntax& syntax) const;

    /// Gets the set of compilation units that have been added to the compilation.
    span<const CompilationUnitSymbol* const> getCompilationUnits() const;

    /// Gets the root of the design. The first time you call this method all top-level
    /// instances will be elaborated and the compilation finalized. After that you can
    /// no longer make any modifications to the compilation object; any attempts to do
    /// so will result in an exception.
    const RootSymbol& getRoot();

    /// Indicates whether the design has been compiled and can no longer accept modifications.
    bool isFinalized() const { return finalized; }

    /// Gets the definition with the given name, or null if there is no such definition.
    /// This takes into account the given scope so that nested definitions are found
    /// before more global ones.
    const Definition* getDefinition(string_view name, const Scope& scope) const;

    /// Gets the definition for the given syntax node, or nullptr if it does not exist.
    const Definition* getDefinition(const ModuleDeclarationSyntax& syntax) const;

    /// Creates a new definition in the given scope based on the given syntax.
    const Definition& createDefinition(const Scope& scope, LookupLocation location,
                                       const ModuleDeclarationSyntax& syntax);

    /// Gets the package with the give name, or null if there is no such package.
    const PackageSymbol* getPackage(string_view name) const;

    /// Gets the built-in 'std' package.
    const PackageSymbol& getStdPackage() const { return *stdPkg; }

    /// Creates a new package in the given scope based on the given syntax.
    const PackageSymbol& createPackage(const Scope& scope, const ModuleDeclarationSyntax& syntax);

    /// Gets the primitive with the given name, or null if there is no such primitive.
    const PrimitiveSymbol* getPrimitive(string_view name) const;

    /// Creates a new primitive in the given scope based on the given syntax.
    const PrimitiveSymbol& createPrimitive(const Scope& scope, const UdpDeclarationSyntax& syntax);

    /// Registers a built-in gate symbol.
    void addGateType(const PrimitiveSymbol& primitive);

    /// Gets the built-in gate type with the given name, or null if there is no such gate.
    const PrimitiveSymbol* getGateType(string_view name) const;

    /// Registers a system subroutine handler, which can be accessed by compiled code.
    void addSystemSubroutine(std::unique_ptr<SystemSubroutine> subroutine);

    /// Registers a type-based system method handler, which can be accessed by compiled code.
    void addSystemMethod(SymbolKind typeKind, std::unique_ptr<SystemSubroutine> method);

    /// Gets a system subroutine with the given name, or null if there is no such subroutine
    /// registered.
    const SystemSubroutine* getSystemSubroutine(string_view name) const;

    /// Gets a system method for the specified type with the given name, or null if there is no such
    /// method registered.
    const SystemSubroutine* getSystemMethod(SymbolKind typeKind, string_view name) const;

    /// Sets the attributes associated with the given symbol.
    void setAttributes(const Symbol& symbol, span<const AttributeSymbol* const> attributes);

    /// Sets the attributes associated with the given statement.
    void setAttributes(const Statement& stmt, span<const AttributeSymbol* const> attributes);

    /// Sets the attributes associated with the given expression.
    void setAttributes(const Expression& expr, span<const AttributeSymbol* const> attributes);

    /// Sets the attributes associated with the given port connection.
    void setAttributes(const PortConnection& conn, span<const AttributeSymbol* const> attributes);

    /// Gets the attributes associated with the given symbol.
    span<const AttributeSymbol* const> getAttributes(const Symbol& symbol) const;

    /// Gets the attributes associated with the given statement.
    span<const AttributeSymbol* const> getAttributes(const Statement& stmt) const;

    /// Gets the attributes associated with the given expression.
    span<const AttributeSymbol* const> getAttributes(const Expression& expr) const;

    /// Gets the attributes associated with the given port connection.
    span<const AttributeSymbol* const> getAttributes(const PortConnection& conn) const;

    /// Notes that the given symbol was imported into the current scope via a package import,
    /// and further that the current scope is within a package declaration. These symbols are
    /// candidates for being exported from this package.
    void notePackageExportCandidate(const PackageSymbol& packageScope, const Symbol& symbol);

    /// Tries to find a symbol that can be exported from the given package to satisfy an import
    /// of a given name from that package. Returns nullptr if no such symbol can be found.
    const Symbol* findPackageExportCandidate(const PackageSymbol& packageScope,
                                             string_view name) const;

    /// Notes the presence of a bind directive. The compilation uses this to decide
    /// when it has done enough traversal of the hierarchy to have seen all bind directives.
    /// If @a targetDef is non-null, the bind directive applies to all instances of the
    /// given definition, which needs special handling.
    /// @returns true if this is the first time this directive has been encountered,
    /// and false if it's already been elaborated (thus constituting an error).
    bool noteBindDirective(const BindDirectiveSyntax& syntax, const Definition* targetDef);

    /// Notes the presence of a DPI export directive. These will be checked for correctness
    /// but are otherwise unused by SystemVerilog code.
    void noteDPIExportDirective(const DPIExportSyntax& syntax, const Scope& scope);

    /// Tracks the existence of an out-of-block declaration (method or constraint) in the
    /// given scope. This can later be retrieved by calling findOutOfBlockDecl().
    void addOutOfBlockDecl(const Scope& scope, const ScopedNameSyntax& name,
                           const SyntaxNode& syntax, SymbolIndex index);

    /// Searches for an out-of-block declaration in the given @a scope with @a declName
    /// for a @a className class. Returns a tuple of syntax pointer and symbol
    /// index in the defining scope, along with a pointer that should be set to true if
    /// the resulting decl is considered "used". If not found, the syntax pointer will be null.
    std::tuple<const SyntaxNode*, SymbolIndex, bool*> findOutOfBlockDecl(
        const Scope& scope, string_view className, string_view declName) const;

    /// Notes that there is a default clocking block associated with the specified scope.
    void noteDefaultClocking(const Scope& scope, const Symbol& clocking, SourceRange range);

    /// Notes that there is a default clocking block associated with the specified scope.
    void noteDefaultClocking(const BindContext& context,
                             const DefaultClockingReferenceSyntax& syntax);

    /// Finds an applicable default clocking block for the given scope, or returns nullptr
    /// if no default clocking is in effect.
    const Symbol* getDefaultClocking(const Scope& scope) const;

    /// Notes that there is a global clocking block associated with the specified scope.
    void noteGlobalClocking(const Scope& scope, const Symbol& clocking, SourceRange range);

    /// Finds an applicable global clocking block for the given scope, or returns nullptr
    /// if no global clocking is in effect.
    const Symbol* getGlobalClocking(const Scope& scope) const;

    /// Notes that there is a default disable associated with the specified scope.
    void noteDefaultDisable(const Scope& scope, const Expression& expr);

    /// Finds an applicable default disable expression for the given scope, or returns nullptr
    /// if no such declaration is in effect.
    const Expression* getDefaultDisable(const Scope& scope) const;

    /// A convenience method for parsing a name string and turning it into a set
    /// of syntax nodes. This is mostly for testing and API purposes; normal
    /// compilation never does this.
    /// Throws an exception if there are errors parsing the name.
    const NameSyntax& parseName(string_view name);

    /// A convenience method for parsing a name string and turning it into a set
    /// of syntax nodes. This is mostly for testing and API purposes. Errors are
    /// added to the provided diagnostics bag.
    const NameSyntax& tryParseName(string_view name, Diagnostics& diags);

    /// Creates a new compilation unit within the design that can be modified dynamically,
    /// which is useful in runtime scripting scenarios. Note that this call will succeed
    /// even if the design has been finalized, but in that case any instantiations in the
    /// script scope won't affect which modules are determined to be top-level instances.
    CompilationUnitSymbol& createScriptScope();

    /// Gets the source manager associated with the compilation. If no syntax trees have
    /// been added to the design this method will return null.
    const SourceManager* getSourceManager() const { return sourceManager; }

    /// Gets the diagnostics produced during lexing, preprocessing, and syntax parsing.
    const Diagnostics& getParseDiagnostics();

    /// Gets the diagnostics produced during semantic analysis, including the binding of
    /// symbols, type checking, and name lookup. Note that this will finalize the compilation,
    /// including forcing the evaluation of any symbols or expressions that were still waiting
    /// for lazy evaluation.
    const Diagnostics& getSemanticDiagnostics();

    /// Gets all of the diagnostics produced during compilation.
    const Diagnostics& getAllDiagnostics();

    /// Adds a set of diagnostics to the compilation's list of semantic diagnostics.
    void addDiagnostics(const Diagnostics& diagnostics);

    /// Sets the default time scale to use when none is specified in the source code.
    void setDefaultTimeScale(TimeScale timeScale) { defaultTimeScale = timeScale; }

    /// Gets the default time scale to use when none is specified in the source code.
    TimeScale getDefaultTimeScale() const { return defaultTimeScale; }

    const Type& getType(SyntaxKind kind) const;
    const Type& getType(const DataTypeSyntax& node, const BindContext& context,
                        const Type* typedefTarget = nullptr);
    const Type& getType(const Type& elementType,
                        const SyntaxList<VariableDimensionSyntax>& dimensions,
                        const BindContext& context);

    const Type& getType(bitwidth_t width, bitmask<IntegralFlags> flags);
    const Type& getScalarType(bitmask<IntegralFlags> flags);
    const NetType& getNetType(TokenKind kind) const;

    /// Various built-in type symbols for easy access.
    const Type& getBitType() const { return *bitType; }
    const Type& getLogicType() const { return *logicType; }
    const Type& getIntType() const { return *intType; }
    const Type& getByteType() const { return *byteType; }
    const Type& getIntegerType() const { return *integerType; }
    const Type& getRealType() const { return *realType; }
    const Type& getShortRealType() const { return *shortRealType; }
    const Type& getStringType() const { return *stringType; }
    const Type& getVoidType() const { return *voidType; }
    const Type& getErrorType() const { return *errorType; }
    const Type& getUnsignedIntType();
    const Type& getNullType();
    const Type& getUnboundedType();
    const Type& getTypeRefType();

    /// Get the 'wire' built in net type. The rest of the built-in net types are rare enough
    /// that we don't bother providing dedicated accessors for them.
    const NetType& getWireNetType() const { return *wireNetType; }

    /// Allocates space for a constant value in the pool of constants.
    ConstantValue* allocConstant(ConstantValue&& value) {
        return constantAllocator.emplace(std::move(value));
    }

    /// Allocates a symbol map.
    SymbolMap* allocSymbolMap() { return symbolMapAllocator.emplace(); }

    /// Allocates a pointer map.
    PointerMap* allocPointerMap() { return pointerMapAllocator.emplace(); }

    /// Allocates a generic class symbol.
    template<typename... Args>
    GenericClassDefSymbol* allocGenericClass(Args&&... args) {
        return genericClassAllocator.emplace(std::forward<Args>(args)...);
    }

    const ImplicitTypeSyntax& createEmptyTypeSyntax(SourceLocation loc);

    /// Forces the given symbol and all children underneath it in the hierarchy to
    /// be elaborated and any relevant diagnostics to be issued.
    void forceElaborate(const Symbol& symbol);

    int getNextEnumSystemId() { return nextEnumSystemId++; }
    int getNextStructSystemId() { return nextStructSystemId++; }
    int getNextUnionSystemId() { return nextUnionSystemId++; }

private:
    friend class Lookup;
    friend class Scope;

    // These functions are called by Scopes to create and track various members.
    Scope::DeferredMemberData& getOrAddDeferredData(Scope::DeferredMemberIndex& index);
    void trackImport(Scope::ImportDataIndex& index, const WildcardImportSymbol& import);
    span<const WildcardImportSymbol*> queryImports(Scope::ImportDataIndex index);

    bool doTypoCorrection() const { return typoCorrections < options.typoCorrectionLimit; }
    void didTypoCorrection() { typoCorrections++; }

    span<const AttributeSymbol* const> getAttributes(const void* ptr) const;

    Diagnostic& addDiag(Diagnostic diag);

    const RootSymbol& getRoot(bool skipDefParamResolution);
    void parseParamOverrides(flat_hash_map<string_view, const ConstantValue*>& results);
    void checkDPIMethods(span<const SubroutineSymbol* const> dpiImports);
    void resolveDefParams(size_t numDefParams);

    // Stored options object.
    CompilationOptions options;

    // Specialized allocators for types that are not trivially destructible.
    TypedBumpAllocator<SymbolMap> symbolMapAllocator;
    TypedBumpAllocator<PointerMap> pointerMapAllocator;
    TypedBumpAllocator<ConstantValue> constantAllocator;
    TypedBumpAllocator<GenericClassDefSymbol> genericClassAllocator;

    // A table to look up scalar types based on combinations of the three flags: signed, fourstate,
    // reg. Two of the entries are not valid and will be nullptr (!fourstate & reg).
    Type* scalarTypeTable[8]{ nullptr };

    // Instances of all the built-in types.
    Type* bitType;
    Type* logicType;
    Type* intType;
    Type* byteType;
    Type* integerType;
    Type* realType;
    Type* shortRealType;
    Type* stringType;
    Type* voidType;
    Type* errorType;
    NetType* wireNetType;

    // Sideband data for scopes that have deferred members.
    SafeIndexedVector<Scope::DeferredMemberData, Scope::DeferredMemberIndex> deferredData;

    // Sideband data for scopes that have wildcard imports. The list of imports
    // is stored here and queried during name lookups.
    SafeIndexedVector<Scope::ImportData, Scope::ImportDataIndex> importData;

    // The lookup table for top-level modules. The value is a pair, with the second
    // element being a boolean indicating whether there exists at least one nested
    // module with the given name (requiring a more involved lookup).
    flat_hash_map<string_view, std::pair<const Definition*, bool>> topDefinitions;

    // A cache of vector types, keyed on various properties such as bit width.
    flat_hash_map<uint32_t, const Type*> vectorTypeCache;

    // Map from syntax kinds to the built-in types.
    flat_hash_map<SyntaxKind, const Type*> knownTypes;

    // Map from token kinds to the built-in net types.
    flat_hash_map<TokenKind, std::unique_ptr<NetType>> knownNetTypes;

    // The name map for packages. Note that packages have their own namespace,
    // which is why they can't share the definitions name table.
    flat_hash_map<string_view, const PackageSymbol*> packageMap;

    // The name map for system subroutines.
    flat_hash_map<string_view, std::unique_ptr<SystemSubroutine>> subroutineMap;

    // The name map for system methods.
    flat_hash_map<std::tuple<string_view, SymbolKind>, std::unique_ptr<SystemSubroutine>> methodMap;

    // Map from pointers (to symbols, statements, expressions) to their associated attributes.
    flat_hash_map<const void*, span<const AttributeSymbol* const>> attributeMap;

    // A set of all instantiated names in the design; used for determining whether a given
    // module has ever been instantiated to know whether it should be considered top-level.
    flat_hash_set<string_view> globalInstantiations;

    struct DefinitionMetadata {
        const SyntaxTree* tree = nullptr;
        const NetType* defaultNetType = nullptr;
        optional<TimeScale> timeScale;
        UnconnectedDrive unconnectedDrive = UnconnectedDrive::None;
    };

    // Map from syntax nodes to parse-time metadata about them.
    flat_hash_map<const ModuleDeclarationSyntax*, DefinitionMetadata> definitionMetadata;

    // The name map for all module, interface, and program definitions.
    // The key is a combination of definition name + the scope in which it was declared.
    flat_hash_map<std::tuple<string_view, const Scope*>, std::unique_ptr<Definition>> definitionMap;

    // A map from diag code + location to the diagnostics that have occurred at that location.
    // This is used to collapse duplicate diagnostics across instantiations into a single report.
    using DiagMap = flat_hash_map<std::tuple<DiagCode, SourceLocation>, std::vector<Diagnostic>>;
    DiagMap diagMap;

    // A map from class name + decl name + scope to out-of-block declarations. These get
    // registered when we find the initial declaration and later get used when we see
    // the class prototype. The value also includes a boolean indicating whether anything
    // has used this declaration -- an error is issued if it's never used.
    mutable flat_hash_map<std::tuple<string_view, string_view, const Scope*>,
                          std::tuple<const SyntaxNode*, const ScopedNameSyntax*, SymbolIndex, bool>>
        outOfBlockDecls;

    std::unique_ptr<RootSymbol> root;
    const SourceManager* sourceManager = nullptr;
    size_t numErrors = 0; // total number of errors inserted into the diagMap
    TimeScale defaultTimeScale;
    bool finalized = false;
    bool finalizing = false; // to prevent reentrant calls to getRoot()
    uint32_t typoCorrections = 0;
    int nextEnumSystemId = 1;
    int nextStructSystemId = 1;
    int nextUnionSystemId = 1;

    // This is storage for a temporary diagnostic that is being constructed.
    // Typically this is done in-place within the diagMap, but for diagnostics
    // that have been supressed we need space to return *something* to the caller.
    Diagnostic tempDiag;

    optional<Diagnostics> cachedParseDiagnostics;
    optional<Diagnostics> cachedSemanticDiagnostics;
    optional<Diagnostics> cachedAllDiagnostics;

    // A list of compilation units that have been added to the compilation.
    std::vector<const CompilationUnitSymbol*> compilationUnits;

    // Storage for syntax trees that have been added to the compilation.
    std::vector<std::shared_ptr<SyntaxTree>> syntaxTrees;

    // A list of definitions that are unreferenced in any instantiations and
    // are also not automatically instantiated as top-level.
    std::vector<const Definition*> unreferencedDefs;

    // The name map for user-defined primitive definitions.
    flat_hash_map<string_view, const PrimitiveSymbol*> udpMap;

    // The name map for built-in primitive definitions. These are stored in a separate
    // map because they are distinguished by keyword names that may otherwise collide
    // with escaped identifiers used by user code.
    flat_hash_map<string_view, const PrimitiveSymbol*> gateMap;

    // A map from definitions to bind directives that will create
    // instances within those definitions.
    flat_hash_map<const Definition*, std::vector<const BindDirectiveSyntax*>> bindDirectivesByDef;

    // A set tracking all bind directives we've encountered during elaboration,
    // which is used to know when we've seen them all and can stop doing early scanning.
    flat_hash_set<const BindDirectiveSyntax*> seenBindDirectives;

    // A tree of parameter overrides to apply when elaborating.
    // Note that instances store pointers into this tree so it must not be
    // modified after elaboration begins.
    ParamOverrideNode paramOverrides;

    // A list of DPI export directives we've encountered during elaboration.
    std::vector<std::pair<const DPIExportSyntax*, const Scope*>> dpiExports;

    // A map of packages to the set of names that are candidates for being
    // exported from those packages.
    flat_hash_map<const PackageSymbol*, flat_hash_map<string_view, const Symbol*>>
        packageExportCandidateMap;

    // A map of scopes to default clocking blocks.
    flat_hash_map<const Scope*, const Symbol*> defaultClockingMap;

    // A map of scopes to global clocking blocks.
    flat_hash_map<const Scope*, const Symbol*> globalClockingMap;

    // A map of scopes to default disable declarations.
    flat_hash_map<const Scope*, const Expression*> defaultDisableMap;

    // The built-in std package.
    const PackageSymbol* stdPkg = nullptr;
};

} // namespace slang
