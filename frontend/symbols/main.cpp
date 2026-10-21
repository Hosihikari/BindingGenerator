// Prototype symbol-level IR extractor for Bedrock 1.21.3.
//
// Reads public symbols from a publics-only PDB and an ELF symbol table, demangles
// the symbols for a small set of classes, merges the two configurations, and
// writes a prototype JSON document. It intentionally does not emit the versioned
// IR yet; the schema is still being shaped.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/DebugInfo/CodeView/SymbolDeserializer.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/DebugInfo/PDB/PDB.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/SymbolStream.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

struct Inputs {
    std::string Pdb;
    std::string Exe;
    std::string Elf;
    std::string Output;
    std::string Version = "1.21.3";
    std::vector<std::string> Classes;
};

struct Method {
    std::string ClassName;
    std::string Name;
    std::string Key;
    std::string ReturnType;
    std::vector<std::string> Parameters;
    std::string Access = "unknown";
    bool Const = false;
    bool RefQualified = false;
    bool Static = false;
    bool Virtual = false;
    bool Ctor = false;
    bool Dtor = false;
    std::string Symbol;
    uint64_t Rva = 0;
    uint64_t Va = 0;
    int VtableIndex = -1;
};

struct Configuration {
    std::string Id;
    std::map<std::string, Method> Methods;
    std::map<std::string, std::string> VtableSymbols;
    std::map<std::string, uint64_t> VtableAddresses;
};

std::string trim(StringRef Text) {
    return Text.trim().str();
}

bool startsWith(const std::string& Text, StringRef Prefix) {
    return StringRef(Text).starts_with(Prefix);
}

std::string replaceAll(std::string Text, StringRef From, StringRef To) {
    size_t Position = 0;
    while ((Position = Text.find(From.str(), Position)) != std::string::npos) {
        Text.replace(Position, From.size(), To.str());
        Position += To.size();
    }
    return Text;
}

std::string normalizeLongWords(const std::string& Text) {
    auto isWord = [](char Character) {
        return std::isalnum(static_cast<unsigned char>(Character)) != 0 || Character == '_';
    };
    auto peekWord = [&](size_t& Position) {
        while (Position < Text.size() && Text[Position] == ' ') {
            ++Position;
        }
        size_t Start = Position;
        while (Position < Text.size() && isWord(Text[Position])) {
            ++Position;
        }
        return Text.substr(Start, Position - Start);
    };

    std::string Out;
    size_t Index = 0;
    while (Index < Text.size()) {
        size_t Start = Index;
        while (Index < Text.size() && isWord(Text[Index])) {
            ++Index;
        }
        if (Start == Index) {
            Out.push_back(Text[Index]);
            ++Index;
            continue;
        }

        std::string Word = Text.substr(Start, Index - Start);
        if (Word == "unsigned") {
            size_t Next = Index;
            if (peekWord(Next) == "long") {
                size_t After = Next;
                if (peekWord(After) == "long") {
                    Out += "unsigned long long";
                    Index = After;
                } else {
                    Out += "unsigned long long";
                    Index = Next;
                }
                continue;
            }
        }
        if (Word == "long") {
            size_t Next = Index;
            std::string NextWord = peekWord(Next);
            if (NextWord == "double") {
                Out += "long";
                continue;
            }
            Out += "long long";
            if (NextWord == "long") {
                Index = Next;
            }
            continue;
        }

        Out += Word;
    }
    return Out;
}

