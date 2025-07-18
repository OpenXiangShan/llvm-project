//===- ModuleDepCollector.h - Callbacks to collect deps ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLING_DEPENDENCYSCANNING_MODULEDEPCOLLECTOR_H
#define LLVM_CLANG_TOOLING_DEPENDENCYSCANNING_MODULEDEPCOLLECTOR_H

#include "clang/Basic/LLVM.h"
#include "clang/Basic/Module.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/Utils.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Serialization/ASTReader.h"
#include "clang/Tooling/DependencyScanning/DependencyScanningService.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

namespace clang {
namespace tooling {
namespace dependencies {

class DependencyActionController;
class DependencyConsumer;
class PrebuiltModuleASTAttrs;

/// Modular dependency that has already been built prior to the dependency scan.
struct PrebuiltModuleDep {
  std::string ModuleName;
  std::string PCMFile;
  std::string ModuleMapFile;

  explicit PrebuiltModuleDep(const Module *M)
      : ModuleName(M->getTopLevelModuleName()),
        PCMFile(M->getASTFile()->getName()),
        ModuleMapFile(M->PresumedModuleMapFile) {}
};

/// Attributes loaded from AST files of prebuilt modules collected prior to
/// ModuleDepCollector creation.
using PrebuiltModulesAttrsMap = llvm::StringMap<PrebuiltModuleASTAttrs>;
class PrebuiltModuleASTAttrs {
public:
  /// When a module is discovered to not be in stable directories, traverse &
  /// update all modules that depend on it.
  void
  updateDependentsNotInStableDirs(PrebuiltModulesAttrsMap &PrebuiltModulesMap);

  /// Read-only access to whether the module is made up of dependencies in
  /// stable directories.
  bool isInStableDir() const { return IsInStableDirs; }

  /// Read-only access to vfs map files.
  const llvm::StringSet<> &getVFS() const { return VFSMap; }

  /// Update the VFSMap to the one discovered from serializing the AST file.
  void setVFS(llvm::StringSet<> &&VFS) { VFSMap = std::move(VFS); }

  /// Add a direct dependent module file, so it can be updated if the current
  /// module is from stable directores.
  void addDependent(StringRef ModuleFile) {
    ModuleFileDependents.insert(ModuleFile);
  }

  /// Update whether the prebuilt module resolves entirely in a stable
  /// directories.
  void setInStableDir(bool V = false) {
    // Cannot reset attribute once it's false.
    if (!IsInStableDirs)
      return;
    IsInStableDirs = V;
  }

private:
  llvm::StringSet<> VFSMap;
  bool IsInStableDirs = true;
  std::set<StringRef> ModuleFileDependents;
};

/// This is used to identify a specific module.
struct ModuleID {
  /// The name of the module. This may include `:` for C++20 module partitions,
  /// or a header-name for C++20 header units.
  std::string ModuleName;

  /// The context hash of a module represents the compiler options that affect
  /// the resulting command-line invocation.
  ///
  /// Modules with the same name and ContextHash but different invocations could
  /// cause non-deterministic build results.
  ///
  /// Modules with the same name but a different \c ContextHash should be
  /// treated as separate modules for the purpose of a build.
  std::string ContextHash;

  bool operator==(const ModuleID &Other) const {
    return std::tie(ModuleName, ContextHash) ==
           std::tie(Other.ModuleName, Other.ContextHash);
  }

  bool operator<(const ModuleID& Other) const {
    return std::tie(ModuleName, ContextHash) <
           std::tie(Other.ModuleName, Other.ContextHash);
  }
};

/// P1689ModuleInfo - Represents the needed information of standard C++20
/// modules for P1689 format.
struct P1689ModuleInfo {
  /// The name of the module. This may include `:` for partitions.
  std::string ModuleName;

  /// Optional. The source path to the module.
  std::string SourcePath;

  /// If this module is a standard c++ interface unit.
  bool IsStdCXXModuleInterface = true;

