#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace clang;

namespace {

struct Options {
    std::string Surface = "levilamina";
    std::string Output;
    std::string Root;
    std::string Configuration = "win-x64-server";
    std::string Header = "ll/api/event/Event.h";
    std::string Source;
    std::vector<std::string> Filters;
    std::string Module = "ll/api/event";
    std::vector<std::string> CompilerArgs;
};

std::string normalizePath(std::string Path) {
    std::replace(Path.begin(), Path.end(), '\\', '/');
    while (Path.size() > 1 && Path.back() == '/') {
        Path.pop_back();
    }
    return Path;
}

void splitConfiguration(const std::string& Id, std::string& Os, std::string& Arch, std::string& Target) {
    size_t First = Id.find('-');
    size_t Second = First == std::string::npos ? std::string::npos : Id.find('-', First + 1);
    Os = First == std::string::npos ? Id : Id.substr(0, First);
    Arch = First == std::string::npos
        ? "x64"
        : Id.substr(First + 1, Second == std::string::npos ? std::string::npos : Second - First - 1);
    Target = Second == std::string::npos ? "server" : Id.substr(Second + 1);
    if (Os == "win") {
        Os = "windows";
    }
    if (Arch.empty()) {
        Arch = "x64";
    }
    if (Target.empty()) {
        Target = "server";
    }
}

bool isTypeWordChar(char C) {
    return std::isalnum(static_cast<unsigned char>(C)) != 0 || C == '_';
}

// Targets whose `long` is 64-bit spell the same fixed-width types differently from
// Windows. Rewrite those spellings to the Windows form so declaration ids, table
// field hashes, and coverage stay identical across platforms.
std::string normalize64BitLongTypes(llvm::StringRef Text) {
    std::string Out;
    size_t I = 0;
    auto peekWord = [&](size_t& Position) {
        while (Position < Text.size() && Text[Position] == ' ') {
            ++Position;
        }
        size_t Start = Position;
        while (Position < Text.size() && isTypeWordChar(Text[Position])) {
            ++Position;
        }
        return Text.slice(Start, Position);
    };

    while (I < Text.size()) {
        size_t Start = I;
        while (I < Text.size() && isTypeWordChar(Text[I])) {
            ++I;
        }
        if (Start == I) {
            Out.push_back(Text[I]);
            ++I;
            continue;
        }

        llvm::StringRef Word = Text.slice(Start, I);
        if (Word == "unsigned") {
            size_t Next = I;
            if (peekWord(Next) == "long") {
                size_t After = Next;
                if (peekWord(After) == "long") {
                    Out += "unsigned long long";
                    I = After;
                } else {
                    Out += "unsigned long long";
                    I = Next;
                }
                continue;
            }
        }

        if (Word == "long") {
            size_t Next = I;
            llvm::StringRef NextWord = peekWord(Next);
            if (NextWord == "double") {
                Out += "long";
                continue;
            }

            Out += "long long";
            if (NextWord == "long") {
                I = Next;
            }
            continue;
        }

        Out += Word;
    }

    return Out;
}

std::pair<llvm::StringRef, llvm::StringRef> splitTopLevelArgument(llvm::StringRef Content) {
    size_t Nested = 0;
    for (size_t I = 0; I < Content.size(); ++I) {
        char C = Content[I];
        if (C == '<') {
            ++Nested;
        } else if (C == '>') {
            --Nested;
        } else if (C == ',' && Nested == 0) {
            return {Content.slice(0, I).trim(), Content.drop_front(I + 1).trim()};
        }
    }
    return {Content.trim(), llvm::StringRef()};
}

template <typename Builder>
std::string replaceTemplateInstances(llvm::StringRef Text, llvm::StringRef Prefix, Builder Build) {
    std::string Out;
    size_t I = 0;
    while (I < Text.size()) {
        size_t Pos = Text.find(Prefix, I);
        if (Pos == llvm::StringRef::npos) {
            Out += Text.substr(I);
            break;
        }
        Out += Text.substr(I, Pos - I);

        size_t ContentStart = Pos + Prefix.size();
        size_t Depth = 1;
        size_t J = ContentStart;
        for (; J < Text.size() && Depth > 0; ++J) {
            if (Text[J] == '<') {
                ++Depth;
            } else if (Text[J] == '>') {
                --Depth;
            }
        }
        if (Depth != 0) {
            Out += Text.substr(Pos);
            break;
        }

        auto Replacement = Build(Text.slice(ContentStart, J - 1));
        if (Replacement) {
            Out += *Replacement;
        } else {
            Out += Text.substr(Pos, J - Pos);
        }
        I = J;
    }
    return Out;
}

std::string wrapNestedIterator(llvm::StringRef Outer, llvm::StringRef Inner, llvm::StringRef Element) {
    std::string Out = Outer.str();
    Out += Inner.str();
    Out += Element.str();
    Out += ">>>";
    return Out;
}

// libstdc++ spells vector iterators as __gnu_cxx::__normal_iterator. Rewrite them to
// the MSVC spelling so declaration ids and stub messages match the Windows IR.
std::string normalizeGnuIteratorTypes(llvm::StringRef Text) {
    return replaceTemplateInstances(
        Text,
        "__gnu_cxx::__normal_iterator<",
        [](llvm::StringRef Content) -> std::optional<std::string> {
            auto [Pointer, Container] = splitTopLevelArgument(Content);
            constexpr llvm::StringLiteral VectorPrefix = "std::vector<";
            if (!Container.starts_with(VectorPrefix) || !Container.ends_with(">")) {
                return std::nullopt;
            }
            llvm::StringRef Element = Container.slice(VectorPrefix.size(), Container.size() - 1);
            bool IsConst = Pointer.starts_with("const ");
            return wrapNestedIterator(
                IsConst ? "std::_Vector_const_iterator<" : "std::_Vector_iterator<",
                "std::_Vector_val<std::_Simple_types<",
                Element);
        });
}

// Rewrite libstdc++ tree, list, and unordered-container iterators to the MSVC forms.
std::string normalizeGnuStdIteratorTypes(std::string Text) {
    Text = replaceTemplateInstances(
        Text,
        "std::_Rb_tree_const_iterator<",
        [](llvm::StringRef Content) -> std::optional<std::string> {
            return wrapNestedIterator(
                "std::_Tree_const_iterator<",
                "std::_Tree_val<std::_Tree_simple_types<",
                Content.trim());
        });
    Text = replaceTemplateInstances(
        Text,
        "std::_Rb_tree_iterator<",
        [](llvm::StringRef Content) -> std::optional<std::string> {
            return wrapNestedIterator(
                "std::_Tree_iterator<",
                "std::_Tree_val<std::_Tree_simple_types<",
                Content.trim());
        });

    auto toListIterator = [](llvm::StringRef Content, llvm::StringRef Outer) -> std::optional<std::string> {
        llvm::StringRef Element = Content.trim();
        if (Element.starts_with("std::_List_val<")) {
            return std::nullopt;
        }
        return wrapNestedIterator(Outer, "std::_List_val<std::_List_simple_types<", Element);
    };
    Text = replaceTemplateInstances(
        Text,
        "std::_List_const_iterator<",
        [&](llvm::StringRef Content) -> std::optional<std::string> {
            return toListIterator(Content, "std::_List_const_iterator<");
        });
    Text = replaceTemplateInstances(
        Text,
        "std::_List_iterator<",
        [&](llvm::StringRef Content) -> std::optional<std::string> {
            return toListIterator(Content, "std::_List_iterator<");
        });

    auto toNodeIterator = [](llvm::StringRef Content, llvm::StringRef Outer) -> std::optional<std::string> {
        auto [Element, Rest] = splitTopLevelArgument(Content);
        return wrapNestedIterator(Outer, "std::_List_val<std::_List_simple_types<", Element);
    };
    Text = replaceTemplateInstances(
        Text,
        "std::__detail::_Node_const_iterator<",
        [&](llvm::StringRef Content) -> std::optional<std::string> {
            return toNodeIterator(Content, "std::_List_const_iterator<");
        });
    Text = replaceTemplateInstances(
        Text,
        "std::__detail::_Node_iterator<",
        [&](llvm::StringRef Content) -> std::optional<std::string> {
            return toNodeIterator(Content, "std::_List_iterator<");
        });
    return Text;
}

// Windows elides the default duration of system_clock::time_point, while libstdc++
// prints its nanosecond duration explicitly. Collapse it to the Windows spelling.
std::string normalizeSystemClockTimePoint(std::string Text) {
    const std::string Long =
        "std::chrono::time_point<std::chrono::system_clock, "
        "std::chrono::duration<long long, std::ratio<1, 1000000000>>>";
    const std::string Short = "std::chrono::time_point<std::chrono::system_clock>";
    size_t Pos = 0;
    while ((Pos = Text.find(Long, Pos)) != std::string::npos) {
        Text.replace(Pos, Long.size(), Short);
        Pos += Short.size();
    }
    return Text;
}

bool parseOptions(int argc, char** argv, Options& Opt) {
    for (int I = 1; I < argc; ++I) {
        llvm::StringRef Arg = argv[I];
        if (Arg == "--") {
            for (++I; I < argc; ++I) {
                Opt.CompilerArgs.emplace_back(argv[I]);
            }
            break;
        }

        auto next = [&](std::string& Value) {
            if (I + 1 >= argc) {
                return false;
            }
            Value = argv[++I];
            return true;
        };

        if (Arg == "--surface") {
            if (!next(Opt.Surface)) {
                return false;
            }
        } else if (Arg == "--output") {
            if (!next(Opt.Output)) {
                return false;
            }
        } else if (Arg == "--root") {
            if (!next(Opt.Root)) {
                return false;
            }
        } else if (Arg == "--configuration") {
            if (!next(Opt.Configuration)) {
                return false;
            }
        } else if (Arg == "--header") {
            if (!next(Opt.Header)) {
                return false;
            }
        } else if (Arg == "--source") {
            if (!next(Opt.Source)) {
                return false;
            }
        } else if (Arg == "--filter") {
            std::string Filter;
            if (!next(Filter)) {
                return false;
            }
            Opt.Filters.push_back(std::move(Filter));
        } else if (Arg == "--module") {
            if (!next(Opt.Module)) {
                return false;
            }
        } else {
            llvm::errs() << "unknown option: " << Arg << "\n";
            return false;
        }
    }

    return !Opt.Output.empty() && !Opt.Root.empty();
}

std::string hashBytes(llvm::StringRef Data) {
    auto Bytes = llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t*>(Data.data()), Data.size());
    auto Hash = llvm::SHA256::hash(Bytes);
    return llvm::toHex(llvm::ArrayRef<uint8_t>(Hash.data(), Hash.size()));
}