std::string normalizeType(std::string Text, bool Windows) {
    Text = replaceAll(std::move(Text), "class ", "");
    Text = replaceAll(std::move(Text), "struct ", "");
    Text = replaceAll(std::move(Text), "enum ", "");
    Text = replaceAll(std::move(Text), "union ", "");
    Text = replaceAll(std::move(Text), "__ptr64", "");
    Text = replaceAll(std::move(Text), "__restrict", "");
    Text = replaceAll(std::move(Text), "__unaligned", "");
    Text = replaceAll(std::move(Text), "__cdecl", "");
    Text = replaceAll(std::move(Text), "__fastcall", "");
    Text = replaceAll(std::move(Text), "__stdcall", "");
    Text = replaceAll(std::move(Text), "__thiscall", "");
    Text = replaceAll(std::move(Text), "__vectorcall", "");
    Text = replaceAll(std::move(Text), "std::__1::", "std::");
    Text = replaceAll(std::move(Text), "std::__cxx11::", "std::");

    if (Windows) {
        Text = replaceAll(std::move(Text), "unsigned __int64", "unsigned long long");
        Text = replaceAll(std::move(Text), "unsigned __int32", "unsigned int");
        Text = replaceAll(std::move(Text), "unsigned __int16", "unsigned short");
        Text = replaceAll(std::move(Text), "unsigned __int8", "unsigned char");
        Text = replaceAll(std::move(Text), "__int64", "long long");
        Text = replaceAll(std::move(Text), "__int32", "int");
        Text = replaceAll(std::move(Text), "__int16", "short");
        Text = replaceAll(std::move(Text), "__int8", "signed char");
    } else {
        Text = normalizeLongWords(Text);
    }

    std::string Collapsed;
    bool PendingSpace = false;
    for (char Character : Text) {
        if (std::isspace(static_cast<unsigned char>(Character)) != 0) {
            PendingSpace = true;
            continue;
        }
        if (PendingSpace && !Collapsed.empty() && Character != '*' && Character != '&' && Character != '<'
            && Character != '>' && Character != ',' && Character != ')' && Character != ']'
            && Collapsed.back() != '(' && Collapsed.back() != '<' && Collapsed.back() != ',') {
            Collapsed.push_back(' ');
        }
        PendingSpace = false;
        Collapsed.push_back(Character);
    }
    return trim(Collapsed);
}

std::vector<std::string> splitParameters(StringRef Text) {
    std::vector<std::string> Parameters;
    size_t Depth = 0;
    size_t Start = 0;
    for (size_t Index = 0; Index < Text.size(); ++Index) {
        char Character = Text[Index];
        if (Character == '<' || Character == '(' || Character == '[') {
            ++Depth;
        } else if (Character == '>' || Character == ')' || Character == ']') {
            --Depth;
        } else if (Character == ',' && Depth == 0) {
            Parameters.push_back(trim(Text.slice(Start, Index)));
            Start = Index + 1;
        }
    }
    StringRef Last = Text.drop_front(Start).trim();
    if (!Last.empty()) {
        Parameters.push_back(Last.str());
    }
    if (Parameters.size() == 1 && Parameters[0] == "void") {
        Parameters.clear();
    }
    return Parameters;
}

struct ParsedMethod {
    bool Valid = false;
    std::string Access = "unknown";
    std::string ReturnType;
    std::string QualifiedName;
    std::string Name;
    std::vector<std::string> Parameters;
    bool Const = false;
    bool RefQualified = false;
    bool Static = false;
    bool Virtual = false;
    bool Ctor = false;
    bool Dtor = false;
};