  enum class ModuleType {
    NamedCXXModule
    // To be supported
    // AngleHeaderUnit,
    // QuoteHeaderUnit
  };
  ModuleType Type = ModuleType::NamedCXXModule;
};

/// An output from a module compilation, such as the path of the module file.
enum class ModuleOutputKind {
  /// The module file (.pcm). Required.
  ModuleFile,
  /// The path of the dependency file (.d), if any.
  DependencyFile,
  /// The null-separated list of names to use as the targets in the dependency
  /// file, if any. Defaults to the value of \c ModuleFile, as in the driver.
  DependencyTargets,
  /// The path of the serialized diagnostic file (.dia), if any.
  DiagnosticSerializationFile,
};

struct ModuleDeps {
  /// The identifier of the module.
  ModuleID ID;

  /// Whether this is a "system" module.
  bool IsSystem;

  /// Whether this module is fully composed of file & module inputs from
  /// locations likely to stay the same across the active development and build
  /// cycle. For example, when all those input paths only resolve in Sysroot.
  ///
  /// External paths, as opposed to virtual file paths, are always used
  /// for computing this value.
  bool IsInStableDirectories;

  /// The path to the modulemap file which defines this module.
  ///
  /// This can be used to explicitly build this module. This file will
  /// additionally appear in \c FileDeps as a dependency.
  std::string ClangModuleMapFile;

  /// A collection of absolute paths to module map files that this module needs
  /// to know about. The ordering is significant.
  std::vector<std::string> ModuleMapFileDeps;

  /// A collection of prebuilt modular dependencies this module directly depends
  /// on, not including transitive dependencies.
  std::vector<PrebuiltModuleDep> PrebuiltModuleDeps;

  /// A list of module identifiers this module directly depends on, not
  /// including transitive dependencies.
  ///
  /// This may include modules with a different context hash when it can be
  /// determined that the differences are benign for this compilation.
  std::vector<ModuleID> ClangModuleDeps;

  /// The set of libraries or frameworks to link against when
  /// an entity from this module is used.
  llvm::SmallVector<Module::LinkLibrary, 2> LinkLibraries;

  /// Invokes \c Cb for all file dependencies of this module. Each provided
  /// \c StringRef is only valid within the individual callback invocation.
  void forEachFileDep(llvm::function_ref<void(StringRef)> Cb) const;

  /// Get (or compute) the compiler invocation that can be used to build this
  /// module. Does not include argv[0].
  const std::vector<std::string> &getBuildArguments() const;

private:
  friend class ModuleDepCollector;
  friend class ModuleDepCollectorPP;

  /// The base directory for relative paths in \c FileDeps.
  std::string FileDepsBaseDir;

  /// A collection of paths to files that this module directly depends on, not
  /// including transitive dependencies.
  std::vector<std::string> FileDeps;

  mutable std::variant<std::monostate, CowCompilerInvocation,
                       std::vector<std::string>>
      BuildInfo;
};

class ModuleDepCollector;

/// Callback that records textual includes and direct modular includes/imports
/// during preprocessing. At the end of the main file, it also collects
/// transitive modular dependencies and passes everything to the
/// \c DependencyConsumer of the parent \c ModuleDepCollector.
class ModuleDepCollectorPP final : public PPCallbacks {
public:
  ModuleDepCollectorPP(ModuleDepCollector &MDC) : MDC(MDC) {}

  void LexedFileChanged(FileID FID, LexedFileChangeReason Reason,
                        SrcMgr::CharacteristicKind FileType, FileID PrevFID,
                        SourceLocation Loc) override;
  void InclusionDirective(SourceLocation HashLoc, const Token &IncludeTok,
                          StringRef FileName, bool IsAngled,
                          CharSourceRange FilenameRange,
                          OptionalFileEntryRef File, StringRef SearchPath,
                          StringRef RelativePath, const Module *SuggestedModule,
                          bool ModuleImported,
                          SrcMgr::CharacteristicKind FileType) override;
  void moduleImport(SourceLocation ImportLoc, ModuleIdPath Path,
                    const Module *Imported) override;

  void EndOfMainFile() override;

private:
  /// The parent dependency collector.
  ModuleDepCollector &MDC;

  void handleImport(const Module *Imported);

