//------------------------------------------------------------------------------
// MiscExpressions.cpp
// Definitions for miscellaneous expressions
//
// File is under the MIT license; see LICENSE for details
//------------------------------------------------------------------------------
#include "slang/binding/MiscExpressions.h"

#include "slang/binding/AssertionExpr.h"
#include "slang/binding/AssignmentExpressions.h"
#include "slang/binding/TimingControl.h"
#include "slang/compilation/Compilation.h"
#include "slang/diagnostics/ConstEvalDiags.h"
#include "slang/diagnostics/ExpressionsDiags.h"
#include "slang/diagnostics/LookupDiags.h"
#include "slang/diagnostics/StatementsDiags.h"
#include "slang/symbols/ASTSerializer.h"
#include "slang/symbols/BlockSymbols.h"
#include "slang/symbols/ClassSymbols.h"
#include "slang/symbols/MemberSymbols.h"
#include "slang/symbols/ParameterSymbols.h"
#include "slang/symbols/SubroutineSymbols.h"
#include "slang/symbols/VariableSymbols.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/types/AllTypes.h"
#include "slang/types/NetType.h"

namespace slang {

Expression& ValueExpressionBase::fromSymbol(const BindContext& context, const Symbol& symbol,
                                            bool isHierarchical, SourceRange sourceRange,
                                            bool constraintAllowed) {
    // Automatic variables have additional restrictions.
    Compilation& comp = context.getCompilation();
    if (VariableSymbol::isKind(symbol.kind) &&
        symbol.as<VariableSymbol>().lifetime == VariableLifetime::Automatic) {

        // If this is actually a class property, check that no static methods,
        // initializers, or nested classes are accessing it.
        if (symbol.kind == SymbolKind::ClassProperty) {
            if (!Lookup::ensureAccessible(symbol, context, sourceRange))
                return badExpr(comp, nullptr);
        }
        else if (context.flags.has(BindFlags::NonProcedural)) {
            context.addDiag(diag::AutoFromNonProcedural, sourceRange) << symbol.name;
            return badExpr(comp, nullptr);
        }
        else if (context.flags.has(BindFlags::StaticInitializer)) {
            context.addDiag(diag::AutoFromStaticInit, sourceRange) << symbol.name;
            return badExpr(comp, nullptr);
        }
        else if (context.flags.has(BindFlags::NonBlockingTimingControl)) {
            context.addDiag(diag::AutoFromNonBlockingTiming, sourceRange) << symbol.name;
            return badExpr(comp, nullptr);
        }
        else if (!context.flags.has(BindFlags::AllowCoverageSampleFormal) &&
                 symbol.as<VariableSymbol>().flags.has(VariableFlags::CoverageSampleFormal)) {
            context.addDiag(diag::CoverageSampleFormal, sourceRange) << symbol.name;
            return badExpr(comp, nullptr);
        }
        else if (context.flags.has(BindFlags::EventExpression) &&
                 symbol.kind == SymbolKind::LocalAssertionVar) {
            context.addDiag(diag::LocalVarEventExpr, sourceRange) << symbol.name;
            return badExpr(comp, nullptr);
        }
    }
    else if (symbol.kind == SymbolKind::ConstraintBlock) {
        if (!symbol.as<ConstraintBlockSymbol>().isStatic)
            Lookup::ensureAccessible(symbol, context, sourceRange);
    }
    else if (symbol.kind == SymbolKind::Parameter &&
             !context.flags.has(BindFlags::AllowUnboundedLiteral) &&
             symbol.as<ParameterSymbol>().getValue(sourceRange).isUnbounded()) {
        context.addDiag(diag::UnboundedNotAllowed, sourceRange);
        return badExpr(comp, nullptr);
    }
    else if (symbol.kind == SymbolKind::Net &&
             symbol.as<NetSymbol>().netType.netKind == NetType::Interconnect &&
             !context.flags.has(BindFlags::AllowInterconnect)) {
        context.addDiag(diag::InterconnectReference, sourceRange) << symbol.name;
        return badExpr(comp, nullptr);
    }
    else if (symbol.kind == SymbolKind::ClockVar && !context.flags.has(BindFlags::LValue) &&
             symbol.as<ClockVarSymbol>().direction == ArgumentDirection::Out) {
        context.addDiag(diag::ClockVarOutputRead, sourceRange) << symbol.name;
        return badExpr(comp, nullptr);
    }

    if (!symbol.isValue()) {
        if ((symbol.kind == SymbolKind::ClockingBlock &&
             context.flags.has(BindFlags::AllowClockingBlock)) ||
            (symbol.kind == SymbolKind::ConstraintBlock && constraintAllowed) ||
            (symbol.kind == SymbolKind::Coverpoint &&
             context.flags.has(BindFlags::AllowCoverpoint))) {
            // Special case for event expressions and constraint block built-in methods.
            return *comp.emplace<HierarchicalReferenceExpression>(symbol, comp.getVoidType(),
                                                                  sourceRange);
        }

        context.addDiag(diag::NotAValue, sourceRange) << symbol.name;
        return badExpr(comp, nullptr);
    }

    // chandles can't be referenced in sequence expressions
    auto& value = symbol.as<ValueSymbol>();
    if (context.flags.has(BindFlags::AssertionExpr) && value.getType().isCHandle()) {
        context.addDiag(diag::CHandleInAssertion, sourceRange);
        return badExpr(comp, nullptr);
    }

    if (isHierarchical)
        return *comp.emplace<HierarchicalValueExpression>(value, sourceRange);
    else
        return *comp.emplace<NamedValueExpression>(value, sourceRange);
}

bool ValueExpressionBase::requireLValueImpl(const BindContext& context, SourceLocation location,
                                            bitmask<AssignFlags> flags,
                                            const Expression* longestStaticPrefix,
                                            EvalContext* customEvalContext) const {
    if (!location)
        location = sourceRange.start();

    if (symbol.kind == SymbolKind::Parameter || symbol.kind == SymbolKind::EnumValue ||
        symbol.kind == SymbolKind::Specparam) {
        auto& diag = context.addDiag(diag::CantModifyConst, location) << symbol.name;
        diag.addNote(diag::NoteDeclarationHere, symbol.location);
        diag << sourceRange;
        return false;
    }

    if (context.flags.has(BindFlags::NonProcedural)) {
        // chandles can only be assigned in procedural contexts.
        if (symbol.getType().isCHandle()) {
            context.addDiag(diag::AssignToCHandle, sourceRange);
            return false;
        }

        if (symbol.kind == SymbolKind::Net &&
            symbol.as<NetSymbol>().netType.netKind == NetType::UWire &&
            flags.has(AssignFlags::InOutPort)) {
            context.addDiag(diag::InOutUWireConn, sourceRange) << symbol.name;
            return false;
        }
    }
    else {
        // Nets can't be assigned in procedural contexts.
        if (symbol.kind == SymbolKind::Net) {
            context.addDiag(diag::AssignToNet, sourceRange);
            return false;
        }
    }

    if (VariableSymbol::isKind(symbol.kind)) {
        if (!checkVariableAssignment(context, symbol.as<VariableSymbol>(), flags, location,
                                     sourceRange)) {
            return false;
        }
    }

    if (!longestStaticPrefix)
        longestStaticPrefix = this;
    context.addDriver(symbol, *longestStaticPrefix, flags, customEvalContext);

    return true;
}

bool ValueExpressionBase::checkVariableAssignment(const BindContext& context,
                                                  const VariableSymbol& var,
                                                  bitmask<AssignFlags> flags,
                                                  SourceLocation assignLoc, SourceRange varRange) {
    auto reportErr = [&](DiagCode code) {
        if (!assignLoc)
            assignLoc = varRange.start();

        auto& diag = context.addDiag(code, assignLoc);
        diag.addNote(diag::NoteDeclarationHere, var.location);
        diag << var.name << varRange;
        return false;
    };

    if (var.flags.has(VariableFlags::Const)) {
        // If we are in a class constructor and this variable does not have an initializer,
        // it's ok to assign to it.
        const Symbol* parent = &context.scope->asSymbol();
        while (parent->kind == SymbolKind::StatementBlock) {
            auto parentScope = parent->getParentScope();
            ASSERT(parentScope);
            parent = &parentScope->asSymbol();
        }

        if (var.getInitializer() || parent->kind != SymbolKind::Subroutine ||
            (parent->as<SubroutineSymbol>().flags & MethodFlags::Constructor) == 0) {
            return reportErr(diag::AssignmentToConstVar);
        }
    }

    if (flags.has(AssignFlags::NonBlocking) && var.lifetime == VariableLifetime::Automatic &&
        var.kind != SymbolKind::ClassProperty) {
        return reportErr(diag::NonblockingAssignmentToAuto);
    }

    if (var.kind == SymbolKind::ClockVar) {
        if (flags.has(AssignFlags::InConcat))
            reportErr(diag::ClockVarAssignConcat);

        auto& cv = var.as<ClockVarSymbol>();
        if (cv.direction == ArgumentDirection::In)
            return reportErr(diag::WriteToInputClockVar);

        if (!flags.has(AssignFlags::NonBlocking))
            return reportErr(diag::ClockVarSyncDrive);
    }

    if (flags.has(AssignFlags::InOutPort))
        return reportErr(diag::InOutVarPortConn);

    // TODO: modport assignability checks
    return true;
}

optional<bitwidth_t> ValueExpressionBase::getEffectiveWidthImpl() const {
    auto cvToWidth = [this](const ConstantValue& cv) -> optional<bitwidth_t> {
        if (!cv.isInteger())
            return std::nullopt;

        auto& sv = cv.integer();
        if (sv.hasUnknown())
            return type->getBitWidth();

        if (sv.isNegative())
            return sv.getMinRepresentedBits();

        return sv.getActiveBits();
    };

    switch (symbol.kind) {
        case SymbolKind::Parameter:
            return cvToWidth(symbol.as<ParameterSymbol>().getValue(sourceRange));
        case SymbolKind::EnumValue:
            return cvToWidth(symbol.as<EnumValueSymbol>().getValue(sourceRange));
        case SymbolKind::Specparam:
            return cvToWidth(symbol.as<SpecparamSymbol>().getValue(sourceRange));
        default:
            return type->getBitWidth();
    }
}

bool ValueExpressionBase::checkConstantBase(EvalContext& context) const {
    // Class types are disallowed in constant expressions. Note that I don't see anything
    // in the spec that would explicitly disallow them, but literally every tool issues
    // an error so for now we will follow suit.
    if (type->isClass()) {
        context.addDiag(diag::ConstEvalClassType, sourceRange);
        return false;
    }

    // Same for covergroups.
    if (type->isCovergroup()) {
        context.addDiag(diag::ConstEvalCovergroupType, sourceRange);
        return false;
    }

    if (symbol.kind == SymbolKind::Specparam && !context.flags.has(EvalFlags::SpecparamsAllowed)) {
        context.addDiag(diag::SpecparamInConstant, sourceRange);
        return false;
    }

    return true;
}

void ValueExpressionBase::serializeTo(ASTSerializer& serializer) const {
    serializer.writeLink("symbol", symbol);
}

ConstantValue NamedValueExpression::evalImpl(EvalContext& context) const {
    if (!checkConstant(context))
        return nullptr;

    switch (symbol.kind) {
        case SymbolKind::Parameter: {
            auto v = symbol.as<ParameterSymbol>().getValue(sourceRange);
            if (v.isUnbounded()) {
                if (auto target = context.getQueueTarget()) {
                    int32_t size = (int32_t)target->queue()->size();
                    return SVInt(32, uint64_t(size - 1), true);
                }
            }
            return v;
        }
        case SymbolKind::EnumValue:
            return symbol.as<EnumValueSymbol>().getValue(sourceRange);
        case SymbolKind::Specparam:
            return symbol.as<SpecparamSymbol>().getValue(sourceRange);
        default:
            ConstantValue* v = context.findLocal(&symbol);
            if (v)
                return *v;
            break;
    }

    // Special casing for covergroup expressions: they are required to be
    // constant, except they can also reference local non-elaboration constants
    // and non-ref formal args.
    if (context.flags.has(EvalFlags::CovergroupExpr)) {
        if (symbol.kind == SymbolKind::FormalArgument) {
            if (symbol.as<FormalArgumentSymbol>().direction == ArgumentDirection::Ref)
                context.addDiag(diag::CoverageExprVar, sourceRange);
        }
        else if (VariableSymbol::isKind(symbol.kind)) {
            if (!symbol.as<VariableSymbol>().flags.has(VariableFlags::Const))
                context.addDiag(diag::CoverageExprVar, sourceRange);
        }
        else if (symbol.kind != SymbolKind::Parameter && symbol.kind != SymbolKind::EnumValue) {
            context.addDiag(diag::CoverageExprVar, sourceRange);
        }
        return nullptr;
    }

    // If we reach this point, the variable was not found, which should mean that
    // it's not actually constant.
    auto& diag = context.addDiag(diag::ConstEvalNonConstVariable, sourceRange) << symbol.name;
    diag.addNote(diag::NoteDeclarationHere, symbol.location);
    return nullptr;
}

LValue NamedValueExpression::evalLValueImpl(EvalContext& context) const {
    if (!checkConstant(context))
        return nullptr;

    auto cv = context.findLocal(&symbol);
    if (!cv) {
        auto& diag = context.addDiag(diag::ConstEvalNonConstVariable, sourceRange) << symbol.name;
        diag.addNote(diag::NoteDeclarationHere, symbol.location);
        return nullptr;
    }

    return LValue(*cv);
}

bool NamedValueExpression::checkConstant(EvalContext& context) const {
    if (context.flags.has(EvalFlags::IsScript))
        return true;

    if (!checkConstantBase(context))
        return false;

    if (!context.inFunction())
        return true;

    const EvalContext::Frame& frame = context.topFrame();
    const SubroutineSymbol* subroutine = frame.subroutine;
    if (!subroutine)
        return true;

    // Constant functions have a bunch of additional restrictions. See [13.4.4]:
    // - All parameter values used within the function shall be defined before the use of
    //   the invoking constant function call.
    // - All identifiers that are not parameters or functions shall be declared locally to
    //   the current function.
    if (symbol.kind != SymbolKind::Parameter && symbol.kind != SymbolKind::EnumValue) {
        const Scope* scope = symbol.getParentScope();
        while (scope && scope != subroutine)
            scope = scope->asSymbol().getParentScope();

        if (scope != subroutine) {
            auto& diag =
                context.addDiag(diag::ConstEvalFunctionIdentifiersMustBeLocal, sourceRange);
            diag.addNote(diag::NoteDeclarationHere, symbol.location);
            return false;
        }
    }
    else {
        // Check whether the referenced parameter is declared prior to the invocation
        // of the constant function. If the two locations are not in the same compilation
        // unit, assume that it's ok. Also if the reference is via a package import
        // that's fine too.
        auto compare = symbol.isDeclaredBefore(frame.lookupLocation);
        if (!compare.value_or(true)) {
            auto scope = symbol.getParentScope();
            if (!scope || scope->asSymbol().kind != SymbolKind::Package ||
                scope == frame.lookupLocation.getScope()) {

                auto& diag = context.addDiag(diag::ConstEvalIdUsedInCEBeforeDecl, sourceRange)
                             << symbol.name;
                diag.addNote(diag::NoteDeclarationHere, symbol.location);
                return false;
            }
        }
    }

    return true;
}

ConstantValue HierarchicalValueExpression::evalImpl(EvalContext& context) const {
    if (!context.compilation.getOptions().allowHierarchicalConst) {
        context.addDiag(diag::ConstEvalHierarchicalName, sourceRange) << symbol.name;
        return nullptr;
    }

    if (!checkConstantBase(context))
        return nullptr;

    switch (symbol.kind) {
        case SymbolKind::Parameter:
        case SymbolKind::EnumValue:
        case SymbolKind::Specparam:
            break;
        default:
            context.addDiag(diag::ConstEvalHierarchicalName, sourceRange) << symbol.name;
            return nullptr;
    }

    switch (symbol.kind) {
        case SymbolKind::Parameter: {
            auto v = symbol.as<ParameterSymbol>().getValue(sourceRange);
            if (v.isUnbounded()) {
                if (auto target = context.getQueueTarget()) {
                    int32_t size = (int32_t)target->queue()->size();
                    return SVInt(32, uint64_t(size - 1), true);
                }
            }
            return v;
        }
        case SymbolKind::EnumValue:
            return symbol.as<EnumValueSymbol>().getValue(sourceRange);
        case SymbolKind::Specparam:
            return symbol.as<SpecparamSymbol>().getValue(sourceRange);
        default:
            THROW_UNREACHABLE;
    }
}

Expression& DataTypeExpression::fromSyntax(Compilation& compilation, const DataTypeSyntax& syntax,
                                           const BindContext& context) {
    const Type& type = compilation.getType(syntax, context);
    if (syntax.kind == SyntaxKind::TypeReference &&
        context.flags.has(BindFlags::AllowTypeReferences)) {
        return *compilation.emplace<TypeReferenceExpression>(compilation.getTypeRefType(), type,
                                                             syntax.sourceRange());
    }

    if (!context.flags.has(BindFlags::AllowDataType)) {
        context.addDiag(diag::ExpectedExpression, syntax.sourceRange());
        return badExpr(compilation, nullptr);
    }

    return *compilation.emplace<DataTypeExpression>(type, syntax.sourceRange());
}

void TypeReferenceExpression::serializeTo(ASTSerializer& serializer) const {
    serializer.write("targetType", targetType);
}

Expression& HierarchicalReferenceExpression::fromSyntax(Compilation& compilation,
                                                        const NameSyntax& syntax,
                                                        const BindContext& context,
                                                        bitmask<LookupFlags> extraLookupFlags) {
    LookupResult result;
    Lookup::name(syntax, context,
                 LookupFlags::ForceHierarchical | LookupFlags::NoSelectors | extraLookupFlags,
                 result);
    result.reportDiags(context);

    const Symbol* symbol = result.found;
    if (!symbol)
        return badExpr(compilation, nullptr);

    return *compilation.emplace<HierarchicalReferenceExpression>(*symbol, compilation.getVoidType(),
                                                                 syntax.sourceRange());
}

void HierarchicalReferenceExpression::serializeTo(ASTSerializer& serializer) const {
    if (symbol)
        serializer.writeLink("symbol", *symbol);
}

ConstantValue LValueReferenceExpression::evalImpl(EvalContext& context) const {
    auto lvalue = context.getTopLValue();
    if (!lvalue)
        return nullptr;

    return lvalue->load();
}

Expression& ClockingEventExpression::fromSyntax(const ClockingPropertyExprSyntax& syntax,
                                                const BindContext& context) {
    auto& comp = context.getCompilation();
    auto& timing = TimingControl::bind(*syntax.event, context);

    if (syntax.expr)
        context.addDiag(diag::UnexpectedClockingExpr, syntax.expr->sourceRange());

    return *comp.emplace<ClockingEventExpression>(comp.getVoidType(), timing, syntax.sourceRange());
}

void ClockingEventExpression::serializeTo(ASTSerializer& serializer) const {
    serializer.write("timingControl", timingControl);
}

static std::tuple<const SequenceExprSyntax*, const ExpressionSyntax*> decomposePropExpr(
    const PropertyExprSyntax& propExpr) {

    const SequenceExprSyntax* seqExpr = nullptr;
    const ExpressionSyntax* regExpr = nullptr;
    if (propExpr.kind == SyntaxKind::SimplePropertyExpr) {
        seqExpr = propExpr.as<SimplePropertyExprSyntax>().expr;
        if (seqExpr->kind == SyntaxKind::SimpleSequenceExpr) {
            auto& simpSeq = seqExpr->as<SimpleSequenceExprSyntax>();
            if (!simpSeq.repetition)
                regExpr = simpSeq.expr;
        }
    }

    return { seqExpr, regExpr };
}

static bool checkAssertionArg(const PropertyExprSyntax& propExpr, const AssertionPortSymbol& formal,
                              const BindContext& context,
                              AssertionInstanceExpression::ActualArg& result,
                              bool isRecursiveProp) {
    auto [seqExpr, regExpr] = decomposePropExpr(propExpr);

    BindContext ctx = context;
    if (isRecursiveProp && !formal.isLocalVar()) {
        // For every recursive instance of property q in the declaration of property p,
        // each actual argument expression e of the instance must satisfy at least one
        // of the following conditions:
        // 1. e is itself a formal argument of p.
        // 2. No formal argument of p appears in e.
        // 3. e is bound to a local variable formal argument of q.
        if (!regExpr)
            ctx.flags |= BindFlags::RecursivePropertyArg;
        else {
            auto expr = regExpr;
            while (expr->kind == SyntaxKind::ParenthesizedExpression)
                expr = expr->as<ParenthesizedExpressionSyntax>().expression;

            // This check filters out cases where the entire argument is a formal argument.
            if (expr->kind != SyntaxKind::IdentifierName)
                ctx.flags |= BindFlags::RecursivePropertyArg;
        }
    }

    auto& type = formal.declaredType.getType();
    switch (type.getCanonicalType().kind) {
        case SymbolKind::UntypedType:
            // Untyped formals allow everything. Bind here just so we notice things like
            // name resolution errors even if the argument ends up being unused in the
            // body of the sequence / property.
            if (regExpr) {
                auto& bound = Expression::bind(*regExpr, ctx, BindFlags::AllowUnboundedLiteral);
                result = &bound;
                return !bound.bad();
            }
            else {
                ctx.flags |= BindFlags::AssertionInstanceArgCheck;
                auto& bound = AssertionExpr::bind(propExpr, ctx);
                result = &bound;
                return !bound.bad();
            }
        case SymbolKind::SequenceType: {
            if (!seqExpr) {
                ctx.addDiag(diag::AssertionArgTypeSequence, propExpr.sourceRange());
                return false;
            }

            auto& bound = AssertionExpr::bind(*seqExpr, ctx);
            if (bound.bad())
                return false;

            bound.requireSequence(ctx);
            result = &bound;
            return true;
        }
        case SymbolKind::PropertyType: {
            auto& bound = AssertionExpr::bind(propExpr, ctx);
            result = &bound;
            return !bound.bad();
        }
        case SymbolKind::EventType: {
            auto& bound = TimingControl::bind(propExpr, ctx);
            result = &bound;
            return !bound.bad();
        }
        case SymbolKind::ErrorType:
            return false;
        default:
            break;
    }

    // For all other types, we need a normal expression that
    // is cast compatible with the target type.
    if (!regExpr) {
        ctx.addDiag(diag::AssertionArgNeedsRegExpr, propExpr.sourceRange()) << type;
        return false;
    }

    auto& bound = Expression::bind(*regExpr, ctx);
    if (bound.bad())
        return false;

    if (!type.isCastCompatible(*bound.type)) {
        ctx.addDiag(diag::AssertionArgTypeMismatch, propExpr.sourceRange()) << *bound.type << type;
        return false;
    }

    // Local var formals that are output or inout must bind only to another local var.
    if (formal.localVarDirection == ArgumentDirection::InOut ||
        formal.localVarDirection == ArgumentDirection::Out) {
        auto sym = bound.getSymbolReference();
        if (!sym || sym->kind != SymbolKind::LocalAssertionVar) {
            ctx.addDiag(diag::AssertionOutputLocalVar, bound.sourceRange);
            return false;
        }

        sym->as<ValueSymbol>().addDriver(DriverKind::Procedural, bound, nullptr,
                                         AssignFlags::AssertionLocalVarFormalArg);
    }

    result = &bound;
    return true;
}

static const AssertionExpr& bindAssertionBody(const Symbol& symbol, const SyntaxNode& syntax,
                                              const BindContext& context,
                                              SourceLocation outputLocalVarArgLoc,
                                              BindContext::AssertionInstanceDetails& instance,
                                              SmallVector<const Symbol*>& localVars) {
    auto createLocals = [&](auto& syntaxType) {
        for (auto varSyntax : syntaxType.variables) {
            SmallVectorSized<const LocalAssertionVarSymbol*, 4> vars;
            LocalAssertionVarSymbol::fromSyntax(*context.scope, *varSyntax, vars);
            for (auto var : vars) {
                var->getDeclaredType()->forceResolveAt(context);
                localVars.append(var);
                if (!var->name.empty()) {
                    // TODO: check duplicates
                    instance.localVars.emplace(var->name, var);
                }
            }
        }
    };

    if (symbol.kind == SymbolKind::Sequence) {
        auto& sds = syntax.as<SequenceDeclarationSyntax>();
        createLocals(sds);

        auto& result = AssertionExpr::bind(*sds.seqExpr, context);
        result.requireSequence(context);

        if (outputLocalVarArgLoc && result.admitsEmpty()) {
            auto& diag =
                context.addDiag(diag::LocalVarOutputEmptyMatch, sds.seqExpr->sourceRange());
            diag << symbol.name;
            diag.addNote(diag::NoteDeclarationHere, outputLocalVarArgLoc);
        }

        return result;
    }
    else {
        auto& pds = syntax.as<PropertyDeclarationSyntax>();
        createLocals(pds);
        return AssertionExpr::bind(*pds.propertySpec, context);
    }
}

Expression& AssertionInstanceExpression::fromLookup(const Symbol& symbol,
                                                    const InvocationExpressionSyntax* syntax,
                                                    SourceRange range, const BindContext& context) {
    auto& comp = context.getCompilation();
    const Type* type;
    const Scope* symbolScope;
    span<const AssertionPortSymbol* const> formalPorts;

    switch (symbol.kind) {
        case SymbolKind::Sequence: {
            auto& seq = symbol.as<SequenceSymbol>();
            type = &comp.getType(SyntaxKind::SequenceType);
            formalPorts = seq.ports;
            symbolScope = &seq;
            break;
        }
        case SymbolKind::Property: {
            auto& prop = symbol.as<PropertySymbol>();
            type = &comp.getType(SyntaxKind::PropertyType);
            formalPorts = prop.ports;
            symbolScope = &prop;
            break;
        }
        case SymbolKind::LetDecl: {
            auto& let = symbol.as<LetDeclSymbol>();
            type = &comp.getVoidType();
            formalPorts = let.ports;
            symbolScope = &let;
            break;
        }
        default:
            THROW_UNREACHABLE;
    }

    SmallVectorSized<const SyntaxNode*, 8> orderedArgs;
    NamedArgMap namedArgs;
    if (syntax && syntax->arguments) {
        if (!collectArgs(context, *syntax->arguments, orderedArgs, namedArgs))
            return badExpr(comp, nullptr);
    }

    BindContext::AssertionInstanceDetails instance;
    instance.symbol = &symbol;
    instance.prevContext = &context;
    instance.instanceLoc = range.start();

    // Check for recursive instantiation. This is illegal for sequences, and allowed in
    // some forms for properties.
    auto currInst = context.assertionInstance;
    while (currInst) {
        if (currInst->symbol == &symbol) {
            if (symbol.kind == SymbolKind::Sequence) {
                context.addDiag(diag::RecursiveSequence, range) << symbol.name;
                return badExpr(comp, nullptr);
            }
            else if (symbol.kind == SymbolKind::LetDecl) {
                context.addDiag(diag::RecursiveLet, range) << symbol.name;
                return badExpr(comp, nullptr);
            }

            // Properties are allowed to be recursive, but we should avoid trying
            // to expand them because that will continue forever. Instead, we want
            // to expand one time for each unique invocation of the property and when
            // we encounter it again we should mark a placeholder and return to stop
            // the recursion.
            if (currInst->isRecursive) {
                auto& body = *comp.emplace<InvalidAssertionExpr>(nullptr);
                return *comp.emplace<AssertionInstanceExpression>(
                    *type, symbol, body, /* isRecursiveProperty */ true, range);
            }
            instance.isRecursive = true;
        }

        if (currInst->argDetails)
            currInst = currInst->argDetails;
        else {
            ASSERT(currInst->prevContext);
            currInst = currInst->prevContext->assertionInstance;
        }
    }

    // Now map all arguments to their formal ports.
    bool bad = false;
    uint32_t orderedIndex = 0;
    SourceLocation outputLocalVarArgLoc;
    SmallVectorSized<std::tuple<const Symbol*, ActualArg>, 8> actualArgs;

    for (auto formal : formalPorts) {
        const BindContext* argCtx = &context;
        const PropertyExprSyntax* expr = nullptr;
        optional<BindContext> defValCtx;

        auto setDefault = [&] {
            expr = formal->defaultValueSyntax;
            defValCtx.emplace(*symbolScope, LookupLocation::after(*formal));
            defValCtx->assertionInstance = &instance;
            argCtx = &defValCtx.value();
        };

        if (orderedIndex < orderedArgs.size()) {
            auto arg = orderedArgs[orderedIndex++];
            if (arg->kind == SyntaxKind::EmptyArgument) {
                // Empty arguments are allowed as long as a default is provided.
                setDefault();
                if (!expr)
                    context.addDiag(diag::ArgCannotBeEmpty, arg->sourceRange()) << formal->name;
            }
            else {
                expr = &arg->as<PropertyExprSyntax>();
            }

            // Make sure there isn't also a named value for this argument.
            if (auto it = namedArgs.find(formal->name); it != namedArgs.end()) {
                auto& diag = context.addDiag(diag::DuplicateArgAssignment,
                                             it->second.first->name.location());
                diag << formal->name;
                diag.addNote(diag::NotePreviousUsage, arg->getFirstToken().location());
                it->second.second = true;
                bad = true;
            }
        }
        else if (auto it = namedArgs.find(formal->name); it != namedArgs.end()) {
            // Mark this argument as used so that we can later detect if
            // any were unused.
            it->second.second = true;

            auto arg = it->second.first->expr;
            if (!arg) {
                // Empty arguments are allowed as long as a default is provided.
                setDefault();
                if (!expr) {
                    context.addDiag(diag::ArgCannotBeEmpty, it->second.first->sourceRange())
                        << formal->name;
                }
            }
        }
        else {
            setDefault();
            if (!expr) {
                if (namedArgs.empty()) {
                    auto& diag = context.addDiag(diag::TooFewArguments, range);
                    diag << symbol.name;
                    diag << formalPorts.size() << orderedArgs.size();
                    bad = true;
                    break;
                }
                else {
                    context.addDiag(diag::UnconnectedArg, range) << formal->name;
                }
            }
        }

        if (!expr) {
            bad = true;
            continue;
        }

        // Map the expression to the port symbol; this will be looked up later
        // when we encounter uses in the sequence / property body.
        instance.argumentMap.emplace(formal, std::make_tuple(expr, *argCtx));

        // Do type checking for all arguments now, even though the actuals will remain as
        // syntax nodes and be rebound when we actually encounter uses of them in the body.
        // This is because the arguments might not actually be used anywhere in the body,
        // so the only place to detect mismatches is here, but we can't save the bound
        // form because assertion item arguments are replaced as-is for each usage.
        ActualArg arg;
        if (!checkAssertionArg(*expr, *formal, *argCtx, arg, instance.isRecursive))
            bad = true;
        else
            actualArgs.append({ formal, arg });

        if (!outputLocalVarArgLoc && (formal->localVarDirection == ArgumentDirection::InOut ||
                                      formal->localVarDirection == ArgumentDirection::Out)) {
            outputLocalVarArgLoc = formal->location;
        }
    }

    // Make sure there weren't too many ordered arguments provided.
    if (orderedIndex < orderedArgs.size()) {
        auto& diag = context.addDiag(diag::TooManyArguments, range);
        diag << symbol.name;
        diag << formalPorts.size();
        diag << orderedArgs.size();
        bad = true;
    }

    for (auto& pair : namedArgs) {
        // We marked all the args that we used, so anything left over is an arg assignment
        // for a non-existent arg.
        if (!pair.second.second) {
            auto& diag = context.addDiag(diag::ArgDoesNotExist, pair.second.first->name.location());
            diag << pair.second.first->name.valueText();
            diag << symbol.name;
            bad = true;
        }
    }

    BindContext bodyContext(*symbolScope, LookupLocation::max);
    bodyContext.assertionInstance = &instance;

    // Let declarations expand directly to an expression.
    if (symbol.kind == SymbolKind::LetDecl)
        return create(comp, *symbol.as<LetDeclSymbol>().exprSyntax, bodyContext);

    // Now instantiate by binding the assertion expression of the sequence / property body.
    auto bodySyntax = symbol.getSyntax();
    ASSERT(bodySyntax);

    SmallVectorSized<const Symbol*, 8> localVars;
    auto& body = bindAssertionBody(symbol, *bodySyntax, bodyContext, outputLocalVarArgLoc, instance,
                                   localVars);

    auto result = comp.emplace<AssertionInstanceExpression>(*type, symbol, body,
                                                            /* isRecursiveProperty */ false, range);
    result->arguments = actualArgs.copy(comp);
    result->localVars = localVars.copy(comp);

    if (instance.isRecursive) {
        if (!context.flags.has(BindFlags::PropertyTimeAdvance))
            context.addDiag(diag::RecursivePropTimeAdvance, range);
        else if (context.flags.has(BindFlags::PropertyNegation))
            context.addDiag(diag::RecursivePropNegation, range);
    }

    if (bad || body.bad())
        return badExpr(comp, result);

    return *result;
}

Expression& AssertionInstanceExpression::makeDefault(const Symbol& symbol) {
    auto parentScope = symbol.getParentScope();
    ASSERT(parentScope);

    BindContext context(*parentScope, LookupLocation::before(symbol));
    auto& comp = context.getCompilation();
    const Type* type;
    const Scope* symbolScope;
    span<const AssertionPortSymbol* const> formalPorts;

    switch (symbol.kind) {
        case SymbolKind::Sequence: {
            auto& seq = symbol.as<SequenceSymbol>();
            type = &comp.getType(SyntaxKind::SequenceType);
            formalPorts = seq.ports;
            symbolScope = &seq;
            break;
        }
        case SymbolKind::Property: {
            auto& prop = symbol.as<PropertySymbol>();
            type = &comp.getType(SyntaxKind::PropertyType);
            formalPorts = prop.ports;
            symbolScope = &prop;
            break;
        }
        case SymbolKind::LetDecl: {
            auto& let = symbol.as<LetDeclSymbol>();
            type = &comp.getVoidType();
            formalPorts = let.ports;
            symbolScope = &let;
            break;
        }
        default:
            THROW_UNREACHABLE;
    }

    BindContext::AssertionInstanceDetails instance;
    instance.symbol = &symbol;
    instance.prevContext = &context;
    instance.instanceLoc = symbol.location;

    // Bind default args, make placeholder entries for args that don't have defaults.
    SourceLocation outputLocalVarArgLoc;
    for (auto formal : formalPorts) {
        if (!formal->defaultValueSyntax) {
            instance.argumentMap.emplace(formal,
                                         std::make_tuple((PropertyExprSyntax*)nullptr, context));
        }
        else {
            BindContext ctx(*symbolScope, LookupLocation::after(*formal));
            ctx.assertionInstance = &instance;

            auto expr = formal->defaultValueSyntax;
            instance.argumentMap.emplace(formal, std::make_tuple(expr, ctx));

            ActualArg arg;
            checkAssertionArg(*expr, *formal, ctx, arg, false);
        }

        if (!outputLocalVarArgLoc && (formal->localVarDirection == ArgumentDirection::InOut ||
                                      formal->localVarDirection == ArgumentDirection::Out)) {
            outputLocalVarArgLoc = formal->location;
        }
    }

    BindContext bodyContext(*symbolScope, LookupLocation::max);
    bodyContext.assertionInstance = &instance;

    // Let declarations expand directly to an expression.
    if (symbol.kind == SymbolKind::LetDecl)
        return create(comp, *symbol.as<LetDeclSymbol>().exprSyntax, bodyContext);

    auto bodySyntax = symbol.getSyntax();
    ASSERT(bodySyntax);

    SmallVectorSized<const Symbol*, 8> localVars;
    auto& body = bindAssertionBody(symbol, *bodySyntax, bodyContext, outputLocalVarArgLoc, instance,
                                   localVars);

    SourceRange range{ symbol.location, symbol.location + 1 };
    auto result = comp.emplace<AssertionInstanceExpression>(*type, symbol, body,
                                                            /* isRecursiveProperty */ false, range);
    result->localVars = localVars.copy(comp);
    return *result;
}

Expression& AssertionInstanceExpression::bindPort(const Symbol& symbol, SourceRange range,
                                                  const BindContext& instanceCtx) {
    Compilation& comp = instanceCtx.getCompilation();
    auto inst = instanceCtx.assertionInstance;
    ASSERT(inst);

    // When looking up an argument reference from within another expanded
    // argument, use that original location's context.
    if (inst->argDetails)
        inst = inst->argDetails;

    // The only way to reference an assertion port should be from within
    // an assertion instance, so we should always find it here.
    auto it = inst->argumentMap.find(&symbol);
    if (it == inst->argumentMap.end())
        return badExpr(comp, nullptr);

    auto& formal = symbol.as<AssertionPortSymbol>();
    auto& type = formal.declaredType.getType();
    auto typeKind = type.getCanonicalType().kind;

    if (typeKind != SymbolKind::ErrorType && typeKind != SymbolKind::UntypedType) {
        if (instanceCtx.flags.has(BindFlags::AssertionDelayOrRepetition)) {
            auto isAllowedIntType = [&] {
                if (typeKind != SymbolKind::PredefinedIntegerType)
                    return false;

                auto ik = type.getCanonicalType().as<PredefinedIntegerType>().integerKind;
                return ik == PredefinedIntegerType::Int || ik == PredefinedIntegerType::ShortInt ||
                       ik == PredefinedIntegerType::LongInt;
            };

            if (!isAllowedIntType()) {
                auto& diag = instanceCtx.addDiag(diag::AssertionDelayFormalType, range);
                diag << type;
                diag.addNote(diag::NoteDeclarationHere, formal.location);
                return badExpr(comp, nullptr);
            }
        }

        if (instanceCtx.flags.has(BindFlags::LValue) && !formal.localVarDirection) {
            instanceCtx.addDiag(diag::AssertionPortTypedLValue, range) << formal.name;
            return badExpr(comp, nullptr);
        }
    }

    if (instanceCtx.flags.has(BindFlags::RecursivePropertyArg)) {
        instanceCtx.addDiag(diag::RecursivePropArgExpr, range) << formal.name;
        return badExpr(comp, nullptr);
    }

    auto [propExpr, argCtx] = it->second;
    if (!propExpr) {
        // The expression can be null when making default instances of
        // sequences and properties. Just return an invalid expression.
        return badExpr(comp, nullptr);
    }

    auto [seqExpr, regExpr] = decomposePropExpr(*propExpr);

    // Inherit any binding flags that are specific to this argument's instantiation.
    argCtx.flags = instanceCtx.flags;

    BindContext::AssertionInstanceDetails details;
    details.argExpansionLoc = range.start();
    details.prevContext = &instanceCtx;
    details.argDetails = argCtx.assertionInstance;
    argCtx.assertionInstance = &details;

    switch (typeKind) {
        case SymbolKind::UntypedType:
            // Untyped formals allow everything. Bind as a regular expression
            // if possible and fall back to an assertion expression if not.
            if (regExpr) {
                auto& result = selfDetermined(comp, *regExpr, argCtx, argCtx.flags);
                result.sourceRange = range;
                return result;
            }
            else if (instanceCtx.flags.has(BindFlags::EventExpression) &&
                     instanceCtx.flags.has(BindFlags::AllowClockingBlock)) {
                // In an event expression, a referenced argument gets interpreted
                // as an event expression itself and not as an assertion expression.
                auto& timing = TimingControl::bind(*propExpr, argCtx);
                return *comp.emplace<ClockingEventExpression>(comp.getVoidType(), timing, range);
            }
            else {
                auto& result = AssertionExpr::bind(*propExpr, argCtx);
                auto& resultType = seqExpr ? comp.getType(SyntaxKind::SequenceType)
                                           : comp.getType(SyntaxKind::PropertyType);
                return *comp.emplace<AssertionInstanceExpression>(
                    resultType, formal, result, /* isRecursiveProperty */ false, range);
            }
        case SymbolKind::SequenceType:
        case SymbolKind::PropertyType: {
            auto& result = AssertionExpr::bind(*propExpr, argCtx);
            auto& resultType = typeKind == SymbolKind::SequenceType
                                   ? comp.getType(SyntaxKind::SequenceType)
                                   : comp.getType(SyntaxKind::PropertyType);

            return *comp.emplace<AssertionInstanceExpression>(
                resultType, formal, result, /* isRecursiveProperty */ false, range);
        }
        case SymbolKind::EventType:
            // If an event expression is allowed here, bind and return. Otherwise issue
            // an error, since an 'event' argument can only be used where event expressions
            // are allowed, regardless of what the actual argument expression looks like.
            if (instanceCtx.flags.has(BindFlags::EventExpression) &&
                instanceCtx.flags.has(BindFlags::AllowClockingBlock)) {
                auto& timing = TimingControl::bind(*propExpr, argCtx);
                return *comp.emplace<ClockingEventExpression>(comp.getVoidType(), timing, range);
            }

            instanceCtx.addDiag(diag::EventExprAssertionArg, range);
            return badExpr(comp, nullptr);
        default: {
            // Arguments should have already been checked for type correctness.
            if (!regExpr)
                return badExpr(comp, nullptr);

            auto& expr = selfDetermined(comp, *regExpr, argCtx, argCtx.flags);
            expr.sourceRange = range;

            if (!expr.type->isMatching(type)) {
                return *comp.emplace<ConversionExpression>(type, ConversionKind::Explicit, expr,
                                                           range);
            }

            return expr;
        }
    }
}

void AssertionInstanceExpression::serializeTo(ASTSerializer& serializer) const {
    serializer.writeLink("symbol", symbol);
    serializer.write("body", body);
    serializer.write("isRecursiveProperty", isRecursiveProperty);

    serializer.startArray("localVars");
    for (auto var : localVars)
        serializer.serialize(*var);
    serializer.endArray();
}

Expression& MinTypMaxExpression::fromSyntax(Compilation& compilation,
                                            const MinTypMaxExpressionSyntax& syntax,
                                            const BindContext& context,
                                            const Type* assignmentTarget) {
    // Only one of the expressions will be considered evaluated.
    auto minFlags = BindFlags::UnevaluatedBranch;
    auto typFlags = BindFlags::UnevaluatedBranch;
    auto maxFlags = BindFlags::UnevaluatedBranch;
    switch (compilation.getOptions().minTypMax) {
        case MinTypMax::Min:
            minFlags = BindFlags::None;
            break;
        case MinTypMax::Typ:
            typFlags = BindFlags::None;
            break;
        case MinTypMax::Max:
            maxFlags = BindFlags::None;
            break;
    }

    auto& min = create(compilation, *syntax.min, context, minFlags, assignmentTarget);
    auto& typ = create(compilation, *syntax.typ, context, typFlags, assignmentTarget);
    auto& max = create(compilation, *syntax.max, context, maxFlags, assignmentTarget);

    Expression* selected = nullptr;
    switch (compilation.getOptions().minTypMax) {
        case MinTypMax::Min:
            selected = &min;
            break;
        case MinTypMax::Typ:
            selected = &typ;
            break;
        case MinTypMax::Max:
            selected = &max;
            break;
    }

    auto result = compilation.emplace<MinTypMaxExpression>(*selected->type, min, typ, max, selected,
                                                           syntax.sourceRange());
    if (min.bad() || typ.bad() || max.bad())
        return badExpr(compilation, result);

    return *result;
}

bool MinTypMaxExpression::propagateType(const BindContext& context, const Type& newType) {
    // Only the selected expression gets a propagated type.
    type = &newType;
    contextDetermined(context, selected_, newType);
    return true;
}

ConstantValue MinTypMaxExpression::evalImpl(EvalContext& context) const {
    return selected().eval(context);
}

optional<bitwidth_t> MinTypMaxExpression::getEffectiveWidthImpl() const {
    return selected().getEffectiveWidth();
}

void MinTypMaxExpression::serializeTo(ASTSerializer& serializer) const {
    serializer.write("selected", selected());
}

Expression& CopyClassExpression::fromSyntax(Compilation& compilation,
                                            const CopyClassExpressionSyntax& syntax,
                                            const BindContext& context) {
    auto& source = selfDetermined(compilation, *syntax.expr, context);
    auto result =
        compilation.emplace<CopyClassExpression>(*source.type, source, syntax.sourceRange());
    if (source.bad())
        return badExpr(compilation, result);

    if (!source.type->isClass()) {
        context.addDiag(diag::CopyClassTarget, source.sourceRange) << *source.type;
        return badExpr(compilation, result);
    }

    return *result;
}

ConstantValue CopyClassExpression::evalImpl(EvalContext& context) const {
    context.addDiag(diag::ConstEvalClassType, sourceRange);
    return nullptr;
}

void CopyClassExpression::serializeTo(ASTSerializer& serializer) const {
    serializer.write("sourceExpr", sourceExpr());
}

Expression& DistExpression::fromSyntax(Compilation& comp, const ExpressionOrDistSyntax& syntax,
                                       const BindContext& context) {
    SmallVectorSized<const ExpressionSyntax*, 8> expressions;
    for (auto item : syntax.distribution->items)
        expressions.append(item->range);

    SmallVectorSized<const Expression*, 8> bound;
    bool bad =
        !bindMembershipExpressions(context, TokenKind::DistKeyword, /* requireIntegral */ true,
                                   /* unwrapUnpacked */ false, /* allowTypeReferences */ false,
                                   /* allowOpenRange */ true, *syntax.expr, expressions, bound);

    SmallVectorSized<DistItem, 4> items;
    size_t index = 1;
    for (auto item : syntax.distribution->items) {
        DistItem di{ *bound[index++], {} };
        if (item->weight) {
            auto weightKind = item->weight->op.kind == TokenKind::ColonSlash ? DistWeight::PerRange
                                                                             : DistWeight::PerValue;
            auto& weightExpr = Expression::bind(*item->weight->expr, context);
            di.weight.emplace(DistWeight{ weightKind, weightExpr });

            if (!context.requireIntegral(weightExpr))
                bad = true;
        }

        items.emplace(di);
    }

    auto result = comp.emplace<DistExpression>(comp.getVoidType(), *bound[0], items.copy(comp),
                                               syntax.sourceRange());
    if (bad)
        return badExpr(comp, result);

    return *result;
}

void DistExpression::serializeTo(ASTSerializer& serializer) const {
    serializer.write("left", left());
    serializer.startArray("items");
    for (auto& item : items_) {
        serializer.startObject();
        serializer.write("value", item.value);
        if (item.weight) {
            serializer.write("kind", item.weight->kind == DistWeight::PerRange ? "PerRange"sv
                                                                               : "PerValue"sv);
            serializer.write("weight", item.weight->expr);
        }
        serializer.endObject();
    }
    serializer.endArray();
}

Expression& TaggedUnionExpression::fromSyntax(Compilation& compilation,
                                              const TaggedUnionExpressionSyntax& syntax,
                                              const BindContext& context,
                                              const Type* assignmentTarget) {
    if (!assignmentTarget || !assignmentTarget->isTaggedUnion()) {
        if (!assignmentTarget || !assignmentTarget->isError())
            context.addDiag(diag::TaggedUnionTarget, syntax.sourceRange());
        return badExpr(compilation, nullptr);
    }

    auto memberName = syntax.member.valueText();
    auto member = assignmentTarget->getCanonicalType().as<Scope>().find(memberName);
    if (!member) {
        if (!memberName.empty()) {
            auto& diag = context.addDiag(diag::UnknownMember, syntax.member.range());
            diag << memberName << *assignmentTarget;
        }
        return badExpr(compilation, nullptr);
    }

    auto& field = member->as<FieldSymbol>();

    const Expression* valueExpr = nullptr;
    if (syntax.expr) {
        valueExpr = &bindRValue(field.getType(), *syntax.expr,
                                syntax.expr->getFirstToken().location(), context);
    }
    else if (!field.getType().isVoid()) {
        context.addDiag(diag::TaggedUnionMissingInit, syntax.sourceRange()) << field.name;
        return badExpr(compilation, nullptr);
    }

    auto result = compilation.emplace<TaggedUnionExpression>(*assignmentTarget, *member, valueExpr,
                                                             syntax.sourceRange());
    if (valueExpr && valueExpr->bad())
        return badExpr(compilation, result);

    return *result;
}

ConstantValue TaggedUnionExpression::evalImpl(EvalContext& context) const {
    ConstantValue initVal;
    if (valueExpr) {
        initVal = valueExpr->eval(context);
        if (!initVal)
            return nullptr;
    }

    auto& field = member.as<FieldSymbol>();

    auto& ct = type->getCanonicalType();
    if (ct.isUnpackedUnion()) {
        SVUnion u;
        u.activeMember = field.offset;
        u.value = std::move(initVal);
        return u;
    }
    else {
        uint32_t tagBits = ct.as<PackedUnionType>().tagBits;
        if (tagBits == 0)
            return nullptr;

        ConstantValue result = type->getDefaultValue();
        auto& resultInt = result.integer();

        // The tag lives in the upper bits and the value is in the lower bits.
        // Any bits in between are undefined.
        bitwidth_t bits = resultInt.getBitWidth();
        resultInt.set(int32_t(bits - 1), int32_t(bits - tagBits),
                      SVInt(tagBits, field.offset, false));

        if (initVal) {
            auto& valInt = initVal.integer();
            resultInt.set(int32_t(valInt.getBitWidth() - 1), 0, valInt);
        }

        return result;
    }
}

void TaggedUnionExpression::serializeTo(ASTSerializer& serializer) const {
    serializer.writeLink("member", member);
    if (valueExpr)
        serializer.write("valueExpr", *valueExpr);
}

} // namespace slang
