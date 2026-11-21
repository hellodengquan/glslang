//
// Copyright (C) 2026 The Khronos Group Inc.
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//    Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//
//    Redistributions in binary form must reproduce the above
//    copyright notice, this list of conditions and the following
//    disclaimer in the documentation and/or other materials provided
//    with the distribution.
//
//    Neither the name of Google Inc. nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// Tests for diagnostic source locations (file / line / column) produced by
// the GLSL preprocessor and parser when the source is shaped by
//   - multi-level #include chains (with nested relative paths),
//   - repeated macro definitions and multiple macro expansions on one line,
//   - #line source-line directives,
//   - included files that do not end with a newline.
// Also verifies that a normally-compiling shader using all of the above
// produces exactly the same SPIR-V module as its manually flattened
// equivalent, i.e. the include/macro/#line machinery does not change the
// generated code.

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "SPIRV/GlslangToSpv.h"
#include "glslang/Public/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"

namespace glslangtest {
namespace {

// Splits a virtual path into segments, resolving "." and ".." so that
// diagnostics and include records always see a canonical path.
std::string NormalizePath(const std::string& path)
{
    std::vector<std::string> parts;
    std::stringstream stream(path);
    std::string item;
    while (std::getline(stream, item, '/')) {
        if (item.empty() || item == ".")
            continue;
        if (item == "..") {
            if (!parts.empty())
                parts.pop_back();
            continue;
        }
        parts.push_back(item);
    }
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0)
            result += '/';
        result += parts[i];
    }
    return result;
}

std::string DirName(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// An includer backed by an in-memory virtual file system. It resolves
// "header" includes relative to the including file (supporting nested
// relative paths such as "l2/b.glsl" from within "l1/a.glsl") and records
// every include callback so tests can reconstruct the include stack that
// glslang reported through the includer interface.
class VirtualFileIncluder : public glslang::TShader::Includer {
public:
    struct IncludeEntry {
        std::string headerName;   // path as written in the #include directive
        std::string includerName; // file containing the #include directive
        size_t depth;             // inclusion depth reported by glslang
        std::string resolvedPath; // canonical path used for diagnostics
    };

    std::map<std::string, std::string> files;
    std::vector<IncludeEntry> includeLog;

    IncludeResult* includeLocal(const char* headerName, const char* includerName,
                                size_t inclusionDepth) override
    {
        const std::string dir = DirName(includerName != nullptr ? includerName : "");
        const std::string resolved =
            NormalizePath(dir.empty() ? headerName : dir + "/" + headerName);
        return include(headerName, includerName, inclusionDepth, resolved);
    }

    IncludeResult* includeSystem(const char* headerName, const char* includerName,
                                 size_t inclusionDepth) override
    {
        return include(headerName, includerName, inclusionDepth, NormalizePath(headerName));
    }

    void releaseInclude(IncludeResult* result) override
    {
        if (result != nullptr) {
            delete[] static_cast<char*>(result->userData);
            delete result;
        }
    }

private:
    IncludeResult* include(const char* headerName, const char* includerName, size_t depth,
                           const std::string& resolved)
    {
        includeLog.push_back({headerName, includerName != nullptr ? includerName : "", depth, resolved});
        const auto it = files.find(resolved);
        if (it == files.end())
            return nullptr;
        char* data = new char[it->second.size()];
        std::copy(it->second.begin(), it->second.end(), data);
        // The resolved path becomes the IncludeResult header name, which is
        // what glslang puts into the synthetic #line prologue and hence into
        // every diagnostic reported for this file.
        return new IncludeResult(it->first.c_str(), data, it->second.size(), data);
    }
};

struct ParseOutcome {
    bool success;
    std::string infoLog;
    std::string debugLog;
};

// Parses a vertex shader with the given includer, displaying error columns
// and allowing cascading errors so all problems on one line are reported.
ParseOutcome ParseVertexShader(const std::string& shaderName, const std::string& source,
                               glslang::TShader::Includer& includer)
{
    glslang::TShader shader(EShLangVertex);
    const char* strings = source.c_str();
    const int lengths = static_cast<int>(source.size());
    const char* names = shaderName.c_str();
    shader.setStringsWithLengthsAndNames(&strings, &lengths, &names, 1);

    const EShMessages messages = (EShMessages)(EShMsgDisplayErrorColumn | EShMsgCascadingErrors);
    const bool success = shader.parse(GetDefaultResources(), 460, false, messages, includer);
    return {success, shader.getInfoLog(), shader.getInfoDebugLog()};
}

// Compiles and links a vertex shader to SPIR-V for the Vulkan environment.
// Returns an empty vector on any failure, appending diagnostics to |log|.
std::vector<uint32_t> CompileToSpirv(const std::string& shaderName, const std::string& source,
                                     glslang::TShader::Includer& includer, std::string* log)
{
    glslang::TShader shader(EShLangVertex);
    const char* strings = source.c_str();
    const char* names = shaderName.c_str();
    shader.setStringsWithLengthsAndNames(&strings, nullptr, &names, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangVertex, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

    const EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 460, false, messages, includer)) {
        *log += shader.getInfoLog();
        *log += shader.getInfoDebugLog();
        return {};
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        *log += program.getInfoLog();
        *log += program.getInfoDebugLog();
        return {};
    }

    std::vector<uint32_t> spirv;
    spv::SpvBuildLogger logger;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangVertex), spirv, &logger);
    *log += logger.getAllMessages();
    return spirv;
}