std::string serializeJson(const llvm::json::Value& Value) {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    llvm::json::OStream J(OS);
    J.value(Value);
    OS.flush();
    return Text;
}

class IrVisitor : public RecursiveASTVisitor<IrVisitor> {
public:
    IrVisitor(ASTContext& Ctx, const Options& Opt) : Ctx(Ctx), Opt(Opt) {}

    bool shouldVisitTemplateInstantiations() const { return true; }

    bool VisitCXXRecordDecl(CXXRecordDecl* Decl) {
        if (Decl->isImplicit() || Decl->isLambda() || Decl->isAnonymousStructOrUnion()) {
            return true;
        }
        if (!Decl->isCompleteDefinition() || !isInRoot(Decl->getLocation())) {
            return true;
        }
        if (Decl->isInvalidDecl() || Decl->isDependentContext()
            || Decl->getDescribedClassTemplate() != nullptr
            || isa<ClassTemplatePartialSpecializationDecl>(Decl)
            || isInStandardNamespace(Decl)
            || isInternalName(Decl)) {
            return true;
        }

        auto* Definition = Decl;
        if (Definition == nullptr) {
            return true;
        }

        std::string DeclId = idFor(Definition, "");
        if (!Emitted.insert(DeclId).second) {
            return true;
        }

        std::string Kind = Definition->isUnion() ? "union" : (Definition->isStruct() ? "struct" : "class");
        Records.emplace_back(makeDeclaration(Definition, DeclId, Kind));
        ++Declarations;

        Records.emplace_back(makeVariant(Definition, DeclId));
        ++Variants;

        for (auto* Member : Definition->decls()) {
            if (auto* Field = dyn_cast<FieldDecl>(Member)) {
                collectDependencyEnums(Field);
                collectDependencyRecords(Field);
                emitMember(Definition, DeclId, Field, "field", false);
            } else if (auto* Variable = dyn_cast<VarDecl>(Member)) {
                collectDependencyEnums(Variable);
                collectDependencyRecords(Variable);
                if (Variable->isStaticDataMember()) {
                    emitMember(Definition, DeclId, Variable, "static_field", true);
                }
            } else if (auto* Method = dyn_cast<CXXMethodDecl>(Member)) {
                if (Method->isImplicit() || Method->isDeleted()) {
                    continue;
                }
                collectDependencyEnums(Method);
                collectDependencyRecords(Method);
                if (isa<CXXConstructorDecl>(Method)) {
                    emitMember(Definition, DeclId, Method, "constructor", false);
                } else if (isa<CXXDestructorDecl>(Method)) {
                    emitMember(Definition, DeclId, Method, "destructor", false);
                } else if (Method->isOverloadedOperator() || isa<CXXConversionDecl>(Method)) {
                    emitMember(Definition, DeclId, Method, "operator", Method->isStatic());
                } else {
                    emitMember(Definition, DeclId, Method, "method", Method->isStatic());
                }
            }
        }

        return true;
    }

