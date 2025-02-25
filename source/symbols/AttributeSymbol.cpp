//------------------------------------------------------------------------------
// AttributeSymbol.cpp
// Symbol definition for source attributes
//
// File is under the MIT license; see LICENSE for details
//------------------------------------------------------------------------------
#include "slang/symbols/AttributeSymbol.h"

#include "slang/binding/Expression.h"
#include "slang/compilation/Compilation.h"
#include "slang/diagnostics/DeclarationsDiags.h"
#include "slang/symbols/ASTSerializer.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/util/StackContainer.h"

namespace slang {

AttributeSymbol::AttributeSymbol(string_view name, SourceLocation loc, const Symbol& symbol,
                                 const ExpressionSyntax& expr) :
    Symbol(SymbolKind::Attribute, name, loc),
    symbol(&symbol), expr(&expr) {
}

AttributeSymbol::AttributeSymbol(string_view name, SourceLocation loc, const Scope& scope,
                                 LookupLocation lookupLocation, const ExpressionSyntax& expr) :
    Symbol(SymbolKind::Attribute, name, loc),
    scope(&scope), expr(&expr), lookupLocation(lookupLocation) {
}

AttributeSymbol::AttributeSymbol(string_view name, SourceLocation loc, const ConstantValue& value) :
    Symbol(SymbolKind::Attribute, name, loc), value(&value) {
}

const ConstantValue& AttributeSymbol::getValue() const {
    if (!value) {
        LookupLocation loc = lookupLocation;
        auto bindScope = scope;
        if (symbol) {
            bindScope = symbol->getParentScope();
            loc = LookupLocation::before(*symbol);
        }

        ASSERT(bindScope);
        ASSERT(expr);

        BindContext context(*bindScope, loc, BindFlags::NoAttributes | BindFlags::NonProcedural);
        auto& bound = Expression::bind(*expr, context);

        value = bindScope->getCompilation().allocConstant(context.eval(bound));
    }

    return *value;
}

void AttributeSymbol::serializeTo(ASTSerializer& serializer) const {
    serializer.write("value", getValue());
}

template<typename TFunc>
static span<const AttributeSymbol* const> createAttributes(
    span<const AttributeInstanceSyntax* const> syntax, const Scope& scope, TFunc&& factory) {

    SmallMap<string_view, size_t, 4> nameMap;
    SmallVectorSized<const AttributeSymbol*, 8> attrs;

    auto& comp = scope.getCompilation();
    for (auto inst : syntax) {
        for (auto spec : inst->specs) {
            auto name = spec->name.valueText();
            if (name.empty())
                continue;

            AttributeSymbol* attr;
            if (!spec->value) {
                ConstantValue value = SVInt(1, 1, false);
                attr = comp.emplace<AttributeSymbol>(name, spec->name.location(),
                                                     *comp.allocConstant(std::move(value)));
            }
            else {
                attr = factory(comp, name, spec->name.location(), *spec->value->expr);
            }

            attr->setSyntax(*spec);

            if (auto it = nameMap.find(name); it != nameMap.end()) {
                scope.addDiag(diag::DuplicateAttribute, attr->location) << name;
                attrs[it->second] = attr;
            }
            else {
                attrs.append(attr);
                nameMap.emplace(name, attrs.size() - 1);
            }
        }
    }

    return attrs.copy(comp);
}

span<const AttributeSymbol* const> AttributeSymbol::fromSyntax(
    span<const AttributeInstanceSyntax* const> syntax, const Scope& scope, const Symbol& symbol) {

    if (syntax.empty())
        return {};

    return createAttributes(
        syntax, scope, [&symbol](auto& comp, auto name, auto loc, auto& exprSyntax) {
            return comp.template emplace<AttributeSymbol>(name, loc, symbol, exprSyntax);
        });
}

span<const AttributeSymbol* const> AttributeSymbol::fromSyntax(
    span<const AttributeInstanceSyntax* const> syntax, const Scope& scope,
    LookupLocation lookupLocation) {

    if (syntax.empty())
        return {};

    return createAttributes(
        syntax, scope,
        [&scope, &lookupLocation](auto& comp, auto name, auto loc, auto& exprSyntax) {
            return comp.template emplace<AttributeSymbol>(name, loc, scope, lookupLocation,
                                                          exprSyntax);
        });
}

} // namespace slang