ParsedMethod parseDemangled(const std::string& Demangled, const std::string& ClassName, bool Windows) {
    ParsedMethod Result;
    std::string Text = trim(Demangled);

    for (const char* Prefix : {"public: ", "protected: ", "private: "}) {
        if (startsWith(Text, Prefix)) {
            Result.Access = StringRef(Prefix).drop_back(2).str();
            Text = Text.substr(std::strlen(Prefix));
            break;
        }
    }

    for (;;) {
        if (startsWith(Text, "virtual ")) {
            Result.Virtual = true;
            Text = Text.substr(8);
        } else if (startsWith(Text, "static ")) {
            Result.Static = true;
            Text = Text.substr(7);
        } else {
            break;
        }
    }

    const std::string Marker = ClassName + "::";
    size_t MarkerPosition = Text.find(Marker);
    if (MarkerPosition == std::string::npos) {
        return Result;
    }

    size_t NameStart = MarkerPosition + Marker.size();
    size_t OpenParen = Text.find('(', NameStart);
    if (OpenParen == std::string::npos) {
        return Result;
    }

    std::string Name = trim(StringRef(Text).slice(NameStart, OpenParen));
    if (Name.empty() || Name.find("::") != std::string::npos || Name.find(' ') != std::string::npos) {
        return Result;
    }

    Result.Name = Name;
    Result.QualifiedName = Marker + Name;
    Result.Ctor = Name == ClassName;
    Result.Dtor = Name == "~" + ClassName;

    std::string Before = trim(StringRef(Text).slice(0, MarkerPosition));
    for (const char* Convention : {"__cdecl", "__fastcall", "__stdcall", "__thiscall", "__vectorcall"}) {
        if (Before.ends_with(Convention)) {
            Before.resize(Before.size() - std::strlen(Convention));
            Before = trim(Before);
        }
    }
    if (!Result.Ctor && !Result.Dtor) {
        Result.ReturnType = normalizeType(Before, Windows);
    }

    size_t Depth = 0;
    size_t CloseParen = std::string::npos;
    for (size_t Index = OpenParen; Index < Text.size(); ++Index) {
        if (Text[Index] == '(') {
            ++Depth;
        } else if (Text[Index] == ')') {
            --Depth;
            if (Depth == 0) {
                CloseParen = Index;
                break;
            }
        }
    }
    if (CloseParen == std::string::npos) {
        return Result;
    }

    for (auto& Parameter : splitParameters(StringRef(Text).slice(OpenParen + 1, CloseParen))) {
        Result.Parameters.push_back(normalizeType(std::move(Parameter), Windows));
    }

    std::string Suffix = trim(StringRef(Text).drop_front(CloseParen + 1));
    Result.Const = StringRef(Suffix).contains("const");
    Result.RefQualified = StringRef(Suffix).contains("&&") || StringRef(Suffix).contains("&");
    Result.Valid = true;
    return Result;
}

std::string makeKey(const ParsedMethod& Method) {
    std::string Key = Method.QualifiedName + "(";
    for (size_t Index = 0; Index < Method.Parameters.size(); ++Index) {
        if (Index != 0) {
            Key += ",";
        }
        Key += Method.Parameters[Index];
    }
    Key += ")";
    if (Method.Const) {
        Key += "const";
    }
    return Key;
}

void addMethod(Configuration& Config, const ParsedMethod& Parsed, const std::string& Symbol, uint64_t Rva, uint64_t Va) {
    Method Method;
    Method.ClassName = Parsed.QualifiedName.substr(0, Parsed.QualifiedName.find("::"));
    Method.Name = Parsed.Name;
    Method.Key = makeKey(Parsed);
    Method.ReturnType = Parsed.ReturnType;
    Method.Parameters = Parsed.Parameters;
    Method.Access = Parsed.Access;
    Method.Const = Parsed.Const;
    Method.RefQualified = Parsed.RefQualified;
    Method.Static = Parsed.Static;
    Method.Virtual = Parsed.Virtual;
    Method.Ctor = Parsed.Ctor;
    Method.Dtor = Parsed.Dtor;
    Method.Symbol = Symbol;
    Method.Rva = Rva;
    Method.Va = Va;
    Config.Methods.emplace(Method.Key, std::move(Method));
}

bool matchesClass(const std::string& Symbol, const std::string& ClassName) {
    return Symbol.find("@" + ClassName + "@@") != std::string::npos
        || Symbol.find("??0" + ClassName + "@@") != std::string::npos
        || Symbol.find("??1" + ClassName + "@@") != std::string::npos
        || Symbol.find("??_7" + ClassName + "@@") != std::string::npos
        || (Symbol.find("??_R") != std::string::npos && Symbol.find(ClassName) != std::string::npos);
}

