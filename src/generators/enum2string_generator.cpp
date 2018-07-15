#include "enum2string_generator.h"
#include "options.h"
#include "ast_reflector.h"
#include "ast_utils.h"

#include <jinja2cpp/reflected_value.h>

#include <clang/ASTMatchers/ASTMatchers.h>

#include <iostream>

using namespace clang::ast_matchers;

namespace jinja2
{
template<>
struct TypeReflection<reflection::EnumInfo> : TypeReflected<reflection::EnumInfo>
{
    static auto& GetAccessors()
    {
        static std::unordered_map<std::string, FieldAccessor> accessors = {
            {"name", [](const reflection::EnumInfo& obj) {return Reflect(obj.name);}},
            {"scopeSpecifier", [](const reflection::EnumInfo& obj) {return Reflect(obj.scopeSpecifier);}},
            {"namespaceQualifier", [](const reflection::EnumInfo& obj) { return obj.namespaceQualifier;}},
            {"isScoped", [](const reflection::EnumInfo& obj) {return obj.isScoped;}},
            {"items", [](const reflection::EnumInfo& obj) {return Reflect(obj.items);}},
        };

        return accessors;
    }
};

template<>
struct TypeReflection<reflection::EnumItemInfo> : TypeReflected<reflection::EnumItemInfo>
{
    static auto& GetAccessors()
    {
        static std::unordered_map<std::string, FieldAccessor> accessors = {
            {"itemName", [](const reflection::EnumItemInfo& obj) {return Reflect(obj.itemName);}},
        };

        return accessors;
    }
};
} // jinja2

namespace codegen
{
namespace
{
DeclarationMatcher enumMatcher =
        enumDecl().bind("enum");

auto g_enum2stringTemplatePreamble =
R"(
{# #ifndef {{HeaderGuard}}
 #define {{HeaderGuard}} #}
 #pragma once

{% for fileName in inputFiles %}
 #include "{{fileName}}"
{% endfor %}

{% for fileName in extraHeaders %}
{% if fileName is startsWith('<') %}
 #include fileName
{% else %}
 #include "fileName"
{% endif %}
{% endfor %}

 #include <flex_lib/stringized_enum.h>
 #include <algorithm>
 #include <utility>
)";

auto g_enumConverters =
R"(
{% set enumName = enum.name %}
{% set scopeSpec = enum.scopeSpecifier %}
{% set namespaceQual = enum.namespaceQualifier %}
{% set prefix = enumScopedName + '::' if not enumInfo.isScoped else enumScopedName + '::' + scopeSpec + ('' if not scopeSpec else '::') %}

inline const char* {{enumName}}ToString({{enumScopedName}} e)
{
    switch (e)
    {
{% for item in enum.items %}
    case {{prefix}}{{item.itemName}}:
        return "{{item.itemName}}";
{% endfor %}
    }
    return "Unknown Item";
}

inline {{enumScopedName}} StringTo{{enumName}}(const char* itemName)
{
    static std::pair<const char*, {{enumScopedName}}> items[] = {
{% for item in enum.items | sort (attribute='itemName') %}
        {"{{item.itemName}}", {{prefix}}{{item.itemName}} } {{',' if not loop.last }}
{% endfor %}
    };

    {{enumScopedName}} result;
    if (!flex_lib::detail::String2Enum(itemName, items, result))
         flex_lib::bad_enum_name::Throw(itemName, "{{enumName}}");

    return result;
}
)";

auto g_flConverterInvokers =
R"(
template<>
inline const char* flex_lib::Enum2String({{enumFullQualifiedName}} e)
{
    return {{enum.namespaceQualifier}}::{{enum.name}}ToString(e);
}

template<>
inline {{enumFullQualifiedName}} flex_lib::String2Enum<{{enumFullQualifiedName}}>(const char* itemName)
{
    return {{enum.namespaceQualifier}}::StringTo{{enum.name}}(itemName);
}
)";

auto g_stdConverterInvokers =
R"(
inline std::string to_string({{enumFullQualifiedName}} e)
{
    return {{enum.namespaceQualifier}}::{{enum.name}}ToString(e);
}
)";
}

Enum2StringGenerator::Enum2StringGenerator(const Options &opts)
    : BasicGenerator(opts)
{
    m_headerPreambleTpl.Load(g_enum2stringTemplatePreamble);
    m_convertersTpl.Load(g_enumConverters);
    m_flConverterInvokers.Load(g_flConverterInvokers);
    m_stdConverterInvokers.Load(g_stdConverterInvokers);
}

void Enum2StringGenerator::SetupMatcher(clang::ast_matchers::MatchFinder &finder, clang::ast_matchers::MatchFinder::MatchCallback *defaultCallback)
{
    finder.addMatcher(enumMatcher, defaultCallback);
}

void Enum2StringGenerator::HandleMatch(const clang::ast_matchers::MatchFinder::MatchResult &matchResult)
{
    if (const clang::EnumDecl* decl = matchResult.Nodes.getNodeAs<clang::EnumDecl>("enum"))
    {
        if (!IsFromInputFiles(decl->getLocStart(), matchResult.Context))
            return;

        reflection::AstReflector reflector(matchResult.Context);

        reflector.ReflectEnum(decl, &m_namespaces);
    }
}

void Enum2StringGenerator::WriteHeaderPreamble(CppSourceStream &hdrOs)
{
    jinja2::ValuesMap params = {
        {"inputFiles", jinja2::Reflect(m_options.inputFiles)},
        {"extraHeaders", jinja2::Reflect(m_options.extraHeaders)},
    };
    m_headerPreambleTpl.Render(hdrOs, params);
}

void Enum2StringGenerator::WriteHeaderPostamble(CppSourceStream &hdrOs)
{
//    hdrOs << out::scope_exit;
}

namespace
{
jinja2::ValuesMap MakeScopedParams(reflection::EnumInfoPtr enumInfo)
{
    std::string scopeSpec = enumInfo->scopeSpecifier;
    std::string scopedName =  scopeSpec + (scopeSpec.empty() ? "" : "::") + enumInfo->name;
    std::string fullQualifiedName = enumInfo->GetFullQualifiedName(false);

    jinja2::ValuesMap params = {
        {"enum", jinja2::Reflect(enumInfo)},
        {"enumScopedName", scopedName},
        {"enumFullQualifiedName", fullQualifiedName},
    };

    return params;
}

}

void Enum2StringGenerator::WriteHeaderContent(CppSourceStream &hdrOs)
{
    std::vector<reflection::EnumInfoPtr> enums;
    WriteNamespaceContents(hdrOs, m_namespaces.GetRootNamespace(), [this, &enums, &hdrOs](CppSourceStream &os, reflection::NamespaceInfoPtr ns) {
        for (auto& enumInfo : ns->enums)
        {
            m_convertersTpl.Render(hdrOs, MakeScopedParams(enumInfo));

            enums.push_back(enumInfo);
        }
    });

    hdrOs << "\n\n";

    for (reflection::EnumInfoPtr enumInfo : enums)
    {
        m_flConverterInvokers.Render(hdrOs, MakeScopedParams(enumInfo));
    }

    hdrOs << "\nnamespace std {\n";

    for (reflection::EnumInfoPtr enumInfo : enums)
    {
        m_stdConverterInvokers.Render(hdrOs, MakeScopedParams(enumInfo));
    }
    hdrOs << "\n}\n\n";
}
} // codegen

codegen::GeneratorPtr CreateEnum2StringGen(const codegen::Options& opts)
{
    return std::make_unique<codegen::Enum2StringGenerator>(opts);
}