  /// Adds direct modular dependencies that have already been built to the
  /// ModuleDeps instance.
  void
  addAllSubmodulePrebuiltDeps(const Module *M, ModuleDeps &MD,
                              llvm::DenseSet<const Module *> &SeenSubmodules);
  void addModulePrebuiltDeps(const Module *M, ModuleDeps &MD,
                             llvm::DenseSet<const Module *> &SeenSubmodules);

  /// Traverses the previously collected direct modular dependencies to discover
  /// transitive modular dependencies and fills the parent \c ModuleDepCollector
  /// with both.
  /// Returns the ID or nothing if the dependency is spurious and is ignored.
  std::optional<ModuleID> handleTopLevelModule(const Module *M);
  void addAllSubmoduleDeps(const Module *M, ModuleDeps &MD,
                           llvm::DenseSet<const Module *> &AddedModules);
  void addModuleDep(const Module *M, ModuleDeps &MD,
                    llvm::DenseSet<const Module *> &AddedModules);

  /// Traverses the affecting modules and updates \c MD with references to the
  /// parent \c ModuleDepCollector info.
  void addAllAffectingClangModules(const Module *M, ModuleDeps &MD,
                              llvm::DenseSet<const Module *> &AddedModules);
  void addAffectingClangModule(const Module *M, ModuleDeps &MD,
                          llvm::DenseSet<const Module *> &AddedModules);

  /// Add discovered module dependency for the given module.
  void addOneModuleDep(const Module *M, const ModuleID ID, ModuleDeps &MD);
};

/// Collects modular and non-modular dependencies of the main file by attaching
/// \c ModuleDepCollectorPP to the preprocessor.
class ModuleDepCollector final : public DependencyCollector {
public:
  ModuleDepCollector(DependencyScanningService &Service,
                     std::unique_ptr<DependencyOutputOptions> Opts,
                     CompilerInstance &ScanInstance, DependencyConsumer &C,
                     DependencyActionController &Controller,
                     CompilerInvocation OriginalCI,
                     const PrebuiltModulesAttrsMap PrebuiltModulesASTMap,
                     const ArrayRef<StringRef> StableDirs);

  void attachToPreprocessor(Preprocessor &PP) override;
  void attachToASTReader(ASTReader &R) override;

  /// Apply any changes implied by the discovered dependencies to the given
  /// invocation, (e.g. disable implicit modules, add explicit module paths).
  void applyDiscoveredDependencies(CompilerInvocation &CI);

private:
  friend ModuleDepCollectorPP;

  /// The parent dependency scanning service.
  DependencyScanningService &Service;
  /// The compiler instance for scanning the current translation unit.
  CompilerInstance &ScanInstance;
  /// The consumer of collected dependency information.
  DependencyConsumer &Consumer;
  /// Callbacks for computing dependency information.
  DependencyActionController &Controller;
  /// Mapping from prebuilt AST filepaths to their attributes referenced during
  /// dependency collecting.
  const PrebuiltModulesAttrsMap PrebuiltModulesASTMap;
  /// Directory paths known to be stable through an active development and build
  /// cycle.
  const ArrayRef<StringRef> StableDirs;
  /// Path to the main source file.
  std::string MainFile;
  /// Non-modular file dependencies. This includes the main source file and
  /// textually included header files.
  std::vector<std::string> FileDeps;
  /// Direct and transitive modular dependencies of the main source file.
  llvm::MapVector<const Module *, std::unique_ptr<ModuleDeps>> ModularDeps;
  /// Secondary mapping for \c ModularDeps allowing lookup by ModuleID without
  /// a preprocessor. Storage owned by \c ModularDeps.
  llvm::DenseMap<ModuleID, ModuleDeps *> ModuleDepsByID;
  /// Direct modular dependencies that have already been built.
  llvm::MapVector<const Module *, PrebuiltModuleDep> DirectPrebuiltModularDeps;
  /// Working set of direct modular dependencies.
  llvm::SetVector<const Module *> DirectModularDeps;
  /// Working set of direct modular dependencies, as they were imported.
  llvm::SmallPtrSet<const Module *, 32> DirectImports;
  /// All direct and transitive visible modules.
  llvm::StringSet<> VisibleModules;