size_t CountOccurrences(const std::string& haystack, const std::string& needle)
{
    size_t count = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size()))
        ++count;
    return count;
}

// An error inside a file pulled in through a two-level include chain must be
// reported with the included file's resolved path and its own line/column,
// not with the including file's location.
TEST(PpDiagnosticLocation, ErrorInNestedIncludeReportsIncludedFileLineAndColumn)
{
    VirtualFileIncluder includer;
    includer.files["common/defs.glsl"] =
        "// Shared definitions.\n"
        "#define DEFS_SCALE 1.0\n";
    includer.files["common/inner/more.glsl"] =
        "float more_value = 1.0;\n"
        "vec4 more_pos = missing_identifier;\n";

    const std::string source =
        "#version 460\n"
        "#extension GL_GOOGLE_include_directive : require\n"
        "#include \"common/defs.glsl\"\n"
        "#include \"common/inner/more.glsl\"\n"
        "void main() { }\n";

    const ParseOutcome outcome = ParseVertexShader("main.vert", source, includer);
    EXPECT_FALSE(outcome.success);
    // The error is on line 2 of common/inner/more.glsl; the identifier
    // 'missing_identifier' starts at column 17 of that line.
    EXPECT_NE(outcome.infoLog.find("ERROR: common/inner/more.glsl:2:17: 'missing_identifier'"),
              std::string::npos) << outcome.infoLog;
    // The error must not be misattributed to the including files.
    EXPECT_EQ(outcome.infoLog.find("ERROR: main.vert"), std::string::npos) << outcome.infoLog;
    EXPECT_EQ(outcome.infoLog.find("ERROR: common/defs.glsl"), std::string::npos) << outcome.infoLog;
}

// With three nested includes using relative paths, the diagnostic must name
// the deepest file, and the includer callbacks must form a chain (each
// includer name equals the previously resolved path, starting at the root
// shader name) so the full include stack stays traceable.
TEST(PpDiagnosticLocation, DeeplyNestedRelativeIncludesKeepTraceableIncludeStack)
{
    VirtualFileIncluder includer;
    includer.files["l1/a.glsl"] =
        "// level 1\n"
        "#include \"l2/b.glsl\"\n";
    includer.files["l1/l2/b.glsl"] =
        "// level 2\n"
        "#include \"l3/c.glsl\"\n";
    includer.files["l1/l2/l3/c.glsl"] =
        "// level 3\n"
        "// still level 3\n"
        "// deeper still\n"
        "int c_value = c_missing;\n";

    const std::string source =
        "#version 460\n"
        "#extension GL_GOOGLE_include_directive : require\n"
        "#include \"l1/a.glsl\"\n"
        "void main() { }\n";

    const ParseOutcome outcome = ParseVertexShader("main.vert", source, includer);
    EXPECT_FALSE(outcome.success);
    // The error is on line 4 of the deepest header, named by its fully
    // resolved nested relative path.
    EXPECT_NE(outcome.infoLog.find("ERROR: l1/l2/l3/c.glsl:4:"), std::string::npos)
        << outcome.infoLog;
    EXPECT_EQ(outcome.infoLog.find("ERROR: l1/a.glsl"), std::string::npos) << outcome.infoLog;
    EXPECT_EQ(outcome.infoLog.find("ERROR: l1/l2/b.glsl"), std::string::npos) << outcome.infoLog;

    // The include stack recorded through the includer interface must chain
    // main.vert -> l1/a.glsl -> l1/l2/b.glsl -> l1/l2/l3/c.glsl with
    // increasing depths, so a backtrace can be reconstructed.
    ASSERT_EQ(includer.includeLog.size(), 3u);
    EXPECT_EQ(includer.includeLog[0].headerName, "l1/a.glsl");
    EXPECT_EQ(includer.includeLog[0].includerName, "main.vert");
    EXPECT_EQ(includer.includeLog[0].depth, 1u);
    EXPECT_EQ(includer.includeLog[0].resolvedPath, "l1/a.glsl");
    EXPECT_EQ(includer.includeLog[1].headerName, "l2/b.glsl");
    EXPECT_EQ(includer.includeLog[1].includerName, "l1/a.glsl");
    EXPECT_EQ(includer.includeLog[1].depth, 2u);
    EXPECT_EQ(includer.includeLog[1].resolvedPath, "l1/l2/b.glsl");
    EXPECT_EQ(includer.includeLog[2].headerName, "l3/c.glsl");
    EXPECT_EQ(includer.includeLog[2].includerName, "l1/l2/b.glsl");
    EXPECT_EQ(includer.includeLog[2].depth, 3u);
    EXPECT_EQ(includer.includeLog[2].resolvedPath, "l1/l2/l3/c.glsl");
}