uint64_t rvaForPublic(const object::COFFObjectFile& Coff, const codeview::PublicSym32& Public) {
    auto Section = Coff.getSection(Public.Segment);
    if (!Section) {
        return 0;
    }
    return (*Section)->VirtualAddress + Public.Offset;
}

bool readCoffBytes(const object::COFFObjectFile& Coff, uint64_t ImageBase, uint64_t Rva, void* Destination, size_t Size) {
    uintptr_t Pointer = 0;
    if (Error ReadError = Coff.getVaPtr(ImageBase + Rva, Pointer)) {
        consumeError(std::move(ReadError));
        return false;
    }
    std::memcpy(Destination, reinterpret_cast<const void*>(Pointer), Size);
    return true;
}

void mapVtableAddresses(Configuration& Config, uint64_t VtableAddress, bool Windows, uint64_t ImageBase, const object::COFFObjectFile* Coff, const object::ObjectFile* Elf) {
    if (!Windows) {
        auto* ElfObject = dyn_cast<object::ELFObjectFileBase>(Elf);
        if (ElfObject == nullptr) {
            return;
        }
        for (auto Section : ElfObject->dynamic_relocation_sections()) {
            for (auto Relocation : Section.relocations()) {
                object::ELFRelocationRef ElfRelocation(Relocation);
                if (ElfRelocation.getType() != ELF::R_X86_64_RELATIVE) {
                    continue;
                }
                uint64_t Offset = ElfRelocation.getOffset();
                if (Offset < VtableAddress || Offset >= VtableAddress + 512 * 8) {
                    continue;
                }
                auto Addend = ElfRelocation.getAddend();
                if (!Addend) {
                    consumeError(Addend.takeError());
                    continue;
                }
                size_t Slot = static_cast<size_t>((Offset - VtableAddress) / 8);
                if (Slot < 2) {
                    continue;
                }
                uint64_t Target = static_cast<uint64_t>(*Addend);
                for (auto& [Key, Method] : Config.Methods) {
                    if (Method.Va == Target) {
                        Method.VtableIndex = static_cast<int>(Slot - 2);
                        Method.Virtual = true;
                    }
                }
            }
        }
        return;
    }

    for (size_t Slot = 0; Slot < 512; ++Slot) {
        uint64_t Target = 0;
        if (!readCoffBytes(*Coff, ImageBase, VtableAddress + Slot * 8, &Target, sizeof(Target)) || Target == 0) {
            break;
        }
        uint64_t TargetAddress = Target - ImageBase;
        for (auto& [Key, Method] : Config.Methods) {
            if (Method.Rva == TargetAddress || Method.Va == TargetAddress) {
                Method.VtableIndex = static_cast<int>(Slot);
                Method.Virtual = true;
            }
        }
    }
}