  /// Options that control the dependency output generation.
  std::unique_ptr<DependencyOutputOptions> Opts;
  /// A Clang invocation that's based on the original TU invocation and that has
  /// been partially transformed into one that can perform explicit build of
  /// a discovered modular dependency. Note that this still needs to be adjusted
  /// for each individual module.
  CowCompilerInvocation CommonInvocation;

  std::optional<P1689ModuleInfo> ProvidedStdCXXModule;
  std::vector<P1689ModuleInfo> RequiredStdCXXModules;

  /// Checks whether the module is known as being prebuilt.
  bool isPrebuiltModule(const Module *M);

  /// Computes all visible modules resolved from direct imports.
  void addVisibleModules();

  /// Adds \p Path to \c FileDeps, making it absolute if necessary.
  void addFileDep(StringRef Path);
  /// Adds \p Path to \c MD.FileDeps, making it absolute if necessary.
  void addFileDep(ModuleDeps &MD, StringRef Path);

  /// Get a Clang invocation adjusted to build the given modular dependency.
  /// This excludes paths that are yet-to-be-provided by the build system.
  CowCompilerInvocation getInvocationAdjustedForModuleBuildWithoutOutputs(
      const ModuleDeps &Deps,
      llvm::function_ref<void(CowCompilerInvocation &)> Optimize) const;

  /// Collect module map files for given modules.
  llvm::DenseSet<const FileEntry *>
  collectModuleMapFiles(ArrayRef<ModuleID> ClangModuleDeps) const;

  /// Add module map files to the invocation, if needed.
  void addModuleMapFiles(CompilerInvocation &CI,
                         ArrayRef<ModuleID> ClangModuleDeps) const;
  /// Add module files (pcm) to the invocation, if needed.
  void addModuleFiles(CompilerInvocation &CI,
                      ArrayRef<ModuleID> ClangModuleDeps) const;
  void addModuleFiles(CowCompilerInvocation &CI,
                      ArrayRef<ModuleID> ClangModuleDeps) const;

  /// Add paths that require looking up outputs to the given dependencies.
  void addOutputPaths(CowCompilerInvocation &CI, ModuleDeps &Deps);

  /// Compute the context hash for \p Deps, and create the mapping
  /// \c ModuleDepsByID[Deps.ID] = &Deps.
  void associateWithContextHash(const CowCompilerInvocation &CI, bool IgnoreCWD,
                                ModuleDeps &Deps);
};

/// Resets codegen options that don't affect modules/PCH.
void resetBenignCodeGenOptions(frontend::ActionKind ProgramAction,
                               const LangOptions &LangOpts,
                               CodeGenOptions &CGOpts);

/// Determine if \c Input can be resolved within a stable directory.
///
/// \param Directories Paths known to be in a stable location. e.g. Sysroot.
/// \param Input Path to evaluate.
bool isPathInStableDir(const ArrayRef<StringRef> Directories,
                       const StringRef Input);

/// Determine if options collected from a module's
/// compilation can safely be considered as stable.
///
/// \param Directories Paths known to be in a stable location. e.g. Sysroot.
/// \param HSOpts Header search options derived from the compiler invocation.
bool areOptionsInStableDir(const ArrayRef<StringRef> Directories,
                           const HeaderSearchOptions &HSOpts);

} // end namespace dependencies
} // end namespace tooling
} // end namespace clang

namespace llvm {
inline hash_code hash_value(const clang::tooling::dependencies::ModuleID &ID) {
  return hash_combine(ID.ModuleName, ID.ContextHash);
}

template <> struct DenseMapInfo<clang::tooling::dependencies::ModuleID> {
  using ModuleID = clang::tooling::dependencies::ModuleID;
  static inline ModuleID getEmptyKey() { return ModuleID{"", ""}; }
  static inline ModuleID getTombstoneKey() {
    return ModuleID{"~", "~"}; // ~ is not a valid module name or context hash
  }
  static unsigned getHashValue(const ModuleID &ID) { return hash_value(ID); }
  static bool isEqual(const ModuleID &LHS, const ModuleID &RHS) {
    return LHS == RHS;
  }
};
} // namespace llvm

#endif // LLVM_CLANG_TOOLING_DEPENDENCYSCANNING_MODULEDEPCOLLECTOR_H