// Two macro expansions on the same source line must each produce
// diagnostics pointing at their own invocation site (line and column),
// never at the #define line. glslang reports each undeclared identifier
// only once per compilation, so the two expansions use distinct argument
// identifiers to make both expansions produce a diagnostic.
TEST(PpDiagnosticLocation, RepeatedMacroExpansionsOnOneLineKeepInvocationColumns)
{
    VirtualFileIncluder includer;
    const std::string source =
        "#version 460\n"
        "#define DOUBLE_USE(id) ((id) + (id))\n"
        "void main() {\n"
        "    float a = DOUBLE_USE(missing_one) * DOUBLE_USE(missing_two);\n"
        "}\n";

    const ParseOutcome outcome = ParseVertexShader("main.vert", source, includer);
    EXPECT_FALSE(outcome.success);
    // Each expansion's error is reported on the invocation line (line 4),
    // at the column where that invocation ends: ')' of the first call is at
    // column 37, ')' of the second call at column 63. Nothing may be
    // attributed to the #define line (line 2).
    EXPECT_NE(outcome.infoLog.find("ERROR: main.vert:4:37: 'missing_one' : undeclared identifier"),
              std::string::npos) << outcome.infoLog;
    EXPECT_NE(outcome.infoLog.find("ERROR: main.vert:4:63: 'missing_two' : undeclared identifier"),
              std::string::npos) << outcome.infoLog;
    EXPECT_EQ(outcome.infoLog.find(":2:"), std::string::npos) << outcome.infoLog;
    EXPECT_EQ(CountOccurrences(outcome.infoLog, "undeclared identifier"), 2u) << outcome.infoLog;
}

// #line with a file name must redirect both the reported file and line, and
// the line must keep incrementing from the redirected value. A #line without
// a file name keeps the previously established name.
TEST(PpDiagnosticLocation, LineDirectiveControlsReportedFileAndLine)
{
    VirtualFileIncluder includer;
    const std::string source =
        "#version 460\n"
        "#extension GL_GOOGLE_cpp_style_line_directive : enable\n"
        "#line 1000 \"virtual/generated.glsl\"\n"
        "float gen_a = missing_a;\n"
        "float gen_b = missing_b;\n"
        "#line 5\n"
        "float gen_c = missing_c;\n";

    const ParseOutcome outcome = ParseVertexShader("main.vert", source, includer);
    EXPECT_FALSE(outcome.success);
    EXPECT_NE(outcome.infoLog.find("ERROR: virtual/generated.glsl:1000:"), std::string::npos)
        << outcome.infoLog;
    EXPECT_NE(outcome.infoLog.find("ERROR: virtual/generated.glsl:1001:"), std::string::npos)
        << outcome.infoLog;
    EXPECT_NE(outcome.infoLog.find("ERROR: virtual/generated.glsl:5:"), std::string::npos)
        << outcome.infoLog;
    // No error may be reported against the physical file or its real lines.
    EXPECT_EQ(outcome.infoLog.find("ERROR: main.vert"), std::string::npos) << outcome.infoLog;
}