    bool VisitFunctionDecl(FunctionDecl* Decl) {
        if (Decl->isImplicit() || isa<CXXMethodDecl>(Decl) || Decl->getDescribedFunctionTemplate() != nullptr) {
            return true;
        }
        if (!Decl->isExternallyVisible() || !isInRoot(Decl->getLocation())) {
            return true;
        }
        if (!Decl->getDeclContext()->isNamespace() || isInternalName(Decl)) {
            return true;
        }

        collectDependencyEnums(Decl);
        collectDependencyRecords(Decl);

        std::string DeclId = idFor(Decl, "");
        if (!Emitted.insert(DeclId).second) {
            return true;
        }

        Records.emplace_back(makeDeclaration(Decl, DeclId, "function"));
        ++Declarations;
        return true;
    }

    bool VisitEnumDecl(EnumDecl* Decl) {
        if (Decl->isImplicit() || !Decl->isCompleteDefinition() || !isInRoot(Decl->getLocation())
            || isInternalName(Decl)) {
            return true;
        }

        std::string DeclId = idFor(Decl, "");
        if (!Emitted.insert(DeclId).second) {
            return true;
        }

        Records.emplace_back(makeDeclaration(Decl, DeclId, "enum"));
        ++Declarations;
        return true;
    }

    // Enums are often declared in headers outside the module filter while still
    // appearing in filtered signatures. Record them as dependency declarations so
    // the ABI planner can map their underlying integer type.
    void collectDependencyEnums(const NamedDecl* Decl) {
        if (const auto* Function = dyn_cast<FunctionDecl>(Decl)) {
            collectDependencyEnum(Function->getReturnType());
            for (auto* Parameter : Function->parameters()) {
                collectDependencyEnum(Parameter->getType());
            }
        } else if (const auto* Field = dyn_cast<FieldDecl>(Decl)) {
            collectDependencyEnum(Field->getType());
        } else if (const auto* Variable = dyn_cast<VarDecl>(Decl)) {
            collectDependencyEnum(Variable->getType());
        }
    }

    void collectDependencyEnum(QualType Type) {
        const auto* Enum = Type.getCanonicalType().getTypePtr()->getAs<EnumType>();
        if (Enum == nullptr) {
            return;
        }

        EnumDecl* Decl = Enum->getDecl();
        if (Decl == nullptr || Decl->isImplicit() || !Decl->isCompleteDefinition()
            || !isInRoot(Decl->getLocation(), false) || isInternalName(Decl)) {
            return;
        }

        DependencyEnums.emplace(idFor(Decl, ""), Decl);
    }

    void EmitDependencyEnums() {
        for (const auto& [Id, Decl] : DependencyEnums) {
            if (!Emitted.insert(Id).second) {
                continue;
            }

            Records.emplace_back(makeDeclaration(Decl, Id, "enum"));
            ++Declarations;
        }
    }

    // Records passed by value in filtered signatures may live in headers outside
    // the module filter. Emit their declarations plus the members needed to
    // construct and inspect them; reference-only dependencies stay opaque.
    void collectDependencyRecords(const NamedDecl* Decl) {
        if (const auto* Function = dyn_cast<FunctionDecl>(Decl)) {
            collectDependencyRecord(Function->getReturnType());
            for (auto* Parameter : Function->parameters()) {
                collectDependencyRecord(Parameter->getType());
            }
        } else if (const auto* Field = dyn_cast<FieldDecl>(Decl)) {
            collectDependencyRecord(Field->getType());
        } else if (const auto* Variable = dyn_cast<VarDecl>(Decl)) {
            collectDependencyRecord(Variable->getType());
        }
    }