bool loadPdb(const Inputs& Inputs, const object::COFFObjectFile& Coff, Configuration& Config) {
    errs() << "loading PDB " << Inputs.Pdb << "\n";
    std::unique_ptr<pdb::IPDBSession> Session;
    if (Error LoadError = pdb::NativeSession::createFromPdbPath(Inputs.Pdb, Session)) {
        errs() << "cannot open PDB: " << toString(std::move(LoadError)) << "\n";
        return false;
    }
    auto* Native = static_cast<pdb::NativeSession*>(Session.get());
    errs() << "PDB session created\n";

    errs() << "opening symbol stream\n";
    auto& Symbols = cantFail(Native->getPDBFile().getPDBSymbolStream());
    bool HadError = false;
    uint64_t ImageBase = Coff.getImageBase();
    uint64_t Seen = 0;

    for (auto& Symbol : Symbols.getSymbols(&HadError)) {
        ++Seen;
        if (Seen % 10000000 == 0) {
            errs() << "  " << Seen << " symbols\n";
        }
        if (Symbol.kind() != codeview::SymbolKind::S_PUB32) {
            continue;
        }
        auto Public = codeview::SymbolDeserializer::deserializeAs<codeview::PublicSym32>(Symbol);
        if (!Public) {
            consumeError(Public.takeError());
            continue;
        }

        bool Wanted = false;
        for (const auto& ClassName : Inputs.Classes) {
            if (matchesClass(Public->Name.str(), ClassName)) {
                Wanted = true;
                break;
            }
        }
        if (!Wanted) {
            continue;
        }

        std::string Mangled = Public->Name.str();
        if (Mangled.find("??_7") == 0) {
            for (const auto& ClassName : Inputs.Classes) {
                if (Mangled == "??_7" + ClassName + "@@6B@") {
                    Config.VtableSymbols[ClassName] = Mangled;
                    Config.VtableAddresses[ClassName] = rvaForPublic(Coff, *Public);
                }
            }
            continue;
        }
        if (Mangled.find("??_R") == 0) {
            continue;
        }

        std::string Demangled = demangle(Mangled);
        if (Demangled == Mangled || Demangled.find("[thunk]") != std::string::npos) {
            continue;
        }

        for (const auto& ClassName : Inputs.Classes) {
            ParsedMethod Parsed = parseDemangled(Demangled, ClassName, true);
            if (!Parsed.Valid) {
                continue;
            }
            uint64_t Rva = rvaForPublic(Coff, *Public);
            addMethod(Config, Parsed, Mangled, Rva, ImageBase + Rva);
            break;
        }
    }

    if (HadError) {
        errs() << "warning: PDB symbol stream reported an error\n";
    }
    return true;
}

bool loadElf(const Inputs& Inputs, const object::ObjectFile& Elf, Configuration& Config) {
    for (auto Symbol : Elf.symbols()) {
        auto Name = Symbol.getName();
        if (!Name) {
            consumeError(Name.takeError());
            continue;
        }
        std::string Mangled = Name->str();
        bool Wanted = false;
        for (const auto& ClassName : Inputs.Classes) {
            std::string Marker = std::to_string(ClassName.size()) + ClassName;
            if (Mangled.find("_ZN" + Marker) != std::string::npos
                || Mangled.find("_ZNK" + Marker) != std::string::npos
                || Mangled.find("_ZTV" + Marker) != std::string::npos) {
                Wanted = true;
                break;
            }
        }
        if (!Wanted) {
            continue;
        }

        if (Mangled.find("_ZTV") == 0) {
            for (const auto& ClassName : Inputs.Classes) {
                std::string Expected = "_ZTV" + std::to_string(ClassName.size()) + ClassName;
                if (Mangled == Expected) {
                    Config.VtableSymbols[ClassName] = Mangled;
                    Config.VtableAddresses[ClassName] = cantFail(Symbol.getValue());
                }
            }
            continue;
        }

        std::string Demangled = demangle(Mangled);
        if (Demangled == Mangled || Demangled.find("[clone") != std::string::npos) {
            continue;
        }

        for (const auto& ClassName : Inputs.Classes) {
            ParsedMethod Parsed = parseDemangled(Demangled, ClassName, false);
            if (!Parsed.Valid) {
                continue;
            }
            uint64_t Address = cantFail(Symbol.getValue());
            addMethod(Config, Parsed, Mangled, Address, Address);
            break;
        }
    }
    return true;
}

std::string hex(uint64_t Value) {
    std::string Text;
    raw_string_ostream Stream(Text);
    Stream << format_hex(Value, 0, false);
    return Text;
}