// An included file whose last line has no trailing newline must still get
// correct locations for its final line, and the line numbers of the
// including file must resume correctly right after the #include directive.
TEST(PpDiagnosticLocation, IncludeWithoutTrailingNewlineRestoresIncludingFileLines)
{
    VirtualFileIncluder includer;
    // Note: deliberately no trailing '\n' after the last line.
    includer.files["frag/noeol.glsl"] =
        "int first = 1;\n"
        "vec3 v = bad_include;";

    const std::string source =
        "#version 460\n"
        "#extension GL_GOOGLE_include_directive : require\n"
        "#include \"frag/noeol.glsl\"\n"
        "int second = bad_main;\n"
        "void main() { }\n";

    const ParseOutcome outcome = ParseVertexShader("main.vert", source, includer);
    EXPECT_FALSE(outcome.success);
    // Error on the last (newline-less) line of the included file.
    EXPECT_NE(outcome.infoLog.find("ERROR: frag/noeol.glsl:2:"), std::string::npos)
        << outcome.infoLog;
    // Error back in the including file, on the line right after #include.
    EXPECT_NE(outcome.infoLog.find("ERROR: main.vert:4:"), std::string::npos) << outcome.infoLog;
}

// A benign (identical) macro redefinition is not an error, while a
// conflicting redefinition must be reported at the redefinition site,
// including when that site is inside an included file.
TEST(PpDiagnosticLocation, MacroRedefinitionReportedAtRedefinitionSite)
{
    VirtualFileIncluder includer;
    includer.files["redef.glsl"] =
        "#define SCALE 2.0\n";

    const std::string source =
        "#version 460\n"
        "#extension GL_GOOGLE_include_directive : require\n"
        "#define SCALE 1.0\n"
        "#define SCALE 1.0\n"
        "#include \"redef.glsl\"\n"
        "void main() { }\n";

    const ParseOutcome outcome = ParseVertexShader("main.vert", source, includer);
    EXPECT_FALSE(outcome.success);
    // The identical redefinition on line 4 is accepted silently; the
    // conflicting one inside the included header is reported against the
    // header's own name, line and column (the macro name starts at
    // column 9 of "#define SCALE 2.0").
    EXPECT_NE(outcome.infoLog.find("ERROR: redef.glsl:1:9: '#define' : Macro redefined"),
              std::string::npos) << outcome.infoLog;
    EXPECT_EQ(CountOccurrences(outcome.infoLog, "Macro redefined"), 1u) << outcome.infoLog;
    EXPECT_EQ(outcome.infoLog.find("ERROR: main.vert"), std::string::npos) << outcome.infoLog;
}

// A shader assembled from multi-level includes, macros (expanded several
// times on one line), a #line directive and a newline-less header must
// produce bit-identical SPIR-V to its manually flattened equivalent: the
// preprocessing machinery must not change the generated module.
TEST(PpDiagnosticLocation, SpirvUnchangedByIncludesMacrosAndLineDirectives)
{
    // The flattened equivalent declares the same extension so that both
    // modules request identical extensions; everything else must match
    // purely because the preprocessed token streams are equivalent.
    const std::string flattened =
        "#version 460\n"
        "#extension GL_GOOGLE_include_directive : require\n"
        "layout(location = 0) in vec3 in_pos;\n"
        "layout(location = 0) out vec4 out_color;\n"
        "vec4 compute_color(vec3 p) { return vec4(p * 2.0 + 1.0, 1.0); }\n"
        "void main() { out_color = compute_color(in_pos); }\n";

    VirtualFileIncluder includer;
    // No trailing newline on the last line, on purpose.
    includer.files["lib/consts.glsl"] =
        "#define SCALE 2.0\n"
        "#define OFFSET 1.0\n"
        "layout(location = 0) in vec3 in_pos;\n"
        "layout(location = 0) out vec4 out_color;";
    includer.files["lib/geom.glsl"] =
        "vec4 compute_color(vec3 p) { return vec4(p * SCALE + OFFSET, SCALE - OFFSET); }\n";
    const std::string assembled =
        "#version 460\n"
        "#extension GL_GOOGLE_include_directive : require\n"
        "#include \"lib/consts.glsl\"\n"
        "#include \"lib/geom.glsl\"\n"
        "#line 40\n"
        "void main() { out_color = compute_color(in_pos); }\n";

    VirtualFileIncluder emptyIncluder;
    std::string logA, logB;
    const std::vector<uint32_t> spirvA = CompileToSpirv("flat.vert", flattened, emptyIncluder, &logA);
    const std::vector<uint32_t> spirvB = CompileToSpirv("main.vert", assembled, includer, &logB);

    ASSERT_FALSE(spirvA.empty()) << logA;
    ASSERT_FALSE(spirvB.empty()) << logB;
    EXPECT_TRUE(logA.empty()) << logA;
    EXPECT_TRUE(logB.empty()) << logB;
    EXPECT_EQ(spirvA, spirvB);
}

}  // anonymous namespace
}  // namespace glslangtest
