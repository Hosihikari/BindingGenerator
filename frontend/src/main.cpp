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
#include <fstream>
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
                emitMember(Definition, DeclId, Field, "field", false);
            } else if (auto* Variable = dyn_cast<VarDecl>(Member)) {
                if (Variable->isStaticDataMember()) {
                    emitMember(Definition, DeclId, Variable, "static_field", true);
                }
            } else if (auto* Method = dyn_cast<CXXMethodDecl>(Member)) {
                if (Method->isImplicit() || Method->isDeleted()) {
                    continue;
                }
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

    std::vector<llvm::json::Object> Records;
    int Declarations = 0;
    int Variants = 0;
    int Members = 0;

private:
    ASTContext& Ctx;
    const Options& Opt;
    std::set<std::string> Emitted;

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

    bool isInRoot(SourceLocation Location) const {
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

        if (Opt.Filters.empty()) {
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
                return Template->getQualifiedNameAsString() + "<" + Arguments + ">";
            }
        }

        return Decl->getQualifiedNameAsString();
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

    std::string typeString(QualType Type) const {
        PrintingPolicy Policy(Ctx.getLangOpts());
        Policy.SuppressTagKeyword = true;
        return Type.getCanonicalType().getAsString(Policy);
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
        Signature["canonical"] = Decl->getQualifiedNameAsString();
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
        Object["name"] = Member->getNameAsString();
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
                Object["overrideOf"] = (*Method->begin_overridden_methods())->getQualifiedNameAsString();
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
    Manifest["irVersion"] = "1.2.0";

    llvm::json::Object Frontend;
    Frontend["name"] = "libtooling";
    Frontend["version"] = "0.2.0";
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
        Configuration["id"] = Opt.Configuration;
        Configuration["os"] = "windows";
        Configuration["arch"] = "x64";
        Configuration["target"] = "server";
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