    void collectDependencyRecord(QualType Type) {
        Type = Type.getCanonicalType();
        if (Type.isNull() || Type->isPointerType() || Type->isReferenceType() || Type->isArrayType()) {
            return;
        }
        if (!Type->isRecordType()) {
            return;
        }

        CXXRecordDecl* Record = Type->getAsCXXRecordDecl();
        if (Record == nullptr || Record->isImplicit() || !Record->isCompleteDefinition()
            || Record->isInvalidDecl() || Record->isDependentContext() || Record->isLambda()
            || Record->isAnonymousStructOrUnion() || Record->getDescribedClassTemplate() != nullptr
            || isa<ClassTemplateSpecializationDecl>(Record)
            || !Record->isExternallyVisible() || !isInRoot(Record->getLocation(), false)
            || isInStandardNamespace(Record) || isInternalName(Record) || !isDeclAccessible(Record)) {
            return;
        }

        DependencyRecords.emplace(idFor(Record, ""), Record);
    }

    void EmitDependencyRecords() {
        std::set<std::string> Processed;
        while (true) {
            auto It = std::find_if(
                DependencyRecords.begin(),
                DependencyRecords.end(),
                [&](const auto& Entry) { return Processed.find(Entry.first) == Processed.end(); }
            );
            if (It == DependencyRecords.end()) {
                break;
            }

            Processed.insert(It->first);
            if (!Emitted.insert(It->first).second) {
                continue;
            }

            CXXRecordDecl* Decl = It->second;
            std::string Kind = Decl->isUnion() ? "union" : (Decl->isStruct() ? "struct" : "class");
            Records.emplace_back(makeDeclaration(Decl, It->first, Kind));
            ++Declarations;
            Records.emplace_back(makeVariant(Decl, It->first));
            ++Variants;

            for (auto* Member : Decl->decls()) {
                if (auto* Field = dyn_cast<FieldDecl>(Member)) {
                    if (Field->getAccess() != AS_public) {
                        continue;
                    }
                    collectDependencyEnums(Field);
                    collectDependencyRecords(Field);
                    emitMember(Decl, It->first, Field, "field", false);
                } else if (auto* Variable = dyn_cast<VarDecl>(Member)) {
                    if (Variable->getAccess() != AS_public || !Variable->isStaticDataMember()) {
                        continue;
                    }
                    collectDependencyEnums(Variable);
                    collectDependencyRecords(Variable);
                    emitMember(Decl, It->first, Variable, "static_field", true);
                } else if (auto* Method = dyn_cast<CXXMethodDecl>(Member)) {
                    if (Method->isImplicit() || Method->isDeleted() || Method->getAccess() != AS_public) {
                        continue;
                    }
                    collectDependencyEnums(Method);
                    collectDependencyRecords(Method);
                    if (isa<CXXConstructorDecl>(Method)) {
                        emitMember(Decl, It->first, Method, "constructor", false);
                    } else if (isa<CXXDestructorDecl>(Method)) {
                        emitMember(Decl, It->first, Method, "destructor", false);
                    }
                }
            }
        }
    }

    std::vector<llvm::json::Object> Records;
    int Declarations = 0;
    int Variants = 0;
    int Members = 0;

private:
    ASTContext& Ctx;
    const Options& Opt;
    std::set<std::string> Emitted;
    std::map<std::string, EnumDecl*> DependencyEnums;
    std::map<std::string, CXXRecordDecl*> DependencyRecords;

    bool isInternalName(const NamedDecl* Decl) const {
        std::string Qualified = Decl->getQualifiedNameAsString();
        if (Qualified.empty() || Qualified.front() == '_') {
            return true;
        }
        return Qualified.rfind("ll::internal::", 0) == 0 || Qualified.rfind("ll::concepts::", 0) == 0;
    }

    bool isInStandardNamespace(const Decl* Decl) const {
        for (auto* Context = Decl->getDeclContext(); Context != nullptr; Context = Context->getParent()) {
            if (auto* Namespace = dyn_cast<NamespaceDecl>(Context)) {
                llvm::StringRef Name = Namespace->getName();
                if (Name == "std" || Name.starts_with("__")) {
                    return true;
                }
            }
        }
        return false;
    }

    bool isInRoot(SourceLocation Location, bool ApplyFilters = true) const {
        if (Location.isInvalid()) {
            return false;
        }

        auto& SM = Ctx.getSourceManager();
        SourceLocation Spelling = SM.getSpellingLoc(Location);
        if (SM.isInSystemHeader(Spelling)) {
            return false;
        }

        llvm::StringRef File = SM.getFilename(Spelling);
        if (File.empty()) {
            return false;
        }

        llvm::SmallString<256> Absolute(File);
        if (llvm::sys::fs::make_absolute(Absolute)) {
            return false;
        }

        std::string Path = normalizePath(Absolute.str().str());
        std::string Root = normalizePath(Opt.Root);
        if (Path.rfind(Root, 0) != 0) {
            return false;
        }

        if (!ApplyFilters || Opt.Filters.empty()) {
            return true;
        }

        for (const auto& Filter : Opt.Filters) {
            if (Path.rfind(normalizePath(Filter), 0) == 0) {
                return true;
            }
        }

        return false;
    }

