#include "Test.h"
#include <fmt/format.h>

#include "slang/compilation/SemanticModel.h"
#include "slang/symbols/ASTVisitor.h"
#include "slang/syntax/SyntaxPrinter.h"
#include "slang/syntax/SyntaxVisitor.h"

class TestRewriter : public SyntaxRewriter<TestRewriter> {
public:
    Compilation compilation;
    SemanticModel model;

    TestRewriter(const std::shared_ptr<SyntaxTree>& tree) : model(compilation) {
        compilation.addSyntaxTree(tree);
    }

    void handle(const TypedefDeclarationSyntax& decl) {
        if (decl.type->kind != SyntaxKind::EnumType)
            return;

        // Create a new localparam hardcoded with the number of entries in the enum.
        auto type = model.getDeclaredSymbol(decl.type->as<EnumTypeSyntax>());
        REQUIRE(type);

        size_t count = type->as<EnumType>().members().size();
        auto& newNode = parse(
            fmt::format("\n    localparam int {}__count = {};", decl.name.valueText(), count));
        insertAfter(decl, newNode);
    }

    void handle(const FunctionDeclarationSyntax& decl) {
        auto portList = decl.prototype->portList;
        if (!portList)
            return;

        auto& argA = factory.functionPort(nullptr, {}, {}, {}, nullptr,
                                          factory.declarator(makeId("argA"), nullptr, nullptr));
        insertAtFront(portList->ports, argA, makeComma());

        auto& argZ = factory.functionPort(nullptr, {}, {}, {}, nullptr,
                                          factory.declarator(makeId("argZ"), nullptr, nullptr));
        insertAtBack(portList->ports, argZ, makeComma());
    }
};

TEST_CASE("Basic rewriting") {
    auto tree = SyntaxTree::fromText(R"(
module M;
    typedef enum int { FOO = 1, BAR = 2, BAZ = 3 } test_t;

    function void foo(int i, output r);
    endfunction
endmodule
)");

    tree = TestRewriter(tree).transform(tree);

    CHECK(SyntaxPrinter::printFile(*tree) == R"(
module M;
    typedef enum int { FOO = 1, BAR = 2, BAZ = 3 } test_t;
    localparam int test_t__count = 3;
    function void foo(argA,int i, output r,argZ);
    endfunction
endmodule
)");
}

TEST_CASE("Rewriting around macros") {
    auto tree = SyntaxTree::fromText(R"(
`define ENUM_MACRO(asdf) \
    typedef enum int {\
        FOO = 1,\
        BAR = 2,\
        BAZ = 3\
    } asdf;

module M;
    `ENUM_MACRO(test_t)
endmodule
)");

    tree = TestRewriter(tree).transform(tree);

    CHECK(SyntaxPrinter::printFile(*tree) == R"(
`define ENUM_MACRO(asdf) \
    typedef enum int {\
        FOO = 1,\
        BAR = 2,\
        BAZ = 3\
    } asdf;
module M;
    `ENUM_MACRO(test_t)
    localparam int test_t__count = 3;
endmodule
)");
}

TEST_CASE("Test AST visiting") {
    auto tree = SyntaxTree::fromText(R"(
module m;
    initial begin
        if (1) begin
            int i = {1 + 2, 5 + 6};
        end
    end
    int j = 3 + 4;
endmodule
)");

    Compilation compilation;
    compilation.addSyntaxTree(tree);

    // Visit the whole tree and count the binary expressions.
    int count = 0;
    compilation.getRoot().visit(makeVisitor([&](const BinaryExpression&) { count++; }));
    CHECK(count == 3);
}

struct Visitor : public ASTVisitor<Visitor, true, true> {
    int count = 0;
    template<typename T>
    void handle(const T& t) {
        if constexpr (std::is_base_of_v<Statement, T>) {
            count++;
        }
        visitDefault(t);
    }
};

TEST_CASE("Test single counting of statements") {
    auto tree = SyntaxTree::fromText(R"(
module m;
    int j;
    initial begin : asdf
        j = j + 3;
        if (1) begin : baz
            static int i;
            i = i + 2;
            if (1) begin : boz
                i = i + 4;
            end
        end
    end
endmodule
)");

    Compilation compilation;
    compilation.addSyntaxTree(tree);

    // Visit the whole tree and count the statements.
    Visitor visitor;
    compilation.getRoot().visit(visitor);
    CHECK(visitor.count == 11);
}
