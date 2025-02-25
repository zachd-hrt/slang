//------------------------------------------------------------------------------
// CoverSymbols.cpp
// Contains coverage-related symbol definitions
//
// File is under the MIT license; see LICENSE for details
//------------------------------------------------------------------------------
#include "slang/symbols/CoverSymbols.h"

#include "slang/binding/TimingControl.h"
#include "slang/compilation/Compilation.h"
#include "slang/diagnostics/DeclarationsDiags.h"
#include "slang/diagnostics/ExpressionsDiags.h"
#include "slang/symbols/ASTVisitor.h"
#include "slang/symbols/ClassSymbols.h"
#include "slang/symbols/SubroutineSymbols.h"
#include "slang/symbols/SymbolBuilders.h"
#include "slang/symbols/VariableSymbols.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/types/AllTypes.h"

namespace {

using namespace slang;

class OptionBuilder {
public:
    explicit OptionBuilder(const Scope& scope) : scope(scope) {}

    void add(const CoverageOptionSyntax& syntax) {
        options.emplace(scope, syntax);

        if (auto name = options.back().getName(); !name.empty()) {
            auto names = options.back().isTypeOption() ? &typeNames : &instNames;
            auto [it, inserted] = names->emplace(name, syntax.expr);
            if (!inserted) {
                auto& diag = scope.addDiag(diag::CoverageOptionDup, syntax.expr->sourceRange());
                diag << name;
                diag.addNote(diag::NotePreviousUsage, it->second->getFirstToken().location());
            }
        }
    }

    span<const CoverageOptionSetter> get() const { return options.copy(scope.getCompilation()); }

private:
    const Scope& scope;
    SmallVectorSized<CoverageOptionSetter, 4> options;
    SmallMap<string_view, const SyntaxNode*, 4> instNames;
    SmallMap<string_view, const SyntaxNode*, 4> typeNames;
};

} // namespace