    std::string qualifiedNameFor(const NamedDecl* Decl) const {
        if (auto* Spec = dyn_cast<ClassTemplateSpecializationDecl>(Decl)) {
            if (auto* Template = Spec->getSpecializedTemplate()) {
                std::string Arguments;
                llvm::raw_string_ostream OS(Arguments);
                PrintingPolicy Policy(Ctx.getLangOpts());
                Policy.SuppressTagKeyword = true;
                bool First = true;
                for (const auto& Argument : Spec->getTemplateArgs().asArray()) {
                    if (Argument.getKind() == TemplateArgument::Pack) {
                        for (const auto& Element : Argument.pack_elements()) {
                            if (!First) {
                                OS << ", ";
                            }
                            First = false;
                            Element.print(Policy, OS, true);
                        }
                        continue;
                    }

                    if (!First) {
                        OS << ", ";
                    }
                    First = false;
                    Argument.print(Policy, OS, true);
                }
                OS.flush();
                return Template->getQualifiedNameAsString() + "<" + normalizePlatformTypeText(Arguments) + ">";
            }
        }

        return normalizePlatformTypeText(Decl->getQualifiedNameAsString());
    }

    std::string idFor(const Decl* Decl, const std::string& ParentId) const {
        const auto* Named = dyn_cast<NamedDecl>(Decl);
        std::string Qualified = Named ? qualifiedNameFor(Named) : Decl->getDeclKindName();
        std::string Suffix;
        if (auto* Function = dyn_cast<FunctionDecl>(Decl)) {
            std::string Signature = typeString(Function->getReturnType()) + "(";
            for (auto* Parameter : Function->parameters()) {
                Signature += typeString(Parameter->getType());
                Signature += ",";
            }
            Signature += ")";
            Suffix = "#" + Signature;
        }

        if (ParentId.empty()) {
            return Opt.Surface + ":" + Qualified + Suffix;
        }

        return ParentId + "@" + Qualified + Suffix;
    }

    std::string accessString(AccessSpecifier Access) const {
        switch (Access) {
        case AS_public:
            return "public";
        case AS_protected:
            return "protected";
        case AS_private:
            return "private";
        default:
            return "public";
        }
    }

    bool isDeclAccessible(const Decl* D) const {
        if (const auto* Tag = dyn_cast<TagDecl>(D)) {
            AccessSpecifier Access = Tag->getAccess();
            if (Access == AS_private || Access == AS_protected) {
                return false;
            }
        }
        for (const DeclContext* Context = D->getDeclContext(); Context; Context = Context->getParent()) {
            if (const auto* Tag = dyn_cast<TagDecl>(Context)) {
                AccessSpecifier Access = Tag->getAccess();
                if (Access == AS_private || Access == AS_protected) {
                    return false;
                }
            } else if (!isa<NamespaceDecl>(Context) && !isa<TranslationUnitDecl>(Context)) {
                break;
            }
        }
        return true;
    }

    bool isTypeAccessible(QualType Type) const {
        Type = Type.getCanonicalType();
        if (Type.isNull()) {
            return true;
        }

        if (const auto* Tag = Type->getAsTagDecl()) {
            if (!isDeclAccessible(Tag)) {
                return false;
            }
            if (const auto* Spec = dyn_cast<ClassTemplateSpecializationDecl>(Tag)) {
                for (const auto& Argument : Spec->getTemplateArgs().asArray()) {
                    if (Argument.getKind() == TemplateArgument::Type
                        && !isTypeAccessible(Argument.getAsType())) {
                        return false;
                    }
                }
            }
            return true;
        }

        if (const auto* Spec = Type->getAs<TemplateSpecializationType>()) {
            if (const auto* Record = Spec->getAsCXXRecordDecl()) {
                if (!isDeclAccessible(Record)) {
                    return false;
                }
            }
            for (const auto& Argument : Spec->template_arguments()) {
                if (Argument.getKind() == TemplateArgument::Type
                    && !isTypeAccessible(Argument.getAsType())) {
                    return false;
                }
            }
            return true;
        }

        if (const auto* Pointer = Type->getAs<PointerType>()) {
            return isTypeAccessible(Pointer->getPointeeType());
        }
        if (const auto* Reference = Type->getAs<ReferenceType>()) {
            return isTypeAccessible(Reference->getPointeeType());
        }
        return true;
    }

    std::string normalizePlatformTypeText(std::string Text) const {
        bool Long64 = Ctx.getTargetInfo().getLongWidth() == 64;
        if (Long64) {
            Text = normalize64BitLongTypes(Text);
        }
        Text = normalizeGnuIteratorTypes(Text);
        Text = normalizeGnuStdIteratorTypes(Text);
        if (Long64) {
            Text = normalizeSystemClockTimePoint(std::move(Text));
        }
        return Text;
    }

    std::string typeString(QualType Type) const {
        PrintingPolicy Policy(Ctx.getLangOpts());
        Policy.SuppressTagKeyword = true;
        return normalizePlatformTypeText(Type.getCanonicalType().getAsString(Policy));
    }

    llvm::json::Object makeSource(SourceLocation Location) const {
        auto& SM = Ctx.getSourceManager();
        SourceLocation Spelling = SM.getSpellingLoc(Location);
        llvm::json::Object Source;
        Source["file"] = SM.getFilename(Spelling).str();
        Source["line"] = static_cast<int64_t>(SM.getSpellingLineNumber(Spelling));
        Source["column"] = static_cast<int64_t>(SM.getSpellingColumnNumber(Spelling));
        return Source;
    }

    llvm::json::Object makeType(QualType Type) const {
        llvm::json::Object Object;
        std::string Canonical = typeString(Type);
        Object["canonical"] = Canonical;

        if (Type->isVoidType()) {
            Object["kind"] = "fundamental";
        } else if (Type->isFundamentalType() || Type->isIntegerType() || Type->isFloatingType() || Type->isBooleanType()) {
            Object["kind"] = "fundamental";
        } else if (Type->isEnumeralType()) {
            Object["kind"] = "enum";
        } else if (Type->isRecordType()) {
            Object["kind"] = "record";
        } else if (Type->isPointerType()) {
            Object["kind"] = "pointer";
        } else if (Type->isReferenceType()) {
            Object["kind"] = "reference";
        } else {
            Object["kind"] = "other";
        }

        return Object;
    }