json::Object buildMethodJson(const std::string& Key, const Configuration& Windows, const Configuration& Linux) {
    const Method* Win = nullptr;
    const Method* Lin = nullptr;
    auto WinIt = Windows.Methods.find(Key);
    if (WinIt != Windows.Methods.end()) {
        Win = &WinIt->second;
    }
    auto LinIt = Linux.Methods.find(Key);
    if (LinIt != Linux.Methods.end()) {
        Lin = &LinIt->second;
    }
    const Method& Base = Win != nullptr ? *Win : *Lin;

    json::Object Object;
    Object["key"] = Key;
    Object["name"] = Base.Name;
    Object["qualifiedName"] = Base.ClassName + "::" + Base.Name;
    Object["access"] = Win != nullptr ? Win->Access : "unknown";
    Object["const"] = Base.Const;
    Object["static"] = Win != nullptr ? Win->Static : false;
    Object["virtual"] = (Win != nullptr && Win->Virtual) || (Lin != nullptr && Lin->Virtual);
    Object["constructor"] = Base.Ctor;
    Object["destructor"] = Base.Dtor;

    json::Array Parameters;
    for (const auto& Parameter : Base.Parameters) {
        Parameters.emplace_back(Parameter);
    }
    Object["parameters"] = std::move(Parameters);

    std::string ReturnType = Win != nullptr ? Win->ReturnType : std::string();
    if (ReturnType.empty() && Lin != nullptr) {
        ReturnType = Lin->ReturnType;
    }
    if (!ReturnType.empty()) {
        Object["returnType"] = ReturnType;
    }

    auto addSide = [&](const char* Name, const Method* MethodInfo) {
        if (MethodInfo == nullptr) {
            return;
        }
        json::Object Side;
        Side["symbol"] = MethodInfo->Symbol;
        Side["address"] = hex(MethodInfo->Va);
        if (MethodInfo->Rva != MethodInfo->Va) {
            Side["rva"] = hex(MethodInfo->Rva);
        }
        if (!MethodInfo->ReturnType.empty()) {
            Side["returnType"] = MethodInfo->ReturnType;
        }
        if (MethodInfo->VtableIndex >= 0) {
            Side["vtableIndex"] = MethodInfo->VtableIndex;
        }
        Object[Name] = std::move(Side);
    };
    addSide("win-x64-server", Win);
    addSide("linux-x64-server", Lin);
    return Object;
}

} // namespace