namespace slang {

CoverageOptionSetter::CoverageOptionSetter(const Scope& scope, const CoverageOptionSyntax& syntax) :
    scope(&scope), syntax(&syntax) {
}

bool CoverageOptionSetter::isTypeOption() const {
    if (syntax->expr->kind == SyntaxKind::AssignmentExpression) {
        auto& assign = syntax->expr->as<BinaryExpressionSyntax>();
        if (assign.left->kind == SyntaxKind::ScopedName) {
            auto& scoped = assign.left->as<ScopedNameSyntax>();
            if (scoped.left->kind == SyntaxKind::IdentifierName) {
                return scoped.left->as<IdentifierNameSyntax>().identifier.valueText() ==
                       "type_option"sv;
            }
        }
    }
    return false;
}

string_view CoverageOptionSetter::getName() const {
    if (syntax->expr->kind == SyntaxKind::AssignmentExpression) {
        auto& assign = syntax->expr->as<BinaryExpressionSyntax>();
        if (assign.left->kind == SyntaxKind::ScopedName) {
            auto& scoped = assign.left->as<ScopedNameSyntax>();
            if (scoped.left->kind == SyntaxKind::IdentifierName &&
                scoped.right->kind == SyntaxKind::IdentifierName) {
                return scoped.right->as<IdentifierNameSyntax>().identifier.valueText();
            }
        }
    }
    return ""sv;
}

const Expression& CoverageOptionSetter::getExpression() const {
    if (!expr) {
        bitmask<BindFlags> flags = BindFlags::AssignmentAllowed;
        bool isTypeOpt = isTypeOption();
        if (isTypeOpt)
            flags |= BindFlags::StaticInitializer;

        BindContext context(*scope, LookupLocation(scope, 3));
        expr = &Expression::bind(*syntax->expr, context, flags);
        context.setAttributes(*expr, syntax->attributes);

        if (isTypeOpt && expr->kind == ExpressionKind::Assignment)
            context.eval(expr->as<AssignmentExpression>().right());
    }
    return *expr;
}

void CoverageOptionSetter::serializeTo(ASTSerializer& serializer) const {
    serializer.write("expr", getExpression());
}

static void addProperty(Scope& scope, string_view name, VariableLifetime lifetime,
                        const StructBuilder& structBuilder) {
    auto& comp = scope.getCompilation();
    auto& prop = *comp.emplace<ClassPropertySymbol>(name, SourceLocation::NoLocation, lifetime,
                                                    Visibility::Public);
    prop.setType(structBuilder.type);
    scope.addMember(prop);
}

static void addBuiltInMethods(Scope& scope, bool isCovergroup) {
    auto& comp = scope.getCompilation();
    auto makeFunc = [&](string_view funcName, const Type& returnType) {
        MethodBuilder builder(comp, funcName, returnType, SubroutineKind::Function);
        scope.addMember(builder.symbol);
        return builder;
    };

    auto& void_t = comp.getVoidType();
    auto& int_t = comp.getIntType();
    auto& real_t = comp.getRealType();
    auto& string_t = comp.getStringType();

    if (isCovergroup)
        makeFunc("set_inst_name"sv, void_t).addArg("name"sv, string_t);

    auto get_coverage = makeFunc("get_coverage"sv, real_t);
    get_coverage.addFlags(MethodFlags::Static);
    get_coverage.addArg("covered_bins"sv, int_t, ArgumentDirection::Ref, SVInt(32, 0, true));
    get_coverage.addArg("total_bins"sv, int_t, ArgumentDirection::Ref, SVInt(32, 0, true));

    auto get_inst_coverage = makeFunc("get_inst_coverage"sv, real_t);
    get_inst_coverage.addArg("covered_bins"sv, int_t, ArgumentDirection::Ref, SVInt(32, 0, true));
    get_inst_coverage.addArg("total_bins"sv, int_t, ArgumentDirection::Ref, SVInt(32, 0, true));

    makeFunc("start"sv, void_t);
    makeFunc("stop"sv, void_t);
}

CovergroupBodySymbol::CovergroupBodySymbol(Compilation& comp, SourceLocation loc) :
    Symbol(SymbolKind::CovergroupBody, ""sv, loc), Scope(comp, this) {

    auto& int_t = comp.getIntType();
    auto& bit_t = comp.getBitType();
    auto& string_t = comp.getStringType();

    StructBuilder option(*this, LookupLocation::min);
    option.addField("name"sv, string_t);
    option.addField("weight"sv, int_t);
    option.addField("goal"sv, int_t);
    option.addField("comment"sv, string_t);
    option.addField("at_least"sv, int_t);
    option.addField("auto_bin_max"sv, int_t, VariableFlags::ImmutableCoverageOption);
    option.addField("cross_num_print_missing"sv, int_t);
    option.addField("detect_overlap"sv, bit_t, VariableFlags::ImmutableCoverageOption);
    option.addField("per_instance"sv, bit_t, VariableFlags::ImmutableCoverageOption);
    option.addField("get_inst_coverage"sv, bit_t, VariableFlags::ImmutableCoverageOption);
    addProperty(*this, "option"sv, VariableLifetime::Automatic, option);

    StructBuilder type_option(*this, LookupLocation::min);
    type_option.addField("weight"sv, int_t);
    type_option.addField("goal"sv, int_t);
    type_option.addField("comment"sv, string_t);
    type_option.addField("strobe"sv, bit_t, VariableFlags::ImmutableCoverageOption);
    type_option.addField("merge_instances"sv, bit_t);
    type_option.addField("distribute_first"sv, bit_t);
    addProperty(*this, "type_option"sv, VariableLifetime::Static, type_option);

    addBuiltInMethods(*this, true);
}

void CovergroupBodySymbol::serializeTo(ASTSerializer& serializer) const {
    if (!options.empty()) {
        serializer.startArray("options");
        for (auto& opt : options)
            opt.serializeTo(serializer);
        serializer.endArray();
    }
}

CovergroupType::CovergroupType(Compilation& compilation, string_view name, SourceLocation loc,
                               const CovergroupBodySymbol& body) :
    Type(SymbolKind::CovergroupType, name, loc),
    Scope(compilation, this), body(body) {
}

const Symbol& CovergroupType::fromSyntax(const Scope& scope,
                                         const CovergroupDeclarationSyntax& syntax) {
    // If we're inside a class, this covergroup is actually anonymous and the name
    // is used to implicitly declare a property of the covergroup type.
    bool inClass = scope.asSymbol().kind == SymbolKind::ClassType;
    string_view name = inClass ? ""sv : syntax.name.valueText();

    auto& comp = scope.getCompilation();
    auto body = comp.emplace<CovergroupBodySymbol>(comp, syntax.name.location());
    auto result = comp.emplace<CovergroupType>(comp, name, syntax.name.location(), *body);
    result->setSyntax(syntax);
    result->setAttributes(scope, syntax.attributes);

    if (syntax.portList) {
        SmallVectorSized<const FormalArgumentSymbol*, 8> args;
        SubroutineSymbol::buildArguments(*result, *syntax.portList, VariableLifetime::Automatic,
                                         args);
        result->arguments = args.copy(comp);

        for (auto arg : result->arguments) {
            if (arg->direction == ArgumentDirection::Out ||
                arg->direction == ArgumentDirection::InOut) {
                scope.addDiag(diag::CovergroupOutArg, arg->location);
            }
        }
    }

    MethodBuilder sample(comp, "sample"sv, comp.getVoidType(), SubroutineKind::Function);
    body->addMember(sample.symbol);

    if (syntax.event && syntax.event->kind == SyntaxKind::WithFunctionSample) {
        auto& wfs = syntax.event->as<WithFunctionSampleSyntax>();
        if (wfs.portList) {
            SmallVectorSized<const FormalArgumentSymbol*, 8> args;
            SubroutineSymbol::buildArguments(*result, *wfs.portList, VariableLifetime::Automatic,
                                             args);

            result->sampleArguments = args.copy(comp);

            for (auto arg : result->sampleArguments) {
                if (arg->direction == ArgumentDirection::Out ||
                    arg->direction == ArgumentDirection::InOut) {
                    scope.addDiag(diag::CovergroupOutArg, arg->location);
                }

                const_cast<FormalArgumentSymbol*>(arg)->flags |=
                    VariableFlags::CoverageSampleFormal;
                sample.copyArg(*arg);
            }
        }
    }

    result->addMember(*body);

    OptionBuilder options(*body);
    for (auto member : syntax.members) {
        if (member->kind == SyntaxKind::CoverageOption)
            options.add(member->as<CoverageOptionSyntax>());
        else
            body->addMembers(*member);
    }

    body->options = options.get();

    if (inClass) {
        auto var =
            comp.emplace<ClassPropertySymbol>(syntax.name.valueText(), syntax.name.location(),
                                              VariableLifetime::Automatic, Visibility::Public);
        var->setType(*result);
        var->flags |= VariableFlags::Const;
        return *var;
    }

    return *result;
}

const TimingControl* CovergroupType::getCoverageEvent() const {
    if (event)
        return *event;

    auto scope = getParentScope();
    auto syntax = getSyntax();
    if (scope && syntax) {
        if (auto evSyntax = syntax->as<CovergroupDeclarationSyntax>().event) {
            LookupLocation ll = LookupLocation::min;
            if (!arguments.empty())
                ll = LookupLocation::after(*arguments.back());

            BindContext context(*this, ll);

            if (evSyntax->kind == SyntaxKind::BlockCoverageEvent) {
                event = &BlockEventListControl::fromSyntax(
                    *evSyntax->as<BlockCoverageEventSyntax>().expr, context);
                return *event;
            }
            else if (evSyntax->kind == SyntaxKind::EventControlWithExpression) {
                event =
                    &TimingControl::bind(evSyntax->as<EventControlWithExpressionSyntax>(), context);
                return *event;
            }
        }
    }

    event = nullptr;
    return nullptr;
}

ConstantValue CovergroupType::getDefaultValueImpl() const {
    return ConstantValue::NullPlaceholder{};
}

void CovergroupType::serializeTo(ASTSerializer& serializer) const {
    if (auto ev = getCoverageEvent())
        serializer.write("event", *ev);
}

const Expression* CoverageBinSymbol::getIffExpr() const {
    if (!isResolved)
        resolve();
    return iffExpr;
}

const Expression* CoverageBinSymbol::getNumberOfBinsExpr() const {
    if (!isResolved)
        resolve();
    return numberOfBinsExpr;
}

const Expression* CoverageBinSymbol::getSetCoverageExpr() const {
    if (!isResolved)
        resolve();
    return setCoverageExpr;
}

const Expression* CoverageBinSymbol::getWithExpr() const {
    if (!isResolved)
        resolve();
    return withExpr;
}

const BinsSelectExpr* CoverageBinSymbol::getCrossSelectExpr() const {
    if (!isResolved)
        resolve();
    return selectExpr;
}

span<const Expression* const> CoverageBinSymbol::getValues() const {
    if (!isResolved)
        resolve();
    return values;
}

span<const CoverageBinSymbol::TransSet> CoverageBinSymbol::getTransList() const {
    if (!isResolved)
        resolve();
    return transList;
}

void CoverageBinSymbol::serializeTo(ASTSerializer& serializer) const {
    switch (binsKind) {
        case Bins:
            serializer.write("binsKind"sv, "Bins"sv);
            break;
        case IllegalBins:
            serializer.write("binsKind"sv, "IllegalBins"sv);
            break;
        case IgnoreBins:
            serializer.write("binsKind"sv, "IgnoreBins"sv);
            break;
    }

    serializer.write("isArray", isArray);
    serializer.write("isWildcard", isWildcard);
    serializer.write("isDefault", isDefault);
    serializer.write("isDefaultSequence", isDefaultSequence);

    if (auto expr = getIffExpr())
        serializer.write("iff", *expr);

    if (auto expr = getNumberOfBinsExpr())
        serializer.write("numberOfBins", *expr);

    if (auto expr = getSetCoverageExpr())
        serializer.write("setCoverage", *expr);

    if (auto expr = getWithExpr())
        serializer.write("with", *expr);

    if (auto expr = getCrossSelectExpr())
        serializer.write("crossSelect", *expr);

    auto valArray = getValues();
    if (!valArray.empty()) {
        serializer.startArray("values");
        for (auto val : valArray)
            serializer.serialize(*val);
        serializer.endArray();
    }

    auto trans = getTransList();
    if (!trans.empty()) {
        serializer.startArray("trans");
        for (auto& set : trans) {
            serializer.startArray();
            for (auto& rangeList : set) {
                serializer.startObject();
                rangeList.serializeTo(serializer);
                serializer.endObject();
            }
            serializer.endArray();
        }
        serializer.endArray();
    }
}

CoverageBinSymbol& CoverageBinSymbol::fromSyntax(const Scope& scope,
                                                 const CoverageBinsSyntax& syntax) {
    auto& comp = scope.getCompilation();
    auto result = comp.emplace<CoverageBinSymbol>(syntax.name.valueText(), syntax.name.location());
    result->setSyntax(syntax);
    result->setAttributes(scope, syntax.attributes);

    result->isWildcard = syntax.wildcard.kind == TokenKind::WildcardKeyword;

    if (syntax.keyword.kind == TokenKind::IgnoreBinsKeyword)
        result->binsKind = IgnoreBins;
    else if (syntax.keyword.kind == TokenKind::IllegalBinsKeyword)
        result->binsKind = IllegalBins;

    if (syntax.size)
        result->isArray = true;

    if (syntax.initializer->kind == SyntaxKind::DefaultCoverageBinInitializer) {
        result->isDefault = true;
        if (syntax.initializer->as<DefaultCoverageBinInitializerSyntax>().sequenceKeyword)
            result->isDefaultSequence = true;
    }

    return *result;
}

CoverageBinSymbol& CoverageBinSymbol::fromSyntax(const Scope& scope,
                                                 const BinsSelectionSyntax& syntax) {
    auto& comp = scope.getCompilation();
    auto result = comp.emplace<CoverageBinSymbol>(syntax.name.valueText(), syntax.name.location());
    result->setSyntax(syntax);
    result->setAttributes(scope, syntax.attributes);

    if (syntax.keyword.kind == TokenKind::IgnoreBinsKeyword)
        result->binsKind = IgnoreBins;
    else if (syntax.keyword.kind == TokenKind::IllegalBinsKeyword)
        result->binsKind = IllegalBins;

    return *result;
}

static const Expression& bindCovergroupExpr(const ExpressionSyntax& syntax,
                                            const BindContext& context,
                                            const Type* lvalueType = nullptr,
                                            bitmask<BindFlags> extraFlags = {}) {
    const Expression* expr;
    if (lvalueType) {
        expr = &Expression::bindRValue(*lvalueType, syntax, syntax.getFirstToken().location(),
                                       context, extraFlags);
    }
    else {
        expr = &Expression::bind(syntax, context, extraFlags);
    }

    context.eval(*expr, EvalFlags::CovergroupExpr);
    return *expr;
}

void CoverageBinSymbol::resolve() const {
    ASSERT(!isResolved);
    isResolved = true;

    auto syntax = getSyntax();
    auto scope = getParentScope();
    ASSERT(syntax && scope);

    auto& comp = scope->getCompilation();
    BindContext context(*scope, LookupLocation::before(*this));

    if (syntax->kind == SyntaxKind::BinsSelection) {
        auto& binsSyntax = syntax->as<BinsSelectionSyntax>();
        if (binsSyntax.iff) {
            iffExpr = &Expression::bind(*binsSyntax.iff->expr, context,
                                        BindFlags::AllowCoverageSampleFormal);
            context.requireBooleanConvertible(*iffExpr);
        }

        selectExpr = &BinsSelectExpr::bind(*binsSyntax.expr, context);
        return;
    }

    auto& coverpoint = scope->asSymbol().as<CoverpointSymbol>();
    auto& type = coverpoint.getType();

    auto& binsSyntax = syntax->as<CoverageBinsSyntax>();
    if (binsSyntax.iff) {
        iffExpr =
            &Expression::bind(*binsSyntax.iff->expr, context, BindFlags::AllowCoverageSampleFormal);
        context.requireBooleanConvertible(*iffExpr);
    }

    if (binsSyntax.size && binsSyntax.size->expr) {
        numberOfBinsExpr = &bindCovergroupExpr(*binsSyntax.size->expr, context);
        context.requireIntegral(*numberOfBinsExpr);
    }

    auto bindWithExpr = [&](const WithClauseSyntax& withSyntax) {
        // Create the iterator variable and set it up with a bind context so that it
        // can be found by the iteration expression.
        auto it = comp.emplace<IteratorSymbol>(*context.scope, "item"sv, coverpoint.location, type);

        BindContext iterCtx = context;
        it->nextIterator = std::exchange(iterCtx.firstIterator, it);

        withExpr = &bindCovergroupExpr(*withSyntax.expr, iterCtx);
        iterCtx.requireBooleanConvertible(*withExpr);
    };

    auto init = binsSyntax.initializer;
    switch (init->kind) {
        case SyntaxKind::RangeCoverageBinInitializer: {
            SmallVectorSized<const Expression*, 4> buffer;
            auto& rcbis = init->as<RangeCoverageBinInitializerSyntax>();
            for (auto elem : rcbis.ranges->valueRanges) {
                bitmask<BindFlags> flags;
                if (elem->kind == SyntaxKind::OpenRangeExpression)
                    flags = BindFlags::AllowUnboundedLiteral;

                auto& expr = bindCovergroupExpr(*elem, context, &type, flags);
                buffer.append(&expr);
            }
            values = buffer.copy(comp);

            if (rcbis.withClause)
                bindWithExpr(*rcbis.withClause);
            break;
        }
        case SyntaxKind::IdWithExprCoverageBinInitializer: {
            auto& iwecbi = init->as<IdWithExprCoverageBinInitializerSyntax>();
            bindWithExpr(*iwecbi.withClause);

            auto targetName = iwecbi.id.valueText();
            if (!targetName.empty() && targetName != coverpoint.name)
                context.addDiag(diag::CoverageBinTargetName, iwecbi.id.range()) << coverpoint.name;
            break;
        }
        case SyntaxKind::TransListCoverageBinInitializer: {
            SmallVectorSized<TransSet, 4> listBuffer;
            for (auto setElem : init->as<TransListCoverageBinInitializerSyntax>().sets) {
                SmallVectorSized<TransRangeList, 4> setBuffer;
                for (auto rangeElem : setElem->ranges)
                    setBuffer.emplace(*rangeElem, type, context);
                listBuffer.append(setBuffer.copy(comp));
            }
            transList = listBuffer.copy(comp);
            break;
        }
        case SyntaxKind::ExpressionCoverageBinInitializer:
            setCoverageExpr = &bindCovergroupExpr(
                *init->as<ExpressionCoverageBinInitializerSyntax>().expr, context);

            if (!setCoverageExpr->bad()) {
                auto& t = *setCoverageExpr->type;
                if (!t.isArray() || t.isAssociativeArray() ||
                    !type.isAssignmentCompatible(*t.getArrayElementType())) {

                    auto& diag =
                        context.addDiag(diag::CoverageSetType, setCoverageExpr->sourceRange);
                    diag << t << coverpoint.name << type;
                }
            }
            break;
        case SyntaxKind::DefaultCoverageBinInitializer:
            // Already handled at construction time.
            break;
        default:
            THROW_UNREACHABLE;
    }
}

CoverageBinSymbol::TransRangeList::TransRangeList(const TransRangeSyntax& syntax, const Type& type,
                                                  const BindContext& context) {
    SmallVectorSized<const Expression*, 4> buffer;
    for (auto elem : syntax.items) {
        auto& expr = bindCovergroupExpr(*elem, context, &type);
        buffer.append(&expr);
    }

    auto& comp = context.getCompilation();
    items = buffer.copy(comp);

    if (syntax.repeat) {
        switch (syntax.repeat->specifier.kind) {
            case TokenKind::Star:
                repeatKind = Consecutive;
                break;
            case TokenKind::Equals:
                repeatKind = Nonconsecutive;
                break;
            case TokenKind::MinusArrow:
                repeatKind = GoTo;
                break;
            default:
                THROW_UNREACHABLE;
        }

        auto bindCount = [&](const ExpressionSyntax& exprSyntax) {
            auto& expr = bindCovergroupExpr(exprSyntax, context);
            context.requireIntegral(expr);
            return &expr;
        };

        if (auto sel = syntax.repeat->selector) {
            if (sel->kind == SyntaxKind::BitSelect) {
                repeatFrom = bindCount(*sel->as<BitSelectSyntax>().expr);
            }
            else {
                auto& rss = sel->as<RangeSelectSyntax>();
                repeatFrom = bindCount(*rss.left);
                repeatTo = bindCount(*rss.right);
            }
        }
    }
}

void CoverageBinSymbol::TransRangeList::serializeTo(ASTSerializer& serializer) const {
    serializer.startArray("items");
    for (auto item : items)
        serializer.serialize(*item);
    serializer.endArray();

    if (repeatFrom)
        serializer.write("repeatFrom", *repeatFrom);
    if (repeatTo)
        serializer.write("repeatTo", *repeatTo);

    switch (repeatKind) {
        case Consecutive:
            serializer.write("repeatKind", "Consecutive"sv);
            break;
        case Nonconsecutive:
            serializer.write("repeatKind", "Nonconsecutive"sv);
            break;
        case GoTo:
            serializer.write("repeatKind", "GoTo"sv);
            break;
        default:
            break;
    }
}

CoverpointSymbol::CoverpointSymbol(Compilation& comp, string_view name, SourceLocation loc) :
    Symbol(SymbolKind::Coverpoint, name, loc), Scope(comp, this),
    declaredType(*this, DeclaredTypeFlags::InferImplicit | DeclaredTypeFlags::AutomaticInitializer |
                            DeclaredTypeFlags::CoverageType) {

    // Set the overrideIndex for the type and expression so that they cannot refer to
    // other members of the parent covergroup. This allows coverpoints named the same
    // as formal arguments to not interfere with lookup.
    declaredType.setOverrideIndex(SymbolIndex(1));

    auto& int_t = comp.getIntType();
    auto& bit_t = comp.getBitType();
    auto& string_t = comp.getStringType();

    StructBuilder option(*this, LookupLocation::min);
    option.addField("weight"sv, int_t);
    option.addField("goal"sv, int_t);
    option.addField("comment"sv, string_t);
    option.addField("at_least"sv, int_t);
    option.addField("auto_bin_max"sv, int_t, VariableFlags::ImmutableCoverageOption);
    option.addField("detect_overlap"sv, bit_t, VariableFlags::ImmutableCoverageOption);
    addProperty(*this, "option"sv, VariableLifetime::Automatic, option);

    StructBuilder type_option(*this, LookupLocation::min);
    type_option.addField("weight"sv, int_t);
    type_option.addField("goal"sv, int_t);
    type_option.addField("comment"sv, string_t);
    addProperty(*this, "type_option"sv, VariableLifetime::Static, type_option);

    addBuiltInMethods(*this, false);
}

CoverpointSymbol& CoverpointSymbol::fromSyntax(const Scope& scope, const CoverpointSyntax& syntax) {
    // It's possible for invalid syntax to parse as a coverpoint. If the keyword wasn't
    // given just give up and return a placeholder.
    auto& comp = scope.getCompilation();
    if (syntax.coverpoint.isMissing()) {
        auto result = comp.emplace<CoverpointSymbol>(comp, ""sv, syntax.getFirstToken().location());
        result->declaredType.setType(comp.getErrorType());
        return *result;
    }

    // Figure out the name of the coverpoint. If there's a label, it provides the name.
    // Otherwise check if the expression is a simple variable reference. If so, we take
    // that variable name as the name of the coverpoint. Otherwise it's unnamed.
    string_view name;
    SourceLocation loc;
    if (syntax.label) {
        name = syntax.label->name.valueText();
        loc = syntax.label->name.location();
    }
    else if (syntax.expr->kind == SyntaxKind::IdentifierName) {
        auto id = syntax.expr->as<IdentifierNameSyntax>().identifier;
        name = id.valueText();
        loc = id.location();
    }
    else {
        loc = syntax.expr->getFirstToken().location();
    }

    auto result = comp.emplace<CoverpointSymbol>(comp, name, loc);
    result->setSyntax(syntax);
    result->setAttributes(scope, syntax.attributes);

    result->declaredType.setTypeSyntax(*syntax.type);
    result->declaredType.setInitializerSyntax(*syntax.expr,
                                              syntax.expr->getFirstToken().location());

    OptionBuilder options(*result);
    for (auto member : syntax.members) {
        if (member->kind == SyntaxKind::CoverageOption)
            options.add(member->as<CoverageOptionSyntax>());
        else
            result->addMembers(*member);
    }

    result->options = options.get();
    return *result;
}

CoverpointSymbol& CoverpointSymbol::fromImplicit(const Scope& scope,
                                                 const IdentifierNameSyntax& syntax) {
    auto loc = syntax.identifier.location();
    auto& comp = scope.getCompilation();
    auto result = comp.emplace<CoverpointSymbol>(comp, syntax.identifier.valueText(), loc);

    result->declaredType.setTypeSyntax(comp.createEmptyTypeSyntax(loc));
    result->declaredType.setInitializerSyntax(syntax, loc);
    return *result;
}

const Expression* CoverpointSymbol::getIffExpr() const {
    if (!iffExpr) {
        auto scope = getParentScope();
        auto syntax = getSyntax();
        ASSERT(scope);

        if (!syntax)
            iffExpr = nullptr;
        else {
            auto iffSyntax = syntax->as<CoverpointSyntax>().iff;
            if (!iffSyntax)
                iffExpr = nullptr;
            else {
                BindContext context(*scope, LookupLocation::min);
                iffExpr = &Expression::bind(*iffSyntax->expr, context,
                                            BindFlags::AllowCoverageSampleFormal);
                context.requireBooleanConvertible(*iffExpr.value());
            }
        }
    }
    return *iffExpr;
}

void CoverpointSymbol::serializeTo(ASTSerializer& serializer) const {
    if (!options.empty()) {
        serializer.startArray("options");
        for (auto& opt : options)
            opt.serializeTo(serializer);
        serializer.endArray();
    }

    if (auto iff = getIffExpr())
        serializer.write("iff", *iff);
}

CoverCrossSymbol::CoverCrossSymbol(Compilation& comp, string_view name, SourceLocation loc,
                                   span<const CoverpointSymbol* const> targets) :
    Symbol(SymbolKind::CoverCross, name, loc),
    Scope(comp, this), targets(targets) {

    auto& int_t = comp.getIntType();
    auto& string_t = comp.getStringType();

    StructBuilder option(*this, LookupLocation::min);
    option.addField("weight"sv, int_t);
    option.addField("goal"sv, int_t);
    option.addField("comment"sv, string_t);
    option.addField("at_least"sv, int_t);
    option.addField("cross_num_print_missing"sv, int_t);
    addProperty(*this, "option"sv, VariableLifetime::Automatic, option);

    StructBuilder type_option(*this, LookupLocation::min);
    type_option.addField("weight"sv, int_t);
    type_option.addField("goal"sv, int_t);
    type_option.addField("comment"sv, string_t);
    addProperty(*this, "type_option"sv, VariableLifetime::Static, type_option);

    addBuiltInMethods(*this, false);
}

void CoverCrossSymbol::fromSyntax(const Scope& scope, const CoverCrossSyntax& syntax,
                                  SmallVector<const Symbol*>& results) {
    string_view name;
    SourceLocation loc;
    if (syntax.label) {
        name = syntax.label->name.valueText();
        loc = syntax.label->name.location();
    }
    else {
        loc = syntax.cross.location();
    }

    SmallVectorSized<const CoverpointSymbol*, 4> targets;
    for (auto item : syntax.items) {
        auto symbol = scope.find(item->identifier.valueText());
        if (symbol && symbol->kind == SymbolKind::Coverpoint) {
            targets.append(&symbol->as<CoverpointSymbol>());
        }
        else {
            // If we didn't find a coverpoint, create one implicitly
            // that will be initialized with this expression.
            auto& newPoint = CoverpointSymbol::fromImplicit(scope, *item);
            targets.append(&newPoint);
            results.append(&newPoint);
        }
    }

    auto& comp = scope.getCompilation();
    auto result = comp.emplace<CoverCrossSymbol>(comp, name, loc, targets.copy(comp));
    result->setSyntax(syntax);
    result->setAttributes(scope, syntax.attributes);

    auto body = comp.emplace<CoverCrossBodySymbol>(comp, loc);
    result->addMember(*body);

    StructBuilder valType(*body, LookupLocation::min);
    for (auto item : targets)
        valType.addField(item->name, item->declaredType);

    auto valType_t = comp.emplace<TypeAliasType>("CrossValType", loc);
    valType_t->targetType.setType(valType.type);
    body->addMember(*valType_t);

    auto queueType = comp.emplace<QueueType>(*valType_t, 0u);
    auto queueType_t = comp.emplace<TypeAliasType>("CrossQueueType", loc);
    queueType_t->targetType.setType(*queueType);
    body->addMember(*queueType_t);
    body->crossQueueType = queueType_t;

    OptionBuilder options(*result);
    for (auto member : syntax.members) {
        if (member->kind == SyntaxKind::CoverageOption)
            options.add(member->as<CoverageOptionSyntax>());
        else
            body->addMembers(*member);
    }

    result->options = options.get();
    results.append(result);
}

const Expression* CoverCrossSymbol::getIffExpr() const {
    if (!iffExpr) {
        auto scope = getParentScope();
        auto syntax = getSyntax();
        ASSERT(scope);

        if (!syntax)
            iffExpr = nullptr;
        else {
            auto iffSyntax = syntax->as<CoverCrossSyntax>().iff;
            if (!iffSyntax)
                iffExpr = nullptr;
            else {
                BindContext context(*scope, LookupLocation::min);
                iffExpr = &Expression::bind(*iffSyntax->expr, context,
                                            BindFlags::AllowCoverageSampleFormal);
                context.requireBooleanConvertible(*iffExpr.value());
            }
        }
    }
    return *iffExpr;
}

void CoverCrossSymbol::serializeTo(ASTSerializer& serializer) const {
    serializer.startArray("targets");
    for (auto target : targets) {
        serializer.startObject();
        serializer.writeLink("coverpoint", *target);
        serializer.endObject();
    }
    serializer.endArray();

    if (!options.empty()) {
        serializer.startArray("options");
        for (auto& opt : options)
            opt.serializeTo(serializer);
        serializer.endArray();
    }

    if (auto iff = getIffExpr())
        serializer.write("iff", *iff);
}

const BinsSelectExpr& BinsSelectExpr::bind(const BinsSelectExpressionSyntax& syntax,
                                           const BindContext& context) {
    BinsSelectExpr* result;
    switch (syntax.kind) {
        case SyntaxKind::ParenthesizedBinsSelectExpr:
            return bind(*syntax.as<ParenthesizedBinsSelectExprSyntax>().expr, context);
        case SyntaxKind::BinsSelectConditionExpr:
            result = &ConditionBinsSelectExpr::fromSyntax(
                syntax.as<BinsSelectConditionExprSyntax>(), context);
            break;
        case SyntaxKind::UnaryBinsSelectExpr:
            result =
                &UnaryBinsSelectExpr::fromSyntax(syntax.as<UnaryBinsSelectExprSyntax>(), context);
            break;
        case SyntaxKind::BinaryBinsSelectExpr:
            result =
                &BinaryBinsSelectExpr::fromSyntax(syntax.as<BinaryBinsSelectExprSyntax>(), context);
            break;
        case SyntaxKind::SimpleBinsSelectExpr:
            result = &SetExprBinsSelectExpr::fromSyntax(syntax.as<SimpleBinsSelectExprSyntax>(),
                                                        context);
            break;
        case SyntaxKind::BinSelectWithFilterExpr:
            result = &BinSelectWithFilterExpr::fromSyntax(
                syntax.as<BinSelectWithFilterExprSyntax>(), context);
            break;
        default:
            THROW_UNREACHABLE;
    }

    result->syntax = &syntax;
    return *result;
}

BinsSelectExpr& BinsSelectExpr::badExpr(Compilation& compilation, const BinsSelectExpr* expr) {
    return *compilation.emplace<InvalidBinsSelectExpr>(expr);
}

void InvalidBinsSelectExpr::serializeTo(ASTSerializer& serializer) const {
    if (child)
        serializer.write("child", *child);
}

BinsSelectExpr& ConditionBinsSelectExpr::fromSyntax(const BinsSelectConditionExprSyntax& syntax,
                                                    const BindContext& context) {
    auto& comp = context.getCompilation();
    auto& nameExpr = Expression::bind(*syntax.name, context, BindFlags::AllowCoverpoint);
    if (nameExpr.bad())
        return badExpr(comp, nullptr);

    auto sym = nameExpr.getSymbolReference();
    if (!sym || (sym->kind != SymbolKind::Coverpoint &&
                 (sym->kind != SymbolKind::CoverageBin ||
                  sym->getParentScope()->asSymbol().kind != SymbolKind::Coverpoint))) {
        context.addDiag(diag::InvalidBinsTarget, syntax.name->sourceRange());
        return badExpr(comp, nullptr);
    }

    auto expr = comp.emplace<ConditionBinsSelectExpr>(*sym);

    if (syntax.intersects) {
        const Type* type;
        if (sym->kind == SymbolKind::Coverpoint)
            type = &sym->as<CoverpointSymbol>().declaredType.getType();
        else
            type = &sym->getParentScope()->asSymbol().as<CoverpointSymbol>().declaredType.getType();

        SmallVectorSized<const Expression*, 4> buffer;
        for (auto elem : syntax.intersects->ranges->valueRanges) {
            bitmask<BindFlags> flags;
            if (elem->kind == SyntaxKind::OpenRangeExpression)
                flags = BindFlags::AllowUnboundedLiteral;

            auto& elemExpr = bindCovergroupExpr(*elem, context, type, flags);
            buffer.append(&elemExpr);
        }
        expr->intersects = buffer.copy(comp);
    }

    return *expr;
}

void ConditionBinsSelectExpr::serializeTo(ASTSerializer& serializer) const {
    serializer.writeLink("target", target);
    if (!intersects.empty()) {
        serializer.startArray("intersects");
        for (auto item : intersects)
            serializer.serialize(*item);
        serializer.endArray();
    }
}

BinsSelectExpr& UnaryBinsSelectExpr::fromSyntax(const UnaryBinsSelectExprSyntax& syntax,
                                                const BindContext& context) {
    auto& comp = context.getCompilation();
    auto& expr = bind(*syntax.expr, context);
    return *comp.emplace<UnaryBinsSelectExpr>(expr);
}

void UnaryBinsSelectExpr::serializeTo(ASTSerializer& serializer) const {
    serializer.write("expr", expr);
    serializer.write("op", "negation"sv);
}

BinsSelectExpr& BinaryBinsSelectExpr::fromSyntax(const BinaryBinsSelectExprSyntax& syntax,
                                                 const BindContext& context) {
    auto& comp = context.getCompilation();
    auto& left = bind(*syntax.left, context);
    auto& right = bind(*syntax.right, context);
    Op op = syntax.op.kind == TokenKind::DoubleAnd ? And : Or;
    return *comp.emplace<BinaryBinsSelectExpr>(left, right, op);
}

void BinaryBinsSelectExpr::serializeTo(ASTSerializer& serializer) const {
    serializer.write("left", left);
    serializer.write("right", right);
    serializer.write("op", op == And ? "and"sv : "or"sv);
}

BinsSelectExpr& SetExprBinsSelectExpr::fromSyntax(const SimpleBinsSelectExprSyntax& syntax,
                                                  const BindContext& context) {
    auto& body = context.scope->asSymbol().as<CoverCrossBodySymbol>();
    ASSERT(body.crossQueueType);

    auto parent = body.getParentScope();
    ASSERT(parent);

    // If the syntax is a simple identifier that names our parent cross,
    // we're selecting the whole cross (which is otherwise not an expression).
    auto& comp = context.getCompilation();
    auto& cross = parent->asSymbol().as<CoverCrossSymbol>();
    if (syntax.expr->kind == SyntaxKind::IdentifierName &&
        syntax.expr->as<IdentifierNameSyntax>().identifier.valueText() == cross.name) {

        if (syntax.matchesClause)
            context.addDiag(diag::InvalidBinsMatches, syntax.matchesClause->sourceRange());

        return *comp.emplace<CrossIdBinsSelectExpr>();
    }

    const Expression* matches = nullptr;
    if (syntax.matchesClause) {
        matches =
            &bindCovergroupExpr(*syntax.matchesClause->pattern->as<ExpressionPatternSyntax>().expr,
                                context, nullptr, BindFlags::AllowUnboundedLiteral);
        if (!matches->bad() && !matches->type->isUnbounded())
            context.requireIntegral(*matches);
    }

    auto& expr = Expression::bindRValue(*body.crossQueueType, *syntax.expr,
                                        syntax.expr->getFirstToken().location(), context);

    return *comp.emplace<SetExprBinsSelectExpr>(expr, matches);
}

void SetExprBinsSelectExpr::serializeTo(ASTSerializer& serializer) const {
    serializer.write("expr", expr);
    if (matchesExpr)
        serializer.write("matchesExpr", *matchesExpr);
}

BinsSelectExpr& BinSelectWithFilterExpr::fromSyntax(const BinSelectWithFilterExprSyntax& syntax,
                                                    const BindContext& context) {
    auto& comp = context.getCompilation();
    auto& expr = bind(*syntax.expr, context);

    // Create the iterator variables for all of the parent cross items
    // and then bind the filter expression.
    BindContext iterCtx = context;

    auto& cross = context.scope->asSymbol().getParentScope()->asSymbol().as<CoverCrossSymbol>();
    for (auto target : cross.targets) {
        auto it = comp.emplace<IteratorSymbol>(*context.scope, target->name, target->location,
                                               target->getType());
        it->nextIterator = std::exchange(iterCtx.firstIterator, it);
    }

    auto& filter = bindCovergroupExpr(*syntax.filter, iterCtx);
    iterCtx.requireBooleanConvertible(filter);

    const Expression* matches = nullptr;
    if (syntax.matchesClause) {
        matches =
            &bindCovergroupExpr(*syntax.matchesClause->pattern->as<ExpressionPatternSyntax>().expr,
                                context, nullptr, BindFlags::AllowUnboundedLiteral);
        if (!matches->bad() && !matches->type->isUnbounded())
            context.requireIntegral(*matches);
    }

    return *comp.emplace<BinSelectWithFilterExpr>(expr, filter, matches);
}

void BinSelectWithFilterExpr::serializeTo(ASTSerializer& serializer) const {
    serializer.write("expr", expr);
    serializer.write("filter", filter);
    if (matchesExpr)
        serializer.write("matchesExpr", *matchesExpr);
}

} // namespace slang