    llvm::json::Object makeSignature(FunctionDecl* Decl) const {
        llvm::json::Object Signature;
        Signature["canonical"] = normalizePlatformTypeText(Decl->getQualifiedNameAsString());
        Signature["returnType"] = typeString(Decl->getReturnType());
        Signature["variadic"] = Decl->isVariadic();

        llvm::json::Array Parameters;
        for (auto* Parameter : Decl->parameters()) {
            llvm::json::Object Item;
            Item["name"] = Parameter->getNameAsString();
            Item["type"] = typeString(Parameter->getType());
            Parameters.emplace_back(std::move(Item));
        }
        Signature["parameters"] = std::move(Parameters);

        if (auto* Method = dyn_cast<CXXMethodDecl>(Decl)) {
            Signature["const"] = Method->isConst();
        }

        return Signature;
    }

    llvm::json::Array inferMacros(NamedDecl* Decl) const {
        bool hasExport = false;
        bool hasNodiscard = false;
        bool hasDeprecated = false;
        for (auto* Attribute : Decl->attrs()) {
            llvm::StringRef Name = Attribute->getSpelling();
            if (Name == "DLLExport" || Name == "DLLImport" || Name == "Visibility"
                || Name == "dllexport" || Name == "dllimport" || Name == "visibility") {
                hasExport = true;
            } else if (Name == "WarnUnusedResult" || Name == "warn_unused_result" || Name == "nodiscard") {
                hasNodiscard = true;
            } else if (Name == "Deprecated" || Name == "deprecated") {
                hasDeprecated = true;
            }
        }

        llvm::json::Array Macros;
        auto add = [&](llvm::StringRef Name) {
            llvm::json::Object Item;
            Item["name"] = Name;
            Item["expansion"] = "attribute-inferred";
            Macros.emplace_back(std::move(Item));
        };

        if (Opt.Surface == "levilamina") {
            if (hasExport && hasNodiscard) {
                add("LLNDAPI");
            } else if (hasExport) {
                add("LLAPI");
            }
        } else if (Opt.Surface == "bedrock") {
            if (hasExport && hasDeprecated) {
                add("MCNAPI");
            } else if (hasExport) {
                add("MCAPI");
            }
        }

        return Macros;
    }

    std::string templateArgumentString(const TemplateArgument& Argument) const {
        std::string Text;
        llvm::raw_string_ostream OS(Text);
        PrintingPolicy Policy(Ctx.getLangOpts());
        Policy.SuppressTagKeyword = true;
        Argument.print(Policy, OS, true);
        OS.flush();
        return Text;
    }

    std::string templateArgumentKind(const TemplateArgument& Argument) const {
        switch (Argument.getKind()) {
        case TemplateArgument::Null:
            return "null";
        case TemplateArgument::Type:
            return "type";
        case TemplateArgument::Declaration:
            return "declaration";
        case TemplateArgument::NullPtr:
            return "nullptr";
        case TemplateArgument::Integral:
            return "integral";
        case TemplateArgument::Template:
            return "template";
        case TemplateArgument::TemplateExpansion:
            return "template_expansion";
        case TemplateArgument::Expression:
            return "expression";
        case TemplateArgument::Pack:
            return "pack";
        }
        return "unknown";
    }

    std::string specializationKind(const ClassTemplateSpecializationDecl* Spec) const {
        switch (Spec->getSpecializationKind()) {
        case TSK_Undeclared:
            return "undeclared";
        case TSK_ImplicitInstantiation:
            return "implicit";
        case TSK_ExplicitSpecialization:
            return "explicit";
        case TSK_ExplicitInstantiationDeclaration:
            return "explicit_instantiation_declaration";
        case TSK_ExplicitInstantiationDefinition:
            return "explicit_instantiation_definition";
        }
        return "unknown";
    }

    llvm::json::Object makeDeclaration(NamedDecl* Decl, const std::string& Id, const std::string& Kind) const {
        llvm::json::Object Object;
        Object["recordType"] = "declaration";
        Object["id"] = Id;
        Object["surface"] = Opt.Surface;
        Object["kind"] = Kind;
        Object["qualifiedName"] = qualifiedNameFor(Decl);
        Object["access"] = accessString(Decl->getAccess());
        Object["linkage"] = Decl->isExternallyVisible() ? "external" : "internal";
        Object["source"] = makeSource(Decl->getLocation());

        llvm::json::Array Attributes;
        for (auto* Attribute : Decl->attrs()) {
            llvm::json::Object Item;
            Item["name"] = Attribute->getSpelling();
            Attributes.emplace_back(std::move(Item));
        }
        if (!Attributes.empty()) {
            Object["attributes"] = std::move(Attributes);
        }

        auto Macros = inferMacros(Decl);
        if (!Macros.empty()) {
            Object["macros"] = std::move(Macros);
        }

        if (auto* Spec = dyn_cast<ClassTemplateSpecializationDecl>(Decl)) {
            llvm::json::Object Template;
            Template["specializationKind"] = specializationKind(Spec);
            Template["instantiationKind"] = Spec->isExplicitSpecialization() ? "explicit" : "implicit";
            llvm::json::Array Arguments;
            for (const auto& Argument : Spec->getTemplateArgs().asArray()) {
                llvm::json::Object Item;
                Item["kind"] = templateArgumentKind(Argument);
                Item["value"] = templateArgumentString(Argument);
                if (Argument.getKind() == TemplateArgument::Type) {
                    if (!isTypeAccessible(Argument.getAsType())) {
                        Item["accessible"] = false;
                    }
                    if (Argument.getAsType()->isIncompleteType()) {
                        Item["complete"] = false;
                    }
                }
                Arguments.emplace_back(std::move(Item));
            }
            Template["arguments"] = std::move(Arguments);
            Object["template"] = std::move(Template);
        }

        if (auto* Function = dyn_cast<FunctionDecl>(Decl)) {
            Object["signature"] = makeSignature(Function);
            Object["inline"] = Function->isInlined();
            Object["defined"] = Function->isThisDeclarationADefinition();
            Object["consteval"] = Function->isConsteval();
        }

        if (auto* Enum = dyn_cast<EnumDecl>(Decl)) {
            Object["underlyingType"] = typeString(Enum->getIntegerType());
        }

        if (auto* Record = dyn_cast<CXXRecordDecl>(Decl)) {
            if (Record->isCompleteDefinition()) {
                bool CopyConstructible = Record->hasSimpleCopyConstructor();
                if (!CopyConstructible) {
                    for (auto* Constructor : Record->ctors()) {
                        if (Constructor->isCopyConstructor()
                            && !Constructor->isDeleted()
                            && Constructor->getAccess() == AS_public) {
                            CopyConstructible = true;
                            break;
                        }
                    }
                }
                Object["abstract"] = Record->isAbstract();
                Object["copyConstructible"] = CopyConstructible;
            }
        }

        return Object;
    }