int main(int argc, char** argv) {
    Inputs Inputs;
    for (int Index = 1; Index < argc; ++Index) {
        StringRef Argument = argv[Index];
        auto next = [&]() -> std::string {
            if (Index + 1 >= argc) {
                errs() << "missing value for " << Argument << "\n";
                std::exit(2);
            }
            return argv[++Index];
        };
        if (Argument == "--pdb") {
            Inputs.Pdb = next();
        } else if (Argument == "--exe") {
            Inputs.Exe = next();
        } else if (Argument == "--elf") {
            Inputs.Elf = next();
        } else if (Argument == "--output") {
            Inputs.Output = next();
        } else if (Argument == "--version") {
            Inputs.Version = next();
        } else if (Argument == "--classes") {
            std::string Classes = next();
            size_t Start = 0;
            while (Start <= Classes.size()) {
                size_t Comma = Classes.find(',', Start);
                std::string Name = trim(StringRef(Classes).slice(Start, Comma));
                if (!Name.empty()) {
                    Inputs.Classes.push_back(Name);
                }
                if (Comma == std::string::npos) {
                    break;
                }
                Start = Comma + 1;
            }
        } else {
            errs() << "unknown argument: " << Argument << "\n";
            return 2;
        }
    }

    if (Inputs.Pdb.empty() || Inputs.Exe.empty() || Inputs.Elf.empty() || Inputs.Output.empty()
        || Inputs.Classes.empty()) {
        errs() << "usage: hosihikari-symbol-ir-frontend --pdb <pdb> --exe <pe> --elf <debug> "
                  "--output <file> [--version <id>] --classes <A,B>\n";
        return 2;
    }

    auto ExeBinary = object::createBinary(Inputs.Exe);
    if (!ExeBinary) {
        errs() << "cannot read PE: " << toString(ExeBinary.takeError()) << "\n";
        return 1;
    }
    auto* Coff = dyn_cast<object::COFFObjectFile>(ExeBinary->getBinary());
    if (Coff == nullptr) {
        errs() << "input is not a COFF/PE image\n";
        return 1;
    }
    errs() << "PE loaded, image base " << format_hex(Coff->getImageBase(), 0, false) << "\n";

    auto ElfBinary = object::createBinary(Inputs.Elf);
    if (!ElfBinary) {
        errs() << "cannot read ELF: " << toString(ElfBinary.takeError()) << "\n";
        return 1;
    }
    auto* Elf = dyn_cast<object::ObjectFile>(ElfBinary->getBinary());
    if (Elf == nullptr) {
        errs() << "input is not an object file\n";
        return 1;
    }

    Configuration Windows;
    Windows.Id = "win-x64-server";
    Configuration Linux;
    Linux.Id = "linux-x64-server";

    errs() << "reading PDB configuration\n";
    if (!loadPdb(Inputs, *Coff, Windows)) {
        return 1;
    }
    errs() << "reading ELF configuration\n";
    if (!loadElf(Inputs, *Elf, Linux)) {
        return 1;
    }

    for (auto& [ClassName, Symbol] : Windows.VtableSymbols) {
        mapVtableAddresses(Windows, Windows.VtableAddresses[ClassName], true, Coff->getImageBase(), Coff, nullptr);
    }
    for (auto& [ClassName, Symbol] : Linux.VtableSymbols) {
        mapVtableAddresses(Linux, Linux.VtableAddresses[ClassName], false, 0, nullptr, Elf);
    }

    json::Object Root;
    Root["prototype"] = "hosihikari.bedrock.symbols/1";
    Root["version"] = Inputs.Version;
    json::Object InputFiles;
    InputFiles["pdb"] = Inputs.Pdb;
    InputFiles["exe"] = Inputs.Exe;
    InputFiles["elf"] = Inputs.Elf;
    Root["inputs"] = std::move(InputFiles);

    json::Array Classes;
    for (const auto& ClassName : Inputs.Classes) {
        json::Object ClassObject;
        ClassObject["name"] = ClassName;

        json::Object Vtables;
        auto addVtable = [&](const char* ConfigId, Configuration& Config) {
            auto Symbol = Config.VtableSymbols.find(ClassName);
            if (Symbol == Config.VtableSymbols.end()) {
                return;
            }
            json::Object Vtable;
            Vtable["symbol"] = Symbol->second;
            Vtable["address"] = hex(Config.VtableAddresses[ClassName]);
            Vtables[ConfigId] = std::move(Vtable);
        };
        addVtable("win-x64-server", Windows);
        addVtable("linux-x64-server", Linux);
        if (!Vtables.empty()) {
            ClassObject["vtables"] = std::move(Vtables);
        }

        std::set<std::string> Keys;
        for (const auto& [Key, Method] : Windows.Methods) {
            if (Method.ClassName == ClassName) {
                Keys.insert(Key);
            }
        }
        for (const auto& [Key, Method] : Linux.Methods) {
            if (Method.ClassName == ClassName) {
                Keys.insert(Key);
            }
        }

        json::Array Methods;
        for (const auto& Key : Keys) {
            Methods.emplace_back(buildMethodJson(Key, Windows, Linux));
        }
        ClassObject["methods"] = std::move(Methods);
        ClassObject["winMethodCount"] = static_cast<int64_t>(std::count_if(
            Windows.Methods.begin(), Windows.Methods.end(),
            [&](const auto& Entry) { return Entry.second.ClassName == ClassName; }));
        ClassObject["linuxMethodCount"] = static_cast<int64_t>(std::count_if(
            Linux.Methods.begin(), Linux.Methods.end(),
            [&](const auto& Entry) { return Entry.second.ClassName == ClassName; }));
        Classes.emplace_back(std::move(ClassObject));
    }
    Root["classes"] = std::move(Classes);

    std::error_code Error;
    raw_fd_ostream OutStream{Inputs.Output, Error};
    if (Error) {
        errs() << "cannot write output: " << Error.message() << "\n";
        return 1;
    }
    OutStream << json::Value(std::move(Root)) << "\n";
    return 0;
}