    llvm::json::Object makeVariant(CXXRecordDecl* Decl, const std::string& DeclId) const {
        llvm::json::Object Object;
        Object["recordType"] = "variant";
        Object["declarationId"] = DeclId;
        Object["configuration"] = Opt.Configuration;
        Object["availability"] = "present";

        const ASTRecordLayout& Layout = Ctx.getASTRecordLayout(Decl);
        llvm::json::Object LayoutObject;
        LayoutObject["size"] = static_cast<int64_t>(Layout.getSize().getQuantity());
        LayoutObject["align"] = static_cast<int64_t>(Layout.getAlignment().getQuantity());

        llvm::json::Array Fields;
        for (auto* Field : Decl->fields()) {
            llvm::json::Object Item;
            Item["memberId"] = idFor(Field, DeclId);
            Item["bitOffset"] = static_cast<int64_t>(Layout.getFieldOffset(Field->getFieldIndex()));
            Item["bitWidth"] = Field->isBitField() ? static_cast<int64_t>(Field->getBitWidthValue()) : 0;
            Fields.emplace_back(std::move(Item));
        }
        LayoutObject["fieldOffsets"] = std::move(Fields);

        llvm::json::Array Bases;
        for (const auto& Base : Decl->bases()) {
            llvm::json::Object Item;
            Item["declarationId"] = Opt.Surface + ":" + typeString(Base.getType());
            Item["offset"] = 0;
            Item["access"] = accessString(Base.getAccessSpecifier());
            Item["virtual"] = Base.isVirtual();
            Bases.emplace_back(std::move(Item));
        }
        LayoutObject["bases"] = std::move(Bases);

        Object["layout"] = std::move(LayoutObject);
        return Object;
    }

    void emitMember(CXXRecordDecl* Owner, const std::string& DeclId, NamedDecl* Member, const std::string& Kind, bool Static) {
        std::string MemberId = idFor(Member, DeclId);
        if (!Emitted.insert(MemberId).second) {
            return;
        }

        llvm::json::Object Object;
        Object["recordType"] = "member";
        Object["id"] = MemberId;
        Object["declarationId"] = DeclId;
        Object["configuration"] = Opt.Configuration;
        Object["kind"] = Kind;
        Object["name"] = normalizePlatformTypeText(Member->getNameAsString());
        Object["access"] = accessString(Member->getAccess());
        Object["static"] = Static;

        if (auto* Function = dyn_cast<FunctionDecl>(Member);
            Function && Function->getDescribedFunctionTemplate()) {
            Object["memberTemplate"] = true;
        }

        if (auto* Method = dyn_cast<CXXMethodDecl>(Member)) {
            switch (Method->getRefQualifier()) {
            case RQ_LValue:
                Object["refQualifier"] = "lvalue";
                break;
            case RQ_RValue:
                Object["refQualifier"] = "rvalue";
                break;
            case RQ_None:
                break;
            }
        }

        if (auto* Variable = dyn_cast<VarDecl>(Member);
            Variable && isa<VarTemplateSpecializationDecl>(Variable)) {
            Object["variableTemplate"] = true;
        }

        llvm::json::Array Attributes;
        for (auto* Attribute : Member->attrs()) {
            llvm::json::Object Item;
            Item["name"] = Attribute->getSpelling();
            Attributes.emplace_back(std::move(Item));
        }
        if (!Attributes.empty()) {
            Object["attributes"] = std::move(Attributes);
        }

        auto Macros = inferMacros(Member);
        if (!Macros.empty()) {
            Object["macros"] = std::move(Macros);
        }

        if (auto* Field = dyn_cast<FieldDecl>(Member)) {
            Object["type"] = makeType(Field->getType());
            const ASTRecordLayout& Layout = Ctx.getASTRecordLayout(Owner);
            Object["bitOffset"] = static_cast<int64_t>(Layout.getFieldOffset(Field->getFieldIndex()));
            Object["bitWidth"] = Field->isBitField() ? static_cast<int64_t>(Field->getBitWidthValue()) : 0;
        } else if (auto* Variable = dyn_cast<VarDecl>(Member)) {
            Object["type"] = makeType(Variable->getType());
            Object["inline"] = Variable->isInline() || Variable->isConstexpr();
            Object["defined"] = Variable->isThisDeclarationADefinition() == VarDecl::Definition;
        } else if (auto* Method = dyn_cast<CXXMethodDecl>(Member)) {
            Object["virtual"] = Method->isVirtual();
            Object["pure"] = Method->isPureVirtual();
            Object["inline"] = Method->isInlined();
            Object["defined"] = Method->hasBody();
            Object["consteval"] = Method->isConsteval();
            Object["signature"] = makeSignature(Method);
            if (Method->begin_overridden_methods() != Method->end_overridden_methods()) {
                Object["overrideOf"] = normalizePlatformTypeText(
                    (*Method->begin_overridden_methods())->getQualifiedNameAsString());
            }
        }

        Records.emplace_back(std::move(Object));
        ++Members;
    }
};

} // namespace

int main(int argc, char** argv) {
    Options Opt;
    if (!parseOptions(argc, argv, Opt)) {
        llvm::errs() << "usage: hosihikari-ir-frontend --surface <surface> --output <dir> --root <include-root> "
                        "--configuration <id> [--header <header>] [--source <file>] [--filter <path>] "
                        "[--module <module>] -- <clang args>\n";
        return 2;
    }

    std::string Code;
    if (!Opt.Source.empty()) {
        auto Buffer = llvm::MemoryBuffer::getFile(Opt.Source);
        if (!Buffer) {
            llvm::errs() << "cannot read source: " << Opt.Source << "\n";
            return 1;
        }
        Code = (*Buffer)->getBuffer().str();
    } else {
        Code = "#include \"" + Opt.Header + "\"\n";
    }
    std::vector<std::string> Args = Opt.CompilerArgs;
    Args.emplace_back("-I" + Opt.Root);
    Args.emplace_back("-std=c++20");
    Args.emplace_back("-fsyntax-only");

    std::unique_ptr<ASTUnit> AST = clang::tooling::buildASTFromCodeWithArgs(
        Code,
        Args,
        "hosihikari_umbrella.cpp"
    );

    if (!AST) {
        llvm::errs() << "failed to build AST\n";
        return 1;
    }

    auto& Diagnostics = AST->getDiagnostics();
    unsigned Errors = Diagnostics.getNumErrors();
    unsigned Warnings = Diagnostics.getNumWarnings();

    IrVisitor Visitor(AST->getASTContext(), Opt);
    Visitor.TraverseDecl(AST->getASTContext().getTranslationUnitDecl());
    Visitor.EmitDependencyRecords();
    Visitor.EmitDependencyEnums();

    llvm::sys::fs::create_directories(Opt.Output);

    std::string ShardPath = Opt.Output + "/shard.jsonl";
    std::string ShardText;
    for (auto& Record : Visitor.Records) {
        llvm::json::Value Value(std::move(Record));
        ShardText += serializeJson(Value);
        ShardText += "\n";
    }

    {
        std::ofstream Shard(ShardPath, std::ios::binary);
        Shard << ShardText;
    }

    std::string ShardHash = "sha256:" + hashBytes(ShardText);
    std::string DefinesHash = "sha256:" + hashBytes("");

    llvm::json::Object Manifest;
    Manifest["schema"] = "hosihikari.bindings.ir/1";
    Manifest["irVersion"] = "1.3.0";

    llvm::json::Object Frontend;
    Frontend["name"] = "libtooling";
    Frontend["version"] = "0.4.0";
    Frontend["llvm"] = "23.1.3";
    Manifest["frontend"] = std::move(Frontend);

    llvm::json::Array Sources;
    {
        llvm::json::Object Source;
        Source["surface"] = Opt.Surface;
        Source["repo"] = "local";
        Source["commit"] = "unknown";
        Source["path"] = normalizePath(Opt.Root);
        Sources.emplace_back(std::move(Source));
    }
    Manifest["sources"] = std::move(Sources);

    llvm::json::Array Configurations;
    {
        llvm::json::Object Configuration;
        std::string Os;
        std::string Arch;
        std::string Target;
        splitConfiguration(Opt.Configuration, Os, Arch, Target);
        Configuration["id"] = Opt.Configuration;
        Configuration["os"] = Os;
        Configuration["arch"] = Arch;
        Configuration["target"] = Target;
        Configuration["defines"] = llvm::json::Array();
        Configuration["definesHash"] = DefinesHash;
        Configurations.emplace_back(std::move(Configuration));
    }
    Manifest["configurations"] = std::move(Configurations);

    llvm::json::Array Shards;
    {
        llvm::json::Object Shard;
        Shard["surface"] = Opt.Surface;
        Shard["configuration"] = Opt.Configuration;
        Shard["module"] = Opt.Module;
        Shard["path"] = "shard.jsonl";
        Shard["sha256"] = ShardHash;
        Shard["sha256Compressed"] = ShardHash;
        Shard["records"] = static_cast<int64_t>(Visitor.Records.size());
        Shards.emplace_back(std::move(Shard));
    }
    Manifest["shards"] = std::move(Shards);

    llvm::json::Object DiagnosticsObject;
    DiagnosticsObject["errors"] = static_cast<int64_t>(Errors);
    DiagnosticsObject["warnings"] = static_cast<int64_t>(Warnings);
    DiagnosticsObject["shardsWithErrors"] = llvm::json::Array();
    Manifest["diagnostics"] = std::move(DiagnosticsObject);

    llvm::json::Object Counts;
    Counts["declarations"] = static_cast<int64_t>(Visitor.Declarations);
    Counts["variants"] = static_cast<int64_t>(Visitor.Variants);
    Counts["members"] = static_cast<int64_t>(Visitor.Members);
    Manifest["counts"] = std::move(Counts);

    {
        std::ofstream ManifestFile(Opt.Output + "/ir-manifest.json", std::ios::binary);
        ManifestFile << serializeJson(llvm::json::Value(std::move(Manifest))) << "\n";
    }

    llvm::outs() << "emitted " << Visitor.Declarations << " declarations, " << Visitor.Members << " members, "
                 << Errors << " errors, " << Warnings << " warnings\n";

    return Errors == 0 ? 0 : 1;
}
